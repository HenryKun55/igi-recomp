/*
 * Guest CPU context and helpers used by the recompiled IGI.exe code.
 *
 * Guest memory is a flat byte array: guest address A lives at g_mem + A.
 * All accessors are unaligned-safe (memcpy) so the same code runs on arm64
 * and on the Vita's ARMv7.
 */
#ifndef IGI_RECOMP_H
#define IGI_RECOMP_H

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

typedef struct Ctx {
    uint32_t r[8];          /* eax ecx edx ebx esp ebp esi edi */
    uint32_t df;            /* direction flag */
    uint32_t eflags_extra;  /* ID/AC bits round-tripped through pushfd/popfd */
    uint32_t fs_base;       /* guest address of the TIB */
    double st[8];           /* x87 register file, physical order */
    uint32_t ftop;
    uint32_t fcw;
    uint32_t fsw;           /* condition codes C0-C3 and exception bits; TOP is composed on read */
} Ctx;

typedef void (*GuestFn)(Ctx *c);

/* Calling convention between generated functions: eax, ecx and esp in registers,
 * eax and esp returned (low and high half); everything else through the context. */
typedef uint64_t (*GuestFastFn)(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax);

typedef struct GuestFunc {
    uint32_t addr;
    GuestFn fn;             /* context-only entry (wrapper) */
    GuestFastFn fast;       /* generated function */
} GuestFunc;

typedef struct GuestImport {
    uint32_t slot;          /* IAT slot address */
    const char *dll;
    const char *name;
    GuestFn fn;
} GuestImport;

extern uint8_t *g_mem;

extern const GuestFunc g_guest_funcs[];
extern const uint32_t g_guest_func_count;
/* Hash table over g_guest_funcs for rt_dispatch (see lookup_func in cpu.c). */
extern const GuestFunc g_guest_func_hash[];
extern const uint32_t g_guest_func_hash_mask;
#define GUEST_FUNC_HASH(a) (((a) * 0x9E3779B1u) >> 16)
extern const GuestImport g_guest_imports[];
extern const uint32_t g_guest_import_count;

/* ---- memory ----------------------------------------------------------- */

static inline uint32_t RD8(uint32_t a) { return g_mem[a]; }
static inline uint32_t RD16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, g_mem + a, 8); return v; }
static inline void WR8(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)v; }
static inline void WR16(uint32_t a, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(g_mem + a, &t, 2); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(g_mem + a, &v, 8); }
static inline double RDF32(uint32_t a) { float v; memcpy(&v, g_mem + a, 4); return (double)v; }
static inline double RDF64(uint32_t a) { double v; memcpy(&v, g_mem + a, 8); return v; }
static inline void WRF32(uint32_t a, double v) { float t = (float)v; memcpy(g_mem + a, &t, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(g_mem + a, &v, 8); }

/* Accessors used by the generated code. Every generated function copies g_mem into
 * the local `gm`: a store through a uint8_t pointer may alias any object, so a
 * global base would be reloaded after every guest store. */
static inline uint32_t rt_ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t rt_ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t rt_ld64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void rt_st16(uint8_t *p, uint32_t v) { uint16_t t = (uint16_t)v; memcpy(p, &t, 2); }
static inline void rt_st32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void rt_st64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static inline double rt_ldf32(const uint8_t *p) { float v; memcpy(&v, p, 4); return (double)v; }
static inline double rt_ldf64(const uint8_t *p) { double v; memcpy(&v, p, 8); return v; }
static inline void rt_stf32(uint8_t *p, double v) { float t = (float)v; memcpy(p, &t, 4); }
static inline void rt_stf64(uint8_t *p, double v) { memcpy(p, &v, 8); }

/* By host pointer (the generator folds small displacements into the pointer). */
#define PRD8(p) ((uint32_t)*(p))
#define PRD16(p) rt_ld16(p)
#define PRD32(p) rt_ld32(p)
#define PRD64(p) rt_ld64(p)
#define PWR8(p, v) (*(p) = (uint8_t)(v))
#define PWR16(p, v) rt_st16((p), (v))
#define PWR32(p, v) rt_st32((p), (v))
#define PWR64(p, v) rt_st64((p), (v))
#define PRDF32(p) rt_ldf32(p)
#define PRDF64(p) rt_ldf64(p)
#define PWRF32(p, v) rt_stf32((p), (v))
#define PWRF64(p, v) rt_stf64((p), (v))

/* By 32-bit guest address. */
#define GRD8(a) PRD8(gm + (uint32_t)(a))
#define GRD16(a) PRD16(gm + (uint32_t)(a))
#define GRD32(a) PRD32(gm + (uint32_t)(a))
#define GRD64(a) PRD64(gm + (uint32_t)(a))
#define GWR8(a, v) PWR8(gm + (uint32_t)(a), (v))
#define GWR16(a, v) PWR16(gm + (uint32_t)(a), (v))
#define GWR32(a, v) PWR32(gm + (uint32_t)(a), (v))
#define GWR64(a, v) PWR64(gm + (uint32_t)(a), (v))

/* Parity flag of the low byte (1 when the number of set bits is even). */
static inline int rt_parity(uint32_t v)
{
    v ^= v >> 4;
    return (int)((0x9669u >> (v & 0xFu)) & 1u);
}

/* ---- x87 --------------------------------------------------------------- */

#define ST(i) (c->st[(c->ftop + (uint32_t)(i)) & 7u])

static inline void fpush(Ctx *c, double v)
{
    c->ftop = (c->ftop - 1u) & 7u;
    c->st[c->ftop] = v;
}

static inline void fpop(Ctx *c) { c->ftop = (c->ftop + 1u) & 7u; }

#define FSW_C0 0x0100u
#define FSW_C1 0x0200u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u

/* Type of the x87 registers the generated code keeps in locals.
 *
 * IGI_X87_FLOAT (off by default) makes them single precision. On the Cortex-A9
 * that avoids the float<->double conversions on every load and store and makes
 * multiplies, divides and square roots cheaper, but it is not exact: results
 * differ from an x87 running with 53- or 64-bit precision control, and the
 * differential tester (tools/recomp/difftest.py) no longer passes. It is closer
 * to hardware only while the game runs with 24-bit precision control (Direct3D
 * sets that unless FPU_PRESERVE is used), and even then the exponent range and
 * double rounding differ. The context copy (Ctx.st) stays double either way. */
#ifdef IGI_X87_FLOAT
typedef float x87_t;
static inline x87_t rt_fsqrt(x87_t v) { return sqrtf(v); }
#else
typedef double x87_t;
static inline x87_t rt_fsqrt(x87_t v) { return sqrt(v); }
#endif

/* The generated code keeps the status word in the local `fp_sw` and, where the
 * stack depth is statically known, the x87 registers in locals as well (see
 * FuncGen.lower in tools/recomp/gen.py). The context copy is synchronised around
 * calls and returns. */
static inline uint32_t rt_fcom(uint32_t sw, double a, double b)
{
    uint32_t cc;
    if (a > b)
        cc = 0;
    else if (a < b)
        cc = FSW_C0;
    else if (a == b)
        cc = FSW_C3;
    else
        cc = FSW_C0 | FSW_C2 | FSW_C3;
    return (sw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)) | cc;
}

/* The operands are kept in fp_ca/fp_cb so that a following `fnstsw ax; test ah, K;
 * jcc` can branch on them directly (FuncGen.fcom_test). */
#define FCOM(a, b) (fp_ca = (a), fp_cb = (b), fp_sw = rt_fcom(fp_sw, fp_ca, fp_cb))
/* Lazy compare: only the operands are kept, for the tests that branch on them. */
#define FCOMX(a, b) (fp_ca = (a), fp_cb = (b))
/* Brings the condition codes up to date after FCOMX compares, before the status word is read. */
#define FSYNC (fp_sw = rt_fcom(fp_sw, fp_ca, fp_cb))
/* The same where only some paths leave the update pending (flag fp_lz). */
#define FCOML(a, b) (fp_ca = (a), fp_cb = (b), fp_lz = 1)
#define FCOME(a, b) (FCOM(a, b), fp_lz = 0)
#define FSYNCL do { if (fp_lz) { FSYNC; fp_lz = 0; } } while (0)

static inline uint32_t rt_fstsw(uint32_t sw, uint32_t top) { return (sw & ~0x3800u) | ((top & 7u) << 11); }

uint32_t rt_fxam(uint32_t sw, double v);

/* Define IGI_X87_CHECK to verify the statically derived x87 stack effect of every
 * call whose effect the generator considers proven. */
void rt_x87_mismatch(Ctx *c, uint32_t addr, uint32_t expect);
#ifdef IGI_X87_CHECK
#define X87_CHECK(expect, addr) do { if (c->ftop != (expect)) rt_x87_mismatch(c, (addr), (expect)); } while (0)
#else
#define X87_CHECK(expect, addr) ((void)0)
#endif

/* Round according to the rounding-control field of the control word. Round to
 * nearest is inline: below 2^52, adding and subtracting 2^52 to the magnitude
 * rounds to nearest-even in the host's default rounding mode (no libm call).
 * Truncation (the game draws its frames with control word 0xE3F) is inline too:
 * below 2^31 a conversion to int32 and back truncates, the sign keeps -0.0. */
static inline double rt_fround(const Ctx *c, double v)
{
    if (__builtin_expect((c->fcw & 0x0C00u) == 0, 1)) {
        double a = fabs(v);
        if (a < 4503599627370496.0)
            return copysign((a + 4503599627370496.0) - 4503599627370496.0, v);
        return v + 0.0;                         /* integral, infinite or NaN (quieted) */
    }
    uint32_t rc = (c->fcw >> 10) & 3u;
    if (rc == 3u && fabs(v) < 2147483648.0)
        return copysign((double)(int32_t)v, v);
    if (rc == 1)
        return floor(v);
    return rc == 2 ? ceil(v) : trunc(v);
}

/* fscale: ST(0) * 2^trunc(ST(1)). Multiplying by the power of two when it is a
 * normal double gives the same correctly rounded result as ldexp. */
static inline double rt_fscale(double v, double s)
{
    if (s > -1022.0 && s < 1024.0) {
        uint64_t b = (uint64_t)((int32_t)s + 1023) << 52;
        double p;
        memcpy(&p, &b, 8);
        return v * p;
    }
    return ldexp(v, (int)trunc(s));
}

/* fild m64: int64 -> double without the library conversion when the value fits
 * in 32 bits (both conversions are exact then). */
static inline double rt_fild64(uint64_t v)
{
    int32_t lo = (int32_t)(uint32_t)v;
    if ((int64_t)lo == (int64_t)v)
        return (double)lo;
    return (double)(int64_t)v;
}

/* fist: the common cases (round to nearest or truncate, result representable) are
 * inline. Adding 1.5 * 2^52 rounds to nearest-even in the host's default rounding
 * mode, and while the sum stays in [2^52, 2^53) its low mantissa bits are the
 * rounded integer in two's complement: no conversion instruction is needed. In
 * truncating mode a C conversion truncates. Everything else (other rounding modes,
 * out of range, NaN) goes out of line (the same results: see fround_small). */
#define RT_FIST_NEAREST(c) (((c)->fcw & 0x0C00u) == 0)
#define RT_FIST_CHOP(c) (((c)->fcw & 0x0C00u) == 0x0C00u)
#define RT_FIST_SMALL(v) ((v) > -2147483647.0 && (v) < 2147483647.0)
#define RT_ROUND_MAGIC 6755399441055744.0
#define RT_ROUND_MAGIC_BITS 0x4338000000000000ull

static inline uint64_t rt_round_bits(double v)
{
    double t = v + RT_ROUND_MAGIC;
    uint64_t b;
    memcpy(&b, &t, 8);
    return b;
}

int64_t rt_fist64_slow(const Ctx *c, double v);
int32_t rt_fist32_slow(const Ctx *c, double v);
int16_t rt_fist16_slow(const Ctx *c, double v);

static inline int64_t rt_fist64(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = rt_round_bits(v);
        if ((uint32_t)(b >> 52) == 0x433u)
            return (int64_t)(b - RT_ROUND_MAGIC_BITS);
    } else if (RT_FIST_CHOP(c) && RT_FIST_SMALL(v)) {
        return (int32_t)v;
    }
    return rt_fist64_slow(c, v);
}

/* In int32 range exactly when the high word of the difference is the sign
 * extension of the low word (which also implies the sum's exponent is 2^52). */
static inline int32_t rt_fist32(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = rt_round_bits(v);
        uint32_t lo = (uint32_t)b, hi = (uint32_t)(b >> 32) - 0x43380000u;
        if (hi == (uint32_t)((int32_t)lo >> 31))
            return (int32_t)lo;
    } else if (RT_FIST_CHOP(c) && RT_FIST_SMALL(v)) {
        return (int32_t)v;
    }
    return rt_fist32_slow(c, v);
}

static inline int16_t rt_fist16(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = rt_round_bits(v);
        uint32_t lo = (uint32_t)b, hi = (uint32_t)(b >> 32) - 0x43380000u;
        if (hi == (uint32_t)((int32_t)lo >> 31) && lo + 0x8000u < 0x10000u)
            return (int16_t)lo;
    } else if (RT_FIST_CHOP(c) && v > -32769.0 && v < 32768.0) {
        return (int16_t)(int32_t)v;
    }
    return rt_fist16_slow(c, v);
}

double rt_ld80(uint32_t a);
void rt_st80(uint32_t a, double v);
void rt_fninit(Ctx *c);
void rt_fnstenv(Ctx *c, uint32_t a);
void rt_fldenv(Ctx *c, uint32_t a);
void rt_fnsave(Ctx *c, uint32_t a);
void rt_frstor(Ctx *c, uint32_t a);

/* ---- integer helpers --------------------------------------------------- */

uint32_t rt_rcl(uint32_t d, uint32_t n, int bits, int *cf, int *of);
uint32_t rt_rcr(uint32_t d, uint32_t n, int bits, int *cf, int *of);
/* rep movs / rep stos on register values (no context round trip). Results are the
 * final ecx, esi and edi. The inline part handles the common forward cases. */
typedef struct RtStr {
    uint32_t cx, si, di;
} RtStr;

/* repne scasb forward (ecx != 0): final ecx and edi and the last byte compared. */
typedef struct RtScas {
    uint32_t ok, cx, di, last;
} RtScas;

RtScas rt_scasb_fwd(const uint8_t *gm, uint32_t n, uint32_t di, uint32_t al);

RtStr rt_rep_movs_slow(uint8_t *gm, uint32_t df, uint32_t n, uint32_t si, uint32_t di, uint32_t size);
RtStr rt_rep_stos_slow(uint8_t *gm, uint32_t df, uint32_t n, uint32_t di, uint32_t v, uint32_t size);

/* Copy of a small constant number of bytes (a multiple of 4, at most 64) that
 * reads the whole source before writing: the result equals an element-by-element
 * forward copy whenever the destination does not start inside the source. With
 * NEON, unaligned 16- and 8-byte vector loads and stores. */
static inline __attribute__((always_inline)) void rt_move_small(uint8_t *d, const uint8_t *s, uint32_t b)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint8x16_t q0 = vdupq_n_u8(0), q1 = q0, q2 = q0, q3 = q0;
    uint8x8_t h = vdup_n_u8(0);
    uint32_t w = 0;
    if (b >= 16u) q0 = vld1q_u8(s);
    if (b >= 32u) q1 = vld1q_u8(s + 16);
    if (b >= 48u) q2 = vld1q_u8(s + 32);
    if (b >= 64u) q3 = vld1q_u8(s + 48);
    if (b & 8u) h = vld1_u8(s + (b & ~15u));
    if (b & 4u) w = rt_ld32(s + (b & ~7u));
    if (b >= 16u) vst1q_u8(d, q0);
    if (b >= 32u) vst1q_u8(d + 16, q1);
    if (b >= 48u) vst1q_u8(d + 32, q2);
    if (b >= 64u) vst1q_u8(d + 48, q3);
    if (b & 8u) vst1_u8(d + (b & ~15u), h);
    if (b & 4u) rt_st32(d + (b & ~7u), w);
#else
    uint8_t t[64];
    memcpy(t, s, b);
    memcpy(d, t, b);
#endif
}

/* Store of a small constant number of copies of a 32-bit value (b as above). */
static inline __attribute__((always_inline)) void rt_fill_small(uint8_t *d, uint32_t v, uint32_t b)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint8x16_t q = vreinterpretq_u8_u32(vdupq_n_u32(v));
    for (uint32_t i = 0; i + 16u <= b; i += 16u)
        vst1q_u8(d + i, q);
    if (b & 8u)
        vst1_u8(d + (b & ~15u), vget_low_u8(q));
    if (b & 4u)
        rt_st32(d + (b & ~7u), v);
#else
    for (uint32_t i = 0; i < b; i += 4u)
        rt_st32(d + i, v);
#endif
}

static inline __attribute__((always_inline)) RtStr rt_rep_movs(uint8_t *gm, uint32_t df, uint32_t n, uint32_t si, uint32_t di, uint32_t size)
{
    uint32_t b = n * size;
    /* Backward, empty, or the destination overlaps ahead of the source (element
     * order is observable): out of line. Otherwise a forward copy equals memmove. */
    if (df || n == 0 || di - si < b)
        return rt_rep_movs_slow(gm, df, n, si, di, size);
    if (__builtin_constant_p(b) && b <= 64u && (b & 3u) == 0) {
        rt_move_small(gm + di, gm + si, b);
    } else if (size == 4 && n <= 8) {
        for (uint32_t i = 0; i < b; i += 4u)
            rt_st32(gm + di + i, rt_ld32(gm + si + i));
    } else {
        memmove(gm + di, gm + si, b);
    }
    RtStr r = { 0, si + b, di + b };
    return r;
}

static inline __attribute__((always_inline)) RtStr rt_rep_stos(uint8_t *gm, uint32_t df, uint32_t n, uint32_t di, uint32_t v, uint32_t size)
{
    if (df || n == 0 || size == 2)
        return rt_rep_stos_slow(gm, df, n, di, v, size);
    uint32_t b = n * size;
    if (size == 4 && __builtin_constant_p(b) && b <= 64u) {
        rt_fill_small(gm + di, v, b);
    } else if (size == 1 || v == 0) {
        memset(gm + di, (int)(v & 0xFFu), b);
    } else {
        for (uint32_t i = 0; i < b; i += 4u)
            rt_st32(gm + di + i, v);
    }
    RtStr r = { 0, 0, di + b };
    return r;
}

/* Native C runtime routines (runtime/src/crt_hle.c); 0: run the guest code. */
int rt_hle_stricmp(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);
int rt_hle_memset(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);

/* Hand-decompiled game functions (runtime/src/native_*.c, NATIVE table in gen.py),
 * same convention. g_igi_native = 0 (IGI_NATIVE=0) makes them all decline. */
extern int g_igi_native;
int rt_native_terrain_render(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);

/* ---- profiler ------------------------------------------------------------
 * With IGI_PROFILE, generated functions keep a shadow call stack and the id of
 * the code currently running (a guest function address, an import's IAT slot,
 * or a host thunk address) that a sampling thread reads (runtime/src/profile.c).
 * Without it every hook compiles to nothing. */
#ifdef IGI_PROFILE
#define RT_PROF_MAX 256u
extern volatile uint32_t rt_prof_cur;
extern volatile uint32_t rt_prof_depth;
extern volatile uint32_t rt_prof_stack[RT_PROF_MAX];
#define PROF_ENTER(a)                                                   \
    const uint32_t prof_pd = rt_prof_depth, prof_prev = rt_prof_cur;    \
    if (prof_pd < RT_PROF_MAX)                                          \
        rt_prof_stack[prof_pd] = (a);                                   \
    rt_prof_depth = prof_pd + 1u;                                       \
    rt_prof_cur = (a);                                                  \
    (void)prof_pd;                                                      \
    (void)prof_prev
#define PROF_EXIT (rt_prof_depth = prof_pd, rt_prof_cur = prof_prev)
#define PROF_HOST(id) (rt_prof_cur = (id))
#define PROF_BACK(a) (rt_prof_cur = (a))
/* A setjmp returning through longjmp: this function is current again. */
#define PROF_REENTER(a) (rt_prof_depth = prof_pd + 1u, rt_prof_cur = (a))
void rt_prof_start(void);
/* Drop the samples taken so far (e.g. boot and menus before a benchmark). */
void rt_prof_reset(void);
/* Stop sampling and write the profile now (for exits that skip atexit). */
void rt_prof_flush(void);
/* Host sections inside a thunk (ids 0xFD0000xx, named in profile.c). */
enum { PROF_H_DECODE = 0xFD000001u, PROF_H_NEARCLIP, PROF_H_PACK, PROF_H_BATCH, PROF_H_STATE, PROF_H_GLDRAW,
       PROF_H_TEXTURE, PROF_H_BLIT, PROF_H_MIX, PROF_H_INDEX, PROF_H_FILL, PROF_H_SOFTFILL,
       PROF_H_SOFTBLIT };
/* Phases inside vitaGL's draw calls (private vitaGL built with PROF_HOOKS=1). */
#define PROF_H_VGL 0xFD000020u
#define PROF_SECTION(id) const uint32_t prof_hprev = rt_prof_cur; rt_prof_cur = (id)
#define PROF_SECTION_END (rt_prof_cur = prof_hprev)
#else
#define PROF_SECTION(id) ((void)0)
#define PROF_SECTION_END ((void)0)
#define PROF_ENTER(a) ((void)0)
#define PROF_EXIT ((void)0)
#define PROF_HOST(id) ((void)0)
#define PROF_BACK(a) ((void)0)
#define PROF_REENTER(a) ((void)0)
#endif

/* ---- control transfer -------------------------------------------------- */

/* Host jump buffer backing the guest setjmp buffer at `guest_buf`. */
jmp_buf *rt_jmpbuf(uint32_t guest_buf);
/* Resume the matching host setjmp; guest registers are already in the context. */
void rt_longjmp(Ctx *c, uint32_t guest_buf) __attribute__((noreturn));


/* Call the guest code at `target` (a recompiled function or a host thunk). */
void rt_dispatch(Ctx *c, uint32_t target);
/* Indirect call or jump from generated code, with the GuestFastFn convention (the
 * caller has stored every other modified register in the context). */
uint64_t rt_call_indirect(Ctx *c, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t eax);
/* Same, also storing the target's g_guest_func_hash entry in the call site's cache
 * (a single pointer, so a concurrent reader always sees a consistent entry). */
uint64_t rt_call_indirect_ic(Ctx *c, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t eax,
                             const GuestFunc **ic);

void rt_unimplemented(Ctx *c, uint32_t addr, const char *what);
/* Diagnostic hook inserted at the entry of functions listed in IGI_PROBES. */
void rt_probe(Ctx *c, uint32_t func);
void rt_divide_error(Ctx *c, uint32_t addr);
void rt_noreturn(Ctx *c, uint32_t addr);

#endif
