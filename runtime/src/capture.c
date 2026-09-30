/* Guest state capture for offline differential tests (desktop only).
 *
 * IGI_TERRAIN_DUMP=PREFIX[:EVERY[:COUNT]] writes the guest registers and every
 * non-zero page of guest memory at the entry of the terrain renderer (0x512C30)
 * to PREFIX_N.bin, for every EVERY-th frame (default 200) up to COUNT files
 * (default 4). tools/recomp/terrainbench.py diff --frame replays such a file
 * through the recompiled and the native renderer and compares the results.
 *
 * IGI_NATIVE_CHECK=N runs the terrain pass of N frames both natively and with the
 * recompiled code from the same state and logs the guest memory differences (not
 * the Direct3D calls: terrainbench.py compares those).
 *
 * File: "IGTD", version, esp, eax, ecx, fcw, r[8], then {address, length, bytes}
 * records of 4 KiB pages, ended by a zero record. */
#include "runtime.h"

#if !defined(__vita__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void (*g_native_capture)(Ctx *c, uint32_t esp);

static char prefix[900];
static unsigned every = 200, count = 4, calls, written;

static void wr(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f);
}

static void capture(Ctx *c, uint32_t esp)
{
    if (written >= count || calls++ % every != every - 1)
        return;
    char path[1000];
    snprintf(path, sizeof(path), "%s_%u.bin", prefix, written++);
    FILE *f = fopen(path, "wb");
    if (!f) {
        rt_log("capture: cannot write %s", path);
        return;
    }
    wr(f, 0x44544749u);   /* "IGTD" */
    wr(f, 1);
    wr(f, esp);
    wr(f, c->r[0]);
    wr(f, c->r[1]);
    wr(f, c->fcw);
    for (int i = 0; i < 8; i++)
        wr(f, c->r[i]);
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
    fclose(f);
    rt_log("capture: %s (%u pages, esp %08X)", path, pages, esp);
}

/* IGI_NATIVE_CHECK=N: for N frames, run the terrain pass natively and with the
 * recompiled code from the same guest state and log the differences (guest
 * memory below CHECK_TOP outside the dead stack, eax/esp). The recompiled
 * result is kept. */
#include "recomp_gen.h"
#define CHECK_TOP 0x10000000u

extern int (*g_native_check)(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);
extern int g_native_checking;
extern int g_igi_native;
static unsigned check_left;
static uint8_t *snap, *res_native;

static int native_check(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    if (!check_left)
        return 0;
    check_left--;
    if (!snap) {
        snap = malloc(CHECK_TOP);
        res_native = malloc(CHECK_TOP);
    }
    Ctx c0 = *c;
    memcpy(snap + GUEST_NULL_GUARD, g_mem + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    g_native_checking = 1;
    int saved = g_igi_native;
    g_igi_native = 1;
    uint64_t rn = f_00512C30(c, esp, ecx, eax);
    memcpy(res_native + GUEST_NULL_GUARD, g_mem + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    memcpy(g_mem + GUEST_NULL_GUARD, snap + GUEST_NULL_GUARD, CHECK_TOP - GUEST_NULL_GUARD);
    *c = c0;
    g_igi_native = 0;
    uint64_t rr = f_00512C30(c, esp, ecx, eax);
    g_igi_native = saved;
    g_native_checking = 0;
    unsigned n = 0;
    uint32_t lo = esp - 0x40000u;
    for (uint32_t a = GUEST_NULL_GUARD; a < CHECK_TOP; a += 4u) {
        if (a == lo)
            a = esp;
        if (memcmp(res_native + a, g_mem + a, 4) != 0 && n++ < 24)
            rt_log("check: %08X native %08X recompiled %08X", a, *(uint32_t *)(res_native + a), RD32(a));
    }
    rt_log("check: terrain frame %s (%u differing words, eax %08X/%08X)", n ? "DIFFERENT" : "identical", n,
           (uint32_t)rn, (uint32_t)rr);
    *ret = rr;
    return 1;
}

void capture_init(void)
{
    const char *k = getenv("IGI_NATIVE_CHECK");
    if (k && atoi(k) > 0) {
        check_left = (unsigned)atoi(k);
        g_native_check = native_check;
        rt_log("check: comparing native and recompiled terrain for %u frames", check_left);
    }
    const char *e = getenv("IGI_TERRAIN_DUMP");
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
    g_native_capture = capture;
    rt_log("capture: terrain frames to %s_N.bin, every %u, up to %u", prefix, every, count);
}
#else
void capture_init(void)
{
}
#endif
