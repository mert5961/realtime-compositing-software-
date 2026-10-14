#include "studio_gpu.hpp"
#include "compositor/live_pool.hpp"
#include "frame_stamp.hpp"
#include <SpoutDX.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace studio {
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
uint64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
void checked(HRESULT result,const char* operation) { if(FAILED(result)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(uint32_t(result))); }
struct Device {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    unsigned adapter=0;
    Device() {
        ComPtr<IDXGIFactory1> factory; checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"GPU enumeration");
        ComPtr<IDXGIAdapter1> candidate,chosen; SIZE_T largest=0;
        for(unsigned i=0;;++i) {
            auto result=factory->EnumAdapters1(i,&candidate);
            if(result==DXGI_ERROR_NOT_FOUND) break;
            checked(result,"Enumerate adapter"); DXGI_ADAPTER_DESC1 desc{}; checked(candidate->GetDesc1(&desc),"Read adapter");
            if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && (!chosen || desc.DedicatedVideoMemory>largest)) {
                chosen=candidate; largest=desc.DedicatedVideoMemory; adapter=i;
            }
            candidate.Reset();
        }
        if(!chosen) throw std::runtime_error("No hardware GPU");
        checked(D3D11CreateDevice(chosen.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"Create GPU device");
    }
};
struct GpuFrame {
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11Query> completion;
    HANDLE handle=nullptr;
    unsigned width=0,height=0,slot=0;
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint64_t sequence=0,generation=0,resource=0,receivedNs=0;
    uint64_t renderBeginNs=0,trackingReceivedNs=0,session=0;
    uint64_t scheduledNs=0,poseTargetNs=0;
};
class StampReader {
    HANDLE mapping=nullptr;
    const FrameStamp* view=nullptr;
    std::wstring name;
public:
    explicit StampReader(const std::string& sender):name(stampName(sender.c_str())) {}
    ~StampReader() { if(view) UnmapViewOfFile(view); if(mapping) CloseHandle(mapping); }
    std::optional<FrameStamp> read() {
        if(!view) {
            mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,name.c_str());
            if(!mapping) return std::nullopt;
            view=static_cast<const FrameStamp*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(FrameStamp)));
            if(!view) { CloseHandle(mapping); mapping=nullptr; return std::nullopt; }
        }
        const LONG before=view->guard; if(before&1) return std::nullopt;
        MemoryBarrier(); FrameStamp snapshot; memcpy(&snapshot,view,sizeof(snapshot)); MemoryBarrier();
        if(before!=view->guard || snapshot.magic!=stampMagic || !snapshot.sequence) return std::nullopt;
        return snapshot;
    }
    bool present() const { return view!=nullptr; }
};
class Source {
    std::atomic<bool> stopping{false}; std::thread thread;
    mutable std::mutex statusMutex; std::string status="Waiting for Blender";
    std::string name;
    void setStatus(std::string text) { std::lock_guard lock(statusMutex); status=std::move(text); }
    void run() {
        try {
            Device gpu; spoutDX receiver;
            if(!receiver.OpenDirectX11(gpu.device.Get())) throw std::runtime_error("Spout device initialization");
            receiver.SetReceiverName(name.c_str());
            StampReader stamps(name); uint64_t previousSession=0;
            long previous=0; uint64_t generation=0,resource=0;
            bool wasConnected=false;
            uint64_t adapterChecked=0; bool mismatch=false;
            while(!stopping) {
                if(!wasConnected && nowNs()-adapterChecked>1000000000) {
                    adapterChecked=nowNs(); char adapterName[256]{};
                    const int senderAdapter=receiver.GetSenderAdapter(name.c_str(),adapterName,256);
                    mismatch=senderAdapter>=0 && unsigned(senderAdapter)!=gpu.adapter;
                    if(mismatch) setStatus(std::string("Blender GPU mismatch: ")+adapterName);
                }
                if(mismatch) { pool.invalidate(); Sleep(50); continue; }
                const auto before=stamps.read();
                const bool connected=receiver.ReceiveTexture();
                const auto after=stamps.read();
                if(receiver.IsUpdated()) { ++generation; previous=0; pool.invalidate(); }
                if(!connected || !receiver.IsConnected()) {
                    if(wasConnected) pool.invalidate(); previous=0; wasConnected=false;
                    setStatus("Waiting for Blender sender "+name); Sleep(2); continue;
                }
                wasConnected=true;
                const auto sequence=receiver.GetSenderFrame();
                if(!receiver.IsFrameCountEnabled() || sequence<=0) { setStatus("Sender frame counter unavailable; image rejected"); Sleep(2); continue; }
                if(sequence==previous) { Sleep(1); continue; }
                if(stamps.present()) {
                    if(!before || !after || before->guard!=after->guard || before->session!=after->session || after->sequence!=uint64_t(sequence)
                        || after->width!=receiver.GetSenderWidth() || after->height!=receiver.GetSenderHeight()) { Sleep(1); continue; }
                    if(previousSession && previousSession!=after->session) { ++generation; pool.invalidate(); }
                    previousSession=after->session;
                } else if(!receiver.IsFrameNew()) { Sleep(1); continue; }
                // Counter advancement occurs inside the SDK's successful locked
                // copy path. Discovery/allocation alone never publishes a frame.
                auto* texture=receiver.GetSenderTexture();
                if(!texture) { Sleep(1); continue; }
                const uint64_t received=nowNs();
                previous=sequence;
                auto reservation=pool.tryWrite();
                if(!reservation) { setStatus("Blender GPU pool full; latest publication discarded"); continue; }
                auto& frame=reservation->get(); D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
                if(desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM)
                    throw std::runtime_error("Blender format must be RGBA8/BGRA8 for this keyer");
                if(!frame.texture || frame.width!=desc.Width || frame.height!=desc.Height || frame.format!=desc.Format) {
                    frame.texture.Reset(); frame.completion.Reset();
                    desc.Usage=D3D11_USAGE_DEFAULT; desc.CPUAccessFlags=0;
                    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET; desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
                    checked(gpu.device->CreateTexture2D(&desc,nullptr,&frame.texture),"Create owned Blender texture");
                    ComPtr<IDXGIResource> shared; checked(frame.texture.As(&shared),"Get shared texture");
                    checked(shared->GetSharedHandle(&frame.handle),"Get owned shared handle");
                    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0}; checked(gpu.device->CreateQuery(&query,&frame.completion),"Create copy completion query");
                    frame.width=desc.Width; frame.height=desc.Height; frame.format=desc.Format; frame.resource=++resource;
                }
                gpu.context->CopyResource(frame.texture.Get(),texture);
                gpu.context->End(frame.completion.Get()); gpu.context->Flush();
                const auto deadline=Clock::now()+std::chrono::milliseconds(500);
                HRESULT completed;
                while((completed=gpu.context->GetData(frame.completion.Get(),nullptr,0,0))==S_FALSE && !stopping && Clock::now()<deadline) Sleep(1);
                if(completed!=S_OK) { checked(completed,"Blender GPU completion"); throw std::runtime_error("Blender copy completion timeout"); }
                frame.sequence=sequence; frame.generation=generation; frame.receivedNs=received; frame.slot=unsigned(reservation->index());
                frame.renderBeginNs=after ? after->renderBeginNs : 0; frame.trackingReceivedNs=after ? after->trackingReceivedNs : 0;
                frame.session=after ? after->session : 0;
                frame.scheduledNs=after ? after->scheduledNs : 0; frame.poseTargetNs=after ? after->poseTargetNs : 0;
                reservation->publish(); setStatus("Blender GPU frames ready");
            }
            receiver.ReleaseReceiver(); receiver.CloseDirectX11();
        } catch(const std::exception& error) { pool.invalidate(); setStatus(std::string("Blender transport fault: ")+error.what()); }
    }
public:
    compositor::LivePool<GpuFrame> pool{4};
    explicit Source(std::string sender):name(std::move(sender)) { thread=std::thread([this]{run();}); }
    ~Source() { stopping=true; if(thread.joinable()) thread.join(); }
    std::string getStatus() const { std::lock_guard lock(statusMutex); return status; }
};

struct GpuCompositor::Impl {
    Device gpu; Source source;
    unsigned width,height; float threshold,softness;
    ComPtr<ID3D11Texture2D> camera,composite,packed,staging;
    ComPtr<ID3D11ShaderResourceView> cameraView,compositeView;
    ComPtr<ID3D11RenderTargetView> compositeTarget,packedTarget;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> keyShader,packShader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    struct Cache { uint64_t resource=0; ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view; };
    std::array<Cache,4> cache;
    Impl(unsigned w,unsigned h,std::string sender,float t,float s):source(std::move(sender)),width(w),height(h),threshold(t),softness(s) {
        if(!w || !h || w%2) throw std::runtime_error("Even image width required");
        auto makeTexture=[&](unsigned tw,unsigned th,unsigned flags,ComPtr<ID3D11Texture2D>& texture) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=tw; desc.Height=th; desc.MipLevels=desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.BindFlags=flags;
            checked(gpu.device->CreateTexture2D(&desc,nullptr,&texture),"Allocate compositor texture");
        };
        makeTexture(w/2,h,D3D11_BIND_SHADER_RESOURCE,camera);
        makeTexture(w,h,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,composite);
        makeTexture(w/2,h,D3D11_BIND_RENDER_TARGET,packed);
        checked(gpu.device->CreateShaderResourceView(camera.Get(),nullptr,&cameraView),"Camera SRV");
        checked(gpu.device->CreateShaderResourceView(composite.Get(),nullptr,&compositeView),"Composite SRV");
        checked(gpu.device->CreateRenderTargetView(composite.Get(),nullptr,&compositeTarget),"Composite target");
        checked(gpu.device->CreateRenderTargetView(packed.Get(),nullptr,&packedTarget),"UYVY target");
        D3D11_TEXTURE2D_DESC desc{}; packed->GetDesc(&desc); desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        checked(gpu.device->CreateTexture2D(&desc,nullptr,&staging),"Output staging");
        const char* shader=R"(
            cbuffer Settings:register(b0) { float width; float height; float threshold; float softness; uint enabled; float3 padding; };
            Texture2D camera:register(t0); Texture2D background:register(t1); SamplerState sampling:register(s0);
            struct V { float4 position:SV_POSITION; float2 uv:TEXCOORD0; };
            V vs(uint id:SV_VertexID) { V v; v.uv=float2((id<<1)&2,id&2); v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1); return v; }
            float4 key(V input):SV_TARGET {
                int2 p=int2(input.position.xy); float4 packed=camera.Load(int3(p.x/2,p.y,0));
                float y=((p.x%2?packed.a:packed.g)*255-16)/219;
                float cb=(packed.r*255-128)/224, cr=(packed.b*255-128)/224;
                float3 rgb=saturate(float3(y+1.5748*cr,y-0.187324*cb-0.468124*cr,y+1.8556*cb));
                if(enabled==0) return float4(rgb,1);
                float alpha=1-smoothstep(threshold,threshold+softness,rgb.g-max(rgb.r,rgb.b));
                rgb.g=min(rgb.g,max(rgb.r,rgb.b)+0.06);
                return float4(lerp(background.Sample(sampling,input.uv).rgb,rgb,alpha),1);
            }
            float4 pack(V input):SV_TARGET {
                int2 p=int2(input.position.xy); float3 a=camera.Load(int3(p.x*2,p.y,0)).rgb;
                float3 b=camera.Load(int3(p.x*2+1,p.y,0)).rgb;
                float ya=dot(a,float3(.2126,.7152,.0722)), yb=dot(b,float3(.2126,.7152,.0722));
                float cb=((a.b-ya)+(b.b-yb))/(2*1.8556), cr=((a.r-ya)+(b.r-yb))/(2*1.5748);
                return float4(128+224*cb,16+219*ya,128+224*cr,16+219*yb)/255;
            }
        )";
        auto compile=[&](const char* entry,const char* profile) {
            ComPtr<ID3DBlob> code,error; auto hr=D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,entry,profile,0,0,&code,&error);
            if(FAILED(hr)) throw std::runtime_error(error?static_cast<const char*>(error->GetBufferPointer()):"Shader compilation failed");
            return code;
        };
        auto vs=compile("vs","vs_5_0"), key=compile("key","ps_5_0"), pack=compile("pack","ps_5_0");
        checked(gpu.device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex),"Vertex shader");
        checked(gpu.device->CreatePixelShader(key->GetBufferPointer(),key->GetBufferSize(),nullptr,&keyShader),"Key shader");
        checked(gpu.device->CreatePixelShader(pack->GetBufferPointer(),pack->GetBufferSize(),nullptr,&packShader),"Pack shader");
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth=32; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        checked(gpu.device->CreateBuffer(&cb,nullptr,&constants),"Key settings buffer");
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        checked(gpu.device->CreateSamplerState(&sd,&sampler),"Background sampler");
    }
    CompositeInfo render(const void* input,unsigned pitch,void* output) {
        const auto begin=nowNs();
        // One frame of bounded presentation delay absorbs the measured 60 Hz
        // viewport jitter of a nominal 50 Hz sender. Future frames stay owned
        // in the pool; capture never waits for Blender to catch up.
        auto background=source.pool.newestMatching([&](const GpuFrame& f) {
            return !f.scheduledNs || f.scheduledNs+20000000<=begin;
        });
        CompositeInfo info;
        if(background) {
            info.blenderReceivedNs=background->receivedNs; info.blenderSequence=background->sequence; info.blenderGeneration=background->generation;
            info.blenderRenderBeginNs=background->renderBeginNs; info.trackingReceivedNs=background->trackingReceivedNs; info.blenderSession=background->session;
            info.blenderScheduledNs=background->scheduledNs; info.poseTargetNs=background->poseTargetNs;
        }
        ComPtr<ID3D11ShaderResourceView> backgroundView;
        info.usedBlender=background && begin-background->receivedNs<=500000000;
        if(background && background->trackingReceivedNs && begin-background->trackingReceivedNs>500000000) info.usedBlender=false;
        if(info.usedBlender) {
            auto& entry=cache[background->slot];
            if(entry.resource!=background->resource) {
                entry.view.Reset(); entry.texture.Reset();
                checked(gpu.device->OpenSharedResource(background->handle,IID_PPV_ARGS(&entry.texture)),"Open owned Blender texture");
                checked(gpu.device->CreateShaderResourceView(entry.texture.Get(),nullptr,&entry.view),"Blender SRV");
                entry.resource=background->resource;
            }
            backgroundView=entry.view;
        }
        // Camera bytes are U Y0 V Y1. Upload and keying happen on this device;
        // Spout waits and resource copy completion remain on the source worker.
        gpu.context->UpdateSubresource(camera.Get(),0,nullptr,input,pitch,0);
        struct Settings { float width,height,threshold,softness; unsigned enabled; float padding[3]; } settings{float(width),float(height),threshold,softness,unsigned(info.usedBlender),{}};
        gpu.context->UpdateSubresource(constants.Get(),0,nullptr,&settings,0,0);
        gpu.context->PSSetConstantBuffers(0,1,constants.GetAddressOf()); gpu.context->PSSetSamplers(0,1,sampler.GetAddressOf());
        gpu.context->VSSetShader(vertex.Get(),nullptr,0); gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        auto draw=[&](unsigned targetWidth,ID3D11RenderTargetView* target,ID3D11PixelShader* shader,ID3D11ShaderResourceView* first,ID3D11ShaderResourceView* second) {
            D3D11_VIEWPORT viewport{0,0,float(targetWidth),float(height),0,1}; gpu.context->RSSetViewports(1,&viewport);
            gpu.context->OMSetRenderTargets(1,&target,nullptr); gpu.context->PSSetShader(shader,nullptr,0);
            ID3D11ShaderResourceView* views[2]={first,second}; gpu.context->PSSetShaderResources(0,2,views); gpu.context->Draw(3,0);
            ID3D11ShaderResourceView* empty[2]={}; gpu.context->PSSetShaderResources(0,2,empty);
        };
        draw(width,compositeTarget.Get(),keyShader.Get(),cameraView.Get(),backgroundView.Get());
        draw(width/2,packedTarget.Get(),packShader.Get(),compositeView.Get(),nullptr);
        gpu.context->OMSetRenderTargets(0,nullptr,nullptr);
        gpu.context->CopyResource(staging.Get(),packed.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(gpu.context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Complete composite readback");
        for(unsigned y=0;y<height;++y) memcpy(static_cast<unsigned char*>(output)+y*width*2,static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch,width*2);
        gpu.context->Unmap(staging.Get(),0);
        // Blocking Map completes all earlier reads of the selected shared frame.
        // Only now may its lease expire and the source worker reuse that surface.
        info.processingMs=double(nowNs()-begin)/1e6; return info;
    }
};
GpuCompositor::GpuCompositor(unsigned w,unsigned h,std::string name,float t,float s):impl_(std::make_unique<Impl>(w,h,std::move(name),t,s)) {}
GpuCompositor::~GpuCompositor()=default;
CompositeInfo GpuCompositor::render(const void* input,unsigned pitch,void* output) { return impl_->render(input,pitch,output); }
std::string GpuCompositor::sourceStatus() const { return impl_->source.getStatus(); }

int gpuSelfTest() {
    const unsigned width=64,height=32;
    Device gpu; spoutDX sender; if(!sender.OpenDirectX11(gpu.device.Get())) throw std::runtime_error("Self-test sender device");
    const std::string name="RealtimeStudioSelfTest-"+std::to_string(GetCurrentProcessId()); sender.SetSenderName(name.c_str());
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture; checked(gpu.device->CreateTexture2D(&desc,nullptr,&texture),"Self-test texture");
    ComPtr<ID3D11RenderTargetView> target; checked(gpu.device->CreateRenderTargetView(texture.Get(),nullptr,&target),"Self-test target");
    const float blue[4]={0,0,1,1}; gpu.context->ClearRenderTargetView(target.Get(),blue);
    GpuCompositor engine(width,height,name);
    std::vector<unsigned char> camera(width*height*2),output(camera.size());
    for(size_t i=0;i<camera.size();i+=4) { camera[i]=42; camera[i+1]=173; camera[i+2]=26; camera[i+3]=173; }
    CompositeInfo info; const auto deadline=Clock::now()+std::chrono::seconds(3);
    do { sender.SendTexture(texture.Get()); info=engine.render(camera.data(),width*2,output.data()); Sleep(5); } while(!info.usedBlender && Clock::now()<deadline);
    if(!info.usedBlender) throw std::runtime_error("GPU source never published a valid frame: "+engine.sourceStatus());
    const unsigned char expected[4]={240,32,118,32};
    for(size_t i=0;i<output.size();++i) if(std::abs(int(output[i])-int(expected[i%4]))>3) throw std::runtime_error("Green key / BT.709 pack numeric validation failed");
    auto held=engine.impl_->source.pool.latest();
    if(!held) throw std::runtime_error("No immutable GPU lease");
    const float red[4]={1,0,0,1}; gpu.context->ClearRenderTargetView(target.Get(),red);
    for(unsigned i=0;i<30;++i) { sender.SendTexture(texture.Get()); Sleep(5); }
    ComPtr<ID3D11Texture2D> heldTexture,readback;
    checked(gpu.device->OpenSharedResource(held->handle,IID_PPV_ARGS(&heldTexture)),"Open held GPU image");
    heldTexture->GetDesc(&desc); desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=desc.MiscFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    checked(gpu.device->CreateTexture2D(&desc,nullptr,&readback),"Held-image diagnostic readback");
    gpu.context->CopyResource(readback.Get(),heldTexture.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    checked(gpu.context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Complete held-image inspection");
    const auto* pixel=static_cast<const unsigned char*>(mapped.pData);
    const bool stillBlue=pixel[0]==0 && pixel[1]==0 && pixel[2]==255 && pixel[3]==255;
    gpu.context->Unmap(readback.Get(),0); held.reset();
    if(!stillBlue) throw std::runtime_error("Held GPU pixels changed while newer red frames arrived");
    info=engine.render(camera.data(),width*2,output.data());
    const unsigned char expectedRed[4]={102,63,240,63};
    for(size_t i=0;i<output.size();++i) if(std::abs(int(output[i])-int(expectedRed[i%4]))>3) throw std::runtime_error("New source image not delivered after held GPU lease");
    Sleep(550); // Deliberate sender stall, confined to this hardware integration test.
    info=engine.render(camera.data(),width*2,output.data());
    if(info.usedBlender) throw std::runtime_error("Stale Blender frame was selected");
    for(size_t i=0;i<output.size();++i) if(std::abs(int(output[i])-int(camera[i]))>3) throw std::runtime_error("Stall camera fallback incorrect");
    sender.ReleaseSender(); sender.CloseDirectX11(); return 0;
}
}
