// signal.cpp -- signals and POSIX timers, for guest code.
//
// Windows delivers no POSIX signals, so nothing here installs a handler or
// makes a timer fire. But libraries set them up at load and read back what
// they set: crash reporters (Crashlytics, Breakpad, Sentry) install handlers
// with sigaction and an alternate stack with sigaltstack, keep the handler
// they replaced to chain to it, and block and unblock signals around their
// work. Stubs that returned 0 without writing their out-parameters handed
// that code uninitialized memory (Waze crashed in Crashlytics' StringPrintf;
// Opera after timer_create). So each call keeps its state, per process or
// per thread as on Linux, and answers from it.
//
// The layouts are the same for x86_64 and arm64 guests where it matters:
// struct sigaction is 32 bytes (copied whole, never interpreted, except its
// first field, the handler); stack_t is { void*, int, size_t } with ss_flags
// at offset 8; sigset_t and sigset64_t are one 64-bit word on LP64.

#include "bionic_internal.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <windows.h>

namespace winapk::bionic {

namespace {

constexpr int kSigCount = 64;  // signals 1..64
constexpr size_t kSigactionSize = 32;
constexpr int kSigBlock = 0, kSigUnblock = 1, kSigSetmask = 2;
constexpr int kSsDisable = 2;

bool valid(int sig) { return sig >= 1 && sig <= kSigCount; }

// ---- sigset_t -------------------------------------------------------------------------
WINAPK_GUEST_ABI int g_sigemptyset(uint64_t* set) {
    if (!set) return *guest_errno() = kEINVAL, -1;
    *set = 0;
    return 0;
}
WINAPK_GUEST_ABI int g_sigfillset(uint64_t* set) {
    if (!set) return *guest_errno() = kEINVAL, -1;
    *set = ~uint64_t{0};
    return 0;
}
WINAPK_GUEST_ABI int g_sigaddset(uint64_t* set, int sig) {
    if (!set || !valid(sig)) return *guest_errno() = kEINVAL, -1;
    *set |= uint64_t{1} << (sig - 1);
    return 0;
}
WINAPK_GUEST_ABI int g_sigdelset(uint64_t* set, int sig) {
    if (!set || !valid(sig)) return *guest_errno() = kEINVAL, -1;
    *set &= ~(uint64_t{1} << (sig - 1));
    return 0;
}
WINAPK_GUEST_ABI int g_sigismember(const uint64_t* set, int sig) {
    if (!set || !valid(sig)) return *guest_errno() = kEINVAL, -1;
    return (*set >> (sig - 1)) & 1 ? 1 : 0;
}

// ---- handlers -----------------------------------------------------------------------
// What each signal's last sigaction set, byte for byte; zero is SIG_DFL.
std::mutex g_actions_lock;
uint8_t g_actions[kSigCount + 1][kSigactionSize];

WINAPK_GUEST_ABI int g_sigaction(int sig, const void* act, void* old) {
    if (!valid(sig) || ((sig == 9 || sig == 19) && act)) return *guest_errno() = kEINVAL, -1;  // SIGKILL, SIGSTOP
    std::lock_guard<std::mutex> hold(g_actions_lock);
    if (old) std::memcpy(old, g_actions[sig], kSigactionSize);
    if (act) std::memcpy(g_actions[sig], act, kSigactionSize);
    return 0;
}

using Handler = void*;
WINAPK_GUEST_ABI Handler g_signal(int sig, Handler handler) {
    if (!valid(sig) || sig == 9 || sig == 19) {
        *guest_errno() = kEINVAL;
        return reinterpret_cast<Handler>(-1);  // SIG_ERR
    }
    std::lock_guard<std::mutex> hold(g_actions_lock);
    Handler previous;
    std::memcpy(&previous, g_actions[sig], sizeof previous);
    std::memset(g_actions[sig], 0, kSigactionSize);
    std::memcpy(g_actions[sig], &handler, sizeof handler);
    return previous;
}

// ---- the alternate stack and the mask, per thread ------------------------------------
struct GuestStack {
    void* sp;
    int32_t flags;
    int32_t pad;
    uint64_t size;
};
thread_local GuestStack t_altstack{nullptr, kSsDisable, 0, 0};
thread_local uint64_t t_mask = 0;

WINAPK_GUEST_ABI int g_sigaltstack(const GuestStack* ss, GuestStack* old) {
    if (old) *old = t_altstack;
    if (ss) {
        t_altstack = *ss;
        if (ss->flags & kSsDisable) t_altstack = GuestStack{nullptr, kSsDisable, 0, 0};
    }
    return 0;
}

int apply_mask(int how, const uint64_t* set, uint64_t* old) {
    const uint64_t previous = t_mask;
    if (set) {
        switch (how) {
            case kSigBlock: t_mask |= *set; break;
            case kSigUnblock: t_mask &= ~*set; break;
            case kSigSetmask: t_mask = *set; break;
            default: return kEINVAL;
        }
    }
    if (old) *old = previous;
    return 0;
}
WINAPK_GUEST_ABI int g_sigprocmask(int how, const uint64_t* set, uint64_t* old) {
    const int err = apply_mask(how, set, old);
    if (err) return *guest_errno() = err, -1;
    return 0;
}
WINAPK_GUEST_ABI int g_pthread_sigmask(int how, const uint64_t* set, uint64_t* old) {
    return apply_mask(how, set, old);  // an errno value, not -1
}
WINAPK_GUEST_ABI int g_sigpending(uint64_t* set) {
    if (set) *set = 0;
    return 0;
}

WINAPK_GUEST_ABI int g_raise(int sig) {
    if (sig == 6) {  // SIGABRT
        std::fprintf(stderr, "[win.apk] guest raised SIGABRT\n");
        std::fflush(stderr);
        std::abort();
    }
    if (sig == 9 || sig == 15) TerminateProcess(GetCurrentProcess(), 128 + sig);
    return 0;
}

// ---- POSIX timers ---------------------------------------------------------------------
// Created and armed, but they do not fire (that would need a delivered
// signal or SIGEV_THREAD callback); said once in the log.
std::atomic<uintptr_t> g_next_timer{1};
std::once_flag g_timer_note;

WINAPK_GUEST_ABI int g_timer_create(int, const void*, void** id) {
    std::call_once(g_timer_note, [] {
        std::fprintf(stderr, "[win.apk] guest created a POSIX timer; timers do not fire yet\n");
        std::fflush(stderr);
    });
    if (!id) return *guest_errno() = kEINVAL, -1;
    *id = reinterpret_cast<void*>(g_next_timer.fetch_add(1));
    return 0;
}
WINAPK_GUEST_ABI int g_timer_settime(void*, int, const void*, void* old) {
    if (old) std::memset(old, 0, 32);  // struct itimerspec: two timespecs
    return 0;
}
WINAPK_GUEST_ABI int g_timer_gettime(void*, void* value) {
    if (value) std::memset(value, 0, 32);
    return 0;
}
WINAPK_GUEST_ABI int g_timer_delete(void*) { return 0; }
WINAPK_GUEST_ABI int g_timer_getoverrun(void*) { return 0; }

// ---- scheduling priority ----------------------------------------------------------------
WINAPK_GUEST_ABI int g_setpriority(int, int, int) { return 0; }
WINAPK_GUEST_ABI int g_getpriority(int, int) { return 0; }

const ShimSymbol kSymbols[] = {
    WINAPK_SHIM("sigemptyset", g_sigemptyset),
    WINAPK_SHIM("sigemptyset64", g_sigemptyset),
    WINAPK_SHIM("sigfillset", g_sigfillset),
    WINAPK_SHIM("sigfillset64", g_sigfillset),
    WINAPK_SHIM("sigaddset", g_sigaddset),
    WINAPK_SHIM("sigaddset64", g_sigaddset),
    WINAPK_SHIM("sigdelset", g_sigdelset),
    WINAPK_SHIM("sigdelset64", g_sigdelset),
    WINAPK_SHIM("sigismember", g_sigismember),
    WINAPK_SHIM("sigismember64", g_sigismember),
    WINAPK_SHIM("sigaction", g_sigaction),
    WINAPK_SHIM("sigaction64", g_sigaction),
    WINAPK_SHIM("signal", g_signal),
    WINAPK_SHIM("bsd_signal", g_signal),
    WINAPK_SHIM("sigaltstack", g_sigaltstack),
    WINAPK_SHIM("sigprocmask", g_sigprocmask),
    WINAPK_SHIM("sigprocmask64", g_sigprocmask),
    WINAPK_SHIM("pthread_sigmask", g_pthread_sigmask),
    WINAPK_SHIM("pthread_sigmask64", g_pthread_sigmask),
    WINAPK_SHIM("sigpending", g_sigpending),
    WINAPK_SHIM("sigpending64", g_sigpending),
    WINAPK_SHIM("raise", g_raise),
    WINAPK_SHIM("timer_create", g_timer_create),
    WINAPK_SHIM("timer_settime", g_timer_settime),
    WINAPK_SHIM("timer_gettime", g_timer_gettime),
    WINAPK_SHIM("timer_delete", g_timer_delete),
    WINAPK_SHIM("timer_getoverrun", g_timer_getoverrun),
    WINAPK_SHIM("setpriority", g_setpriority),
    WINAPK_SHIM("getpriority", g_getpriority),
};

}  // namespace

ShimTable signal_symbols() { return WINAPK_SHIM_TABLE(kSymbols); }

}  // namespace winapk::bionic
