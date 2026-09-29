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

void rt_rep_movs(Ctx *c, int size)
{
    uint32_t n = c->r[1];
    uint32_t src = c->r[6], dst = c->r[7];
    uint32_t bytes = n * (uint32_t)size;
    if (n == 0)
        return;
    if (!c->df) {
        /* Byte-wise forward semantics matter only when dst overlaps ahead of src. */
        if (dst <= src || dst >= src + bytes) {
            memmove(g_mem + dst, g_mem + src, bytes);
        } else {
            for (uint32_t i = 0; i < bytes; i++)
                g_mem[dst + i] = g_mem[src + i];
        }
        c->r[6] = src + bytes;
        c->r[7] = dst + bytes;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            memmove(g_mem + dst, g_mem + src, (size_t)size);
            src -= (uint32_t)size;
            dst -= (uint32_t)size;
        }
        c->r[6] = src;
        c->r[7] = dst;
    }
    c->r[1] = 0;
}

void rt_rep_stos(Ctx *c, int size)
{
    uint32_t n = c->r[1];
    uint32_t dst = c->r[7];
    uint32_t v = c->r[0];
    int32_t step = c->df ? -size : size;
    if (!c->df && size == 1) {
        memset(g_mem + dst, (int)(v & 0xFFu), n);
        dst += n;
    } else if (!c->df && size == 4 && v == 0) {
        memset(g_mem + dst, 0, (size_t)n * 4u);
        dst += n * 4u;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (size == 1)
                WR8(dst, v);
            else if (size == 2)
                WR16(dst, v);
            else
                WR32(dst, v);
            dst += (uint32_t)step;
        }
    }
    c->r[7] = dst;
    c->r[1] = 0;
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
            g_current_thunk = target;
            fn(c);
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

#ifdef IGI_X87_CHECK
void rt_x87_mismatch(Ctx *c, uint32_t addr, uint32_t expect)
{
    rt_fatal(c, "x87 stack top %u after the call at %08X, expected %u", c->ftop, addr, expect);
}
#endif

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
