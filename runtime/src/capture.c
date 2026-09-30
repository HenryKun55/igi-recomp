/* Guest state capture for offline differential tests (desktop only).
 *
 * IGI_TERRAIN_DUMP=PREFIX[:EVERY[:COUNT]] writes the guest registers and every
 * non-zero page of guest memory at the entry of the terrain renderer (0x512C30)
 * to PREFIX_N.bin, for every EVERY-th frame (default 200) up to COUNT files
 * (default 4). tools/recomp/terrainbench.py diff --frame replays such a file
 * through the recompiled and the native renderer and compares the results.
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

void capture_init(void)
{
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
