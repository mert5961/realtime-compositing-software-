#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace studio {
uint64_t nowNs();
struct CompositeInfo {
    uint64_t blenderSequence=0, blenderGeneration=0, blenderReceivedNs=0;
    uint64_t blenderRenderBeginNs=0,trackingReceivedNs=0,blenderSession=0;
    bool usedBlender=false;
    double processingMs=0;
};
class GpuCompositor {
    friend int gpuSelfTest();
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    GpuCompositor(unsigned width,unsigned height,std::string sender,float threshold=.12f,float softness=.10f);
    ~GpuCompositor();
    CompositeInfo render(const void* uyvy,unsigned pitch,void* output);
    std::string sourceStatus() const;
};
int gpuSelfTest();
}
