#include "studio_runtime.hpp"
#include "sdi_profile.hpp"
#include <cstring>
#include <stdexcept>

namespace studio {
Runtime::Runtime(const BurnConfig& config,std::string name,const std::atomic<KeyParams>& k,unsigned every):NTV2Burn(config),sender(std::move(name)),key(k),faultEvery(every) {}
Runtime::~Runtime() { Quit(); }
void Runtime::fault(std::string message) { std::lock_guard lock(statsMutex); if(stats.fault.empty()) stats.fault=std::move(message); }
AJAStatus Runtime::Run() {
    // This milestone validates progressive BT.709 HD, video only. No guessed
    // interlaced/SD color interpretation and no implicit format conversions.
    if((width()!=1920 || height()!=1080) && (width()!=1280 || height()!=720)) return AJA_STATUS_UNSUPPORTED;
    if(!IsProgressivePicture(mVideoFormat) || mConfig.fPixelFormat!=NTV2_FBF_8BIT_YCBCR) return AJA_STATUS_UNSUPPORTED;
    // Lock output cadence to the selected camera input on this HD path. This
    // is reference selection, not exposure/tracking clock calibration.
    if(!mDevice.SetReference(NTV2InputSourceToReferenceSource(mConfig.fInputSource))) return AJA_STATUS_INITIALIZE;
    const size_t bytes=size_t(width())*height()*2;
    completed=std::make_unique<compositor::LivePool<OutputFrame>>(3,[&](size_t){ OutputFrame frame; frame.bytes.resize(bytes); return frame; });
    black=std::make_shared<OutputFrame>(); black->bytes.resize(bytes);
    for(size_t i=0;i<bytes;i+=4) { black->bytes[i]=black->bytes[i+2]=128; black->bytes[i+1]=black->bytes[i+3]=16; }
    engine=std::make_unique<GpuCompositor>(width(),height(),sender,key.load());
    mDevice.AutoCirculateStop(mConfig.fInputChannel); mDevice.AutoCirculateStop(mConfig.fOutputChannel);
    if(!mDevice.AutoCirculateInitForInput(mConfig.fInputChannel,mConfig.fInputFrames,NTV2_AUDIOSYSTEM_INVALID,0)
        || !mDevice.AutoCirculateInitForOutput(mConfig.fOutputChannel,mConfig.fOutputFrames,NTV2_AUDIOSYSTEM_INVALID,0)) return AJA_STATUS_INITIALIZE;
    AUTOCIRCULATE_TRANSFER transfer; transfer.SetVideoBuffer(reinterpret_cast<ULWord*>(black->bytes.data()),ULWord(bytes));
    // Startup prefill is explicit. Output then advances on hardware vertical
    // opportunities; Blender is never called or waited on by this thread.
    for(unsigned i=0;i<3;++i) if(!mDevice.AutoCirculateTransfer(mConfig.fOutputChannel,transfer)) return AJA_STATUS_FAIL;
    if(!mDevice.AutoCirculateStart(mConfig.fOutputChannel) || !mDevice.AutoCirculateStart(mConfig.fInputChannel)) return AJA_STATUS_FAIL;
    captureThread=std::thread([this]{capture();}); outputThread=std::thread([this]{output();});
    return AJA_STATUS_SUCCESS;
}
void Runtime::Quit() {
    stopping=true;
    if(captureThread.joinable()) captureThread.join();
    if(outputThread.joinable()) outputThread.join();
    if(completed) { mDevice.AutoCirculateStop(mConfig.fInputChannel); mDevice.AutoCirculateStop(mConfig.fOutputChannel); }
    NTV2Burn::Quit();
}
RuntimeStats Runtime::snapshot() {
    AUTOCIRCULATE_STATUS input,output; GetStatus(input,output);
    std::lock_guard lock(statsMutex);
    auto result=stats; result.inputDrops=input.GetDroppedFrameCount(); result.outputDrops=output.GetDroppedFrameCount();
    result.inputLevel=input.GetBufferLevel(); result.outputLevel=output.GetBufferLevel(); return result;
}
void Runtime::capture() {
    try {
        sdi_profile::lane=0;
        NTV2Buffer camera(mFormatDesc.GetVideoWriteSize());
        AUTOCIRCULATE_TRANSFER transfer; transfer.SetVideoBuffer(camera,ULWord(camera.GetByteCount()));
        uint64_t attempts=0;
        while(!stopping) {
            AUTOCIRCULATE_STATUS status;
            if(!mDevice.AutoCirculateGetStatus(mConfig.fInputChannel,status)) { fault("Cannot read capture status"); stopping=true; break; }
            if(!status.IsRunning() || status.GetBufferLevel()<1) {
                sdi_profile::timed(sdi_profile::Kind::InputDeviceWait,[&]{return mDevice.WaitForInputVerticalInterrupt(mConfig.fInputChannel);}); continue;
            }
            const bool injected=faultEvery && ++attempts%faultEvery==0;
            const bool captured=sdi_profile::timed(sdi_profile::Kind::CaptureDma,[&]{
                const bool ok=mDevice.AutoCirculateTransfer(mConfig.fInputChannel,transfer);
                if(injected) { memset(camera.GetHostPointer(),0,camera.GetByteCount()/2); return false; }
                return ok;
            });
            const uint64_t ready=nowNs();
            if(!captured) {
                std::lock_guard lock(statsMutex); ++stats.captureFailures; if(injected) ++stats.injectedFailures;
                else if(stats.fault.empty()) stats.fault="Capture DMA failed; rejected frame, repeating valid output";
                continue; // No pixels processed or published from this failed DMA.
            }
            uint64_t sequence;
            { std::lock_guard lock(statsMutex); sequence=++stats.captured; }
            auto reservation=completed->tryWrite();
            if(!reservation) { std::lock_guard lock(statsMutex); ++stats.poolFull; continue; }
            auto& frame=reservation->get();
            const auto begin=std::chrono::steady_clock::now();
            engine->setKey(key.load(std::memory_order_relaxed)); // live tuning; lock-free, never waits
            frame.composite=engine->render(mFormatDesc.GetTopVisibleRowAddress(camera),mFormatDesc.GetBytesPerRow(),frame.bytes.data());
            sdi_profile::record(sdi_profile::Kind::Processing,begin);
            frame.cameraSequence=sequence; frame.cameraReadyNs=ready;
            // The compositor readback has completed. Output receives immutable
            // host bytes and holds a lease throughout synchronous AJA DMA.
            reservation->publish();
            { std::lock_guard lock(statsMutex); stats.sourceStatus=engine->sourceStatus(); }
        }
    } catch(const std::exception& error) { fault(std::string("Capture/composite fault: ")+error.what()); stopping=true; }
}
void Runtime::output() {
    try {
        sdi_profile::lane=1;
        std::shared_ptr<const OutputFrame> selected=black;
        uint64_t previous=0;
        uint64_t previousBlenderSession=0,previousBlenderTick=0;
        while(!stopping) {
            const bool opportunity=sdi_profile::timed(sdi_profile::Kind::OutputDeviceWait,[&]{return mDevice.WaitForOutputVerticalInterrupt(mConfig.fOutputChannel);});
            if(!opportunity) { fault("Output clock interrupt failed"); stopping=true; break; }
            AUTOCIRCULATE_STATUS status;
            if(!mDevice.AutoCirculateGetStatus(mConfig.fOutputChannel,status)) { fault("Output status unavailable"); stopping=true; break; }
            if(!status.CanAcceptMoreOutputFrames()) continue;
            auto latest=completed->latest();
            if(latest) selected=std::move(latest);
            AUTOCIRCULATE_TRANSFER transfer; transfer.SetVideoBuffer(reinterpret_cast<ULWord*>(const_cast<unsigned char*>(selected->bytes.data())),ULWord(selected->bytes.size()));
            const bool ok=sdi_profile::timed(sdi_profile::Kind::OutputDma,[&]{return mDevice.AutoCirculateTransfer(mConfig.fOutputChannel,transfer);});
            const uint64_t submitted=nowNs();
            std::lock_guard lock(statsMutex);
            if(!ok) { ++stats.outputFailures; if(stats.fault.empty()) stats.fault="Output DMA failed"; continue; }
            ++stats.submitted;
            if(selected->cameraSequence==previous) ++stats.repeats;
            if(!selected->composite.usedBlender) ++stats.fallbacks;
            previous=selected->cameraSequence;
            stats.cameraSequence=selected->cameraSequence; stats.blenderSequence=selected->composite.blenderSequence;
            stats.blenderGeneration=selected->composite.blenderGeneration;
            stats.cameraReadyAgeMs=selected->cameraReadyNs ? double(submitted-selected->cameraReadyNs)/1e6 : -1;
            stats.blenderReceiveAgeMs=selected->composite.blenderReceivedNs ? double(submitted-selected->composite.blenderReceivedNs)/1e6 : -1;
            stats.processingMs=selected->composite.processingMs;
            stats.blenderRenderAgeMs=selected->composite.blenderRenderBeginNs ? double(submitted-selected->composite.blenderRenderBeginNs)/1e6 : -1;
            stats.trackingReceiveAgeMs=selected->composite.trackingReceivedNs>1 ? double(submitted-selected->composite.trackingReceivedNs)/1e6 : -1;
            stats.blenderSession=selected->composite.blenderSession;
            const auto& bg=selected->composite;
            if(bg.usedBlender && bg.blenderScheduledNs) {
                if(previousBlenderSession==bg.blenderSession && previousBlenderTick) {
                    if(bg.blenderScheduledNs==previousBlenderTick) ++stats.blenderRepeats;
                    else if(bg.blenderScheduledNs>previousBlenderTick+20000000)
                        stats.blenderSkipped+=(bg.blenderScheduledNs-previousBlenderTick)/20000000-1;
                }
                previousBlenderSession=bg.blenderSession; previousBlenderTick=bg.blenderScheduledNs;
            } else previousBlenderSession=previousBlenderTick=0;
            stats.poseTargetAgeMs=bg.poseTargetNs ? double(submitted-bg.poseTargetNs)/1e6 : -1;
        }
    } catch(const std::exception& error) { fault(std::string("Output fault: ")+error.what()); stopping=true; }
}
}
