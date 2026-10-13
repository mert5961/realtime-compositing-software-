#include <SpoutDX.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
double seconds(Clock::duration value) { return std::chrono::duration<double>(value).count(); }
void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream message;
        message << operation << " failed (0x" << std::hex << static_cast<unsigned long>(result) << ')';
        throw std::runtime_error(message.str());
    }
}

struct Options {
    std::string sender = "RealtimeBlender";
    std::filesystem::path log, snapshot;
    double duration = 0;
    bool headless = false;
};

// D3D resources stay on a single thread. The SDK copies under its interprocess
// lock into its local texture. Preview sampling follows on the same GPU context.
class Preview {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11ShaderResourceView> image;
    HWND window = nullptr;
    std::string adapterName;
    UINT width = 0, height = 0;

    void initialize(HWND hwnd) {
        window = hwnd;
        ComPtr<IDXGIFactory1> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
        ComPtr<IDXGIAdapter1> adapter, chosen;
        SIZE_T largest = 0;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            check(adapter->GetDesc1(&desc), "Read adapter");
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (!chosen || desc.DedicatedVideoMemory > largest)) {
                chosen = adapter; largest = desc.DedicatedVideoMemory;
                char name[256]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 256, nullptr, nullptr);
                adapterName = name;
            }
            adapter.Reset();
        }
        if (!chosen) throw std::runtime_error("No hardware GPU found");
        check(D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context), "Create GPU device");
        if (!window) return;
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        check(factory->CreateSwapChain(device.Get(), &desc, &swap), "Create preview");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        const char* shader = R"(
            struct V { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
            V vs(uint id:SV_VertexID) {
                V v; v.uv=float2((id<<1)&2,id&2);
                v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v;
            }
            Texture2D frame:register(t0); SamplerState sampling:register(s0);
            float4 ps(V v):SV_TARGET {
                float4 c=frame.Sample(sampling,v.uv);
                float checker=fmod(floor(v.position.x/20)+floor(v.position.y/20),2);
                return float4(lerp(lerp(0.12,0.22,checker).xxx,c.rgb,c.a),1);
            }
        )";
        ComPtr<ID3DBlob> vs, ps, errors;
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs, &errors), "Compile vertex shader");
        check(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps, &errors), "Compile pixel shader");
        check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertex), "Create vertex shader");
        check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixel), "Create pixel shader");
        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, &sampler), "Create sampler");
    }

    void setTexture(ID3D11Texture2D* texture) {
        image.Reset();
        if (texture && window) check(device->CreateShaderResourceView(texture, nullptr, &image), "Open received texture for preview");
    }
    void render(UINT sourceWidth, UINT sourceHeight) {
        if (!window) return;
        RECT rect{}; GetClientRect(window, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return;
        if (!target || width != UINT(rect.right) || height != UINT(rect.bottom)) {
            context->OMSetRenderTargets(0, nullptr, nullptr); target.Reset();
            width = rect.right; height = rect.bottom;
            check(swap->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "Resize preview");
            ComPtr<ID3D11Texture2D> buffer;
            check(swap->GetBuffer(0, IID_PPV_ARGS(&buffer)), "Get preview buffer");
            check(device->CreateRenderTargetView(buffer.Get(), nullptr, &target), "Create preview target");
        }
        const float background[] = {0.025f, 0.03f, 0.04f, 1};
        context->ClearRenderTargetView(target.Get(), background);
        if (image && sourceWidth && sourceHeight) {
            const float scale = std::min(float(width)/sourceWidth, float(height)/sourceHeight);
            const float w = sourceWidth*scale, h = sourceHeight*scale;
            D3D11_VIEWPORT viewport{(width-w)/2, (height-h)/2, w, h, 0, 1};
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertex.Get(), nullptr, 0); context->PSSetShader(pixel.Get(), nullptr, 0);
            context->PSSetSamplers(0, 1, sampler.GetAddressOf());
            context->PSSetShaderResources(0, 1, image.GetAddressOf());
            context->Draw(3, 0);
            ID3D11ShaderResourceView* empty = nullptr;
            context->PSSetShaderResources(0, 1, &empty);
        }
        check(swap->Present(0, 0), "Present preview");
    }

    // Explicit diagnostic only. Normal receive/preview never maps pixels to RAM.
    void saveSnapshot(ID3D11Texture2D* texture, const std::filesystem::path& path) {
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!bgra && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            throw std::runtime_error("Snapshot requires RGBA8 or BGRA8");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc, nullptr, &staging), "Create snapshot staging texture");
        context->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read diagnostic snapshot");
        std::ofstream file(path, std::ios::binary);
        file << "P6\n" << desc.Width << ' ' << desc.Height << "\n255\n";
        for (UINT y=0; y<desc.Height; ++y) {
            auto row = static_cast<const unsigned char*>(mapped.pData) + y*mapped.RowPitch;
            for (UINT x=0; x<desc.Width; ++x) {
                const char rgb[] = {char(row[x*4+(bgra?2:0)]), char(row[x*4+1]), char(row[x*4+(bgra?0:2)])};
                file.write(rgb, 3);
            }
        }
        context->Unmap(staging.Get(), 0);
        file.flush();
        if (!file) throw std::runtime_error("Could not save diagnostic snapshot");
    }
};

HWND senderEdit = nullptr, statusLabel = nullptr, previewWindow = nullptr;
bool reconnect = false;
LRESULT CALLBACK windowProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == 1) { reconnect = true; return 0; }
    if (msg == WM_SIZE && previewWindow) {
        MoveWindow(previewWindow, 10, 130, std::max(1, int(LOWORD(lp))-20), std::max(1,int(HIWORD(lp))-140), TRUE);
        MoveWindow(statusLabel, 12, 48, std::max(1,int(LOWORD(lp))-24), 78, TRUE);
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(window, msg, wp, lp);
}

int run(const Options& options) {
    HWND window = nullptr;
    if (!options.headless) {
        WNDCLASSA wc{}; wc.lpfnWndProc = windowProc; wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "RealtimeBlenderTextureTest"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); RegisterClassA(&wc);
        window = CreateWindowA(wc.lpszClassName, "Blender Texture Test - Spout GPU receiver", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1120, 820, nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) throw std::runtime_error("Could not create test window");
        CreateWindowA("STATIC", "Sender", WS_CHILD|WS_VISIBLE, 12, 16, 55, 22, window, nullptr, nullptr, nullptr);
        senderEdit = CreateWindowA("EDIT", options.sender.c_str(), WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL,
            70, 12, 300, 26, window, nullptr, nullptr, nullptr);
        SendMessage(senderEdit, EM_SETLIMITTEXT, 255, 0);
        CreateWindowA("BUTTON", "Connect / reset", WS_CHILD|WS_VISIBLE, 382, 12, 145, 27,
            window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
        statusLabel = CreateWindowA("STATIC", "Waiting for Blender...", WS_CHILD|WS_VISIBLE,
            12, 48, 1060, 78, window, nullptr, nullptr, nullptr);
        previewWindow = CreateWindowA("STATIC", "", WS_CHILD|WS_VISIBLE, 10, 130, 1080, 640, window, nullptr, nullptr, nullptr);
        ShowWindow(window, SW_SHOW); UpdateWindow(window);
    }
    Preview preview; preview.initialize(previewWindow);
    spoutDX receiver;
    if (!receiver.OpenDirectX11(preview.device.Get())) throw std::runtime_error("Cannot open Spout D3D11 receiver");
    receiver.SetReceiverName(options.sender.c_str());
    std::filesystem::create_directories("output");
    auto logPath = options.log;
    if (logPath.empty()) logPath = "output/blender-texture-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + ".csv";
    std::ofstream log(logPath);
    if (!log) throw std::runtime_error("Cannot open texture test CSV");
    log << "elapsed_s,connected,width,height,format,generation,new_frames,skipped_sender_frames,frame_counter_available,receive_fps,receive_api_avg_ms,receive_api_max_ms,last_new_frame_age_ms\n";
    const auto start = Clock::now();
    auto sampled = start, drawn = start, lastNew = start;
    unsigned long long frames=0, skipped=0, sampleFrames=0, calls=0, generation=0;
    double totalMs=0, maxMs=0;
    long previousFrame=0;
    bool connected=false, counted=false, quit=false, snapshotSaved=false;
    UINT sourceWidth=0, sourceHeight=0;
    timeBeginPeriod(1);
    struct TimerCleanup { ~TimerCleanup() { timeEndPeriod(1); } } timerCleanup;
    while (!quit) {
        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessage(&msg);
        }
        if (quit) break;
        if (reconnect) {
            char name[256]{}; GetWindowTextA(senderEdit, name, 256);
            preview.setTexture(nullptr); receiver.ReleaseReceiver(); receiver.SetReceiverName(name);
            previousFrame=0; frames=skipped=sampleFrames=0; connected=counted=false; reconnect=false;
        }
        const auto before = Clock::now();
        const bool ok = receiver.ReceiveTexture();
        const double ms = seconds(Clock::now()-before)*1000;
        ++calls; totalMs+=ms; maxMs=std::max(maxMs,ms);
        const bool updated = receiver.IsUpdated(); // Required to clear the SDK's update latch.
        if (updated) { preview.setTexture(nullptr); previousFrame=0; ++generation; }
        connected=ok && receiver.IsConnected();
        if (connected) {
            sourceWidth=receiver.GetSenderWidth(); sourceHeight=receiver.GetSenderHeight();
            auto* texture = receiver.GetSenderTexture();
            if (texture && (updated || (!preview.image && previewWindow))) preview.setTexture(texture);
            const long frame = receiver.GetSenderFrame();
            counted = receiver.IsFrameCountEnabled() && frame > 0;
            if (texture && counted && receiver.IsFrameNew() && frame != previousFrame) {
                if (counted && previousFrame > 0 && frame > previousFrame)
                    skipped += static_cast<unsigned long long>(frame-previousFrame-1);
                if (counted && previousFrame > 0 && frame < previousFrame) ++generation;
                previousFrame=frame; ++frames; lastNew=Clock::now();
                if (!snapshotSaved && !options.snapshot.empty() && frames >= 5) {
                    preview.saveSnapshot(texture,options.snapshot); snapshotSaved=true;
                }
            }
        } else {
            preview.setTexture(nullptr); previousFrame=0; counted=false; sourceWidth=sourceHeight=0;
        }
        const auto now=Clock::now();
        const double elapsed=seconds(now-start), interval=seconds(now-sampled);
        if (seconds(now-drawn)>=1.0/60.0) { preview.render(sourceWidth,sourceHeight); drawn=now; }
        if (interval>=0.25 || (options.duration>0 && elapsed>=options.duration)) {
            const double fps=counted ? double(frames-sampleFrames)/interval : -1;
            const double age=frames ? seconds(now-lastNew)*1000 : -1;
            log << std::fixed << std::setprecision(3) << elapsed << ',' << connected << ',' << sourceWidth << ',' << sourceHeight
                << ',' << unsigned(receiver.GetSenderFormat()) << ',' << generation << ',' << frames << ',' << skipped << ',' << counted
                << ',' << fps << ',' << totalMs/calls << ',' << maxMs << ',' << age << '\n';
            log.flush();
            if (!log) throw std::runtime_error("Texture test CSV write failed");
            if (window) {
                std::ostringstream text;
                text << (connected ? (age>500 ? "STALLED" : "Connected") : "Waiting for sender") << " | " << sourceWidth << " x " << sourceHeight
                    << " | new frames " << frames << " | observed FPS " << std::fixed << std::setprecision(1) << fps
                    << " | skipped sender frames " << skipped << (counted ? "" : " (counter unavailable)")
                    << "\r\nReceive API avg/max " << std::setprecision(3) << totalMs/calls << " / " << maxMs
                    << " ms | last new frame age " << std::setprecision(0) << age << " ms"
                    << "\r\n" << preview.adapterName << " | GPU receive + preview | CSV: " << logPath.string();
                SetWindowTextA(statusLabel,text.str().c_str());
            }
            sampled=now; sampleFrames=frames; totalMs=maxMs=0; calls=0;
        }
        if (options.duration>0 && elapsed>=options.duration) break;
        Sleep(1);
    }
    preview.setTexture(nullptr); receiver.ReleaseReceiver(); receiver.CloseDirectX11();
    if (window && IsWindow(window)) DestroyWindow(window);
    if (options.duration>0 && (frames==0 || (!options.snapshot.empty() && !snapshotSaved))) return 2;
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Options options;
    try {
        for (int i=1; i<__argc; ++i) {
            const std::string arg=__argv[i];
            if (arg=="--headless") { options.headless=true; continue; }
            if (i+1>=__argc) throw std::runtime_error("Missing option value");
            const std::string value=__argv[++i];
            if (arg=="--sender") options.sender=value;
            else if (arg=="--seconds") { options.duration=std::stod(value); if (!(options.duration>0)) throw std::runtime_error("Seconds must be positive"); }
            else if (arg=="--log") options.log=value;
            else if (arg=="--snapshot") options.snapshot=value;
            else throw std::runtime_error("Unknown command-line option");
        }
        if (options.headless && options.duration<=0) throw std::runtime_error("Headless testing requires --seconds");
        return run(options);
    } catch (const std::exception& error) {
        std::filesystem::create_directories("output");
        std::ofstream("output/blender-texture-error.txt") << error.what() << '\n';
        if (!options.headless) MessageBoxA(nullptr,error.what(),"Blender texture test",MB_ICONERROR);
        return 1;
    }
}
