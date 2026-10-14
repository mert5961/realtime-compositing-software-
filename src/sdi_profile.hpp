#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <string>
#include <type_traits>

namespace sdi_profile {
using Clock = std::chrono::steady_clock;
enum class Kind { CaptureDma, OutputDma, CaptureHostWait, OutputHostWait,
    InputDeviceWait, OutputDeviceWait, Processing, HostPublish, HostRelease, Count };
inline constexpr std::array names{"capture_dma", "output_dma", "capture_host_wait", "output_host_wait",
    "input_device_wait", "output_device_wait", "capture_processing", "host_publish", "host_release"};
struct Metric {
    std::atomic<uint64_t> count{0}, totalNs{0}, maxNs{0}, failures{0};
};
struct View {
    uint64_t count = 0, failures = 0;
    double averageMs = 0, maxMs = 0;
};
struct Host { unsigned capture = 0, ready = 0, output = 0;
    unsigned occupied() const { return capture + ready + output; } };
struct Slot { const void* frame = nullptr; std::atomic<unsigned> stage{0}; std::atomic<bool> valid{false}; };
struct Event {
    uint64_t endNs, durationNs;
    Kind kind;
    Host host;
    bool success, shutdown;
};
// One preallocated trace per SDK thread; no allocation or file writes in the frame loops.
inline constexpr unsigned traceCapacity = 65536;
struct Lane { std::array<Event, traceCapacity> events; unsigned count = 0; uint64_t discarded = 0; };
inline std::array<Metric, size_t(Kind::Count)> metrics;
inline std::array<Slot, 10> slots;
inline std::array<Lane, 2> lanes;
inline thread_local unsigned lane = 0;
inline unsigned slotCount = 0;
inline std::atomic<unsigned> hostPeak{0};
inline std::atomic<bool> stopping{false};
inline Clock::time_point started;

inline void reset() {
    for (auto& metric : metrics) { metric.count = 0; metric.totalNs = 0; metric.maxNs = 0; metric.failures = 0; }
    for (auto& slot : slots) { slot.frame = nullptr; slot.stage = 0; slot.valid = false; }
    for (auto& trace : lanes) { trace.count = 0; trace.discarded = 0; }
    slotCount = 0; hostPeak = 0; stopping = false; started = Clock::now();
}
inline void registerFrame(const void* frame) { if (slotCount < slots.size()) slots[slotCount++].frame = frame; }
inline void setValid(const void* frame, bool valid) {
    for (auto& slot : slots) if (slot.frame == frame) { slot.valid.store(valid); return; }
}
inline bool isValid(const void* frame) {
    for (auto& slot : slots) if (slot.frame == frame) return slot.valid.load();
    return false;
}
inline void stage(const void* frame, unsigned value) {
    for (auto& slot : slots) if (slot.frame == frame) { slot.stage.store(value, std::memory_order_relaxed); return; }
}
inline Host host() {
    Host result;
    for (auto& slot : slots) {
        switch (slot.stage.load(std::memory_order_relaxed)) {
        case 1: ++result.capture; break;
        case 2: ++result.ready; break;
        case 3: ++result.output; break;
        }
    }
    return result;
}
inline void record(Kind kind, Clock::time_point begin, bool success = true) {
    const auto end = Clock::now();
    const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end-begin).count());
    const bool shutdown = stopping.load(std::memory_order_relaxed);
    const auto occupancy = host();
    if (!shutdown) {
        auto& metric = metrics[size_t(kind)];
        metric.totalNs.fetch_add(ns, std::memory_order_relaxed);
        metric.count.fetch_add(1, std::memory_order_relaxed);
        if (!success) metric.failures.fetch_add(1, std::memory_order_relaxed);
        auto maximum = metric.maxNs.load(std::memory_order_relaxed);
        while (maximum < ns && !metric.maxNs.compare_exchange_weak(maximum,ns,std::memory_order_relaxed)) {}
        auto peak = hostPeak.load(std::memory_order_relaxed);
        while (peak < occupancy.occupied() && !hostPeak.compare_exchange_weak(peak,occupancy.occupied(),std::memory_order_relaxed)) {}
    }
    auto& trace = lanes[lane];
    if (trace.count < traceCapacity) trace.events[trace.count++] = {
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(end-started).count()),
        ns,kind,occupancy,success,shutdown};
    else ++trace.discarded;
}
template<class Function> auto timed(Kind kind, Function function) {
    const auto begin = Clock::now();
    auto result = function();
    record(kind,begin,bool(result));
    return result;
}
template<class Function> auto acquire(Kind kind, unsigned value, Function function) {
    const auto begin = Clock::now();
    auto frame = function();
    if (frame) stage(frame,value);
    record(kind,begin,frame != nullptr);
    return frame;
}
inline View view(Kind kind) {
    const auto& metric = metrics[size_t(kind)];
    View result;
    result.count = metric.count.load(std::memory_order_relaxed);
    result.failures = metric.failures.load(std::memory_order_relaxed);
    result.averageMs = result.count ? double(metric.totalNs.load(std::memory_order_relaxed)) / result.count / 1e6 : 0;
    result.maxMs = double(metric.maxNs.load(std::memory_order_relaxed)) / 1e6;
    return result;
}
inline bool dump(const std::string& prefix) {
    // Called after both SDK threads have joined. Lane counts and events are then immutable.
    std::ofstream trace(prefix + "-trace.csv"), summary(prefix + "-profile.csv");
    if (!trace || !summary) return false;
    trace << "end_seconds,thread,event,duration_ms,success,host_capture,host_ready,host_output,host_occupied,shutdown\n";
    trace << std::fixed << std::setprecision(6);
    for (unsigned i = 0; i < lanes.size(); ++i)
        for (unsigned n = 0; n < lanes[i].count; ++n) {
            const auto& event = lanes[i].events[n];
            trace << double(event.endNs)/1e9 << ',' << (i ? "output" : "capture") << ',' << names[size_t(event.kind)]
                << ',' << double(event.durationNs)/1e6 << ',' << event.success << ',' << event.host.capture
                << ',' << event.host.ready << ',' << event.host.output << ',' << event.host.occupied() << ',' << event.shutdown << '\n';
        }
    summary << "event,count,average_ms,max_ms,failures\n";
    summary << std::fixed << std::setprecision(6);
    for (unsigned k = 0; k < unsigned(Kind::Count); ++k) {
        const auto metric = view(Kind(k));
        summary << names[k] << ',' << metric.count << ',' << metric.averageMs << ',' << metric.maxMs << ',' << metric.failures << '\n';
    }
    summary << "trace_discarded," << lanes[0].discarded + lanes[1].discarded << ",0,0,0\n";
    trace.flush(); summary.flush();
    return bool(trace) && bool(summary);
}
}
