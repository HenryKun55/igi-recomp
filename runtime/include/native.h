/*
 * Shared by the hand-decompiled game functions (runtime/src/native_*.c).
 *
 * A native function replaces a recompiled one at its entry (NATIVE table in
 * tools/recomp/gen.py) with the GuestFastFn convention plus a result pointer:
 * it returns 1 with *ret = esp << 32 | eax when it ran, 0 to let the recompiled
 * code run. It must leave guest memory, the callee-saved registers (context),
 * eax, ecx, edx, esp and the x87 stack exactly as the recompiled code does, and
 * make the same guest calls (same targets, arguments, stack and registers).
 */
#ifndef IGI_NATIVE_H
#define IGI_NATIVE_H

#include "recomp.h"
#include "runtime.h"

/* Debug hook (desktop, runtime/src/capture.c): state capture and the live
 * native/recompiled comparison. Returns -1 to continue normally, otherwise the
 * value the native entry returns (having set *ret). */
typedef int (*NativeHook)(uint32_t fn, Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);
extern NativeHook g_native_hook;
/* Entry address of a native function that declines (the comparison's recompiled run). */
extern uint32_t g_native_decline;

/* First statement of every native entry point (c, esp, ecx, eax and ret in scope). */
#define NATIVE_ENTRY(fn)                                                    \
    do {                                                                    \
        if (__builtin_expect(g_native_hook != 0, 0)) {                      \
            int hook_r_ = g_native_hook((fn), c, esp, ecx, eax, ret);       \
            if (hook_r_ >= 0)                                               \
                return hook_r_;                                             \
        }                                                                   \
        if (!g_igi_native || g_native_decline == (fn))                      \
            return 0;                                                       \
    } while (0)

/* Guest call as `call` does it: pushes the return address below esp (the
 * arguments are already on the guest stack) and dispatches like an indirect call
 * of the recompiled code. Returns esp << 32 | eax of the callee. */
static inline uint64_t native_call(Ctx *c, uint32_t fn, uint32_t esp, uint32_t ret_addr, uint32_t ecx,
                                   uint32_t eax)
{
    esp -= 4u;
    WR32(esp, ret_addr);
    return rt_call_indirect(c, fn, esp, ecx, eax);
}

/* Same with a per-call-site cache of the target's dispatch entry (like the
 * generated code): `static const GuestFunc *ic = g_guest_func_hash;` at the site. */
static inline uint64_t native_call_ic(Ctx *c, uint32_t fn, uint32_t esp, uint32_t ret_addr, uint32_t ecx,
                                      uint32_t eax, const GuestFunc **ic)
{
    esp -= 4u;
    WR32(esp, ret_addr);
    const GuestFunc *ge = *ic;
    if (ge->addr == fn && ge->fast)
        return ge->fast(c, esp, ecx, eax);
    return rt_call_indirect_ic(c, fn, esp, ecx, eax, ic);
}

/* The guest's frame below its entry esp is dead after the return, but later
 * guest code reads uninitialised locals, so a native function leaves there what
 * the guest code would: the words collected here (offset from base, the lowest
 * address of the frame) are written by native_frame_flush. */
typedef struct NativeFrame {
    uint32_t base;
    uint64_t set;               /* bit k: w[k] is written */
    uint32_t w[64];
} NativeFrame;

static inline void native_frame_put(NativeFrame *f, uint32_t off, uint32_t v)
{
    f->w[off >> 2] = v;
    f->set |= 1ull << (off >> 2);
}

static inline void native_frame_putf(NativeFrame *f, uint32_t off, float v)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    native_frame_put(f, off, b);
}

static inline void native_frame_flush(const NativeFrame *f)
{
    for (uint64_t m = f->set; m; m &= m - 1u) {
        uint32_t k = (uint32_t)__builtin_ctzll(m);
        WR32(f->base + 4u * k, f->w[k]);
    }
}

/* The x87 status word as the recompiled code keeps it: the condition codes of
 * the last comparison (evaluated lazily) over the value the last call left.
 * The generated code stores it to the context before every call. */
typedef struct NativeFsw {
    uint32_t sw;
    int pending;
    double a, b;
} NativeFsw;

static inline void native_fsw_sync(NativeFsw *f, const Ctx *c)
{
    f->sw = c->fsw;
    f->pending = 0;
    f->a = f->b = 0.0;
}

static inline void native_fcom(NativeFsw *f, double a, double b)
{
    f->a = a;
    f->b = b;
    f->pending = 1;
}

static inline uint32_t native_fsw(const NativeFsw *f)
{
    return f->pending ? rt_fcom(f->sw, f->a, f->b) : f->sw;
}

static inline float native_ldf(uint32_t a)
{
    float v;
    memcpy(&v, g_mem + a, 4);
    return v;
}

static inline void native_stf(uint32_t a, float v)
{
    memcpy(g_mem + a, &v, 4);
}

static inline double native_ldd(uint32_t a)
{
    double v;
    memcpy(&v, g_mem + a, 8);
    return v;
}

/* x87 comparisons as the game tests them: C0 ("below") is also set for
 * unordered operands, "above" needs C0 = C3 = 0. */
#define X87_BELOW(a, b) (!((a) >= (b)))
#define X87_ABOVE(a, b) ((a) > (b))

#endif
