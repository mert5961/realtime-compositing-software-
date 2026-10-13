#include "compositor/core.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace compositor {

struct FramePool::State {
    enum class Phase { free, writing, published };
    struct Slot {
        std::vector<Pixel> pixels;
        FrameInfo info;
        Phase phase{Phase::free};
        std::size_t readers{};
    };
    mutable std::mutex mutex;
    std::vector<Slot> slots;
    std::size_t bytes{};
};

FramePool::FramePool(std::size_t capacity, std::uint32_t width, std::uint32_t height)
    : state_(std::make_shared<State>()) {
    if (!capacity || !width || !height)
        throw std::invalid_argument("Frame pool dimensions and capacity must be positive");
    const auto limit = std::numeric_limits<std::size_t>::max();
    if (static_cast<std::size_t>(width) > limit / height / sizeof(Pixel) / capacity)
        throw std::overflow_error("Frame pool size overflow");
    const auto count = static_cast<std::size_t>(width) * height;
    state_->bytes = count * sizeof(Pixel) * capacity;
    state_->slots.resize(capacity);
    for (auto& slot : state_->slots) slot.pixels.resize(count);
}

std::optional<FramePool::WriteLease> FramePool::try_write() {
    std::lock_guard lock(state_->mutex);
    std::optional<std::size_t> candidate;
    for (std::size_t i = 0; i < state_->slots.size(); ++i) {
        const auto& slot = state_->slots[i];
        if (slot.phase == State::Phase::free) { candidate = i; break; }
        if (slot.phase == State::Phase::published && slot.readers == 0 &&
            (!candidate || slot.info.content_time < state_->slots[*candidate].info.content_time))
            candidate = i;
    }
    if (!candidate) return std::nullopt;
    state_->slots[*candidate].phase = State::Phase::writing;
    return WriteLease(state_, *candidate);
}

std::optional<FramePool::ReadLease> FramePool::latest(Time target, Time now, Time max_age) const {
    if (target < Time::zero() || now < Time::zero() || max_age < Time::zero())
        throw std::invalid_argument("Frame selection times must be nonnegative");
    std::lock_guard lock(state_->mutex);
    std::optional<std::size_t> candidate;
    for (std::size_t i = 0; i < state_->slots.size(); ++i) {
        const auto& slot = state_->slots[i];
        if (slot.phase != State::Phase::published || slot.info.ready_time > now ||
            slot.info.content_time > target || target - slot.info.content_time > max_age)
            continue;
        if (!candidate || slot.info.content_time > state_->slots[*candidate].info.content_time ||
            (slot.info.content_time == state_->slots[*candidate].info.content_time &&
             slot.info.sequence > state_->slots[*candidate].info.sequence))
            candidate = i;
    }
    if (!candidate) return std::nullopt;
    ++state_->slots[*candidate].readers;
    return ReadLease(state_, *candidate);
}

std::size_t FramePool::capacity() const { return state_->slots.size(); }
std::size_t FramePool::storage_bytes() const { return state_->bytes; }

FramePool::ReadLease::ReadLease(std::shared_ptr<State> state, std::size_t slot)
    : state_(std::move(state)), slot_(slot) {}

FramePool::ReadLease::ReadLease(const ReadLease& other)
    : state_(other.state_), slot_(other.slot_) {
    if (state_) { std::lock_guard lock(state_->mutex); ++state_->slots[slot_].readers; }
}

FramePool::ReadLease& FramePool::ReadLease::operator=(const ReadLease& other) {
    if (this != &other) { ReadLease copy(other); *this = std::move(copy); }
    return *this;
}

FramePool::ReadLease::ReadLease(ReadLease&& other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_) {}

FramePool::ReadLease& FramePool::ReadLease::operator=(ReadLease&& other) noexcept {
    if (this != &other) { release(); state_ = std::move(other.state_); slot_ = other.slot_; }
    return *this;
}

void FramePool::ReadLease::release() {
    if (state_) {
        { std::lock_guard lock(state_->mutex); --state_->slots[slot_].readers; }
        state_.reset();
    }
}

FramePool::ReadLease::~ReadLease() { release(); }

std::span<const Pixel> FramePool::ReadLease::pixels() const {
    if (!state_) throw std::logic_error("Read from an empty lease");
    return state_->slots[slot_].pixels;
}

FrameInfo FramePool::ReadLease::info() const {
    if (!state_) throw std::logic_error("Read from an empty lease");
    return state_->slots[slot_].info;
}

FramePool::WriteLease::WriteLease(std::shared_ptr<State> state, std::size_t slot)
    : state_(std::move(state)), slot_(slot) {}

FramePool::WriteLease::WriteLease(WriteLease&& other) noexcept
    : state_(std::move(other.state_)), slot_(other.slot_) {}

FramePool::WriteLease& FramePool::WriteLease::operator=(WriteLease&& other) noexcept {
    if (this != &other) { release(); state_ = std::move(other.state_); slot_ = other.slot_; }
    return *this;
}

void FramePool::WriteLease::release() {
    if (state_) {
        { std::lock_guard lock(state_->mutex); state_->slots[slot_].phase = State::Phase::free; }
        state_.reset();
    }
}

FramePool::WriteLease::~WriteLease() { release(); }

std::span<Pixel> FramePool::WriteLease::pixels() {
    if (!state_) throw std::logic_error("Write to an empty lease");
    return state_->slots[slot_].pixels;
}

void FramePool::WriteLease::publish(FrameInfo info) {
    if (!state_) throw std::logic_error("Publish from an empty lease");
    if (info.content_time < Time::zero() || info.ready_time < info.content_time)
        throw std::invalid_argument("Invalid frame timestamps");
    {
        std::lock_guard lock(state_->mutex);
        auto& slot = state_->slots[slot_];
        slot.info = info;
        slot.phase = State::Phase::published;
    }
    state_.reset();
}

FrameScheduler::FrameScheduler(SchedulerConfig config) : config_(config) {
    if (config.max_age < Time::zero() || config.max_skew < Time::zero() ||
        config.processing_budget < Time::zero())
        throw std::invalid_argument("Scheduler budgets must be nonnegative");
}

Selection FrameScheduler::select(const FramePool& foreground, const FramePool& scene,
                                FrameTick tick) const {
    if (tick.now < Time::zero() || tick.target_content_time < Time::zero() ||
        tick.deadline < Time::zero())
        throw std::invalid_argument("Scheduler times must be nonnegative");
    Selection result;
    if (tick.now > tick.deadline || tick.deadline - tick.now < config_.processing_budget) {
        result.status = SelectionStatus::deadline_missed;
        return result;
    }
    auto fg = foreground.latest(tick.target_content_time, tick.now, config_.max_age);
    auto bg = scene.latest(tick.target_content_time, tick.now, config_.max_age);
    if (!fg || !bg) return result;
    const auto delta = fg->info().content_time - bg->info().content_time;
    if (delta > config_.max_skew || delta < -config_.max_skew) {
        result.status = SelectionStatus::excessive_skew;
        return result;
    }
    result.status = SelectionStatus::ready;
    result.foreground = std::move(fg);
    result.scene = std::move(bg);
    return result;
}

void composite(std::span<const Pixel> foreground, std::span<const Pixel> background,
               std::span<Pixel> output) {
    if (foreground.size() != background.size() || foreground.size() != output.size())
        throw std::invalid_argument("Composite dimensions differ");
    for (std::size_t i = 0; i < output.size(); ++i) {
        const auto fg = foreground[i];
        const auto bg = background[i];
        // General straight-alpha over, including transparent background support.
        const unsigned weight_fg = static_cast<unsigned>(fg.a) * 255;
        const unsigned weight_bg = static_cast<unsigned>(bg.a) * (255 - fg.a);
        const unsigned weight = weight_fg + weight_bg;
        const auto blend = [&](unsigned f, unsigned b) -> std::uint8_t {
            return weight ? static_cast<std::uint8_t>((f * weight_fg + b * weight_bg + weight / 2) / weight) : 0;
        };
        output[i] = {blend(fg.r, bg.r), blend(fg.g, bg.g), blend(fg.b, bg.b),
                     static_cast<std::uint8_t>((weight + 127) / 255)};
    }
}

} // namespace compositor
