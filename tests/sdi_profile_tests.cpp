#include "sdi_profile.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    using namespace sdi_profile;
    try {
        reset();
        int frame = 0;
        registerFrame(&frame);
        auto pointer = acquire(Kind::CaptureHostWait,1,[&] { return &frame; });
        require(pointer == &frame && host().capture == 1,"Capture ownership or pointer changed");
        stage(&frame,2);
        require(host().ready == 1,"Ready occupancy missing");
        pointer = acquire(Kind::OutputHostWait,3,[&] { return &frame; });
        require(pointer == &frame && host().output == 1,"Output ownership changed");
        stage(&frame,0);
        require(host().occupied() == 0,"Released frame still occupied");
        require(!timed(Kind::CaptureDma,[] { return false; }),"Failed DMA result changed");
        require(view(Kind::CaptureDma).failures == 1,"DMA failure not recorded");
        stopping = true;
        timed(Kind::CaptureDma,[] { return false; });
        require(view(Kind::CaptureDma).count == 1,"Shutdown operation included in run metrics");
        reset();
        std::thread capture([] { lane = 0; for (unsigned i=0;i<100;++i) timed(Kind::CaptureDma,[] { return true; }); });
        std::thread output([] { lane = 1; for (unsigned i=0;i<100;++i) timed(Kind::OutputDma,[] { return true; }); });
        capture.join(); output.join();
        require(lanes[0].count == 100 && lanes[1].count == 100,"Per-thread traces incomplete");
        require(view(Kind::CaptureDma).count == 100 && view(Kind::OutputDma).count == 100,"Aggregate counts incomplete");
        std::filesystem::create_directories("profile-test-output");
        require(dump("profile-test-output/test"),"Unable to save profile");
        std::ifstream trace("profile-test-output/test-trace.csv");
        std::string line; unsigned lines = 0;
        while (std::getline(trace,line)) ++lines;
        require(lines == 201,"Trace CSV lost events");
        reset();
        require(view(Kind::CaptureDma).count == 0 && lanes[0].count == 0,"Reset retained previous run data");
        lane = 0;
        for (unsigned i = 0; i < traceCapacity + 3; ++i) record(Kind::CaptureDma,Clock::now());
        require(lanes[0].count == traceCapacity && lanes[0].discarded == 3,"Trace capacity not bounded");
        require(view(Kind::CaptureDma).count == traceCapacity + 3,"Metrics stopped when trace filled");
        std::cout << "Profile ownership, failure, shutdown and concurrent trace checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
