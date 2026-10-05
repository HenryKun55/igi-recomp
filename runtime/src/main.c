/* Loads IGI.exe into guest memory, wires imports to host thunks, and runs it. */
#include "igi_controls.h"
#include "native.h"
#include "runtime.h"
#include "tick_rate.h"

#include <stdio.h>
#include <stdlib.h>

char g_game_dir[1024];

static Ctx g_ctx;

/* ---- logging ------------------------------------------------------------ */

static FILE *log_file;

void platform_shutdown(void);

uint32_t win32_perf_counter_ms(void);

static double log_time(void)
{
    return (double)win32_perf_counter_ms() / 1000.0;
}

void rt_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[%7.3f] ", log_time());
    if (log_file)
        fprintf(log_file, "[%7.3f] ", log_time());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    if (log_file) {
        va_start(ap, fmt);
        vfprintf(log_file, fmt, ap);
        fputc('\n', log_file);
        fflush(log_file);
        va_end(ap);
    }
}

void rt_fatal(Ctx *c, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rt_log("FATAL: %s", msg);
    if (c) {
        rt_log("  eax=%08X ecx=%08X edx=%08X ebx=%08X", c->r[0], c->r[1], c->r[2], c->r[3]);
        rt_log("  esp=%08X ebp=%08X esi=%08X edi=%08X", c->r[4], c->r[5], c->r[6], c->r[7]);
        rt_log("  stack:");
        for (uint32_t i = 0; i < 16u; i++) {
            uint32_t a = c->r[4] + i * 4u;
            if (a < GUEST_STACK_BASE || a >= GUEST_STACK_BASE + GUEST_STACK_SIZE)
                break;
            rt_log("    [esp+%02X] %08X", i * 4u, RD32(a));
        }
    }
    platform_shutdown();
    exit(1);
}

void rt_missing_import(Ctx *c, const char *name)
{
    rt_fatal(c, "import not implemented: %s (called from %08X)", name, RD32(c->r[4]));
}

/* ---- thunks ------------------------------------------------------------- */

static GuestFn thunk_fns[THUNK_MAX];
static const char *thunk_names[THUNK_MAX];
static uint32_t thunk_count;

/* Call thunk `i` with eax, ecx and esp already stored in the context (what
 * rt_dispatch does for a thunk address, without the range check and lookup). */
static uint64_t thunk_run(Ctx *c, uint32_t i)
{
    const uint32_t target = THUNK_BASE + i * THUNK_STRIDE;
#ifdef IGI_PROFILE
    uint32_t prof_prev = rt_prof_cur;
    rt_prof_cur = target;
#endif
    g_current_thunk = target;
    thunk_fns[i](c);
#ifdef IGI_PROFILE
    rt_prof_cur = prof_prev;
#endif
    return ((uint64_t)c->r[4] << 32) | c->r[0];
}

/* Dispatch entries for the first THUNK_FAST_MAX thunks, cached by indirect call
 * sites like the entries of generated functions: `fast` is a per-thunk entry with
 * the GuestFastFn convention (the thunk index is a constant in it). */
#define THUNK_FAST_MAX 1024u
#define TF1(n)                                                                  \
    static uint64_t thunk_fast_##n(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax) \
    {                                                                           \
        c->r[0] = eax;                                                          \
        c->r[1] = ecx;                                                          \
        c->r[4] = esp;                                                          \
        return thunk_run(c, 0##n);                                              \
    }
#define TF8(n) TF1(n##0) TF1(n##1) TF1(n##2) TF1(n##3) TF1(n##4) TF1(n##5) TF1(n##6) TF1(n##7)
#define TF64(n) TF8(n##0) TF8(n##1) TF8(n##2) TF8(n##3) TF8(n##4) TF8(n##5) TF8(n##6) TF8(n##7)
#define TF512(n) TF64(n##0) TF64(n##1) TF64(n##2) TF64(n##3) TF64(n##4) TF64(n##5) TF64(n##6) TF64(n##7)
TF512(0)
TF512(1)
#define TP1(n) thunk_fast_##n,
#define TP8(n) TP1(n##0) TP1(n##1) TP1(n##2) TP1(n##3) TP1(n##4) TP1(n##5) TP1(n##6) TP1(n##7)
#define TP64(n) TP8(n##0) TP8(n##1) TP8(n##2) TP8(n##3) TP8(n##4) TP8(n##5) TP8(n##6) TP8(n##7)
#define TP512(n) TP64(n##0) TP64(n##1) TP64(n##2) TP64(n##3) TP64(n##4) TP64(n##5) TP64(n##6) TP64(n##7)
static const GuestFastFn thunk_fast[THUNK_FAST_MAX] = {TP512(0) TP512(1)};
static GuestFunc thunk_entries[THUNK_FAST_MAX];

static uint32_t thunk_add(GuestFn fn, const char *name)
{
    if (thunk_count == THUNK_MAX)
        rt_fatal(NULL, "too many host thunks");
    uint32_t i = thunk_count;
    uint32_t addr = THUNK_BASE + i * THUNK_STRIDE;
    thunk_fns[i] = fn;
    thunk_names[i] = name;
    if (i < THUNK_FAST_MAX) {
        thunk_entries[i].fn = fn;
        thunk_entries[i].fast = thunk_fast[i];
        thunk_entries[i].addr = addr;
    }
    thunk_count = i + 1u;
    return addr;
}

uint32_t thunk_register(GuestFn fn, const char *name)
{
    for (uint32_t i = 0; i < thunk_count; i++)
        if (thunk_fns[i] == fn)
            return THUNK_BASE + i * THUNK_STRIDE;
    return thunk_add(fn, name);
}

uint32_t thunk_register_unique(GuestFn fn, const char *name)
{
    return thunk_add(fn, name);
}

void thunk_set_fast(GuestFn fn, GuestFastFn fast)
{
#ifndef IGI_PROFILE
    for (uint32_t i = 0; i < thunk_count && i < THUNK_FAST_MAX; i++)
        if (thunk_fns[i] == fn)
            thunk_entries[i].fast = fast;
#else
    (void)fn;
    (void)fast;
#endif
}

GuestFn thunk_lookup(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count ? thunk_fns[i] : NULL;
}

const GuestFunc *thunk_entry(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count && i < THUNK_FAST_MAX ? &thunk_entries[i] : NULL;
}

const char *thunk_name(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count ? thunk_names[i] : "?";
}

/* ---- guest calls -------------------------------------------------------- */

uint32_t guest_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t saved[8];
    memcpy(saved, c->r, sizeof(saved));
    uint32_t esp = c->r[4];
    for (int i = nargs - 1; i >= 0; i--) {
        esp -= 4u;
        WR32(esp, args[i]);
    }
    esp -= 4u;
    WR32(esp, GUEST_RETURN_MAGIC);
    c->r[4] = esp;
    rt_dispatch(c, fn);
    uint32_t ret = c->r[0];
    memcpy(c->r, saved, sizeof(saved));
    return ret;
}

/* ---- PE loader ---------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static uint32_t load_exe(const char *path)
{
    size_t size;
    uint8_t *d = read_file(path, &size);
    if (!d)
        rt_fatal(NULL, "cannot read %s", path);
    uint32_t pe = rd32(d + 0x3C);
    if (memcmp(d + pe, "PE\0\0", 4) != 0)
        rt_fatal(NULL, "%s is not a PE file", path);
    uint32_t coff = pe + 4u;
    uint16_t nsec = rd16(d + coff + 2u);
    uint16_t opt_size = rd16(d + coff + 16u);
    uint32_t opt = coff + 20u;
    uint32_t entry = rd32(d + opt + 16u);
    uint32_t base = rd32(d + opt + 28u);
    uint32_t hdr_size = rd32(d + opt + 60u);
    if (base != GUEST_IMAGE_BASE)
        rt_fatal(NULL, "unexpected image base %08X", base);
    memcpy(g_mem + base, d, hdr_size);
    uint32_t sh = opt + opt_size;
    for (uint16_t i = 0; i < nsec; i++, sh += 40u) {
        uint32_t vsize = rd32(d + sh + 8u);
        uint32_t va = rd32(d + sh + 12u);
        uint32_t raw_size = rd32(d + sh + 16u);
        uint32_t raw_off = rd32(d + sh + 20u);
        uint32_t n = raw_size < vsize || !vsize ? raw_size : vsize;
        memset(g_mem + base + va, 0, vsize > raw_size ? vsize : raw_size);
        memcpy(g_mem + base + va, d + raw_off, n);
    }
    free(d);

    /* Point every IAT slot at the host implementation's thunk. */
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImport *imp = &g_guest_imports[i];
        WR32(imp->slot, thunk_register(imp->fn, imp->name));
    }
    return base + entry;
}

/* Thread information block: SEH chain head, stack bounds, self pointer. */
static void setup_tib(Ctx *c)
{
    uint32_t tib = scratch_alloc(0x1000, 16);
    WR32(tib + 0x00u, 0xFFFFFFFFu);
    WR32(tib + 0x04u, GUEST_STACK_BASE + GUEST_STACK_SIZE);
    WR32(tib + 0x08u, GUEST_STACK_BASE);
    WR32(tib + 0x18u, tib);
    c->fs_base = tib;
}

extern void win32_init(int argc, char **argv);

/* Hand-decompiled game functions (runtime/src/native_*.c) are on unless
 * IGI_NATIVE=0 (desktop) or native=0 in ux0:data/igi/igi.cfg (Vita); -FN turns off
 * only the function at guest address FN (hex). */
static void native_setting(void)
{
    const char *v = getenv("IGI_NATIVE");
#if defined(__vita__)
    static char line[64];
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char buf[64];
        while (fgets(buf, sizeof(buf), f))
            if (strncmp(buf, "native=", 7) == 0) {
                memcpy(line, buf + 7, sizeof(line) - 7);
                v = line;
            }
        fclose(f);
    }
#endif
    if (v && v[0] == '-') {
        /* -FN (hex guest address): only that one runs the recompiled code (A/B timing). */
        g_native_decline = (uint32_t)strtoul(v + 1, NULL, 16);
        rt_log("native game functions: on, except %08X", g_native_decline);
        return;
    }
    if (v)
        g_igi_native = v[0] != '0';
    rt_log("native game functions: %s", g_igi_native ? "on" : "off (recompiled code)");
}

/* Terrain renderer version: IGI_TERRAIN_EXACT=0|1 (desktop) or terrain_exact=0|1
 * in ux0:data/igi/igi.cfg (Vita); see runtime/src/native_terrain.c. */
static void terrain_setting(void)
{
    const char *v = getenv("IGI_TERRAIN_EXACT");
#if defined(__vita__)
    static char line[16];
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char buf[64];
        while (fgets(buf, sizeof(buf), f))
            if (strncmp(buf, "terrain_exact=", 14) == 0) {
                line[0] = buf[14];
                v = line;
            }
        fclose(f);
    }
#endif
    if (v && *v)
        g_terrain_exact = v[0] != '0';
    rt_log("terrain renderer: %s", g_terrain_exact ? "bit-exact (double precision)" : "fast (single precision)");
}

int main(int argc, char **argv)
{
    const char *dir = getenv("IGI_DIR");
    if (argc > 1)
        dir = argv[1];
#if defined(__vita__)
    if (!dir)
        dir = "ux0:data/igi/pc";
    log_file = fopen("ux0:data/igi/igi.log", "w");
#else
    if (!dir)
        dir = ".";
    log_file = fopen("igi.log", "w");
#endif
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", dir);
    if (log_file)
        setvbuf(log_file, NULL, _IONBF, 0);

    mem_init();
    native_setting();
    terrain_setting();
    capture_init();
    char exe[1100];
    snprintf(exe, sizeof(exe), "%s/IGI.exe", g_game_dir);
    uint32_t entry = load_exe(exe);
    tick_rate_init();
    igi_controls_init();
    rt_log("loaded %s, entry %08X, %u functions, %u imports", exe, entry, g_guest_func_count, g_guest_import_count);

#ifdef IGI_PROFILE
    rt_prof_start();
#endif
    Ctx *c = &g_ctx;
    rt_fninit(c);
    setup_tib(c);
    c->r[4] = GUEST_STACK_BASE + GUEST_STACK_SIZE - 64u;
    win32_init(argc, argv);

    c->r[4] -= 4u;
    WR32(c->r[4], GUEST_RETURN_MAGIC);
    rt_dispatch(c, entry);
    rt_log("guest entry point returned, eax=%08X", c->r[0]);
    return (int)c->r[0];
}
