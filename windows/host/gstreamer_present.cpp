// gstreamer_present.cpp -- Stage 1 asynchronous GStreamer presentation.
// Keeps ViewCanvasDriver compatibility while moving frame queueing, D3D11
// upload and presentation off the Java draw thread.
#include "gstreamer_present.h"
#include "gstreamer_startup.h"
#include "win32_window.h"

#ifdef WINAPK_HAVE_GSTREAMER
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace winapk::host {
namespace {
struct Presenter {
    std::mutex lock;
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    GstElement* sink = nullptr;
    int width = 0;
    int height = 0;
    uint64_t frame = 0;
    bool failed = false;
    bool logged = false;
};
Presenter g;

bool enabled() {
    const char* value = std::getenv("MACAPK_GSTREAMER_PRESENT");
    return !value || std::strcmp(value, "0") != 0;
}

void destroy_locked() {
    if (g.pipeline) gst_element_set_state(g.pipeline, GST_STATE_NULL);
    if (g.source) gst_object_unref(g.source);
    if (g.sink) gst_object_unref(g.sink);
    if (g.pipeline) gst_object_unref(g.pipeline);
    g.source = nullptr;
    g.sink = nullptr;
    g.pipeline = nullptr;
    g.width = g.height = 0;
}

bool start_locked(int width, int height) {
    destroy_locked();
    winapk::gstreamer_startup::ensure_initialized();

    GstElementFactory* d3d11_factory = gst_element_factory_find("d3d11videosink");
    if (!d3d11_factory) {
        std::fprintf(stderr, "[win.apk] GStreamer: d3d11videosink is unavailable; using Win32 presenter\n");
        g.failed = true;
        return false;
    }
    gst_object_unref(d3d11_factory);

    GError* error = nullptr;
    const char* description =
        "appsrc name=winapk_src is-live=true do-timestamp=true format=time "
        "block=false max-buffers=2 ! "
        "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ! "
        "d3d11upload ! d3d11videosink name=winapk_sink sync=false force-aspect-ratio=false";
    g.pipeline = gst_parse_launch(description, &error);
    if (!g.pipeline) {
        std::fprintf(stderr, "[win.apk] GStreamer pipeline failed: %s\n",
                     error ? error->message : "unknown error");
        if (error) g_error_free(error);
        g.failed = true;
        return false;
    }
    if (error) g_error_free(error);

    g.source = gst_bin_get_by_name(GST_BIN(g.pipeline), "winapk_src");
    g.sink = gst_bin_get_by_name(GST_BIN(g.pipeline), "winapk_sink");
    if (!g.source || !g.sink) {
        std::fprintf(stderr, "[win.apk] GStreamer pipeline elements are missing\n");
        destroy_locked();
        g.failed = true;
        return false;
    }

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "BGRA",
        "width", G_TYPE_INT, width,
        "height", G_TYPE_INT, height,
        "framerate", GST_TYPE_FRACTION, 60, 1,
        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(g.source), caps);
    gst_caps_unref(caps);
    gst_app_src_set_stream_type(GST_APP_SRC(g.source), GST_APP_STREAM_TYPE_STREAM);

    void* handle = window_handle();
    if (handle && GST_IS_VIDEO_OVERLAY(g.sink)) {
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(g.sink),
                                             reinterpret_cast<guintptr>(handle));
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(g.sink), FALSE);
    }

    if (gst_element_set_state(g.pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::fprintf(stderr, "[win.apk] GStreamer pipeline could not enter PLAYING; using Win32 presenter\n");
        destroy_locked();
        g.failed = true;
        return false;
    }
    g.width = width;
    g.height = height;
    g.frame = 0;
    if (!g.logged) {
        std::fprintf(stderr,
            "[win.apk] GStreamer presenter active: full-client stretch, force-aspect-ratio=false\n");
        g.logged = true;
    }
    return true;
}
} // namespace

bool gstreamer_present_frame(const uint32_t* argb, int width, int height) {
    if (!enabled() || !argb || width <= 0 || height <= 0) return false;
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.failed) return false;
    if (!g.pipeline || width != g.width || height != g.height) {
        if (!start_locked(width, height)) return false;
    }

    const gsize bytes = static_cast<gsize>(width) * static_cast<gsize>(height) * 4;
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!buffer) return false;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return false;
    }
    std::memcpy(map.data, argb, bytes);
    gst_buffer_unmap(buffer, &map);

    const GstClockTime duration = gst_util_uint64_scale_int(1, GST_SECOND, 60);
    GST_BUFFER_PTS(buffer) = g.frame * duration;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = duration;
    ++g.frame;

    // Ownership transfers to appsrc. appsrc queues for its streaming thread;
    // block=false plus the downstream leaky queue prevents display backpressure
    // from stalling ViewCanvasDriver.
    const GstFlowReturn result = gst_app_src_push_buffer(GST_APP_SRC(g.source), buffer);
    if (result != GST_FLOW_OK) {
        std::fprintf(stderr, "[win.apk] GStreamer push failed (%d); using Win32 presenter\n",
                     static_cast<int>(result));
        destroy_locked();
        g.failed = true;
        return false;
    }
    return true;
}

void gstreamer_present_shutdown() {
    std::lock_guard<std::mutex> hold(g.lock);
    destroy_locked();
}
} // namespace winapk::host

#else
namespace winapk::host {
bool gstreamer_present_frame(const uint32_t*, int, int) { return false; }
void gstreamer_present_shutdown() {}
}
#endif
