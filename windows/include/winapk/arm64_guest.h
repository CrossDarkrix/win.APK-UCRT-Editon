// arm64_guest.h -- running Android arm64-v8a code inside the Windows x64 host
// (milestone M10, stage 1). The CPU itself is in arm64_cpu.h; this is the
// bridge around it.
//
// Where guest code enters the system (PORTING.md, D8) nothing changes for the
// rest of the runtime: ElfImage loads an arm64 library like an x86_64 one,
// GuestLinker links it, and every call between the two worlds keeps the System
// V convention the x86_64 path already uses. The bridge converts at the edge:
//
//   * guest -> host: an arm64 library's imports are bound to the same System V
//     host functions an x86_64 library gets (the Bionic shim, libdl, GL, ...).
//     When the interpreter branches to an address that is not arm64 code, it
//     calls that address as a System V function: x0-x5 go to rdi..r9, x6, x7
//     and the guest's stack arguments to the host stack, v0-v7 to xmm0-xmm7
//     (with al = 8 for variadic callees); rax/rdx/xmm0 come back as x0/x1/v0.
//     That covers every function whose arguments are scalars or pointers,
//     variadic ones included. The one argument type the two conventions lay
//     out differently is va_list: functions that take one (vsnprintf, the JNI
//     ...V forms) are registered with register_va_list_function and get the
//     guest's AAPCS64 va_list rewritten as a System V one.
//
//   * host -> guest: arm64 code is mapped without execute permission, so a
//     host call into it (a callback the guest registered: qsort comparators,
//     pthread_create start routines, JNI natives, JNI_OnLoad, atexit
//     handlers, ...) raises an execute fault. A vectored exception handler
//     turns that fault into a call through the interpreter, with the System V
//     arguments moved into AAPCS64 registers. No callback needs to be known in
//     advance. host_callable() gives the same entry without the fault, for hot
//     paths such as native-method thunks.
//
// Each host thread that runs guest code gets its own CPU state, a 4 MiB guest
// stack and bionic's TLS slot array (TPIDR_EL0, with the stack guard in slot 5),
// created on first use and freed when the thread exits. Guest -> host -> guest
// nesting (a qsort comparator inside qsort inside guest code) continues on the
// same guest stack.
//
// setjmp and longjmp are the exception to "every import is a host function":
// they save and restore guest registers, so the bridge implements them itself
// (guest_intrinsic).
//
// Not supported yet: arguments or results passed by value as structs larger
// than 16 bytes or as floating-point aggregates (the conventions disagree;
// libc and JNI have none), raw system calls (SVC), and ELF TLS through TLSDESC
// (apps built for API 29+ that use thread_local; emulated TLS works).

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk::arm64 {

// Installs the interpreter hooks and the exception handler. Called by the
// loader before the first arm64 image runs; safe to call more than once.
void init();

// The executable segments of a loaded arm64 image. The interpreter runs only
// inside registered ranges; everything else is a host call.
void register_code(const void* start, size_t size);
void unregister_code(const void* start);
bool is_guest_code(const void* p);

// Calls the guest function at `fn` on the calling thread with up to eight
// integer or pointer arguments, and returns x0.
uint64_t call(const void* fn, const uint64_t* args = nullptr, size_t nargs = 0);

// A System V entry point for the guest function at `fn` (cached per address),
// or `fn` itself if it is not arm64 code.
void* host_callable(void* fn);

// `host_fn` takes a va_list as argument number `index` (0-based); calls from
// guest code get it converted from AAPCS64 to System V.
void register_va_list_function(const void* host_fn, unsigned index);

// The engine that runs guest code. kAot, the default: the code translated at
// install time (arm64_aot.h) where a library has it, and for the rest the JIT
// (dynarmic, arm64_jit.h) when the build has it, else the interpreter.
// WINAPK_ARM64_ENGINE=interp, =jit or =aot chooses. set_engine returns false
// if the JIT is not built in. Threads pick the engine up the next time they
// enter guest code; code the JIT has already translated stays as it is.
enum class Engine { kInterpreter, kJit, kAot };
bool set_engine(Engine e);
Engine engine();

// Where guest time goes: totals for threads that have exited plus the
// calling thread. The counts are always kept; the times only while profiling
// is on (set_profiling, or WINAPK_ARM64_STATS=1, which also prints these
// totals to stderr when the process exits).
struct Stats {
    uint64_t guest_entries = 0;  // host -> guest calls (call(), callbacks, JNI natives)
    uint64_t host_calls = 0;     // guest -> host calls
    uint64_t jit_exits = 0;      // returns from JIT code to its run loop
    uint64_t interp_insns = 0;   // instructions the JIT handed to the interpreter
    uint64_t aot_entries = 0;    // guest calls that started in translated code
    uint64_t aot_bails = 0;      // ... and went on in the JIT or the interpreter
    uint64_t run_ns = 0;         // time in outermost guest calls, host calls included
    uint64_t host_ns = 0;        // time in host functions, guest callbacks they make excluded
};
Stats stats();
void set_profiling(bool on);

// What an arm64 library's import of `name` binds to when no host function
// can stand in for it, because it works on the caller's registers (setjmp,
// longjmp and their variants); nullptr for every other name. The loader asks
// this before the linker's resolver.
void* guest_intrinsic(const char* name);

// What an arm64 library's import of `name` binds to instead of the Bionic
// shim's function, when arm64 lays out one of its structs differently from
// x86_64 (struct stat, struct epoll_event; arm64_abi.cpp); nullptr for every
// other name. Asked after guest_intrinsic.
void* abi_override(const char* name);

// A raw Linux system call (SVC #0) from arm64 code: number `nr` (x8),
// arguments x0-x5. False if it is not one the shim can answer; otherwise
// *result is what the kernel would return (-errno on failure).
bool linux_syscall(uint64_t nr, const uint64_t args[6], uint64_t* result);

}  // namespace winapk::arm64
