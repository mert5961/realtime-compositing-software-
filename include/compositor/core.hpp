#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace compositor {

using Time = std::chrono::nanoseconds;

// Initial CPU transport: packed RGBA8, straight alpha, synthetic display RGB.
// A real renderer adapter must supply explicit color and GPU synchronization contracts.
struct Pixel {
    std::uint8_t r{}, g{}, b{}, a{255};
};

struct FrameInfo {
    std::uint64_t sequence{};
    Time content_time{}; // All sources mapped to the session's monotonic timeline.
    Time ready_time{};   // When the complete frame became available.
};

class FramePool {
    struct State;
public:
    class ReadLease {
    public:
        ReadLease(const ReadLease& other);
        ReadLease& operator=(const ReadLease& other);
        ReadLease(ReadLease&& other) noexcept;
        ReadLease& operator=(ReadLease&& other) noexcept;
        ~ReadLease();
        std::span<const Pixel> pixels() const;
        FrameInfo info() const;
    private:
        friend class FramePool;
        ReadLease(std::shared_ptr<State> state, std::size_t slot);
        void release();
        std::shared_ptr<State> state_;
        std::size_t slot_{};
    };

    class WriteLease {
    public:
        WriteLease(const WriteLease&) = delete;
        WriteLease& operator=(const WriteLease&) = delete;
        WriteLease(WriteLease&& other) noexcept;
        WriteLease& operator=(WriteLease&& other) noexcept;
        ~WriteLease();
        std::span<Pixel> pixels();
        void publish(FrameInfo info); // Invalidates this write lease and its spans.
    private:
        friend class FramePool;
        WriteLease(std::shared_ptr<State> state, std::size_t slot);
        void release();
        std::shared_ptr<State> state_;
        std::size_t slot_{};
    };

    FramePool(std::size_t capacity, std::uint32_t width, std::uint32_t height);
    FramePool(const FramePool&) = delete;
    FramePool& operator=(const FramePool&) = delete;
    // Never waits for a surface. May replace the oldest unleased published frame.
    std::optional<WriteLease> try_write();
    // Newest eligible content, including out-of-order publication handling.
    std::optional<ReadLease> latest(Time target, Time now, Time max_age) const;
    std::size_t capacity() const;
    std::size_t storage_bytes() const;
private:
    std::shared_ptr<State> state_;
};

enum class SelectionStatus { ready, missing_input, excessive_skew, deadline_missed };

struct SchedulerConfig {
    Time max_age{std::chrono::milliseconds(100)};
    Time max_skew{std::chrono::milliseconds(40)};
    Time processing_budget{std::chrono::milliseconds(2)};
};

struct FrameTick {
    Time target_content_time{};
    Time now{};
    Time deadline{};
};

struct Selection {
    SelectionStatus status{SelectionStatus::missing_input};
    std::optional<FramePool::ReadLease> foreground;
    std::optional<FramePool::ReadLease> scene;
};

class FrameScheduler {
public:
    explicit FrameScheduler(SchedulerConfig config = {});
    Selection select(const FramePool& foreground, const FramePool& scene,
                     FrameTick tick) const;
private:
    SchedulerConfig config_;
};

// Demonstration blend only: not a production linear-light/color-managed keyer.
void composite(std::span<const Pixel> foreground, std::span<const Pixel> background,
               std::span<Pixel> output);

} // namespace compositor
