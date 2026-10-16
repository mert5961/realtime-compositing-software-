#include "studio_control.hpp"
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

// Synthetic engine for UI development without the AJA card. It follows the real
// engine's state machine, sample cadence, validation and failure reporting, and
// never claims to be on air (simulated() is true).
namespace studio {
namespace {
class FakeControl final : public Control {
    FakeOptions options;
    mutable std::mutex mutex;
    EngineSnapshot snap;
    std::deque<EngineEvent> events; uint64_t eventSequence=0;
    std::atomic<bool> stopRequested{false};
    std::thread worker;
    void event(EventLevel level,std::string text) {
        std::lock_guard lock(mutex);
        if(events.size()==256) { events.pop_front(); ++snap.droppedEvents; }
        events.push_back({++eventSequence,std::chrono::system_clock::now(),level,std::move(text)});
    }
    void run(EngineConfig config) {
        using namespace std::chrono;
        const auto wait=[&](double seconds) {
            const auto until=steady_clock::now()+duration_cast<steady_clock::duration>(duration<double>(seconds));
            while(!stopRequested && steady_clock::now()<until) std::this_thread::sleep_for(milliseconds(10));
        };
        std::string failure;
        { std::lock_guard lock(mutex); snap.message="Initializing studio video path..."; }
        wait(options.startupSeconds);
        if(!config.input || !config.output || config.input==config.output || config.input>4 || config.output>4) failure="Choose two valid different SDI connectors";
        RuntimeStats stats; std::mt19937 random(7); std::normal_distribution<double> jitter(0,1);
        if(failure.empty()) {
            const auto csv=config.logDirectory+"/studio-simulated.csv";
            { std::lock_guard lock(mutex); snap.state=EngineState::Running; snap.message.clear(); snap.csvPath=csv; }
            event(EventLevel::Info,"Running 1080p50 (simulated) | CSV "+csv+" (not written)");
            const auto start=steady_clock::now(); bool stalled=false;
            while(!stopRequested) {
                std::this_thread::sleep_for(milliseconds(options.sampleMs));
                const double elapsed=duration<double>(steady_clock::now()-start).count();
                const double period=options.stallEvery;
                const bool stall=period>0 && elapsed>period && std::fmod(elapsed,period)<options.stallSeconds;
                if(stall!=stalled) event(stall?EventLevel::Fault:EventLevel::Info,stall?"Blender stale >500 ms: camera passthrough (simulated)":"Blender recovered (simulated)");
                stalled=stall;
                const uint64_t frames=uint64_t(elapsed*50);
                const uint64_t step=frames>stats.captured ? frames-stats.captured:0;
                stats.captured=frames; stats.submitted=frames+1; stats.repeats=2;
                if(stall) stats.fallbacks+=step;
                stats.cameraSequence=frames; stats.blenderSequence=stall?stats.blenderSequence:frames; stats.blenderGeneration=1; stats.blenderSession=1;
                stats.cameraReadyAgeMs=20+jitter(random)*.6;
                stats.blenderReceiveAgeMs=stall?-1:30+std::abs(jitter(random))*6;
                stats.blenderRenderAgeMs=stall?-1:45+std::abs(jitter(random))*5;
                stats.trackingReceiveAgeMs=stall?-1:62+std::abs(jitter(random))*3;
                stats.poseTargetAgeMs=stall?-1:61.8+std::abs(jitter(random))*2;
                stats.processingMs=.8+std::abs(jitter(random))*.1;
                if(random()%97==0) ++stats.blenderRepeats; if(random()%101==0) ++stats.blenderSkipped;
                stats.inputLevel=1+random()%2; stats.outputLevel=3;
                stats.sourceStatus=stall?"Waiting for Blender sender "+config.sender:"Blender GPU frames ready";
                if(options.failAfter>0 && elapsed>=options.failAfter) stats.fault="Output DMA failed (simulated)";
                { std::lock_guard lock(mutex); snap.stats=stats; snap.hasStats=true; snap.elapsedSeconds=elapsed; }
                const auto verdict=assess(stats,elapsed,config.seconds,true,false);
                if(verdict.stop) { failure=verdict.reason; break; }
            }
        }
        { std::lock_guard lock(mutex); snap.state=EngineState::Stopping; }
        wait(.2);
        const int result=failure.empty() && passed(false,0,true,stats) ? 0:1;
        if(result && failure.empty()) failure="Studio stopped before video was captured and delivered";
        if(result) event(EventLevel::Fault,failure);
        event(EventLevel::Info,std::string(result?"FAIL":"PASS")+" captured="+std::to_string(stats.captured)+" submitted="+std::to_string(stats.submitted)+" (simulated)");
        std::lock_guard lock(mutex);
        snap.state=result?EngineState::Failed:EngineState::Stopped; snap.message=result?failure:std::string(); snap.result=result;
    }
public:
    explicit FakeControl(FakeOptions o):options(o) {}
    ~FakeControl() override { stopRequested=true; if(worker.joinable()) worker.join(); }
    std::vector<DeviceInfo> enumerateDevices() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // driver queries are not instant
        DeviceInfo device; device.name="Simulated Corvid 44"; device.serial="SIM00000"; device.videoChannels=4; return {device};
    }
    void start(const EngineConfig& config) override {
        if(!validKey(config.key)) throw std::invalid_argument("Threshold must be 0..1; softness >0..1");
        if(active()) throw std::logic_error("Studio engine is already running");
        if(worker.joinable()) worker.join();
        {
            std::lock_guard lock(mutex); const auto dropped=snap.droppedEvents; snap=EngineSnapshot{};
            snap.state=EngineState::Starting; snap.message="Checking AJA card ownership..."; snap.config=config; snap.key=config.key; snap.droppedEvents=dropped;
        }
        stopRequested=false;
        event(EventLevel::Info,"Start: device "+config.device+" | SDI "+std::to_string(config.input)+" -> SDI "+std::to_string(config.output)+" | sender "+config.sender+" (simulated)");
        worker=std::thread([this,config]{ run(config); });
    }
    void stop() override { stopRequested=true; }
    bool active() const override { std::lock_guard lock(mutex); return isActive(snap.state); }
    EngineSnapshot snapshot() const override { std::lock_guard lock(mutex); return snap; }
    bool setKeyParams(float threshold,float softness) override {
        if(!validKey({threshold,softness})) return false;
        std::lock_guard lock(mutex); snap.key={threshold,softness}; return true;
    }
    std::vector<EngineEvent> drainEvents() override {
        std::lock_guard lock(mutex);
        std::vector<EngineEvent> result(std::make_move_iterator(events.begin()),std::make_move_iterator(events.end()));
        events.clear(); return result;
    }
    bool simulated() const override { return true; }
};
}
std::unique_ptr<Control> makeFakeControl(FakeOptions options) { return std::make_unique<FakeControl>(options); }
}
