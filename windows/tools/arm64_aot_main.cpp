// arm64_aot_main.cpp -- winapk-aot: translates an arm64-v8a .so into C ahead
// of time, for the installer to compile into `<lib>.so.aot.dll` (see
// include/winapk/arm64_aot.h for how the runtime uses it).
//
//   winapk-aot <lib.so> <out.c> [parts] [--profile <file>] [--full-below <insns>]
//   winapk-aot --hash <lib.so>
//
// parts: out.c.1.c ..., to compile in parallel (fewer when the library is
// small). With --profile, a library of more than --full-below instructions
// is translated only where the profile says code ran (see
// include/winapk/arm64_aot.h, "Profiles"). Exit code 0: translated whole;
// 4: translated where the profile says code ran; 3: nothing ran yet, so
// nothing was written.
//
// Functions are found from the symbol tables and, transitively, from the
// targets of BL; each is decoded by following its branches. In the C, every
// guest register a function touches is a local variable, loaded from the
// Cpu on entry and stored back only where something else may look at it:
// calls, returns, and the instructions handed to the interpreter. Decoding
// follows the interpreter (src/arm64/arm64_cpu.cpp) case by case, so the two
// can be checked against each other; the differential test runs the same
// libraries through both.
//
// Natively translated: the integer instructions, branches, and loads and
// stores of general and SIMD&FP registers. Everything else (floating-point
// and SIMD arithmetic, exclusives and atomics, most system registers) goes
// through the interpreter, one instruction at a time, which is correct but
// slow; the summary on stderr counts those, so the next ones to translate are
// always known.

#include "winapk/arm64_aot.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using winapk::arm64::aot_hash;

// ---- ELF ---------------------------------------------------------------------

struct Ehdr64 {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct Phdr64 {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
struct Shdr64 {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
};
struct Sym64 {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};

struct Image {
    std::vector<uint8_t> file;
    struct Seg {
        uint64_t vaddr, size, offset;
    };
    std::vector<Seg> exec;
    uint64_t text_hash = 0;
    std::map<uint64_t, uint64_t> symbols;  // function entry -> size (0: unknown)

    bool code(uint64_t va) const {
        for (const Seg& s : exec) {
            if (va >= s.vaddr && va + 4 <= s.vaddr + s.size) return true;
        }
        return false;
    }
    uint32_t insn(uint64_t va) const {
        for (const Seg& s : exec) {
            if (va >= s.vaddr && va + 4 <= s.vaddr + s.size) {
                uint32_t w;
                std::memcpy(&w, file.data() + s.offset + (va - s.vaddr), 4);
                return w;
            }
        }
        return 0;
    }
};

template <typename T> bool read_at(const std::vector<uint8_t>& f, uint64_t off, T* out) {
    if (off > f.size() || f.size() - off < sizeof(T)) return false;
    std::memcpy(out, f.data() + off, sizeof(T));
    return true;
}

bool read_image(const char* path, Image* img, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        *error = std::string("cannot open ") + path;
        return false;
    }
    img->file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    Ehdr64 eh;
    if (!read_at(img->file, 0, &eh) || std::memcmp(eh.ident, "\x7f" "ELF", 4) != 0 || eh.ident[4] != 2 ||
        eh.machine != 183) {
        *error = "not a 64-bit arm64 ELF file";
        return false;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < eh.phnum; ++i) {
        Phdr64 ph;
        if (!read_at(img->file, eh.phoff + uint64_t(i) * eh.phentsize, &ph)) break;
        if (ph.type != 1 || !(ph.flags & 1) || ph.filesz == 0) continue;
        if (ph.offset + ph.filesz > img->file.size()) {
            *error = "truncated executable segment";
            return false;
        }
        img->exec.push_back({ph.vaddr, ph.filesz, ph.offset});
        h = aot_hash(img->file.data() + ph.offset, ph.filesz, h);
    }
    img->text_hash = h;
    if (img->exec.empty()) {
        *error = "no executable segment";
        return false;
    }
    // Function symbols from .symtab and .dynsym (a stripped library still
    // has the exported ones; BL targets find the rest).
    for (unsigned i = 0; i < eh.shnum; ++i) {
        Shdr64 sh;
        if (!read_at(img->file, eh.shoff + uint64_t(i) * eh.shentsize, &sh)) break;
        if (sh.type != 2 && sh.type != 11) continue;  // SHT_SYMTAB, SHT_DYNSYM
        for (uint64_t off = 0; off + sizeof(Sym64) <= sh.size; off += sizeof(Sym64)) {
            Sym64 s;
            if (!read_at(img->file, sh.offset + off, &s)) break;
            if ((s.info & 0xf) != 2 || s.shndx == 0 || !img->code(s.value)) continue;  // STT_FUNC, defined
            uint64_t& size = img->symbols[s.value];
            if (s.size > size) size = s.size;
        }
    }
    return true;
}

// ---- decoding helpers (as in src/arm64/arm64_internal.h) ----------------------

uint32_t field(uint32_t insn, unsigned hi, unsigned lo) {
    return (insn >> lo) & ((hi - lo == 31) ? ~0u : ((1u << (hi - lo + 1)) - 1));
}
bool bit(uint32_t insn, unsigned b) { return (insn >> b) & 1; }
uint64_t mask(unsigned n) { return n >= 64 ? ~uint64_t{0} : ((uint64_t{1} << n) - 1); }
int64_t sext(uint64_t v, unsigned bits) {
    return bits >= 64 ? static_cast<int64_t>(v) : static_cast<int64_t>(v << (64 - bits)) >> (64 - bits);
}
uint64_t ror(uint64_t v, unsigned r, unsigned size) {
    v &= mask(size);
    r %= size;
    if (r == 0) return v;
    return ((v >> r) | (v << (size - r))) & mask(size);
}

bool decode_bit_masks(unsigned n, unsigned imms, unsigned immr, bool immediate, unsigned datasize, uint64_t* wmask,
                      uint64_t* tmask) {
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

// ---- control flow ----------------------------------------------------------------

enum class Flow { kSeq, kCond, kB, kBL, kBLR, kBR, kRet };

Flow classify(uint32_t insn, uint64_t va, uint64_t* target) {
    if ((insn & 0xff000010) == 0x54000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        return Flow::kCond;
    }
    if ((insn & 0x7c000000) == 0x14000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 25, 0), 26) << 2);
        return bit(insn, 31) ? Flow::kBL : Flow::kB;
    }
    if ((insn & 0x7e000000) == 0x34000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        return Flow::kCond;
    }
    if ((insn & 0x7e000000) == 0x36000000) {
        *target = va + static_cast<uint64_t>(sext(field(insn, 18, 5), 14) << 2);
        return Flow::kCond;
    }
    if ((insn & 0xfe000000) == 0xd6000000) {
        const unsigned opc = field(insn, 24, 21);
        if (field(insn, 20, 16) != 0x1f || (opc > 2 && opc != 8 && opc != 9)) return Flow::kSeq;
        switch (opc & 3) {
            case 0: return Flow::kBR;
            case 1: return Flow::kBLR;
            default: return Flow::kRet;
        }
    }
    return Flow::kSeq;
}

struct Function {
    uint64_t entry = 0, end = 0;  // end 0: size unknown
    std::set<uint64_t> insns, labels;
    std::set<uint64_t> falls_out;  // instructions whose fallthrough leaves the function
};

constexpr size_t kMaxInsns = 100000;

// Functions above this many instructions are compiled without optimization
// (Emitter::function). WINAPK_AOT_OPTNONE_INSNS overrides it, for measuring.
size_t g_optnone_insns = 4000;

// Follows the branches of the function at f->entry. BL targets go to `calls`.
bool discover(const Image& img, const std::set<uint64_t>& entries, Function* f, std::set<uint64_t>* calls) {
    auto inside = [&](uint64_t va) {
        if (!img.code(va)) return false;
        if (f->end) return va >= f->entry && va < f->end;
        return va == f->entry || !entries.count(va);  // another function's entry is a tail call
    };
    std::vector<uint64_t> work = {f->entry};
    f->labels.insert(f->entry);
    while (!work.empty()) {
        uint64_t va = work.back();
        work.pop_back();
        for (;;) {
            if (f->insns.count(va)) break;
            if (!inside(va)) {
                f->falls_out.insert(va - 4);
                break;
            }
            if (f->insns.size() >= kMaxInsns) return false;
            f->insns.insert(va);
            uint64_t t = 0;
            const Flow flow = classify(img.insn(va), va, &t);
            if (flow == Flow::kCond || flow == Flow::kB) {
                if (inside(t) && !f->labels.count(t)) {
                    f->labels.insert(t);
                    work.push_back(t);
                }
            }
            if (flow == Flow::kBL && img.code(t)) calls->insert(t);
            if (flow == Flow::kB || flow == Flow::kBR || flow == Flow::kRet) break;
            va += 4;
        }
    }
    return true;
}

// ---- C emission -----------------------------------------------------------------

std::string fmt(const char* f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    std::vsnprintf(out.data(), out.size() + 1, f, ap2);
    va_end(ap2);
    return out;
}

std::string hex(uint64_t v) { return fmt("0x%" PRIx64 "ull", v); }

const char* kPrelude = R"(// Generated by winapk-aot. Compile with clang -O2 -shared -fno-strict-aliasing.
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t x[31];
    uint64_t sp, pc;
    uint32_t nzcv, fpcr, fpsr;
    uint64_t tpidr;
    _Alignas(16) uint64_t v[32][2];
} Cpu;
typedef struct {
    uint32_t (*branch)(Cpu* c, uint64_t target);
    void (*step)(Cpu* c);
    uint32_t (*plain)(uint64_t target);
    void (__attribute__((sysv_abi)) *call_sysv)(const void* fn, void* frame);
} AotRuntime;
typedef struct { uint64_t vaddr; uint32_t (*fn)(Cpu* c); } AotEntry;
typedef struct {
    uint32_t version, layout[4];
    uint64_t text_hash;
    uint64_t* base;
    AotRuntime* runtime;
    const AotEntry* entries;
    uint64_t count;
} AotModule;

// Defined once, in the file with the module table (the code may be split
// across several files compiled in parallel).
extern uint64_t aot_base;
extern AotRuntime aot_rt;

// Guest memory: unaligned, and volatile so that the C compiler keeps every
// access, in order, as the guest's own synchronization expects.
typedef volatile uint8_t vu8;
typedef volatile uint16_t __attribute__((aligned(1))) vu16;
typedef volatile uint32_t __attribute__((aligned(1))) vu32;
typedef volatile uint64_t __attribute__((aligned(1))) vu64;
#define LD8(a) (*(vu8*)(uintptr_t)(a))
#define LD16(a) (*(vu16*)(uintptr_t)(a))
#define LD32(a) (*(vu32*)(uintptr_t)(a))
#define LD64(a) (*(vu64*)(uintptr_t)(a))
#define ST8(a, v) (*(vu8*)(uintptr_t)(a) = (uint8_t)(v))
#define ST16(a, v) (*(vu16*)(uintptr_t)(a) = (uint16_t)(v))
#define ST32(a, v) (*(vu32*)(uintptr_t)(a) = (uint32_t)(v))
#define ST64(a, v) (*(vu64*)(uintptr_t)(a) = (uint64_t)(v))

#define INL static inline __attribute__((always_inline))
INL uint64_t ror64(uint64_t v, unsigned r) { r &= 63; return r ? (v >> r) | (v << (64 - r)) : v; }
INL uint64_t ror32(uint64_t v, unsigned r) { uint32_t w = (uint32_t)v; r &= 31; return r ? (uint32_t)((w >> r) | (w << (32 - r))) : w; }
INL uint64_t asr64(uint64_t v, unsigned s) { return (uint64_t)((int64_t)v >> s); }
INL uint64_t asr32(uint64_t v, unsigned s) { return (uint32_t)((int32_t)(uint32_t)v >> s); }
// AddWithCarry(), setting NZCV.
INL uint64_t addf64(uint64_t x, uint64_t y, uint64_t cin, uint8_t* n, uint8_t* z, uint8_t* c, uint8_t* v) {
    const unsigned __int128 u = (unsigned __int128)x + y + cin;
    const uint64_t r = (uint64_t)u;
    *n = r >> 63; *z = r == 0; *c = (uint8_t)(u >> 64); *v = (((x ^ r) & (y ^ r)) >> 63) & 1;
    return r;
}
INL uint64_t addf32(uint64_t x64, uint64_t y64, uint64_t cin, uint8_t* n, uint8_t* z, uint8_t* c, uint8_t* v) {
    const uint32_t x = (uint32_t)x64, y = (uint32_t)y64;
    const uint64_t u = (uint64_t)x + y + cin;
    const uint32_t r = (uint32_t)u;
    *n = r >> 31; *z = r == 0; *c = (uint8_t)(u >> 32); *v = (((x ^ r) & (y ^ r)) >> 31) & 1;
    return r;
}
INL uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
INL uint64_t clz32(uint64_t v) { return (uint32_t)v ? (uint64_t)__builtin_clz((uint32_t)v) : 32; }
INL uint64_t rev16_64(uint64_t v) { return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull); }
INL uint64_t rev32_64(uint64_t v) { return (uint64_t)__builtin_bswap32((uint32_t)v) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32); }
INL uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
INL uint64_t udiv32(uint64_t a, uint64_t b) { return (uint32_t)b ? (uint32_t)a / (uint32_t)b : 0; }
INL uint64_t sdiv64(uint64_t a, uint64_t b) { return b == 0 ? 0 : b == ~0ull ? 0 - a : (uint64_t)((int64_t)a / (int64_t)b); }
INL uint64_t sdiv32(uint64_t a, uint64_t b) {
    const int32_t x = (int32_t)a, y = (int32_t)b;
    return y == 0 ? 0 : y == -1 ? (uint32_t)(0u - (uint32_t)x) : (uint32_t)(x / y);
}

// SIMD&FP registers and their lane views.
#define VEC(T, n, name) typedef T name __attribute__((vector_size(n)));
VEC(uint64_t, 16, v2u64) VEC(int64_t, 16, v2i64) VEC(uint32_t, 16, v4u32) VEC(int32_t, 16, v4i32)
VEC(uint16_t, 16, v8u16) VEC(int16_t, 16, v8i16) VEC(uint8_t, 16, v16u8) VEC(int8_t, 16, v16i8)
VEC(float, 16, v4f32) VEC(double, 16, v2f64)
VEC(uint32_t, 8, v2u32) VEC(int32_t, 8, v2i32) VEC(uint16_t, 8, v4u16) VEC(int16_t, 8, v4i16)
VEC(uint8_t, 8, v8u8) VEC(int8_t, 8, v8i8) VEC(float, 8, v2f32)
VEC(uint16_t, 32, v16u16) VEC(int16_t, 32, v16i16) VEC(uint32_t, 32, v8u32) VEC(int32_t, 32, v8i32)
VEC(uint64_t, 32, v4u64) VEC(int64_t, 32, v4i64)
INL v2u64 lo64(v2u64 v) { return (v2u64){v[0], 0}; }

INL float f32(uint64_t b) { const uint32_t w = (uint32_t)b; float f; __builtin_memcpy(&f, &w, 4); return f; }
INL double f64(uint64_t b) { double d; __builtin_memcpy(&d, &b, 8); return d; }
INL uint64_t b32(float f) { uint32_t w; __builtin_memcpy(&w, &f, 4); return w; }
INL uint64_t b64(double d) { uint64_t w; __builtin_memcpy(&w, &d, 8); return w; }

// FPToFixed on an already rounded value: saturating, NaN -> 0, as on Arm.
INL uint64_t sat_s64(double r) {
    if (r != r) return 0;
    if (r >= 9223372036854775808.0) return 0x7fffffffffffffffull;
    if (r < -9223372036854775808.0) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)r;
}
INL uint64_t sat_s32(double r) {
    if (r != r) return 0;
    if (r >= 2147483648.0) return 0x7fffffff;
    if (r < -2147483648.0) return 0x80000000;
    return (uint32_t)(int32_t)r;
}
INL uint64_t sat_u64(double r) {
    if (r != r || r <= 0) return 0;
    if (r >= 18446744073709551616.0) return ~0ull;
    return (uint64_t)r;
}
INL uint64_t sat_u32(double r) {
    if (r != r || r <= 0) return 0;
    if (r >= 4294967296.0) return 0xffffffff;
    return (uint32_t)r;
}
// Arm's FMAX/FMIN (NaN in, NaN out; +0 > -0) and FMAXNM/FMINNM (a NaN loses).
#define FMINMAX(T, sfx)                                                                              \
    INL T fmax_##sfx(T a, T b) {                                                                     \
        if (a != a || b != b) return a + b;                                                          \
        if (a == 0 && b == 0) return __builtin_signbit(a) ? b : a;                                   \
        return a > b ? a : b;                                                                        \
    }                                                                                                \
    INL T fmin_##sfx(T a, T b) {                                                                     \
        if (a != a || b != b) return a + b;                                                          \
        if (a == 0 && b == 0) return __builtin_signbit(a) ? a : b;                                   \
        return a < b ? a : b;                                                                        \
    }                                                                                                \
    INL T fmaxnm_##sfx(T a, T b) { return a != a && b == b ? b : b != b && a == a ? a : fmax_##sfx(a, b); } \
    INL T fminnm_##sfx(T a, T b) { return a != a && b == b ? b : b != b && a == a ? a : fmin_##sfx(a, b); }
FMINMAX(float, f) FMINMAX(double, d)

// A direct call of a plain host function (AotRuntime::plain) with the
// guest's argument registers and stack words in *c (as the bridge's
// host_call passes them), results back into *c.
typedef struct {
    uint64_t gpr[6], fpr[8];
    const uint64_t* stack;
    uint64_t nstack, rax, rdx, xmm0, xmm1;
} SysvFrame;
static __attribute__((noinline)) void host_call(Cpu* restrict c, uint64_t target) {
    SysvFrame f;
    uint64_t stk[16];
    for (int i = 0; i < 6; ++i) f.gpr[i] = c->x[i];
    for (int i = 0; i < 8; ++i) f.fpr[i] = c->v[i][0];
    stk[0] = c->x[6];
    stk[1] = c->x[7];
    __builtin_memcpy(stk + 2, (const void*)(uintptr_t)c->sp, 14 * 8);
    f.stack = stk;
    f.nstack = 16;
    aot_rt.call_sysv((const void*)(uintptr_t)target, &f);
    c->x[0] = f.rax;
    c->x[1] = f.rdx;
    c->v[0][0] = f.xmm0; c->v[0][1] = 0;
    c->v[1][0] = f.xmm1; c->v[1][1] = 0;
}
// A call through the runtime, remembering at the call site a target that
// turned out to be a plain host function: the next call to it is direct.
#define CALL_SITE(st, t)                                                           \
    do {                                                                           \
        static uint64_t site;                                                      \
        if ((t) == __atomic_load_n(&site, __ATOMIC_RELAXED)) {                     \
            host_call(c, (t));                                                     \
            (st) = 0;                                                              \
        } else {                                                                   \
            (st) = aot_rt.branch(c, (t));                                          \
            if ((st) == 0 && aot_rt.plain(t)) __atomic_store_n(&site, (t), __ATOMIC_RELAXED); \
        }                                                                          \
    } while (0)
)";

// Lane types of the generated C, by element bits.
const char* ut(unsigned eb) { return eb == 8 ? "v16u8" : eb == 16 ? "v8u16" : eb == 32 ? "v4u32" : "v2u64"; }
const char* st(unsigned eb) { return eb == 8 ? "v16i8" : eb == 16 ? "v8i16" : eb == 32 ? "v4i32" : "v2i64"; }
const char* ft(unsigned eb) { return eb == 32 ? "v4f32" : "v2f64"; }
const char* et(unsigned eb, bool sgn) {
    static const char* kU[] = {"uint8_t", "uint16_t", "uint32_t", "uint64_t"};
    static const char* kS[] = {"int8_t", "int16_t", "int32_t", "int64_t"};
    const unsigned i = eb == 8 ? 0 : eb == 16 ? 1 : eb == 32 ? 2 : 3;
    return sgn ? kS[i] : kU[i];
}
// The 32-byte types the widening reductions need.
const char* wide32(unsigned eb, bool sgn) {
    if (eb == 16) return sgn ? "v16i16" : "v16u16";
    if (eb == 32) return sgn ? "v8i32" : "v8u32";
    return sgn ? "v4i64" : "v4u64";
}
std::string shuffle(const char* type, const std::string& a, const std::string& b, const std::vector<unsigned>& idx) {
    std::string s = std::string("__builtin_shufflevector((") + type + ")(" + a + "), (" + type + ")(" + b + ")";
    for (unsigned i : idx) s += fmt(", %u", i);
    return s + ")";
}
std::vector<unsigned> iota(unsigned from, unsigned n, unsigned step = 1) {
    std::vector<unsigned> v;
    for (unsigned i = 0; i < n; ++i) v.push_back(from + i * step);
    return v;
}
// Pads a lane list to `lanes` (the rest is cleared by lo64 or unused).
std::vector<unsigned> pad(std::vector<unsigned> v, unsigned lanes) {
    while (v.size() < lanes) v.push_back(v.empty() ? 0 : v[0]);
    return v;
}

uint64_t vfp_expand_imm(unsigned imm8, unsigned ebits) {
    const uint64_t a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 15;
    if (ebits == 32) {
        const uint64_t exp = ((b ^ 1) << 7) | ((b ? 0x1f : 0) << 2) | cd;
        return (a << 31) | (exp << 23) | (efgh << 19);
    }
    const uint64_t exp = ((b ^ 1) << 10) | ((b ? 0xff : 0) << 2) | cd;
    return (a << 63) | (exp << 52) | (efgh << 48);
}

// Round to integral in the operand's precision: 0 nearest-even, 1 +inf,
// 2 -inf, 3 zero, 4 nearest-away (the interpreter's Rounding order).
std::string round_expr(const std::string& v, unsigned type, unsigned mode) {
    static const char* kFns[] = {"nearbyint", "ceil", "floor", "trunc", "round"};
    return fmt("__builtin_%s%s(%s)", kFns[mode], type ? "" : "f", v.c_str());
}

class Emitter {
public:
    Emitter(const Image& img, const Function& f, const std::set<uint64_t>& translated)
        : img_(img), f_(f), translated_(translated) {}

    std::string function(size_t* interpreted);

private:
    // Register access. x31 reads as zero (XZR) or as SP, by operand.
    std::string rx(unsigned n) {
        if (n == 31) return "0ull";
        used_x_ |= uint64_t{1} << n;
        return fmt("X%u", n);
    }
    std::string rxsp(unsigned n) {
        if (n == 31) {
            used_x_ |= uint64_t{1} << 31;
            return "SP";
        }
        return rx(n);
    }
    std::string reg(unsigned n) {  // x31 as SP, for writes
        used_x_ |= uint64_t{1} << n;
        written_x_ |= uint64_t{1} << n;
        return n == 31 ? "SP" : fmt("X%u", n);
    }
    void wx(unsigned n, const std::string& v, bool sf) {
        if (n == 31) {  // XZR: only the side effects (the flags of CMP, CMN, TST)
            line("(void)(" + v + ");");
            return;
        }
        line(reg(n) + (sf ? " = (uint64_t)(" + v + ");" : " = (uint64_t)(uint32_t)(" + v + ");"));
    }
    void wxsp(unsigned n, const std::string& v, bool sf) {
        line(reg(n) + (sf ? " = (uint64_t)(" + v + ");" : " = (uint64_t)(uint32_t)(" + v + ");"));
    }
    // SIMD&FP registers: one 128-bit vector each (v2u64), so that clang keeps
    // them in XMM registers and lowers lane operations to SSE/AVX.
    std::string vr(unsigned n) {
        used_v_ |= 1u << n;
        return fmt("V%u", n);
    }
    std::string vl(unsigned n) { return vr(n) + "[0]"; }
    std::string vh(unsigned n) { return vr(n) + "[1]"; }
    void wv(unsigned n, const std::string& lo, const std::string& hi) {
        written_v_ |= 1u << n;
        line(vr(n) + " = (v2u64){(uint64_t)(" + lo + "), (uint64_t)(" + hi + ")};");
    }
    // A whole-register result, any 16-byte vector type; `bytes` 8 clears the
    // upper half, as every write of a 64-bit vector or a scalar does.
    void wq(unsigned n, const std::string& v, unsigned bytes = 16) {
        written_v_ |= 1u << n;
        line(vr(n) + (bytes == 16 ? " = (v2u64)(" + v + ");" : " = lo64((v2u64)(" + v + "));"));
    }
    const char* flags() {
        flags_written_ = true;
        return "&FN, &FZ, &FC, &FV";
    }
    std::string cond(unsigned c) {
        flags_used_ = true;
        std::string r;
        switch (c >> 1) {
            case 0: r = "FZ"; break;
            case 1: r = "FC"; break;
            case 2: r = "FN"; break;
            case 3: r = "FV"; break;
            case 4: r = "(FC && !FZ)"; break;
            case 5: r = "(FN == FV)"; break;
            case 6: r = "(FN == FV && !FZ)"; break;
            default: return "1";
        }
        return (c & 1) ? "!" + r : r;
    }
    std::string addr(uint64_t va) { return "(B + " + hex(va) + ")"; }
    void line(const std::string& s) { body_ += "    " + s + "\n"; }

    std::string shift(const std::string& v, unsigned type, unsigned amount, bool sf) {
        const unsigned size = sf ? 64 : 32;
        amount %= size;
        const std::string m = sf ? "" : "(uint32_t)";
        switch (type) {
            case 0: return fmt("(uint64_t)%s(%s << %u)", m.c_str(), v.c_str(), amount);
            case 1: return fmt("((uint64_t)%s(%s) >> %u)", m.c_str(), v.c_str(), amount);
            case 2: return fmt("asr%u(%s, %u)", size, v.c_str(), amount);
            default: return fmt("ror%u(%s, %u)", size, v.c_str(), amount);
        }
    }
    std::string extend(const std::string& v, unsigned option, unsigned sh) {
        static const char* kCasts[] = {"(uint8_t)", "(uint16_t)", "(uint32_t)", "",
                                        "(int64_t)(int8_t)", "(int64_t)(int16_t)", "(int64_t)(int32_t)", ""};
        return fmt("((uint64_t)%s(%s) << %u)", kCasts[option & 7], v.c_str(), sh);
    }

    // Writes back what others may read: before calls (flags excluded: the
    // AAPCS64 does not keep them across calls) and with the flags before the
    // interpreter or a jump that may leave the function.
    std::string flush(bool with_flags) { return with_flags ? "FLUSH_ALL();" : "FLUSH();"; }
    // A jump to `t` outside this function: a tail call, or the runtime.
    std::string exit_str(uint64_t t) {
        if (translated_.count(t)) return flush(false) + fmt(" return F_%" PRIx64 "(c);", t);
        return flush(true) + " c->pc = " + addr(t) + "; return aot_rt.branch(c, " + addr(t) + ");";
    }
    void exit_to(uint64_t t) { line(exit_str(t)); }
    void call_to(const std::string& target_expr, uint64_t direct, uint64_t ret) {
        wx(30, addr(ret), true);
        line("{");
        line("    const uint64_t t = " + target_expr + ";");
        line("    " + flush(false));
        if (direct && translated_.count(direct)) {
            line(fmt("    const uint32_t st = F_%" PRIx64 "(c);", direct));
        } else {
            line("    uint32_t st; c->pc = t; CALL_SITE(st, t);");
        }
        line("    if (st) return st;");
        line("    RELOAD_CALL();");
        line("}");
        reload_call_used_ = true;
    }
    void interpret(uint64_t va) {
        ++interpreted_;
        if (std::getenv("WINAPK_AOT_LIST")) std::fprintf(stderr, "interpreted 0x%" PRIx64 "\n", va);
        line(flush(true) + " c->pc = " + addr(va) + "; aot_rt.step(c); RELOAD_ALL();");
        line("if (c->pc != " + addr(va + 4) + ") return 1;");
    }

    bool branch(uint64_t va, uint32_t insn);
    bool dp_immediate(uint32_t insn, uint64_t va);
    bool dp_register(uint32_t insn);
    bool loads_stores(uint32_t insn, uint64_t va);
    bool ldst_register(uint32_t insn);
    bool ldst_pair(uint32_t insn);
    bool system(uint32_t insn);

    // SIMD&FP data processing (arm64_simd.cpp's groups).
    std::string fpv(unsigned n, unsigned type) { return (type ? "f64(" : "f32(") + vl(n) + ")"; }
    void wfp(unsigned n, unsigned type, const std::string& v) { wv(n, (type ? "b64(" : "b32(") + v + ")", "0"); }
    bool simd(uint32_t insn);
    bool fp_data(uint32_t insn);
    bool fp_three_source(uint32_t insn);
    bool v_three_same(uint32_t insn);
    bool v_three_different(uint32_t insn);
    bool v_two_reg_misc(uint32_t insn);
    bool v_across(uint32_t insn);
    bool v_copy(uint32_t insn, bool scalar);
    bool v_permute(uint32_t insn);
    bool v_extract(uint32_t insn);
    bool v_modified_imm(uint32_t insn);
    bool v_shift_imm(uint32_t insn);
    bool v_by_element(uint32_t insn);
    bool s_three_same(uint32_t insn);
    bool s_pairwise(uint32_t insn);
    bool structures(uint32_t insn);

    const Image& img_;
    const Function& f_;
    const std::set<uint64_t>& translated_;
    std::string body_;
    uint64_t used_x_ = 0, written_x_ = 0;  // bit 31: SP
    uint32_t used_v_ = 0, written_v_ = 0;
    bool flags_used_ = false, flags_written_ = false, reload_call_used_ = false;
    size_t interpreted_ = 0;
};

bool Emitter::branch(uint64_t va, uint32_t insn) {
    uint64_t t = 0;
    const Flow flow = classify(insn, va, &t);
    auto go = [&](uint64_t target) {
        if (f_.labels.count(target)) return fmt("goto L_%" PRIx64 ";", target);
        return "{ " + exit_str(target) + " }";  // out of the function
    };
    switch (flow) {
        case Flow::kSeq:
            return false;
        case Flow::kCond: {
            std::string c;
            if ((insn & 0xff000010) == 0x54000000) {
                c = cond(field(insn, 3, 0));
            } else if ((insn & 0x7e000000) == 0x34000000) {
                std::string v = rx(field(insn, 4, 0));
                if (!bit(insn, 31)) v = "(uint32_t)" + v;
                c = bit(insn, 24) ? v + " != 0" : v + " == 0";
            } else {
                const unsigned b = (bit(insn, 31) << 5) | field(insn, 23, 19);
                c = fmt("((%s >> %u) & 1) == %u", rx(field(insn, 4, 0)).c_str(), b, unsigned(bit(insn, 24)));
            }
            line("if (" + c + ") " + go(t));
            return true;
        }
        case Flow::kB:
            line(go(t));
            return true;
        case Flow::kBL:
            call_to(addr(t), t, va + 4);
            return true;
        case Flow::kBLR: {
            // read the target before x30 is written (BLR x30)
            line("{ const uint64_t blr = " + rx(field(insn, 9, 5)) + ";");
            call_to("blr", 0, va + 4);
            line("}");
            return true;
        }
        case Flow::kBR: {
            line("{");
            line("    const uint64_t t = " + rx(field(insn, 9, 5)) + ";");
            line("    switch (t - B) {");
            for (uint64_t l : f_.labels) line(fmt("        case 0x%" PRIx64 "ull: goto L_%" PRIx64 ";", l, l));
            line("    }");
            line("    " + flush(true) + " c->pc = t;");
            line("    { uint32_t st; CALL_SITE(st, t); if (st == 0) c->pc = c->x[30]; return st; }");
            line("}");
            return true;
        }
        case Flow::kRet:
            line("{ const uint64_t t = " + rx(field(insn, 9, 5)) + "; " + flush(false) +
                 " c->pc = t; return 0; }");
            return true;
    }
    return false;
}

bool Emitter::dp_immediate(uint32_t insn, uint64_t va) {
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5);
    const bool sf = bit(insn, 31);
    switch (field(insn, 25, 23)) {
        case 0: case 1: {  // ADR, ADRP
            const int64_t imm = sext((field(insn, 23, 5) << 2) | field(insn, 30, 29), 21);
            const uint64_t v = bit(insn, 31) ? (va & ~uint64_t{0xfff}) + (static_cast<uint64_t>(imm) << 12)
                                             : va + static_cast<uint64_t>(imm);
            wx(rd, addr(v), true);
            return true;
        }
        case 2: {  // ADD/ADDS/SUB/SUBS (immediate)
            const bool sub = bit(insn, 30), s = bit(insn, 29);
            uint64_t imm = field(insn, 21, 10);
            if (bit(insn, 22)) imm <<= 12;
            const std::string a = rxsp(rn);
            if (s) {
                wx(rd, fmt("addf%u(%s, %s, %u, %s)", sf ? 64 : 32, a.c_str(), hex(sub ? ~imm : imm).c_str(),
                           unsigned(sub), flags()),
                   sf);
            } else {
                wxsp(rd, a + (sub ? " - " : " + ") + hex(imm), sf);
            }
            return true;
        }
        case 4: {  // AND/ORR/EOR/ANDS (immediate)
            uint64_t wmask, tmask;
            if (!sf && bit(insn, 22)) return false;
            if (!decode_bit_masks(bit(insn, 22), field(insn, 15, 10), field(insn, 21, 16), true, sf ? 64 : 32,
                                  &wmask, &tmask)) {
                return false;
            }
            static const char* kOps[] = {" & ", " | ", " ^ ", " & "};
            const unsigned opc = field(insn, 30, 29);
            const std::string r = "(" + rx(rn) + kOps[opc] + hex(wmask) + ")";
            if (opc == 3) {
                line("{ const uint64_t r = " + std::string(sf ? "" : "(uint32_t)") + r + ";");
                flags_written_ = true;
                line(sf ? "  FN = r >> 63; FZ = r == 0; FC = 0; FV = 0;"
                        : "  FN = (r >> 31) & 1; FZ = (uint32_t)r == 0; FC = 0; FV = 0;");
                wx(rd, "r", sf);
                line("}");
            } else {
                wxsp(rd, r, sf);
            }
            return true;
        }
        case 5: {  // MOVN/MOVZ/MOVK
            const unsigned hw = field(insn, 22, 21);
            if (!sf && hw > 1) return false;
            const unsigned pos = hw * 16;
            const uint64_t imm = uint64_t(field(insn, 20, 5)) << pos;
            switch (field(insn, 30, 29)) {
                case 0: wx(rd, hex(~imm), sf); return true;
                case 2: wx(rd, hex(imm), sf); return true;
                case 3:
                    wx(rd, "(" + rx(rd) + " & " + hex(~(uint64_t{0xffff} << pos)) + ") | " + hex(imm), sf);
                    return true;
                default: return false;
            }
        }
        case 6: {  // SBFM/BFM/UBFM
            const unsigned opc = field(insn, 30, 29);
            const unsigned size = sf ? 64 : 32;
            if (opc == 3 || bit(insn, 22) != sf) return false;
            const unsigned immr = field(insn, 21, 16), imms = field(insn, 15, 10);
            uint64_t wmask, tmask;
            if (!decode_bit_masks(bit(insn, 22), imms, immr, false, size, &wmask, &tmask)) return false;
            line("{ const uint64_t src = " + rx(rn) + " & " + hex(mask(size)) + ";");
            line(fmt("  const uint64_t bot_src = ror%u(src, %u);", size, immr));
            if (opc == 1) {  // BFM
                line("  const uint64_t dst = " + rx(rd) + ";");
                line("  const uint64_t bot = (dst & " + hex(~wmask) + ") | (bot_src & " + hex(wmask) + ");");
                wx(rd, "((dst & " + hex(~tmask) + ") | (bot & " + hex(tmask) + ")) & " + hex(mask(size)), true);
            } else if (opc == 0) {  // SBFM
                line("  const uint64_t bot = bot_src & " + hex(wmask) + ";");
                line(fmt("  const uint64_t top = ((src >> %u) & 1) ? %s : 0;", imms, hex(mask(size)).c_str()));
                wx(rd, "((top & " + hex(~tmask) + ") | (bot & " + hex(tmask) + ")) & " + hex(mask(size)), true);
            } else {  // UBFM
                wx(rd, "bot_src & " + hex(wmask & tmask), true);
            }
            line("}");
            return true;
        }
        case 7: {  // EXTR
            const unsigned size = sf ? 64 : 32;
            if (bit(insn, 22) != sf || field(insn, 30, 29) != 0 || bit(insn, 21)) return false;
            const unsigned lsb = field(insn, 15, 10);
            if (lsb >= size) return false;
            const std::string hi = "(" + rx(rn) + " & " + hex(mask(size)) + ")";
            const std::string lo = "(" + rx(field(insn, 20, 16)) + " & " + hex(mask(size)) + ")";
            if (lsb == 0) {
                wx(rd, hi, true);
            } else {
                wx(rd, fmt("((%s >> %u) | (%s << %u)) & %s", lo.c_str(), lsb, hi.c_str(), size - lsb,
                           hex(mask(size)).c_str()),
                   true);
            }
            return true;
        }
        default:
            return false;
    }
}

bool Emitter::dp_register(uint32_t insn) {
    const bool sf = bit(insn, 31);
    const unsigned size = sf ? 64 : 32;
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16);
    const std::string m = hex(mask(size));

    if ((insn & 0x1f000000) == 0x0a000000) {  // logical (shifted register)
        const unsigned imm6 = field(insn, 15, 10);
        if (!sf && imm6 >= 32) return false;
        std::string b = shift(rx(rm), field(insn, 23, 22), imm6, sf);
        if (bit(insn, 21)) b = "~" + b;
        static const char* kOps[] = {" & ", " | ", " ^ ", " & "};
        const unsigned opc = field(insn, 30, 29);
        line("{ const uint64_t r = (" + rx(rn) + kOps[opc] + "(" + b + ")) & " + m + ";");
        if (opc == 3) {
            flags_written_ = true;
            line(fmt("  FN = (r >> %u) & 1; FZ = r == 0; FC = 0; FV = 0;", size - 1));
        }
        wx(rd, "r", true);
        line("}");
        return true;
    }
    if ((insn & 0x1f200000) == 0x0b000000) {  // add/sub (shifted register)
        const unsigned imm6 = field(insn, 15, 10), type = field(insn, 23, 22);
        if (type == 3 || (!sf && imm6 >= 32)) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const std::string b = shift(rx(rm), type, imm6, sf);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s(%s), %u, %s)", size, rx(rn).c_str(), sub ? "~" : "", b.c_str(), unsigned(sub),
                       flags()),
               sf);
        } else {
            wx(rd, rx(rn) + (sub ? " - " : " + ") + "(" + b + ")", sf);
        }
        return true;
    }
    if ((insn & 0x1f200000) == 0x0b200000) {  // add/sub (extended register)
        const unsigned imm3 = field(insn, 12, 10);
        if (imm3 > 4 || field(insn, 23, 22) != 0) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        const std::string b = extend(rx(rm), field(insn, 15, 13), imm3);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s(%s), %u, %s)", size, rxsp(rn).c_str(), sub ? "~" : "", b.c_str(),
                       unsigned(sub), flags()),
               sf);
        } else {
            wxsp(rd, rxsp(rn) + (sub ? " - " : " + ") + b, sf);
        }
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a000000) {  // ADC/ADCS/SBC/SBCS
        if (field(insn, 15, 10) != 0) return false;
        const bool sub = bit(insn, 30), s = bit(insn, 29);
        flags_used_ = true;
        const std::string b = std::string(sub ? "~" : "") + rx(rm);
        if (s) {
            wx(rd, fmt("addf%u(%s, %s, FC, %s)", size, rx(rn).c_str(), b.c_str(), flags()), sf);
        } else {
            wx(rd, rx(rn) + " + (" + b + ") + FC", sf);
        }
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a400000) {  // CCMN/CCMP (register and immediate)
        if (!bit(insn, 29) || bit(insn, 10) || bit(insn, 4)) return false;
        const bool sub = bit(insn, 30);
        const std::string c = cond(field(insn, 15, 12));
        const std::string b = bit(insn, 11) ? hex(rm) : rx(rm);
        const unsigned nzcv = field(insn, 3, 0);
        line("if (" + c + ") {");
        line(fmt("    (void)addf%u(%s, %s(%s), %u, %s);", size, rx(rn).c_str(), sub ? "~" : "", b.c_str(),
                 unsigned(sub), flags()));
        line(fmt("} else { FN = %u; FZ = %u; FC = %u; FV = %u; }", (nzcv >> 3) & 1, (nzcv >> 2) & 1, (nzcv >> 1) & 1,
                 nzcv & 1));
        return true;
    }
    if ((insn & 0x1fe00000) == 0x1a800000) {  // CSEL/CSINC/CSINV/CSNEG
        if (bit(insn, 29) || bit(insn, 11)) return false;
        const unsigned op = (bit(insn, 30) << 1) | bit(insn, 10);
        static const char* kAlt[] = {"%s", "%s + 1", "~%s", "0 - %s"};
        const std::string alt = fmt(kAlt[op], rx(rm).c_str());
        wx(rd, "(" + cond(field(insn, 15, 12)) + " ? " + rx(rn) + " : (" + alt + ")) & " + m, true);
        return true;
    }
    if ((insn & 0x5fe00000) == 0x5ac00000) {  // data processing (1 source)
        const unsigned opcode2 = field(insn, 20, 16), opcode = field(insn, 15, 10);
        if (bit(insn, 29)) return false;
        if (opcode2 == 1) return sf;  // PAC*/AUT*/XPAC*: nothing to do (pointers are never signed)
        if (opcode2 != 0) return false;
        const std::string a = "(" + rx(rn) + " & " + m + ")";
        switch (opcode) {
            case 0:
                wx(rd, sf ? "__builtin_bitreverse64(" + a + ")" : "__builtin_bitreverse32((uint32_t)" + a + ")", true);
                return true;
            case 1:
                wx(rd, "rev16_64(" + a + ") & " + m, true);
                return true;
            case 2:
                wx(rd, sf ? "rev32_64(" + a + ")" : "__builtin_bswap32((uint32_t)" + a + ")", true);
                return true;
            case 3:
                if (!sf) return false;
                wx(rd, "__builtin_bswap64(" + a + ")", true);
                return true;
            case 4:
                wx(rd, fmt("clz%u(%s)", size, a.c_str()), true);
                return true;
            case 5:  // CLS
                line("{ const uint64_t t = (" + a + " ^ (" + a + " << 1)) & " + m + ";");
                wx(rd, fmt("t == 0 ? %u : clz%u(t)", size - 1, size), true);
                line("}");
                return true;
            default:
                return false;
        }
    }
    if ((insn & 0x5fe00000) == 0x1ac00000) {  // data processing (2 source)
        if (bit(insn, 29)) return false;
        const unsigned opcode = field(insn, 15, 10);
        const std::string a = "(" + rx(rn) + " & " + m + ")", b = "(" + rx(rm) + " & " + m + ")";
        switch (opcode) {
            case 2: wx(rd, fmt("udiv%u(%s, %s)", size, a.c_str(), b.c_str()), true); return true;
            case 3: wx(rd, fmt("sdiv%u(%s, %s)", size, a.c_str(), b.c_str()), true); return true;
            case 8: case 9: case 10: case 11: {  // LSLV, LSRV, ASRV, RORV
                const std::string amt = fmt("(unsigned)(%s %% %u)", b.c_str(), size);
                std::string r;
                switch (opcode - 8) {
                    case 0: r = "(" + a + " << " + amt + ") & " + m; break;
                    case 1: r = a + " >> " + amt; break;
                    case 2: r = fmt("asr%u(%s, %s)", size, a.c_str(), amt.c_str()); break;
                    default: r = fmt("ror%u(%s, %s)", size, a.c_str(), amt.c_str()); break;
                }
                wx(rd, r, true);
                return true;
            }
            case 12: wx(rd, "0", true); return true;  // PACGA
            default: return false;                    // CRC32*: the interpreter
        }
    }
    if ((insn & 0x1f000000) == 0x1b000000) {  // data processing (3 source)
        const unsigned op = (field(insn, 30, 29) << 4) | (field(insn, 23, 21) << 1) | bit(insn, 15);
        const std::string a = rx(rn), b = rx(rm), acc = rx(field(insn, 14, 10));
        switch (op) {
            case 0x00: wx(rd, acc + " + " + a + " * " + b, sf); return true;  // MADD
            case 0x01: wx(rd, acc + " - " + a + " * " + b, sf); return true;  // MSUB
            case 0x02: if (!sf) return false;
                wx(rd, acc + " + (uint64_t)((int64_t)(int32_t)" + a + " * (int32_t)" + b + ")", true); return true;
            case 0x03: if (!sf) return false;
                wx(rd, acc + " - (uint64_t)((int64_t)(int32_t)" + a + " * (int32_t)" + b + ")", true); return true;
            case 0x04: if (!sf) return false;  // SMULH
                wx(rd, "(uint64_t)(((__int128)(int64_t)" + a + " * (int64_t)" + b + ") >> 64)", true); return true;
            case 0x0a: if (!sf) return false;
                wx(rd, acc + " + (uint64_t)(uint32_t)" + a + " * (uint32_t)" + b, true); return true;
            case 0x0b: if (!sf) return false;
                wx(rd, acc + " - (uint64_t)(uint32_t)" + a + " * (uint32_t)" + b, true); return true;
            case 0x0c: if (!sf) return false;  // UMULH
                wx(rd, "(uint64_t)(((unsigned __int128)" + a + " * " + b + ") >> 64)", true); return true;
            default: return false;
        }
    }
    return false;
}

// LDR/STR (immediate, unscaled, pre/post-indexed, register offset), general
// and SIMD&FP registers.
bool Emitter::ldst_register(uint32_t insn) {
    const bool simd = bit(insn, 26);
    const unsigned size = field(insn, 31, 30), opc = field(insn, 23, 22);
    const unsigned rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    unsigned scale = size;
    if (simd) {
        if (opc & 2) {
            if (size != 0) return false;
            scale = 4;  // Q
        }
    }
    if (!simd && ((size == 3 && opc == 3) || (size == 2 && opc == 3))) return false;
    if (!simd && size == 3 && opc == 2 && bit(insn, 24)) return true;  // PRFM: nothing to do
    if (!bit(insn, 24) && bit(insn, 21) && field(insn, 11, 10) != 2) return false;  // atomics, LDRAA/LDRAB
    if (!bit(insn, 24) && bit(insn, 21) && !(field(insn, 15, 13) & 2)) return false;
    std::string a, wb;
    const std::string base = rxsp(rn);
    if (bit(insn, 24)) {
        a = base + " + " + hex(uint64_t(field(insn, 21, 10)) << scale);
    } else if (!bit(insn, 21)) {
        const int64_t imm9 = sext(field(insn, 20, 12), 9);
        switch (field(insn, 11, 10)) {
            case 0: case 2: a = base + " + " + hex(static_cast<uint64_t>(imm9)); break;
            case 1: a = base; wb = base + " + " + hex(static_cast<uint64_t>(imm9)); break;
            default: a = base + " + " + hex(static_cast<uint64_t>(imm9)); wb = "a"; break;
        }
    } else if (field(insn, 11, 10) == 2) {
        const unsigned option = field(insn, 15, 13);
        if (!(option & 2)) return false;
        a = base + " + " + extend(rx(field(insn, 20, 16)), option, bit(insn, 12) ? scale : 0);
    } else {
        return false;  // atomics, LDRAA/LDRAB
    }
    line("{ const uint64_t a = " + a + ";");
    if (simd) {
        const bool load = opc & 1;
        if (scale == 4) {
            if (load) wv(rt, "LD64(a)", "LD64(a + 8)");
            else line("  ST64(a, " + vl(rt) + "); ST64(a + 8, " + vh(rt) + ");");
        } else {
            static const char* kLd[] = {"LD8(a)", "LD16(a)", "LD32(a)", "LD64(a)"};
            static const char* kSt[] = {"ST8", "ST16", "ST32", "ST64"};
            if (load) wv(rt, kLd[size], "0");
            else line(std::string("  ") + kSt[size] + "(a, " + vl(rt) + ");");
        }
    } else {
        static const char* kLd[] = {"LD8(a)", "LD16(a)", "LD32(a)", "LD64(a)"};
        static const char* kSt[] = {"ST8", "ST16", "ST32", "ST64"};
        static const char* kSx[] = {"(int64_t)(int8_t)", "(int64_t)(int16_t)", "(int64_t)(int32_t)", ""};
        if (opc == 0) {
            line(std::string("  ") + kSt[size] + "(a, " + rx(rt) + ");");
        } else if (opc == 1) {
            wx(rt, kLd[size], true);
        } else if (size == 3) {
            // PRFM / PRFUM: nothing to do
        } else if (opc == 2) {
            wx(rt, std::string("(uint64_t)") + kSx[size] + kLd[size], true);
        } else {
            wx(rt, std::string("(uint64_t)") + kSx[size] + kLd[size], false);
        }
    }
    if (!wb.empty()) {
        line("  " + reg(rn) + " = " + wb + ";");
    }
    line("}");
    return true;
}

bool Emitter::ldst_pair(uint32_t insn) {
    const bool simd = bit(insn, 26);
    const unsigned opc = field(insn, 31, 30), mode = field(insn, 24, 23);
    const bool l = bit(insn, 22);
    const unsigned rt2 = field(insn, 14, 10), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    unsigned scale;
    if (simd) {
        if (opc == 3) return false;
        scale = 2 + opc;
    } else {
        if (opc == 3 || (opc == 1 && !l)) return false;
        scale = opc == 2 ? 3 : 2;
    }
    const int64_t off = sext(field(insn, 21, 15), 7) << scale;
    const std::string base = rxsp(rn);
    line("{ const uint64_t base = " + base + ";");
    line("  const uint64_t a = " + std::string(mode == 1 ? "base" : "base + " + hex(static_cast<uint64_t>(off))) +
         ";");
    const unsigned bytes = 1u << scale;
    if (simd) {
        static const char* kLd[] = {"", "", "LD32", "LD64"};
        static const char* kSt[] = {"", "", "ST32", "ST64"};
        if (l) {
            if (bytes == 16) {
                line("  const uint64_t a0 = LD64(a), a1 = LD64(a + 8), b0 = LD64(a + 16), b1 = LD64(a + 24);");
                wv(rt, "a0", "a1");
                wv(rt2, "b0", "b1");
            } else {
                line(fmt("  const uint64_t a0 = %s(a), b0 = %s(a + %u);", kLd[scale], kLd[scale], bytes));
                wv(rt, "a0", "0");
                wv(rt2, "b0", "0");
            }
        } else if (bytes == 16) {
            line("  ST64(a, " + vl(rt) + "); ST64(a + 8, " + vh(rt) + "); ST64(a + 16, " + vl(rt2) +
                 "); ST64(a + 24, " + vh(rt2) + ");");
        } else {
            line(fmt("  %s(a, %s); %s(a + %u, %s);", kSt[scale], vl(rt).c_str(), kSt[scale], bytes,
                     vl(rt2).c_str()));
        }
    } else if (l) {
        const char* ld = scale == 3 ? "LD64" : "LD32";
        const char* sx = opc == 1 ? "(uint64_t)(int64_t)(int32_t)" : "(uint64_t)";
        line(fmt("  const uint64_t a0 = %s%s(a), b0 = %s%s(a + %u);", sx, ld, sx, ld, bytes));
        wx(rt, "a0", true);
        wx(rt2, "b0", true);
    } else {
        const char* st = scale == 3 ? "ST64" : "ST32";
        line(fmt("  %s(a, %s); %s(a + %u, %s);", st, rx(rt).c_str(), st, bytes, rx(rt2).c_str()));
    }
    if (mode == 1 || mode == 3) line("  " + reg(rn) + " = base + " + hex(static_cast<uint64_t>(off)) + ";");
    line("}");
    return true;
}

bool Emitter::loads_stores(uint32_t insn, uint64_t va) {
    if ((insn & 0xbf000000) == 0x0c000000 || (insn & 0xbf000000) == 0x0d000000) return structures(insn);
    if ((insn & 0x3f000000) == 0x08000000) return false;  // exclusives, acquire/release: the interpreter
    if ((insn & 0x3b000000) == 0x18000000) {  // LDR (literal)
        const uint64_t a = va + static_cast<uint64_t>(sext(field(insn, 23, 5), 19) << 2);
        const unsigned rt = field(insn, 4, 0);
        if (bit(insn, 26)) {
            switch (field(insn, 31, 30)) {
                case 0: wv(rt, "LD32(" + addr(a) + ")", "0"); return true;
                case 1: wv(rt, "LD64(" + addr(a) + ")", "0"); return true;
                case 2: wv(rt, "LD64(" + addr(a) + ")", "LD64(" + addr(a + 8) + ")"); return true;
                default: return false;
            }
        }
        switch (field(insn, 31, 30)) {
            case 0: wx(rt, "LD32(" + addr(a) + ")", true); return true;
            case 1: wx(rt, "LD64(" + addr(a) + ")", true); return true;
            case 2: wx(rt, "(uint64_t)(int64_t)(int32_t)LD32(" + addr(a) + ")", true); return true;
            default: return true;  // PRFM (literal)
        }
    }
    if ((insn & 0x3a000000) == 0x28000000) return ldst_pair(insn);
    if ((insn & 0x3a000000) == 0x38000000) return ldst_register(insn);
    return false;
}

bool Emitter::system(uint32_t insn) {
    if ((insn & 0xfffff01f) == 0xd503201f) return true;  // hints: NOP, YIELD, BTI, PACIASP, ...
    if ((insn & 0xfffff09f) == 0xd503309f) {             // DSB / DMB / ISB
        const unsigned crm = field(insn, 11, 8);
        if (field(insn, 7, 5) != 6 && (crm & 3) == 3) line("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
        else line("__atomic_signal_fence(__ATOMIC_SEQ_CST);");
        return true;
    }
    if ((insn & 0xffffffe0) == 0xd53bd040) {  // MRS Xt, TPIDR_EL0
        wx(field(insn, 4, 0), "c->tpidr", true);
        return true;
    }
    return false;
}

// ---- SIMD&FP ---------------------------------------------------------------------
//
// Each case follows its counterpart in src/arm64/arm64_simd.cpp; whatever is
// not here (half precision, saturating and rounding forms, tables, LD3/LD4,
// most scalar AdvSIMD) goes through the interpreter.

bool Emitter::simd(uint32_t insn) {
    if ((insn & 0x5f200000) == 0x1e200000) return fp_data(insn);
    if ((insn & 0x5f200000) == 0x1e000000) return false;  // fixed-point conversions
    if ((insn & 0x5f000000) == 0x1f000000) return fp_three_source(insn);
    if ((insn & 0x9f200400) == 0x0e200400) return v_three_same(insn);
    if ((insn & 0x9f200c00) == 0x0e200000) return v_three_different(insn);
    if ((insn & 0x9f3e0c00) == 0x0e200800) return v_two_reg_misc(insn);
    if ((insn & 0x9f3e0c00) == 0x0e300800) return v_across(insn);
    if ((insn & 0x9fe08400) == 0x0e000400) return v_copy(insn, false);
    if ((insn & 0xbf208c00) == 0x0e000800) return v_permute(insn);
    if ((insn & 0xbf208c00) == 0x0e000000) return false;  // TBL/TBX
    if ((insn & 0xbfe08400) == 0x2e000000) return v_extract(insn);
    if ((insn & 0x9ff80400) == 0x0f000400) return v_modified_imm(insn);
    if ((insn & 0x9f800400) == 0x0f000400) return v_shift_imm(insn);
    if ((insn & 0x9f000400) == 0x0f000000) return v_by_element(insn);
    if ((insn & 0xdf200400) == 0x5e200400) return s_three_same(insn);
    if ((insn & 0xdf3e0c00) == 0x5e300800) return s_pairwise(insn);
    if ((insn & 0xdfe08400) == 0x5e000400) return v_copy(insn, true);
    return false;
}

bool Emitter::fp_data(uint32_t insn) {
    const unsigned type = field(insn, 23, 22);
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16);
    const std::string T = type ? "double" : "float";
    if (field(insn, 15, 10) == 0) {  // conversions between FP and integer
        const bool sf = bit(insn, 31);
        const unsigned rmode = field(insn, 20, 19), opcode = field(insn, 18, 16);
        if (opcode == 6 || opcode == 7) {  // FMOV general <-> FP
            if (type == 2) {
                if (!sf || rmode != 1) return false;
            } else if (rmode != 0 || type == 3 || (type == 0 && sf) || (type == 1 && !sf)) {
                return false;
            }
            if (opcode == 6) {
                if (rmode == 1) wx(rd, vh(rn), true);
                else wx(rd, sf ? vl(rn) : "(uint32_t)" + vl(rn), true);
            } else if (rmode == 1) {
                wv(rd, vl(rd), rx(rn));
            } else {
                wv(rd, sf ? rx(rn) : "(uint32_t)" + rx(rn), "0");
            }
            return true;
        }
        if (type > 1) return false;
        if (opcode == 2 || opcode == 3) {  // SCVTF, UCVTF
            if (rmode != 0) return false;
            const std::string cast = opcode == 3 ? (sf ? "(uint64_t)" : "(uint32_t)") : (sf ? "(int64_t)" : "(int32_t)");
            wfp(rd, type, "(" + T + ")(" + cast + rx(rn) + ")");
            return true;
        }
        unsigned mode;
        bool uns;
        if (opcode <= 1) {  // FCVT{N,P,M,Z}{S,U}
            mode = rmode;
            uns = opcode == 1;
        } else if ((opcode == 4 || opcode == 5) && rmode == 0) {  // FCVTA{S,U}
            mode = 4;
            uns = opcode == 5;
        } else {
            return false;
        }
        wx(rd, fmt("sat_%c%u(%s)", uns ? 'u' : 's', sf ? 64 : 32, round_expr(fpv(rn, type), type, mode).c_str()),
           true);
        return true;
    }
    if (type > 1) return false;
    const char* sfx = type ? "d" : "f";
    switch (field(insn, 11, 10)) {
        case 1: return false;  // FCCMP
        case 2: {
            const std::string a = fpv(rn, type), b = fpv(rm, type);
            std::string r;
            switch (field(insn, 15, 12)) {
                case 0: r = a + " * " + b; break;
                case 1: r = a + " / " + b; break;
                case 2: r = a + " + " + b; break;
                case 3: r = a + " - " + b; break;
                case 4: r = fmt("fmax_%s(%s, %s)", sfx, a.c_str(), b.c_str()); break;
                case 5: r = fmt("fmin_%s(%s, %s)", sfx, a.c_str(), b.c_str()); break;
                case 6: r = fmt("fmaxnm_%s(%s, %s)", sfx, a.c_str(), b.c_str()); break;
                case 7: r = fmt("fminnm_%s(%s, %s)", sfx, a.c_str(), b.c_str()); break;
                case 8: r = "-(" + a + " * " + b + ")"; break;
                default: return false;
            }
            wfp(rd, type, "(" + T + ")(" + r + ")");
            return true;
        }
        case 3:  // FCSEL
            wv(rd, "(" + cond(field(insn, 15, 12)) + " ? " + vl(rn) + " : " + vl(rm) + ") & " + hex(mask(type ? 64 : 32)),
               "0");
            return true;
        default: break;
    }
    if (field(insn, 12, 10) == 4) {  // FMOV (scalar, immediate)
        if (field(insn, 9, 5) != 0) return false;
        wv(rd, hex(vfp_expand_imm(field(insn, 20, 13), type ? 64 : 32)), "0");
        return true;
    }
    if (field(insn, 13, 10) == 8) {  // FCMP, FCMPE
        const bool zero = bit(insn, 3);
        flags_written_ = true;
        line("{ const double a = " + fpv(rn, type) + ", b = " + (zero ? std::string("0.0") : fpv(rm, type)) + ";");
        line("  if (a != a || b != b) { FN = 0; FZ = 0; FC = 1; FV = 1; }");
        line("  else if (a == b) { FN = 0; FZ = 1; FC = 1; FV = 0; }");
        line("  else if (a < b) { FN = 1; FZ = 0; FC = 0; FV = 0; }");
        line("  else { FN = 0; FZ = 0; FC = 1; FV = 0; } }");
        return true;
    }
    if (field(insn, 14, 10) == 0x10) {  // one-source
        const unsigned opcode = field(insn, 20, 15);
        const std::string a = fpv(rn, type);
        if (opcode >= 4 && opcode <= 7) {  // FCVT between single and double
            const unsigned to = opcode & 3;
            if (to == type || to > 1) return false;
            wfp(rd, to, std::string(to ? "(double)" : "(float)") + a);
            return true;
        }
        std::string r;
        switch (opcode) {
            case 0: wv(rd, vl(rn) + " & " + hex(mask(type ? 64 : 32)), "0"); return true;  // FMOV (register)
            case 1: r = fmt("__builtin_fabs%s(%s)", type ? "" : "f", a.c_str()); break;
            case 2: r = "-" + a; break;
            case 3: r = fmt("__builtin_sqrt%s(%s)", type ? "" : "f", a.c_str()); break;
            case 8: r = round_expr(a, type, 0); break;
            case 9: r = round_expr(a, type, 1); break;
            case 10: r = round_expr(a, type, 2); break;
            case 11: r = round_expr(a, type, 3); break;
            case 12: r = round_expr(a, type, 4); break;
            case 14: case 15: r = round_expr(a, type, 0); break;
            default: return false;
        }
        wfp(rd, type, "(" + T + ")(" + r + ")");
        return true;
    }
    return false;
}

bool Emitter::fp_three_source(uint32_t insn) {
    const unsigned type = field(insn, 23, 22);
    if (type > 1) return false;
    const bool o1 = bit(insn, 21), o0 = bit(insn, 15);
    const unsigned rd = field(insn, 4, 0), rn = field(insn, 9, 5), rm = field(insn, 20, 16), ra = field(insn, 14, 10);
    // FMADD: a + n*m; FMSUB: a - n*m; FNMADD: -a - n*m; FNMSUB: -a + n*m. All fused.
    const bool neg_n = o0 != o1, neg_a = o1;
    wfp(rd, type, fmt("__builtin_fma%s(%s%s, %s, %s%s)", type ? "" : "f", neg_n ? "-" : "", fpv(rn, type).c_str(),
                      fpv(rm, type).c_str(), neg_a ? "-" : "", fpv(ra, type).c_str()));
    return true;
}

bool Emitter::v_three_same(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 11);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const unsigned bytes = q ? 16 : 8;
    const std::string a = vr(rn), b = vr(rm), d = vr(rd);

    if (opcode >= 0x18) {  // floating point
        const bool hi = size & 2;
        const unsigned eb = (size & 1) ? 64 : 32;
        if (eb == 64 && !q) return false;
        const std::string F = ft(eb), FA = "((" + F + ")" + a + ")", FB = "((" + F + ")" + b + ")",
                          FD = "((" + F + ")" + d + ")";
        const char* sfx = eb == 64 ? "d" : "f";
        const char* one = eb == 64 ? "" : "f";
        const unsigned key = (u << 6) | (hi << 5) | opcode, lanes = 128 / eb;
        auto per_lane = [&](const char* fn, const std::string& x, const std::string& y) {
            std::string r = "(" + F + "){";
            for (unsigned i = 0; i < lanes; ++i) r += fmt("%s%s_%s(%s[%u], %s[%u])", i ? ", " : "", fn, sfx, x.c_str(), i, y.c_str(), i);
            return r + "}";
        };
        std::string r;
        switch (key) {
            case 0x1a: r = FA + " + " + FB; break;
            case 0x3a: r = FA + " - " + FB; break;
            case 0x1b: case 0x5b: r = FA + " * " + FB; break;
            case 0x5f: r = FA + " / " + FB; break;
            case 0x19: r = "__builtin_elementwise_fma(" + FA + ", " + FB + ", " + FD + ")"; break;
            case 0x39: r = "__builtin_elementwise_fma(-" + FA + ", " + FB + ", " + FD + ")"; break;
            case 0x7a: r = "__builtin_elementwise_abs(" + FA + " - " + FB + ")"; break;
            case 0x1c: r = "(" + FA + " == " + FB + ")"; break;
            case 0x5c: r = "(" + FA + " >= " + FB + ")"; break;
            case 0x7c: r = "(" + FA + " > " + FB + ")"; break;
            case 0x5d: r = "(__builtin_elementwise_abs(" + FA + ") >= __builtin_elementwise_abs(" + FB + "))"; break;
            case 0x7d: r = "(__builtin_elementwise_abs(" + FA + ") > __builtin_elementwise_abs(" + FB + "))"; break;
            case 0x1f: r = fmt("(2.0%s - %s * %s)", one, FA.c_str(), FB.c_str()); break;
            case 0x3f: r = fmt("((3.0%s - %s * %s) / 2.0%s)", one, FA.c_str(), FB.c_str(), one); break;
            case 0x1e: r = per_lane("fmax", FA, FB); break;
            case 0x3e: r = per_lane("fmin", FA, FB); break;
            case 0x18: r = per_lane("fmaxnm", FA, FB); break;
            case 0x38: r = per_lane("fminnm", FA, FB); break;
            case 0x5a: case 0x5e: case 0x7e: case 0x58: case 0x78: {  // pairwise
                const unsigned n = bytes * 8 / eb;  // result lanes
                std::vector<unsigned> even, odd;
                for (unsigned i = 0; i < n; ++i) {
                    const unsigned src = (i < n / 2 ? 0 : lanes) + (i % (n / 2)) * 2;
                    even.push_back(src);
                    odd.push_back(src + 1);
                }
                const std::string ev = shuffle(F.c_str(), a, b, pad(even, lanes)),
                                  od = shuffle(F.c_str(), a, b, pad(odd, lanes));
                line("{ const " + F + " pe = " + ev + ", po = " + od + ";");
                switch (key) {
                    case 0x5a: wq(rd, "pe + po", bytes); break;
                    case 0x5e: wq(rd, per_lane("fmax", "pe", "po"), bytes); break;
                    case 0x7e: wq(rd, per_lane("fmin", "pe", "po"), bytes); break;
                    case 0x58: wq(rd, per_lane("fmaxnm", "pe", "po"), bytes); break;
                    default: wq(rd, per_lane("fminnm", "pe", "po"), bytes); break;
                }
                line("}");
                return true;
            }
            default: return false;
        }
        wq(rd, r, bytes);
        return true;
    }

    if (opcode == 3) {  // bitwise: AND BIC ORR ORN / EOR BSL BIT BIF
        std::string r;
        switch ((u << 2) | size) {
            case 0: r = a + " & " + b; break;
            case 1: r = a + " & ~" + b; break;
            case 2: r = a + " | " + b; break;
            case 3: r = a + " | ~" + b; break;
            case 4: r = a + " ^ " + b; break;
            case 5: r = "(" + d + " & " + a + ") | (~" + d + " & " + b + ")"; break;  // BSL
            case 6: r = "(" + b + " & " + a + ") | (~" + b + " & " + d + ")"; break;  // BIT
            default: r = "(~" + b + " & " + a + ") | (" + b + " & " + d + ")"; break;  // BIF
        }
        wq(rd, r, bytes);
        return true;
    }
    const unsigned eb = 8u << size;
    if (size == 3 && !q) return false;
    const std::string U = ut(eb), S = st(eb);
    const std::string UA = "((" + U + ")" + a + ")", UB = "((" + U + ")" + b + ")", UD = "((" + U + ")" + d + ")";
    const std::string SA = "((" + S + ")" + a + ")", SB = "((" + S + ")" + b + ")";
    std::string r;
    switch (opcode) {
        case 0x10: r = u ? UA + " - " + UB : UA + " + " + UB; break;
        case 0x11: r = u ? "(" + UA + " == " + UB + ")" : "((" + UA + " & " + UB + ") != 0)"; break;
        case 0x06: r = u ? "(" + UA + " > " + UB + ")" : "(" + SA + " > " + SB + ")"; break;
        case 0x07: r = u ? "(" + UA + " >= " + UB + ")" : "(" + SA + " >= " + SB + ")"; break;
        case 0x0c: r = u ? "__builtin_elementwise_max(" + UA + ", " + UB + ")" : "__builtin_elementwise_max(" + SA + ", " + SB + ")"; break;
        case 0x0d: r = u ? "__builtin_elementwise_min(" + UA + ", " + UB + ")" : "__builtin_elementwise_min(" + SA + ", " + SB + ")"; break;
        case 0x12: r = u ? UD + " - " + UA + " * " + UB : UD + " + " + UA + " * " + UB; break;
        case 0x13: if (u) return false; r = UA + " * " + UB; break;
        case 0x17: {  // ADDP
            if (u) return false;
            const unsigned lanes = 128 / eb, n = bytes * 8 / eb;
            std::vector<unsigned> even, odd;
            for (unsigned i = 0; i < n; ++i) {
                const unsigned src = (i < n / 2 ? 0 : lanes) + (i % (n / 2)) * 2;
                even.push_back(src);
                odd.push_back(src + 1);
            }
            r = shuffle(U.c_str(), a, b, pad(even, lanes)) + " + " + shuffle(U.c_str(), a, b, pad(odd, lanes));
            break;
        }
        default: return false;
    }
    wq(rd, r, bytes);
    return true;
}

bool Emitter::v_three_different(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 12);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (size == 3) return false;
    const unsigned eb = 8u << size, wb = eb * 2, n = 64 / eb, off = q ? n : 0;
    const std::string N = u ? ut(eb) : st(eb), W = u ? ut(wb) : st(wb);
    auto widen = [&](const std::string& v) {
        return "__builtin_convertvector(" + shuffle(N.c_str(), v, v, iota(off, n)) + ", " + W + ")";
    };
    const std::string NA = widen(vr(rn)), NB = widen(vr(rm));
    const std::string WA = "((" + W + ")" + vr(rn) + ")", WD = "((" + W + ")" + vr(rd) + ")";
    std::string r;
    switch (opcode) {
        case 0x0: r = NA + " + " + NB; break;
        case 0x1: r = WA + " + " + NB; break;
        case 0x2: r = NA + " - " + NB; break;
        case 0x3: r = WA + " - " + NB; break;
        case 0x8: r = WD + " + " + NA + " * " + NB; break;
        case 0xa: r = WD + " - " + NA + " * " + NB; break;
        case 0xc: r = NA + " * " + NB; break;
        case 0x7: r = "__builtin_elementwise_max(" + NA + ", " + NB + ") - __builtin_elementwise_min(" + NA + ", " + NB + ")"; break;
        case 0x5: r = WD + " + (__builtin_elementwise_max(" + NA + ", " + NB + ") - __builtin_elementwise_min(" + NA + ", " + NB + "))"; break;
        default: return false;
    }
    wq(rd, r);
    return true;
}

bool Emitter::v_two_reg_misc(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const unsigned bytes = q ? 16 : 8;
    const std::string a = vr(rn);
    const bool fp = (opcode >= 0x0c && opcode <= 0x0f && (size & 2)) || opcode >= 0x16;
    if (fp) {
        const unsigned eb = (size & 1) ? 64 : 32;
        const bool hi = size & 2;
        if (opcode == 0x16) {  // FCVTN/FCVTN2, double -> single
            if (hi || !(size & 1)) return false;
            const std::string conv = "__builtin_convertvector((v2f64)" + a + ", v2f32)";
            line("{ const v2f32 cv = " + conv + ";");
            line("  const v4f32 c4 = __builtin_shufflevector(cv, cv, 0, 1, 0, 1);");
            if (q) wq(rd, "__builtin_shufflevector((v4f32)" + vr(rd) + ", c4, 0, 1, 4, 5)");
            else wq(rd, "c4", 8);
            line("}");
            return true;
        }
        if (opcode == 0x17 && !u) {  // FCVTL/FCVTL2, single -> double
            if (hi || !(size & 1)) return false;
            wq(rd, "__builtin_convertvector(__builtin_shufflevector((v4f32)" + a + ", (v4f32)" + a +
                       (q ? ", 2, 3)" : ", 0, 1)") + ", v2f64)");
            return true;
        }
        if (eb == 64 && !q) return false;
        const std::string F = ft(eb), U = ut(eb), S = st(eb), FA = "((" + F + ")" + a + ")";
        const char* one = eb == 64 ? "" : "f";
        const unsigned key = (u << 6) | (hi << 5) | opcode;
        std::string r;
        auto cvt = [&](unsigned mode, bool uns) {
            const unsigned n = 128 / eb;
            std::string s = "(" + U + "){";
            for (unsigned i = 0; i < n; ++i) {
                s += fmt("%ssat_%c%u(%s)", i ? ", " : "", uns ? 'u' : 's', eb,
                         round_expr(fmt("%s[%u]", FA.c_str(), i), eb == 64, mode).c_str());
            }
            return s + "}";
        };
        switch (key) {
            case 0x2c: r = fmt("(%s > 0.0%s)", FA.c_str(), one); break;
            case 0x2d: r = fmt("(%s == 0.0%s)", FA.c_str(), one); break;
            case 0x2e: r = fmt("(%s < 0.0%s)", FA.c_str(), one); break;
            case 0x6c: r = fmt("(%s >= 0.0%s)", FA.c_str(), one); break;
            case 0x6d: r = fmt("(%s <= 0.0%s)", FA.c_str(), one); break;
            case 0x2f: r = "((" + U + ")" + a + " & " + hex(mask(eb - 1)) + ")"; break;
            case 0x6f: r = "((" + U + ")" + a + " ^ " + hex(uint64_t{1} << (eb - 1)) + ")"; break;
            case 0x7f: r = "__builtin_elementwise_sqrt(" + FA + ")"; break;
            case 0x18: case 0x59: case 0x79: r = "__builtin_elementwise_roundeven(" + FA + ")"; break;
            case 0x38: r = "__builtin_elementwise_ceil(" + FA + ")"; break;
            case 0x19: r = "__builtin_elementwise_floor(" + FA + ")"; break;
            case 0x39: r = "__builtin_elementwise_trunc(" + FA + ")"; break;
            case 0x58: r = "__builtin_elementwise_round(" + FA + ")"; break;
            case 0x1d: r = "__builtin_convertvector((" + S + ")" + a + ", " + F + ")"; break;
            case 0x5d: r = "__builtin_convertvector((" + U + ")" + a + ", " + F + ")"; break;
            case 0x1a: case 0x5a: r = cvt(0, u); break;
            case 0x3a: case 0x7a: r = cvt(1, u); break;
            case 0x1b: case 0x5b: r = cvt(2, u); break;
            case 0x3b: case 0x7b: r = cvt(3, u); break;
            case 0x1c: case 0x5c: r = cvt(4, u); break;
            case 0x3d: case 0x3f: r = fmt("(1.0%s / %s)", one, FA.c_str()); break;
            case 0x7d: r = fmt("(1.0%s / __builtin_elementwise_sqrt(%s))", one, FA.c_str()); break;
            default: return false;
        }
        wq(rd, r, bytes);
        return true;
    }

    const unsigned eb = 8u << size;
    const std::string U = ut(eb), S = st(eb), UA = "((" + U + ")" + a + ")", SA = "((" + S + ")" + a + ")";
    const unsigned lanes = 128 / eb;
    if (opcode == 0x12 && !u) {  // XTN/XTN2: the low half of each wide element
        if (size == 3) return false;
        const unsigned n = 64 / eb;
        if (q) {
            std::vector<unsigned> idx = iota(0, n);
            for (unsigned i = 0; i < n; ++i) idx.push_back(lanes + 2 * i);
            wq(rd, shuffle(U.c_str(), vr(rd), a, idx));
        } else {
            wq(rd, shuffle(U.c_str(), a, a, pad(iota(0, n, 2), lanes)), 8);
        }
        return true;
    }
    if (opcode <= 0x01) {  // REV64 / REV32 / REV16
        const unsigned group = opcode == 1 ? 16 : (u ? 32 : 64);
        if (eb >= group) return false;
        const unsigned per = group / eb, n = bytes * 8 / eb;
        std::vector<unsigned> idx;
        for (unsigned i = 0; i < n; ++i) idx.push_back((i / per) * per + (per - 1 - i % per));
        wq(rd, shuffle(U.c_str(), a, a, pad(idx, lanes)), bytes);
        return true;
    }
    if (opcode == 0x05) {
        if (size == 0) {
            wq(rd, u ? "~" + a : "__builtin_elementwise_popcount((v16u8)" + a + ")", bytes);  // NOT, CNT
            return true;
        }
        if (u && size == 1) {  // RBIT (per byte)
            wq(rd, "__builtin_elementwise_bitreverse((v16u8)" + a + ")", bytes);
            return true;
        }
        return false;
    }
    if (size == 3 && !q) return false;
    std::string r;
    switch ((u << 5) | opcode) {
        case 0x08: r = "(" + SA + " > 0)"; break;
        case 0x09: r = "(" + SA + " == 0)"; break;
        case 0x0a: r = "(" + SA + " < 0)"; break;
        case 0x28: r = "(" + SA + " >= 0)"; break;
        case 0x29: r = "(" + SA + " <= 0)"; break;
        case 0x0b: r = "__builtin_elementwise_abs(" + SA + ")"; break;
        case 0x2b: r = "(0 - " + UA + ")"; break;
        default: return false;
    }
    wq(rd, r, bytes);
    return true;
}

bool Emitter::v_across(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (size == 3 || (size == 2 && !q)) return false;
    const unsigned eb = 8u << size, n = (q ? 16 : 8) * 8 / eb;
    const std::string T = u ? ut(eb) : st(eb), a = vr(rn);
    const std::string src = q ? "((" + T + ")" + a + ")" : shuffle(T.c_str(), a, a, iota(0, n));
    std::string r;
    unsigned obits = eb;
    switch (opcode) {
        case 0x1b: if (u) return false; r = "__builtin_reduce_add(" + src + ")"; break;  // ADDV
        case 0x0a: r = "__builtin_reduce_max(" + src + ")"; break;                         // SMAXV/UMAXV
        case 0x1a: r = "__builtin_reduce_min(" + src + ")"; break;                         // SMINV/UMINV
        case 0x03: {  // SADDLV/UADDLV
            obits = eb * 2;
            const std::string W = n * obits > 128 ? wide32(obits, !u) : (u ? ut(obits) : st(obits));
            r = "__builtin_reduce_add(__builtin_convertvector(" + src + ", " + W + "))";
            break;
        }
        default: return false;
    }
    wv(rd, fmt("(uint64_t)(%s)%s & %s", et(obits, !u), r.c_str(), hex(mask(obits)).c_str()), "0");
    return true;
}

bool Emitter::v_copy(uint32_t insn, bool scalar) {
    const bool q = bit(insn, 30), op = bit(insn, 29);
    const unsigned imm5 = field(insn, 20, 16), imm4 = field(insn, 14, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if ((imm5 & 0xf) == 0) return false;
    const unsigned sz = __builtin_ctz(imm5), eb = 8u << sz, index = imm5 >> (sz + 1);
    const std::string U = ut(eb), S = st(eb);
    const std::string elem = fmt("((%s)%s)[%u]", U.c_str(), vr(rn).c_str(), index);
    if (scalar) {  // DUP (element), scalar
        if (op || imm4 != 0) return false;
        wv(rd, "(uint64_t)" + elem, "0");
        return true;
    }
    if (op) {  // INS (element)
        if (!q) return false;
        line("{ " + U + " t = (" + U + ")" + vr(rd) + "; t[" + std::to_string(index) + "] = " +
             fmt("((%s)%s)[%u]", U.c_str(), vr(rn).c_str(), imm4 >> sz) + ";");
        wq(rd, "t");
        line("}");
        return true;
    }
    switch (imm4) {
        case 0:  // DUP (element)
            if (sz == 3 && !q) return false;
            wq(rd, "(" + U + "){} + " + elem, q ? 16 : 8);
            return true;
        case 1:  // DUP (general)
            if (sz == 3 && !q) return false;
            wq(rd, "(" + U + "){} + (" + et(eb, false) + ")" + rx(rn), q ? 16 : 8);
            return true;
        case 3:  // INS (general)
            line("{ " + U + " t = (" + U + ")" + vr(rd) + "; t[" + std::to_string(index) + "] = (" + et(eb, false) +
                 ")" + rx(rn) + ";");
            wq(rd, "t");
            line("}");
            return true;
        case 5:  // SMOV
            wx(rd, fmt("(uint64_t)(int64_t)((%s)%s)[%u]", S.c_str(), vr(rn).c_str(), index), q);
            return true;
        case 7:  // UMOV
            wx(rd, "(uint64_t)" + elem, true);
            return true;
        default:
            return false;
    }
}

bool Emitter::v_permute(uint32_t insn) {
    const bool q = bit(insn, 30);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 14, 12);
    if (size == 3 && !q) return false;
    const unsigned eb = 8u << size, lanes = 128 / eb, n = (q ? 16 : 8) * 8 / eb, half = n / 2;
    const unsigned part = (opcode >> 2) & 1;
    std::vector<unsigned> idx;
    for (unsigned i = 0; i < n; ++i) {
        switch (opcode & 3) {
            case 1: {  // UZP1/UZP2
                const unsigned src = 2 * i + part;
                idx.push_back(src < n ? src : lanes + src - n);
                break;
            }
            case 2: {  // TRN1/TRN2
                const unsigned pair = i & ~1u;
                idx.push_back((i & 1) ? lanes + pair + part : pair + part);
                break;
            }
            case 3: {  // ZIP1/ZIP2
                const unsigned src = part * half + i / 2;
                idx.push_back((i & 1) ? lanes + src : src);
                break;
            }
            default: return false;
        }
    }
    wq(field(insn, 4, 0), shuffle(ut(eb), vr(field(insn, 9, 5)), vr(field(insn, 20, 16)), pad(idx, lanes)),
       q ? 16 : 8);
    return true;
}

bool Emitter::v_extract(uint32_t insn) {  // EXT
    const bool q = bit(insn, 30);
    const unsigned imm4 = field(insn, 14, 11), bytes = q ? 16 : 8;
    if (!q && imm4 >= 8) return false;
    std::vector<unsigned> idx;
    for (unsigned j = imm4; j < imm4 + bytes; ++j) idx.push_back(j < bytes ? j : 16 + (j - bytes));
    wq(field(insn, 4, 0), shuffle("v16u8", vr(field(insn, 9, 5)), vr(field(insn, 20, 16)), pad(idx, 16)), bytes);
    return true;
}

bool Emitter::v_modified_imm(uint32_t insn) {
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
    const std::string k = "(v2u64){" + hex(imm) + ", " + hex(imm) + "}";
    const bool orr_bic = (cmode < 12) && (cmode & 1);
    std::string r;
    if (orr_bic) r = op ? vr(rd) + " & ~" + k : vr(rd) + " | " + k;
    else if (op && cmode != 14 && cmode != 15) r = "~" + k;  // MVNI
    else r = k;
    wq(rd, r, q ? 16 : 8);
    return true;
}

bool Emitter::v_shift_imm(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned immh = field(insn, 22, 19), immb = field(insn, 18, 16), opcode = field(insn, 15, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (immh == 0) return false;
    const unsigned eb = 8u << (31 - __builtin_clz(immh)), immhb = (immh << 3) | immb;
    const unsigned rshift = 2 * eb - immhb, lshift = immhb - eb, lanes = 128 / eb;
    const std::string a = vr(rn), d = vr(rd), U = ut(eb), S = st(eb);
    const std::string UA = "((" + U + ")" + a + ")", SA = "((" + S + ")" + a + ")", UD = "((" + U + ")" + d + ")";
    if (opcode == 0x10 && !u) {  // SHRN/SHRN2: eb is the narrow size
        if (eb == 64) return false;
        const unsigned n = 64 / eb;
        line(fmt("{ const %s sh = (%s)%s >> %u;", ut(2 * eb), ut(2 * eb), a.c_str(), rshift));
        if (q) {
            std::vector<unsigned> idx = iota(0, n);
            for (unsigned i = 0; i < n; ++i) idx.push_back(lanes + 2 * i);
            wq(rd, shuffle(U.c_str(), d, "sh", idx));
        } else {
            wq(rd, shuffle(U.c_str(), "sh", "sh", pad(iota(0, n, 2), lanes)), 8);
        }
        line("}");
        return true;
    }
    if (opcode == 0x14) {  // SSHLL/USHLL (SXTL/UXTL) and the "2" forms
        if (eb == 64) return false;
        const unsigned n = 64 / eb, base = q ? n : 0;
        const std::string N = u ? U : S, W = u ? ut(2 * eb) : st(2 * eb);
        // widened (sign- or zero-extended), then shifted as unsigned
        wq(rd, std::string("((") + ut(2 * eb) + ")__builtin_convertvector(" + shuffle(N.c_str(), a, a, iota(base, n)) +
                   ", " + W + ") << " + std::to_string(lshift) + ")");
        return true;
    }
    if (eb == 64 && !q) return false;
    const unsigned bytes = q ? 16 : 8;
    const std::string shr = rshift == eb ? (u ? "((" + U + "){})" : "(" + SA + " >> " + std::to_string(eb - 1) + ")")
                                         : (u ? "(" + UA + " >> " : "(" + SA + " >> ") + std::to_string(rshift) + ")";
    switch (opcode) {
        case 0x00: wq(rd, shr, bytes); return true;                         // SSHR/USHR
        case 0x02: wq(rd, UD + " + (" + U + ")" + shr, bytes); return true;  // SSRA/USRA
        case 0x0a:
            if (!u) {  // SHL
                wq(rd, UA + " << " + std::to_string(lshift), bytes);
            } else {  // SLI
                wq(rd, "(" + UD + " & " + hex(mask(lshift)) + ") | (" + UA + " << " + std::to_string(lshift) + ")",
                   bytes);
            }
            return true;
        case 0x08: {  // SRI
            if (!u) return false;
            const uint64_t m = mask(eb), keep = rshift == eb ? m : ~(m >> rshift) & m;
            const std::string moved = rshift == eb ? "0" : "(" + UA + " >> " + std::to_string(rshift) + ")";
            wq(rd, "(" + UD + " & " + hex(keep) + ") | " + moved, bytes);
            return true;
        }
        default:
            return false;
    }
}

bool Emitter::v_by_element(uint32_t insn) {
    const bool q = bit(insn, 30), u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 12);
    const unsigned l = bit(insn, 21), mbit = bit(insn, 20), h = bit(insn, 11);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    const unsigned bytes = q ? 16 : 8;
    unsigned rm = field(insn, 19, 16), index;
    if (opcode == 0x1 || opcode == 0x5 || opcode == 0x9) {  // FMLA, FMLS, FMUL
        if (size < 2) return false;
        const bool dbl = size & 1;
        if (dbl && l) return false;
        index = dbl ? h : (h << 1) | l;
        rm |= mbit << 4;
        if (dbl && !q) return false;
        const std::string F = ft(dbl ? 64 : 32), FA = "((" + F + ")" + vr(rn) + ")", FD = "((" + F + ")" + vr(rd) + ")";
        const std::string y = fmt("((%s){} + ((%s)%s)[%u])", F.c_str(), F.c_str(), vr(rm).c_str(), index);
        std::string r;
        if (opcode == 1) r = "__builtin_elementwise_fma(" + FA + ", " + y + ", " + FD + ")";
        else if (opcode == 5) r = "__builtin_elementwise_fma(-" + FA + ", " + y + ", " + FD + ")";
        else r = FA + " * " + y;
        wq(rd, r, bytes);
        return true;
    }
    unsigned eb;
    if (size == 1) {
        index = (h << 2) | (l << 1) | mbit;
        eb = 16;
    } else if (size == 2) {
        index = (h << 1) | l;
        rm |= mbit << 4;
        eb = 32;
    } else {
        return false;
    }
    if (opcode == 0x2 || opcode == 0x6 || opcode == 0xa) {  // SMLAL/UMLAL, SMLSL/UMLSL, SMULL/UMULL (+2)
        const unsigned n = 64 / eb, base = q ? n : 0, wb = 2 * eb;
        const std::string N = u ? ut(eb) : st(eb), W = u ? ut(wb) : st(wb);
        const std::string x = "__builtin_convertvector(" + shuffle(N.c_str(), vr(rn), vr(rn), iota(base, n)) + ", " + W + ")";
        const std::string y = fmt("((%s){} + (%s)((%s)%s)[%u])", W.c_str(), et(wb, !u), N.c_str(), vr(rm).c_str(), index);
        const std::string acc = "((" + W + ")" + vr(rd) + ")";
        std::string r;
        if (opcode == 0x2) r = acc + " + " + x + " * " + y;
        else if (opcode == 0x6) r = acc + " - " + x + " * " + y;
        else r = x + " * " + y;
        wq(rd, r);
        return true;
    }
    const std::string U = ut(eb), UA = "((" + U + ")" + vr(rn) + ")", UD = "((" + U + ")" + vr(rd) + ")";
    const std::string y = fmt("((%s){} + ((%s)%s)[%u])", U.c_str(), U.c_str(), vr(rm).c_str(), index);
    std::string r;
    switch (opcode) {
        case 0x0: if (!u) return false; r = UD + " + " + UA + " * " + y; break;  // MLA
        case 0x4: if (!u) return false; r = UD + " - " + UA + " * " + y; break;  // MLS
        case 0x8: if (u) return false; r = UA + " * " + y; break;                // MUL
        default: return false;
    }
    wq(rd, r, bytes);
    return true;
}

bool Emitter::s_three_same(uint32_t insn) {  // scalar, 64-bit integer forms only
    const bool u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 15, 11);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (size != 3 || opcode >= 0x18) return false;
    const std::string a = vl(rn), b = vl(rm);
    std::string r;
    switch (opcode) {
        case 0x10: r = u ? a + " - " + b : a + " + " + b; break;
        case 0x11: r = u ? "(" + a + " == " + b + " ? ~0ull : 0)" : "((" + a + " & " + b + ") ? ~0ull : 0)"; break;
        case 0x06: r = u ? "(" + a + " > " + b + " ? ~0ull : 0)" : "((int64_t)" + a + " > (int64_t)" + b + " ? ~0ull : 0)"; break;
        case 0x07: r = u ? "(" + a + " >= " + b + " ? ~0ull : 0)" : "((int64_t)" + a + " >= (int64_t)" + b + " ? ~0ull : 0)"; break;
        default: return false;
    }
    wv(rd, r, "0");
    return true;
}

bool Emitter::s_pairwise(uint32_t insn) {
    const bool u = bit(insn, 29);
    const unsigned size = field(insn, 23, 22), opcode = field(insn, 16, 12);
    const unsigned rn = field(insn, 9, 5), rd = field(insn, 4, 0);
    if (!u && opcode == 0x1b && size == 3) {  // ADDP Dd, Vn.2D
        wv(rd, vl(rn) + " + " + vh(rn), "0");
        return true;
    }
    if (!u || opcode != 0x0d || (size & 2)) return false;  // FADDP only
    if (size & 1) wv(rd, "b64(f64(" + vl(rn) + ") + f64(" + vh(rn) + "))", "0");
    else wv(rd, "b32(f32(" + vl(rn) + ") + f32(" + vl(rn) + " >> 32))", "0");
    return true;
}

// LD1/ST1 (1-4 registers), LD2/ST2, and LD1/ST1/LD1R of one lane.
bool Emitter::structures(uint32_t insn) {
    const bool q = bit(insn, 30), l = bit(insn, 22), post = bit(insn, 23);
    const unsigned rm = field(insn, 20, 16), rn = field(insn, 9, 5), rt = field(insn, 4, 0);
    const unsigned bytes = q ? 16 : 8;
    unsigned total;
    line("{ const uint64_t base = " + rxsp(rn) + ";");
    if (!bit(insn, 24)) {  // multiple structures
        const unsigned opcode = field(insn, 15, 12), size = field(insn, 11, 10);
        unsigned regs, selem;
        switch (opcode) {
            case 0x2: regs = 4; selem = 1; break;
            case 0x6: regs = 3; selem = 1; break;
            case 0x7: regs = 1; selem = 1; break;
            case 0xa: regs = 2; selem = 1; break;
            case 0x8: regs = 2; selem = 2; break;
            default: body_.resize(body_.rfind("    {")); return false;
        }
        if (size == 3 && !q && selem != 1) {
            body_.resize(body_.rfind("    {"));
            return false;
        }
        const unsigned eb = 8u << size, lanes = 128 / eb, n = bytes * 8 / eb;
        auto ld = [&](unsigned off) {
            return q ? fmt("(v2u64){LD64(base + %u), LD64(base + %u)}", off, off + 8)
                     : fmt("(v2u64){LD64(base + %u), 0}", off);
        };
        if (selem == 1) {
            for (unsigned i = 0; i < regs; ++i) {
                const unsigned t = (rt + i) % 32;
                if (l) {
                    wq(t, ld(i * bytes));
                } else if (q) {
                    line(fmt("  ST64(base + %u, %s); ST64(base + %u, %s);", i * 16, vl(t).c_str(), i * 16 + 8,
                             vh(t).c_str()));
                } else {
                    line(fmt("  ST64(base + %u, %s);", i * 8, vl(t).c_str()));
                }
            }
        } else {  // LD2/ST2: elements alternate between the two registers
            const unsigned t0 = rt, t1 = (rt + 1) % 32;
            if (l) {
                if (q) {
                    line("  const v2u64 m0 = " + ld(0) + ", m1 = " + ld(16) + ";");
                    wq(t0, shuffle(ut(eb), "m0", "m1", iota(0, lanes, 2)));
                    wq(t1, shuffle(ut(eb), "m0", "m1", iota(1, lanes, 2)));
                } else {
                    line("  const v2u64 m0 = (v2u64){LD64(base), LD64(base + 8)};");
                    wq(t0, shuffle(ut(eb), "m0", "m0", pad(iota(0, n, 2), lanes)), 8);
                    wq(t1, shuffle(ut(eb), "m0", "m0", pad(iota(1, n, 2), lanes)), 8);
                }
            } else {
                std::vector<unsigned> zip1, zip2;
                for (unsigned i = 0; i < lanes; ++i) {
                    zip1.push_back((i & 1) ? lanes + i / 2 : i / 2);
                    zip2.push_back((i & 1) ? lanes + lanes / 2 + i / 2 : lanes / 2 + i / 2);
                }
                line("  const v2u64 z0 = (v2u64)" + shuffle(ut(eb), vr(t0), vr(t1), zip1) + ";");
                line("  ST64(base, z0[0]); ST64(base + 8, z0[1]);");
                if (q) {
                    line("  const v2u64 z1 = (v2u64)" + shuffle(ut(eb), vr(t0), vr(t1), zip2) + ";");
                    line("  ST64(base + 16, z1[0]); ST64(base + 24, z1[1]);");
                }
            }
        }
        total = regs * bytes;
    } else {  // single structure
        const unsigned opcode = field(insn, 15, 13), size = field(insn, 11, 10);
        const bool s = bit(insn, 12), r = bit(insn, 21);
        const unsigned selem = (((opcode & 1) << 1) | r) + 1;
        unsigned scale = opcode >> 1, index = 0;
        bool replicate = false, ok = selem == 1;
        switch (scale) {
            case 0: index = (q << 3) | (s << 2) | size; break;
            case 1: ok = ok && !(size & 1); index = (q << 2) | (s << 1) | (size >> 1); break;
            case 2:
                if (size & 2) ok = false;
                else if (size == 0) index = (q << 1) | s;
                else { ok = ok && !s; index = q; scale = 3; }
                break;
            default: ok = ok && l && !s; scale = size; replicate = true; break;
        }
        if (!ok) {
            body_.resize(body_.rfind("    {"));
            return false;
        }
        const unsigned eb = 8u << scale;
        static const char* kLd[] = {"LD8", "LD16", "LD32", "LD64"};
        static const char* kSt[] = {"ST8", "ST16", "ST32", "ST64"};
        const std::string U = ut(eb);
        if (replicate) {
            wq(rt, "(" + U + "){} + (" + et(eb, false) + ")" + kLd[scale] + "(base)", bytes);
        } else if (l) {
            line("  " + U + " t = (" + U + ")" + vr(rt) + "; t[" + std::to_string(index) + "] = " + kLd[scale] + "(base);");
            wq(rt, "t");
        } else {
            line(fmt("  %s(base, ((%s)%s)[%u]);", kSt[scale], U.c_str(), vr(rt).c_str(), index));
        }
        total = eb / 8;
    }
    if (post) line("  " + reg(rn) + " = base + " + (rm == 31 ? hex(total) : rx(rm)) + ";");
    line("}");
    return true;
}

std::string Emitter::function(size_t* interpreted) {
    for (uint64_t va : f_.insns) {
        if (f_.labels.count(va)) body_ += fmt("L_%" PRIx64 ":;\n", va);
        const uint32_t insn = img_.insn(va);
        bool done = false;
        uint64_t target;
        if (classify(insn, va, &target) != Flow::kSeq) {
            done = branch(va, insn);
        } else {
            switch (field(insn, 28, 25)) {
                case 0x8: case 0x9: done = dp_immediate(insn, va); break;
                case 0xa: case 0xb: done = ((insn & 0xffc00000) == 0xd5000000) && system(insn); break;
                case 0x4: case 0x6: case 0xc: case 0xe: done = loads_stores(insn, va); break;
                case 0x5: case 0xd: done = dp_register(insn); break;
                case 0x7: case 0xf: done = simd(insn); break;
                default: break;
            }
        }
        if (!done) interpret(va);
        if (f_.falls_out.count(va)) exit_to(va + 4);
    }
    *interpreted = interpreted_;

    // Declarations and the write-back macros, now that the body says what it uses.
    std::string out;
    const std::string name = fmt("F_%" PRIx64, f_.entry);
    std::string flush, reload_all, reload_call;
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_x_ >> n & 1)) continue;
        const std::string r = n == 31 ? "SP" : fmt("X%u", n), slot = n == 31 ? "c->sp" : fmt("c->x[%u]", n);
        if (written_x_ >> n & 1) flush += " " + slot + " = " + r + ";";
        reload_all += " " + r + " = " + slot + ";";
        if (n < 8) reload_call += " " + r + " = " + slot + ";";
    }
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_v_ >> n & 1)) continue;
        if (written_v_ >> n & 1) flush += fmt(" *(v2u64*)c->v[%u] = V%u;", n, n);
        reload_all += fmt(" V%u = *(v2u64*)c->v[%u];", n, n);
        if (n < 8) reload_call += fmt(" V%u = *(v2u64*)c->v[%u];", n, n);
    }
    const bool has_flags = flags_used_ || flags_written_;
    std::string flush_all = flush;
    if (flags_written_) {
        flush_all += " c->nzcv = ((uint32_t)FN << 31) | ((uint32_t)FZ << 30) | ((uint32_t)FC << 29) | "
                     "((uint32_t)FV << 28);";
    }
    if (has_flags) {
        reload_all += " FN = (c->nzcv >> 31) & 1; FZ = (c->nzcv >> 30) & 1; FC = (c->nzcv >> 29) & 1;"
                      " FV = (c->nzcv >> 28) & 1;";
    }
    out += "#define FLUSH() do {" + flush + " } while (0)\n";
    out += "#define FLUSH_ALL() do {" + flush_all + " } while (0)\n";
    out += "#define RELOAD_ALL() do {" + reload_all + " } while (0)\n";
    out += "#define RELOAD_CALL() do {" + reload_call + " } while (0)\n";
    // A huge function costs clang far more than its size (time grows faster
    // than the number of blocks): one of 10K+ instructions in
    // libjuicebox_sdk_jni.so took 200 s at -O1, 12.6 s unoptimized. So, as
    // other static recompilers do, those few are not optimized; they still
    // run natively, with the guest registers in stack slots, as the JIT
    // keeps them in memory.
    if (f_.insns.size() > g_optnone_insns) out += "__attribute__((optnone, noinline)) ";
    out += "uint32_t " + name + "(Cpu* restrict c) {\n";
    out += "    const uint64_t B = aot_base;\n";
    for (unsigned n = 0; n < 32; ++n) {
        if (!(used_x_ >> n & 1)) continue;
        out += n == 31 ? "    uint64_t SP = c->sp;\n" : fmt("    uint64_t X%u = c->x[%u];\n", n, n);
    }
    for (unsigned n = 0; n < 32; ++n) {
        if (used_v_ >> n & 1) out += fmt("    v2u64 V%u = *(v2u64*)c->v[%u];\n", n, n);
    }
    if (has_flags) {
        out += "    uint8_t FN = (c->nzcv >> 31) & 1, FZ = (c->nzcv >> 30) & 1, FC = (c->nzcv >> 29) & 1,"
               " FV = (c->nzcv >> 28) & 1;\n";
    }
    out += fmt("    goto L_%" PRIx64 ";\n", f_.entry);
    out += body_;
    out += "}\n#undef FLUSH\n#undef FLUSH_ALL\n#undef RELOAD_ALL\n#undef RELOAD_CALL\n\n";
    return out;
}

// A profile's addresses (library vaddrs), when it was written for this build
// of the library; see arm64_aot.h, "Profiles".
bool read_profile(const char* path, uint64_t text_hash, std::set<uint64_t>* hits) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (!in || !std::getline(in, line)) return false;
    unsigned long long hash = 0;
    if (std::sscanf(line.c_str(), "winapk-aot-profile 1 %llx", &hash) != 1 || hash != text_hash) return false;
    while (std::getline(in, line)) {
        if (!line.empty()) hits->insert(std::strtoull(line.c_str(), nullptr, 16));
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* usage = "usage: winapk-aot <lib.so> <out.c> [parts] [--profile <file>] [--full-below <insns>]\n"
                        "       winapk-aot --hash <lib.so>   (prints the text hash a translation is keyed by)\n";
    if (argc == 3 && std::string(argv[1]) == "--hash") {
        Image img;
        std::string error;
        if (!read_image(argv[2], &img, &error)) {
            std::fprintf(stderr, "winapk-aot: %s: %s\n", argv[2], error.c_str());
            return 1;
        }
        std::printf("%016" PRIx64 "\n", img.text_hash);
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "%s", usage);
        return 2;
    }
    size_t parts = 1, full_below = 0;
    const char* profile = nullptr;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--profile" && i + 1 < argc) profile = argv[++i];
        else if (a == "--full-below" && i + 1 < argc) full_below = std::strtoull(argv[++i], nullptr, 10);
        else if (i == 3 && std::isdigit(static_cast<unsigned char>(a[0]))) parts = std::max(1, std::atoi(argv[i]));
        else {
            std::fprintf(stderr, "%s", usage);
            return 2;
        }
    }
    if (const char* t = std::getenv("WINAPK_AOT_OPTNONE_INSNS")) g_optnone_insns = std::strtoull(t, nullptr, 10);
    Image img;
    std::string error;
    if (!read_image(argv[1], &img, &error)) {
        std::fprintf(stderr, "winapk-aot: %s: %s\n", argv[1], error.c_str());
        return 1;
    }

    // Every function: the symbols, then the targets of BL, transitively.
    std::set<uint64_t> entries;
    for (const auto& s : img.symbols) entries.insert(s.first);
    std::map<uint64_t, Function> functions;
    std::vector<uint64_t> work(entries.begin(), entries.end());
    size_t skipped = 0;
    while (!work.empty()) {
        const uint64_t entry = work.back();
        work.pop_back();
        if (functions.count(entry)) continue;
        Function f;
        f.entry = entry;
        auto it = img.symbols.find(entry);
        if (it != img.symbols.end() && it->second) f.end = entry + it->second;
        std::set<uint64_t> calls;
        if (!discover(img, entries, &f, &calls)) {
            ++skipped;
            continue;
        }
        functions.emplace(entry, std::move(f));
        for (uint64_t t : calls) {
            if (entries.insert(t).second) work.push_back(t);
        }
    }
    size_t total = 0;
    for (const auto& f : functions) total += f.second.insns.size();

    // A large library with a profile: only the functions code ran in. The
    // rest stays with the JIT, as untranslated code always does.
    const bool by_profile = profile && total > full_below;
    if (by_profile) {
        std::set<uint64_t> hits;
        if (!read_profile(profile, img.text_hash, &hits)) hits.clear();
        const size_t all = functions.size();
        for (auto it = functions.begin(); it != functions.end();) {
            const std::set<uint64_t>& in = it->second.insns;
            auto h = hits.lower_bound(in.empty() ? 0 : *in.begin());
            bool ran = false;
            for (; h != hits.end() && !in.empty() && *h <= *in.rbegin(); ++h) {
                if (in.count(*h)) {
                    ran = true;
                    break;
                }
            }
            it = ran ? std::next(it) : functions.erase(it);
        }
        total = 0;
        for (const auto& f : functions) total += f.second.insns.size();
        std::fprintf(stderr, "winapk-aot: %s: profile: %zu of %zu functions ran\n", argv[1], functions.size(), all);
        if (functions.empty()) return 3;
    }
    std::set<uint64_t> translated;
    for (const auto& f : functions) translated.insert(f.first);

    // The C: with `parts` > 1, out.c holds the module table and
    // out.c.1.c .. out.c.<parts>.c the functions, balanced by instruction
    // count, for compiling in parallel. A part under ~20000 instructions
    // costs clang more in fixed overhead than it saves.
    parts = std::max<size_t>(1, std::min(parts, (total + 19999) / 20000));
    std::string protos;
    for (uint64_t e : translated) protos += fmt("uint32_t F_%" PRIx64 "(Cpu* restrict c);\n", e);
    std::vector<std::string> bodies(parts);
    // Largest functions first, each to the part with the fewest instructions
    // so far: a few huge functions (clang's time grows faster than their
    // size) land in different parts instead of holding one up.
    std::vector<const Function*> order;
    for (const auto& f : functions) order.push_back(&f.second);
    std::stable_sort(order.begin(), order.end(),
                     [](const Function* a, const Function* b) { return a->insns.size() > b->insns.size(); });
    std::vector<size_t> load(parts, 0);
    size_t insns = 0, interpreted = 0;
    for (const Function* f : order) {
        Emitter em(img, *f, translated);
        size_t n = 0;
        const size_t part = static_cast<size_t>(std::min_element(load.begin(), load.end()) - load.begin());
        bodies[part] += em.function(&n);
        load[part] += f->insns.size();
        insns += f->insns.size();
        interpreted += n;
    }
    std::string table = "uint64_t aot_base;\nAotRuntime aot_rt;\n\nstatic const AotEntry entries[] = {\n";
    for (uint64_t e : translated) table += fmt("    {0x%" PRIx64 "ull, F_%" PRIx64 "},\n", e, e);
    table += "};\n\n";
    table += "__declspec(dllexport) const AotModule winapk_aot_module = {\n";
    table += fmt("    %u, {offsetof(Cpu, sp), offsetof(Cpu, nzcv), offsetof(Cpu, tpidr), offsetof(Cpu, v)},\n",
                 winapk::arm64::kAotVersion);
    table += fmt("    0x%" PRIx64 "ull, &aot_base, &aot_rt, entries, sizeof entries / sizeof entries[0],\n",
                 img.text_hash);
    table += "};\n";

    auto write = [&](const std::string& path, const std::string& text) {
        std::ofstream o(path, std::ios::binary);
        o << text;
        if (!o) std::fprintf(stderr, "winapk-aot: cannot write %s\n", path.c_str());
        return static_cast<bool>(o);
    };
    const std::string head = std::string(kPrelude) + protos + "\n";
    if (parts == 1) {
        if (!write(argv[2], head + bodies[0] + table)) return 1;
    } else {
        if (!write(argv[2], head + table)) return 1;
        for (size_t i = 0; i < parts; ++i) {
            if (!write(fmt("%s.%zu.c", argv[2], i + 1), head + bodies[i])) return 1;
        }
    }
    size_t largest = 0, unoptimized = 0;
    for (const Function* f : order) {
        largest = std::max(largest, f->insns.size());
        if (f->insns.size() > g_optnone_insns) ++unoptimized;
    }
    std::fprintf(stderr,
                 "winapk-aot: %s: %zu functions, %zu instructions, %zu through the interpreter (%.1f%%), "
                 "largest function %zu instructions, %zu unoptimized%s\n",
                 argv[1], functions.size(), insns, interpreted,
                 insns ? 100.0 * static_cast<double>(interpreted) / static_cast<double>(insns) : 0.0, largest,
                 unoptimized, skipped ? fmt(", %zu too large to translate", skipped).c_str() : "");
    return by_profile ? 4 : 0;
}
