#pragma once
#include <cstdint>
namespace winapk::host {
// Push the newest software-rendered BGRA frame into GStreamer. Returns false
// when GStreamer is disabled, unavailable, or failed, so the caller can fall
// back to the original Win32 present path.
bool gstreamer_present_frame(const uint32_t* argb, int width, int height);
void gstreamer_present_shutdown();
}
