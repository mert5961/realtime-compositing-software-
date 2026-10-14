#pragma once
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <cstdint>

namespace compositor {
// Bounded frame storage with latest or scheduled selection. A read lease preserves pixels, not just the
// resource object: a writer cannot reserve any slot with outstanding readers.
// GPU callers must establish producer completion before publish, and retain
// their lease until downstream GPU/DMA completion, not merely CPU submission.
template<class T> class LivePool {
    struct Slot { std::shared_ptr<T> value; bool writing=false, valid=false; uint64_t order=0; };
    struct State { std::mutex mutex; std::vector<Slot> slots; uint64_t order=0; };
    std::shared_ptr<State> state_ = std::make_shared<State>();
public:
    class Write {
        std::shared_ptr<State> state_;
        size_t index_;
        bool finished_=false;
        friend class LivePool;
        Write(std::shared_ptr<State> state,size_t index):state_(std::move(state)),index_(index) {}
    public:
        Write(const Write&)=delete;
        Write& operator=(const Write&)=delete;
        Write(Write&& other) noexcept:state_(std::move(other.state_)),index_(other.index_),finished_(other.finished_) { other.finished_=true; }
        ~Write() { if (state_ && !finished_) { std::lock_guard lock(state_->mutex); state_->slots[index_].writing=false; } }
        T& get() { return *state_->slots[index_].value; }
        size_t index() const { return index_; }
        void publish() {
            std::lock_guard lock(state_->mutex);
            auto& slot=state_->slots[index_]; slot.valid=true; slot.writing=false; slot.order=++state_->order; finished_=true;
        }
    };
    template<class Factory> LivePool(size_t capacity,Factory factory) {
        for(size_t i=0;i<capacity;++i) state_->slots.push_back({std::make_shared<T>(factory(i))});
    }
    explicit LivePool(size_t capacity):LivePool(capacity,[](size_t){return T{};}) {}
    std::optional<Write> tryWrite() {
        std::lock_guard lock(state_->mutex);
        size_t selected=state_->slots.size();
        for(size_t i=0;i<state_->slots.size();++i) {
            const auto& slot=state_->slots[i];
            if(!slot.writing && slot.value.use_count()==1 && (selected==state_->slots.size() || slot.order<state_->slots[selected].order)) selected=i;
        }
        if(selected==state_->slots.size()) return std::nullopt;
        auto& slot=state_->slots[selected]; slot.writing=true; slot.valid=false;
        return Write(state_,selected);
    }
    std::shared_ptr<const T> latest() const {
        return newestMatching([](const T&){ return true; });
    }
    template<class Predicate> std::shared_ptr<const T> newestMatching(Predicate predicate) const {
        std::lock_guard lock(state_->mutex);
        const Slot* selected=nullptr;
        for(const auto& slot:state_->slots) if(slot.valid && !slot.writing && predicate(*slot.value) && (!selected || slot.order>selected->order)) selected=&slot;
        return selected ? selected->value : nullptr;
    }
    void invalidate() { std::lock_guard lock(state_->mutex); for(auto& slot:state_->slots) slot.valid=false; }
};
}
