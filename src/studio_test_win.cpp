#include <windows.h>
#include "studio_engine.hpp"
#include "studio_gpu.hpp"
#include "ajabase/system/debug.h"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

// Thin clients of studio::Engine: a timed CLI run and the Win32 test UI.
// The UI thread only calls Engine methods and polls snapshot() on a timer.
namespace {
studio::Engine engine;
std::string notice; // UI input errors; shown until the next successful action
HWND statusLabel,inputEdit,outputEdit,senderEdit,thresholdEdit,softnessEdit,startButton,stopButton,applyButton;
std::string status(const studio::EngineSnapshot& snap) {
    using studio::EngineState;
    if(snap.state==EngineState::Idle) return "Ready: SDI 1 camera -> basic green key + Blender -> SDI 4 television.";
    if(snap.state==EngineState::Failed || !snap.message.empty()) return snap.message;
    if(!snap.hasStats) return "Initializing studio video path...";
    const auto& stats=snap.stats; std::ostringstream text;
    text << "Camera + Blender studio composite | " << std::fixed << std::setprecision(1) << snap.elapsedSeconds << " s"
         << " | key " << std::setprecision(3) << snap.key.threshold << " / " << snap.key.softness << std::setprecision(1) << "\r\n"
         << "Capture " << stats.captured << " | SDI submitted " << stats.submitted << " | repeated outputs " << stats.repeats << "\r\n"
         << "Blender repeated/skipped ticks " << stats.blenderRepeats << " / " << stats.blenderSkipped << " | pose target age " << stats.poseTargetAgeMs << " ms\r\n"
         << "AJA input/output drops " << stats.inputDrops << " / " << stats.outputDrops << " | failed capture/output DMA " << stats.captureFailures << " / " << stats.outputFailures << "\r\n"
         << "Camera fallback outputs " << stats.fallbacks << " | completed host pool: 3 | pool full " << stats.poolFull << "\r\n\r\n"
         << "Camera ready age " << stats.cameraReadyAgeMs << " ms | Blender receive age " << stats.blenderReceiveAgeMs << " ms\r\n"
         << "Composite API " << stats.processingMs << " ms | Blender generation/sequence " << stats.blenderGeneration << '/' << stats.blenderSequence << "\r\n"
         << "Blender render age " << stats.blenderRenderAgeMs << " ms | tracking receive age " << stats.trackingReceiveAgeMs << " ms\r\n"
         << stats.sourceStatus << "\r\n\r\n"
         << "Age is measured from capture completion / Blender reception to SDI DMA submission.\r\nExposure age and tracking alignment are not yet calibrated.\r\n"
         << "Blender absent/stale (>500 ms): camera passthrough. No completed camera: repeat valid output.\r\n"
         << "CSV: " << snap.csvPath;
    return text.str();
}
std::string describe(const studio::EngineSnapshot& snap) { return notice.empty() ? status(snap) : notice+"\r\n"+status(snap); }
int runCli(const studio::EngineConfig& config) {
    try { engine.start(config); } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    const int result=engine.wait(); const auto snap=engine.snapshot(); const auto& stats=snap.stats;
    if(result) std::cerr << snap.message << '\n';
    std::cout << (result==0?"PASS":"FAIL") << " captured=" << stats.captured << " submitted=" << stats.submitted << " rejected=" << stats.captureFailures << " repeats=" << stats.repeats << " input_drops=" << stats.inputDrops << " output_drops=" << stats.outputDrops << " csv=" << snap.csvPath << '\n';
    return result;
}
std::string edit(HWND handle) { char buffer[256]{}; GetWindowTextA(handle,buffer,256); return buffer; }
HWND control(HWND parent,const char* cls,const char* text,int x,int y,int width,int height,int id=0) {
    auto handle=CreateWindowA(cls,text,WS_VISIBLE|WS_CHILD|(strcmp(cls,"EDIT")==0?WS_BORDER|ES_AUTOHSCROLL:0),x,y,width,height,parent,HMENU(INT_PTR(id)),nullptr,nullptr);
    SendMessage(handle,WM_SETFONT,WPARAM(GetStockObject(DEFAULT_GUI_FONT)),TRUE); return handle;
}
LRESULT CALLBACK proc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_CREATE) {
        control(window,"STATIC","Green-screen camera + Blender -> AJA SDI output",20,15,700,30);
        control(window,"STATIC","Input SDI",20,55,70,24); inputEdit=control(window,"EDIT","1",95,52,50,24);
        control(window,"STATIC","Output SDI",170,55,80,24); outputEdit=control(window,"EDIT","4",255,52,50,24);
        control(window,"STATIC","Blender sender",330,55,100,24); senderEdit=control(window,"EDIT","RealtimeStudio",435,52,230,24);
        control(window,"STATIC","Green threshold",20,95,100,24); thresholdEdit=control(window,"EDIT","0.12",125,92,60,24);
        control(window,"STATIC","Softness",220,95,75,24); softnessEdit=control(window,"EDIT","0.10",300,92,60,24);
        startButton=control(window,"BUTTON","Start studio",400,90,120,30,1); stopButton=control(window,"BUTTON","Stop",535,90,100,30,2);
        applyButton=control(window,"BUTTON","Apply key",650,90,100,30,3);
        statusLabel=control(window,"STATIC","",20,145,730,340); SetTimer(window,1,250,nullptr); return 0;
    }
    if(message==WM_TIMER) {
        const auto snap=engine.snapshot(); const bool running=studio::isActive(snap.state);
        SetWindowTextA(statusLabel,describe(snap).c_str()); EnableWindow(startButton,!running); EnableWindow(stopButton,running); EnableWindow(applyButton,running);
        for(auto field:{inputEdit,outputEdit,senderEdit}) EnableWindow(field,!running); return 0;
    }
    if(message==WM_COMMAND && LOWORD(w)==1) {
        if(engine.active()) return 0;
        try {
            studio::EngineConfig config; config.input=std::stoul(edit(inputEdit)); config.output=std::stoul(edit(outputEdit)); config.sender=edit(senderEdit);
            config.key={std::stof(edit(thresholdEdit)),std::stof(edit(softnessEdit))};
            engine.start(config); notice.clear();
        } catch(const std::exception& error) { notice=error.what(); } return 0;
    }
    if(message==WM_COMMAND && LOWORD(w)==2) { engine.stop(); return 0; }
    if(message==WM_COMMAND && LOWORD(w)==3) {
        bool applied=false;
        try { applied=engine.setKeyParams(std::stof(edit(thresholdEdit)),std::stof(edit(softnessEdit))); } catch(const std::exception&) {}
        if(applied) notice.clear(); else notice="Key not applied: threshold must be 0..1; softness >0..1 (still running with previous key)"; return 0;
    }
    if(message==WM_CLOSE) { engine.stop(); engine.wait(); DestroyWindow(window); return 0; }
    if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(window,message,w,l);
}
}
int main(int argc,char** argv) {
    try {
        AJADebug::Open(); studio::EngineConfig config;
        bool autoStart=false;
        if(argc==2 && std::string(argv[1])=="--gpu-selftest") { int result=studio::gpuSelfTest(); std::cout << "PASS: GPU green key, live key change, BT.709, immutable held pixels, new-frame delivery and stale-source fallback\n"; return result; }
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i]; if(arg=="--autostart") { autoStart=true; continue; }
            if(i+1==argc) throw std::runtime_error("Missing argument"); std::string value=argv[++i];
            if(arg=="--seconds") config.seconds=std::stoul(value);
            else if(arg=="--device") config.device=value;
            else if(arg=="--input") config.input=std::stoul(value);
            else if(arg=="--output") config.output=std::stoul(value);
            else if(arg=="--sender") config.sender=value;
            else if(arg=="--threshold") config.key.threshold=std::stof(value);
            else if(arg=="--softness") config.key.softness=std::stof(value);
            else if(arg=="--fault-every") config.faultEvery=std::stoul(value);
            else throw std::runtime_error("Unknown argument "+arg);
        }
        if(argc>1 && !autoStart) { if(!config.seconds) throw std::runtime_error("CLI run requires --seconds"); return runCli(config); }
        FreeConsole(); WNDCLASSA cls{}; cls.lpfnWndProc=proc; cls.hInstance=GetModuleHandle(nullptr); cls.lpszClassName="RealtimeStudioTest";
        cls.hCursor=LoadCursor(nullptr,IDC_ARROW); cls.hbrBackground=HBRUSH(COLOR_BTNFACE+1); RegisterClassA(&cls);
        auto window=CreateWindowA(cls.lpszClassName,"Realtime Studio Composite",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,800,555,nullptr,nullptr,cls.hInstance,nullptr);
        ShowWindow(window,SW_SHOW); if(autoStart) PostMessage(window,WM_COMMAND,1,0);
        MSG message{}; while(GetMessage(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessage(&message); } return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
