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

typedef struct GuestFunc {
    uint32_t addr;
    GuestFn fn;
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

#define FCOM(a, b) (fp_sw = rt_fcom(fp_sw, (a), (b)))

static inline uint32_t rt_fstsw(uint32_t sw, uint32_t top) { return (sw & ~0x3800u) | ((top & 7u) << 11); }

uint32_t rt_fxam(uint32_t sw, double v);

/* Define IGI_X87_CHECK to verify the statically derived x87 stack effect of every
 * call whose effect the generator considers proven. */
#ifdef IGI_X87_CHECK
void rt_x87_mismatch(Ctx *c, uint32_t addr, uint32_t expect);
#define X87_CHECK(expect, addr) do { if (c->ftop != (expect)) rt_x87_mismatch(c, (addr), (expect)); } while (0)
#else
#define X87_CHECK(expect, addr) ((void)0)
#endif

/* Round according to the rounding-control field of the control word. */
static inline double rt_fround(Ctx *c, double v)
{
    switch ((c->fcw >> 10) & 3u) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: return nearbyint(v);
    }
}

/* fist fast path: |v| < 2^31 - 1 rounds exactly with a truncating conversion (one
 * VCVT on ARM) and the exact fraction v - trunc(v), instead of libm rounding calls
 * and, for 64-bit results, a libgcc conversion. */
#define RT_FIST_FAST(v) ((v) > -2147483647.0 && (v) < 2147483647.0)

static inline int32_t rt_fround_small(const Ctx *c, double v)
{
    int32_t t = (int32_t)v;
    double f = v - (double)t;
    switch ((c->fcw >> 10) & 3u) {
    case 1: return t - (f < 0.0);
    case 2: return t + (f > 0.0);
    case 3: return t;
    default:
        if (f > 0.5 || (f == 0.5 && (t & 1)))
            return t + 1;
        if (f < -0.5 || (f == -0.5 && (t & 1)))
            return t - 1;
        return t;
    }
}

static inline int64_t rt_fist64(Ctx *c, double v)
{
    if (RT_FIST_FAST(v))
        return rt_fround_small(c, v);
    double r = rt_fround(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)r;
}

static inline int32_t rt_fist32(Ctx *c, double v)
{
    if (RT_FIST_FAST(v))
        return rt_fround_small(c, v);
    double r = rt_fround(c, v);
    if (!(r >= -2147483648.0 && r < 2147483648.0))
        return INT32_MIN;
    return (int32_t)r;
}

static inline int16_t rt_fist16(Ctx *c, double v)
{
    if (RT_FIST_FAST(v)) {
        int32_t r = rt_fround_small(c, v);
        return r >= -32768 && r < 32768 ? (int16_t)r : INT16_MIN;
    }
    double r = rt_fround(c, v);
    if (!(r >= -32768.0 && r < 32768.0))
        return INT16_MIN;
    return (int16_t)r;
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
void rt_rep_movs(Ctx *c, int size);
void rt_rep_stos(Ctx *c, int size);

/* ---- control transfer -------------------------------------------------- */

/* Host jump buffer backing the guest setjmp buffer at `guest_buf`. */
jmp_buf *rt_jmpbuf(uint32_t guest_buf);
/* Resume the matching host setjmp; guest registers are already in the context. */
void rt_longjmp(Ctx *c, uint32_t guest_buf) __attribute__((noreturn));


/* Call the guest code at `target` (a recompiled function or a host thunk). */
void rt_dispatch(Ctx *c, uint32_t target);

void rt_unimplemented(Ctx *c, uint32_t addr, const char *what);
/* Diagnostic hook inserted at the entry of functions listed in IGI_PROBES. */
void rt_probe(Ctx *c, uint32_t func);
void rt_divide_error(Ctx *c, uint32_t addr);
void rt_noreturn(Ctx *c, uint32_t addr);

#endif
