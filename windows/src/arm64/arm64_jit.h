// arm64_jit.h -- the arm64 JIT: dynarmic (0BSD) translating A64 to x86-64,
// behind the same Cpu state and Hooks as the interpreter (arm64_cpu.h).
//
// Built only with -DWINAPK_ARM64_JIT=ON (CMakeLists.txt fetches dynarmic);
// otherwise make_jit_engine() returns nullptr and the interpreter runs
// everything. Even with the JIT, the interpreter stays: dynarmic hands it the
// odd instruction it does not translate (InterpreterFallback), and the
// differential test checks the JIT against it.

#pragma once

#include "winapk/arm64_cpu.h"

#include <memory>

namespace winapk::arm64 {

class JitEngine {
public:
    virtual ~JitEngine() = default;
    // Runs from c.pc until c.pc == c.stop_pc, like run(c): branches out of
    // guest code go to Hooks::host_call, SVC to Hooks::svc.
    virtual void run(Cpu& c) = 0;

    // For stats() (arm64_guest.h): returns from generated code to run() (host
    // calls, interpreter fallbacks, SVCs and dynarmic's own halts), and the
    // instructions handed to the interpreter.
    uint64_t exits = 0;
    uint64_t interp_insns = 0;
};

// A JIT for one host thread, whose TPIDR_EL0 is c.tpidr (read in place, so
// `c` must outlive the engine). nullptr if the JIT is not built in or all
// exclusive-monitor slots are taken; the caller then interprets.
//
// The engine makes host calls from inside its generated code (see
// Hooks::full_state_call), so guest code the host function calls back into
// must run on another engine: the bridge keeps one per nesting level. The
// first (`outermost`) gets the large code cache; the others run callbacks.
std::unique_ptr<JitEngine> make_jit_engine(Cpu& c, bool outermost = true);

// Whether this build has the JIT.
bool jit_built();

}  // namespace winapk::arm64
