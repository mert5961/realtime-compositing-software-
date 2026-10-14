// Called only on Blender's draw thread, with its offscreen framebuffer bound.
#include <SpoutSender.h>
#include <memory>
#include <string>
#include <chrono>
#include "frame_stamp.hpp"

namespace {
std::unique_ptr<SpoutSender> sender;
HGLRC ownerContext = nullptr;
DWORD ownerThread = 0;
std::string error;
using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
BindFramebuffer bindFramebuffer = nullptr;
HANDLE stampMapping=nullptr;
studio::FrameStamp* stamp=nullptr;
void closeStamp() {
    if(stamp) { InterlockedIncrement(&stamp->guard); stamp->magic=0; InterlockedIncrement(&stamp->guard); UnmapViewOfFile(stamp); stamp=nullptr; }
    if(stampMapping) { CloseHandle(stampMapping); stampMapping=nullptr; }
}
constexpr GLenum readBinding = 0x8CAA, drawBinding = 0x8CA6;
constexpr GLenum readFramebuffer = 0x8CA8, drawFramebuffer = 0x8CA9;

// Spout blits through its own FBO. Restore both bindings and the texture binding
// so Blender's state cache still matches the actual OpenGL state on return.
struct GLState {
    GLint read = 0, draw = 0, texture = 0;
    GLboolean scissor = GL_FALSE;
    GLState() {
        glGetIntegerv(readBinding, &read);
        glGetIntegerv(drawBinding, &draw);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
    }
    ~GLState() {
        bindFramebuffer(readFramebuffer, read);
        bindFramebuffer(drawFramebuffer, draw);
        glBindTexture(GL_TEXTURE_2D, texture);
        if (scissor) glEnable(GL_SCISSOR_TEST);
    }
};
bool ownsContext() {
    return ownerContext && wglGetCurrentContext() == ownerContext && GetCurrentThreadId() == ownerThread;
}
}

#define BRIDGE extern "C" __declspec(dllexport)
BRIDGE const char* blender_spout_error() { return error.c_str(); }
BRIDGE const char* blender_spout_name() { return sender ? sender->GetName() : ""; }
BRIDGE uint64_t blender_spout_clock() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// Diagnostic pattern, used only by --validate. Asymmetric corners exercise
// origin, channel order, size, and generation identity through the real GPU path.
BRIDGE int blender_spout_pattern(unsigned width, unsigned height, unsigned phase) {
    if (!wglGetCurrentContext()) return 0;
    GLint box[4]; GLfloat color[4]; GLboolean mask[4];
    glGetIntegerv(GL_SCISSOR_BOX,box); glGetFloatv(GL_COLOR_CLEAR_VALUE,color);
    glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    const bool enabled=glIsEnabled(GL_SCISSOR_TEST)!=0;
    glEnable(GL_SCISSOR_TEST); glColorMask(TRUE,TRUE,TRUE,TRUE);
    const float colors[4][4]={{phase==2?0.f:1.f,phase==2?1.f:0.f,phase==1?0.f:1.f,1}, {0,1,0,1}, {0,0,1,1}, {1,1,0,1}};
    for (unsigned i=0;i<4;++i) {
        glScissor((i%2)*width/2,(i<2?1:0)*height/2,width/2,height/2);
        glClearColor(colors[i][0],colors[i][1],colors[i][2],1); glClear(GL_COLOR_BUFFER_BIT);
    }
    glClearColor(color[0],color[1],color[2],color[3]); glScissor(box[0],box[1],box[2],box[3]);
    glColorMask(mask[0],mask[1],mask[2],mask[3]); if (!enabled) glDisable(GL_SCISSOR_TEST);
    return 1;
}

BRIDGE int blender_spout_start(const char* name) {
    try {
        if (sender) { error = "Sender already started"; return 0; }
        ownerContext = wglGetCurrentContext();
        ownerThread = GetCurrentThreadId();
        if (!ownerContext) { error = "An active OpenGL context is required"; return 0; }
        bindFramebuffer = reinterpret_cast<BindFramebuffer>(wglGetProcAddress("glBindFramebuffer"));
        if (!bindFramebuffer) { error = "OpenGL framebuffer functions unavailable"; return 0; }
        GLState restore;
        auto candidate = std::make_unique<SpoutSender>();
        candidate->SetAutoShare(false);
        candidate->SetCPUshare(false);
        if (!candidate->IsGLDXready()) {
            error = "OpenGL/DirectX GPU interop unavailable; CPU fallback is disabled";
            return 0;
        }
        candidate->SetSenderName(name);
        if (strcmp(candidate->GetName(), name) != 0) {
            error = "Sender name is already in use. Choose another name."; return 0;
        }
        candidate->SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        const auto mappingName=studio::stampName(candidate->GetName());
        stampMapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(studio::FrameStamp),mappingName.c_str());
        if(!stampMapping) { error="Cannot create frame metadata mapping"; return 0; }
        stamp=static_cast<studio::FrameStamp*>(MapViewOfFile(stampMapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(studio::FrameStamp)));
        if(!stamp) { closeStamp(); error="Cannot map frame metadata"; return 0; }
        InterlockedExchange(&stamp->guard,1);
        stamp->magic=studio::stampMagic; stamp->session=blender_spout_clock(); stamp->sequence=0;
        stamp->renderBeginNs=0; stamp->trackingReceivedNs=0; stamp->width=stamp->height=0;
        InterlockedExchange(&stamp->guard,2);
        sender = std::move(candidate);
        error.clear();
        return 1;
    } catch (...) { error = "Native sender initialization failed"; return 0; }
}

BRIDGE int blender_spout_send(unsigned width, unsigned height) {
    try {
        if (!sender || !ownsContext()) { error = "Sender draw context changed"; return 0; }
        GLState restore;
        if (restore.draw == 0 || width == 0 || height == 0) {
            error = "Bind the Blender offscreen framebuffer before sending"; return 0;
        }
        bindFramebuffer(readFramebuffer, restore.draw);
        const long previous = sender->GetFrame();
        const bool ok = sender->SendFbo(static_cast<GLuint>(restore.draw), width, height, true);
        if (!ok || sender->GetCPU() || !sender->GetGLDX()) {
            error = "Spout GPU framebuffer transfer failed"; return 0;
        }
        // Upstream returns true even if the interprocess lock was busy. Report
        // a skipped publication separately instead of inventing a sent frame.
        return sender->GetFrame() != previous ? 1 : 2;
    } catch (...) { error = "Native sender transfer failed"; return 0; }
}

BRIDGE int blender_spout_stop() {
    if (!sender) return 1;
    if (!ownsContext()) { error = "Stop must run in the original draw context"; return 0; }
    GLState restore;
    closeStamp();
    sender->ReleaseSender();
    sender.reset();
    ownerContext = nullptr;
    return 1;
}

BRIDGE int blender_spout_send_tagged(unsigned width,unsigned height,uint64_t renderBeginNs,uint64_t trackingReceivedNs) {
    if(!stamp || !sender || !ownsContext()) { error="Tagged sender context unavailable"; return 0; }
    // Odd generation covers the entire image publication. Readers validate
    // unchanged even guards BEFORE and AFTER reception and require the exact
    // SDK image counter. A separate unpaired metadata update is never accepted.
    InterlockedIncrement(&stamp->guard);
    const int result=blender_spout_send(width,height);
    if(result==1) {
        stamp->sequence=sender->GetFrame(); stamp->renderBeginNs=renderBeginNs;
        stamp->trackingReceivedNs=trackingReceivedNs; stamp->width=width; stamp->height=height;
    }
    InterlockedIncrement(&stamp->guard);
    return result;
}
