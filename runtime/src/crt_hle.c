/* Native versions of hot routines of IGI.exe's statically linked C runtime.
 *
 * tools/recomp/gen.py routes the entry of each routine here when the routine's
 * bytes match the expected MSVC runtime code (HLE table in gen.py). Each version
 * reproduces the routine exactly as the guest sees it: return value, every
 * register the original leaves changed (including scratch ecx/edx), the stores
 * below esp made by its pushes, and memory. They use the generated-function
 * convention: eax, ecx and esp come in as arguments, eax and esp go back in the
 * result, every other register through the context. Returning 0 runs the
 * original guest code instead. */
#include "recomp.h"

#define RET(esp, eax) (((uint64_t)(esp) << 32) | (uint32_t)(eax))

/* _stricmp(s1, s2), C locale only (the locale-aware path calls tolower). */
int rt_hle_stricmp(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    if (RD32(0x009361E4u) != 0)          /* __lc_handle[LC_CTYPE] */
        return 0;
    /* push ebp (after the return address), push edi, push esi, push ebx */
    WR32(esp - 4u, c->r[5]);
    WR32(esp - 8u, c->r[7]);
    WR32(esp - 12u, c->r[6]);
    WR32(esp - 16u, c->r[3]);
    const uint8_t *m = g_mem;
    uint32_t s1 = RD32(esp + 4u), s2 = RD32(esp + 8u);
    uint32_t cl = ecx & 0xFFu;
    int32_t result = 0;
    for (;;) {
        uint32_t a = m[s2++];              /* al */
        uint32_t b = m[s1++];              /* ah */
        if (a == b) {
            if (a == 0)
                break;
            continue;
        }
        cl = ((a - 0x41u) & 0xFFu) < 0x1Au ? 0x20u : 0u;
        uint32_t la = (a + cl) & 0xFFu;
        cl = ((b - 0x41u) & 0xFFu) < 0x1Au ? 0x20u : 0u;
        uint32_t lb = (b + cl) & 0xFFu;
        if (lb == la)
            continue;
        result = lb < la ? -1 : 1;
        break;
    }
    c->r[1] = (ecx & 0xFFFFFF00u) | cl;
    *ret = RET(esp + 4u, result);
    return 1;
}

/* memset(dst, value, count) */
int rt_hle_memset(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx;
    (void)eax;
    uint32_t dst = RD32(esp + 4u), n = RD32(esp + 12u);
    if (n == 0) {
        c->r[1] = dst;
        c->r[2] = 0;
        *ret = RET(esp + 4u, dst);
        return 1;
    }
    uint32_t v = g_mem[esp + 8u];
    WR32(esp - 4u, c->r[7]);             /* push edi */
    memset(g_mem + dst, (int)v, n);
    c->r[1] = n >= 4u ? 0u : dst;
    c->r[2] = 0;
    *ret = RET(esp + 4u, dst);
    return 1;
}
