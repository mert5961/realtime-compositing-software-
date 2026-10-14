#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace studio {
// Same-machine QPC-derived steady-clock nanoseconds. These are render-begin
// and tracking RECEIVE timestamps; they do not assert a camera exposure clock.
struct FrameStamp {
    volatile LONG guard=0;
    uint32_t magic=0;
    uint64_t session=0,sequence=0,renderBeginNs=0,trackingReceivedNs=0;
    uint32_t width=0,height=0;
};
static_assert(sizeof(FrameStamp)==48);
inline constexpr uint32_t stampMagic=0x52435346;
inline std::wstring stampName(const char* name) {
    std::wstring result=L"Local\\RealtimeCompositor.Meta.";
    while(*name) result+=wchar_t(static_cast<unsigned char>(*name++));
    return result;
}
}
