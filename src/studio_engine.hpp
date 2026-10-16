#pragma once
#include "studio_control.hpp"
#include "studio_types.hpp"
#include <memory>
#include <vector>

namespace studio {
// Thin control API shared by the CLI, the Win32 test UI and the JUCE app.
// Everything except enumerateDevices() is safe to call from a UI thread: no
// method touches AJA, Spout or D3D11. A monitor thread owns the card, samples
// the runtime every 250 ms, writes the CSV and decides pass/fail.
class Engine {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Engine();
    ~Engine(); // stops and joins an active run
    Engine(const Engine&)=delete;
    Engine& operator=(const Engine&)=delete;
    // Queries the AJA driver. Blocking; call it off the UI thread.
    static std::vector<DeviceInfo> enumerateDevices();
    // Starts a run on the monitor thread. Throws std::logic_error while a run is
    // active and std::invalid_argument for out-of-range key parameters.
    void start(const EngineConfig& config);
    void stop();   // requests a stop and returns immediately
    int wait();    // joins the monitor thread; returns 0 on pass, 1 on fail
    bool active() const;
    EngineSnapshot snapshot() const; // cached copy refreshed by the monitor thread
    // Applies to the next composited frame. Returns false (and changes nothing) if out of range.
    bool setKeyParams(float threshold,float softness);
    std::vector<EngineEvent> drainEvents(); // bounded queue; oldest events drop when full
};
// The real engine behind the UI-facing studio::Control interface.
std::unique_ptr<Control> makeEngineControl();
}
