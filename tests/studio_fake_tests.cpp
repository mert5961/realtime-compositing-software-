#include "studio_control.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace studio;
void require(bool valid,const char* message) { if(!valid) throw std::runtime_error(message); }
template<class Predicate> bool eventually(Predicate predicate,double seconds=3) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
    while(std::chrono::steady_clock::now()<until) { if(predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return predicate();
}
bool hasEvent(const std::vector<EngineEvent>& events,EventLevel level,const std::string& text) {
    for(const auto& e:events) if(e.level==level && e.text.find(text)!=std::string::npos) return true;
    return false;
}
int main() {
    try {
        FakeOptions fast; fast.startupSeconds=.1; fast.stallEvery=0; fast.sampleMs=40;
        auto control=makeFakeControl(fast);
        require(control->simulated(),"Fake engine must identify itself as simulated");
        require(control->snapshot().state==EngineState::Idle && !control->active(),"New engine not idle");
        const auto devices=control->enumerateDevices();
        require(devices.size()==1 && devices[0].videoChannels==4,"Simulated device list wrong");

        EngineConfig bad; bad.key={1.5f,.1f};
        bool rejected=false; try { control->start(bad); } catch(const std::invalid_argument&) { rejected=true; }
        require(rejected && !control->active(),"Invalid key started a run");

        EngineConfig config;
        control->start(config);
        require(control->active(),"Start did not activate synchronously");
        bool refused=false; try { control->start(config); } catch(const std::logic_error&) { refused=true; }
        require(refused,"Second start while active was accepted");
        require(eventually([&]{ auto s=control->snapshot(); return s.state==EngineState::Running && s.hasStats && s.stats.captured>0; }),"Never reached Running with frames");
        require(control->setKeyParams(.3f,.2f) && control->snapshot().key.threshold==.3f,"Live key not applied");
        require(!control->setKeyParams(.3f,0) && control->snapshot().key.softness==.2f,"Invalid live key changed the key");
        control->stop();
        require(eventually([&]{ return !control->active(); }),"Stop did not finish");
        auto snap=control->snapshot();
        require(snap.state==EngineState::Stopped && snap.result==0 && snap.message.empty(),"Clean stop did not pass");
        auto events=control->drainEvents();
        require(hasEvent(events,EventLevel::Info,"Start:") && hasEvent(events,EventLevel::Info,"PASS"),"Start/PASS events missing");
        require(control->drainEvents().empty(),"Events not drained");

        EngineConfig sameConnector; sameConnector.input=sameConnector.output=2;
        control->start(sameConnector);
        require(eventually([&]{ return !control->active(); }),"Invalid routing run did not end");
        snap=control->snapshot();
        require(snap.state==EngineState::Failed && snap.result==1 && snap.message=="Choose two valid different SDI connectors","Invalid routing not reported like the real engine");
        require(hasEvent(control->drainEvents(),EventLevel::Fault,"SDI connectors"),"Routing fault event missing");

        FakeOptions faulty=fast; faulty.failAfter=.3;
        auto failing=makeFakeControl(faulty);
        failing->start(config);
        require(eventually([&]{ return !failing->active(); }),"Simulated fault did not stop the run");
        snap=failing->snapshot();
        require(snap.state==EngineState::Failed && snap.message.find("Output DMA failed")!=std::string::npos,"Simulated fault reason missing");
        require(hasEvent(failing->drainEvents(),EventLevel::Fault,"Output DMA failed"),"Fault event missing");
        std::cout << "PASS: simulated engine state machine, validation, live key, events, fault stop\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
