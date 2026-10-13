// Called only on Blender's draw thread, with its offscreen framebuffer bound.
#include <SpoutSender.h>
#include <memory>
#include <string>

namespace {
std::unique_ptr<SpoutSender> sender;
HGLRC ownerContext = nullptr;
DWORD ownerThread = 0;
std::string error;
using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
BindFramebuffer bindFramebuffer = nullptr;
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
        candidate->SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
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
    sender->ReleaseSender();
    sender.reset();
    ownerContext = nullptr;
    return 1;
}
