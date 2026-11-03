// arm64_bench.c -- the arm64 engines' benchmarks (winapk-arm64-test).
//
// Like arm64_isa.c, the same file is compiled for aarch64-linux-android (the
// guest library) and into the test itself (the native reference). Each
// benchmark stresses one cost of running translated code, so a change to the
// JIT or the bridge shows up where it belongs:
//
//   compute    integer arithmetic, loads and stores in one tight loop
//   float      floating point and auto-vectorized NEON loops
//   indirect   calls through a function-pointer table (indirect branches)
//   memory     a pointer chase over 4 MiB (cache misses; should cost the same)
//   libc       small memcpy/strlen calls into the host (guest -> host)
//   callback   qsort with a guest comparator (host -> guest)
//
// Every benchmark is u64 f(u64 iterations, u64 seed, u64 unused) and returns
// a checksum, which must match the native one.

#include <stdint.h>

#pragma STDC FP_CONTRACT OFF

#ifndef BENCH_EXPORT
#define BENCH_EXPORT __attribute__((visibility("default"), noinline))
#endif

#ifdef BENCH_HOST
// The test includes <cstdlib> and <cstring> first.
#else
typedef unsigned long size_t;
void* malloc(size_t);
void free(void*);
void* memcpy(void*, const void*, size_t);
size_t strlen(const char*);
void qsort(void*, size_t, size_t, int (*)(const void*, const void*));
#endif

typedef uint64_t u64;
typedef uint32_t u32;

static u64 lcg(u64 h) { return h * 6364136223846793005ull + 1442695040888963407ull; }

BENCH_EXPORT u64 bench_compute(u64 n, u64 seed, u64 c) {
    u32 table[64];
    for (int i = 0; i < 64; ++i) table[i] = (u32)(seed * (u64)(i + 1) + c);
    u64 h = seed;
    for (u64 i = 0; i < n; ++i) {
        h = lcg(h);
        table[i & 63] ^= (u32)(h >> 33);
        h ^= table[(i * 7) & 63];
    }
    return h;
}

#define FLOAT_N 1024

BENCH_EXPORT u64 bench_float(u64 n, u64 seed, u64 c) {
    (void)c;
    static float a[FLOAT_N], b[FLOAT_N];
    for (int i = 0; i < FLOAT_N; ++i) {
        a[i] = (float)((seed + (u64)i) % 97) * 0.25f;
        b[i] = (float)((seed * 3 + (u64)i) % 89) * 0.125f;
    }
    double total = 0;
    for (u64 r = 0; r < n; ++r) {
        const float k = (float)(r % 7) * 0.5f - 1.5f;
        for (int i = 0; i < FLOAT_N; ++i) b[i] = b[i] * 0.5f + a[i] * k;  // vectorized
        float dot = 0;
        for (int i = 0; i < FLOAT_N; ++i) dot += a[i] * b[i];  // in order: not vectorized without -ffast-math
        total += dot;
    }
    union {
        double d;
        u64 u;
    } bits = {total};
    return bits.u;
}

static __attribute__((noinline)) u64 op0(u64 h) { return h + 0x9e3779b97f4a7c15ull; }
static __attribute__((noinline)) u64 op1(u64 h) { return h ^ (h >> 29); }
static __attribute__((noinline)) u64 op2(u64 h) { return h * 0xbf58476d1ce4e5b9ull; }
static __attribute__((noinline)) u64 op3(u64 h) { return (h << 13) | (h >> 51); }
static __attribute__((noinline)) u64 op4(u64 h) { return h - (h >> 7); }
static __attribute__((noinline)) u64 op5(u64 h) { return h ^ 0x94d049bb133111ebull; }
static __attribute__((noinline)) u64 op6(u64 h) { return h + (h << 3); }
static __attribute__((noinline)) u64 op7(u64 h) { return ~h; }

BENCH_EXPORT u64 bench_indirect(u64 n, u64 seed, u64 c) {
    (void)c;
    static u64 (*const ops[8])(u64) = {op0, op1, op2, op3, op4, op5, op6, op7};
    u64 h = seed;
    for (u64 i = 0; i < n; ++i) h = ops[(h >> 17) & 7](h) + i;
    return h;
}

#define CHASE_N (1u << 20)  // 4 MiB of u32

BENCH_EXPORT u64 bench_memory(u64 n, u64 seed, u64 c) {
    (void)c;
    u32* next = (u32*)malloc(sizeof(u32) * CHASE_N);
    if (!next) return 0;
    // One cycle through every slot, in a shuffled order (Sattolo's algorithm).
    for (u32 i = 0; i < CHASE_N; ++i) next[i] = i;
    u64 h = seed;
    for (u32 i = CHASE_N - 1; i > 0; --i) {
        h = lcg(h);
        const u32 j = (u32)((h >> 33) % i);
        const u32 t = next[i];
        next[i] = next[j];
        next[j] = t;
    }
    u32 p = 0;
    u64 sum = 0;
    for (u64 i = 0; i < n; ++i) {
        p = next[p];
        sum += p;
    }
    free(next);
    return sum;
}

BENCH_EXPORT u64 bench_libc(u64 n, u64 seed, u64 c) {
    (void)c;
    char src[160], dst[160];
    for (int i = 0; i < 159; ++i) src[i] = (char)('a' + (seed + (u64)i) % 26);
    src[159] = 0;
    u64 h = seed;
    for (u64 i = 0; i < n; ++i) {
        const size_t len = 16 + (size_t)(h % 128);
        memcpy(dst, src + (i & 15), len);
        dst[len] = 0;
        h = lcg(h) + strlen(dst) + (unsigned char)dst[i % len];
    }
    return h;
}

static int compare_u32(const void* a, const void* b) {
    const u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

#define SORT_N 512

BENCH_EXPORT u64 bench_callback(u64 n, u64 seed, u64 c) {
    (void)c;
    u32 v[SORT_N];
    u64 h = seed, sum = 0;
    for (u64 r = 0; r < n; ++r) {
        for (int i = 0; i < SORT_N; ++i) {
            h = lcg(h);
            v[i] = (u32)(h >> 40);
        }
        qsort(v, SORT_N, sizeof v[0], compare_u32);
        sum = sum * 31 + v[0] + v[SORT_N / 2] + v[SORT_N - 1];
    }
    return sum;
}

// name, function, iterations (sized for a few ms natively).
#define BENCHES(X)                       \
    X(bench_compute, 3000000)            \
    X(bench_float, 2000)                 \
    X(bench_indirect, 3000000)           \
    X(bench_memory, 2000000)             \
    X(bench_libc, 1000000)               \
    X(bench_callback, 300)
