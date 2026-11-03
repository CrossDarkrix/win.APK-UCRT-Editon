// arm64_internal.h -- field extraction and register access shared by the two
// halves of the interpreter (arm64_cpu.cpp, arm64_simd.cpp).

#pragma once

#include "winapk/arm64_cpu.h"

#include <cstdint>

namespace winapk::arm64 {

inline uint32_t field(uint32_t insn, unsigned hi, unsigned lo) {
    return (insn >> lo) & ((hi - lo == 31) ? ~0u : ((1u << (hi - lo + 1)) - 1));
}
inline bool bit(uint32_t insn, unsigned b) { return (insn >> b) & 1; }

// Low `n` bits set (n = 0..64).
inline uint64_t mask(unsigned n) { return n >= 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1); }

// Sign-extends the low `bits` bits of v.
inline int64_t sext(uint64_t v, unsigned bits) {
    return bits >= 64 ? static_cast<int64_t>(v)
                      : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
}

// Rotate the low `size` bits of v right by r.
inline uint64_t ror(uint64_t v, unsigned r, unsigned size) {
    v &= mask(size);
    r %= size;
    if (r == 0) return v;
    return ((v >> r) | (v << (size - r))) & mask(size);
}

// Register 31 reads as zero (XZR) in most operand positions and as SP in the
// few that name the stack pointer; the decoder picks the right accessor.
inline uint64_t get_x(const Cpu& c, unsigned n) { return n == 31 ? 0 : c.x[n]; }
inline uint64_t get_xsp(const Cpu& c, unsigned n) { return n == 31 ? c.sp : c.x[n]; }
inline void set_x(Cpu& c, unsigned n, uint64_t v) {
    if (n != 31) c.x[n] = v;
}
inline void set_xsp(Cpu& c, unsigned n, uint64_t v) {
    if (n == 31) c.sp = v;
    else c.x[n] = v;
}

// ConditionHolds(): cond is the 4-bit condition field, nzcv as Cpu::nzcv.
inline bool cond_holds(uint32_t nzcv, unsigned cond) {
    const bool n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, cf = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1;
    bool r;
    switch (cond >> 1) {
        case 0: r = z; break;
        case 1: r = cf; break;
        case 2: r = n; break;
        case 3: r = v; break;
        case 4: r = cf && !z; break;
        case 5: r = n == v; break;
        case 6: r = n == v && !z; break;
        default: r = true; break;
    }
    if ((cond & 1) && cond != 15) r = !r;
    return r;
}

}  // namespace winapk::arm64
