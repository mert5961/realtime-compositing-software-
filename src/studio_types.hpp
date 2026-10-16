#pragma once
// Engine-facing types with no AJA, Spout or D3D11 dependency. UI clients
// (Win32 test UI, CLI, JUCE app) and portable tests include only this.
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <string>

namespace studio {
struct KeyParams { float threshold=.12f,softness=.10f; };
inline bool validKey(KeyParams key) { return key.threshold>=0 && key.threshold<1 && key.softness>0 && key.softness<=1; }

struct RuntimeStats {
    uint64_t captured=0,captureFailures=0,injectedFailures=0,submitted=0,outputFailures=0,repeats=0,fallbacks=0,poolFull=0;
    uint64_t cameraSequence=0,blenderSequence=0,blenderGeneration=0;
    double cameraReadyAgeMs=-1,blenderReceiveAgeMs=-1,processingMs=0;
    double blenderRenderAgeMs=-1,trackingReceiveAgeMs=-1;
    uint64_t blenderSession=0;
    uint64_t blenderRepeats=0,blenderSkipped=0;
    double poseTargetAgeMs=-1;
    unsigned inputDrops=0,outputDrops=0,inputLevel=0,outputLevel=0;
    std::string sourceStatus="Starting",fault;
};

struct EngineConfig {
    std::string device="0",sender="RealtimeStudio",logDirectory="output";
    unsigned input=1,output=4; // 1-based SDI connectors
    unsigned seconds=0;        // 0 = run until stop(); otherwise a timed pass/fail run
    unsigned faultEvery=0;     // test-only injected capture failures
    KeyParams key;
};
struct DeviceInfo { unsigned index=0; std::string name,serial; unsigned videoChannels=0; int32_t ownerPid=0; /* another process streaming on it; 0 = free or us */ };

enum class EngineState { Idle,Starting,Running,Stopping,Stopped,Failed };
inline const char* toString(EngineState state) {
    switch(state) {
    case EngineState::Idle: return "Idle"; case EngineState::Starting: return "Starting"; case EngineState::Running: return "Running";
    case EngineState::Stopping: return "Stopping"; case EngineState::Stopped: return "Stopped"; case EngineState::Failed: return "Failed";
    }
    return "Unknown";
}
inline bool isActive(EngineState state) { return state==EngineState::Starting || state==EngineState::Running || state==EngineState::Stopping; }

enum class EventLevel { Info,Fault };
struct EngineEvent { uint64_t sequence=0; std::chrono::system_clock::time_point time; EventLevel level=EventLevel::Info; std::string text; };

struct EngineSnapshot {
    EngineState state=EngineState::Idle;
    EngineConfig config;      // configuration of the current/last run
    KeyParams key;            // live key parameters
    RuntimeStats stats; bool hasStats=false;
    double elapsedSeconds=0;
    std::string message;      // status while starting, failure reason when failed
    std::string csvPath;
    int result=-1;            // -1 while active/never run, 0 pass, 1 fail
    uint64_t droppedEvents=0;
};

// Monitor decisions, evaluated every sample in this order. A stop with
// error=false and complete=true is a finished timed run.
struct Verdict { bool stop=false,error=false,complete=false; std::string reason; };
inline Verdict assess(const RuntimeStats& stats,double elapsed,unsigned seconds,bool logOk,bool signalChanged) {
    if(!logOk) return {true,true,false,"Studio logging failed"};
    if(!stats.fault.empty()) return {true,true,false,stats.fault};
    if(signalChanged) return {true,true,false,"Camera signal lost or format changed"};
    if(elapsed>=5 && (!stats.captured || !stats.submitted)) return {true,true,false,"Studio video failed to start within five seconds"};
    if(seconds && elapsed>=seconds) return {true,false,true,{}};
    return {};
}
// Perfect Blonde: a run passes only if frames were actually captured and delivered.
inline bool passed(bool error,unsigned seconds,bool complete,const RuntimeStats& stats) {
    return !error && (!seconds || complete) && stats.captured && stats.submitted;
}

inline constexpr const char* csvHeader="seconds,captured,capture_failures,injected_failures,submitted,output_failures,repeats,camera_fallbacks,pool_full,input_drops,output_drops,input_level,output_level,camera_sequence,blender_generation,blender_sequence,camera_ready_age_ms,blender_receive_age_ms,composite_api_ms,blender_render_age_ms,tracking_receive_age_ms,blender_session,blender_repeats,blender_skipped,pose_target_age_ms,threshold,softness\n";
inline void writeCsvRow(std::ostream& csv,double elapsed,const RuntimeStats& stats,KeyParams key) {
    csv << std::fixed << std::setprecision(3) << elapsed << ',' << stats.captured << ',' << stats.captureFailures << ',' << stats.injectedFailures << ',' << stats.submitted << ',' << stats.outputFailures
        << ',' << stats.repeats << ',' << stats.fallbacks << ',' << stats.poolFull << ',' << stats.inputDrops << ',' << stats.outputDrops << ',' << stats.inputLevel << ',' << stats.outputLevel
        << ',' << stats.cameraSequence << ',' << stats.blenderGeneration << ',' << stats.blenderSequence << ',' << stats.cameraReadyAgeMs << ',' << stats.blenderReceiveAgeMs << ',' << stats.processingMs
        << ',' << stats.blenderRenderAgeMs << ',' << stats.trackingReceiveAgeMs << ',' << stats.blenderSession
        << ',' << stats.blenderRepeats << ',' << stats.blenderSkipped << ',' << stats.poseTargetAgeMs << ',' << key.threshold << ',' << key.softness << '\n';
}
}
