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
