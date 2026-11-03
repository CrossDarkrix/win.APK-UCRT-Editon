// arm64_simd.cpp -- the A64 interpreter's floating-point and Advanced SIMD
// (NEON) instructions, and the SIMD&FP loads and stores. See arm64_cpu.h.
//
// Floating point runs on the host's SSE unit in its default mode (round to
// nearest even). FPCR's rounding mode, flush-to-zero and default-NaN bits are
// not modelled, and FPSR's cumulative flags are not raised: Android code
// almost never changes them. Where Arm and x86 differ on a defined result
// (float-to-integer conversion saturates on Arm, NaN converts to 0, FMAXNM
// ignores a quiet NaN), the Arm result is computed explicitly.

#include "winapk/arm64_cpu.h"

#include "arm64_internal.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace winapk::arm64 {

namespace {

// ---- element access ---------------------------------------------------------

uint64_t elem(const VReg& v, unsigned ebits, unsigned i) {
    switch (ebits) {
        case 8: return v.b[i];
        case 16: return v.h[i];
        case 32: return v.s[i];
        default: return v.d[i];
    }
}
void set_elem(VReg& v, unsigned ebits, unsigned i, uint64_t x) {
    switch (ebits) {
        case 8: v.b[i] = uint8_t(x); break;
        case 16: v.h[i] = uint16_t(x); break;
        case 32: v.s[i] = uint32_t(x); break;
        default: v.d[i] = x; break;
    }
}

// Writes a result register: bits beyond `bytes` are cleared, as every
// AdvSIMD/FP write to a D, S, H or B register (or a 64-bit vector) does.
void write_v(Cpu& c, unsigned rd, const VReg& r, unsigned bytes) {
    VReg out{};
    std::memcpy(out.b, r.b, bytes);
    c.v[rd] = out;
}

float f32(uint64_t bits) {
    float f;
    const uint32_t b = uint32_t(bits);
    std::memcpy(&f, &b, 4);
    return f;
}
double f64(uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, 8);
    return d;
}
uint64_t bits32(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
uint64_t bits64(double d) {
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}

// IEEE half precision, by hand (no F16C assumption on the host).
float half_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1f, frac = h & 0x3ff;
    uint32_t out;
    if (exp == 0) {
        if (frac == 0) {
            out = sign;
        } else {  // subnormal: normalize
            exp = 127 - 15 + 1;
            while (!(frac & 0x400)) {
                frac <<= 1;
                --exp;
            }
            out = sign | (exp << 23) | ((frac & 0x3ff) << 13);
        }
    } else if (exp == 0x1f) {
        out = sign | 0x7f800000 | (frac << 13) | (frac ? 0x400000 : 0);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (frac << 13);
    }
    return f32(out);
}
uint16_t float_to_half(float f) {
    const uint32_t b = uint32_t(bits32(f));
    const uint16_t sign = uint16_t((b >> 16) & 0x8000);
    const int32_t exp = int32_t((b >> 23) & 0xff);
    uint32_t frac = b & 0x7fffff;
    if (exp == 0xff) return uint16_t(sign | 0x7c00 | (frac ? 0x200 | (frac >> 13) : 0));
    int32_t e = exp - 127 + 15;
    if (e >= 0x1f) return uint16_t(sign | 0x7c00);  // overflow -> infinity
    if (e <= 0) {
        if (e < -10) return sign;
        frac |= 0x800000;
        const unsigned shift = unsigned(14 - e);
        uint32_t half = frac >> shift;
        const uint32_t rem = frac & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rem > mid || (rem == mid && (half & 1))) ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(e) << 10) | (frac >> 13);
    const uint32_t rem = frac & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (half & 1))) ++half;  // may carry into the exponent: correct
    return uint16_t(sign | half);
}

// ---- floating-point helpers -------------------------------------------------

enum Rounding { kNearestEven, kPlusInf, kMinusInf, kZero, kNearestAway };

template <typename F> F round_to_integral(F v, int mode) {
    switch (mode) {
        case kPlusInf: return std::ceil(v);
        case kMinusInf: return std::floor(v);
        case kZero: return std::trunc(v);
        case kNearestAway: return std::round(v);
        default: return std::nearbyint(v);
    }
}

// FPToFixed: saturating, NaN -> 0, as Arm defines it (x86's cvtt* return the
// "integer indefinite" value instead).
template <typename F> uint64_t fp_to_int(F v, unsigned bits, bool is_unsigned, int mode, int fbits = 0) {
    if (std::isnan(v)) return 0;
    if (fbits) v = v * std::ldexp(F(1), fbits);
    const F r = round_to_integral(v, mode);
    if (is_unsigned) {
        if (r <= F(0)) return 0;
        if (r >= std::ldexp(F(1), int(bits))) return mask(bits);
        return static_cast<uint64_t>(r);
    }
    const F lim = std::ldexp(F(1), int(bits) - 1);
    if (r >= lim) return mask(bits - 1);
    if (r < -lim) return uint64_t{1} << (bits - 1);
    return static_cast<uint64_t>(static_cast<int64_t>(r)) & mask(bits);
}

template <typename F> F int_to_fp(uint64_t v, unsigned bits, bool is_unsigned, int fbits = 0) {
    F r = is_unsigned ? static_cast<F>(v & mask(bits)) : static_cast<F>(sext(v, bits));
    if (fbits) r = r / std::ldexp(F(1), fbits);
    return r;
}

template <typename F> F fmax_arm(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<F>::quiet_NaN();
    if (a == F(0) && b == F(0)) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}
template <typename F> F fmin_arm(F a, F b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<F>::quiet_NaN();
    if (a == F(0) && b == F(0)) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <typename F> F fmaxnm_arm(F a, F b) {
    if (std::isnan(a) && !std::isnan(b)) return b;
    if (std::isnan(b) && !std::isnan(a)) return a;
    return fmax_arm(a, b);
}
template <typename F> F fminnm_arm(F a, F b) {
    if (std::isnan(a) && !std::isnan(b)) return b;
    if (std::isnan(b) && !std::isnan(a)) return a;
    return fmin_arm(a, b);
}

uint32_t fcmp_flags(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return 0x30000000;  // C V
    if (a == b) return 0x60000000;                          // Z C
    if (a < b) return 0x80000000;                           // N
    return 0x20000000;                                      // C
}

// A lane-typed float operation on raw bits (ebits = 32 or 64).
template <typename Op> uint64_t fop2(unsigned ebits, uint64_t a, uint64_t b, Op op) {
    return ebits == 32 ? bits32(op(f32(a), f32(b))) : bits64(op(f64(a), f64(b)));
}
template <typename Op> uint64_t fop1(unsigned ebits, uint64_t a, Op op) {
    return ebits == 32 ? bits32(op(f32(a))) : bits64(op(f64(a)));
}
double fget(unsigned ebits, uint64_t a) { return ebits == 32 ? double(f32(a)) : f64(a); }

// VFPExpandImm
uint64_t vfp_expand_imm(unsigned imm8, unsigned ebits) {
    const uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
    if (ebits == 16) {
        const uint64_t exp = ((b ^ 1) << 4) | ((b ? 3 : 0) << 2) | cd;
        return (a << 15) | (exp << 10) | (efgh << 6);
    }
    if (ebits == 32) {
        const uint64_t exp = ((b ^ 1) << 7) | ((b ? 0x1f : 0) << 2) | cd;
        return (a << 31) | (exp << 23) | (efgh << 19);
    }
    const uint64_t exp = ((b ^ 1) << 10) | ((b ? 0xff : 0) << 2) | cd;
    return (a << 63) | (exp << 52) | (efgh << 48);
}

// ---- integer lane helpers ---------------------------------------------------

uint64_t sat_signed(__int128 v, unsigned bits) {
    const __int128 hi = (static_cast<__int128>(1) << (bits - 1)) - 1, lo = -(static_cast<__int128>(1) << (bits - 1));
    if (v > hi) v = hi;
    if (v < lo) v = lo;
    return static_cast<uint64_t>(v) & mask(bits);
}
uint64_t sat_unsigned(__int128 v, unsigned bits) {
    const __int128 hi = (bits == 64) ? static_cast<__int128>(~uint64_t{0}) : (static_cast<__int128>(1) << bits) - 1;
    if (v > hi) v = hi;
    if (v < 0) v = 0;
    return static_cast<uint64_t>(v) & mask(bits);
}
__int128 as_int(uint64_t v, unsigned bits, bool is_unsigned) {
    return is_unsigned ? static_cast<__int128>(v & mask(bits)) : static_cast<__int128>(sext(v, bits));
}

// SSHL/USHL and the rounding/saturating variants: the shift is the signed low
// byte of the second operand; negative shifts right.
uint64_t shl_by_reg(uint64_t a, uint64_t b, unsigned bits, bool is_unsigned, bool rounding, bool saturating) {
    const int sh = int(int8_t(uint8_t(b)));
    const __int128 v = as_int(a, bits, is_unsigned);
    __int128 r;
    if (sh >= 0) {
        if (sh >= 127) r = v == 0 ? 0 : (v > 0 ? (static_cast<__int128>(1) << 126) : -(static_cast<__int128>(1) << 126));
        else r = v << sh;
    } else {
        const int s = -sh;
        if (s >= 127) {
            r = (v < 0 && !rounding) ? -1 : 0;
        } else {
            const __int128 round = rounding ? (static_cast<__int128>(1) << (s - 1)) : 0;
            r = (v + round) >> s;
        }
    }
    if (saturating) return is_unsigned ? sat_unsigned(r, bits) : sat_signed(r, bits);
    return static_cast<uint64_t>(r) & mask(bits);
}

// ---- Advanced SIMD: three registers, same type ------------------------------

bool three_same(Cpu& c, uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 11);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const VReg a = c.v[rn], b = c.v[rm], d = c.v[rd];
    VReg r{};
    const unsigned bytes = scalar ? 0 : (q ? 16 : 8);

    if (opcode >= 0x18) {  // floating point (size<1> is the operation, size<0> the precision)
        const bool hi = size & 2;
        const unsigned ebits = (size & 1) ? 64 : 32;
        if (!scalar && ebits == 64 && !q) return false;
        const unsigned n = scalar ? 1 : (q ? 16 : 8) * 8 / ebits;
        const unsigned key = (u << 6) | (hi << 5) | opcode;
        auto lane = [&](unsigned i) -> bool {
            const uint64_t x = elem(a, ebits, i), y = elem(b, ebits, i);
            const double fx = fget(ebits, x), fy = fget(ebits, y);
            uint64_t out;
            switch (key) {
                case 0x18: out = ebits == 32 ? bits32(fmaxnm_arm(f32(x), f32(y))) : bits64(fmaxnm_arm(fx, fy)); break;
                case 0x38: out = ebits == 32 ? bits32(fminnm_arm(f32(x), f32(y))) : bits64(fminnm_arm(fx, fy)); break;
                case 0x19:  // FMLA (fused on Arm)
                    out = ebits == 32 ? bits32(std::fma(f32(x), f32(y), f32(elem(d, 32, i))))
                                      : bits64(std::fma(fx, fy, f64(elem(d, 64, i))));
                    break;
                case 0x39:  // FMLS
                    out = ebits == 32 ? bits32(std::fma(-f32(x), f32(y), f32(elem(d, 32, i))))
                                      : bits64(std::fma(-fx, fy, f64(elem(d, 64, i))));
                    break;
                case 0x1a: out = fop2(ebits, x, y, [](auto p, auto q2) { return p + q2; }); break;   // FADD
                case 0x3a: out = fop2(ebits, x, y, [](auto p, auto q2) { return p - q2; }); break;   // FSUB
                case 0x1b: out = fop2(ebits, x, y, [](auto p, auto q2) { return p * q2; }); break;   // FMULX
                case 0x5b: out = fop2(ebits, x, y, [](auto p, auto q2) { return p * q2; }); break;   // FMUL
                case 0x7a: out = fop2(ebits, x, y, [](auto p, auto q2) { return std::fabs(p - q2); }); break;  // FABD
                case 0x5f: out = fop2(ebits, x, y, [](auto p, auto q2) { return p / q2; }); break;   // FDIV
                case 0x1e: out = ebits == 32 ? bits32(fmax_arm(f32(x), f32(y))) : bits64(fmax_arm(fx, fy)); break;
                case 0x3e: out = ebits == 32 ? bits32(fmin_arm(f32(x), f32(y))) : bits64(fmin_arm(fx, fy)); break;
                case 0x1c: out = fx == fy ? mask(ebits) : 0; break;                   // FCMEQ
                case 0x5c: out = fx >= fy ? mask(ebits) : 0; break;                   // FCMGE
                case 0x7c: out = fx > fy ? mask(ebits) : 0; break;                    // FCMGT
                case 0x5d: out = std::fabs(fx) >= std::fabs(fy) ? mask(ebits) : 0; break;  // FACGE
                case 0x7d: out = std::fabs(fx) > std::fabs(fy) ? mask(ebits) : 0; break;   // FACGT
                case 0x1f: out = fop2(ebits, x, y, [](auto p, auto q2) { return decltype(p)(2) - p * q2; }); break;  // FRECPS
                case 0x3f:  // FRSQRTS
                    out = fop2(ebits, x, y, [](auto p, auto q2) { return (decltype(p)(3) - p * q2) / decltype(p)(2); });
                    break;
                default: return false;
            }
            set_elem(r, ebits, i, out);
            return true;
        };
        // Pairwise forms: FADDP, FMAXP, FMINP, FMAXNMP, FMINNMP (vector only).
        if (!scalar && (key == 0x5a || key == 0x5e || key == 0x7e || key == 0x58 || key == 0x78)) {
            VReg cat[2] = {a, b};
            const unsigned half = n / 2;
            for (unsigned i = 0; i < n; ++i) {
                const VReg& src = cat[i / half];
                const unsigned j = (i % half) * 2;
                const uint64_t x = elem(src, ebits, j), y = elem(src, ebits, j + 1);
                uint64_t out;
                if (ebits == 32) {
                    const float fx = f32(x), fy = f32(y);
                    out = bits32(key == 0x5a ? fx + fy : key == 0x5e ? fmax_arm(fx, fy) : key == 0x7e ? fmin_arm(fx, fy)
                                 : key == 0x58 ? fmaxnm_arm(fx, fy) : fminnm_arm(fx, fy));
                } else {
                    const double fx = f64(x), fy = f64(y);
                    out = bits64(key == 0x5a ? fx + fy : key == 0x5e ? fmax_arm(fx, fy) : key == 0x7e ? fmin_arm(fx, fy)
                                 : key == 0x58 ? fmaxnm_arm(fx, fy) : fminnm_arm(fx, fy));
                }
                set_elem(r, ebits, i, out);
            }
            write_v(c, rd, r, q ? 16 : 8);
            return true;
        }
        for (unsigned i = 0; i < n; ++i)
            if (!lane(i)) return false;
        write_v(c, rd, r, scalar ? ebits / 8 : (q ? 16 : 8));
        return true;
    }

    if (opcode == 3) {  // bitwise: AND BIC ORR ORN / EOR BSL BIT BIF
        if (scalar) return false;
        for (unsigned i = 0; i < 2; ++i) {
            const uint64_t x = a.d[i], y = b.d[i], z = d.d[i];
            uint64_t out;
            switch ((u << 2) | size) {
                case 0: out = x & y; break;
                case 1: out = x & ~y; break;
                case 2: out = x | y; break;
                case 3: out = x | ~y; break;
                case 4: out = x ^ y; break;
                case 5: out = (z & x) | (~z & y); break;   // BSL
                case 6: out = (y & x) | (~y & z); break;   // BIT
                default: out = (~y & x) | (y & z); break;  // BIF
            }
            r.d[i] = out;
        }
        write_v(c, rd, r, bytes);
        return true;
    }

    const unsigned ebits = 8u << size;
    if (scalar && size != 3 && !(opcode == 1 || opcode == 5 || opcode == 9 || opcode == 11 || opcode == 22))
        return false;  // most scalar integer forms are 64-bit only
    if (!scalar && size == 3 && !q) return false;
    const unsigned n = scalar ? 1 : (q ? 16 : 8) / (ebits / 8);
    const uint64_t m = mask(ebits);

    // Pairwise integer forms: ADDP, SMAXP/UMAXP, SMINP/UMINP.
    if (!scalar && (opcode == 0x17 || opcode == 0x14 || opcode == 0x15)) {
        if (opcode == 0x17 && u) return false;
        VReg cat[2] = {a, b};
        const unsigned half = n / 2;
        for (unsigned i = 0; i < n; ++i) {
            const VReg& src = cat[i / half];
            const unsigned j = (i % half) * 2;
            const uint64_t x = elem(src, ebits, j), y = elem(src, ebits, j + 1);
            uint64_t out;
            if (opcode == 0x17) {
                out = x + y;
            } else {
                const __int128 sx = as_int(x, ebits, u), sy = as_int(y, ebits, u);
                out = static_cast<uint64_t>(opcode == 0x14 ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy));
            }
            set_elem(r, ebits, i, out & m);
        }
        write_v(c, rd, r, q ? 16 : 8);
        return true;
    }

    for (unsigned i = 0; i < n; ++i) {
        const uint64_t x = elem(a, ebits, i), y = elem(b, ebits, i), z = elem(d, ebits, i);
        const __int128 sx = as_int(x, ebits, u), sy = as_int(y, ebits, u);
        uint64_t out;
        switch (opcode) {
            case 0x00: out = static_cast<uint64_t>((sx + sy) >> 1); break;          // SHADD/UHADD
            case 0x01: out = u ? sat_unsigned(sx + sy, ebits) : sat_signed(sx + sy, ebits); break;  // SQADD/UQADD
            case 0x02: out = static_cast<uint64_t>((sx + sy + 1) >> 1); break;      // SRHADD/URHADD
            case 0x04: out = static_cast<uint64_t>((sx - sy) >> 1); break;          // SHSUB/UHSUB
            case 0x05: out = u ? sat_unsigned(sx - sy, ebits) : sat_signed(sx - sy, ebits); break;  // SQSUB/UQSUB
            case 0x06: out = sx > sy ? m : 0; break;                                // CMGT/CMHI
            case 0x07: out = sx >= sy ? m : 0; break;                               // CMGE/CMHS
            case 0x08: out = shl_by_reg(x, y, ebits, u, false, false); break;       // SSHL/USHL
            case 0x09: out = shl_by_reg(x, y, ebits, u, false, true); break;        // SQSHL/UQSHL
            case 0x0a: out = shl_by_reg(x, y, ebits, u, true, false); break;        // SRSHL/URSHL
            case 0x0b: out = shl_by_reg(x, y, ebits, u, true, true); break;         // SQRSHL/UQRSHL
            case 0x0c: out = static_cast<uint64_t>(sx > sy ? sx : sy); break;       // SMAX/UMAX
            case 0x0d: out = static_cast<uint64_t>(sx < sy ? sx : sy); break;       // SMIN/UMIN
            case 0x0e: out = static_cast<uint64_t>(sx > sy ? sx - sy : sy - sx); break;  // SABD/UABD
            case 0x0f: out = z + static_cast<uint64_t>(sx > sy ? sx - sy : sy - sx); break;  // SABA/UABA
            case 0x10: out = u ? x - y : x + y; break;                              // ADD/SUB
            case 0x11: out = u ? (x == y ? m : 0) : ((x & y) ? m : 0); break;       // CMEQ/CMTST
            case 0x12: out = u ? z - x * y : z + x * y; break;                      // MLA/MLS
            case 0x13: {                                                            // MUL/PMUL
                if (!u) {
                    out = x * y;
                } else {
                    if (ebits != 8) return false;
                    uint64_t p = 0;
                    for (unsigned k = 0; k < 8; ++k)
                        if ((y >> k) & 1) p ^= x << k;
                    out = p;
                }
                break;
            }
            case 0x16: {  // SQDMULH/SQRDMULH
                if (ebits != 16 && ebits != 32) return false;
                const __int128 p = 2 * as_int(x, ebits, false) * as_int(y, ebits, false) +
                                   (u ? (static_cast<__int128>(1) << (ebits - 1)) : 0);
                out = sat_signed(p >> ebits, ebits);
                break;
            }
            default:
                return false;
        }
        set_elem(r, ebits, i, out & m);
    }
    write_v(c, rd, r, scalar ? ebits / 8 : bytes);
    return true;
}

// ---- Advanced SIMD: three registers, different types ------------------------

bool three_different(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 12);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (size == 3) return false;
    const unsigned eb = 8u << size, wb = eb * 2;  // narrow and wide element bits
    const unsigned n = 64 / eb;                   // results per instruction
    const unsigned off = q ? n : 0;               // the "2" forms read the upper half
    const VReg a = c.v[rn], b = c.v[rm], d = c.v[rd];
    VReg r{};
    for (unsigned i = 0; i < n; ++i) {
        const __int128 nb = as_int(elem(b, eb, off + i), eb, u);
        const __int128 na = as_int(elem(a, eb, off + i), eb, u);
        const __int128 wa = as_int(elem(a, wb, i), wb, u), wbv = as_int(elem(b, wb, i), wb, u);
        const __int128 acc = as_int(elem(d, wb, i), wb, u);
        uint64_t out;
        switch (opcode) {
            case 0x0: out = static_cast<uint64_t>(na + nb); break;           // SADDL/UADDL
            case 0x1: out = static_cast<uint64_t>(wa + nb); break;           // SADDW/UADDW
            case 0x2: out = static_cast<uint64_t>(na - nb); break;           // SSUBL/USUBL
            case 0x3: out = static_cast<uint64_t>(wa - nb); break;           // SSUBW/USUBW
            case 0x5: out = static_cast<uint64_t>(acc + (na > nb ? na - nb : nb - na)); break;  // SABAL
            case 0x7: out = static_cast<uint64_t>(na > nb ? na - nb : nb - na); break;          // SABDL
            case 0x8: out = static_cast<uint64_t>(acc + na * nb); break;     // SMLAL/UMLAL
            case 0xa: out = static_cast<uint64_t>(acc - na * nb); break;     // SMLSL/UMLSL
            case 0xc: out = static_cast<uint64_t>(na * nb); break;           // SMULL/UMULL
            case 0x4: case 0x6: {  // ADDHN/RADDHN, SUBHN/RSUBHN: narrow, high half
                const __int128 s = opcode == 4 ? wa + wbv : wa - wbv;
                const uint64_t sum = static_cast<uint64_t>(s + (u ? (static_cast<__int128>(1) << (eb - 1)) : 0));
                set_elem(r, eb, off + i, (sum >> eb) & mask(eb));
                continue;
            }
            case 0x9: case 0xb: case 0xd: {  // SQDMLAL, SQDMLSL, SQDMULL
                if (u || (eb != 16 && eb != 32)) return false;
                const __int128 p = static_cast<__int128>(sat_signed(2 * na * nb, wb) & mask(wb));
                const __int128 ps = sext(static_cast<uint64_t>(p), wb);
                if (opcode == 0xd) out = static_cast<uint64_t>(ps);
                else out = sat_signed(opcode == 0x9 ? acc + ps : acc - ps, wb);
                break;
            }
            default:
                return false;
        }
        set_elem(r, wb, i, out & mask(wb));
    }
    if (opcode == 4 || opcode == 6) {
        if (q) {  // the "2" forms keep the lower half of Vd
            r.d[0] = d.d[0];
            write_v(c, rd, r, 16);
        } else {
            write_v(c, rd, r, 8);
        }
        return true;
    }
    write_v(c, rd, r, 16);
    return true;
}

// ---- Advanced SIMD: two-register miscellaneous ------------------------------

bool two_reg_misc(Cpu& c, uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const VReg a = c.v[rn], d = c.v[rd];
    VReg r{};
    const unsigned total = scalar ? 0 : (q ? 16 : 8);

    // Floating-point forms: opcode 0b01100..0b01111 with size<1> = 1, and
    // 0b10110..0b11111.
    const bool fp = (opcode >= 0x0c && opcode <= 0x0f && (size & 2)) || opcode >= 0x16;
    if (fp) {
        const unsigned ebits = (size & 1) ? 64 : 32;
        const bool hi = size & 2;
        // Narrowing/widening conversions first.
        if (opcode == 0x16 && !scalar) {  // FCVTN/FCVTN2 (and FCVTXN as FCVTN)
            if (hi) return false;
            VReg out = q ? d : VReg{};
            const unsigned base = q ? (size & 1 ? 2 : 4) : 0;
            if (size & 1) {
                for (unsigned i = 0; i < 2; ++i) out.s[base + i] = uint32_t(bits32(float(a.df[i])));
            } else {
                for (unsigned i = 0; i < 4; ++i) out.h[base + i] = float_to_half(a.f[i]);
            }
            write_v(c, rd, out, q ? 16 : 8);
            return true;
        }
        if (opcode == 0x17 && !u && !scalar) {  // FCVTL/FCVTL2
            if (hi) return false;
            const unsigned base = q ? (size & 1 ? 2 : 4) : 0;
            if (size & 1) {
                for (unsigned i = 0; i < 2; ++i) r.df[i] = double(a.f[base + i]);
            } else {
                for (unsigned i = 0; i < 4; ++i) r.f[i] = half_to_float(a.h[base + i]);
            }
            write_v(c, rd, r, 16);
            return true;
        }
        if (!scalar && ebits == 64 && !q) return false;
        const unsigned n = scalar ? 1 : (q ? 16 : 8) * 8 / ebits;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t x = elem(a, ebits, i);
            const double fx = fget(ebits, x);
            uint64_t out;
            const unsigned key = (u << 6) | (hi << 5) | opcode;
            switch (key) {
                case 0x2c: out = fx > 0 ? mask(ebits) : 0; break;    // FCMGT #0
                case 0x2d: out = fx == 0 ? mask(ebits) : 0; break;   // FCMEQ #0
                case 0x2e: out = fx < 0 ? mask(ebits) : 0; break;    // FCMLT #0
                case 0x6c: out = fx >= 0 ? mask(ebits) : 0; break;   // FCMGE #0
                case 0x6d: out = fx <= 0 ? mask(ebits) : 0; break;   // FCMLE #0
                case 0x2f: out = x & ~(uint64_t{1} << (ebits - 1)); break;  // FABS
                case 0x6f: out = x ^ (uint64_t{1} << (ebits - 1)); break;   // FNEG
                case 0x7f: out = fop1(ebits, x, [](auto p) { return std::sqrt(p); }); break;  // FSQRT
                case 0x18: out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kNearestEven); }); break;  // FRINTN
                case 0x38: out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kPlusInf); }); break;  // FRINTP
                case 0x19: out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kMinusInf); }); break;  // FRINTM
                case 0x39: out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kZero); }); break;  // FRINTZ
                case 0x58: out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kNearestAway); }); break;  // FRINTA
                case 0x59: case 0x79:  // FRINTX, FRINTI
                    out = fop1(ebits, x, [](auto p) { return round_to_integral(p, kNearestEven); });
                    break;
                case 0x1a: case 0x5a: out = ebits == 32 ? fp_to_int(f32(x), 32, u, kNearestEven) : fp_to_int(fx, 64, u, kNearestEven); break;  // FCVTNS/NU
                case 0x3a: case 0x7a: out = ebits == 32 ? fp_to_int(f32(x), 32, u, kPlusInf) : fp_to_int(fx, 64, u, kPlusInf); break;  // FCVTPS/PU
                case 0x1b: case 0x5b: out = ebits == 32 ? fp_to_int(f32(x), 32, u, kMinusInf) : fp_to_int(fx, 64, u, kMinusInf); break;  // FCVTMS/MU
                case 0x3b: case 0x7b: out = ebits == 32 ? fp_to_int(f32(x), 32, u, kZero) : fp_to_int(fx, 64, u, kZero); break;  // FCVTZS/ZU
                case 0x1c: case 0x5c: out = ebits == 32 ? fp_to_int(f32(x), 32, u, kNearestAway) : fp_to_int(fx, 64, u, kNearestAway); break;  // FCVTAS/AU
                case 0x1d: case 0x5d:  // SCVTF, UCVTF
                    out = ebits == 32 ? bits32(int_to_fp<float>(x, 32, u)) : bits64(int_to_fp<double>(x, 64, u));
                    break;
                case 0x3d: out = fop1(ebits, x, [](auto p) { return decltype(p)(1) / p; }); break;  // FRECPE
                case 0x7d: out = fop1(ebits, x, [](auto p) { return decltype(p)(1) / std::sqrt(p); }); break;  // FRSQRTE
                case 0x3f: out = fop1(ebits, x, [](auto p) { return decltype(p)(1) / p; }); break;  // FRECPX (approx.)
                default: return false;
            }
            set_elem(r, ebits, i, out);
        }
        write_v(c, rd, r, scalar ? ebits / 8 : total);
        return true;
    }

    const unsigned ebits = 8u << size;
    const uint64_t m = mask(ebits);
    // Narrowing: XTN, SQXTN, UQXTN, SQXTUN (vector and scalar).
    if (opcode == 0x12 || opcode == 0x14) {
        if (size == 3) return false;
        const unsigned wb = ebits * 2;
        const unsigned n = scalar ? 1 : 64 / ebits;
        VReg out = (!scalar && q) ? d : VReg{};
        const unsigned base = (!scalar && q) ? n : 0;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t w = elem(a, wb, i);
            uint64_t v;
            if (opcode == 0x12 && !u) v = w & m;                                     // XTN
            else if (opcode == 0x12) v = sat_unsigned(sext(w, wb), ebits);           // SQXTUN
            else if (!u) v = sat_signed(sext(w, wb), ebits);                         // SQXTN
            else v = sat_unsigned(static_cast<__int128>(w & mask(wb)), ebits);       // UQXTN
            set_elem(out, ebits, base + i, v);
        }
        write_v(c, rd, out, scalar ? ebits / 8 : (q ? 16 : 8));
        return true;
    }
    if (opcode == 0x13 && u && !scalar) {  // SHLL/SHLL2
        if (size == 3) return false;
        const unsigned n = 64 / ebits, base = q ? n : 0;
        for (unsigned i = 0; i < n; ++i) set_elem(r, ebits * 2, i, elem(a, ebits, base + i) << ebits);
        write_v(c, rd, r, 16);
        return true;
    }
    if ((opcode == 0x02 || opcode == 0x06) && !scalar) {  // SADDLP/UADDLP, SADALP/UADALP
        if (size == 3) return false;
        const unsigned wb = ebits * 2, n = (q ? 16 : 8) * 8 / wb;
        for (unsigned i = 0; i < n; ++i) {
            __int128 s = as_int(elem(a, ebits, 2 * i), ebits, u) + as_int(elem(a, ebits, 2 * i + 1), ebits, u);
            if (opcode == 0x06) s += as_int(elem(d, wb, i), wb, u);
            set_elem(r, wb, i, static_cast<uint64_t>(s) & mask(wb));
        }
        write_v(c, rd, r, q ? 16 : 8);
        return true;
    }
    if (opcode <= 0x01 && !scalar) {  // REV64 / REV32 / REV16
        const unsigned group = opcode == 1 ? 16 : (u ? 32 : 64);  // container bits
        if (ebits >= group) return false;
        const unsigned per = group / ebits, n = (q ? 16 : 8) * 8 / ebits;
        for (unsigned i = 0; i < n; ++i) {
            const unsigned g = i / per, k = i % per;
            set_elem(r, ebits, i, elem(a, ebits, g * per + (per - 1 - k)));
        }
        write_v(c, rd, r, q ? 16 : 8);
        return true;
    }
    if (u && opcode == 0x05 && size == 1) {  // RBIT (vector, per byte)
        if (scalar) return false;
        for (unsigned j = 0; j < (q ? 16u : 8u); ++j) {
            uint8_t rb = 0;
            for (unsigned k = 0; k < 8; ++k) rb |= uint8_t(((a.b[j] >> k) & 1) << (7 - k));
            r.b[j] = rb;
        }
        write_v(c, rd, r, q ? 16 : 8);
        return true;
    }
    if (scalar && size != 3 && opcode != 0x07 && opcode != 0x03) return false;
    if (!scalar && size == 3 && !q) return false;
    const unsigned n = scalar ? 1 : (q ? 16 : 8) / (ebits / 8);
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t x = elem(a, ebits, i);
        const int64_t sx = sext(x, ebits);
        uint64_t out;
        switch ((u << 5) | opcode) {
            case 0x04: {  // CLS
                const uint64_t t = (x ^ (x << 1)) & m;
                out = t == 0 ? ebits - 1 : uint64_t(__builtin_clzll(t) - (64 - ebits));
                break;
            }
            case 0x24: out = x == 0 ? ebits : uint64_t(__builtin_clzll(x) - (64 - ebits)); break;  // CLZ
            case 0x05: if (size) return false; out = uint64_t(__builtin_popcountll(x)); break;    // CNT
            case 0x25: if (size) return false; out = ~x; break;  // NOT
            case 0x08: out = sx > 0 ? m : 0; break;   // CMGT #0
            case 0x09: out = sx == 0 ? m : 0; break;  // CMEQ #0
            case 0x0a: out = sx < 0 ? m : 0; break;   // CMLT #0
            case 0x28: out = sx >= 0 ? m : 0; break;  // CMGE #0
            case 0x29: out = sx <= 0 ? m : 0; break;  // CMLE #0
            case 0x0b: out = uint64_t(sx < 0 ? -sx : sx); break;  // ABS
            case 0x2b: out = uint64_t(-sx); break;                // NEG
            case 0x07: out = sat_signed(sx < 0 ? -static_cast<__int128>(sx) : sx, ebits); break;  // SQABS
            case 0x27: out = sat_signed(-static_cast<__int128>(sx), ebits); break;                // SQNEG
            case 0x03:  // SUQADD
                out = sat_signed(static_cast<__int128>(sext(elem(d, ebits, i), ebits)) + static_cast<__int128>(x & m), ebits);
                break;
            case 0x23:  // USQADD
                out = sat_unsigned(static_cast<__int128>(elem(d, ebits, i) & m) + sx, ebits);
                break;
            default: return false;
        }
        set_elem(r, ebits, i, out & m);
    }
    write_v(c, rd, r, scalar ? ebits / 8 : total);
    return true;
}

// ---- Advanced SIMD: across lanes and scalar pairwise ------------------------

bool across_lanes(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const VReg a = c.v[rn];
    VReg r{};
    if (opcode == 0x0c || opcode == 0x0f) {  // FMAXNMV/FMINNMV/FMAXV/FMINV (single precision, Q=1)
        if (!u || (size & 1) || !q) return false;
        const bool is_min = size & 2;
        float acc = a.f[0];
        for (unsigned i = 1; i < 4; ++i) {
            if (opcode == 0x0c) acc = is_min ? fminnm_arm(acc, a.f[i]) : fmaxnm_arm(acc, a.f[i]);
            else acc = is_min ? fmin_arm(acc, a.f[i]) : fmax_arm(acc, a.f[i]);
        }
        r.f[0] = acc;
        write_v(c, rd, r, 4);
        return true;
    }
    if (size == 3 || (size == 2 && !q)) return false;
    const unsigned ebits = 8u << size, n = (q ? 16 : 8) / (ebits / 8);
    __int128 acc = as_int(elem(a, ebits, 0), ebits, u);
    for (unsigned i = 1; i < n; ++i) {
        const __int128 x = as_int(elem(a, ebits, i), ebits, u);
        switch (opcode) {
            case 0x03: case 0x1b: acc += x; break;            // SADDLV/UADDLV, ADDV
            case 0x0a: if (x > acc) acc = x; break;           // SMAXV/UMAXV
            case 0x1a: if (x < acc) acc = x; break;           // SMINV/UMINV
            default: return false;
        }
    }
    if (opcode == 0x1b && u) return false;
    const unsigned obits = opcode == 0x03 ? ebits * 2 : ebits;
    set_elem(r, obits, 0, static_cast<uint64_t>(acc) & mask(obits));
    write_v(c, rd, r, obits / 8);
    return true;
}

bool scalar_pairwise(Cpu& c, uint32_t insn) {
    const bool u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const VReg a = c.v[field(insn, 9, 5)];
    const unsigned rd = field(insn, 4, 0);
    VReg r{};
    if (!u && opcode == 0x1b && size == 3) {  // ADDP Dd, Vn.2D
        r.d[0] = a.d[0] + a.d[1];
        write_v(c, rd, r, 8);
        return true;
    }
    if (!u) return false;
    const bool dbl = size & 1, is_min = size & 2;
    if (dbl) {
        const double x = a.df[0], y = a.df[1];
        double out;
        switch (opcode) {
            case 0x0d: if (is_min) return false; out = x + y; break;  // FADDP
            case 0x0c: out = is_min ? fminnm_arm(x, y) : fmaxnm_arm(x, y); break;
            case 0x0f: out = is_min ? fmin_arm(x, y) : fmax_arm(x, y); break;
            default: return false;
        }
        r.df[0] = out;
        write_v(c, rd, r, 8);
    } else {
        const float x = a.f[0], y = a.f[1];
        float out;
        switch (opcode) {
            case 0x0d: if (is_min) return false; out = x + y; break;
            case 0x0c: out = is_min ? fminnm_arm(x, y) : fmaxnm_arm(x, y); break;
            case 0x0f: out = is_min ? fmin_arm(x, y) : fmax_arm(x, y); break;
            default: return false;
        }
        r.f[0] = out;
        write_v(c, rd, r, 4);
    }
    return true;
}

// ---- Advanced SIMD: shift by immediate (vector and scalar) ------------------

bool shift_immediate(Cpu& c, uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned immh = field(insn, 22, 19), immb = field(insn, 18, 16), opcode = field(insn, 15, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (immh == 0) return false;
    const unsigned ebits = 8u << (31 - __builtin_clz(immh));
    const unsigned immhb = (immh << 3) | immb;
    const unsigned rshift = 2 * ebits - immhb, lshift = immhb - ebits;
    const VReg a = c.v[rn], d = c.v[rd];
    VReg r{};
    const uint64_t m = mask(ebits);

    // Narrowing right shifts: SHRN, RSHRN, SQSHRN, SQRSHRN, UQSHRN, UQRSHRN, SQSHRUN, SQRSHRUN.
    if (opcode >= 0x10 && opcode <= 0x13) {
        if (ebits == 64) return false;
        const unsigned wb = ebits * 2, nb = ebits;  // immh names the narrow size
        const unsigned rs = 2 * nb - immhb;
        const unsigned n = scalar ? 1 : 64 / nb;
        VReg out = (!scalar && q) ? d : VReg{};
        const unsigned base = (!scalar && q) ? n : 0;
        const bool round = opcode & 1;
        // SHRN/RSHRN (opcode 0x10/0x11, U=0) and UQSHRN/UQRSHRN (0x12/0x13,
        // U=1) read the wide source as unsigned; SQSHRUN/SQRSHRUN (0x10/0x11,
        // U=1) and SQSHRN/SQRSHRN (0x12/0x13, U=0) as signed.
        const bool src_signed = (opcode <= 0x11) == u;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t w = elem(a, wb, i);
            __int128 v = src_signed ? static_cast<__int128>(sext(w, wb)) : static_cast<__int128>(w & mask(wb));
            if (round) v += static_cast<__int128>(1) << (rs - 1);
            v >>= rs;
            uint64_t res;
            if (opcode <= 0x11 && !u) res = static_cast<uint64_t>(v) & mask(nb);          // SHRN, RSHRN
            else if (opcode <= 0x11) res = sat_unsigned(v, nb);                          // SQSHRUN, SQRSHRUN
            else if (u) res = sat_unsigned(v, nb);                                       // UQSHRN, UQRSHRN
            else res = sat_signed(v, nb);                                                // SQSHRN, SQRSHRN
            set_elem(out, nb, base + i, res);
        }
        write_v(c, rd, out, scalar ? nb / 8 : (q ? 16 : 8));
        return true;
    }
    if (opcode == 0x14 && !scalar) {  // SSHLL/USHLL (SXTL/UXTL), and the "2" forms
        if (ebits == 64) return false;
        const unsigned n = 64 / ebits, base = q ? n : 0;
        for (unsigned i = 0; i < n; ++i) {
            const __int128 v = as_int(elem(a, ebits, base + i), ebits, u);
            set_elem(r, ebits * 2, i, static_cast<uint64_t>(v << lshift) & mask(ebits * 2));
        }
        write_v(c, rd, r, 16);
        return true;
    }
    if (opcode == 0x1c || opcode == 0x1f) {  // SCVTF/UCVTF (fixed), FCVTZS/FCVTZU (fixed)
        if (ebits < 32) return false;
        if (!scalar && ebits == 64 && !q) return false;
        const unsigned n = scalar ? 1 : (q ? 16 : 8) * 8 / ebits;
        const int fbits = int(rshift);
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t x = elem(a, ebits, i);
            uint64_t out;
            if (opcode == 0x1c) {
                out = ebits == 32 ? bits32(int_to_fp<float>(x, 32, u, fbits)) : bits64(int_to_fp<double>(x, 64, u, fbits));
            } else {
                out = ebits == 32 ? fp_to_int(f32(x), 32, u, kZero, fbits) : fp_to_int(f64(x), 64, u, kZero, fbits);
            }
            set_elem(r, ebits, i, out);
        }
        write_v(c, rd, r, scalar ? ebits / 8 : (q ? 16 : 8));
        return true;
    }
    if (scalar && ebits != 64 && opcode != 0x0e && opcode != 0x0c) return false;
    if (!scalar && ebits == 64 && !q) return false;
    const unsigned n = scalar ? 1 : (q ? 16 : 8) / (ebits / 8);
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t x = elem(a, ebits, i), z = elem(d, ebits, i);
        const __int128 v = as_int(x, ebits, u);
        uint64_t out;
        auto shr = [&](bool round) -> uint64_t {
            __int128 t = v;
            if (round) t += static_cast<__int128>(1) << (rshift - 1);
            return static_cast<uint64_t>(t >> rshift);
        };
        switch (opcode) {
            case 0x00: out = shr(false); break;           // SSHR/USHR
            case 0x02: out = z + shr(false); break;       // SSRA/USRA
            case 0x04: out = shr(true); break;            // SRSHR/URSHR
            case 0x06: out = z + shr(true); break;        // SRSRA/URSRA
            case 0x08: {                                  // SRI
                if (!u) return false;
                const uint64_t keep = rshift == ebits ? m : ~(m >> rshift) & m;
                out = (z & keep) | (rshift == ebits ? 0 : ((x & m) >> rshift));
                break;
            }
            case 0x0a:                                    // SHL / SLI
                if (!u) {
                    out = x << lshift;
                } else {
                    const uint64_t keep = mask(lshift);
                    out = (z & keep) | (x << lshift);
                }
                break;
            case 0x0c:                                    // SQSHLU
                if (!u) return false;
                out = sat_unsigned(static_cast<__int128>(sext(x, ebits)) << lshift, ebits);
                break;
            case 0x0e:                                    // SQSHL/UQSHL (immediate)
                out = u ? sat_unsigned(v << lshift, ebits) : sat_signed(v << lshift, ebits);
                break;
            default:
                return false;
        }
        set_elem(r, ebits, i, out & m);
    }
    write_v(c, rd, r, scalar ? ebits / 8 : (q ? 16 : 8));
    return true;
}

// ---- Advanced SIMD: by element ----------------------------------------------

bool by_element(Cpu& c, uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 12);
    const unsigned l = bit(insn, 21), mbit = bit(insn, 20), h = bit(insn, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const VReg a = c.v[rn], d = c.v[rd];
    VReg r{};
    const bool fp_op = opcode == 0x1 || opcode == 0x5 || opcode == 0x9;
    unsigned rm = field(insn, 19, 16), index;
    if (fp_op) {
        if (size < 2) return false;  // half precision
        const bool dbl = size & 1;
        if (dbl && l) return false;
        index = dbl ? h : (h << 1) | l;
        rm |= mbit << 4;
        const unsigned ebits = dbl ? 64 : 32;
        if (!scalar && dbl && !q) return false;
        const unsigned n = scalar ? 1 : (q ? 16 : 8) * 8 / ebits;
        const VReg b = c.v[rm];
        for (unsigned i = 0; i < n; ++i) {
            uint64_t out;
            if (dbl) {
                const double x = a.df[i], y = b.df[index], z = d.df[i];
                const double res = opcode == 1 ? std::fma(x, y, z) : opcode == 5 ? std::fma(-x, y, z) : x * y;
                out = bits64(res);
            } else {
                const float x = a.f[i], y = b.f[index], z = d.f[i];
                const float res = opcode == 1 ? std::fma(x, y, z) : opcode == 5 ? std::fma(-x, y, z) : x * y;
                out = bits32(res);
            }
            set_elem(r, ebits, i, out);
        }
        write_v(c, rd, r, scalar ? ebits / 8 : (q ? 16 : 8));
        return true;
    }
    unsigned ebits;
    if (size == 1) {
        index = (h << 2) | (l << 1) | mbit;
        ebits = 16;
    } else if (size == 2) {
        index = (h << 1) | l;
        rm |= mbit << 4;
        ebits = 32;
    } else {
        return false;
    }
    const VReg b = c.v[rm];
    const __int128 y = as_int(elem(b, ebits, index), ebits, u);
    if (opcode == 0x2 || opcode == 0x6 || opcode == 0xa || opcode == 0x3 || opcode == 0x7 || opcode == 0xb) {
        if (scalar && !(opcode == 0x3 || opcode == 0x7 || opcode == 0xb)) return false;
        // Long forms: SMLAL/UMLAL, SMLSL/UMLSL, SMULL/UMULL (+ "2"), SQDMLAL, SQDMLSL, SQDMULL.
        const unsigned wb = ebits * 2, n = scalar ? 1 : 64 / ebits, base = (!scalar && q) ? n : 0;
        for (unsigned i = 0; i < n; ++i) {
            const __int128 x = as_int(elem(a, ebits, base + i), ebits, u && (opcode & 1) == 0);
            const __int128 acc = as_int(elem(d, wb, i), wb, u && (opcode & 1) == 0);
            uint64_t out;
            switch (opcode) {
                case 0x2: out = static_cast<uint64_t>(acc + x * y); break;
                case 0x6: out = static_cast<uint64_t>(acc - x * y); break;
                case 0xa: out = static_cast<uint64_t>(x * y); break;
                default: {
                    if (u) return false;
                    const __int128 p = sext(sat_signed(2 * x * y, wb), wb);
                    out = opcode == 0xb ? static_cast<uint64_t>(p) : sat_signed(opcode == 0x3 ? acc + p : acc - p, wb);
                    break;
                }
            }
            set_elem(r, wb, i, out & mask(wb));
        }
        write_v(c, rd, r, scalar ? wb / 8 : 16);
        return true;
    }
    const unsigned n = scalar ? 1 : (q ? 16 : 8) / (ebits / 8);
    for (unsigned i = 0; i < n; ++i) {
        const uint64_t x = elem(a, ebits, i), z = elem(d, ebits, i);
        const uint64_t yy = static_cast<uint64_t>(y);
        uint64_t out;
        switch (opcode) {
            case 0x0: if (!u) return false; out = z + x * yy; break;  // MLA
            case 0x4: if (!u) return false; out = z - x * yy; break;  // MLS
            case 0x8: if (u) return false; out = x * yy; break;       // MUL
            case 0xc: case 0xd: {                                     // SQDMULH, SQRDMULH
                const __int128 p = 2 * as_int(x, ebits, false) * as_int(yy, ebits, false) +
                                   (opcode == 0xd ? (static_cast<__int128>(1) << (ebits - 1)) : 0);
                out = sat_signed(p >> ebits, ebits);
                break;
            }
            default: return false;
        }
        set_elem(r, ebits, i, out & mask(ebits));
    }
    write_v(c, rd, r, scalar ? ebits / 8 : (q ? 16 : 8));
    return true;
}

// ---- Advanced SIMD: copy, permute, extract, table, modified immediate -------

bool copy(Cpu& c, uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), op = bit(insn, 29);
    const unsigned imm5 = field(insn, 20, 16), imm4 = field(insn, 14, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if ((imm5 & 0xf) == 0) return false;
    const unsigned sz = __builtin_ctz(imm5);  // 0 B, 1 H, 2 S, 3 D
    const unsigned ebits = 8u << sz, index = imm5 >> (sz + 1);
    if (scalar) {  // DUP (element), scalar: MOV Bd/Hd/Sd/Dd, Vn.T[index]
        if (op || imm4 != 0) return false;
        VReg r{};
        set_elem(r, ebits, 0, elem(c.v[rn], ebits, index));
        write_v(c, rd, r, ebits / 8);
        return true;
    }
    if (op) {  // INS (element)
        if (!q) return false;
        const unsigned index2 = imm4 >> sz;
        VReg r = c.v[rd];
        set_elem(r, ebits, index, elem(c.v[rn], ebits, index2));
        c.v[rd] = r;
        return true;
    }
    VReg r{};
    switch (imm4) {
        case 0: {  // DUP (element)
            if (sz == 3 && !q) return false;
            const uint64_t v = elem(c.v[rn], ebits, index);
            for (unsigned i = 0; i < (q ? 16 : 8) * 8 / ebits; ++i) set_elem(r, ebits, i, v);
            write_v(c, rd, r, q ? 16 : 8);
            return true;
        }
        case 1: {  // DUP (general)
            if (sz == 3 && !q) return false;
            const uint64_t v = get_x(c, rn) & mask(ebits);
            for (unsigned i = 0; i < (q ? 16 : 8) * 8 / ebits; ++i) set_elem(r, ebits, i, v);
            write_v(c, rd, r, q ? 16 : 8);
            return true;
        }
        case 3: {  // INS (general)
            r = c.v[rd];
            set_elem(r, ebits, index, get_x(c, rn));
            c.v[rd] = r;
            return true;
        }
        case 5: {  // SMOV
            const int64_t v = sext(elem(c.v[rn], ebits, index), ebits);
            set_x(c, rd, q ? static_cast<uint64_t>(v) : uint32_t(v));
            return true;
        }
        case 7:  // UMOV
            set_x(c, rd, elem(c.v[rn], ebits, index));
            return true;
        default:
            return false;
    }
}

bool permute(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 14, 12);
    const VReg a = c.v[field(insn, 9, 5)], b = c.v[field(insn, 20, 16)];
    const unsigned rd = field(insn, 4, 0);
    if (size == 3 && !q) return false;
    const unsigned ebits = 8u << size, n = (q ? 16 : 8) * 8 / ebits, half = n / 2;
    VReg r{};
    const unsigned part = (opcode >> 2) & 1;  // 1 for the "2" variants
    for (unsigned i = 0; i < n; ++i) {
        uint64_t v;
        switch (opcode & 3) {
            case 1: {  // UZP1/UZP2
                const unsigned src = 2 * i + part;
                v = src < n ? elem(a, ebits, src) : elem(b, ebits, src - n);
                break;
            }
            case 2: {  // TRN1/TRN2
                const unsigned pair = i & ~1u;
                v = (i & 1) ? elem(b, ebits, pair + part) : elem(a, ebits, pair + part);
                break;
            }
            case 3: {  // ZIP1/ZIP2
                const unsigned src = part * half + i / 2;
                v = (i & 1) ? elem(b, ebits, src) : elem(a, ebits, src);
                break;
            }
            default:
                return false;
        }
        set_elem(r, ebits, i, v);
    }
    write_v(c, rd, r, q ? 16 : 8);
    return true;
}

bool extract(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30);
    const unsigned imm4 = field(insn, 14, 11);
    if (!q && imm4 >= 8) return false;
    const VReg a = c.v[field(insn, 9, 5)], b = c.v[field(insn, 20, 16)];
    const unsigned bytes = q ? 16 : 8;
    uint8_t cat[32];
    std::memcpy(cat, a.b, bytes);
    std::memcpy(cat + bytes, b.b, bytes);
    VReg r{};
    std::memcpy(r.b, cat + imm4, bytes);
    write_v(c, field(insn, 4, 0), r, bytes);
    return true;
}

bool table_lookup(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30), tbx = bit(insn, 12);
    const unsigned len = field(insn, 14, 13) + 1, rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    uint8_t table[64];
    for (unsigned i = 0; i < len; ++i) std::memcpy(table + 16 * i, c.v[(rn + i) % 32].b, 16);
    const VReg idx = c.v[field(insn, 20, 16)];
    VReg r = tbx ? c.v[rd] : VReg{};
    const unsigned bytes = q ? 16 : 8;
    for (unsigned i = 0; i < bytes; ++i) {
        const unsigned k = idx.b[i];
        if (k < 16 * len) r.b[i] = table[k];
        else if (!tbx) r.b[i] = 0;
    }
    write_v(c, rd, r, bytes);
    return true;
}

bool modified_immediate(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30), op = bit(insn, 29);
    const unsigned cmode = field(insn, 15, 12), rd = field(insn, 4, 0);
    if (bit(insn, 11)) return false;  // FMOV (vector, half precision)
    const uint64_t imm8 = (field(insn, 18, 16) << 5) | field(insn, 9, 5);
    uint64_t imm;
    switch (cmode >> 1) {
        case 0: imm = imm8; imm |= imm << 32; break;
        case 1: imm = imm8 << 8; imm |= imm << 32; break;
        case 2: imm = imm8 << 16; imm |= imm << 32; break;
        case 3: imm = imm8 << 24; imm |= imm << 32; break;
        case 4: imm = imm8; imm |= imm << 16; imm |= imm << 32; break;
        case 5: imm = imm8 << 8; imm |= imm << 16; imm |= imm << 32; break;
        case 6:
            imm = (cmode & 1) ? ((imm8 << 16) | 0xffff) : ((imm8 << 8) | 0xff);
            imm |= imm << 32;
            break;
        default:
            if (!(cmode & 1) && !op) {
                imm = imm8 * 0x0101010101010101ull;
            } else if (!(cmode & 1) && op) {
                imm = 0;
                for (unsigned i = 0; i < 8; ++i)
                    if ((imm8 >> i) & 1) imm |= uint64_t{0xff} << (8 * i);
            } else if (!op) {
                imm = vfp_expand_imm(unsigned(imm8), 32);
                imm |= imm << 32;
            } else {
                if (!q) return false;
                imm = vfp_expand_imm(unsigned(imm8), 64);
            }
            break;
    }
    VReg r = c.v[rd];
    const bool orr_bic = (cmode < 12) && (cmode & 1);
    for (unsigned i = 0; i < 2; ++i) {
        if (orr_bic) r.d[i] = op ? (r.d[i] & ~imm) : (r.d[i] | imm);
        else if (op && cmode != 14 && cmode != 15) r.d[i] = ~imm;  // MVNI
        else r.d[i] = imm;
    }
    write_v(c, rd, r, q ? 16 : 8);
    return true;
}

// ---- scalar floating point --------------------------------------------------

// The value of FP register n as the given precision (type: 0 single, 1 double, 3 half).
double fp_read(const Cpu& c, unsigned n, unsigned type) {
    switch (type) {
        case 0: return c.v[n].f[0];
        case 1: return c.v[n].df[0];
        default: return half_to_float(c.v[n].h[0]);
    }
}
void fp_write(Cpu& c, unsigned n, unsigned type, double v) {
    VReg r{};
    switch (type) {
        case 0: r.f[0] = float(v); break;
        case 1: r.df[0] = v; break;
        default: r.h[0] = float_to_half(float(v)); break;
    }
    c.v[n] = r;
}
// Single precision arithmetic must round to single at each step, not via
// double: compute in the operand's own type.
template <typename Fn> void fp_binary(Cpu& c, unsigned type, unsigned rd, unsigned rn, unsigned rm, Fn fn) {
    VReg r{};
    if (type == 0) r.f[0] = fn(c.v[rn].f[0], c.v[rm].f[0]);
    else if (type == 1) r.df[0] = fn(c.v[rn].df[0], c.v[rm].df[0]);
    else r.h[0] = float_to_half(fn(half_to_float(c.v[rn].h[0]), half_to_float(c.v[rm].h[0])));
    c.v[rd] = r;
}

bool fp_data(Cpu& c, uint32_t insn) {
    const unsigned type = field(insn, 23, 22);
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16);
    if (field(insn, 15, 10) == 0) {  // conversions between FP and integer
        const bool sf = bit(insn, 31);
        const unsigned rmode = field(insn, 20, 19), opcode = field(insn, 18, 16);
        const unsigned ibits = sf ? 64 : 32;
        if (opcode == 6 || opcode == 7) {  // FMOV general <-> FP
            // W<->S, X<->D, W/X<->H, and X<->V.D[1] (type 10, rmode 01).
            if (type == 2) {
                if (!sf || rmode != 1) return false;
            } else if (rmode != 0 || (type == 0 && sf) || (type == 1 && !sf)) {
                return false;
            }
            if (opcode == 6) {
                if (rmode == 1) set_x(c, rd, c.v[rn].d[1]);
                else if (type == 3) set_x(c, rd, c.v[rn].h[0]);
                else set_x(c, rd, sf ? c.v[rn].d[0] : c.v[rn].s[0]);
            } else {
                if (rmode == 1) {
                    c.v[rd].d[1] = get_x(c, rn);
                } else {
                    VReg r{};
                    if (type == 3) r.h[0] = uint16_t(get_x(c, rn));
                    else if (sf) r.d[0] = get_x(c, rn);
                    else r.s[0] = uint32_t(get_x(c, rn));
                    c.v[rd] = r;
                }
            }
            return true;
        }
        if (type == 2) return false;
        if (opcode == 2 || opcode == 3) {  // SCVTF, UCVTF
            if (rmode != 0) return false;
            const bool uns = opcode == 3;
            const uint64_t x = get_x(c, rn);
            VReg r{};
            if (type == 0) r.f[0] = int_to_fp<float>(x, ibits, uns);
            else if (type == 1) r.df[0] = int_to_fp<double>(x, ibits, uns);
            else r.h[0] = float_to_half(int_to_fp<float>(x, ibits, uns));
            c.v[rd] = r;
            return true;
        }
        // FCVT{N,P,M,Z}{S,U} (opcode 0/1 with rmode), FCVTA{S,U} (opcode 4/5, rmode 0)
        int mode;
        bool uns;
        if (opcode <= 1) {
            static const int kModes[4] = {kNearestEven, kPlusInf, kMinusInf, kZero};
            mode = kModes[rmode];
            uns = opcode == 1;
        } else if (opcode == 4 || opcode == 5) {
            if (rmode != 0) return false;
            mode = kNearestAway;
            uns = opcode == 5;
        } else {
            return false;
        }
        uint64_t out;
        if (type == 0) out = fp_to_int(c.v[rn].f[0], ibits, uns, mode);
        else if (type == 1) out = fp_to_int(c.v[rn].df[0], ibits, uns, mode);
        else out = fp_to_int(half_to_float(c.v[rn].h[0]), ibits, uns, mode);
        set_x(c, rd, out);
        return true;
    }

    if (type == 2) return false;
    switch (field(insn, 11, 10)) {
        case 1: {  // FCCMP, FCCMPE
            if (cond_holds(c.nzcv, field(insn, 15, 12))) {
                c.nzcv = fcmp_flags(fp_read(c, rn, type), fp_read(c, rm, type));
            } else {
                c.nzcv = field(insn, 3, 0) << 28;
            }
            return true;
        }
        case 2: {  // two-source
            switch (field(insn, 15, 12)) {
                case 0: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return a * b; }); return true;
                case 1: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return a / b; }); return true;
                case 2: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return a + b; }); return true;
                case 3: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return a - b; }); return true;
                case 4: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return fmax_arm(a, b); }); return true;
                case 5: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return fmin_arm(a, b); }); return true;
                case 6: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return fmaxnm_arm(a, b); }); return true;
                case 7: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return fminnm_arm(a, b); }); return true;
                case 8: fp_binary(c, type, rd, rn, rm, [](auto a, auto b) { return -(a * b); }); return true;
                default: return false;
            }
        }
        case 3: {  // FCSEL
            const unsigned src = cond_holds(c.nzcv, field(insn, 15, 12)) ? rn : rm;
            VReg r{};
            const unsigned bytes = type == 0 ? 4 : type == 1 ? 8 : 2;
            std::memcpy(r.b, c.v[src].b, bytes);
            c.v[rd] = r;
            return true;
        }
        default: break;
    }
    if (field(insn, 12, 10) == 4) {  // FMOV (scalar, immediate)
        if (field(insn, 9, 5) != 0) return false;
        const unsigned imm8 = field(insn, 20, 13);
        VReg r{};
        if (type == 0) r.s[0] = uint32_t(vfp_expand_imm(imm8, 32));
        else if (type == 1) r.d[0] = vfp_expand_imm(imm8, 64);
        else r.h[0] = uint16_t(vfp_expand_imm(imm8, 16));
        c.v[rd] = r;
        return true;
    }
    if (field(insn, 13, 10) == 8) {  // FCMP, FCMPE
        const bool zero = bit(insn, 3);
        c.nzcv = fcmp_flags(fp_read(c, rn, type), zero ? 0.0 : fp_read(c, rm, type));
        return true;
    }
    if (field(insn, 14, 10) == 0x10) {  // one-source
        const unsigned opcode = field(insn, 20, 15);
        if (opcode >= 4 && opcode <= 7) {  // FCVT between precisions
            const unsigned to = opcode & 3;
            if (to == type || to == 2) return false;
            fp_write(c, rd, to, fp_read(c, rn, type));
            return true;
        }
        VReg r{};
        auto un = [&](auto fn) {
            if (type == 0) r.f[0] = fn(c.v[rn].f[0]);
            else if (type == 1) r.df[0] = fn(c.v[rn].df[0]);
            else r.h[0] = float_to_half(fn(half_to_float(c.v[rn].h[0])));
        };
        switch (opcode) {
            case 0:  // FMOV (register)
                std::memcpy(r.b, c.v[rn].b, type == 0 ? 4 : type == 1 ? 8 : 2);
                break;
            case 1: un([](auto a) { return std::fabs(a); }); break;
            case 2: un([](auto a) { return -a; }); break;
            case 3: un([](auto a) { return std::sqrt(a); }); break;
            case 8: un([](auto a) { return round_to_integral(a, kNearestEven); }); break;  // FRINTN
            case 9: un([](auto a) { return round_to_integral(a, kPlusInf); }); break;      // FRINTP
            case 10: un([](auto a) { return round_to_integral(a, kMinusInf); }); break;    // FRINTM
            case 11: un([](auto a) { return round_to_integral(a, kZero); }); break;        // FRINTZ
            case 12: un([](auto a) { return round_to_integral(a, kNearestAway); }); break; // FRINTA
            case 14: case 15: un([](auto a) { return round_to_integral(a, kNearestEven); }); break;  // FRINTX, FRINTI
            default: return false;
        }
        c.v[rd] = r;
        return true;
    }
    return false;
}

bool fp_fixed_conversion(Cpu& c, uint32_t insn) {
    const bool sf = bit(insn, 31);
    const unsigned type = field(insn, 23, 22), rmode = field(insn, 20, 19), opcode = field(insn, 18, 16);
    const unsigned scale = field(insn, 15, 10), rd = field(insn, 4, 0), rn = field(insn, 9, 5);
    const int fbits = int(64 - scale);
    const unsigned ibits = sf ? 64 : 32;
    if (type > 1 || (!sf && scale < 32)) return false;
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {  // SCVTF/UCVTF (fixed)
        VReg r{};
        if (type == 0) r.f[0] = int_to_fp<float>(get_x(c, rn), ibits, opcode == 3, fbits);
        else r.df[0] = int_to_fp<double>(get_x(c, rn), ibits, opcode == 3, fbits);
        c.v[rd] = r;
        return true;
    }
    if (rmode == 3 && (opcode == 0 || opcode == 1)) {  // FCVTZS/FCVTZU (fixed)
        const uint64_t out = type == 0 ? fp_to_int(c.v[rn].f[0], ibits, opcode == 1, kZero, fbits)
                                       : fp_to_int(c.v[rn].df[0], ibits, opcode == 1, kZero, fbits);
        set_x(c, rd, out);
        return true;
    }
    return false;
}

bool fp_three_source(Cpu& c, uint32_t insn) {
    const unsigned type = field(insn, 23, 22);
    const bool o1 = bit(insn, 21), o0 = bit(insn, 15);
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16), ra = field(insn, 14, 10);
    VReg r{};
    // FMADD: a + n*m; FMSUB: a - n*m; FNMADD: -a - n*m; FNMSUB: -a + n*m. All fused.
    if (type == 0) {
        float n = c.v[rn].f[0], m = c.v[rm].f[0], a = c.v[ra].f[0];
        if (o0) n = -n;
        if (o1) a = -a, n = -n;
        r.f[0] = std::fma(n, m, a);
    } else if (type == 1) {
        double n = c.v[rn].df[0], m = c.v[rm].df[0], a = c.v[ra].df[0];
        if (o0) n = -n;
        if (o1) a = -a, n = -n;
        r.df[0] = std::fma(n, m, a);
    } else if (type == 3) {
        float n = half_to_float(c.v[rn].h[0]), m = half_to_float(c.v[rm].h[0]), a = half_to_float(c.v[ra].h[0]);
        if (o0) n = -n;
        if (o1) a = -a, n = -n;
        r.h[0] = float_to_half(std::fma(n, m, a));
    } else {
        return false;
    }
    c.v[rd] = r;
    return true;
}

// ---- SIMD&FP loads and stores -----------------------------------------------

void vload(Cpu& c, unsigned rt, uint64_t addr, unsigned bytes) {
    VReg r{};
    std::memcpy(r.b, reinterpret_cast<const void*>(addr), bytes);
    c.v[rt] = r;
}
void vstore(const Cpu& c, unsigned rt, uint64_t addr, unsigned bytes) {
    std::memcpy(reinterpret_cast<void*>(addr), c.v[rt].b, bytes);
}

bool ld_st_structures(Cpu& c, uint32_t insn) {
    const bool q = bit(insn, 30), l = bit(insn, 22), post = bit(insn, 23);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    const uint64_t base = get_xsp(c, rn);
    uint64_t total;
    if (!bit(insn, 24)) {  // multiple structures
        const unsigned opcode = field(insn, 15, 12), size = field(insn, 11, 10);
        unsigned regs, selem;
        switch (opcode) {
            case 0x0: regs = 4; selem = 4; break;
            case 0x2: regs = 4; selem = 1; break;
            case 0x4: regs = 3; selem = 3; break;
            case 0x6: regs = 3; selem = 1; break;
            case 0x7: regs = 1; selem = 1; break;
            case 0x8: regs = 2; selem = 2; break;
            case 0xa: regs = 2; selem = 1; break;
            default: return false;
        }
        if (size == 3 && !q && selem != 1) return false;
        const unsigned ebytes = 1u << size, elems = (q ? 16 : 8) / ebytes;
        const unsigned rpt = regs / selem;
        uint64_t addr = base;
        VReg tmp[4];
        for (unsigned i = 0; i < regs; ++i) tmp[i] = l ? VReg{} : c.v[(rt + i) % 32];
        for (unsigned r = 0; r < rpt; ++r) {
            for (unsigned e = 0; e < elems; ++e) {
                for (unsigned s = 0; s < selem; ++s) {
                    VReg& v = tmp[r + s];
                    if (l) std::memcpy(v.b + e * ebytes, reinterpret_cast<const void*>(addr), ebytes);
                    else std::memcpy(reinterpret_cast<void*>(addr), v.b + e * ebytes, ebytes);
                    addr += ebytes;
                }
            }
        }
        if (l) {
            for (unsigned i = 0; i < regs; ++i) write_v(c, (rt + i) % 32, tmp[i], q ? 16 : 8);
        }
        total = addr - base;
    } else {  // single structure
        const unsigned opcode = field(insn, 15, 13), size = field(insn, 11, 10);
        const bool s = bit(insn, 12), r = bit(insn, 21);
        const unsigned selem = (((opcode & 1) << 1) | r) + 1;
        unsigned scale = opcode >> 1, index = 0;
        bool replicate = false;
        switch (scale) {
            case 0: index = (q << 3) | (s << 2) | size; break;
            case 1: if (size & 1) return false; index = (q << 2) | (s << 1) | (size >> 1); break;
            case 2:
                if (size & 2) return false;
                if (size == 0) {
                    index = (q << 1) | s;
                } else {
                    if (s) return false;
                    index = q;
                    scale = 3;
                }
                break;
            default:
                if (!l || s) return false;
                scale = size;
                replicate = true;
                break;
        }
        const unsigned ebytes = 1u << scale;
        uint64_t addr = base;
        for (unsigned k = 0; k < selem; ++k) {
            const unsigned t = (rt + k) % 32;
            if (replicate) {
                VReg v{};
                for (unsigned e = 0; e < (q ? 16u : 8u) / ebytes; ++e)
                    std::memcpy(v.b + e * ebytes, reinterpret_cast<const void*>(addr), ebytes);
                write_v(c, t, v, q ? 16 : 8);
            } else if (l) {
                std::memcpy(c.v[t].b + index * ebytes, reinterpret_cast<const void*>(addr), ebytes);
            } else {
                std::memcpy(reinterpret_cast<void*>(addr), c.v[t].b + index * ebytes, ebytes);
            }
            addr += ebytes;
        }
        total = addr - base;
    }
    if (post) set_xsp(c, rn, base + (rm == 31 ? total : get_x(c, rm)));
    return true;
}

}  // namespace

bool exec_simd_ldst(Cpu& c, uint32_t insn) {
    if ((insn & 0xbf000000) == 0x0c000000 || (insn & 0xbf000000) == 0x0d000000) {
        // Advanced SIMD load/store multiple / single structures. Without
        // post-index, bits 20..16 must be zero.
        if (!bit(insn, 23) && field(insn, 20, 16) != 0) return false;
        return ld_st_structures(c, insn);
    }
    if ((insn & 0x3f000000) == 0x1c000000) {  // LDR (literal, SIMD&FP)
        const unsigned opc = field(insn, 31, 30);
        if (opc == 3) return false;
        vload(c, field(insn, 4, 0), c.pc + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2), 4u << opc);
        return true;
    }
    if ((insn & 0x3e000000) == 0x2c000000) {  // LDP/STP (SIMD&FP)
        const unsigned opc = field(insn, 31, 30), mode = field(insn, 24, 23);
        if (opc == 3) return false;
        const unsigned bytes = 4u << opc;
        const int64_t off = sext(field(insn, 21, 15), 7) * int64_t(bytes);
        const unsigned rn = field(insn, 9, 5), rt = field(insn, 4, 0), rt2 = field(insn, 14, 10);
        const uint64_t base = get_xsp(c, rn), addr = mode == 1 ? base : base + off;
        if (bit(insn, 22)) {
            vload(c, rt, addr, bytes);
            vload(c, rt2, addr + bytes, bytes);
        } else {
            vstore(c, rt, addr, bytes);
            vstore(c, rt2, addr + bytes, bytes);
        }
        if (mode == 1 || mode == 3) set_xsp(c, rn, base + off);
        return true;
    }
    if ((insn & 0x3e000000) == 0x3c000000) {  // LDR/STR (SIMD&FP register forms)
        const unsigned size = field(insn, 31, 30), opc = field(insn, 23, 22);
        const unsigned rn = field(insn, 9, 5), rt = field(insn, 4, 0);
        unsigned scale = size;
        if (opc & 2) {
            if (size != 0) return false;
            scale = 4;
        }
        const unsigned bytes = 1u << scale;
        const uint64_t base = get_xsp(c, rn);
        uint64_t addr, wb = 0;
        bool writeback = false;
        if (bit(insn, 24)) {
            addr = base + (uint64_t(field(insn, 21, 10)) << scale);
        } else if (!bit(insn, 21)) {
            const int64_t imm9 = sext(field(insn, 20, 12), 9);
            switch (field(insn, 11, 10)) {
                case 0: addr = base + imm9; break;
                case 1: addr = base; writeback = true; wb = base + imm9; break;
                case 3: addr = base + imm9; writeback = true; wb = addr; break;
                default: return false;
            }
        } else if (field(insn, 11, 10) == 2) {
            const unsigned option = field(insn, 15, 13);
            if (!(option & 2)) return false;
            uint64_t off = get_x(c, field(insn, 20, 16));
            switch (option) {
                case 2: off = uint32_t(off); break;
                case 6: off = static_cast<uint64_t>(int64_t(int32_t(off))); break;
                default: break;
            }
            addr = base + (bit(insn, 12) ? off << scale : off);
        } else {
            return false;
        }
        if (opc & 1) vload(c, rt, addr, bytes);
        else vstore(c, rt, addr, bytes);
        if (writeback) set_xsp(c, rn, wb);
        return true;
    }
    return false;
}

bool exec_simd(Cpu& c, uint32_t insn) {
    // Scalar floating point (and FP<->integer conversions).
    if ((insn & 0x5f200000) == 0x1e200000) return fp_data(c, insn);
    if ((insn & 0x5f200000) == 0x1e000000) return fp_fixed_conversion(c, insn);
    if ((insn & 0x5f000000) == 0x1f000000) return fp_three_source(c, insn);

    // Advanced SIMD, vector.
    if ((insn & 0x9f200400) == 0x0e200400) return three_same(c, insn, false);
    if ((insn & 0x9f200c00) == 0x0e200000) return three_different(c, insn);
    if ((insn & 0x9f3e0c00) == 0x0e200800) return two_reg_misc(c, insn, false);
    if ((insn & 0x9f3e0c00) == 0x0e300800) return across_lanes(c, insn);
    if ((insn & 0x9fe08400) == 0x0e000400) return copy(c, insn, false);
    if ((insn & 0xbf208c00) == 0x0e000800) return permute(c, insn);
    if ((insn & 0xbf208c00) == 0x0e000000) return table_lookup(c, insn);
    if ((insn & 0xbfe08400) == 0x2e000000) return extract(c, insn);
    if ((insn & 0x9ff80400) == 0x0f000400) return modified_immediate(c, insn);
    if ((insn & 0x9f800400) == 0x0f000400) return shift_immediate(c, insn, false);
    if ((insn & 0x9f000400) == 0x0f000000) return by_element(c, insn, false);

    // Advanced SIMD, scalar.
    if ((insn & 0xdf200400) == 0x5e200400) return three_same(c, insn, true);
    if ((insn & 0xdf3e0c00) == 0x5e200800) return two_reg_misc(c, insn, true);
    if ((insn & 0xdf3e0c00) == 0x5e300800) return scalar_pairwise(c, insn);
    if ((insn & 0xdfe08400) == 0x5e000400) return copy(c, insn, true);
    if ((insn & 0xdf800400) == 0x5f000400) return shift_immediate(c, insn, true);
    if ((insn & 0xdf000400) == 0x5f000000) return by_element(c, insn, true);
    return false;
}

}  // namespace winapk::arm64
