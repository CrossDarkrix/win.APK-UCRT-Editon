// arm64_cpu.h -- an AArch64 (A64) user-mode CPU for running Android arm64-v8a
// code on an x64 host. Milestone M10, stage 1 (see PORTING.md).
//
// This is an interpreter: it decodes and executes one instruction at a time.
// It is the correctness baseline the ARM track is built on, not the final
// engine: a JIT (dynarmic, 0BSD) can replace run() later without changing the
// state layout below or anything in arm64_guest.h.
//
// Guest and host share one address space. Android arm64 and Windows x64 are
// both LP64 and little-endian, so a guest pointer is a host pointer and guest
// loads and stores are plain host memory accesses. That is what makes it
// possible to pass pointers between guest code and the Bionic shim unchanged.
//
// What is implemented (and checked by test/arm64_test.cpp against the same C
// code compiled for the host):
//   * the whole A64 base integer ISA: arithmetic, logic, bitfields, shifts,
//     multiply/divide, conditional select/compare, CRC32;
//   * branches, including the PAC/BTI hint forms (executed as plain branches
//     and NOPs: there is nothing to authenticate against);
//   * every load/store addressing mode, pairs, literals, exclusives
//     (LDXR/STXR as compare-and-swap), acquire/release and the LSE atomics;
//   * scalar floating point (single, double, conversions to and from integers
//     and half precision);
//   * the Advanced SIMD (NEON) forms compilers emit for ordinary C/C++:
//     vector integer and float arithmetic, compares, shifts, widen/narrow,
//     permutes, table lookups, reductions, loads/stores of 1-4 structures.
//
// An instruction outside that set stops the guest with its encoding and
// address (Hooks::fault), so the next one to add is always named.

#pragma once

#include <cstddef>
#include <cstdint>

namespace winapk::arm64 {

union alignas(16) VReg {
    uint8_t b[16];
    uint16_t h[8];
    uint32_t s[4];
    uint64_t d[2];
    float f[4];
    double df[2];
};

struct Cpu {
    uint64_t x[31] = {};  // x0..x30 (x30 is the link register)
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint32_t nzcv = 0;    // PSTATE.NZCV in bits 31..28, as MRS NZCV reads it
    uint32_t fpcr = 0;
    uint32_t fpsr = 0;
    uint64_t tpidr = 0;   // TPIDR_EL0: bionic's TLS slot array
    VReg v[32] = {};

    // The exclusive monitor (LDXR/STXR). A store-exclusive succeeds if memory
    // still holds what the load-exclusive read, which is how every
    // translator emulates the monitor on a host without one.
    bool excl_valid = false;
    uint64_t excl_addr = 0;
    unsigned excl_size = 0;  // bytes, 1..16
    uint64_t excl_val[2] = {};

    // run() returns when pc reaches this address. The guest bridge points the
    // link register of every call it makes at it.
    uint64_t stop_pc = 0;
    void* user = nullptr;
};

// A function translated ahead of time (arm64_aot.h).
using AotFn = uint32_t (*)(Cpu* c);

struct Hooks {
    // The executable guest range containing `pc`, or false if `pc` is not
    // guest code. run() caches the range, so this is called once per jump
    // into a different range, not once per instruction.
    bool (*code_range)(uint64_t pc, uint64_t* lo, uint64_t* hi) = nullptr;
    // The guest branched to an address outside guest code: a host function.
    // Called with c.pc == target; it makes the call and sets c.pc to where the
    // guest continues (normally c.x[30]).
    void (*host_call)(Cpu& c, uint64_t target) = nullptr;
    // Whether host_call(c, target) needs more of `c` than x0-x7, x30, sp and
    // v0-v7, or changes more than x0, x1, v0, v1 and pc (setjmp, longjmp).
    // The JIT makes the other host calls from inside its generated code with
    // just those registers, instead of stopping and copying the whole state.
    bool (*full_state_call)(uint64_t target) = nullptr;
    // The translated function at `pc`, when AOT code is in use (arm64_aot.h).
    // The JIT runs it in place of translating that code itself.
    AotFn (*aot_entry)(uint64_t pc) = nullptr;
    // A host function whose arguments the bridge passes as they are (no
    // va_list, no guest callback to wrap, not setjmp/longjmp): translated
    // code calls it without going through host_call.
    bool (*plain_host)(uint64_t target) = nullptr;
    // SVC #imm. Android apps do not make raw system calls, but libraries
    // that inline a few (Unity's, some anti-tamper code) exist.
    void (*svc)(Cpu& c, uint32_t imm) = nullptr;
    // An undefined or unimplemented instruction, or a BRK. Must not return
    // (the interpreter aborts if it does).
    void (*fault)(Cpu& c, uint32_t insn, const char* what) = nullptr;
};

// Installed once, before the first run().
void set_hooks(const Hooks& hooks);
const Hooks& hooks();

// Executes from c.pc until c.pc == c.stop_pc.
void run(Cpu& c);

// Executes exactly one instruction at c.pc (which must be guest code).
void step(Cpu& c);

// Internal to the interpreter: the FP/SIMD half (arm64_simd.cpp). Returns
// false for an encoding it does not implement.
bool exec_simd(Cpu& c, uint32_t insn);
bool exec_simd_ldst(Cpu& c, uint32_t insn);

}  // namespace winapk::arm64
