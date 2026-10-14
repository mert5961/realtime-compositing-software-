#pragma once
#include "ntv2burn.h"
#include "studio_gpu.hpp"
#include "compositor/live_pool.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace studio {
struct OutputFrame {
    std::vector<unsigned char> bytes;
    uint64_t cameraSequence=0,cameraReadyNs=0;
    CompositeInfo composite;
};
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
class Runtime : public NTV2Burn {
    std::atomic<bool> stopping{false};
    std::thread captureThread,outputThread;
    std::unique_ptr<compositor::LivePool<OutputFrame>> completed;
    std::shared_ptr<OutputFrame> black;
    std::unique_ptr<GpuCompositor> engine;
    mutable std::mutex statsMutex;
    RuntimeStats stats;
    std::string sender;
    float threshold,softness;
    unsigned faultEvery;
    void fault(std::string message);
    void capture(); void output();
protected:
    AJAStatus SetupHostBuffers() override { return AJA_STATUS_SUCCESS; }
public:
    Runtime(const BurnConfig& config,std::string sender,float threshold,float softness,unsigned faultEvery=0);
    ~Runtime() override;
    AJAStatus Run() override;
    void Quit() override;
    RuntimeStats snapshot();
    unsigned width() const { return mFormatDesc.GetRasterWidth(); }
    unsigned height() const { return mFormatDesc.GetVisibleRasterHeight(); }
};
}
