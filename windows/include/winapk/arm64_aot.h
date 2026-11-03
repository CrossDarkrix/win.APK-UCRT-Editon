// arm64_aot.h -- arm64 code translated ahead of time, at install time.
//
// winapk-aot (tools/arm64_aot_main.cpp) reads an arm64-v8a .so and writes C:
// one C function per guest function, with the guest registers as local
// variables, so the C compiler allocates registers and optimizes across a
// whole function (what a JIT translating block by block, with the registers
// in memory between blocks, cannot do). The installer compiles that C with
// clang (-O2, for the CPU it runs on) into `<lib>.so.aot.dll` next to the
// library. When the loader maps the library it loads the DLL, and the bridge
// runs translated functions natively.
//
// The translation never has to be complete. A translated function returns
// kAotDone when the guest function returns (c->pc is then its return
// address), or kAotBail with the whole guest state in *c and c->pc where
// execution continues: a branch to an address the translator could not
// follow, guest code outside the DLL, longjmp. The JIT then runs from c->pc,
// and everything above it in the guest call (the C frames of translated
// callers return kAotBail too, without touching *c). Instructions the
// translator does not emit natively go through the interpreter one at a time
// (AotRuntime::step), with the registers written back around them.
//
// Calls follow the AAPCS64: across a call, the translated caller keeps its
// own copies of the callee-saved registers and reloads only x0-x7 and
// v0-v7, which is what a compiled callee may change.
//
// Profiles. Compiling a whole large library takes clang most of an hour
// (libwaze.so: 1.5M instructions), most of it for code the app never runs.
// So, as ART does with its profile-guided compiles, nothing waits for the
// translation: the app starts on the JIT, the JIT records which code it
// compiled (aot_profile_hit), and that goes to `<lib>.so.aot.profile`
// (aot_profile_flush): a header line `winapk-aot-profile 1 <text_hash>`,
// then one library vaddr per line, in hex. A background translation
// (winapk-runtime --aot-translate) compiles a small library whole and a large
// one only where its profile says code ran, again as the profile grows. A
// translation finished while the app runs is loaded at once
// (aot_load_pending), for the functions not entered yet.

#pragma once

#include "winapk/arm64_cpu.h"

#include <cstddef>
#include <cstdint>

namespace winapk::arm64 {

constexpr uint32_t kAotVersion = 2;
constexpr uint32_t kAotDone = 0;
constexpr uint32_t kAotBail = 1;

// Filled in by the runtime before any translated code runs.
struct AotRuntime {
    // A call or jump to `target` (an address the translation does not know
    // statically: BLR, BR, a branch out of the library). c->x[30] is the
    // guest return address. Returns kAotDone when the call has returned.
    uint32_t (*branch)(Cpu* c, uint64_t target);
    // Executes the instruction at c->pc through the interpreter.
    void (*step)(Cpu* c);
    // Whether `target` is a host function translated code may call directly
    // through call_sysv: no va_list or guest callback among its arguments,
    // not setjmp/longjmp (Hooks::plain_host). Call sites cache the answer.
    uint32_t (*plain)(uint64_t target);
    // The bridge's System V call (winapk_arm64_call_sysv): the argument
    // registers and stack words in, rax/rdx/xmm0/xmm1 out.
    void(__attribute__((sysv_abi)) * call_sysv)(const void* fn, void* frame);
};

struct AotEntry {
    uint64_t vaddr;  // ELF virtual address of a translated function
    AotFn fn;
};

// The DLL's one export, `winapk_aot_module`.
struct AotModule {
    uint32_t version;
    uint32_t layout[4];    // sizeof(Cpu) prefix the code uses: offsets of sp, nzcv, tpidr, v
    uint64_t text_hash;    // FNV-1a of the library's executable segments (file bytes)
    uint64_t* base;        // the library's load bias, set by the runtime
    AotRuntime* runtime;   // set by the runtime
    const AotEntry* entries;
    uint64_t count;
};

// FNV-1a 64, as winapk-aot computes text_hash.
inline uint64_t aot_hash(const uint8_t* p, size_t n, uint64_t h = 0xcbf29ce484222325ull) {
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Loads `<so_path>.aot.dll` for an arm64 library mapped at `bias` whose
// executable segments hash to `text_hash`; false if there is none or it
// does not match (built for another version of the library or of Win.apk).
// [code_lo, code_hi) is the library's code, for the profile.
bool aot_load(const char* so_path, uint64_t bias, uint64_t text_hash, uint64_t code_lo, uint64_t code_hi);

// The JIT compiled guest code at `pc`: recorded for the library's profile.
void aot_profile_hit(uint64_t pc);

// Appends the addresses recorded since the last call to each library's
// profile. Returns how many profiles grew.
size_t aot_profile_flush();

// Loads translations a background translation finished since the libraries
// were mapped (`<lib>.so.aot.dll`, or `.aot.dll.new` while an older one is in
// use). Returns how many it loaded.
size_t aot_load_pending();

// The translated function at guest address `pc`, if any (and AOT is on).
AotFn aot_lookup(uint64_t pc);

// Runs a translated function on c (c->pc is its entry). kAotDone when the
// guest function returned; kAotBail to continue from c->pc.
uint32_t aot_run(AotFn fn, Cpu* c);

}  // namespace winapk::arm64
