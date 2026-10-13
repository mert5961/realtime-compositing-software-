#include "compositor/core.hpp"

#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace compositor;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void publish(FramePool& pool, std::uint64_t sequence, Time content, Time ready) {
    auto writer = pool.try_write();
    check(writer.has_value(), "Expected a writable surface");
    for (auto& pixel : writer->pixels()) pixel = {static_cast<std::uint8_t>(sequence % 256), 0, 0, 255};
    writer->publish({sequence, content, ready});
}

void ownership() {
    FramePool pool(1, 2, 2);
    {
        auto writer = pool.try_write();
        check(!pool.latest(0ns, 0ns, 0ns), "Unpublished pixels must stay invisible");
        check(!pool.try_write(), "A writer must exclude another writer");
    }
    publish(pool, 7, 0ns, 0ns); // Abandoned writer returned its slot.
    auto first = pool.latest(0ns, 0ns, 0ns);
    check(first.has_value(), "Published frame missing");
    auto second = first;
    first.reset();
    check(!pool.try_write(), "Copied read lease must retain the surface");
    check(second->pixels()[0].r == 7, "Held pixels changed");
    auto moved = std::move(second);
    second.reset();
    check(!pool.try_write(), "Moved read lease must retain the surface");
    moved.reset();
    check(pool.try_write().has_value(), "Last consumer must release the surface");

    std::optional<FramePool::ReadLease> surviving;
    {
        FramePool temporary(1, 1, 1);
        publish(temporary, 9, 0ns, 0ns);
        surviving = temporary.latest(0ns, 0ns, 0ns);
    }
    check(surviving->pixels()[0].r == 9, "Lease must keep storage alive after pool destruction");
}

void selection_and_capacity() {
    FramePool pool(3, 1, 1);
    publish(pool, 3, 30ms, 35ms);
    publish(pool, 1, 10ms, 40ms); // Older content can complete after newer content.
    publish(pool, 2, 20ms, 25ms);
    check(pool.latest(40ms, 40ms, 100ms)->info().sequence == 3, "Select by content time, not arrival order");
    check(pool.latest(40ms, 30ms, 100ms)->info().sequence == 2, "Incomplete frame selected");
    check(pool.latest(25ms, 40ms, 100ms)->info().sequence == 2, "Future content selected");
    check(!pool.latest(100ms, 100ms, 10ms), "Expired frame selected");
    auto held = pool.latest(40ms, 40ms, 100ms);
    const auto bytes = pool.storage_bytes();
    for (std::uint64_t i = 4; i < 1000; ++i) publish(pool, i, Time(i * 1'000'000), Time(i * 1'000'000));
    check(held->info().sequence == 3 && held->pixels()[0].r == 3, "Overflow overwrote a leased frame");
    check(pool.capacity() == 3 && pool.storage_bytes() == bytes, "Pool grew under load");
    check(pool.latest(1000ms, 1000ms, 100ms)->info().sequence == 999, "Stale work accumulated");
}

void scheduling() {
    FramePool camera(3, 1, 1), blender(3, 1, 1);
    FrameScheduler scheduler({100ms, 20ms, 2ms});
    check(scheduler.select(camera, blender, {0ms, 0ms, 10ms}).status == SelectionStatus::missing_input,
          "Startup without inputs must be explicit");
    publish(camera, 1, 10ms, 10ms);
    publish(blender, 1, 0ms, 0ms);
    check(scheduler.select(camera, blender, {10ms, 10ms, 12ms}).status == SelectionStatus::ready,
          "Budget boundary should be accepted");
    check(scheduler.select(camera, blender, {10ms, 11ms, 12ms}).status == SelectionStatus::deadline_missed,
          "Work exceeding remaining budget must be rejected");
    publish(camera, 2, 40ms, 40ms);
    auto rejected = scheduler.select(camera, blender, {40ms, 40ms, 50ms});
    check(rejected.status == SelectionStatus::excessive_skew, "Temporal mismatch must be rejected");
    check(!rejected.foreground && !rejected.scene, "Rejected selection must release input leases");
    check(scheduler.select(camera, blender, {200ms, 200ms, 210ms}).status == SelectionStatus::missing_input,
          "Stalled sources must expire");
    publish(camera, 3, 220ms, 220ms);
    publish(blender, 3, 220ms, 220ms);
    check(scheduler.select(camera, blender, {220ms, 220ms, 230ms}).status == SelectionStatus::ready,
          "Scheduler must recover after a source stall");
}

void blending() {
    std::array<Pixel, 3> foreground{{{255, 0, 0, 0}, {255, 0, 0, 255}, {255, 0, 0, 128}}};
    std::array<Pixel, 3> background{{{0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}}};
    std::array<Pixel, 3> result{};
    composite(foreground, background, result);
    check(result[0].b == 255 && result[0].r == 0, "Transparent foreground must preserve background");
    check(result[1].r == 255 && result[1].b == 0, "Opaque foreground must replace background");
    check(result[2].r == 128 && result[2].b == 127 && result[2].a == 255, "Half-alpha blend incorrect");
    background[2].a = 0;
    composite(foreground, background, result);
    check(result[2].r == 255 && result[2].a == 128, "Straight alpha over transparent background incorrect");
    bool caught = false;
    try { composite(foreground, background, std::span<Pixel>(result).first(1)); }
    catch (const std::invalid_argument&) { caught = true; }
    check(caught, "Mismatched image sizes must be rejected");
}

void concurrent_publication() {
    FramePool pool(3, 16, 16);
    std::atomic<bool> done{false};
    std::atomic<bool> corrupted{false};
    std::atomic<unsigned> reads{0};
    std::jthread consumer([&] {
        do {
            auto frame = pool.latest(1s, 1s, 1s);
            if (!frame) { std::this_thread::yield(); continue; }
            const auto expected = static_cast<std::uint8_t>(frame->info().sequence % 256);
            for (const auto pixel : frame->pixels()) if (pixel.r != expected) corrupted = true;
            ++reads;
        } while (!done.load());
    });
    for (std::uint64_t i = 0; i < 5000; ++i) publish(pool, i, Time(i), Time(i));
    done = true;
    consumer.join();
    check(reads > 0 && !corrupted, "Concurrent publication produced torn pixels or metadata");
}
} // namespace

int main() {
    try {
        ownership();
        selection_and_capacity();
        scheduling();
        blending();
        concurrent_publication();
        std::cout << "PASS: ownership, bounded storage, timestamps, deadlines, stall recovery, alpha, concurrency\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
