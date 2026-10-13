#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include "ntv2burn.h"
#include "ntv2devicescanner.h"
#include "sdi_profile.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

using Clock = std::chrono::steady_clock;
static_assert(CIRCULAR_BUFFER_SIZE == 10, "Update the profiling host-slot tracker for a different SDK ring size");
struct Sample {
    std::string state = "Ready. Select camera input and television output, then Start.";
    std::string format, csv;
    double seconds = 0, captureFps = 0, outputFps = 0;
    ULWord captured = 0, played = 0, inputDrops = 0, outputDrops = 0, inputBuffer = 0, outputBuffer = 0;
    unsigned inputCapacity = 0, outputCapacity = 0;
    unsigned inputFrameStore = 0, outputFrameStore = 0;
    sdi_profile::Host host;
    unsigned hostPeak = 0;
    std::array<sdi_profile::View, size_t(sdi_profile::Kind::Count)> timings{};
};
std::mutex sampleMutex;
Sample sample;
std::atomic<bool> running{false}, stopRequested{false};
std::thread worker;
HWND deviceBox, inputBox, outputBox, inputBuffersBox, outputBuffersBox, startButton, stopButton, statusText, trackingText, profileText;
SOCKET trackingSocket = INVALID_SOCKET;
uint64_t packets = 0, bytes = 0;
Clock::time_point lastPacket;
HFONT uiFont;

// Access denied means unknown, not dead. Never release a possibly live owner.
bool definitelyExited(int32_t pid) {
    if (pid <= 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}

bool recoverOwner(CNTV2Card& card, std::string& message) {
    ULWord signature = 0; int32_t pid = 0;
    if (!card.GetStreamingApplication(signature, pid)) {
        message = "Cannot read device ownership."; return false;
    }
    if (!pid) { message = "Card has no process owner."; return true; }
    if (!definitelyExited(pid)) {
        message = "PID " + std::to_string(pid) + " is running or cannot be checked. Ownership unchanged.";
        return false;
    }
    // Release by matching the recorded PID, rather than forcibly replacing an owner.
    if (!card.ReleaseStreamForApplication(signature, pid)) {
        message = "Failed to release exited owner PID " + std::to_string(pid) + "."; return false;
    }
    int32_t remaining = 0;
    if (!card.GetStreamingApplication(signature, remaining) || remaining) {
        message = "Ownership changed or could not be cleared. Refresh devices."; return false;
    }
    message = "Released stale AJA ownership for exited PID " + std::to_string(pid) + ".";
    return true;
}

void state(const std::string& message) {
    std::lock_guard lock(sampleMutex);
    sample.state = message;
}

std::string probe() {
    std::ostringstream text;
    for (unsigned index = 0; index < 16; ++index) {
        CNTV2Card card;
        if (!CNTV2DeviceScanner::GetDeviceAtIndex(index, card)) continue;
        ULWord signature = 0; int32_t pid = 0;
        card.GetStreamingApplication(signature, pid);
        text << index << ": " << NTV2DeviceIDToString(card.GetDeviceID()) << " owner PID=" << pid << '\n';
        for (unsigned ch = 0; ch < card.features().GetNumVideoChannels(); ++ch) {
            bool transmit = false;
            card.GetSDITransmitEnable(NTV2Channel(ch), transmit);
            text << "  SDI " << ch + 1 << (transmit ? " output: " : " input: ")
                 << NTV2VideoFormatToString(card.GetInputVideoFormat(NTV2ChannelToInputSource(NTV2Channel(ch))), true) << '\n';
        }
    }
    return text.str();
}

void runTest(std::string device, unsigned input, unsigned output, unsigned duration = 0,
             unsigned inputBuffers = 4, unsigned outputBuffers = 4) {
    running = true;
    try {
        if (inputBuffers < 3 || inputBuffers > 7 || outputBuffers < 3 || outputBuffers > 7) {
            state("Device buffer counts must be between 3 and 7."); running = false; return;
        }
        CNTV2Card card;
        if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(device, card)) {
            state("Device unavailable. Refresh devices."); running = false; return;
        }
        ULWord signature = 0; int32_t pid = 0;
        if (!card.GetStreamingApplication(signature, pid)) {
            state("Cannot read device ownership. Test not started."); running = false; return;
        }
        if (pid && pid != int32_t(GetCurrentProcessId())) {
            if (!definitelyExited(pid)) {
                state("Device is owned by PID " + std::to_string(pid) + ". Stop its AJA session first."); running = false; return;
            }
            std::string recovery;
            if (!recoverOwner(card, recovery)) { state(recovery); running = false; return; }
        }
        if (input == output || input >= card.features().GetNumVideoChannels() || output >= card.features().GetNumVideoChannels()) {
            state("Choose two different connectors available on this card."); running = false; return;
        }
        // This first test uses the SDK's single-frame-store SD/HD capture/playout path.
        auto format = card.GetInputVideoFormat(NTV2ChannelToInputSource(NTV2Channel(input)));
        if (NTV2_IS_4K_VIDEO_FORMAT(format) || NTV2_IS_QUAD_QUAD_FORMAT(format)) {
            state("UHD/4K detected. This initial test supports SD/HD/2K only; set camera to HD."); running = false; return;
        }
        BurnConfig config(device);
        config.fInputFrames = NTV2ACFrameRange(inputBuffers);
        config.fOutputFrames = NTV2ACFrameRange(outputBuffers);
        config.fInputSource = NTV2ChannelToInputSource(NTV2Channel(input));
        config.fInputChannel = NTV2Channel(input);
        config.fOutputDest = NTV2ChannelToOutputDestination(NTV2Channel(output));
        config.fOutputChannel = NTV2Channel(output);
        config.fTimecodeSource = NTV2_TCINDEX_INVALID;
        config.fSuppressAudio = true;
        config.fWithAnc = false;
        config.fDoMultiFormat = false;
        sdi_profile::reset();
        NTV2Burn burn(config);
        state("Initializing SDI capture/playout...");
        AJAStatus result = burn.Init();
        if (AJA_FAILURE(result)) {
            state("AJA initialization failed (" + std::to_string(result) + "). Check signal and output/sdi-test.log.");
            running = false; return;
        }
        format = card.GetInputVideoFormat(config.fInputSource);
        if (NTV2_IS_4K_VIDEO_FORMAT(format) || NTV2_IS_QUAD_QUAD_FORMAT(format)) {
            state("UHD signal detected after input lock. Set camera to HD and restart."); running = false; return;
        }
        std::filesystem::create_directories("output");
        const auto id = std::chrono::system_clock::now().time_since_epoch().count();
        const auto path = "output/sdi-" + std::to_string(id) + ".csv";
        std::ofstream csv(path);
        if (!csv) { state("Unable to create CSV log; test not started."); running = false; return; }
        csv << "seconds,captured,played,capture_fps,output_fps,input_drops,output_drops,input_buffer,output_buffer,signal,input_capacity,output_capacity,host_capacity,host_capture,host_ready,host_output,host_occupied,host_peak,input_frame_store,output_frame_store,input_sdi,output_sdi";
        for (auto name : sdi_profile::names) csv << ',' << name << "_count," << name << "_avg_ms," << name << "_max_ms," << name << "_failures";
        csv << '\n';
        sdi_profile::started = Clock::now();
        result = burn.Run();
        if (AJA_FAILURE(result)) { state("AJA could not start capture/playout."); running = false; return; }
        const auto started = sdi_profile::started;
        auto previous = started;
        ULWord previousInput = 0, previousOutput = 0;
        while (!stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            AUTOCIRCULATE_STATUS in, out;
            burn.GetStatus(in, out);
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - previous).count();
            Sample next;
            next.seconds = std::chrono::duration<double>(now - started).count();
            next.captured = in.GetProcessedFrameCount(); next.played = out.GetProcessedFrameCount();
            next.inputDrops = in.GetDroppedFrameCount(); next.outputDrops = out.GetDroppedFrameCount();
            next.inputBuffer = in.GetBufferLevel(); next.outputBuffer = out.GetBufferLevel();
            next.inputCapacity = in.GetFrameCount(); next.outputCapacity = out.GetFrameCount();
            next.inputFrameStore = unsigned(in.GetChannel()) + 1; next.outputFrameStore = unsigned(out.GetChannel()) + 1;
            next.host = sdi_profile::host(); next.hostPeak = sdi_profile::hostPeak.load();
            for (size_t k = 0; k < next.timings.size(); ++k) next.timings[k] = sdi_profile::view(sdi_profile::Kind(k));
            next.captureFps = ULWord(next.captured - previousInput) / elapsed;
            next.outputFps = ULWord(next.played - previousOutput) / elapsed;
            auto liveFormat = card.GetInputVideoFormat(config.fInputSource);
            const bool signalOk = liveFormat == format;
            next.state = signalOk ? "Running: camera -> host memory -> SDI television (video only)." : "Input signal lost or format changed. Stop and restart after signal is stable.";
            next.format = NTV2VideoFormatToString(liveFormat, true); next.csv = path;
            csv << std::fixed << std::setprecision(3) << next.seconds << ',' << next.captured << ',' << next.played
                << ',' << next.captureFps << ',' << next.outputFps << ',' << next.inputDrops << ',' << next.outputDrops
                << ',' << next.inputBuffer << ',' << next.outputBuffer << ',' << signalOk
                << ',' << next.inputCapacity << ',' << next.outputCapacity << ',' << CIRCULAR_BUFFER_SIZE
                << ',' << next.host.capture << ',' << next.host.ready << ',' << next.host.output << ',' << next.host.occupied() << ',' << next.hostPeak
                << ',' << next.inputFrameStore << ',' << next.outputFrameStore << ',' << input+1 << ',' << output+1;
            for (const auto& metric : next.timings) csv << ',' << metric.count << ',' << metric.averageMs << ',' << metric.maxMs << ',' << metric.failures;
            csv << '\n';
            csv.flush();
            if (!csv) { next.state = "CSV write failed. Test stopped."; stopRequested = true; }
            { std::lock_guard lock(sampleMutex); sample = next; }
            previous = now; previousInput = next.captured; previousOutput = next.played;
            if (next.seconds >= 5 && (!next.captured || !next.played)) {
                state("Capture/playout did not start within 5 seconds. Check output/sdi-test.log."); break;
            }
            if (!signalOk || (duration && next.seconds >= duration)) break;
        }
        sdi_profile::stopping = true;
        burn.Quit(); // Join frame threads before saving the preallocated event traces.
        if (!sdi_profile::dump(path.substr(0,path.size()-4))) state("Stopped, but profiling logs could not be saved.");
    } catch (const std::exception& error) { state(std::string("Test failed: ") + error.what()); }
    { std::lock_guard lock(sampleMutex);
      if (sample.state.rfind("Running", 0) == 0 || sample.state == "Stopping...") sample.state = "Stopped. Final counters retained; CSV saved."; }
    running = false;
}

void refreshDevices() {
    SendMessageA(deviceBox, CB_RESETCONTENT, 0, 0);
    int selected = 0, itemIndex = 0;
    for (unsigned index = 0; index < 16; ++index) {
        CNTV2Card card;
        if (!CNTV2DeviceScanner::GetDeviceAtIndex(index, card)) continue;
        const auto name = NTV2DeviceIDToString(card.GetDeviceID());
        const auto label = std::to_string(index) + " - " + name;
        auto item = SendMessageA(deviceBox, CB_ADDSTRING, 0, LPARAM(label.c_str()));
        SendMessageA(deviceBox, CB_SETITEMDATA, item, index);
        if (name.find("Corvid44") != std::string::npos) selected = itemIndex;
        ++itemIndex;
    }
    SendMessageA(deviceBox, CB_SETCURSEL, selected, 0);
    auto chosen = SendMessage(deviceBox, CB_GETITEMDATA, selected, 0);
    CNTV2Card card;
    if (chosen != CB_ERR && CNTV2DeviceScanner::GetDeviceAtIndex(ULWord(chosen), card)) {
        ULWord signature = 0; int32_t pid = 0;
        card.GetStreamingApplication(signature, pid);
        std::lock_guard lock(sampleMutex);
        sample.format = NTV2VideoFormatToString(card.GetInputVideoFormat(NTV2_INPUTSOURCE_SDI1), true);
        if (pid) sample.state = "Selected card is owned by PID " + std::to_string(pid) + ". Stop that application's AJA session before Start.";
    }
}

HWND control(HWND parent, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    auto handle = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, x,y,w,h,parent,HMENU(INT_PTR(id)),GetModuleHandle(nullptr),nullptr);
    SendMessage(handle, WM_SETFONT, WPARAM(uiFont), TRUE);
    return handle;
}

void trackingUpdate() {
    if (trackingSocket == INVALID_SOCKET) return;
    char buffer[65536];
    for (int i = 0; i < 512; ++i) {
        int count = recv(trackingSocket, buffer, sizeof(buffer), 0);
        if (count == SOCKET_ERROR) break;
        ++packets; bytes += count; lastPacket = Clock::now();
    }
    std::ostringstream text;
    text << "UDP 8873: " << packets << " packets / " << bytes << " bytes";
    if (packets) text << " | last packet " << std::fixed << std::setprecision(1) << std::chrono::duration<double>(Clock::now()-lastPacket).count() << " s ago";
    else text << " | waiting for data";
    text << "\r\nPacket activity only; tracking protocol and packet loss are not decoded.";
    SetWindowTextA(trackingText, text.str().c_str());
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    switch(message) {
    case WM_CREATE: {
        uiFont = CreateFontA(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
        control(window,"STATIC","AJA SDI Input / Output Test",0,24,20,650,30);
        control(window,"STATIC","Device",0,24,65,100,24);
        deviceBox = control(window,"COMBOBOX","",CBS_DROPDOWNLIST | WS_VSCROLL,130,60,450,240);
        control(window,"BUTTON","Refresh",0,595,60,120,30,3);
        control(window,"STATIC","Camera input",0,24,110,110,24);
        inputBox = control(window,"COMBOBOX","",CBS_DROPDOWNLIST,140,105,135,250);
        control(window,"STATIC","TV output",0,300,110,100,24);
        outputBox = control(window,"COMBOBOX","",CBS_DROPDOWNLIST,410,105,135,250);
        for (int ch = 1; ch <= 8; ++ch) {
            auto label = "SDI " + std::to_string(ch);
            SendMessageA(inputBox,CB_ADDSTRING,0,LPARAM(label.c_str())); SendMessageA(outputBox,CB_ADDSTRING,0,LPARAM(label.c_str()));
        }
        control(window,"STATIC","Input buffers",0,24,155,110,24);
        inputBuffersBox = control(window,"COMBOBOX","",CBS_DROPDOWNLIST,140,150,135,200);
        control(window,"STATIC","Output buffers",0,300,155,110,24);
        outputBuffersBox = control(window,"COMBOBOX","",CBS_DROPDOWNLIST,410,150,135,200);
        for (unsigned count = 3; count <= 7; ++count) {
            auto label = std::to_string(count) + " frames";
            SendMessageA(inputBuffersBox,CB_ADDSTRING,0,LPARAM(label.c_str()));
            SendMessageA(outputBuffersBox,CB_ADDSTRING,0,LPARAM(label.c_str()));
        }
        unsigned savedInput = 0, savedOutput = 1, savedInputBuffers = 4, savedOutputBuffers = 4;
        std::ifstream settings("output/sdi-settings.txt");
        unsigned in = 0, out = 1;
        if (settings >> in >> out && in < 8 && out < 8 && in != out) { savedInput = in; savedOutput = out; }
        unsigned ib = 4, ob = 4;
        if (settings >> ib >> ob && ib >= 3 && ib <= 7 && ob >= 3 && ob <= 7) { savedInputBuffers = ib; savedOutputBuffers = ob; }
        SendMessage(inputBox,CB_SETCURSEL,savedInput,0); SendMessage(outputBox,CB_SETCURSEL,savedOutput,0);
        SendMessage(inputBuffersBox,CB_SETCURSEL,savedInputBuffers-3,0); SendMessage(outputBuffersBox,CB_SETCURSEL,savedOutputBuffers-3,0);
        startButton = control(window,"BUTTON","Start test",0,24,200,140,38,1);
        stopButton = control(window,"BUTTON","Stop",0,180,200,120,38,2);
        control(window,"BUTTON","Listen UDP 8873",0,325,200,170,38,4);
        control(window,"BUTTON","Open logs",0,515,200,140,38,5);
        statusText = control(window,"STATIC","",0,24,260,710,230);
        trackingText = control(window,"STATIC","Tracking: UDP 8873 listening is optional. Port may be occupied by nosLauncher.",0,24,505,710,65);
        control(window,"STATIC","SD/HD/2K | 8-bit YCbCr | Host ring: 10 reusable frames\r\nBuffer capacity is not measured end-to-end latency. Change counts while stopped.\r\nStart takes exclusive card control. Drops: AJA capture overruns / output underruns.",0,24,590,710,85);
        profileText = control(window,"STATIC","Profiling starts with the test.",0,770,65,445,570);
        refreshDevices(); SetTimer(window,1,250,nullptr); return 0;
    }
    case WM_TIMER: {
        Sample view; { std::lock_guard lock(sampleMutex); view = sample; }
        std::ostringstream text;
        text << view.state << "\r\n\r\nSignal: " << (view.format.empty() ? "not started" : view.format)
             << "    Elapsed: " << std::fixed << std::setprecision(1) << view.seconds << " s\r\n\r\n"
             << "CAPTURE    " << view.captured << " frames    " << view.captureFps << " FPS    Drops: " << view.inputDrops
             << "\r\nOUTPUT      " << view.played << " frames    " << view.outputFps << " FPS    Drops: " << view.outputDrops
             << "\r\n\r\nCard occupancy/capacity: input " << view.inputBuffer << '/' << view.inputCapacity
             << "   output " << view.outputBuffer << '/' << view.outputCapacity << "   | host capacity: 10"
             << "\r\nCSV: " << (view.csv.empty() ? "created on Start" : view.csv);
        SetWindowTextA(statusText,text.str().c_str());
        auto metricText = [&](const char* label, sdi_profile::Kind kind) {
            const auto& metric = view.timings[size_t(kind)];
            std::ostringstream line;
            line << label << "\r\n  " << std::fixed << std::setprecision(3) << metric.averageMs << " / " << metric.maxMs
                 << " ms   (" << metric.count << " calls, " << metric.failures << " failed)\r\n";
            return line.str();
        };
        std::ostringstream profiling;
        profiling << "TIMINGS: run average / maximum\r\n\r\n"
            << metricText("Capture DMA API",sdi_profile::Kind::CaptureDma)
            << metricText("Output DMA API",sdi_profile::Kind::OutputDma)
            << metricText("Capture: acquire host frame",sdi_profile::Kind::CaptureHostWait)
            << metricText("Output: acquire host frame",sdi_profile::Kind::OutputHostWait)
            << metricText("Capture: input device wait",sdi_profile::Kind::InputDeviceWait)
            << metricText("Output: device-space wait",sdi_profile::Kind::OutputDeviceWait)
            << metricText("Capture processing / timecode",sdi_profile::Kind::Processing)
            << "HOST RAM: " << view.host.occupied() << "/10 occupied, peak " << view.hostPeak
            << "\r\nCapture " << view.host.capture << " | ready " << view.host.ready << " | output " << view.host.output
            << "\r\nCard frame stores: input " << view.inputFrameStore << " / output " << view.outputFrameStore
            << "\r\n\r\nTrace + profile CSV saved after Stop.\r\nAPI elapsed times include driver / scheduling.\r\nDevice waits also include normal frame pacing.";
        SetWindowTextA(profileText,profiling.str().c_str());
        EnableWindow(startButton,!running); EnableWindow(stopButton,running);
        EnableWindow(deviceBox,!running); EnableWindow(inputBox,!running); EnableWindow(outputBox,!running);
        EnableWindow(inputBuffersBox,!running); EnableWindow(outputBuffersBox,!running);
        trackingUpdate(); return 0;
    }
    case WM_COMMAND:
        if (HIWORD(w) == CBN_SELCHANGE && (HWND(l) == deviceBox || HWND(l) == inputBox)) {
            auto selected = SendMessage(deviceBox,CB_GETCURSEL,0,0);
            auto index = SendMessage(deviceBox,CB_GETITEMDATA,selected,0);
            auto channel = SendMessage(inputBox,CB_GETCURSEL,0,0);
            CNTV2Card card;
            if (index != CB_ERR && CNTV2DeviceScanner::GetDeviceAtIndex(ULWord(index),card)) {
                ULWord signature = 0; int32_t pid = 0;
                card.GetStreamingApplication(signature,pid);
                std::lock_guard lock(sampleMutex);
                sample.format = NTV2VideoFormatToString(card.GetInputVideoFormat(NTV2ChannelToInputSource(NTV2Channel(channel))),true);
                sample.state = pid ? "Card is owned by PID " + std::to_string(pid) + ". Stop that application's AJA session first." : "Ready. Select the television output and click Start.";
            }
            return 0;
        }
        if (HIWORD(w) != BN_CLICKED) break;
        switch(LOWORD(w)) {
        case 1: {
            if (running) return 0;
            if (worker.joinable()) worker.join();
            const auto selected = SendMessage(deviceBox,CB_GETCURSEL,0,0);
            if (selected == CB_ERR) { state("No AJA devices found."); return 0; }
            auto device = SendMessage(deviceBox,CB_GETITEMDATA,selected,0);
            unsigned input = unsigned(SendMessage(inputBox,CB_GETCURSEL,0,0));
            unsigned output = unsigned(SendMessage(outputBox,CB_GETCURSEL,0,0));
            unsigned inputBuffers = unsigned(SendMessage(inputBuffersBox,CB_GETCURSEL,0,0)) + 3;
            unsigned outputBuffers = unsigned(SendMessage(outputBuffersBox,CB_GETCURSEL,0,0)) + 3;
            std::ofstream("output/sdi-settings.txt") << input << ' ' << output << ' ' << inputBuffers << ' ' << outputBuffers << '\n';
            { std::lock_guard lock(sampleMutex); sample = Sample{}; sample.state = "Starting..."; }
            stopRequested = false; running = true;
            worker = std::thread(runTest,std::to_string(device),input,output,0,inputBuffers,outputBuffers); return 0;
        }
        case 2: stopRequested = true; state("Stopping..."); return 0;
        case 3: if (!running) refreshDevices(); return 0;
        case 4: {
            if (trackingSocket != INVALID_SOCKET) { closesocket(trackingSocket); trackingSocket = INVALID_SOCKET; SetWindowTextA(trackingText,"UDP listener stopped."); return 0; }
            trackingSocket = socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
            BOOL exclusive = TRUE;
            setsockopt(trackingSocket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(8873); address.sin_addr.s_addr = INADDR_ANY;
            if (bind(trackingSocket,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == SOCKET_ERROR) {
                auto error = WSAGetLastError(); closesocket(trackingSocket); trackingSocket = INVALID_SOCKET;
                const auto msg = "Cannot listen on UDP 8873 (" + std::to_string(error) + "). nosLauncher currently uses this port; stop its tracking receiver or forward data to a free port.";
                SetWindowTextA(trackingText,msg.c_str()); return 0;
            }
            u_long nonblocking = 1; ioctlsocket(trackingSocket,FIONBIO,&nonblocking);
            packets = bytes = 0; return 0;
        }
        case 5: {
            std::filesystem::create_directories("output");
            auto path = std::filesystem::absolute("output").string();
            ShellExecuteA(window,"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL); return 0;
        }
        } break;
    case WM_CLOSE:
        stopRequested = true;
        if (worker.joinable()) worker.join();
        if (trackingSocket != INVALID_SOCKET) closesocket(trackingSocket);
        DestroyWindow(window); return 0;
    case WM_DESTROY: DeleteObject(uiFont); PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(window,message,w,l);
}

int main(int argc, char** argv) {
    AJADebug::Open();
    if (argc > 1 && std::string(argv[1]) == "--probe") { std::cout << probe(); return 0; }
    if (argc == 3 && std::string(argv[1]) == "--recover-owner") {
        CNTV2Card card;
        if (!CNTV2DeviceScanner::GetFirstDeviceFromArgument(argv[2],card)) { std::cerr << "Device unavailable.\n"; return 1; }
        std::string message;
        bool recovered = recoverOwner(card,message);
        std::cout << message << '\n'; return recovered ? 0 : 1;
    }
    std::filesystem::create_directories("output");
    std::ofstream diagnostics("output/sdi-test.log",std::ios::app);
    auto oldError = std::cerr.rdbuf(diagnostics.rdbuf());
    WSADATA data{}; WSAStartup(MAKEWORD(2,2),&data);
    if ((argc == 6 || argc == 8) && std::string(argv[1]) == "--test") {
        unsigned inputBuffers = argc == 8 ? unsigned(std::stoul(argv[6])) : 4;
        unsigned outputBuffers = argc == 8 ? unsigned(std::stoul(argv[7])) : 4;
        stopRequested = false; runTest(argv[2],unsigned(std::stoul(argv[3])-1),unsigned(std::stoul(argv[4])-1),unsigned(std::stoul(argv[5])),inputBuffers,outputBuffers);
        std::cout << sample.state << "\nCaptured=" << sample.captured << " Played=" << sample.played << " InputDrops=" << sample.inputDrops << " OutputDrops=" << sample.outputDrops << " HostPeak=" << sample.hostPeak << " CSV=" << sample.csv << '\n';
        std::cerr.rdbuf(oldError); WSACleanup(); return sample.captured && sample.played ? 0 : 1;
    }
    FreeConsole();
    WNDCLASSA cls{}; cls.lpfnWndProc = windowProc; cls.hInstance = GetModuleHandle(nullptr);
    cls.lpszClassName = "CompositorSDITest"; cls.hbrBackground = HBRUSH(COLOR_WINDOW+1); cls.hCursor = LoadCursor(nullptr,IDC_ARROW);
    RegisterClassA(&cls);
    auto window = CreateWindowA(cls.lpszClassName,"Realtime Compositor - AJA SDI Test",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,1260,725,nullptr,nullptr,cls.hInstance,nullptr);
    ShowWindow(window,SW_SHOW);
    MSG message; while(GetMessage(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessage(&message); }
    std::cerr.rdbuf(oldError); WSACleanup(); return 0;
}
