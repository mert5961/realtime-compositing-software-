#include <windows.h>
#include "studio_engine.hpp"
#include "studio_runtime.hpp"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"
#include "sdi_profile.hpp"
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace studio {
static_assert(std::atomic<KeyParams>::is_always_lock_free,"Live key parameters must never block the capture thread");
namespace {
constexpr size_t eventCapacity=256;
bool exited(int32_t pid) {
    HANDLE handle=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));
    if(!handle) return GetLastError()==ERROR_INVALID_PARAMETER;
    bool dead=WaitForSingleObject(handle,0)==WAIT_OBJECT_0; CloseHandle(handle); return dead;
}
}

struct Engine::Impl {
    mutable std::mutex mutex; // guards snap and events; never held across AJA calls
    EngineSnapshot snap;
    std::deque<EngineEvent> events; uint64_t eventSequence=0;
    std::atomic<bool> stopRequested{false};
    std::atomic<KeyParams> key{KeyParams{}}; // read by the capture thread before every composite
    std::thread worker;
    void event(EventLevel level,std::string text) {
        std::lock_guard lock(mutex);
        if(events.size()==eventCapacity) { events.pop_front(); ++snap.droppedEvents; }
        events.push_back({++eventSequence,std::chrono::system_clock::now(),level,std::move(text)});
    }
    void status(EngineState state,std::string message) { std::lock_guard lock(mutex); snap.state=state; snap.message=std::move(message); }
    void run(const EngineConfig& config);
};

void Engine::Impl::run(const EngineConfig& config) {
    int result=1; std::string failure; RuntimeStats stats;
    try {
        CNTV2Card card;
        if(!CNTV2DeviceScanner::GetFirstDeviceFromArgument(config.device,card)) throw std::runtime_error("AJA card not found");
        ULWord signature=0; int32_t pid=0;
        if(!card.GetStreamingApplication(signature,pid)) throw std::runtime_error("Cannot read AJA owner");
        if(pid && pid!=int32_t(GetCurrentProcessId())) {
            if(!exited(pid)) throw std::runtime_error("AJA card is owned by PID "+std::to_string(pid)+". Stop its AJA session first.");
            if(!card.ReleaseStreamForApplication(signature,pid)) throw std::runtime_error("Cannot release exited AJA owner");
            event(EventLevel::Info,"Released AJA card from exited PID "+std::to_string(pid));
        }
        const unsigned channels=card.features().GetNumVideoChannels();
        if(!config.input || !config.output || config.input==config.output || config.input>channels || config.output>channels) throw std::runtime_error("Choose two valid different SDI connectors");
        BurnConfig burn(config.device); burn.fInputChannel=NTV2Channel(config.input-1); burn.fInputSource=NTV2ChannelToInputSource(burn.fInputChannel);
        burn.fOutputChannel=NTV2Channel(config.output-1); burn.fOutputDest=NTV2ChannelToOutputDestination(burn.fOutputChannel);
        burn.fPixelFormat=NTV2_FBF_8BIT_YCBCR; burn.fSuppressAudio=true; burn.fWithAnc=false; burn.fTimecodeSource=NTV2_TCINDEX_INVALID;
        burn.fInputFrames=NTV2ACFrameRange(4); burn.fOutputFrames=NTV2ACFrameRange(4);
        sdi_profile::reset();
        Runtime runtime(burn,config.sender,key,config.faultEvery);
        status(EngineState::Starting,"Initializing studio video path...");
        if(AJA_FAILURE(runtime.Init())) throw std::runtime_error("AJA initialization failed. Check camera signal and card ownership.");
        const auto format=card.GetInputVideoFormat(burn.fInputSource);
        if(AJA_FAILURE(runtime.Run())) throw std::runtime_error("Studio runtime supports progressive 1080p/720p BT.709 video only");
        std::filesystem::create_directories(config.logDirectory);
        const auto prefix=config.logDirectory+"/studio-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::ofstream csv(prefix+".csv"); if(!csv) throw std::runtime_error("Cannot open studio CSV");
        csv << csvHeader;
        { std::lock_guard lock(mutex); snap.state=EngineState::Running; snap.message.clear(); snap.csvPath=prefix+".csv"; }
        event(EventLevel::Info,"Running "+NTV2VideoFormatToString(format,true)+" | CSV "+prefix+".csv");
        const auto start=std::chrono::steady_clock::now(); bool complete=false,error=false;
        while(!stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            stats=runtime.snapshot(); const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            writeCsvRow(csv,elapsed,stats,key.load()); csv.flush();
            { std::lock_guard lock(mutex); snap.stats=stats; snap.hasStats=true; snap.elapsedSeconds=elapsed; }
            const auto verdict=assess(stats,elapsed,config.seconds,bool(csv),card.GetInputVideoFormat(burn.fInputSource)!=format);
            if(verdict.stop) { error=verdict.error; complete=verdict.complete; failure=verdict.reason; break; }
        }
        status(EngineState::Stopping,failure);
        runtime.Quit(); sdi_profile::stopping=true;
        if(!sdi_profile::dump(prefix)) { error=true; failure="Studio profiling save failed"; }
        result=passed(error,config.seconds,complete,stats) ? 0:1;
        if(result && failure.empty()) failure="Studio stopped before video was captured and delivered";
    } catch(const std::exception& error) { failure=error.what(); }
    if(result) event(EventLevel::Fault,failure);
    event(EventLevel::Info,std::string(result==0?"PASS":"FAIL")+" captured="+std::to_string(stats.captured)+" submitted="+std::to_string(stats.submitted)
        +" repeats="+std::to_string(stats.repeats)+" input_drops="+std::to_string(stats.inputDrops)+" output_drops="+std::to_string(stats.outputDrops));
    std::lock_guard lock(mutex);
    snap.state=result ? EngineState::Failed:EngineState::Stopped; snap.message=result ? failure:std::string(); snap.result=result;
}

Engine::Engine():impl_(std::make_unique<Impl>()) {}
Engine::~Engine() { stop(); if(impl_->worker.joinable()) impl_->worker.join(); }

std::vector<DeviceInfo> Engine::enumerateDevices() {
    std::vector<DeviceInfo> devices;
    for(ULWord index=0;;++index) {
        CNTV2Card card; if(!CNTV2DeviceScanner::GetDeviceAtIndex(index,card)) break;
        DeviceInfo info; info.index=index; info.name=card.GetDisplayName(); card.GetSerialNumberString(info.serial);
        info.videoChannels=card.features().GetNumVideoChannels();
        ULWord signature=0; card.GetStreamingApplication(signature,info.ownerPid);
        if(info.ownerPid==int32_t(GetCurrentProcessId())) info.ownerPid=0;
        devices.push_back(std::move(info));
    }
    return devices;
}

void Engine::start(const EngineConfig& config) {
    if(!validKey(config.key)) throw std::invalid_argument("Threshold must be 0..1; softness >0..1");
    if(active()) throw std::logic_error("Studio engine is already running");
    if(impl_->worker.joinable()) impl_->worker.join();
    {
        std::lock_guard lock(impl_->mutex); auto& snap=impl_->snap;
        const auto dropped=snap.droppedEvents; snap=EngineSnapshot{};
        snap.state=EngineState::Starting; snap.message="Checking AJA card ownership..."; snap.config=config; snap.key=config.key; snap.droppedEvents=dropped;
    }
    impl_->stopRequested=false; impl_->key=config.key;
    impl_->event(EventLevel::Info,"Start: device "+config.device+" | SDI "+std::to_string(config.input)+" -> SDI "+std::to_string(config.output)+" | sender "+config.sender);
    impl_->worker=std::thread([impl=impl_.get(),config]{ impl->run(config); });
}
void Engine::stop() { impl_->stopRequested=true; }
int Engine::wait() {
    if(impl_->worker.joinable()) impl_->worker.join();
    std::lock_guard lock(impl_->mutex); return impl_->snap.result;
}
bool Engine::active() const { std::lock_guard lock(impl_->mutex); return isActive(impl_->snap.state); }
EngineSnapshot Engine::snapshot() const { std::lock_guard lock(impl_->mutex); return impl_->snap; }
bool Engine::setKeyParams(float threshold,float softness) {
    const KeyParams key{threshold,softness};
    if(!validKey(key)) return false;
    // Not an event: slider drags would flood the bounded fault log. The CSV records every sample's key.
    impl_->key=key;
    std::lock_guard lock(impl_->mutex); impl_->snap.key=key; return true;
}
std::vector<EngineEvent> Engine::drainEvents() {
    std::lock_guard lock(impl_->mutex);
    std::vector<EngineEvent> result(std::make_move_iterator(impl_->events.begin()),std::make_move_iterator(impl_->events.end()));
    impl_->events.clear(); return result;
}

namespace {
class EngineControl final : public Control {
    Engine engine;
public:
    std::vector<DeviceInfo> enumerateDevices() override { return Engine::enumerateDevices(); }
    void start(const EngineConfig& config) override { engine.start(config); }
    void stop() override { engine.stop(); }
    bool active() const override { return engine.active(); }
    EngineSnapshot snapshot() const override { return engine.snapshot(); }
    bool setKeyParams(float threshold,float softness) override { return engine.setKeyParams(threshold,softness); }
    std::vector<EngineEvent> drainEvents() override { return engine.drainEvents(); }
    bool simulated() const override { return false; }
};
}
std::unique_ptr<Control> makeEngineControl() { return std::make_unique<EngineControl>(); }
}
