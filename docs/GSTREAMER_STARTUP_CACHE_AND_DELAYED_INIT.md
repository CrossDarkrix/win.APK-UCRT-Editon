# GStreamer startup optimization

This revision combines two changes:

1. The final UCRT stage pre-generates `gstreamer-registry.bin` with the final
   plugin and scanner paths by running the staged `gst-inspect-1.0.exe`.
2. GStreamer initialization is started asynchronously only after
   `present_pixels()` submits the first frame. Media paths call the same
   `ensure_initialized()` function and block only when media is requested before
   warm-up completes.

Startup diagnostics are written to stderr as:

- `[win.apk][startup] gstreamer init begin`
- `[win.apk][startup] gstreamer init end (<milliseconds> ms)`

The existing session-volume and preferred-output-device changes are preserved.
