/* Debug support of the hand-decompiled game functions (desktop only).
 *
 * IGI_NATIVE_DUMP=FN:PREFIX[:EVERY[:COUNT]] writes the guest registers, the x87
 * state and every non-zero page of guest memory at the entry of the native
 * function FN (hex guest address) to PREFIX_N.bin, for every EVERY-th call
 * (default 200) up to COUNT files (default 4). IGI_TERRAIN_DUMP=PREFIX[:...] is
 * the same for the terrain renderer (0x512C30). tools/recomp/nativebench.py
 * (and terrainbench.py for the terrain) replays such a file through the
 * recompiled and the native code and compares the results.
 *
 * IGI_NATIVE_CHECK=N[:FN[:EVERY]] runs N calls (every EVERY-th, default 1) of
 * the native function FN (default: every native function) both natively and
 * with the recompiled code from the same state, and logs the differences:
 * guest memory below CHECK_TOP outside the dead stack, eax, ecx, edx, esp, the
 * callee-saved registers and the x87 stack top and live registers. The
 * recompiled result is kept.
 *
 * File: "IGTD", version 2, fn, esp, fcw, ftop, fsw, r[8], st[8] (doubles), then
 * {address, length, bytes} records of 4 KiB pages, ended by a zero record, then
 * {thunk address, length, name} records of the host thunks, ended by a zero word.
 * Version 1 (terrain only): "IGTD", 1, esp, eax, ecx, fcw, r[8], pages. */
#include "native.h"

#if !defined(__vita__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char prefix[900];
static uint32_t dump_fn;
static unsigned every = 200, count = 4, calls, written;

static uint32_t check_fn, check_every = 1, check_calls;
static unsigned check_left;
static int checking;
static uint8_t *snap, *res_native;

static void wr(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void capture(uint32_t fn, Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax)
{
    if (fn != dump_fn || written >= count || calls++ % every != every - 1)
        return;
    char path[1000];
    snprintf(path, sizeof(path), "%s_%u.bin", prefix, written++);
    FILE *f = fopen(path, "wb");
    if (!f) {
        rt_log("capture: cannot write %s", path);
        return;
    }
    uint32_t r[8];
    memcpy(r, c->r, sizeof(r));
    r[0] = eax;
    r[1] = ecx;
    r[4] = esp;
    wr(f, 0x44544749u);   /* "IGTD" */
    wr(f, 2);
    wr(f, fn);
    wr(f, esp);
    wr(f, c->fcw);
    wr(f, c->ftop);
    wr(f, c->fsw);
    for (int i = 0; i < 8; i++)
        wr(f, r[i]);
    fwrite(c->st, 8, 8, f);
    static const uint8_t zero[4096];
    uint32_t pages = 0;
    for (uint32_t a = GUEST_NULL_GUARD; a < g_guest_mem_size && a >= GUEST_NULL_GUARD; a += 4096u) {
        if (memcmp(g_mem + a, zero, 4096) == 0)
            continue;
        wr(f, a);
        wr(f, 4096);
        fwrite(g_mem + a, 4096, 1, f);
        pages++;
    }
    wr(f, 0);
    wr(f, 0);
    /* Host thunk names (the replay needs their argument sizes). */
    for (uint32_t t = THUNK_BASE; t < THUNK_BASE + THUNK_MAX * THUNK_STRIDE; t += THUNK_STRIDE) {
        const char *name = thunk_name(t);
        if (strcmp(name, "?") == 0)
            break;
        uint32_t len = (uint32_t)strlen(name);
        wr(f, t);
        wr(f, len);
        fwrite(name, 1, len, f);
    }
    wr(f, 0);
    fclose(f);
    rt_log("capture: %s (%08X, %u pages, esp %08X)", path, fn, pages, esp);
}

/* IGI_NATIVE_CHECK: run the native and the recompiled version from the same
 * guest state and log the differences. The recompiled result is kept. */
#define CHECK_TOP 0x10000000u
#define CHECK_STACK 0x40000u   /* dead stack below the entry esp, not compared */

static int check(uint32_t fn, Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!snap) {
        snap = malloc(CHECK_TOP);
        res_native = malloc(CHECK_TOP);
        if (!snap || !res_native) {
            check_left = 0;
            return -1;
        }
    }
    check_left--;
    Ctx c0 = *c;
    memcpy(snap + GUEST_NULL_GUARD, g_mem + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    checking = 1;
    uint32_t saved_decline = g_native_decline;
    g_native_decline = 0;
    uint64_t rn = rt_call_indirect(c, fn, esp, ecx, eax);
    Ctx cn = *c;
    memcpy(res_native + GUEST_NULL_GUARD, g_mem + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    memcpy(g_mem + GUEST_NULL_GUARD, snap + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    *c = c0;
    g_native_decline = fn;
    uint64_t rr = rt_call_indirect(c, fn, esp, ecx, eax);
    g_native_decline = saved_decline;
    checking = 0;

    unsigned n = 0;
    uint32_t lo = esp - CHECK_STACK;
    for (uint32_t a = GUEST_NULL_GUARD; a < CHECK_TOP; a += 4u) {
        if (a == lo)
            a = esp;
        if (memcmp(res_native + a, g_mem + a, 4) != 0 && n++ < 24)
            rt_log("check %08X: mem %08X native %08X recompiled %08X", fn, a, *(uint32_t *)(res_native + a), RD32(a));
    }
    static const int regs[] = { 1, 2, 3, 5, 6, 7 };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        if (cn.r[regs[i]] != c->r[regs[i]] && n++ < 48)
            rt_log("check %08X: r%d native %08X recompiled %08X", fn, regs[i], cn.r[regs[i]], c->r[regs[i]]);
    if (rn != rr && n++ < 48)
        rt_log("check %08X: esp:eax native %016llX recompiled %016llX", fn, (unsigned long long)rn,
               (unsigned long long)rr);
    if (cn.ftop != c->ftop && n++ < 48)
        rt_log("check %08X: x87 top native %u recompiled %u", fn, cn.ftop, c->ftop);
    /* Registers live at the entry (the function leaves them alone). */
    for (uint32_t k = c0.ftop; k != 0 && k < 8u && cn.ftop == c->ftop; k++)
        if (memcmp(&cn.st[k], &c->st[k], 8) != 0 && n++ < 48)
            rt_log("check %08X: st phys %u differs", fn, k);
    rt_log("check %08X: call %u %s (%u differences, eax %08X/%08X)", fn, check_calls, n ? "DIFFERENT" : "identical",
           n, (uint32_t)rn, (uint32_t)rr);
    *ret = rr;
    return 1;
}

static int hook(uint32_t fn, Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (checking)
        return -1;
    if (prefix[0])
        capture(fn, c, esp, ecx, eax);
    if (check_left && (!check_fn || fn == check_fn) && check_calls++ % check_every == check_every - 1)
        return check(fn, c, esp, ecx, eax, ret);
    return -1;
}

void capture_init(void)
{
    const char *k = getenv("IGI_NATIVE_CHECK");
    if (k && atoi(k) > 0) {
        char *p;
        check_left = (unsigned)strtoul(k, &p, 10);
        if (*p == ':') {
            check_fn = (uint32_t)strtoul(p + 1, &p, 16);
            if (*p == ':')
                check_every = (uint32_t)strtoul(p + 1, NULL, 10);
        }
        if (!check_every)
            check_every = 1;
        g_native_hook = hook;
        rt_log("check: comparing native and recompiled code for %u calls of %08X, every %u", check_left, check_fn,
               check_every);
    }
    const char *e = getenv("IGI_NATIVE_DUMP");
    if (e && *e) {
        char *p;
        dump_fn = (uint32_t)strtoul(e, &p, 16);
        if (*p != ':')
            return;
        e = p + 1;
    } else {
        e = getenv("IGI_TERRAIN_DUMP");
        dump_fn = 0x00512C30u;
    }
    if (!e || !*e)
        return;
    snprintf(prefix, sizeof(prefix), "%s", e);
    char *p = strchr(prefix, ':');
    if (p) {
        *p++ = 0;
        every = (unsigned)strtoul(p, &p, 10);
        if (*p == ':')
            count = (unsigned)strtoul(p + 1, NULL, 10);
    }
    if (!every)
        every = 1;
    g_native_hook = hook;
    rt_log("capture: calls of %08X to %s_N.bin, every %u, up to %u", dump_fn, prefix, every, count);
}
#else
void capture_init(void)
{
}
#endif
