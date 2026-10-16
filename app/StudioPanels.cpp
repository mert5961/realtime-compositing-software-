#include "StudioPanels.h"
#include <algorithm>
#include <chrono>

using namespace juce;

namespace {
RustLookAndFeel* rustLook(Component& c) { return dynamic_cast<RustLookAndFeel*>(&c.getLookAndFeel()); }
Colour severityColour(Severity s) { return s==Severity::Fault ? palette::fault : s==Severity::Warn ? palette::warn : palette::dimText; }
String formatMs(double ms) { return ms<0 ? String("--") : String(ms,1)+" ms"; }
String formatElapsed(double seconds) {
    const auto total=int64(seconds);
    return String::formatted("%02d:%02d:%02d",int(total/3600),int(total/60%60),int(total%60));
}
void styleLabel(Label& label,float height,Colour colour,bool bold=false) {
    label.setFont(RustLookAndFeel::font(height,bold)); label.setColour(Label::textColourId,colour);
}
}

//==============================================================================
RustPanel::RustPanel(String t):title(std::move(t)) {}

void RustPanel::paint(Graphics& g) {
    const auto bounds=getLocalBounds().toFloat();
    g.setColour(palette::panel); g.fillRoundedRectangle(bounds.reduced(4),6);
    if(auto* look=rustLook(*this)) {
        look->drawRustFrame(g,bounds,8,6);
        for(auto corner:{bounds.getTopLeft().translated(13,13),bounds.getTopRight().translated(-13,13),bounds.getBottomLeft().translated(13,-13),bounds.getBottomRight().translated(-13,-13)})
            look->drawRivet(g,corner,3.5f);
    }
    g.setColour(palette::pink); g.setFont(RustLookAndFeel::font(15,true).withExtraKerningFactor(.12f));
    g.drawText(title.toUpperCase(),Rectangle<int>(26,12,getWidth()-52,22),Justification::centredLeft);
    g.setColour(palette::rust.withAlpha(.6f)); g.fillRect(Rectangle<int>(26,37,getWidth()-52,1));
}

Rectangle<int> RustPanel::content() const { return getLocalBounds().reduced(18).withTrimmedTop(26); }

//==============================================================================
Sparkline::Sparkline(float range,float warn):minimumRange(range),warnAt(warn) { setOpaque(true); }
void Sparkline::push(double value) { values[size_t(head)]=float(value); head=(head+1)%capacity; count=std::min(count+1,capacity); repaint(); }
void Sparkline::clear() { head=count=0; repaint(); }

void Sparkline::paint(Graphics& g) {
    g.fillAll(palette::well);
    const auto area=getLocalBounds().toFloat().reduced(2,3);
    float maximum=0;
    for(int i=0;i<count;++i) maximum=std::max(maximum,values[size_t((head-count+i+capacity)%capacity)]);
    const float range=std::max(minimumRange,maximum*1.15f);
    const auto yFor=[&](float v) { return area.getBottom()-area.getHeight()*std::min(v,range)/range; };
    g.setColour(palette::rust.withAlpha(.35f)); g.drawHorizontalLine(int(yFor(range*.5f)),area.getX(),area.getRight());
    if(warnAt>0 && warnAt<range) {
        const float dashes[]={3,3}; g.setColour(palette::warn.withAlpha(.6f));
        g.drawDashedLine({area.getX(),yFor(warnAt),area.getRight(),yFor(warnAt)},dashes,2,1);
    }
    Path line; bool drawing=false; Point<float> last; bool haveLast=false;
    for(int i=0;i<count;++i) {
        const float v=values[size_t((head-count+i+capacity)%capacity)];
        const float x=area.getRight()-area.getWidth()*float(count-1-i)/float(capacity-1);
        if(v<0) { drawing=false; continue; }
        const Point<float> p(x,yFor(v));
        if(drawing) line.lineTo(p); else line.startNewSubPath(p);
        drawing=true; last=p; haveLast=true;
    }
    g.setColour(palette::pink); g.strokePath(line,PathStrokeType(1.6f,PathStrokeType::curved,PathStrokeType::rounded));
    if(haveLast) g.fillEllipse(Rectangle<float>(5,5).withCentre(last));
    g.setColour(palette::dimText); g.setFont(RustLookAndFeel::font(11));
    g.drawText(String(range,0),getLocalBounds().reduced(4,1),Justification::topLeft);
}

//==============================================================================
StatTile::StatTile(String c,Severity s):caption(std::move(c)),whenIncreasing(s) { setOpaque(true); }

void StatTile::setCounter(uint64_t v,bool newSample) {
    const auto text=String(int64(v));
    if(newSample) { recent.push_back(v); if(recent.size()>21) recent.pop_front(); } // 20 intervals = 5 s
    const uint64_t rose=recent.empty() ? 0 : recent.back()-std::min(recent.back(),recent.front());
    if(text!=value || rose!=risen) { value=text; risen=rose; repaint(); }
}
void StatTile::setText(String text) { if(text!=value) { value=std::move(text); repaint(); } }
void StatTile::reset() { value="-"; recent.clear(); risen=0; repaint(); }

void StatTile::paint(Graphics& g) {
    g.fillAll(palette::panel);
    const auto bounds=getLocalBounds().toFloat().reduced(3);
    const auto alarm=whenIncreasing!=Severity::Neutral && risen>0;
    const auto accent=alarm ? severityColour(whenIncreasing) : palette::pinkDeep;
    g.setColour(alarm ? accent.withAlpha(.16f) : palette::well); g.fillRoundedRectangle(bounds,4);
    g.setColour(accent); g.fillRoundedRectangle(bounds.withWidth(4),2);
    auto text=getLocalBounds().reduced(14,7);
    g.setColour(palette::dimText); g.setFont(RustLookAndFeel::font(12,true).withExtraKerningFactor(.04f));
    g.drawFittedText(caption.toUpperCase(),text.removeFromTop(16),Justification::centredLeft,1,.75f);
    g.setColour(alarm ? accent : palette::text); g.setFont(RustLookAndFeel::font(24,true));
    g.drawText(value,text,Justification::centredLeft);
    if(alarm) { g.setFont(RustLookAndFeel::font(12,true)); g.drawText("+"+String(int64(risen))+" in 5 s",text,Justification::bottomRight); }
}

//==============================================================================
AgeRow::AgeRow(String n,float range,float warn):name(std::move(n)),warnAt(warn),spark(range,warn) { setOpaque(true); addAndMakeVisible(spark); }

void AgeRow::set(double ms,bool newSample) {
    if(newSample) spark.push(ms);
    if(ms!=current) { current=ms; repaint(getLocalBounds().withWidth(spark.getX())); }
}
void AgeRow::reset() { current=-1; spark.clear(); repaint(); }

void AgeRow::paint(Graphics& g) {
    g.fillAll(palette::panel);
    auto area=getLocalBounds().withWidth(spark.getX()-8);
    g.setColour(palette::dimText); g.setFont(RustLookAndFeel::font(14));
    g.drawText(name,area.removeFromLeft(170),Justification::centredLeft);
    const bool stale=warnAt>0 && current>warnAt;
    g.setColour(current<0 ? palette::dimText : stale ? palette::warn : palette::text); g.setFont(RustLookAndFeel::font(17,true));
    g.drawText(formatMs(current),area,Justification::centredRight);
}

void AgeRow::resized() { spark.setBounds(getLocalBounds().withTrimmedLeft(285).reduced(0,2)); }

//==============================================================================
void StatusHeader::set(const studio::EngineSnapshot& snap,bool sim,const String& r) {
    const auto e=snap.hasStats ? formatElapsed(snap.elapsedSeconds) : String();
    const auto m=String(snap.state==studio::EngineState::Running ? snap.stats.sourceStatus : snap.message);
    if(snap.state==state && e==elapsed && m==message && r==route && sim==simulated && passed==(snap.result==0)) return;
    state=snap.state; elapsed=e; message=m; route=r; simulated=sim; passed=snap.result==0; repaint();
}

void StatusHeader::paint(Graphics& g) {
    using studio::EngineState;
    g.fillAll(palette::background);
    auto area=getLocalBounds().reduced(18,10);
    if(auto* look=rustLook(*this)) { g.setFillType(look->rustFill()); g.fillRect(getLocalBounds().removeFromBottom(4)); }
    g.setColour(palette::text); g.setFont(RustLookAndFeel::font(22,true).withExtraKerningFactor(.08f));
    g.drawText("REALTIME STUDIO",area.removeFromLeft(240),Justification::centredLeft);

    Colour badge=palette::dimText; String label=String(studio::toString(state)).toUpperCase();
    switch(state) {
    case EngineState::Running: badge=palette::pink; label="ON AIR"; break;
    case EngineState::Starting: case EngineState::Stopping: badge=palette::warn; break;
    case EngineState::Failed: badge=palette::fault; break;
    case EngineState::Stopped: label=passed ? "STOPPED" : "STOPPED (FAIL)"; break;
    default: break;
    }
    if(simulated && state==EngineState::Running) label="SIMULATING";
    auto badgeArea=area.removeFromLeft(230).reduced(0,4).toFloat();
    g.setColour(badge.withAlpha(state==EngineState::Running ? 1.f : .2f)); g.fillRoundedRectangle(badgeArea,6);
    g.setColour(badge); g.drawRoundedRectangle(badgeArea.reduced(1),6,2);
    g.setColour(state==EngineState::Running ? palette::background : badge); g.setFont(RustLookAndFeel::font(22,true));
    g.drawText(label,badgeArea.toNearestInt(),Justification::centred);
    area.removeFromLeft(16);
    if(elapsed.isNotEmpty()) { g.setColour(palette::text); g.setFont(RustLookAndFeel::font(26,true)); g.drawText(elapsed,area.removeFromLeft(130),Justification::centredLeft); }

    auto right=area.removeFromRight(jmin(460,area.getWidth()/2));
    if(simulated) {
        g.setColour(palette::warn); g.setFont(RustLookAndFeel::font(15,true));
        g.drawText("SIMULATED ENGINE - NOT CONNECTED TO SDI",right.removeFromTop(right.getHeight()/2),Justification::centredRight);
    }
    g.setColour(palette::dimText); g.setFont(RustLookAndFeel::font(15)); g.drawFittedText(route,right,Justification::centredRight,1,.8f);
    g.setColour(state==EngineState::Failed ? palette::fault : palette::dimText); g.setFont(RustLookAndFeel::font(state==EngineState::Failed ? 17.f : 15.f,state==EngineState::Failed));
    g.drawFittedText(message,area.reduced(8,0),Justification::centredLeft,2);
}

//==============================================================================
TransportPanel::TransportPanel():RustPanel("Transport") {
    for(auto* label:{&deviceLabel,&inputLabel,&outputLabel,&senderLabel}) { styleLabel(*label,14,palette::dimText); addAndMakeVisible(*label); }
    styleLabel(hint,13,palette::warn); addAndMakeVisible(hint);
    for(auto* box:{&device,&inputBox,&outputBox}) { addAndMakeVisible(*box); box->onChange=[this]{ validate(); if(onChanged) onChanged(); }; }
    senderEdit.setFont(RustLookAndFeel::font(15)); senderEdit.setText("RealtimeStudio",false); senderEdit.setIndents(8,6);
    senderEdit.onTextChange=[this]{ validate(); if(onChanged) onChanged(); };
    addAndMakeVisible(senderEdit);
    start.setColour(TextButton::buttonColourId,palette::pink); start.setColour(TextButton::textColourOffId,palette::background);
    stop.setColour(TextButton::buttonColourId,Colour(0xff3a2630));
    rescan.onClick=[this]{ if(onRescan) onRescan(); };
    start.onClick=[this]{ if(onStart) onStart(); };
    stop.onClick=[this]{ if(onStop) onStop(); };
    for(auto* b:{&rescan,&start,&stop}) addAndMakeVisible(*b);
    fillConnectors(4); setRouting(1,4,"RealtimeStudio"); setScanning();
}

void TransportPanel::fillConnectors(unsigned channels) {
    const auto in=input(),out=output();
    inputBox.clear(dontSendNotification); outputBox.clear(dontSendNotification);
    for(unsigned i=1;i<=std::max(1u,channels);++i) { inputBox.addItem("SDI "+String(i),int(i)); outputBox.addItem("SDI "+String(i),int(i)); }
    inputBox.setSelectedId(int(in ? in : 1),dontSendNotification); outputBox.setSelectedId(int(out ? out : std::min(4u,channels)),dontSendNotification);
}

void TransportPanel::setScanning() {
    scanning=true; device.clear(dontSendNotification); device.setTextWhenNothingSelected("Scanning AJA devices..."); validate();
}

void TransportPanel::setDevices(const std::vector<studio::DeviceInfo>& found,int preferred) {
    scanning=false; devices=found; device.clear(dontSendNotification);
    for(size_t i=0;i<devices.size();++i) {
        const auto& d=devices[i];
        String text=String(int(d.index))+": "+String(d.name)+(d.serial.empty() ? String() : "  "+String(d.serial));
        if(d.ownerPid) text+="  - IN USE by PID "+String(d.ownerPid);
        device.addItem(text,int(i)+1);
    }
    device.setTextWhenNothingSelected(devices.empty() ? "No AJA device found" : "Choose a card");
    int pick=0; for(size_t i=0;i<devices.size();++i) if(int(devices[i].index)==preferred) pick=int(i);
    if(!devices.empty()) { device.setSelectedId(pick+1,dontSendNotification); fillConnectors(devices[size_t(pick)].videoChannels); }
    validate();
}

void TransportPanel::setScanFailed(const String& reason) {
    scanning=false; devices.clear(); device.clear(dontSendNotification); device.setTextWhenNothingSelected("Scan failed: "+reason); validate();
}

void TransportPanel::setActive(bool a) {
    if(a==active) return;
    active=a;
    for(Component* c:{static_cast<Component*>(&device),static_cast<Component*>(&inputBox),static_cast<Component*>(&outputBox),static_cast<Component*>(&senderEdit),static_cast<Component*>(&rescan)}) c->setEnabled(!active);
    validate();
}

void TransportPanel::validate() {
    String problem;
    if(scanning) problem="Scanning for AJA cards...";
    else if(deviceIndex()<0) problem="No AJA card selected";
    else if(input()==output()) problem="Camera input and program output must be different connectors";
    else if(sender().isEmpty()) problem="Enter the Blender Spout sender name";
    else if(const auto& d=devices[size_t(deviceIndex())]; d.ownerPid)
        problem="Card is owned by PID "+String(d.ownerPid)+"; Start fails unless that process has exited";
    hint.setText(active ? String() : problem,dontSendNotification);
    const bool blocked=scanning || deviceIndex()<0 || input()==output() || sender().isEmpty();
    start.setEnabled(!active && !blocked); stop.setEnabled(active);
}

String TransportPanel::deviceId() const { const auto i=deviceIndex(); return i<0 ? String() : String(int(devices[size_t(i)].index)); }
int TransportPanel::deviceIndex() const { const int id=device.getSelectedId(); return id>0 && size_t(id)<=devices.size() ? id-1 : -1; }
unsigned TransportPanel::input() const { return unsigned(std::max(0,inputBox.getSelectedId())); }
unsigned TransportPanel::output() const { return unsigned(std::max(0,outputBox.getSelectedId())); }
String TransportPanel::sender() const { return senderEdit.getText().trim(); }
void TransportPanel::setRouting(unsigned in,unsigned out,const String& name) {
    inputBox.setSelectedId(int(in),dontSendNotification); outputBox.setSelectedId(int(out),dontSendNotification);
    senderEdit.setText(name,false); validate();
}
String TransportPanel::routeText() const { return "SDI "+String(int(input()))+"  ->  key + '"+sender()+"'  ->  SDI "+String(int(output())); }

void TransportPanel::resized() {
    auto area=content();
    const auto row=[&](Label& label,Component& field) {
        auto line=area.removeFromTop(34); label.setBounds(line.removeFromLeft(112)); field.setBounds(line.reduced(0,1)); area.removeFromTop(8);
    };
    auto first=area.removeFromTop(34); deviceLabel.setBounds(first.removeFromLeft(112)); rescan.setBounds(first.removeFromRight(84)); first.removeFromRight(6); device.setBounds(first.reduced(0,1));
    area.removeFromTop(8);
    row(inputLabel,inputBox); row(outputLabel,outputBox); row(senderLabel,senderEdit);
    hint.setBounds(area.removeFromTop(36));
    auto buttons=area.removeFromTop(52);
    start.setBounds(buttons.removeFromLeft(buttons.getWidth()*3/5).reduced(0,2)); buttons.removeFromLeft(10); stop.setBounds(buttons.reduced(0,2));
}

//==============================================================================
KeyPanel::KeyPanel():RustPanel("Green key") {
    configure(threshold,0,.99,.12); configure(softness,.001,1,.10);
    for(auto* label:{&thresholdLabel,&softnessLabel}) { styleLabel(*label,13,palette::dimText,true); label->setJustificationType(Justification::centred); addAndMakeVisible(*label); }
    styleLabel(note,13,palette::dimText);
    note.setText("Changes reach the next composited frame. Double-click a knob for its default.",dontSendNotification);
    styleLabel(engineKey,14,palette::text,true);
    addAndMakeVisible(note); addAndMakeVisible(engineKey);
    reset.onClick=[this]{ setKey({}); if(onKeyChanged) onKeyChanged(key(),true); };
    addAndMakeVisible(reset);
}

void KeyPanel::configure(Slider& s,double low,double high,double fallback) {
    s.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    s.setRotaryParameters(MathConstants<float>::pi*1.2f,MathConstants<float>::pi*2.8f,true);
    s.setTextBoxStyle(Slider::TextBoxBelow,false,80,24);
    s.setRange(low,high,.001); s.setNumDecimalPlacesToDisplay(3); s.setValue(fallback,dontSendNotification);
    s.setDoubleClickReturnValue(true,fallback); s.setMouseDragSensitivity(400);
    s.onValueChange=[this,&s]{ if(onKeyChanged) onKeyChanged(key(),!s.isMouseButtonDown()); };
    s.onDragEnd=[this]{ if(onKeyChanged) onKeyChanged(key(),true); };
    addAndMakeVisible(s);
}

studio::KeyParams KeyPanel::key() const { return {float(threshold.getValue()),float(softness.getValue())}; }
void KeyPanel::setKey(studio::KeyParams k) { threshold.setValue(k.threshold,dontSendNotification); softness.setValue(k.softness,dontSendNotification); }
void KeyPanel::setEngineKey(studio::KeyParams k,bool active) {
    const auto text=active ? "In use: threshold "+String(k.threshold,3)+"  softness "+String(k.softness,3) : String("Applied when the studio starts");
    if(text!=engineKey.getText()) engineKey.setText(text,dontSendNotification);
}

void KeyPanel::resized() {
    auto area=content();
    auto knobs=area.removeFromTop(jmin(200,area.getHeight()-90));
    auto left=knobs.removeFromLeft(knobs.getWidth()/2), right=knobs;
    thresholdLabel.setBounds(left.removeFromTop(18)); threshold.setBounds(left.reduced(6,0));
    softnessLabel.setBounds(right.removeFromTop(18)); softness.setBounds(right.reduced(6,0));
    area.removeFromTop(6);
    engineKey.setBounds(area.removeFromTop(22));
    auto bottom=area.removeFromBottom(36); reset.setBounds(bottom.removeFromRight(110));
    note.setBounds(area);
}

//==============================================================================
DiagnosticsPanel::DiagnosticsPanel():RustPanel("Diagnostics") {
    for(auto* t:tiles()) addAndMakeVisible(*t);
    for(auto* a:ages()) addAndMakeVisible(*a);
    styleLabel(source,15,palette::text,true); addAndMakeVisible(source);
    styleLabel(limits,12,palette::dimText);
    limits.setText("Limits: progressive 1080p/720p only - 8-bit UYVY BT.709 limited - no audio - timed tracking needs 50 fps - "
                   "3 completed host surfaces, 4 AJA slots per direction. Ages are measured to SDI DMA submission: not glass-to-glass latency, not exposure-aligned. "
                   "Blender/pose older than 500 ms selects camera passthrough.",dontSendNotification);
    limits.setJustificationType(Justification::topLeft);
    addAndMakeVisible(limits);
}

void DiagnosticsPanel::update(const studio::EngineSnapshot& snap,bool newSample) {
    if(!snap.hasStats) return;
    const auto& s=snap.stats;
    captured.setCounter(s.captured,newSample); submitted.setCounter(s.submitted,newSample); repeats.setCounter(s.repeats,newSample);
    fallbacks.setCounter(s.fallbacks,newSample); poolFull.setCounter(s.poolFull,newSample);
    inputDrops.setCounter(s.inputDrops,newSample); outputDrops.setCounter(s.outputDrops,newSample);
    captureFail.setCounter(s.captureFailures-s.injectedFailures,newSample); outputFail.setCounter(s.outputFailures,newSample);
    blenderTicks.setCounter(s.blenderRepeats+s.blenderSkipped,newSample); blenderTicks.setText(String(int64(s.blenderRepeats))+" / "+String(int64(s.blenderSkipped)));
    inputLevel.setText(String(int(s.inputLevel))+" / 4"); outputLevel.setText(String(int(s.outputLevel))+" / 4");
    cameraAge.set(s.cameraReadyAgeMs,newSample); blenderAge.set(s.blenderReceiveAgeMs,newSample); renderAge.set(s.blenderRenderAgeMs,newSample);
    trackingAge.set(s.trackingReceiveAgeMs,newSample); poseAge.set(s.poseTargetAgeMs,newSample); compositeTime.set(s.processingMs,newSample);
    const String status=s.sourceStatus;
    if(status!=source.getText()) { source.setText(status,dontSendNotification); source.setColour(Label::textColourId,status.contains("ready") ? palette::text : palette::warn); }
}

void DiagnosticsPanel::reset() {
    for(auto* t:tiles()) t->reset();
    for(auto* a:ages()) a->reset();
    source.setText({},dontSendNotification);
}

void DiagnosticsPanel::resized() {
    auto area=content();
    const int columns=area.getWidth()>900 ? 6 : 4;
    const auto list=tiles(); const int rowsNeeded=int((list.size()+size_t(columns)-1)/size_t(columns));
    for(int r=0;r<rowsNeeded;++r) {
        auto row=area.removeFromTop(66); const int w=row.getWidth()/columns;
        for(int c=0;c<columns;++c) { const size_t i=size_t(r*columns+c); if(i<list.size()) list[i]->setBounds(row.removeFromLeft(w).reduced(3)); }
    }
    area.removeFromTop(10);
    for(auto* a:ages()) { a->setBounds(area.removeFromTop(36)); area.removeFromTop(2); }
    area.removeFromTop(6);
    source.setBounds(area.removeFromTop(24));
    limits.setBounds(area);
}

//==============================================================================
EventLogPanel::EventLogPanel():RustPanel("Events & faults") {
    list.setRowHeight(22); list.setColour(ListBox::backgroundColourId,palette::well);
    addAndMakeVisible(list);
    styleLabel(csv,13,palette::dimText); csv.setText("CSV: (no run yet)",dontSendNotification); addAndMakeVisible(csv);
    openLogs.onClick=[this]{ if(onOpenLogs) onOpenLogs(); }; addAndMakeVisible(openLogs);
}

void EventLogPanel::add(const studio::EngineEvent& e) {
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(e.time.time_since_epoch()).count();
    append({Time(int64(ms)).formatted("%H:%M:%S"),String(e.text),e.level==studio::EventLevel::Fault ? Severity::Fault : Severity::Neutral,false});
}
void EventLogPanel::addLocal(Severity severity,const String& text) { append({Time::getCurrentTime().formatted("%H:%M:%S"),text,severity,true}); }

void EventLogPanel::append(Row row) {
    const bool follow=rows.empty() || list.getViewport()->getViewPositionY()+list.getViewport()->getViewHeight()>=list.getRowHeight()*int(rows.size())-2;
    rows.push_back(std::move(row)); if(rows.size()>1000) rows.pop_front();
    list.updateContent();
    if(follow) list.scrollToEnsureRowIsOnscreen(int(rows.size())-1);
    list.repaint();
}

void EventLogPanel::setCsvPath(const String& path) {
    const auto text="CSV: "+(path.isEmpty() ? String("(no run yet)") : path);
    if(text!=csv.getText()) csv.setText(text,dontSendNotification);
}

void EventLogPanel::paintListBoxItem(int index,Graphics& g,int width,int height,bool selected) {
    if(index<0 || size_t(index)>=rows.size()) return;
    const auto& row=rows[size_t(index)];
    if(selected) g.fillAll(palette::pinkDeep.withAlpha(.35f));
    else if(row.severity==Severity::Fault) g.fillAll(palette::fault.withAlpha(.1f));
    g.setFont(RustLookAndFeel::font(14));
    g.setColour(palette::dimText); g.drawText(row.time,6,0,70,height,Justification::centredLeft);
    const auto tag=row.severity==Severity::Fault ? "FAULT" : row.severity==Severity::Warn ? "WARN" : row.local ? "OPERATOR" : "ENGINE";
    g.setColour(row.severity==Severity::Neutral ? (row.local ? palette::pinkDeep.brighter(.4f) : palette::dimText) : severityColour(row.severity));
    g.setFont(RustLookAndFeel::font(12,true)); g.drawText(tag,78,0,74,height,Justification::centredLeft);
    g.setColour(row.severity==Severity::Fault ? palette::fault.brighter(.3f) : palette::text); g.setFont(RustLookAndFeel::font(14));
    g.drawText(row.text,156,0,width-160,height,Justification::centredLeft,true);
}

void EventLogPanel::resized() {
    auto area=content();
    auto bottom=area.removeFromBottom(34); openLogs.setBounds(bottom.removeFromRight(110)); csv.setBounds(bottom.withTrimmedRight(8));
    area.removeFromBottom(8);
    list.setBounds(area);
}
