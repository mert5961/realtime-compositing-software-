#pragma once
#include "ntv2burn.h"
#include "studio_gpu.hpp"
#include "studio_types.hpp"
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
class Runtime : public NTV2Burn {
    std::atomic<bool> stopping{false};
    std::thread captureThread,outputThread;
    std::unique_ptr<compositor::LivePool<OutputFrame>> completed;
    std::shared_ptr<OutputFrame> black;
    std::unique_ptr<GpuCompositor> engine;
    mutable std::mutex statsMutex;
    RuntimeStats stats;
    std::string sender;
    const std::atomic<KeyParams>& key; // owned by the caller; outlives the runtime
    unsigned faultEvery;
    void fault(std::string message);
    void capture(); void output();
protected:
    AJAStatus SetupHostBuffers() override { return AJA_STATUS_SUCCESS; }
public:
    Runtime(const BurnConfig& config,std::string sender,const std::atomic<KeyParams>& key,unsigned faultEvery=0);
    ~Runtime() override;
    AJAStatus Run() override;
    void Quit() override;
    RuntimeStats snapshot();
    unsigned width() const { return mFormatDesc.GetRasterWidth(); }
    unsigned height() const { return mFormatDesc.GetVisibleRasterHeight(); }
};
}
