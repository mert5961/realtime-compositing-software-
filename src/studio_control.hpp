#pragma once
#include "studio_types.hpp"
#include <memory>
#include <vector>

namespace studio {
// What a UI needs from an engine. Implemented by the real AJA/Spout engine
// (makeEngineControl, studio_engine.hpp) and by a synthetic engine for UI work
// without hardware (makeFakeControl). Everything except enumerateDevices() is
// safe on a UI thread; enumerateDevices() may block on the driver.
class Control {
public:
    virtual ~Control()=default;
    virtual std::vector<DeviceInfo> enumerateDevices()=0;
    virtual void start(const EngineConfig& config)=0; // throws std::logic_error/std::invalid_argument
    virtual void stop()=0;                            // request only; returns immediately
    virtual bool active() const=0;
    virtual EngineSnapshot snapshot() const=0;
    virtual bool setKeyParams(float threshold,float softness)=0;
    virtual std::vector<EngineEvent> drainEvents()=0;
    virtual bool simulated() const=0;
};

struct FakeOptions {
    double startupSeconds=.8;  // Starting -> Running delay
    double stallEvery=25;      // simulated Blender stall (camera fallback) period; 0 disables
    double stallSeconds=2;
    double failAfter=0;        // >0: simulated runtime fault stops the run after this many seconds
    unsigned sampleMs=250;     // matches the real monitor cadence
};
std::unique_ptr<Control> makeFakeControl(FakeOptions options={});
}
