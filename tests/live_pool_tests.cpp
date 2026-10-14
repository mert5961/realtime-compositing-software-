#include "compositor/live_pool.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

struct Frame { uint64_t sequence=0; std::array<uint64_t,128> pixels{}; };
void require(bool valid,const char* message) { if(!valid) throw std::runtime_error(message); }
int main() {
    try {
        compositor::LivePool<Frame> pool(2);
        {
            auto write=pool.tryWrite(); write->get().sequence=999; // Failed transfer writes partial bytes.
        }
        require(!pool.latest(),"Abandoned failed frame was published");
        auto first=pool.tryWrite(); require(bool(first),"Failed reservation leaked its slot");
        first->get().sequence=1; first->get().pixels.fill(1); first->publish();
        auto held=pool.latest();
        auto second=pool.tryWrite(); second->get().sequence=2; second->get().pixels.fill(2); second->publish();
        auto heldSecond=pool.latest();
        auto due=pool.newestMatching([](const Frame& f){return f.sequence<=1;});
        require(due && due->sequence==1,"Future frame displaced a due frame");
        require(!pool.newestMatching([](const Frame& f){return f.sequence<1;}),"Frame published before its deadline");
        due.reset();
        require(!pool.tryWrite(),"Busy consumer surface could be overwritten");
        pool.invalidate(); require(!pool.latest(),"Old generation remained selectable");
        require(held->pixels[0]==1 && heldSecond->pixels[0]==2,"Generation invalidation destroyed held pixels");
        held.reset(); auto reused=pool.tryWrite(); require(bool(reused),"Released slot did not recycle");
        reused->get().pixels.fill(3); reused->publish();
        require(heldSecond->pixels[127]==2,"Consumer image changed while held");
        compositor::LivePool<Frame> concurrent(3);
        std::atomic<bool> finished=false;
        std::thread producer([&]{ for(uint64_t sequence=1;sequence<=10000;++sequence) {
            auto write=concurrent.tryWrite(); if(!write) { std::this_thread::yield(); --sequence; continue; }
            write->get().sequence=sequence; write->get().pixels.fill(sequence); write->publish();
        } finished=true; });
        bool stable=true;
        do {
            auto frame=concurrent.latest();
            if(frame) {
                const auto sequence=frame->sequence;
                std::this_thread::yield();
                for(auto pixel:frame->pixels) if(pixel!=sequence) stable=false;
            }
        } while(!finished);
        producer.join(); require(stable,"Concurrent publication overwrote a held image");
        std::cout << "PASS: invalid reservation, immutable pixels, bounded exhaustion, generations, concurrent consumers\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
