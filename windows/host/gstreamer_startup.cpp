#include "gstreamer_startup.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#ifdef WINAPK_HAVE_GSTREAMER
#include <gst/gst.h>
#endif

namespace winapk::gstreamer_startup {
namespace {
std::once_flag g_init_once;
std::atomic<bool> g_warm_thread_started{false};
}

void ensure_initialized() {
#ifdef WINAPK_HAVE_GSTREAMER
    std::call_once(g_init_once, [] {
        const auto begin = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[win.apk][startup] gstreamer init begin\n");
        std::fflush(stderr);
        gst_init(nullptr, nullptr);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();
        std::fprintf(stderr, "[win.apk][startup] gstreamer init end (%lld ms)\n",
                     static_cast<long long>(elapsed));
        std::fflush(stderr);
    });
#endif
}

void warm_after_first_frame_async() {
#ifdef WINAPK_HAVE_GSTREAMER
    bool expected = false;
    if (!g_warm_thread_started.compare_exchange_strong(expected, true)) return;
    std::thread([] { ensure_initialized(); }).detach();
#endif
}

}  // namespace winapk::gstreamer_startup
