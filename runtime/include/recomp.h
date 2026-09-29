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

static inline int rt_parity(uint32_t v) { return !__builtin_parity(v & 0xFFu); }

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

static inline void fcompare(Ctx *c, double a, double b)
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
    c->fsw = (c->fsw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)) | cc;
}

static inline uint32_t rt_fstsw(Ctx *c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7u) << 11); }

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

static inline int64_t rt_fist64(Ctx *c, double v)
{
    double r = rt_fround(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)r;
}

static inline int32_t rt_fist32(Ctx *c, double v)
{
    double r = rt_fround(c, v);
    if (!(r >= -2147483648.0 && r < 2147483648.0))
        return INT32_MIN;
    return (int32_t)r;
}

static inline int16_t rt_fist16(Ctx *c, double v)
{
    double r = rt_fround(c, v);
    if (!(r >= -32768.0 && r < 32768.0))
        return INT16_MIN;
    return (int16_t)r;
}

double rt_ld80(uint32_t a);
void rt_st80(uint32_t a, double v);
void rt_fxam(Ctx *c);
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

/* Call the guest code at `target` (a recompiled function or a host thunk). */
void rt_dispatch(Ctx *c, uint32_t target);

void rt_unimplemented(Ctx *c, uint32_t addr, const char *what);
/* Diagnostic hook inserted at the entry of functions listed in IGI_PROBES. */
void rt_probe(Ctx *c, uint32_t func);
void rt_divide_error(Ctx *c, uint32_t addr);
void rt_noreturn(Ctx *c, uint32_t addr);

#endif
