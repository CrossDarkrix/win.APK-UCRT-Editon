// arm64_test.cpp -- winapk-arm64-test: the arm64 interpreter and its bridge to
// the Windows host (milestone M10, stage 1).
//
//   winapk-arm64-test <dir with libwinapk_arm64_*.so>
//
// Two kinds of checks:
//
//   * Differential: test/guest/arm64_isa.c is compiled into this program and,
//     three times (-O2, -O3, -O3 with the ARMv8.1 atomics), into arm64 guest
//     libraries. Every function runs both ways on the same inputs and the
//     results must match bit for bit: the native build is the reference for
//     the interpreter.
//
//   * The bridge, with test/guest/arm64_bridge_guest.c: libc calls into the
//     Bionic shim, variadic calls and the guest's own va_list, callbacks from
//     the host into guest code (qsort, pthread_create, __cxa_atexit, nested
//     guest -> host -> guest -> host), calls in both directions with stack
//     arguments and floating point, constructors, the stack protector, and
//     setjmp/longjmp.
//
//   * Speed, with test/guest/arm64_bench.c (built into this program too):
//     each benchmark natively and on each engine, the JIT's translation cost,
//     and where its time goes (arm64_guest.h, Stats).
//
// Exit code: the number of failed checks.

#include "winapk/arm64_guest.h"
#include "winapk/guest_abi.h"
#include "winapk/linker.h"
#include "winapk/mini_bionic.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#define ISA_EXPORT static __attribute__((noinline, used))
#include "arm64_isa.c"
#define BENCH_HOST
#define BENCH_EXPORT static __attribute__((noinline, used))
#include "arm64_bench.c"

namespace {

int g_failures = 0;

void check(const char* what, bool ok, const std::string& detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok) ++g_failures;
}

std::string hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%" PRIx64, v);
    return buf;
}

void* sym(winapk::GuestLinker& linker, void* lib, const char* name) {
    std::string error;
    void* p = linker.lookup(lib, name, nullptr, &error);
    if (!p) check(name, false, error);
    return p;
}

const uint64_t kInputs[][3] = {
    {0, 0, 0},
    {1, 2, 3},
    {0xffffffffffffffffull, 1, 0x8000000000000000ull},
    {12345678901234ull, 987654321ull, 42},
    {0x7fffffff, 0x80000000, 0xffffffff},
    {0xdeadbeefcafebabeull, 0x0123456789abcdefull, 0xfedcba9876543210ull},
    {17, 0xffffffffffffffffull, 5},
    {1000000, 3, 7},
};

using IsaFn = u64 (*)(u64, u64, u64);
using GuestIsaFn = u64(WINAPK_GUEST_ABI*)(u64, u64, u64);

void run_isa(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open", lib != nullptr, error);
    if (!lib) return;
    struct Case {
        const char* name;
        IsaFn native;
    };
#define ISA_CASE(name) {#name, &name},
    static const Case kCases[] = {ISA_TESTS(ISA_CASE)};
#undef ISA_CASE
    for (const Case& c : kCases) {
        void* fn = sym(linker, lib, c.name);
        if (!fn) continue;
        std::string first_mismatch;
        int mismatches = 0;
        for (const auto& in : kInputs) {
            const uint64_t want = c.native(in[0], in[1], in[2]);
            const uint64_t got = winapk::arm64::call(fn, in, 3);
            if (got != want) {
                if (!mismatches)
                    first_mismatch = "(" + hex(in[0]) + ", " + hex(in[1]) + ", " + hex(in[2]) + "): native " +
                                     hex(want) + ", arm64 " + hex(got);
                ++mismatches;
            }
        }
        check(c.name, mismatches == 0, first_mismatch);
    }
    // The same code entered the two other ways the host reaches guest code.
    auto* fn = reinterpret_cast<GuestIsaFn>(sym(linker, lib, "isa_loop"));
    if (fn) {
        const uint64_t want = isa_loop(5, 6, 300);
        check("a System V call into arm64 code (execute-fault path)", fn(5, 6, 300) == want);
        auto* entry = reinterpret_cast<GuestIsaFn>(winapk::arm64::host_callable(reinterpret_cast<void*>(fn)));
        check("the same call through host_callable()", entry != fn && entry(5, 6, 300) == want);
    }
}

// The host function arm64_call_host calls (System V, like every shim function).
WINAPK_GUEST_ABI int64_t host_many_args(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f, int64_t g,
                                        int64_t h, int64_t i, int64_t j, double x, double y) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j +
           static_cast<int64_t>(x * 100 + y * 1000);
}

void run_bridge(winapk::GuestLinker& linker, const char* file) {
    std::printf("%s\n", file);
    std::string error;
    void* lib = linker.open(file, winapk::GuestLinker::kRtldNow, &error);
    check("open (APS2 + RELR relocations, stack protector)", lib != nullptr, error);
    if (!lib) return;
    auto fn = [&](const char* name) { return sym(linker, lib, name); };
    // For functions returning int: AAPCS64 leaves the upper half of x0 undefined.
    auto call0 = [&](const char* name) -> int {
        void* f = fn(name);
        return f ? static_cast<int>(winapk::arm64::call(f)) : -999999;
    };

    check("constructor ran through the interpreter", call0("arm64_ctor_ran") == 0x5150);
    {
        const uint64_t seed = 3;
        int want = 0;
        for (int i = 0; i < 100; ++i) want += static_cast<signed char>(seed + i);
        check("stack-protected function (guard in TPIDR_EL0[5])",
              static_cast<int>(winapk::arm64::call(fn("arm64_stack_guard"), &seed, 1)) == want);
    }
    check("libc through the shim (malloc, memcpy, strlen, strcmp, errno)", call0("arm64_libc") == 36);

    char buf[256] = {};
    uint64_t args[2] = {reinterpret_cast<uint64_t>(buf), sizeof buf};
    winapk::arm64::call(fn("arm64_snprintf"), args, 2);
    const char* want_printf = "1 two 3 4.50 5 6 7f 8 9.3 10 eleven 12";
    check("variadic snprintf with stack arguments", std::strcmp(buf, want_printf) == 0, buf);
    std::memset(buf, 0, sizeof buf);
    winapk::arm64::call(fn("arm64_vsnprintf"), args, 2);
    const char* want_v = "va|1|2.500|3|4|5|6|7|8|9.5|end";
    check("the guest's AAPCS64 va_list given to vsnprintf", std::strcmp(buf, want_v) == 0, buf);
    check("__android_log_print", call0("arm64_log_print") > 0);

    check("guest comparator called by the host's qsort", call0("arm64_qsort") == -3912);
    check("guest -> host -> guest -> host (qsort comparator calling snprintf)", call0("arm64_nested") == 10004);
    int64_t want_threads = 0;
    for (int64_t n = 100; n < 104; ++n) want_threads += n * (n + 1) * (2 * n + 1) / 6;
    const int64_t threads = static_cast<int64_t>(winapk::arm64::call(fn("arm64_threads")));
    check("pthread_create with a guest start routine, four threads", threads == want_threads,
          std::to_string(threads));

    using ManyFn = int64_t(WINAPK_GUEST_ABI*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                               int64_t, int64_t, double, double);
    if (auto* many = reinterpret_cast<ManyFn>(fn("arm64_many_args"))) {
        check("host -> guest: 10 integer + 2 double arguments", many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0.5, 0.25) == 685);
    }
    using ScaleFn = double(WINAPK_GUEST_ABI*)(double, float, int);
    if (auto* scale = reinterpret_cast<ScaleFn>(fn("arm64_scale"))) {
        check("host -> guest: double and float arguments, double result", scale(2.5, 4.0f, 3) == 30.0);
    }
    const uint64_t host_fn = reinterpret_cast<uint64_t>(&host_many_args);
    check("guest -> host: 10 integer + 2 double arguments",
          static_cast<int64_t>(winapk::arm64::call(fn("arm64_call_host"), &host_fn, 1)) == 685);
    using ParseFn = double(WINAPK_GUEST_ABI*)(const char*);
    if (auto* parse = reinterpret_cast<ParseFn>(fn("arm64_parse"))) {
        check("double result from the host (strtod) back to the host", parse("1.25") == 2.5);
    }

    check("setjmp/longjmp across three guest frames", call0("arm64_setjmp") == 421239,
          std::to_string(call0("arm64_setjmp")));

    const int exits_before = call0("arm64_exit_count");
    check("__cxa_atexit with a guest handler", call0("arm64_register_atexit") == 0);
    winapk::mini_bionic_run_atexit();
    check("the handler ran from the host's atexit list", call0("arm64_exit_count") == exits_before + 7);
}

// The benchmarks of arm64_bench.c natively and on each engine. The results
// must agree; the times are informational (no threshold: CI machines vary too
// much). For each benchmark:
//
//   * native and the JIT, warm (best of three runs), and their ratio. The
//     interpreter, about 80 times slower, runs 1/kInterpScale of the
//     iterations and is checked for its result only.
//   * first: how much longer the JIT's first call takes than a warm one, on
//     code it has not seen: mostly translation, the cost that translating at
//     install time would take off the app's start.
//   * where the JIT's time goes, from a profiled run: host functions versus
//     arm64 code, and the transitions and fallbacks per run.
constexpr uint64_t kInterpScale = 32;

double time_ms(const std::function<uint64_t()>& body, uint64_t* result) {
    const auto t0 = std::chrono::steady_clock::now();
    *result = body();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double best_ms(const std::function<uint64_t()>& body, uint64_t* result) {
    double best = 0;
    for (int i = 0; i < 3; ++i) {
        const double ms = time_ms(body, result);
        if (i == 0 || ms < best) best = ms;
    }
    return best;
}

void run_speed(winapk::GuestLinker& linker,
               const std::vector<std::pair<winapk::arm64::Engine, const char*>>& engines) {
    using winapk::arm64::Engine;
    std::printf("\n==== speed (libwinapk_arm64_bench.so, -O2; ratios to native) ====\n");
    std::string error;
    void* lib = linker.open("libwinapk_arm64_bench.so", winapk::GuestLinker::kRtldNow, &error);
    check("open libwinapk_arm64_bench.so", lib != nullptr, error);
    if (!lib) return;
    struct Bench {
        const char* name;
        IsaFn native;
        uint64_t iterations;
    };
#define BENCH_CASE(name, n) {#name, &name, n},
    static const Bench kBenches[] = {BENCHES(BENCH_CASE)};
#undef BENCH_CASE
    size_t results = 0, results_ok = 0;
    const Engine last = engines.back().first;
    std::printf("  %-10s %9s", "", "native");
    for (const auto& e : engines) {
        if (e.first != Engine::kInterpreter) std::printf(" %15s", e.second);
    }
    if (last != Engine::kInterpreter) std::printf(" %9s  where the %s's time goes", "first", engines.back().second);
    std::printf("\n");
    for (const Bench& b : kBenches) {
        void* fn = sym(linker, lib, b.name);
        if (!fn) continue;
        const uint64_t seed = 12345;
        uint64_t want = 0;
        const double native_ms = best_ms([&] { return b.native(b.iterations, seed, 0); }, &want);
        const uint64_t want_scaled = b.native(b.iterations / kInterpScale, seed, 0);
        std::printf("  %-10s %7.1fms", b.name + 6, native_ms);
        double first_ms = 0;
        std::vector<bool> matches;
        for (const auto& e : engines) {
            winapk::arm64::set_engine(e.first);
            uint64_t got = 0;
            if (e.first == Engine::kInterpreter) {
                const uint64_t args[3] = {b.iterations / kInterpScale, seed, 0};
                got = winapk::arm64::call(fn, args, 3);
                matches.push_back(got == want_scaled);
                continue;
            }
            const uint64_t args[3] = {b.iterations, seed, 0};
            const double cold = time_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            const double ms = best_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            first_ms = cold - ms;
            std::printf(" %7.1fms %5.1fx", ms, native_ms > 0 ? ms / native_ms : 0.0);
            matches.push_back(got == want);
        }
        if (last != Engine::kInterpreter) {
            winapk::arm64::set_profiling(true);
            const winapk::arm64::Stats s0 = winapk::arm64::stats();
            const uint64_t args[3] = {b.iterations, seed, 0};
            uint64_t got = 0;
            time_ms([&] { return winapk::arm64::call(fn, args, 3); }, &got);
            const winapk::arm64::Stats s1 = winapk::arm64::stats();
            winapk::arm64::set_profiling(false);
            const double run = static_cast<double>(s1.run_ns - s0.run_ns);
            const double host = static_cast<double>(s1.host_ns - s0.host_ns);
            std::printf(" %7.2fms  host %4.1f%%, %llu host calls, %llu callbacks, %llu exits, %llu interpreted",
                        first_ms, run > 0 ? 100 * host / run : 0.0,
                        static_cast<unsigned long long>(s1.host_calls - s0.host_calls),
                        static_cast<unsigned long long>(s1.guest_entries - s0.guest_entries - 1),
                        static_cast<unsigned long long>(s1.jit_exits - s0.jit_exits),
                        static_cast<unsigned long long>(s1.interp_insns - s0.interp_insns));
            if (last == Engine::kAot) {
                std::printf(", AOT %llu entries %llu bails",
                            static_cast<unsigned long long>(s1.aot_entries - s0.aot_entries),
                            static_cast<unsigned long long>(s1.aot_bails - s0.aot_bails));
            }
        }
        std::printf("\n");
        for (size_t i = 0; i < engines.size(); ++i) {
            if (!matches[i])
                check((std::string(b.name) + " on the " + engines[i].second + " matches native").c_str(), false);
        }
        results_ok += std::count(matches.begin(), matches.end(), true);
        results += matches.size();
    }
    check("every benchmark result matches native", results_ok == results,
          std::to_string(results_ok) + " of " + std::to_string(results));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: winapk-arm64-test <dir with libwinapk_arm64_*.so>\n");
        return 2;
    }
    winapk::LinkerConfig config;
    config.search_paths.push_back(std::filesystem::u8path(argv[1]));
    config.host = winapk::mini_bionic_resolve;
    winapk::GuestLinker linker(config);

    // Every check on each engine the build has: the interpreter, the JIT
    // (dynarmic) when it is built in, and the code translated ahead of time
    // (the <lib>.so.aot.dll files next to the libraries, when present).
    std::vector<std::pair<winapk::arm64::Engine, const char*>> engines = {
        {winapk::arm64::Engine::kInterpreter, "interpreter"}};
    if (winapk::arm64::set_engine(winapk::arm64::Engine::kJit)) {
        engines.push_back({winapk::arm64::Engine::kJit, "JIT"});
    } else {
        std::printf("(this build has no JIT: -DWINAPK_ARM64_JIT=ON adds it)\n");
    }
    engines.push_back({winapk::arm64::Engine::kAot, "AOT"});
    for (const auto& e : engines) {
        winapk::arm64::set_engine(e.first);
        std::printf("\n==== Win.apk arm64, %s ====\n", e.second);
        run_isa(linker, "libwinapk_arm64_isa_O2.so");
        run_isa(linker, "libwinapk_arm64_isa_O3.so");
        run_isa(linker, "libwinapk_arm64_isa_lse.so");
        run_bridge(linker, "libwinapk_arm64_bridge.so");
    }
    run_speed(linker, engines);

    std::printf("\n%s: %d failure(s)\n", g_failures ? "FAILED" : "OK", g_failures);
    return g_failures;
}
