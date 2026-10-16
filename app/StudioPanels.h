#pragma once
#include "RustLookAndFeel.h"
#include "studio_control.hpp"
#include <array>
#include <deque>
#include <functional>
#include <vector>

// UI building blocks. Everything here runs on the message thread and only
// consumes studio::EngineSnapshot / EngineEvent values; none of it calls the engine.

class RustPanel : public juce::Component {
public:
    explicit RustPanel(juce::String title);
    void paint(juce::Graphics&) override;
protected:
    juce::Rectangle<int> content() const; // area inside the frame, below the title
private:
    juce::String title;
};

class Sparkline : public juce::Component {
public:
    explicit Sparkline(float minimumRange=100,float warnAt=-1);
    void push(double value); // negative = unavailable (gap)
    void clear();
    void paint(juce::Graphics&) override;
private:
    static constexpr int capacity=240; // 60 s at the 4 Hz monitor cadence
    std::array<float,capacity> values{};
    int head=0,count=0;
    float minimumRange,warnAt;
};

enum class Severity { Neutral,Warn,Fault };

class StatTile : public juce::Component {
public:
    StatTile(juce::String caption,Severity whenIncreasing);
    void setCounter(uint64_t value,bool newSample); // colours the tile if it rose in the last 5 s
    void setText(juce::String text);
    void reset();
    void paint(juce::Graphics&) override;
private:
    juce::String caption,value="-";
    Severity whenIncreasing;
    std::deque<uint64_t> recent;
    uint64_t risen=0;
};

class AgeRow : public juce::Component {
public:
    AgeRow(juce::String name,float minimumRange,float warnAt);
    void set(double ms,bool newSample);
    void reset();
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    juce::String name; double current=-1; float warnAt;
    Sparkline spark;
};

class StatusHeader : public juce::Component {
public:
    void set(const studio::EngineSnapshot&,bool simulated,const juce::String& route);
    void paint(juce::Graphics&) override;
private:
    studio::EngineState state=studio::EngineState::Idle;
    juce::String elapsed,message,route; bool simulated=false,passed=false;
};

class TransportPanel : public RustPanel {
public:
    TransportPanel();
    std::function<void()> onStart,onStop,onRescan,onChanged;
    void setScanning();
    void setDevices(const std::vector<studio::DeviceInfo>&,int preferredIndex);
    void setScanFailed(const juce::String& reason);
    void setActive(bool active);
    juce::String deviceId() const;   // AJA device argument ("0", "1", ...), empty if none
    int deviceIndex() const;
    unsigned input() const;
    unsigned output() const;
    juce::String sender() const;
    void setRouting(unsigned input,unsigned output,const juce::String& sender);
    juce::String routeText() const;
    void resized() override;
private:
    void fillConnectors(unsigned channels);
    void validate();
    juce::Label deviceLabel{{},"AJA card"},inputLabel{{},"Camera in"},outputLabel{{},"Program out"},senderLabel{{},"Blender sender"},hint;
    juce::ComboBox device,inputBox,outputBox;
    juce::TextEditor senderEdit;
    juce::TextButton rescan{"Rescan"},start{"START"},stop{"STOP"};
    std::vector<studio::DeviceInfo> devices;
    bool active=false,scanning=false;
};

class KeyPanel : public RustPanel {
public:
    KeyPanel();
    std::function<void(studio::KeyParams,bool settled)> onKeyChanged; // settled: drag ended / typed
    studio::KeyParams key() const;
    void setKey(studio::KeyParams);
    void setEngineKey(studio::KeyParams key,bool active);
    void resized() override;
private:
    void configure(juce::Slider&,double low,double high,double fallback);
    juce::Slider threshold,softness;
    juce::Label thresholdLabel{{},"THRESHOLD"},softnessLabel{{},"SOFTNESS"},note,engineKey;
    juce::TextButton reset{"Reset key"};
};

class DiagnosticsPanel : public RustPanel {
public:
    DiagnosticsPanel();
    void update(const studio::EngineSnapshot&,bool newSample);
    void reset();
    void resized() override;
private:
    StatTile captured{"Captured",Severity::Neutral},submitted{"SDI out",Severity::Neutral},repeats{"Repeats",Severity::Warn},
        fallbacks{"Cam fallback",Severity::Warn},poolFull{"Pool full",Severity::Warn},blenderTicks{"Blender rep/skip",Severity::Warn},
        inputDrops{"AJA in drops",Severity::Fault},outputDrops{"AJA out drops",Severity::Fault},captureFail{"Capture fails",Severity::Fault},
        outputFail{"Output fails",Severity::Fault},inputLevel{"AJA in level",Severity::Neutral},outputLevel{"AJA out level",Severity::Neutral};
    AgeRow cameraAge{"Camera ready age",60,-1},blenderAge{"Blender receive age",100,500},renderAge{"Blender render age",100,500},
        trackingAge{"Tracking receive age",100,500},poseAge{"Pose target age",100,500},compositeTime{"Composite API time",5,-1};
    juce::Label source,limits;
    std::vector<StatTile*> tiles() { return {&captured,&submitted,&repeats,&fallbacks,&poolFull,&blenderTicks,&inputDrops,&outputDrops,&captureFail,&outputFail,&inputLevel,&outputLevel}; }
    std::vector<AgeRow*> ages() { return {&cameraAge,&blenderAge,&renderAge,&trackingAge,&poseAge,&compositeTime}; }
};

class EventLogPanel : public RustPanel, private juce::ListBoxModel {
public:
    EventLogPanel();
    std::function<void()> onOpenLogs;
    void add(const studio::EngineEvent&);
    void addLocal(Severity,const juce::String& text); // UI-originated entries (operator actions)
    void setCsvPath(const juce::String& path);
    void resized() override;
private:
    struct Row { juce::String time,text; Severity severity; bool local; };
    int getNumRows() override { return int(rows.size()); }
    void paintListBoxItem(int row,juce::Graphics&,int width,int height,bool selected) override;
    void append(Row);
    std::deque<Row> rows;
    juce::ListBox list{"events",this};
    juce::Label csv;
    juce::TextButton openLogs{"Open logs"};
};
