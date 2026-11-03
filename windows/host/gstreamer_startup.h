#pragma once

namespace winapk::gstreamer_startup {

// Synchronously makes GStreamer ready. Media paths call this as a correctness
// fallback when media is requested before first-frame warm-up has completed.
void ensure_initialized();

// Starts GStreamer initialization on a detached worker. This is called exactly
// after the first frame has been submitted to the Win32 presenter.
void warm_after_first_frame_async();

}  // namespace winapk::gstreamer_startup
