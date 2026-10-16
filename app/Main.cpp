#include "RustLookAndFeel.h"
#include "StudioPanels.h"
#include "studio_control.hpp"
#if STUDIO_HAS_ENGINE
#include "studio_engine.hpp"
#endif

using namespace juce;

namespace ids {
const Identifier settings{"StudioSettings"},device{"device"},input{"input"},output{"output"},sender{"sender"},threshold{"threshold"},softness{"softness"};
}

// Logs go to <project>/output like the CLI, found from the executable location
// so a double-clicked exe and the launcher script write to the same place.
static File logDirectory() {
    for(auto dir=File::getSpecialLocation(File::currentExecutableFile).getParentDirectory();dir.exists() && !dir.isRoot();dir=dir.getParentDirectory())
        if(dir.getChildFile("STUDIO_RUNTIME.md").existsAsFile()) return dir.getChildFile("output");
    return File::getCurrentWorkingDirectory().getChildFile("output");
}

//==============================================================================
// Polls the engine on a 5 Hz timer. The message thread never calls AJA, Spout
// or D3D11: device scans run on `scanner`, everything else is a Control call.
class MainComponent : public Component, private Timer {
public:
    MainComponent(std::unique_ptr<studio::Control> c,PropertiesFile& p):control(std::move(c)),properties(p) {
        if(auto xml=properties.getXmlValue(ids::settings.toString())) settings=ValueTree::fromXml(*xml);
        if(!settings.isValid() || !settings.hasType(ids::settings)) settings=ValueTree(ids::settings);
        for(auto* panel:std::initializer_list<Component*>{&header,&transport,&key,&diagnostics,&events}) addAndMakeVisible(panel);

        transport.setRouting(unsigned(int(settings.getProperty(ids::input,1))),unsigned(int(settings.getProperty(ids::output,4))),settings.getProperty(ids::sender,"RealtimeStudio").toString());
        key.setKey({float(double(settings.getProperty(ids::threshold,.12))),float(double(settings.getProperty(ids::softness,.10)))});
        if(!studio::validKey(key.key())) key.setKey({});
        transport.onChanged=[this]{ save(); };
        transport.onRescan=[this]{ scan(); };
        transport.onStart=[this]{ startStudio(); };
        transport.onStop=[this]{ control->stop(); events.addLocal(Severity::Neutral,"Stop requested"); };
        key.onKeyChanged=[this](studio::KeyParams k,bool settled) {
            if(!control->setKeyParams(k.threshold,k.softness)) { events.addLocal(Severity::Warn,"Key rejected (threshold 0..1, softness >0..1)"); return; }
            if(settled) { save(); if(control->active()) events.addLocal(Severity::Neutral,"Key set to threshold "+String(k.threshold,3)+", softness "+String(k.softness,3)); }
        };
        events.onOpenLogs=[this]{ openLogs(); };
        if(control->simulated()) events.addLocal(Severity::Warn,"Simulated engine: no AJA card, Spout or GPU is used. Start with no arguments for the real studio.");
        events.addLocal(Severity::Neutral,"Logs: "+logs.getFullPathName());
        setSize(1380,880);
        scan();
        startTimerHz(5);
    }
    ~MainComponent() override { stopTimer(); save(); }

    void requestQuit(std::function<void()> quit) {
        if(!control->active()) { quit(); return; }
        confirm=AlertWindow::showScopedAsync(MessageBoxOptions::makeOptionsOkCancel(MessageBoxIconType::WarningIcon,"Studio is on air",
            "Quitting stops the studio engine and the SDI program output.\nQuit anyway?","Stop and quit","Keep running",this),
            [quit](int result){ if(result==1) quit(); });
    }

    void paint(Graphics& g) override { g.fillAll(palette::background); }
    void resized() override {
        auto area=getLocalBounds();
        header.setBounds(area.removeFromTop(78));
        area.reduce(10,10);
        auto left=area.removeFromLeft(400); area.removeFromLeft(10);
        transport.setBounds(left.removeFromTop(372)); left.removeFromTop(10); key.setBounds(left);
        diagnostics.setBounds(area.removeFromTop(530)); area.removeFromTop(10); events.setBounds(area);
    }
private:
    void scan() {
        transport.setScanning();
        const int preferred=int(settings.getProperty(ids::device,0));
        scanner.addJob([this,preferred,safe=SafePointer<MainComponent>(this)] {
            std::vector<studio::DeviceInfo> found; String error;
            try { found=control->enumerateDevices(); } catch(const std::exception& e) { error=e.what(); }
            MessageManager::callAsync([safe,found,error,preferred] {
                if(!safe) return;
                if(error.isNotEmpty()) { safe->transport.setScanFailed(error); safe->events.addLocal(Severity::Fault,"Device scan failed: "+error); }
                else safe->transport.setDevices(found,preferred);
            });
        });
    }
    void startStudio() {
        studio::EngineConfig config;
        config.device=transport.deviceId().toStdString(); config.input=transport.input(); config.output=transport.output();
        config.sender=transport.sender().toStdString(); config.key=key.key(); config.logDirectory=logs.getFullPathName().toStdString();
        try { control->start(config); save(); }
        catch(const std::exception& e) { events.addLocal(Severity::Fault,String("Start refused: ")+e.what()); }
    }
    void openLogs() {
        const auto csv=File(snapshot.csvPath);
        if(snapshot.csvPath.size() && csv.existsAsFile()) { csv.revealToUser(); return; }
        logs.createDirectory(); logs.startAsProcess();
    }
    void save() {
        settings.setProperty(ids::device,transport.deviceIndex()>=0 ? transport.deviceId().getIntValue() : int(settings.getProperty(ids::device,0)),nullptr);
        settings.setProperty(ids::input,int(transport.input()),nullptr); settings.setProperty(ids::output,int(transport.output()),nullptr);
        settings.setProperty(ids::sender,transport.sender(),nullptr);
        const auto k=key.key(); settings.setProperty(ids::threshold,double(k.threshold),nullptr); settings.setProperty(ids::softness,double(k.softness),nullptr);
        if(auto xml=settings.createXml()) properties.setValue(ids::settings.toString(),xml.get());
    }
    void timerCallback() override {
        snapshot=control->snapshot();
        const bool active=studio::isActive(snapshot.state);
        if(snapshot.state==studio::EngineState::Starting && lastState!=studio::EngineState::Starting) { diagnostics.reset(); lastElapsed=-1; }
        const bool newSample=snapshot.hasStats && snapshot.elapsedSeconds!=lastElapsed;
        if(newSample) lastElapsed=snapshot.elapsedSeconds;
        header.set(snapshot,control->simulated(),active || snapshot.hasStats ? routeOf(snapshot.config) : transport.routeText());
        transport.setActive(active);
        key.setEngineKey(snapshot.key,active);
        diagnostics.update(snapshot,newSample);
        for(const auto& e:control->drainEvents()) events.add(e);
        if(snapshot.droppedEvents!=droppedEvents) { events.addLocal(Severity::Warn,String(int64(snapshot.droppedEvents-droppedEvents))+" engine events dropped (queue full)"); droppedEvents=snapshot.droppedEvents; }
        events.setCsvPath(snapshot.csvPath);
        lastState=snapshot.state;
    }
    static String routeOf(const studio::EngineConfig& c) {
        return "Card "+String(c.device)+"  |  SDI "+String(int(c.input))+" -> SDI "+String(int(c.output))+"  |  "+String(c.sender);
    }

    std::unique_ptr<studio::Control> control; // declared before `scanner`: scan jobs finish before it is destroyed
    PropertiesFile& properties;
    ValueTree settings{ids::settings};
    File logs=logDirectory();
    StatusHeader header; TransportPanel transport; KeyPanel key; DiagnosticsPanel diagnostics; EventLogPanel events;
    studio::EngineSnapshot snapshot;
    studio::EngineState lastState=studio::EngineState::Idle;
    double lastElapsed=-1; uint64_t droppedEvents=0;
    ScopedMessageBox confirm;
    ThreadPool scanner{1};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

//==============================================================================
class StudioControlApplication : public JUCEApplication {
public:
    const String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return false; } // two instances would compete for the card
    void anotherInstanceStarted(const String&) override { if(window) window->toFront(true); }

    void initialise(const String& commandLine) override {
        PropertiesFile::Options options;
        options.applicationName="RealtimeStudioControl"; options.filenameSuffix=".settings"; options.folderName="RealtimeStudio";
        options.osxLibrarySubFolder="Application Support"; options.millisecondsBeforeSaving=1000;
        properties=std::make_unique<PropertiesFile>(options);
        look=std::make_unique<RustLookAndFeel>(); // textures generated once, here
        LookAndFeel::setDefaultLookAndFeel(look.get());

        const auto args=StringArray::fromTokens(commandLine,true);
        std::unique_ptr<studio::Control> control;
        bool fake=args.contains("--fake");
       #if !STUDIO_HAS_ENGINE
        fake=true; // built without AJA/Spout
       #endif
        if(fake) {
            studio::FakeOptions fakeOptions;
            if(const int i=args.indexOf("--fake-fault");i>=0) fakeOptions.failAfter=args[i+1].getDoubleValue();
            control=studio::makeFakeControl(fakeOptions);
        }
       #if STUDIO_HAS_ENGINE
        else control=studio::makeEngineControl();
       #endif
        window=std::make_unique<MainWindow>(getApplicationName()+(fake ? " (SIMULATED)" : String()),std::move(control),*properties);
        if(auto state=properties->getValue("window");state.isNotEmpty()) window->restoreWindowStateFromString(state);
    }
    void shutdown() override {
        if(window) properties->setValue("window",window->getWindowStateAsString());
        window=nullptr; // stops and joins the engine
        properties->saveIfNeeded();
        LookAndFeel::setDefaultLookAndFeel(nullptr);
        look=nullptr;
    }
    void systemRequestedQuit() override {
        if(window) window->content().requestQuit([]{ JUCEApplication::getInstance()->quit(); });
        else quit();
    }
private:
    class MainWindow : public DocumentWindow {
    public:
        MainWindow(const String& name,std::unique_ptr<studio::Control> control,PropertiesFile& properties)
            :DocumentWindow(name,palette::background,DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(std::move(control),properties),true);
            setResizable(true,true); setResizeLimits(1100,760,10000,10000);
            centreWithSize(getWidth(),getHeight()); setVisible(true);
        }
        MainComponent& content() { return *static_cast<MainComponent*>(getContentComponent()); }
        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<RustLookAndFeel> look; // outlives the window
    std::unique_ptr<PropertiesFile> properties;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION(StudioControlApplication)
