#include "studio_types.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

void require(bool valid,const char* message) { if(!valid) throw std::runtime_error(message); }
int main() {
    try {
        using namespace studio;
        require(validKey({}) && validKey({0,.1f}) && validKey({.99f,1}),"Valid key rejected");
        require(!validKey({1,.1f}) && !validKey({-.01f,.1f}) && !validKey({.5f,0}) && !validKey({.5f,1.01f}) && !validKey({std::nanf(""),.1f}),"Invalid key accepted");

        RuntimeStats healthy; healthy.captured=10; healthy.submitted=10;
        require(!assess(healthy,1,0,true,false).stop,"Healthy run stopped");
        auto faulted=healthy; faulted.fault="Output DMA failed";
        auto verdict=assess(faulted,1,0,false,true);
        require(verdict.stop && verdict.error && verdict.reason=="Studio logging failed","Logging failure must be reported first");
        verdict=assess(faulted,1,0,true,true);
        require(verdict.stop && verdict.error && verdict.reason=="Output DMA failed","Runtime fault must precede signal loss");
        verdict=assess(healthy,1,0,true,true);
        require(verdict.stop && verdict.error && verdict.reason=="Camera signal lost or format changed","Signal loss not detected");
        RuntimeStats silent; silent.captured=10;
        require(!assess(silent,4.9,0,true,false).stop,"Startup grace period too short");
        verdict=assess(silent,5,0,true,false);
        require(verdict.stop && verdict.error,"Capture without delivery passed the startup check");
        verdict=assess(healthy,60,60,true,false);
        require(verdict.stop && !verdict.error && verdict.complete,"Timed run did not complete");
        require(!assess(healthy,3600,0,true,false).stop,"Untimed run stopped by itself");

        require(passed(false,0,false,healthy),"User-stopped healthy run failed");
        require(!passed(false,60,false,healthy),"Incomplete timed run passed");
        require(!passed(true,60,true,healthy),"Faulted run passed");
        RuntimeStats undelivered; undelivered.captured=10;
        require(!passed(false,60,true,undelivered),"Run without delivered frames passed");

        const std::string header=csvHeader;
        require(header.rfind("seconds,captured,capture_failures,injected_failures,submitted,output_failures,repeats,camera_fallbacks,pool_full,input_drops,output_drops,input_level,output_level,camera_sequence,blender_generation,blender_sequence,camera_ready_age_ms,blender_receive_age_ms,composite_api_ms,blender_render_age_ms,tracking_receive_age_ms,blender_session,blender_repeats,blender_skipped,pose_target_age_ms,",0)==0,
            "Existing CSV columns changed");
        std::ostringstream row; writeCsvRow(row,1.25,healthy,{.2f,.05f});
        const auto text=row.str();
        require(std::count(header.begin(),header.end(),',')==std::count(text.begin(),text.end(),','),"CSV row and header column counts differ");
        require(text.rfind("1.250,10,",0)==0 && text.ends_with(",0.200,0.050\n"),"CSV row values or key columns wrong");
        std::cout << "PASS: key validation, monitor verdict order, pass/fail rule, CSV columns\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
