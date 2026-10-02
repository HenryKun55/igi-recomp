/* Out-of-line CPU helpers for the recompiled code: x87 extras, string ops, dispatch. */
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>

uint8_t *g_mem;

/* ---- x87 --------------------------------------------------------------- */

double rt_ld80(uint32_t a)
{
    uint64_t mant = RD64(a);
    uint32_t se = RD16(a + 8u);
    int sign = (se >> 15) & 1;
    int exp = (int)(se & 0x7FFFu);
    double v;
    if (exp == 0 && mant == 0)
        v = 0.0;
    else if (exp == 0x7FFF)
        v = (mant << 1) ? NAN : INFINITY;
    else
        v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void rt_st80(uint32_t a, double v)
{
    uint64_t mant = 0;
    uint32_t se = 0;
    if (signbit(v))
        se = 0x8000u;
    v = fabs(v);
    if (isnan(v)) {
        se |= 0x7FFFu;
        mant = 0xC000000000000000ull;
    } else if (isinf(v)) {
        se |= 0x7FFFu;
        mant = 0x8000000000000000ull;
    } else if (v != 0.0) {
        int e;
        double m = frexp(v, &e);    /* v = m * 2^e, 0.5 <= m < 1 */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint32_t)(e - 1 + 16383) & 0x7FFFu;
    }
    WR64(a, mant);
    WR16(a + 8u, se);
}

uint32_t rt_fxam(uint32_t sw, double v)
{
    uint32_t cc;
    if (isnan(v))
        cc = FSW_C0;
    else if (isinf(v))
        cc = FSW_C0 | FSW_C2;
    else if (v == 0.0)
        cc = FSW_C3;
    else if (fpclassify(v) == FP_SUBNORMAL)
        cc = FSW_C2 | FSW_C3;
    else
        cc = FSW_C2;
    if (signbit(v))
        cc |= FSW_C1;
    return (sw & ~(FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)) | cc;
}

void rt_fninit(Ctx *c)
{
    c->fcw = 0x037Fu;
    c->fsw = 0;
    c->ftop = 0;
}

void rt_fnstenv(Ctx *c, uint32_t a)
{
    WR32(a + 0u, c->fcw);
    WR32(a + 4u, rt_fstsw(c->fsw, c->ftop));
    WR32(a + 8u, 0xFFFFu);  /* tag word: we do not track empty registers */
    for (uint32_t i = 12u; i < 28u; i += 4u)
        WR32(a + i, 0);
    /* fnstenv masks all exceptions afterwards. */
    c->fcw |= 0x3Fu;
}

void rt_fldenv(Ctx *c, uint32_t a)
{
    uint32_t sw = RD32(a + 4u);
    c->fcw = RD32(a) & 0xFFFFu;
    c->fsw = sw & ~0x3800u;
    c->ftop = (sw >> 11) & 7u;
}

void rt_fnsave(Ctx *c, uint32_t a)
{
    rt_fnstenv(c, a);
    for (uint32_t i = 0; i < 8u; i++)
        rt_st80(a + 28u + i * 10u, ST(i));
    rt_fninit(c);
}

void rt_frstor(Ctx *c, uint32_t a)
{
    rt_fldenv(c, a);
    for (uint32_t i = 0; i < 8u; i++)
        ST(i) = rt_ld80(a + 28u + i * 10u);
}

/* fist outside the inline fast path. For |v| < 2^31 - 1 the rounding is exact
 * with a truncating conversion and the exact fraction v - trunc(v). */
#define FIST_SMALL(v) ((v) > -2147483647.0 && (v) < 2147483647.0)

static int32_t fround_small(const Ctx *c, double v)
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

int64_t rt_fist64_slow(const Ctx *c, double v)
{
    if (FIST_SMALL(v))
        return fround_small(c, v);
    double r = rt_fround(c, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)r;
}

int32_t rt_fist32_slow(const Ctx *c, double v)
{
    if (FIST_SMALL(v))
        return fround_small(c, v);
    double r = rt_fround(c, v);
    if (!(r >= -2147483648.0 && r < 2147483648.0))
        return INT32_MIN;
    return (int32_t)r;
}

int16_t rt_fist16_slow(const Ctx *c, double v)
{
    if (FIST_SMALL(v)) {
        int32_t r = fround_small(c, v);
        return r >= -32768 && r < 32768 ? (int16_t)r : INT16_MIN;
    }
    double r = rt_fround(c, v);
    if (!(r >= -32768.0 && r < 32768.0))
        return INT16_MIN;
    return (int16_t)r;
}

/* ---- integer ----------------------------------------------------------- */

uint32_t rt_rcl(uint32_t d, uint32_t n, int bits, int *cf, int *of)
{
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    n %= (uint32_t)(bits + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = (d >> (bits - 1)) & 1u;
        d = ((d << 1) | (uint32_t)*cf) & mask;
        *cf = (int)out;
    }
    if (n)
        *of = (int)(((d >> (bits - 1)) & 1u) ^ (uint32_t)*cf);
    return d;
}

uint32_t rt_rcr(uint32_t d, uint32_t n, int bits, int *cf, int *of)
{
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    n %= (uint32_t)(bits + 1);
    if (n)
        *of = (int)(((d >> (bits - 1)) & 1u) ^ (uint32_t)*cf);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t out = d & 1u;
        d = ((d >> 1) | ((uint32_t)*cf << (bits - 1))) & mask;
        *cf = (int)out;
    }
    return d;
}

/* Stops at the first match, reading aligned words, which never touch a page the
 * byte loop would not have read. */
RtScas rt_scasb_fwd(const uint8_t *gm, uint32_t n, uint32_t di, uint32_t al)
{
    const uint8_t *p = gm + di;
    uint32_t i = 0;
    RtScas r = { 1, 0, 0, al };
    while (i < n && ((uintptr_t)(p + i) & 3u)) {
        if (p[i] == al)
            goto found;
        i++;
    }
    const uint32_t pat = al * 0x01010101u;
    while (n - i >= 4u) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        w ^= pat;
        if ((w - 0x01010101u) & ~w & 0x80808080u)
            break;
        i += 4u;
    }
    for (; i < n; i++)
        if (p[i] == al)
            goto found;
    r.cx = 0;
    r.di = di + n;
    r.last = p[n - 1u];
    return r;
found:
    r.cx = n - i - 1u;
    r.di = di + i + 1u;
    return r;
}

/* Element by element, as the CPU does: needed for backward copies and for a
 * destination overlapping ahead of the source (pattern replication). */
RtStr rt_rep_movs_slow(uint8_t *gm, uint32_t df, uint32_t n, uint32_t si, uint32_t di, uint32_t size)
{
    uint32_t step = df ? 0u - size : size;
    for (; n; n--) {
        if (size == 1)
            gm[di] = gm[si];
        else if (size == 2)
            rt_st16(gm + di, rt_ld16(gm + si));
        else
            rt_st32(gm + di, rt_ld32(gm + si));
        si += step;
        di += step;
    }
    RtStr r = { 0, si, di };
    return r;
}

RtStr rt_rep_stos_slow(uint8_t *gm, uint32_t df, uint32_t n, uint32_t di, uint32_t v, uint32_t size)
{
    uint32_t step = df ? 0u - size : size;
    for (; n; n--) {
        if (size == 1)
            gm[di] = (uint8_t)v;
        else if (size == 2)
            rt_st16(gm + di, v);
        else
            rt_st32(gm + di, v);
        di += step;
    }
    RtStr r = { 0, 0, di };
    return r;
}

/* GCC must not turn these loops back into memmove/memset calls. */
#if defined(__GNUC__) && !defined(__clang__)
#define RT_NO_LIBCALL __attribute__((optimize("no-tree-loop-distribute-patterns")))
#else
#define RT_NO_LIBCALL
#endif

RT_NO_LIBCALL void rt_copy_fwd(uint8_t *d, const uint8_t *s, uint32_t b)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; b >= 16u; b -= 16u, d += 16, s += 16)
        vst1q_u8(d, vld1q_u8(s));
    if (b & 8u) {
        vst1_u8(d, vld1_u8(s));
        d += 8;
        s += 8;
    }
    if (b & 4u) {
        rt_st32(d, rt_ld32(s));
        d += 4;
        s += 4;
    }
    if (b & 2u) {
        d[0] = s[0];
        d[1] = s[1];
        d += 2;
        s += 2;
    }
    if (b & 1u)
        d[0] = s[0];
#else
    memmove(d, s, b);
#endif
}

RT_NO_LIBCALL void rt_fill_fwd(uint8_t *d, uint32_t v, uint32_t b)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const uint8x16_t q = vreinterpretq_u8_u32(vdupq_n_u32(v));
    for (; b >= 16u; b -= 16u, d += 16)
        vst1q_u8(d, q);
    if (b & 8u) {
        vst1_u8(d, vget_low_u8(q));
        d += 8;
    }
    if (b & 4u) {
        rt_st32(d, v);
        d += 4;
    }
    if (b & 2u) {
        d[0] = (uint8_t)v;
        d[1] = (uint8_t)(v >> 8);
        d += 2;
    }
    if (b & 1u)
        d[0] = (uint8_t)v;
#else
    for (uint32_t i = 0; i < b; i++)
        d[i] = (uint8_t)(v >> (8u * (i & 3u)));
#endif
}

/* ---- dispatch ---------------------------------------------------------- */

/* Open-addressing hash table built by the generator (dispatch_table.c): a
 * power-of-two sized array of {addr, fn} with linear probing and empty slots
 * (addr 0) terminating each probe sequence. */
static GuestFn lookup_func(uint32_t addr)
{
    uint32_t mask = g_guest_func_hash_mask;
    uint32_t i = GUEST_FUNC_HASH(addr) & mask;
    for (;;) {
        const GuestFunc *e = &g_guest_func_hash[i];
        if (e->addr == addr)
            return e->fn;
        if (!e->addr)
            return NULL;
        i = (i + 1u) & mask;
    }
}

uint32_t g_current_thunk;

void rt_dispatch(Ctx *c, uint32_t target)
{
    GuestFn fn;
    if (target >= THUNK_BASE && target < THUNK_BASE + THUNK_MAX * THUNK_STRIDE) {
        fn = thunk_lookup(target);
        if (fn) {
#ifdef IGI_PROFILE
            uint32_t prof_prev = rt_prof_cur;
            rt_prof_cur = target;
#endif
            g_current_thunk = target;
            fn(c);
#ifdef IGI_PROFILE
            rt_prof_cur = prof_prev;
#endif
            return;
        }
    } else {
        fn = lookup_func(target);
        if (fn) {
            fn(c);
            return;
        }
    }
    if (target == GUEST_RETURN_MAGIC)
        rt_fatal(c, "guest returned to the host return address as a jump target");
    rt_fatal(c, "dispatch to unknown address %08X (return address %08X)", target, RD32(c->r[4]));
}

/* Reached only from code generated with IGI_X87_CHECK. */
void rt_x87_mismatch(Ctx *c, uint32_t addr, uint32_t expect)
{
    rt_fatal(c, "x87 stack top %u after the call at %08X, expected %u", c->ftop, addr, expect);
}

/* Indirect call of a host thunk (rt_dispatch with the registers in the context). */
static inline uint64_t call_thunk(Ctx *c, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t eax)
{
    const GuestFunc *e = thunk_entry(target);
    if (e)
        return e->fast(c, esp, ecx, eax);
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[4] = esp;
    rt_dispatch(c, target);
    return ((uint64_t)c->r[4] << 32) | c->r[0];
}

uint64_t rt_call_indirect_ic(Ctx *c, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t eax,
                             const GuestFunc **ic)
{
    /* Host thunks (imports, COM methods called through vtables) are not in the
     * table: their entries come from thunk_entry and are cached like the others. */
    if (target - THUNK_BASE < THUNK_MAX * THUNK_STRIDE) {
        const GuestFunc *e = thunk_entry(target);
        if (e) {
            *ic = e;
            return e->fast(c, esp, ecx, eax);
        }
        return call_thunk(c, target, esp, ecx, eax);
    }
    uint32_t mask = g_guest_func_hash_mask;
    uint32_t i = GUEST_FUNC_HASH(target) & mask;
    for (;;) {
        const GuestFunc *e = &g_guest_func_hash[i];
        if (e->addr == target && e->fast) {
            *ic = e;
            return e->fast(c, esp, ecx, eax);
        }
        if (!e->addr)
            break;
        i = (i + 1u) & mask;
    }
    return rt_call_indirect(c, target, esp, ecx, eax);
}

uint64_t rt_call_indirect(Ctx *c, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t eax)
{
    if (target - THUNK_BASE < THUNK_MAX * THUNK_STRIDE)
        return call_thunk(c, target, esp, ecx, eax);
    uint32_t mask = g_guest_func_hash_mask;
    uint32_t i = GUEST_FUNC_HASH(target) & mask;
    for (;;) {
        const GuestFunc *e = &g_guest_func_hash[i];
        if (e->addr == target && e->fast)
            return e->fast(c, esp, ecx, eax);
        if (!e->addr)
            break;
        i = (i + 1u) & mask;
    }
    /* Unknown address, which rt_dispatch reports. */
    c->r[0] = eax;
    c->r[1] = ecx;
    c->r[4] = esp;
    rt_dispatch(c, target);
    return ((uint64_t)c->r[4] << 32) | c->r[0];
}

void rt_unimplemented(Ctx *c, uint32_t addr, const char *what)
{
    rt_fatal(c, "unimplemented instruction at %08X: %s", addr, what);
}

void rt_divide_error(Ctx *c, uint32_t addr)
{
    rt_fatal(c, "integer divide by zero at %08X", addr);
}

void rt_noreturn(Ctx *c, uint32_t addr)
{
    rt_fatal(c, "returned from a no-return call at %08X", addr);
}


/* ---- setjmp / longjmp ---------------------------------------------------- */

#define MAX_JMPBUFS 16
static uint32_t jmp_guest[MAX_JMPBUFS];
static jmp_buf jmp_host[MAX_JMPBUFS];

jmp_buf *rt_jmpbuf(uint32_t guest_buf)
{
    for (int i = 0; i < MAX_JMPBUFS; i++)
        if (jmp_guest[i] == guest_buf)
            return &jmp_host[i];
    for (int i = 0; i < MAX_JMPBUFS; i++)
        if (!jmp_guest[i]) {
            jmp_guest[i] = guest_buf;
            return &jmp_host[i];
        }
    rt_fatal(NULL, "too many setjmp buffers");
}

void rt_longjmp(Ctx *c, uint32_t guest_buf)
{
    (void)c;
    for (int i = 0; i < MAX_JMPBUFS; i++)
        if (jmp_guest[i] == guest_buf)
            longjmp(jmp_host[i], 1);
    rt_fatal(c, "longjmp to unknown buffer %08X", guest_buf);
}
