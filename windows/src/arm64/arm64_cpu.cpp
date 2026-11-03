// arm64_cpu.cpp -- the A64 interpreter: integer, branch, system and memory
// instructions. FP and Advanced SIMD are in arm64_simd.cpp. See arm64_cpu.h.
//
// Decoding follows the Arm ARM's top-level tables (C4.1): bits 28..25 pick
// the group, then each group's own fields. Field names in the comments are the
// ones the Arm ARM uses, so every case can be checked against it directly.

#include "winapk/arm64_cpu.h"

#include "arm64_internal.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace winapk::arm64 {

namespace {

Hooks g_hooks;

// ---- flags and conditions ---------------------------------------------------

uint32_t pack_nzcv(bool n, bool z, bool c, bool v) {
    return (uint32_t(n) << 31) | (uint32_t(z) << 30) | (uint32_t(c) << 29) | (uint32_t(v) << 28);
}

// AddWithCarry() from the Arm ARM, for 32 or 64 bits.
uint64_t add_with_carry(uint64_t x, uint64_t y, bool carry, bool sf, uint32_t* nzcv) {
    if (sf) {
        const unsigned __int128 u = static_cast<unsigned __int128>(x) + y + carry;
        const uint64_t r = static_cast<uint64_t>(u);
        const __int128 s = static_cast<__int128>(static_cast<int64_t>(x)) + static_cast<int64_t>(y) + carry;
        if (nzcv) *nzcv = pack_nzcv(r >> 63, r == 0, (u >> 64) != 0, s != static_cast<int64_t>(r));
        return r;
    }
    const uint64_t u = uint64_t(uint32_t(x)) + uint32_t(y) + carry;
    const uint32_t r = static_cast<uint32_t>(u);
    const int64_t s = int64_t(int32_t(x)) + int32_t(y) + carry;
    if (nzcv) *nzcv = pack_nzcv(r >> 31, r == 0, (u >> 32) != 0, s != int32_t(r));
    return r;
}

uint32_t logic_flags(uint64_t r, bool sf) {
    return sf ? pack_nzcv(r >> 63, r == 0, false, false)
              : pack_nzcv((r >> 31) & 1, uint32_t(r) == 0, false, false);
}

uint64_t shift_reg(uint64_t v, unsigned type, unsigned amount, bool sf) {
    const unsigned size = sf ? 64 : 32;
    amount %= size;
    v &= mask(size);
    switch (type) {
        case 0: return (v << amount) & mask(size);                          // LSL
        case 1: return v >> amount;                                         // LSR
        case 2: return static_cast<uint64_t>(sext(v, size) >> amount) & mask(size);  // ASR
        default: return ror(v, amount, size);                               // ROR
    }
}

uint64_t extend_reg(uint64_t v, unsigned option, unsigned shift) {
    switch (option) {
        case 0: v = uint8_t(v); break;
        case 1: v = uint16_t(v); break;
        case 2: v = uint32_t(v); break;
        case 3: break;
        case 4: v = static_cast<uint64_t>(int64_t(int8_t(v))); break;
        case 5: v = static_cast<uint64_t>(int64_t(int16_t(v))); break;
        case 6: v = static_cast<uint64_t>(int64_t(int32_t(v))); break;
        default: break;
    }
    return v << shift;
}

// DecodeBitMasks() from the Arm ARM. False for a reserved encoding.
bool decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned datasize,
                      uint64_t* wmask, uint64_t* tmask) {
    const unsigned combined = (n << 6) | (~imms & 0x3f);
    if (combined == 0) return false;
    unsigned len = 31 - __builtin_clz(combined);
    if (len < 1) return false;
    const unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    const unsigned s = imms & levels, r = immr & levels;
    const unsigned esize = 1u << len;
    if (esize > datasize) return false;
    const unsigned d = (s - r) & levels;
    const uint64_t welem = mask(s + 1), telem = mask(d + 1);
    uint64_t w = ror(welem, r, esize), t = telem;
    for (unsigned i = esize; i < 64; i *= 2) {
        w |= w << i;
        t |= t << i;
    }
    *wmask = w & mask(datasize);
    *tmask = t & mask(datasize);
    return true;
}

[[noreturn]] void fail(Cpu& c, uint32_t insn, const char* what) {
    if (g_hooks.fault) g_hooks.fault(c, insn, what);
    std::abort();
}
[[noreturn]] void undefined(Cpu& c, uint32_t insn) { fail(c, insn, "unimplemented or undefined instruction"); }

// ---- data processing, immediate ---------------------------------------------

void dp_immediate(Cpu& c, uint32_t insn) {
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5);
    const bool sf = bit(insn, 31);
    switch (field(insn, 25, 23)) {
        case 0: case 1: {  // ADR, ADRP
            const int64_t imm = sext((field(insn, 23, 5) << 2) | field(insn, 30, 29), 21);
            if (bit(insn, 31)) {
                set_x(c, rd, (c.pc & ~uint64_t{0xfff}) + (static_cast<uint64_t>(imm) << 12));
            } else {
                set_x(c, rd, c.pc + static_cast<uint64_t>(imm));
            }
            break;
        }
        case 2: {  // ADD/ADDS/SUB/SUBS (immediate)
            const bool sub = bit(insn, 30), s = bit(insn, 29);
            uint64_t imm = field(insn, 21, 10);
            if (bit(insn, 22)) imm <<= 12;
            const uint64_t a = get_xsp(c, rn);
            uint32_t flags;
            const uint64_t r = add_with_carry(a, sub ? ~imm : imm, sub, sf, &flags);
            if (s) {
                c.nzcv = flags;
                set_x(c, rd, sf ? r : uint32_t(r));
            } else {
                set_xsp(c, rd, sf ? r : uint32_t(r));
            }
            break;
        }
        case 4: {  // AND/ORR/EOR/ANDS (immediate)
            uint64_t wmask, tmask;
            if (!sf && bit(insn, 22)) undefined(c, insn);
            if (!decode_bit_masks(bit(insn, 22), field(insn, 15, 10), field(insn, 21, 16), true, sf ? 64 : 32,
                                  &wmask, &tmask)) {
                undefined(c, insn);
            }
            const uint64_t a = get_x(c, rn);
            uint64_t r;
            switch (field(insn, 30, 29)) {
                case 0: r = a & wmask; break;
                case 1: r = a | wmask; break;
                case 2: r = a ^ wmask; break;
                default: r = a & wmask; break;
            }
            if (!sf) r = uint32_t(r);
            if (field(insn, 30, 29) == 3) {
                c.nzcv = logic_flags(r, sf);
                set_x(c, rd, r);
            } else {
                set_xsp(c, rd, r);
            }
            break;
        }
        case 5: {  // MOVN/MOVZ/MOVK
            const unsigned hw = field(insn, 22, 21);
            if (!sf && hw > 1) undefined(c, insn);
            const unsigned pos = hw * 16;
            const uint64_t imm = uint64_t(field(insn, 20, 5)) << pos;
            uint64_t r;
            switch (field(insn, 30, 29)) {
                case 0: r = ~imm; break;
                case 2: r = imm; break;
                case 3: r = (get_x(c, rd) & ~(uint64_t{0xffff} << pos)) | imm; break;
                default: undefined(c, insn);
            }
            set_x(c, rd, sf ? r : uint32_t(r));
            break;
        }
        case 6: {  // SBFM/BFM/UBFM
            const unsigned opc = field(insn, 30, 29);
            const unsigned size = sf ? 64 : 32;
            if (opc == 3 || bit(insn, 22) != sf) undefined(c, insn);
            const unsigned immr = field(insn, 21, 16), imms = field(insn, 15, 10);
            uint64_t wmask, tmask;
            if (!decode_bit_masks(bit(insn, 22), imms, immr, false, size, &wmask, &tmask)) undefined(c, insn);
            const uint64_t src = get_x(c, rn) & mask(size);
            const uint64_t bot_src = ror(src, immr, size);
            uint64_t r;
            if (opc == 1) {  // BFM
                const uint64_t dst = get_x(c, rd);
                const uint64_t bot = (dst & ~wmask) | (bot_src & wmask);
                r = (dst & ~tmask) | (bot & tmask);
            } else if (opc == 0) {  // SBFM
                const uint64_t bot = bot_src & wmask;
                const uint64_t top = ((src >> imms) & 1) ? mask(size) : 0;
                r = (top & ~tmask) | (bot & tmask);
            } else {  // UBFM
                r = bot_src & wmask & tmask;
            }
            set_x(c, rd, r & mask(size));
            break;
        }
        case 7: {  // EXTR
            const unsigned size = sf ? 64 : 32;
            if (bit(insn, 22) != sf || field(insn, 30, 29) != 0 || bit(insn, 21)) undefined(c, insn);
            const unsigned lsb = field(insn, 15, 10);
            if (lsb >= size) undefined(c, insn);
            // EXTR Rd, Rn, Rm, #lsb: bits lsb.. of the pair Rn:Rm (Rn high).
            const uint64_t hi = get_x(c, rn) & mask(size);
            const uint64_t lo = get_x(c, field(insn, 20, 16)) & mask(size);
            uint64_t r = lsb == 0 ? hi : ((lo >> lsb) | (hi << (size - lsb)));
            set_x(c, rd, r & mask(size));
            break;
        }
        default:
            undefined(c, insn);
    }
}

// ---- branches, exceptions, system -------------------------------------------


// The generic timer (CNTVCT_EL0): nanoseconds of a monotonic clock, with
// CNTFRQ_EL0 saying so.
uint64_t counter_frequency() { return 1000000000; }
uint64_t counter_ticks() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void system_insn(Cpu& c, uint32_t insn) {
    // HINT space: NOP, YIELD, WFE, WFI, SEV, PACIASP/AUTIASP, BTI, ... none of
    // them has an effect a user-mode interpreter has to model.
    if ((insn & 0xfffff01f) == 0xd503201f) return;
    // CLREX
    if ((insn & 0xfffff0ff) == 0xd503305f) {
        c.excl_valid = false;
        return;
    }
    // DSB / DMB / ISB. x86 is TSO: only an ordering that includes store->load
    // needs a fence (CRm<1:0> == 11 is the "full" domain variant).
    if ((insn & 0xfffff09f) == 0xd503309f) {
        const unsigned crm = field(insn, 11, 8);
        if (field(insn, 7, 5) != 6 && (crm & 3) == 3) __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return;
    }
    // MSR (immediate): PSTATE fields. Only the ones user code touches
    // (DAIF is privileged, SSBS/PAN/UAO/DIT/TCO are no-ops for us).
    if ((insn & 0xfff8f01f) == 0xd500401f) return;
    // SYS: cache maintenance (DC CVAU, IC IVAU, DC ZVA, ...).
    if ((insn & 0xfff80000) == 0xd5080000) {
        const unsigned op1 = field(insn, 18, 16), crn = field(insn, 15, 12), crm = field(insn, 11, 8),
                       op2 = field(insn, 7, 5);
        if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) {  // DC ZVA: zero 64 bytes
            const uint64_t a = get_x(c, field(insn, 4, 0)) & ~uint64_t{63};
            std::memset(reinterpret_cast<void*>(a), 0, 64);
        }
        // DC CVAU/CIVAC/CVAC and IC IVAU: the interpreter has no code cache
        // to invalidate (a JIT will have to act here).
        return;
    }
    // MRS / MSR (register)
    if ((insn & 0xffd00000) == 0xd5100000) {
        const bool read = bit(insn, 21);
        // Bits 20..5 are op0(2):op1(3):CRn(4):CRm(4):op2(3), the usual way
        // system registers are written down (op0 is 2 or 3 here).
        const unsigned sysreg = field(insn, 20, 5);
        const unsigned rt = field(insn, 4, 0);
        switch (sysreg) {
            case 0xde82:  // TPIDR_EL0 (3,3,13,0,2)
                if (read) set_x(c, rt, c.tpidr); else c.tpidr = get_x(c, rt);
                return;
            case 0xde83:  // TPIDRRO_EL0 (3,3,13,0,3): reads as 0 in user space on Linux
                if (read) set_x(c, rt, 0);
                return;
            case 0xda10:  // NZCV (3,3,4,2,0)
                if (read) set_x(c, rt, c.nzcv); else c.nzcv = uint32_t(get_x(c, rt)) & 0xf0000000;
                return;
            case 0xda20:  // FPCR (3,3,4,4,0)
                if (read) set_x(c, rt, c.fpcr); else c.fpcr = uint32_t(get_x(c, rt));
                return;
            case 0xda21:  // FPSR (3,3,4,4,1)
                if (read) set_x(c, rt, c.fpsr); else c.fpsr = uint32_t(get_x(c, rt));
                return;
            case 0xdf00:  // CNTFRQ_EL0 (3,3,14,0,0)
                if (read) set_x(c, rt, counter_frequency());
                return;
            case 0xdf01:  // CNTPCT_EL0
            case 0xdf02:  // CNTVCT_EL0 (3,3,14,0,2)
            case 0xdf05:  // CNTPCTSS_EL0
            case 0xdf06:  // CNTVCTSS_EL0
                if (read) set_x(c, rt, counter_ticks());
                return;
            case 0xd801:  // CTR_EL0 (3,3,0,0,1): 64-byte lines, as most cores report
                if (read) set_x(c, rt, 0x8444c004);
                return;
            case 0xd807:  // DCZID_EL0 (3,3,0,0,7): DC ZVA allowed, 64-byte blocks
                if (read) set_x(c, rt, 4);
                return;
            case 0xda11:  // DAIF (3,3,4,2,1): user code only reads it
                if (read) set_x(c, rt, 0);
                return;
            case 0xc000:  // MIDR_EL1 (3,0,0,0,0): Linux emulates the read for user space
                if (read) set_x(c, rt, 0x410fd080);  // a Cortex-A72
                return;
            default:
                // The ID registers (3,0,0,x,x) read as "nothing optional", which
                // is what makes compiler-rt and libraries pick plain code paths.
                if (read && sysreg >= 0xc000 && sysreg < 0xc080) {
                    set_x(c, rt, 0);
                    return;
                }
                break;
        }
    }
    undefined(c, insn);
}

void branch_system(Cpu& c, uint32_t insn, bool* branched) {
    // B.cond
    if ((insn & 0xff000010) == 0x54000000) {
        if (cond_holds(c.nzcv, field(insn, 3, 0))) {
            c.pc += static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
            *branched = true;
        }
        return;
    }
    // B, BL
    if ((insn & 0x7c000000) == 0x14000000) {
        if (bit(insn, 31)) c.x[30] = c.pc + 4;
        c.pc += static_cast<uint64_t>(sext(field(insn, 25, 0), 26) << 2);
        *branched = true;
        return;
    }
    // CBZ, CBNZ
    if ((insn & 0x7e000000) == 0x34000000) {
        uint64_t v = get_x(c, field(insn, 4, 0));
        if (!bit(insn, 31)) v = uint32_t(v);
        if ((v == 0) != bit(insn, 24)) {
            c.pc += static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
            *branched = true;
        }
        return;
    }
    // TBZ, TBNZ
    if ((insn & 0x7e000000) == 0x36000000) {
        const unsigned b = (bit(insn, 31) << 5) | field(insn, 23, 19);
        if (((get_x(c, field(insn, 4, 0)) >> b) & 1) == bit(insn, 24)) {
            c.pc += static_cast<uint64_t>(sext(field(insn, 18, 5), 14) << 2);
            *branched = true;
        }
        return;
    }
    // BR, BLR, RET and their pointer-authenticated forms (BRAA, BLRAAZ,
    // RETAA, ...). Authentication always succeeds here: guest pointers are
    // never signed, because PACIASP and friends are executed as NOPs.
    if ((insn & 0xfe000000) == 0xd6000000) {
        const unsigned opc = field(insn, 24, 21);
        if (field(insn, 20, 16) != 0x1f || (opc > 2 && opc != 8 && opc != 9)) undefined(c, insn);
        const uint64_t target = get_x(c, field(insn, 9, 5));
        const unsigned kind = opc & 3;  // 0 BR, 1 BLR, 2 RET
        if (kind == 1) c.x[30] = c.pc + 4;
        c.pc = target;
        *branched = true;
        return;
    }
    // SVC, HVC, SMC, BRK, HLT, DCPS
    if ((insn & 0xff000000) == 0xd4000000) {
        const unsigned opc = field(insn, 23, 21), ll = field(insn, 1, 0);
        if (opc == 0 && ll == 1) {
            if (!g_hooks.svc) fail(c, insn, "SVC (system call) without a handler");
            c.pc += 4;  // the handler sees pc after the SVC, as the kernel does
            g_hooks.svc(c, field(insn, 20, 5));
            *branched = true;
            return;
        }
        if (opc == 1 && ll == 0) fail(c, insn, "BRK (the guest aborted or hit a trap)");
        undefined(c, insn);
    }
    if ((insn & 0xffc00000) == 0xd5000000) {
        system_insn(c, insn);
        return;
    }
    undefined(c, insn);
}

// ---- loads and stores -------------------------------------------------------

template <typename T> T load(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}
template <typename T> void store(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

uint64_t load_n(uint64_t a, unsigned bytes) {
    switch (bytes) {
        case 1: return load<uint8_t>(a);
        case 2: return load<uint16_t>(a);
        case 4: return load<uint32_t>(a);
        default: return load<uint64_t>(a);
    }
}
void store_n(uint64_t a, unsigned bytes, uint64_t v) {
    switch (bytes) {
        case 1: store<uint8_t>(a, uint8_t(v)); break;
        case 2: store<uint16_t>(a, uint16_t(v)); break;
        case 4: store<uint32_t>(a, uint32_t(v)); break;
        default: store<uint64_t>(a, v); break;
    }
}

uint64_t atomic_load_n(uint64_t a, unsigned bytes) {
    switch (bytes) {
        case 1: return __atomic_load_n(reinterpret_cast<uint8_t*>(a), __ATOMIC_SEQ_CST);
        case 2: return __atomic_load_n(reinterpret_cast<uint16_t*>(a), __ATOMIC_SEQ_CST);
        case 4: return __atomic_load_n(reinterpret_cast<uint32_t*>(a), __ATOMIC_SEQ_CST);
        default: return __atomic_load_n(reinterpret_cast<uint64_t*>(a), __ATOMIC_SEQ_CST);
    }
}
void atomic_store_n(uint64_t a, unsigned bytes, uint64_t v) {
    switch (bytes) {
        case 1: __atomic_store_n(reinterpret_cast<uint8_t*>(a), uint8_t(v), __ATOMIC_SEQ_CST); break;
        case 2: __atomic_store_n(reinterpret_cast<uint16_t*>(a), uint16_t(v), __ATOMIC_SEQ_CST); break;
        case 4: __atomic_store_n(reinterpret_cast<uint32_t*>(a), uint32_t(v), __ATOMIC_SEQ_CST); break;
        default: __atomic_store_n(reinterpret_cast<uint64_t*>(a), v, __ATOMIC_SEQ_CST); break;
    }
}
// Compare-and-swap of 1..8 bytes; on failure *expected holds what memory had.
bool atomic_cas_n(uint64_t a, unsigned bytes, uint64_t* expected, uint64_t desired) {
    switch (bytes) {
        case 1: {
            uint8_t e = uint8_t(*expected);
            const bool ok = __atomic_compare_exchange_n(reinterpret_cast<uint8_t*>(a), &e, uint8_t(desired), false,
                                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        case 2: {
            uint16_t e = uint16_t(*expected);
            const bool ok = __atomic_compare_exchange_n(reinterpret_cast<uint16_t*>(a), &e, uint16_t(desired), false,
                                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        case 4: {
            uint32_t e = uint32_t(*expected);
            const bool ok = __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(a), &e, uint32_t(desired), false,
                                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *expected = e;
            return ok;
        }
        default:
            return __atomic_compare_exchange_n(reinterpret_cast<uint64_t*>(a), expected, desired, false,
                                               __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}
bool atomic_cas_128(uint64_t a, uint64_t expected[2], const uint64_t desired[2]) {
    unsigned __int128 e = (static_cast<unsigned __int128>(expected[1]) << 64) | expected[0];
    const unsigned __int128 d = (static_cast<unsigned __int128>(desired[1]) << 64) | desired[0];
    const unsigned __int128 old =
        __sync_val_compare_and_swap(reinterpret_cast<unsigned __int128*>(a), e, d);
    const bool ok = old == e;
    expected[0] = static_cast<uint64_t>(old);
    expected[1] = static_cast<uint64_t>(old >> 64);
    return ok;
}

// LDXR/STXR/LDAXR/STLXR (single and pair), LDAR/STLR, CAS/CASP.
void ldst_exclusive(Cpu& c, uint32_t insn) {
    const unsigned size = field(insn, 31, 30);
    const bool o2 = bit(insn, 23), l = bit(insn, 22), o1 = bit(insn, 21);
    const unsigned rs = field(insn, 20, 16), rt2 = field(insn, 14, 10), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    const uint64_t addr = get_xsp(c, rn);
    const unsigned bytes = 1u << size;

    if (o2 && o1) {  // CAS, CASA, CASL, CASAL (byte/half/word/doubleword)
        if (rt2 != 31) undefined(c, insn);
        uint64_t expected = get_x(c, rs) & mask(bytes * 8);
        atomic_cas_n(addr, bytes, &expected, get_x(c, rt));
        set_x(c, rs, expected);
        return;
    }
    if (!o2 && o1 && !bit(insn, 31) && rt2 == 31) {  // CASP (pairs of words or doublewords)
        const bool wide = bit(insn, 30);
        if ((rs & 1) || (rt & 1)) undefined(c, insn);
        if (wide) {
            uint64_t e[2] = {get_x(c, rs), get_x(c, rs + 1)};
            const uint64_t d[2] = {get_x(c, rt), get_x(c, rt + 1)};
            atomic_cas_128(addr, e, d);
            set_x(c, rs, e[0]);
            set_x(c, rs + 1, e[1]);
        } else {
            uint64_t e = uint32_t(get_x(c, rs)) | (get_x(c, rs + 1) << 32);
            const uint64_t d = uint32_t(get_x(c, rt)) | (get_x(c, rt + 1) << 32);
            atomic_cas_n(addr, 8, &e, d);
            set_x(c, rs, uint32_t(e));
            set_x(c, rs + 1, e >> 32);
        }
        return;
    }
    if (o2) {  // LDAR/LDLAR, STLR/STLLR
        if (l) {
            set_x(c, rt, atomic_load_n(addr, bytes));
        } else {
            atomic_store_n(addr, bytes, get_x(c, rt));
        }
        return;
    }
    if (!o1) {  // LDXR/LDAXR, STXR/STLXR
        if (l) {
            const uint64_t v = atomic_load_n(addr, bytes);
            c.excl_valid = true;
            c.excl_addr = addr;
            c.excl_size = bytes;
            c.excl_val[0] = v;
            set_x(c, rt, v);
        } else {
            uint32_t status = 1;
            if (c.excl_valid && c.excl_addr == addr && c.excl_size == bytes) {
                uint64_t expected = c.excl_val[0];
                if (atomic_cas_n(addr, bytes, &expected, get_x(c, rt))) status = 0;
            }
            c.excl_valid = false;
            set_x(c, rs, status);
        }
        return;
    }
    // LDXP/LDAXP, STXP/STLXP: two words (size 2) or two doublewords (size 3).
    if (size < 2) undefined(c, insn);
    if (size == 2) {
        if (l) {
            const uint64_t v = atomic_load_n(addr, 8);
            c.excl_valid = true;
            c.excl_addr = addr;
            c.excl_size = 8;
            c.excl_val[0] = v;
            set_x(c, rt, uint32_t(v));
            set_x(c, rt2, v >> 32);
        } else {
            uint32_t status = 1;
            if (c.excl_valid && c.excl_addr == addr && c.excl_size == 8) {
                uint64_t expected = c.excl_val[0];
                if (atomic_cas_n(addr, 8, &expected, uint32_t(get_x(c, rt)) | (get_x(c, rt2) << 32))) status = 0;
            }
            c.excl_valid = false;
            set_x(c, rs, status);
        }
        return;
    }
    if (l) {
        // An atomic 16-byte read: a compare-and-swap of a value with itself.
        uint64_t v[2] = {0, 0};
        const uint64_t same[2] = {0, 0};
        if (atomic_cas_128(addr, v, same)) { /* memory was zero, still is */ }
        c.excl_valid = true;
        c.excl_addr = addr;
        c.excl_size = 16;
        c.excl_val[0] = v[0];
        c.excl_val[1] = v[1];
        set_x(c, rt, v[0]);
        set_x(c, rt2, v[1]);
    } else {
        uint32_t status = 1;
        if (c.excl_valid && c.excl_addr == addr && c.excl_size == 16) {
            uint64_t e[2] = {c.excl_val[0], c.excl_val[1]};
            const uint64_t d[2] = {get_x(c, rt), get_x(c, rt2)};
            if (atomic_cas_128(addr, e, d)) status = 0;
        }
        c.excl_valid = false;
        set_x(c, rs, status);
    }
}

// LDADD, LDCLR, LDEOR, LDSET, LD{S,U}{MAX,MIN}, SWP (and their ST* aliases).
void ldst_atomic(Cpu& c, uint32_t insn) {
    const unsigned bytes = 1u << field(insn, 31, 30);
    const unsigned rs = field(insn, 20, 16), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    const bool o3 = bit(insn, 15);
    const unsigned opc = field(insn, 14, 12);
    const uint64_t addr = get_xsp(c, rn);
    const unsigned bits = bytes * 8;
    if (o3 && opc == 4) {  // LDAPR
        set_x(c, rt, atomic_load_n(addr, bytes));
        return;
    }
    if (o3 && opc != 0) undefined(c, insn);
    const uint64_t operand = get_x(c, rs) & mask(bits);
    uint64_t old = atomic_load_n(addr, bytes);
    for (;;) {
        uint64_t next;
        if (o3) {
            next = operand;  // SWP
        } else {
            switch (opc) {
                case 0: next = old + operand; break;
                case 1: next = old & ~operand; break;
                case 2: next = old ^ operand; break;
                case 3: next = old | operand; break;
                case 4: next = sext(old, bits) > sext(operand, bits) ? old : operand; break;
                case 5: next = sext(old, bits) < sext(operand, bits) ? old : operand; break;
                case 6: next = old > operand ? old : operand; break;
                default: next = old < operand ? old : operand; break;
            }
        }
        if (atomic_cas_n(addr, bytes, &old, next & mask(bits))) break;
    }
    set_x(c, rt, old);
}

// LDR/STR (immediate, unscaled, pre/post-indexed, register offset) for the
// general-purpose registers; the SIMD&FP forms go to exec_simd_ldst.
void ldst_register(Cpu& c, uint32_t insn) {
    const unsigned size = field(insn, 31, 30), opc = field(insn, 23, 22);
    const unsigned rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    uint64_t addr;
    bool writeback = false;
    uint64_t wb = 0;
    const uint64_t base = get_xsp(c, rn);
    if (bit(insn, 24)) {
        addr = base + (uint64_t(field(insn, 21, 10)) << size);
    } else if (!bit(insn, 21)) {
        const int64_t imm9 = sext(field(insn, 20, 12), 9);
        switch (field(insn, 11, 10)) {
            case 0: case 2: addr = base + imm9; break;  // LDUR/STUR, LDTR/STTR
            case 1: addr = base; writeback = true; wb = base + imm9; break;  // post-index
            default: addr = base + imm9; writeback = true; wb = addr; break;  // pre-index
        }
    } else if (field(insn, 11, 10) == 2) {
        const unsigned option = field(insn, 15, 13);
        if (!(option & 2)) undefined(c, insn);
        addr = base + extend_reg(get_x(c, field(insn, 20, 16)), option, bit(insn, 12) ? size : 0);
    } else if (field(insn, 11, 10) == 0) {
        ldst_atomic(c, insn);
        return;
    } else {
        undefined(c, insn);  // LDRAA/LDRAB
    }

    const unsigned bytes = 1u << size;
    if (opc == 0) {
        store_n(addr, bytes, get_x(c, rt));
    } else if (opc == 1) {
        set_x(c, rt, load_n(addr, bytes));
    } else if (size == 3) {
        if (opc != 2) undefined(c, insn);
        // PRFM / PRFUM: nothing to do
    } else if (opc == 2) {
        set_x(c, rt, static_cast<uint64_t>(sext(load_n(addr, bytes), bytes * 8)));
    } else {
        if (size == 2) undefined(c, insn);
        set_x(c, rt, uint32_t(sext(load_n(addr, bytes), bytes * 8)));
    }
    if (writeback) set_xsp(c, rn, wb);
}

void ldst_pair(Cpu& c, uint32_t insn) {
    const unsigned opc = field(insn, 31, 30), mode = field(insn, 24, 23);
    const bool l = bit(insn, 22);
    const unsigned rt2 = field(insn, 14, 10), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    if (opc == 3 || (opc == 1 && !l)) undefined(c, insn);  // STGP (MTE) is not supported
    const unsigned scale = opc == 2 ? 3 : 2;
    const int64_t off = sext(field(insn, 21, 15), 7) << scale;
    const uint64_t base = get_xsp(c, rn);
    const uint64_t addr = mode == 1 ? base : base + off;
    const unsigned bytes = 1u << scale;
    if (l) {
        uint64_t a = load_n(addr, bytes), b = load_n(addr + bytes, bytes);
        if (opc == 1) {  // LDPSW
            a = static_cast<uint64_t>(sext(a, 32));
            b = static_cast<uint64_t>(sext(b, 32));
        }
        set_x(c, rt, a);
        set_x(c, rt2, b);
    } else {
        const uint64_t a = get_x(c, rt), b = get_x(c, rt2);
        store_n(addr, bytes, a);
        store_n(addr + bytes, bytes, b);
    }
    if (mode == 1 || mode == 3) set_xsp(c, rn, base + off);
}

void loads_stores(Cpu& c, uint32_t insn) {
    const bool v = bit(insn, 26);
    if (v) {
        if (!exec_simd_ldst(c, insn)) undefined(c, insn);
        return;
    }
    if ((insn & 0x3f000000) == 0x08000000) {
        ldst_exclusive(c, insn);
        return;
    }
    if ((insn & 0x3b000000) == 0x18000000) {  // LDR (literal), LDRSW (literal), PRFM (literal)
        const uint64_t addr = c.pc + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        const unsigned rt = field(insn, 4, 0);
        switch (field(insn, 31, 30)) {
            case 0: set_x(c, rt, load<uint32_t>(addr)); break;
            case 1: set_x(c, rt, load<uint64_t>(addr)); break;
            case 2: set_x(c, rt, static_cast<uint64_t>(int64_t(load<int32_t>(addr)))); break;
            default: break;
        }
        return;
    }
    if ((insn & 0x3a000000) == 0x28000000) {
        ldst_pair(c, insn);
        return;
    }
    if ((insn & 0x3a000000) == 0x38000000) {
        ldst_register(c, insn);
        return;
    }
    // LDAPUR/STLUR (RCpc unscaled): treated as their plain counterparts.
    if ((insn & 0x3f200c00) == 0x19000000) {
        const unsigned size = field(insn, 31, 30), opc = field(insn, 23, 22);
        const uint64_t addr = get_xsp(c, field(insn, 9, 5)) + sext(field(insn, 20, 12), 9);
        const unsigned rt = field(insn, 4, 0), bytes = 1u << size;
        if (opc == 0) atomic_store_n(addr, bytes, get_x(c, rt));
        else if (opc == 1) set_x(c, rt, atomic_load_n(addr, bytes));
        else if (opc == 2) set_x(c, rt, static_cast<uint64_t>(sext(atomic_load_n(addr, bytes), bytes * 8)));
        else set_x(c, rt, uint32_t(sext(atomic_load_n(addr, bytes), bytes * 8)));
        return;
    }
    undefined(c, insn);
}

// ---- data processing, register ----------------------------------------------

uint32_t crc32_update(uint32_t crc, uint64_t data, unsigned bytes, bool castagnoli) {
    const uint32_t poly = castagnoli ? 0x82f63b78u : 0xedb88320u;
    for (unsigned i = 0; i < bytes; ++i) {
        crc ^= uint8_t(data >> (8 * i));
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    }
    return crc;
}

void dp_register(Cpu& c, uint32_t insn) {
    const bool sf = bit(insn, 31);
    const unsigned size = sf ? 64 : 32;
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16);
    auto out = [&](uint64_t r) { set_x(c, rd, r & mask(size)); };

    if ((insn & 0x1f000000) == 0x0a000000) {  // logical (shifted register)
        const unsigned imm6 = field(insn, 15, 10);
        if (!sf && imm6 >= 32) undefined(c, insn);
        uint64_t b = shift_reg(get_x(c, rm), field(insn, 23, 22), imm6, sf);
        if (bit(insn, 21)) b = ~b;
        const uint64_t a = get_x(c, rn);
        uint64_t r;
        switch (field(insn, 30, 29)) {
            case 0: r = a & b; break;
            case 1: r = a | b; break;
            case 2: r = a ^ b; break;
            default: r = a & b; break;
        }
        r &= mask(size);
        if (field(insn, 30, 29) == 3) c.nzcv = logic_flags(r, sf);
        out(r);
        return;
    }
    if ((insn & 0x1f200000) == 0x0b000000) {  // add/sub (shifted register)
        const unsigned imm6 = field(insn, 15, 10), type = field(insn, 23, 22);
        if (type == 3 || (!sf && imm6 >= 32)) undefined(c, insn);
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const uint64_t b = shift_reg(get_x(c, rm), type, imm6, sf);
        uint32_t flags;
        const uint64_t r = add_with_carry(get_x(c, rn), sub ? ~b : b, sub, sf, &flags);
        if (s) c.nzcv = flags;
        out(r);
        return;
    }
    if ((insn & 0x1f200000) == 0x0b200000) {  // add/sub (extended register)
        const unsigned imm3 = field(insn, 12, 10);
        if (imm3 > 4 || field(insn, 23, 22) != 0) undefined(c, insn);
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const uint64_t b = extend_reg(get_x(c, rm), field(insn, 15, 13), imm3);
        uint32_t flags;
        const uint64_t r = add_with_carry(get_xsp(c, rn), sub ? ~b : b, sub, sf, &flags) & mask(size);
        if (s) {
            c.nzcv = flags;
            set_x(c, rd, r);
        } else {
            set_xsp(c, rd, r);
        }
        return;
    }
    if ((insn & 0x1fe00000) == 0x1a000000) {  // ADC/ADCS/SBC/SBCS
        if (field(insn, 15, 10) != 0) undefined(c, insn);
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const uint64_t b = get_x(c, rm);
        uint32_t flags;
        const uint64_t r = add_with_carry(get_x(c, rn), sub ? ~b : b, (c.nzcv >> 29) & 1, sf, &flags);
        if (s) c.nzcv = flags;
        out(r);
        return;
    }
    if ((insn & 0x1fe00000) == 0x1a400000) {  // CCMN/CCMP (register and immediate)
        if (!bit(insn, 29) || bit(insn, 10) || bit(insn, 4)) undefined(c, insn);
        const bool sub = bit(insn, 30);
        if (cond_holds(c.nzcv, field(insn, 15, 12))) {
            const uint64_t b = bit(insn, 11) ? uint64_t(rm) : get_x(c, rm);
            add_with_carry(get_x(c, rn), sub ? ~b : b, sub, sf, &c.nzcv);
        } else {
            c.nzcv = field(insn, 3, 0) << 28;
        }
        return;
    }
    if ((insn & 0x1fe00000) == 0x1a800000) {  // CSEL/CSINC/CSINV/CSNEG
        if (bit(insn, 29) || bit(insn, 11)) undefined(c, insn);
        const unsigned op = (bit(insn, 30) << 1) | bit(insn, 10);
        uint64_t r;
        if (cond_holds(c.nzcv, field(insn, 15, 12))) {
            r = get_x(c, rn);
        } else {
            r = get_x(c, rm);
            switch (op) {
                case 1: r = r + 1; break;
                case 2: r = ~r; break;
                case 3: r = 0 - r; break;
                default: break;
            }
        }
        out(r);
        return;
    }
    if ((insn & 0x5fe00000) == 0x5ac00000) {  // data processing (1 source)
        const unsigned opcode2 = field(insn, 20, 16), opcode = field(insn, 15, 10);
        if (bit(insn, 29)) undefined(c, insn);
        if (opcode2 == 1) {
            // PACIA/PACIB/AUTIA/... and XPACI/XPACD: pointers are never
            // signed here, so every one of them leaves the register as is.
            if (!sf) undefined(c, insn);
            return;
        }
        if (opcode2 != 0) undefined(c, insn);
        const uint64_t a = get_x(c, rn) & mask(size);
        uint64_t r = 0;
        switch (opcode) {
            case 0:  // RBIT
                for (unsigned i = 0; i < size; ++i) r |= ((a >> i) & 1) << (size - 1 - i);
                break;
            case 1:  // REV16
                for (unsigned i = 0; i < size; i += 16)
                    r |= uint64_t(__builtin_bswap16(uint16_t(a >> i))) << i;
                break;
            case 2:  // REV32 (64-bit) / REV (32-bit)
                for (unsigned i = 0; i < size; i += 32)
                    r |= uint64_t(__builtin_bswap32(uint32_t(a >> i))) << i;
                break;
            case 3:  // REV (64-bit)
                if (!sf) undefined(c, insn);
                r = __builtin_bswap64(a);
                break;
            case 4:  // CLZ
                r = a == 0 ? size : (sf ? __builtin_clzll(a) : __builtin_clz(uint32_t(a)));
                break;
            case 5: {  // CLS
                const uint64_t t = (a ^ (a << 1)) & mask(size);
                // leading sign bits, not counting the sign bit itself
                r = t == 0 ? size - 1 : (sf ? __builtin_clzll(t) : __builtin_clz(uint32_t(t)));
                break;
            }
            default:
                undefined(c, insn);
        }
        out(r);
        return;
    }
    if ((insn & 0x5fe00000) == 0x1ac00000) {  // data processing (2 source)
        if (bit(insn, 29)) undefined(c, insn);
        const unsigned opcode = field(insn, 15, 10);
        const uint64_t a = get_x(c, rn) & mask(size), b = get_x(c, rm) & mask(size);
        switch (opcode) {
            case 2:  // UDIV
                out(b == 0 ? 0 : a / b);
                return;
            case 3: {  // SDIV (no trap: x/0 = 0 and INT_MIN/-1 = INT_MIN, as on ARM)
                const int64_t sa = sext(a, size), sb = sext(b, size);
                if (sb == 0) out(0);
                else if (sb == -1) out(static_cast<uint64_t>(0) - static_cast<uint64_t>(sa));
                else out(static_cast<uint64_t>(sa / sb));
                return;
            }
            case 8: case 9: case 10: case 11:  // LSLV, LSRV, ASRV, RORV
                out(shift_reg(a, opcode - 8, unsigned(b % size), sf));
                return;
            case 12:  // PACGA
                out(0);
                return;
            case 16: case 17: case 18: case 19:  // CRC32B/H/W/X
            case 20: case 21: case 22: case 23: {  // CRC32CB/H/W/X
                const unsigned sz = opcode & 3;
                if ((sz == 3) != sf) undefined(c, insn);
                set_x(c, rd, crc32_update(uint32_t(get_x(c, rn)), get_x(c, rm), 1u << sz, opcode & 4));
                return;
            }
            default:
                undefined(c, insn);
        }
    }
    if ((insn & 0x1f000000) == 0x1b000000) {  // data processing (3 source)
        const unsigned op = (field(insn, 30, 29) << 4) | (field(insn, 23, 21) << 1) | bit(insn, 15);
        const uint64_t a = get_x(c, rn), b = get_x(c, rm), acc = get_x(c, field(insn, 14, 10));
        switch (op) {
            case 0x00: out(acc + a * b); return;                                    // MADD
            case 0x01: out(acc - a * b); return;                                    // MSUB
            case 0x02: if (!sf) break; out(acc + uint64_t(int64_t(int32_t(a)) * int32_t(b))); return;  // SMADDL
            case 0x03: if (!sf) break; out(acc - uint64_t(int64_t(int32_t(a)) * int32_t(b))); return;  // SMSUBL
            case 0x04:  // SMULH
                if (!sf) break;
                out(static_cast<uint64_t>((static_cast<__int128>(int64_t(a)) * int64_t(b)) >> 64));
                return;
            case 0x0a: if (!sf) break; out(acc + uint64_t(uint32_t(a)) * uint32_t(b)); return;  // UMADDL
            case 0x0b: if (!sf) break; out(acc - uint64_t(uint32_t(a)) * uint32_t(b)); return;  // UMSUBL
            case 0x0c:  // UMULH
                if (!sf) break;
                out(static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) >> 64));
                return;
            default: break;
        }
        undefined(c, insn);
    }
    undefined(c, insn);
}

}  // namespace

void set_hooks(const Hooks& h) { g_hooks = h; }
const Hooks& hooks() { return g_hooks; }

void step(Cpu& c) {
    uint32_t insn;
    std::memcpy(&insn, reinterpret_cast<const void*>(c.pc), 4);
    bool branched = false;
    switch (field(insn, 28, 25)) {
        case 0x8: case 0x9:
            dp_immediate(c, insn);
            break;
        case 0xa: case 0xb:
            branch_system(c, insn, &branched);
            break;
        case 0x4: case 0x6: case 0xc: case 0xe:
            loads_stores(c, insn);
            break;
        case 0x5: case 0xd:
            dp_register(c, insn);
            break;
        case 0x7: case 0xf:
            if (!exec_simd(c, insn)) undefined(c, insn);
            break;
        default:
            undefined(c, insn);
    }
    if (!branched) c.pc += 4;
}

void run(Cpu& c) {
    uint64_t lo = 0, hi = 0;
    while (c.pc != c.stop_pc) {
        if (c.pc - lo >= hi - lo) {
            if (!g_hooks.code_range(c.pc, &lo, &hi)) {
                lo = hi = 0;
                g_hooks.host_call(c, c.pc);
                continue;
            }
        }
        step(c);
    }
}

}  // namespace winapk::arm64
