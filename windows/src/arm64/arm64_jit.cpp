// arm64_jit.cpp -- the arm64 JIT on dynarmic. See arm64_jit.h.
//
// Guest and host share the address space, so dynarmic's fastmem runs with a
// base of 0 and all 64 address bits: a guest load or store is one host load
// or store in the generated code. A fault there (a bad guest pointer) makes
// dynarmic recompile the block with the memory callbacks below, which then
// fault in the host as the interpreter would.
//
// When the guest branches to an address outside registered guest code, the
// code fetch fails (MemoryReadCode returns nothing) and dynarmic raises
// NoExecuteFault. That ends an empty block, so dynarmic's copy of the guest
// registers is complete at that point: the callback makes the host call
// right there, through the same Hooks::host_call the interpreter uses, with
// only the argument registers copied out and the results back, and sets the
// PC the block continues at (the return address). Only setjmp, longjmp and
// the return to the bridge (the return address it gives every guest call,
// which is how run() stops) halt execution and go through run() with the
// whole state.

#include "arm64_jit.h"

#ifdef WINAPK_HAVE_DYNARMIC

#include "arm64_internal.h"
#include "winapk/arm64_aot.h"

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <cstring>
#include <mutex>
#include <vector>

namespace winapk::arm64 {

namespace {

using Dynarmic::A64::Exception;
using Dynarmic::A64::VAddr;
using Dynarmic::A64::Vector;

// dynarmic implements LDXR/STXR through a monitor shared by every JIT, each
// with its own processor id. Ids are handed out per thread and reused after
// the thread exits; a thread that finds none left interprets.
constexpr size_t kMonitorSlots = 256;

Dynarmic::ExclusiveMonitor& monitor() {
    static Dynarmic::ExclusiveMonitor m(kMonitorSlots);
    return m;
}

std::mutex g_slots_lock;
std::vector<bool> g_slots_used(kMonitorSlots, false);

int take_slot() {
    std::lock_guard<std::mutex> hold(g_slots_lock);
    for (size_t i = 0; i < kMonitorSlots; ++i) {
        if (!g_slots_used[i]) {
            g_slots_used[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}

void give_slot(int slot) {
    std::lock_guard<std::mutex> hold(g_slots_lock);
    g_slots_used[static_cast<size_t>(slot)] = false;
}

// Host calls from inside the generated code: on unless
// WINAPK_ARM64_JIT_INPLACE=0, which halts the JIT for every host call and
// goes through run() with the whole state. Each way has an app that fails
// and the other way runs (WhatsApp needs in place, Hill Climb Racing ran with
// halting), one run each: an open bug, perhaps a race, not yet told apart.
// Differences worth knowing: in place, the host function runs with the
// guest's MXCSR (dynarmic switches it only on leaving Run) and with JIT
// frames, which have no unwind data, on the stack.
bool in_place_calls() {
    static const bool on = [] {
        const char* v = std::getenv("WINAPK_ARM64_JIT_INPLACE");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

bool exact_nans() {
    static const bool exact = [] {
        const char* v = std::getenv("WINAPK_ARM64_FP");
        return v && std::strcmp(v, "exact") == 0;
    }();
    return exact;
}

template <typename T> T load(VAddr a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}
template <typename T> void store(VAddr a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

template <typename T> bool cas(VAddr a, T desired, T expected) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(a), &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

class DynarmicEngine final : public JitEngine, private Dynarmic::A64::UserCallbacks {
public:
    DynarmicEngine(Cpu& c, int slot, size_t code_cache_size) : cpu_(c), slot_(slot) {
        Dynarmic::A64::UserConfig conf;
        conf.callbacks = this;
        conf.processor_id = static_cast<size_t>(slot);
        conf.global_monitor = &monitor();
        conf.tpidr_el0 = &c.tpidr;
        conf.tpidrro_el0 = &kZero;
        conf.fastmem_pointer = uintptr_t{0};
        conf.fastmem_address_space_bits = 64;
        conf.silently_mirror_fastmem = false;
        conf.recompile_on_fastmem_failure = true;
        conf.fastmem_exclusive_access = true;
        conf.recompile_on_exclusive_fastmem_failure = true;
        conf.enable_cycle_counting = false;
        conf.hook_data_cache_operations = true;
        conf.define_unpredictable_behaviour = true;
        conf.cntfrq_el0 = 1000000000;  // CNTVCT_EL0 counts nanoseconds, as in the interpreter
        conf.code_cache_size = code_cache_size;
        // A NaN result may be x86's NaN rather than the one Arm would give
        // (payload and sign): every other result is the same, and dynarmic
        // then skips the NaN fix-up after each floating-point instruction
        // and uses the host's FMA as it is. WINAPK_ARM64_FP=exact keeps
        // Arm's NaNs.
        if (!exact_nans()) {
            conf.unsafe_optimizations = true;
            conf.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        jit_.reset(new Dynarmic::A64::Jit(conf));
    }

    ~DynarmicEngine() override {
        jit_.reset();
        give_slot(slot_);
    }

    void run(Cpu& c) override {
        load_state(c);
        for (;;) {
            pending_ = Pending::kNone;
            jit_->Run();
            ++exits;
            jit_->ClearHalt(kHalt);
            save_state(c);
            switch (pending_) {
                case Pending::kHostCall:
                    c.pc = target_;
                    if (target_ == c.stop_pc) return;
                    hooks().host_call(c, target_);
                    if (c.pc == c.stop_pc) return;  // longjmp or a tail call home
                    break;
                case Pending::kInterpret:
                    c.pc = target_;
                    for (size_t i = 0; i < count_; ++i) {
                        uint64_t lo, hi;
                        if (c.pc == c.stop_pc || !hooks().code_range(c.pc, &lo, &hi)) break;
                        step(c);
                        ++interp_insns;
                    }
                    if (c.pc == c.stop_pc) return;
                    break;
                case Pending::kSvc:
                    hooks().svc(c, svc_);
                    break;
                case Pending::kNone:
                    break;  // a halt dynarmic asked for itself (cache invalidation)
            }
            load_state(c);
        }
    }

private:
    enum class Pending { kNone, kHostCall, kInterpret, kSvc };
    static constexpr Dynarmic::HaltReason kHalt = Dynarmic::HaltReason::UserDefined1;
    static const uint64_t kZero;

    void load_state(const Cpu& c) {
        std::array<uint64_t, 31> x;
        std::memcpy(x.data(), c.x, sizeof(c.x));
        jit_->SetRegisters(x);
        jit_->SetSP(c.sp);
        jit_->SetPC(c.pc);
        jit_->SetPstate(c.nzcv);
        jit_->SetFpcr(c.fpcr);
        jit_->SetFpsr(c.fpsr);
        std::array<Vector, 32> v;
        for (size_t i = 0; i < 32; ++i) v[i] = {c.v[i].d[0], c.v[i].d[1]};
        jit_->SetVectors(v);
        jit_->ClearExclusiveState();
    }

    void save_state(Cpu& c) const {
        const std::array<uint64_t, 31> x = jit_->GetRegisters();
        std::memcpy(c.x, x.data(), sizeof(c.x));
        c.sp = jit_->GetSP();
        c.pc = jit_->GetPC();
        c.nzcv = jit_->GetPstate() & 0xf0000000;
        c.fpcr = jit_->GetFpcr();
        c.fpsr = jit_->GetFpsr();
        const std::array<Vector, 32> v = jit_->GetVectors();
        for (size_t i = 0; i < 32; ++i) {
            c.v[i].d[0] = v[i][0];
            c.v[i].d[1] = v[i][1];
        }
        c.excl_valid = false;
    }

    // A host call from inside the generated code (see the top of this file).
    // Guest code the host function calls back into runs on another engine.
    void host_call_in_place(VAddr target) {
        Cpu& c = cpu_;
        for (size_t i = 0; i < 8; ++i) {
            c.x[i] = jit_->GetRegister(i);
            const Vector v = jit_->GetVector(i);
            c.v[i].d[0] = v[0];
            c.v[i].d[1] = v[1];
        }
        c.x[30] = jit_->GetRegister(30);
        c.sp = jit_->GetSP();
        c.pc = target;
        hooks().host_call(c, target);
        jit_->SetRegister(0, c.x[0]);
        jit_->SetRegister(1, c.x[1]);
        jit_->SetVector(0, {c.v[0].d[0], c.v[0].d[1]});
        jit_->SetVector(1, {c.v[1].d[0], c.v[1].d[1]});
        jit_->SetPC(c.pc);
    }

    void halt(Pending p, uint64_t target, size_t count = 1) {
        pending_ = p;
        target_ = target;
        count_ = count;
        jit_->HaltExecution(kHalt);
    }

    // ---- dynarmic's callbacks ----

    std::optional<uint32_t> MemoryReadCode(VAddr vaddr) override {
        // Code translated at install time is not translated again: the
        // fetch fails, and ExceptionRaised runs the translation.
        if (hooks().aot_entry && hooks().aot_entry(vaddr)) return std::nullopt;
        uint64_t lo, hi;
        if (!hooks().code_range(vaddr, &lo, &hi) || vaddr + 4 > hi) return std::nullopt;
        // Each block is fetched once, when dynarmic compiles it: the code
        // that runs, for the translation's profile (arm64_aot.h, "Profiles").
        aot_profile_hit(vaddr);
        return load<uint32_t>(vaddr);
    }

    uint8_t MemoryRead8(VAddr a) override { return load<uint8_t>(a); }
    uint16_t MemoryRead16(VAddr a) override { return load<uint16_t>(a); }
    uint32_t MemoryRead32(VAddr a) override { return load<uint32_t>(a); }
    uint64_t MemoryRead64(VAddr a) override { return load<uint64_t>(a); }
    Vector MemoryRead128(VAddr a) override { return {load<uint64_t>(a), load<uint64_t>(a + 8)}; }
    void MemoryWrite8(VAddr a, uint8_t v) override { store(a, v); }
    void MemoryWrite16(VAddr a, uint16_t v) override { store(a, v); }
    void MemoryWrite32(VAddr a, uint32_t v) override { store(a, v); }
    void MemoryWrite64(VAddr a, uint64_t v) override { store(a, v); }
    void MemoryWrite128(VAddr a, Vector v) override {
        store(a, v[0]);
        store(a + 8, v[1]);
    }

    bool MemoryWriteExclusive8(VAddr a, uint8_t v, uint8_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(VAddr a, uint16_t v, uint16_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(VAddr a, uint32_t v, uint32_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(VAddr a, uint64_t v, uint64_t e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(VAddr a, Vector v, Vector e) override {
        unsigned __int128 expected = (static_cast<unsigned __int128>(e[1]) << 64) | e[0];
        const unsigned __int128 desired = (static_cast<unsigned __int128>(v[1]) << 64) | v[0];
        return __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128*>(a), expected, desired);
    }

    void InterpreterFallback(VAddr pc, size_t num_instructions) override {
        halt(Pending::kInterpret, pc, num_instructions);
    }

    void CallSVC(uint32_t swi) override {
        svc_ = swi;
        halt(Pending::kSvc, 0);
    }

    void ExceptionRaised(VAddr pc, Exception exception) override {
        switch (exception) {
            case Exception::NoExecuteFault:
                if (AotFn fn = pc != cpu_.stop_pc && hooks().aot_entry ? hooks().aot_entry(pc) : nullptr) {
                    // Into a translated function, with the whole state: it
                    // leaves c.pc where the JIT goes on (its return address,
                    // or where it stopped).
                    save_state(cpu_);
                    cpu_.pc = pc;
                    fn(&cpu_);
                    load_state(cpu_);
                } else if (in_place_calls() && pc != cpu_.stop_pc && !(hooks().full_state_call && hooks().full_state_call(pc))) {
                    host_call_in_place(pc);
                } else {
                    halt(Pending::kHostCall, pc);
                }
                break;
            case Exception::Breakpoint: {
                Cpu c;
                c.pc = pc;
                if (hooks().fault) hooks().fault(c, load<uint32_t>(pc), "BRK (the guest aborted or hit a trap)");
                std::abort();
            }
            case Exception::UnallocatedEncoding:
            case Exception::ReservedValue:
            case Exception::UnpredictableInstruction:
                // Not translated: the interpreter executes it, or names it.
                halt(Pending::kInterpret, pc, 1);
                break;
            default:
                break;  // WFE, SEV, YIELD, ...: hints, nothing to do
        }
    }

    void DataCacheOperationRaised(Dynarmic::A64::DataCacheOperation op, VAddr value) override {
        if (op == Dynarmic::A64::DataCacheOperation::ZeroByVA) {
            std::memset(reinterpret_cast<void*>(value & ~VAddr{63}), 0, 64);
        }
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, VAddr value) override {
        // Guest code that writes code (a JIT of its own) says so with IC IVAU.
        if (op == Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU) {
            jit_->InvalidateCacheRange(value & ~VAddr{63}, 64);
        } else {
            jit_->ClearCache();
        }
    }

    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return ~uint64_t{0}; }
    uint64_t GetCNTPCT() override {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    std::unique_ptr<Dynarmic::A64::Jit> jit_;
    Cpu& cpu_;
    int slot_;
    Pending pending_ = Pending::kNone;
    uint64_t target_ = 0;
    size_t count_ = 0;
    uint32_t svc_ = 0;
};

const uint64_t DynarmicEngine::kZero = 0;

}  // namespace

std::unique_ptr<JitEngine> make_jit_engine(Cpu& c, bool outermost) {
    const int slot = take_slot();
    if (slot < 0) return nullptr;
    const size_t cache = (outermost ? 64 : 16) * size_t{1024 * 1024};
    return std::unique_ptr<JitEngine>(new DynarmicEngine(c, slot, cache));
}

bool jit_built() { return true; }

}  // namespace winapk::arm64

#else  // !WINAPK_HAVE_DYNARMIC

namespace winapk::arm64 {

std::unique_ptr<JitEngine> make_jit_engine(Cpu&, bool) { return nullptr; }
bool jit_built() { return false; }

}  // namespace winapk::arm64

#endif
