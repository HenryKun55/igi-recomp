/* Loads IGI.exe into guest memory, wires imports to host thunks, and runs it. */
#include "native.h"
#include "runtime.h"

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

uint32_t thunk_register(GuestFn fn, const char *name)
{
    for (uint32_t i = 0; i < thunk_count; i++)
        if (thunk_fns[i] == fn)
            return THUNK_BASE + i * THUNK_STRIDE;
    if (thunk_count == THUNK_MAX)
        rt_fatal(NULL, "too many host thunks");
    thunk_fns[thunk_count] = fn;
    thunk_names[thunk_count] = name;
    return THUNK_BASE + (thunk_count++) * THUNK_STRIDE;
}

uint32_t thunk_register_unique(GuestFn fn, const char *name)
{
    if (thunk_count == THUNK_MAX)
        rt_fatal(NULL, "too many host thunks");
    thunk_fns[thunk_count] = fn;
    thunk_names[thunk_count] = name;
    return THUNK_BASE + (thunk_count++) * THUNK_STRIDE;
}

GuestFn thunk_lookup(uint32_t addr)
{
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < thunk_count ? thunk_fns[i] : NULL;
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
    capture_init();
    char exe[1100];
    snprintf(exe, sizeof(exe), "%s/IGI.exe", g_game_dir);
    uint32_t entry = load_exe(exe);
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
