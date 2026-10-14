#include <windows.h>
#include "studio_runtime.hpp"
#include "ntv2devicescanner.h"
#include "sdi_profile.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
struct Options { std::string device="0",sender="RealtimeStudio"; unsigned input=1,output=4,seconds=0,faultEvery=0; float threshold=.12f,softness=.10f; };
std::atomic<bool> running{false},stop{false}; std::thread testThread;
std::mutex viewMutex; std::string view="Ready: SDI 1 camera -> basic green key + Blender -> SDI 4 television.";
HWND statusLabel,inputEdit,outputEdit,senderEdit,thresholdEdit,softnessEdit,startButton,stopButton;
void setView(std::string text) { std::lock_guard lock(viewMutex); view=std::move(text); }
bool exited(int32_t pid) {
    HANDLE handle=OpenProcess(SYNCHRONIZE,FALSE,DWORD(pid));
    if(!handle) return GetLastError()==ERROR_INVALID_PARAMETER;
    bool dead=WaitForSingleObject(handle,0)==WAIT_OBJECT_0; CloseHandle(handle); return dead;
}
int run(const Options& options) {
    running=true; int result=1;
    try {
        CNTV2Card card;
        if(!CNTV2DeviceScanner::GetFirstDeviceFromArgument(options.device,card)) throw std::runtime_error("AJA card not found");
        ULWord signature=0; int32_t pid=0;
        if(!card.GetStreamingApplication(signature,pid)) throw std::runtime_error("Cannot read AJA owner");
        if(pid && pid!=int32_t(GetCurrentProcessId())) {
            if(!exited(pid)) throw std::runtime_error("AJA card is owned by PID "+std::to_string(pid)+". Stop its AJA session first.");
            if(!card.ReleaseStreamForApplication(signature,pid)) throw std::runtime_error("Cannot release exited AJA owner");
        }
        if(!options.input || !options.output || options.input==options.output || options.input>card.features().GetNumVideoChannels() || options.output>card.features().GetNumVideoChannels()) throw std::runtime_error("Choose two valid different SDI connectors");
        BurnConfig config(options.device); config.fInputChannel=NTV2Channel(options.input-1); config.fInputSource=NTV2ChannelToInputSource(config.fInputChannel);
        config.fOutputChannel=NTV2Channel(options.output-1); config.fOutputDest=NTV2ChannelToOutputDestination(config.fOutputChannel);
        config.fPixelFormat=NTV2_FBF_8BIT_YCBCR; config.fSuppressAudio=true; config.fWithAnc=false; config.fTimecodeSource=NTV2_TCINDEX_INVALID;
        config.fInputFrames=NTV2ACFrameRange(4); config.fOutputFrames=NTV2ACFrameRange(4);
        sdi_profile::reset();
        studio::Runtime runtime(config,options.sender,options.threshold,options.softness,options.faultEvery);
        setView("Initializing studio video path...");
        if(AJA_FAILURE(runtime.Init())) throw std::runtime_error("AJA initialization failed. Check camera signal and card ownership.");
        const auto format=card.GetInputVideoFormat(config.fInputSource);
        if(AJA_FAILURE(runtime.Run())) throw std::runtime_error("Studio runtime supports progressive 1080p/720p BT.709 video only");
        std::filesystem::create_directories("output");
        auto prefix="output/studio-"+std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        std::ofstream csv(prefix+".csv"); if(!csv) throw std::runtime_error("Cannot open studio CSV");
        csv << "seconds,captured,capture_failures,injected_failures,submitted,output_failures,repeats,camera_fallbacks,pool_full,input_drops,output_drops,input_level,output_level,camera_sequence,blender_generation,blender_sequence,camera_ready_age_ms,blender_receive_age_ms,composite_api_ms,blender_render_age_ms,tracking_receive_age_ms,blender_session,blender_repeats,blender_skipped,pose_target_age_ms\n";
        const auto start=std::chrono::steady_clock::now(); bool complete=false,error=false;
        studio::RuntimeStats stats;
        while(!stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            stats=runtime.snapshot(); const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            csv << std::fixed << std::setprecision(3) << elapsed << ',' << stats.captured << ',' << stats.captureFailures << ',' << stats.injectedFailures << ',' << stats.submitted << ',' << stats.outputFailures
                << ',' << stats.repeats << ',' << stats.fallbacks << ',' << stats.poolFull << ',' << stats.inputDrops << ',' << stats.outputDrops << ',' << stats.inputLevel << ',' << stats.outputLevel
                << ',' << stats.cameraSequence << ',' << stats.blenderGeneration << ',' << stats.blenderSequence << ',' << stats.cameraReadyAgeMs << ',' << stats.blenderReceiveAgeMs << ',' << stats.processingMs
                << ',' << stats.blenderRenderAgeMs << ',' << stats.trackingReceiveAgeMs << ',' << stats.blenderSession
                << ',' << stats.blenderRepeats << ',' << stats.blenderSkipped << ',' << stats.poseTargetAgeMs << '\n';
            csv.flush();
            std::ostringstream text;
            text << "Camera + Blender studio composite | " << std::fixed << std::setprecision(1) << elapsed << " s\r\n"
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
                 << "CSV: " << prefix << ".csv";
            setView(text.str());
            if(!csv) { error=true; setView("Studio logging failed"); break; }
            if(!stats.fault.empty()) { error=true; setView(stats.fault); break; }
            if(card.GetInputVideoFormat(config.fInputSource)!=format) { error=true; setView("Camera signal lost or format changed"); break; }
            if(elapsed>=5 && (!stats.captured || !stats.submitted)) { error=true; setView("Studio video failed to start within five seconds"); break; }
            if(options.seconds && elapsed>=options.seconds) { complete=true; break; }
        }
        runtime.Quit(); sdi_profile::stopping=true;
        if(!sdi_profile::dump(prefix)) { error=true; setView("Studio profiling save failed"); }
        result=(!error && (!options.seconds || complete) && stats.captured && stats.submitted) ? 0:1;
        if(options.seconds) std::cout << (result==0?"PASS":"FAIL") << " captured=" << stats.captured << " submitted=" << stats.submitted << " rejected=" << stats.captureFailures << " repeats=" << stats.repeats << " input_drops=" << stats.inputDrops << " output_drops=" << stats.outputDrops << " csv=" << prefix << ".csv\n";
    } catch(const std::exception& error) { setView(error.what()); std::cerr << error.what() << '\n'; }
    running=false; return result;
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
        statusLabel=control(window,"STATIC","",20,145,730,340); SetTimer(window,1,250,nullptr); return 0;
    }
    if(message==WM_TIMER) {
        std::lock_guard lock(viewMutex); SetWindowTextA(statusLabel,view.c_str()); EnableWindow(startButton,!running); EnableWindow(stopButton,running);
        for(auto field:{inputEdit,outputEdit,senderEdit,thresholdEdit,softnessEdit}) EnableWindow(field,!running); return 0;
    }
    if(message==WM_COMMAND && LOWORD(w)==1) {
        if(running) return 0; if(testThread.joinable()) testThread.join();
        try {
            Options options; options.input=std::stoul(edit(inputEdit)); options.output=std::stoul(edit(outputEdit)); options.sender=edit(senderEdit);
            options.threshold=std::stof(edit(thresholdEdit)); options.softness=std::stof(edit(softnessEdit));
            if(!(options.threshold>=0 && options.threshold<1 && options.softness>0 && options.softness<=1)) throw std::runtime_error("Threshold must be 0..1; softness >0..1");
            stop=false; running=true; testThread=std::thread([options]{run(options);});
        } catch(const std::exception& error) { setView(error.what()); } return 0;
    }
    if(message==WM_COMMAND && LOWORD(w)==2) { stop=true; return 0; }
    if(message==WM_CLOSE) { stop=true; if(testThread.joinable()) testThread.join(); DestroyWindow(window); return 0; }
    if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(window,message,w,l);
}
}
int main(int argc,char** argv) {
    try {
        AJADebug::Open(); Options options;
        bool autoStart=false;
        if(argc==2 && std::string(argv[1])=="--gpu-selftest") { int result=studio::gpuSelfTest(); std::cout << "PASS: GPU green key, BT.709, immutable held pixels, new-frame delivery and stale-source fallback\n"; return result; }
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i]; if(arg=="--autostart") { autoStart=true; continue; }
            if(i+1==argc) throw std::runtime_error("Missing argument"); std::string value=argv[++i];
            if(arg=="--seconds") options.seconds=std::stoul(value);
            else if(arg=="--device") options.device=value;
            else if(arg=="--input") options.input=std::stoul(value);
            else if(arg=="--output") options.output=std::stoul(value);
            else if(arg=="--sender") options.sender=value;
            else if(arg=="--threshold") options.threshold=std::stof(value);
            else if(arg=="--softness") options.softness=std::stof(value);
            else if(arg=="--fault-every") options.faultEvery=std::stoul(value);
            else throw std::runtime_error("Unknown argument "+arg);
        }
        if(argc>1 && !autoStart) { if(!options.seconds) throw std::runtime_error("CLI run requires --seconds"); return run(options); }
        FreeConsole(); WNDCLASSA cls{}; cls.lpfnWndProc=proc; cls.hInstance=GetModuleHandle(nullptr); cls.lpszClassName="RealtimeStudioTest";
        cls.hCursor=LoadCursor(nullptr,IDC_ARROW); cls.hbrBackground=HBRUSH(COLOR_BTNFACE+1); RegisterClassA(&cls);
        auto window=CreateWindowA(cls.lpszClassName,"Realtime Studio Composite",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,800,555,nullptr,nullptr,cls.hInstance,nullptr);
        ShowWindow(window,SW_SHOW); if(autoStart) PostMessage(window,WM_COMMAND,1,0);
        MSG message{}; while(GetMessage(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessage(&message); } return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
