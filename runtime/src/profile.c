/* Sampling profiler for the recompiled game (CMake option IGI_PROFILE).
 *
 * Generated code publishes the id of the code that is running in rt_prof_cur (a
 * guest function address, the IAT slot of a host import, or a host thunk address
 * for COM methods and callbacks) and keeps a shadow stack of guest functions (see
 * PROF_ENTER in recomp.h). A thread on another core samples both about 1000
 * times a second and accumulates:
 *   - exclusive samples per id (where the time is spent),
 *   - inclusive samples per guest function (on the shadow stack),
 *   - caller/callee pairs (which guest function the current code was called from).
 * Results go to ux0:data/igi/profile.txt (Vita) or igi_profile.txt (desktop) every
 * 10 seconds and at exit. */
#include "runtime.h"

typedef int profile_translation_unit_is_not_empty;

#ifdef IGI_PROFILE

#include <stdio.h>
#include <stdlib.h>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#define PROFILE_PATH "ux0:data/igi/profile.txt"
#else
#include <pthread.h>
#include <unistd.h>
#define PROFILE_PATH "igi_profile.txt"
#endif

volatile uint32_t rt_prof_cur;
volatile uint32_t rt_prof_depth;
volatile uint32_t rt_prof_stack[RT_PROF_MAX];

#define SAMPLE_US 1000u
#define DUMP_EVERY 10000u        /* samples */
#define MAP_BITS 15
#define MAP_SIZE (1u << MAP_BITS)
#define TOP 80

typedef struct Entry {
    uint64_t key;
    uint32_t count;
    uint32_t used;
} Entry;

typedef struct Map {
    Entry e[MAP_SIZE];
    uint32_t n;
} Map;

static Map excl, incl, pairs;
static uint32_t total;
static volatile int reset_requested, stopped;

static void map_add(Map *m, uint64_t key)
{
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> (64 - MAP_BITS));
    for (uint32_t probes = 0; probes < MAP_SIZE; probes++) {
        Entry *e = &m->e[i];
        if (e->used && e->key == key) {
            e->count++;
            return;
        }
        if (!e->used) {
            if (m->n >= MAP_SIZE * 3u / 4u)
                return;              /* full: drop rare keys */
            e->used = 1;
            e->key = key;
            e->count = 1;
            m->n++;
            return;
        }
        i = (i + 1u) & (MAP_SIZE - 1u);
    }
}

static void sample(void)
{
    uint32_t cur = rt_prof_cur;
    uint32_t depth = rt_prof_depth;
    if (depth > RT_PROF_MAX)
        depth = RT_PROF_MAX;
    uint32_t stack[RT_PROF_MAX];
    for (uint32_t i = 0; i < depth; i++)
        stack[i] = rt_prof_stack[i];
    total++;
    map_add(&excl, cur);
    /* Each guest function on the stack counts once per sample (recursion). */
    for (uint32_t i = 0; i < depth; i++) {
        uint32_t j = 0;
        while (j < i && stack[j] != stack[i])
            j++;
        if (j == i)
            map_add(&incl, stack[i]);
    }
    /* Caller: the innermost guest function below the current code. */
    uint32_t caller = 0;
    if (depth && stack[depth - 1] != cur)
        caller = stack[depth - 1];
    else if (depth >= 2)
        caller = stack[depth - 2];
    map_add(&pairs, ((uint64_t)caller << 32) | cur);
}

static const char *name_of(uint32_t id, char *buf, size_t n)
{
    if (id == 0) {
        snprintf(buf, n, "(host, outside guest code)");
        return buf;
    }
    for (uint32_t i = 0; i < g_guest_import_count; i++)
        if (g_guest_imports[i].slot == id) {
            snprintf(buf, n, "%s!%s", g_guest_imports[i].dll, g_guest_imports[i].name);
            return buf;
        }
    static const char *const host[] = { "?", "decode", "near clip", "pack", "batch", "gl state", "gl draw",
                                        "texture", "blit", "audio mix", "index scan", "fill (render target)",
                                        "fill (memory surface)", "blit (memory surface)" };
    if ((id & 0xFFFFFF00u) == 0xFD000000u && (id & 0xFFu) < sizeof(host) / sizeof(host[0])) {
        snprintf(buf, n, "host %s", host[id & 0xFFu]);
        return buf;
    }
    if (id >= THUNK_BASE && id < THUNK_BASE + THUNK_MAX * THUNK_STRIDE) {
        snprintf(buf, n, "thunk %s", thunk_name(id));
        return buf;
    }
    snprintf(buf, n, "f_%08X", id);
    return buf;
}

static int by_count(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return x->key < y->key ? -1 : x->key > y->key;
}

static void dump_map(FILE *f, const Map *m, const char *title, int is_pair)
{
    static Entry sorted[MAP_SIZE];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAP_SIZE; i++)
        if (m->e[i].used)
            sorted[n++] = m->e[i];
    qsort(sorted, n, sizeof(Entry), by_count);
    fprintf(f, "\n%s\n", title);
    for (uint32_t i = 0; i < n && i < TOP; i++) {
        char a[160], b[160];
        double pct = total ? 100.0 * sorted[i].count / total : 0.0;
        if (is_pair)
            fprintf(f, "%6.2f%% %8u  %-32s <- %s\n", pct, sorted[i].count,
                    name_of((uint32_t)sorted[i].key, a, sizeof(a)),
                    name_of((uint32_t)(sorted[i].key >> 32), b, sizeof(b)));
        else
            fprintf(f, "%6.2f%% %8u  %08X  %s\n", pct, sorted[i].count, (uint32_t)sorted[i].key,
                    name_of((uint32_t)sorted[i].key, a, sizeof(a)));
    }
}

static void dump(void)
{
    FILE *f = fopen(PROFILE_PATH, "w");
    if (!f)
        return;
    fprintf(f, "IGI profile: %u samples (about %u ms of run time, one sample per %u us)\n", total,
            total * SAMPLE_US / 1000u, SAMPLE_US);
    dump_map(f, &excl, "Exclusive (code running when sampled; imports as dll!name, COM/callbacks as thunk):", 0);
    dump_map(f, &incl, "Inclusive (guest functions on the shadow call stack):", 0);
    dump_map(f, &pairs, "Current code <- calling guest function:", 1);
    fclose(f);
}

static void sampler_loop(void)
{
    for (;;) {
#if defined(__vita__)
        sceKernelDelayThread(SAMPLE_US);
#else
        usleep(SAMPLE_US);
#endif
        if (stopped)
            continue;
        if (reset_requested) {
            memset(&excl, 0, sizeof(excl));
            memset(&incl, 0, sizeof(incl));
            memset(&pairs, 0, sizeof(pairs));
            total = 0;
            reset_requested = 0;
        }
        sample();
        if (total % DUMP_EVERY == 0)
            dump();
    }
}

#if defined(__vita__)
static int sampler_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    sampler_loop();
    return 0;
}
#else
static void *sampler_thread(void *arg)
{
    (void)arg;
    sampler_loop();
    return NULL;
}
#endif

void rt_prof_reset(void)
{
    reset_requested = 1;   /* done by the sampler thread, between samples */
}

void rt_prof_flush(void)
{
    stopped = 1;
    /* Let a sample in progress finish before reading the maps. */
#if defined(__vita__)
    sceKernelDelayThread(2 * SAMPLE_US);
#else
    usleep(2 * SAMPLE_US);
#endif
    dump();
}

void rt_prof_start(void)
{
    atexit(dump);
#if defined(__vita__)
    /* High priority on a core the game thread does not use. */
    SceUID t = sceKernelCreateThread("igi_profiler", sampler_thread, 0x10000100 - 20, 0x4000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, sampler_thread, NULL) == 0)
        pthread_detach(t);
#endif
    rt_log("profiler: sampling every %u us into %s", SAMPLE_US, PROFILE_PATH);
}

#endif
