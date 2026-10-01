/* Update-rate A/B harness (desktop debugging aid, see docs/tick_rate.md).
 *
 * The game advances in fixed steps at the rate set by 0x402820 (30 Hz in
 * gameplay, IGI_GAME_HZ overrides it). To check that a run at another rate plays
 * at the same speed, this drives and records gameplay in game time, t = steps
 * done / rate since the gameplay rate was set, so runs at different rates can be
 * compared at equal t:
 *
 *   IGI_TICK_TEST=PREFIX   enables it; output files start with PREFIX
 *   IGI_TICK_INPUT=SPEC    input by game time, "T0-T1:ACT;...": ACT is a DIK code
 *                          in hex (held from T0 to T1), lmb (left button held),
 *                          mx=R / my=R (mouse motion, R units per second)
 *   IGI_TICK_WATCH=SPEC    values logged every step to PREFIX_trace.txt,
 *                          "NAME=CHAIN:TYPE,...": CHAIN is hex numbers joined by
 *                          '>' (5690D0>10>24 = [[0x5690D0]+0x10]+0x24), TYPE is
 *                          f (float), d (double), i (int32), u (uint32), h (int16)
 *   IGI_TICK_SNAP=T,...    guest memory snapshots PREFIX_snap_T.bin (non-zero pages)
 *   IGI_TICK_SHOT=T,...    screenshots PREFIX_shot_T.bmp of the next presented frame
 *   IGI_TICK_END=T         exits at that game time
 * Use it with IGI_FAKE_CLOCK and IGI_NO_HOST_INPUT so runs are deterministic, and
 * the benchmark driver (IGI_BENCH) to get into mission 1: in this mode the driver
 * only goes through the menus and leaves the rest (cutscene included) to the game. */
#include "tick_test.h"
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(__vita__)

#define CLOCK_OBJ 0x00567C8Cu   /* game clock: +0x34 steps, +0x3C rate (Hz) */
#define MAX_ITEMS 64

typedef struct Input {
    double t0, t1;
    int kind;        /* 0 key, 1 lmb, 2 mouse x, 3 mouse y */
    int code;        /* DIK code */
    double rate;     /* mouse units per second */
    int active;
    double acc;
} Input;

typedef struct Watch {
    char name[32];
    uint32_t chain[8];
    int n;
    char type;
} Watch;

static int enabled = -1;
static char prefix[512];
static Input inputs[MAX_ITEMS];
static int n_inputs;
static Watch watches[MAX_ITEMS];
static int n_watches;
static double snaps[MAX_ITEMS], shots[MAX_ITEMS];
static int n_snaps, n_shots;
static double end_t = -1.0;

static int epoch;              /* gameplay rate set */
static uint32_t last_steps = 0xFFFFFFFFu;
static FILE *trace;
static char shot_name[600];
static int shot_pending;

static int parse_times(const char *s, double *out)
{
    int n = 0;
    while (s && *s && n < MAX_ITEMS) {
        char *end;
        out[n++] = strtod(s, &end);
        if (end == s)
            break;
        s = *end ? end + 1 : end;
    }
    return n;
}

static void parse_inputs(const char *s)
{
    while (s && *s && n_inputs < MAX_ITEMS) {
        Input *in = &inputs[n_inputs];
        char *end;
        in->t0 = strtod(s, &end);
        if (*end != '-')
            break;
        in->t1 = strtod(end + 1, &end);
        if (*end != ':')
            break;
        const char *act = end + 1;
        const char *next = strchr(act, ';');
        if (strncmp(act, "lmb", 3) == 0) {
            in->kind = 1;
        } else if (act[0] == 'm' && (act[1] == 'x' || act[1] == 'y') && act[2] == '=') {
            in->kind = act[1] == 'x' ? 2 : 3;
            in->rate = strtod(act + 3, NULL);
        } else {
            in->kind = 0;
            in->code = (int)strtol(act, NULL, 16);
        }
        n_inputs++;
        s = next ? next + 1 : NULL;
    }
}

static void parse_watches(const char *s)
{
    while (s && *s && n_watches < MAX_ITEMS) {
        Watch *w = &watches[n_watches];
        const char *eq = strchr(s, '=');
        if (!eq)
            break;
        snprintf(w->name, sizeof(w->name), "%.*s", (int)(eq - s), s);
        const char *p = eq + 1;
        w->n = 0;
        for (;;) {
            char *end;
            w->chain[w->n++] = (uint32_t)strtoul(p, &end, 16);
            p = end;
            if (*p != '>' || w->n == 8)
                break;
            p++;
        }
        w->type = *p == ':' ? p[1] : 'u';
        n_watches++;
        const char *next = strchr(p, ',');
        s = next ? next + 1 : NULL;
    }
}

static int tick_enabled(void)
{
    if (enabled < 0) {
        const char *e = getenv("IGI_TICK_TEST");
        enabled = e && *e;
        if (!enabled)
            return 0;
        snprintf(prefix, sizeof(prefix), "%s", e);
        parse_inputs(getenv("IGI_TICK_INPUT"));
        parse_watches(getenv("IGI_TICK_WATCH"));
        n_snaps = parse_times(getenv("IGI_TICK_SNAP"), snaps);
        n_shots = parse_times(getenv("IGI_TICK_SHOT"), shots);
        e = getenv("IGI_TICK_END");
        end_t = e ? atof(e) : -1.0;
        rt_log("tick test: %d inputs, %d watches, %d snapshots, %d screenshots, end %.2f", n_inputs, n_watches,
               n_snaps, n_shots, end_t);
    }
    return enabled;
}

int tick_test_enabled(void)
{
    return tick_enabled();
}

void tick_test_set_rate(uint32_t ret, uint32_t hz)
{
    if (!tick_enabled())
        return;
    /* Gameplay starts with 0x4026AB/0x4026BC (after loading a level); the menus
     * save and restore the rate around themselves (0x417AAA, 0x418CCE). */
    if (ret == 0x004026B0u || ret == 0x004026C1u) {
        epoch = 1;
        last_steps = 0xFFFFFFFFu;
        rt_log("tick test: gameplay at %u Hz", hz);
        if (!trace) {
            char path[600];
            snprintf(path, sizeof(path), "%s_trace.txt", prefix);
            trace = fopen(path, "w");
            if (trace) {
                fprintf(trace, "t step");
                for (int i = 0; i < n_watches; i++)
                    fprintf(trace, " %s", watches[i].name);
                fprintf(trace, "\n");
            }
        }
    }
}

static uint32_t watch_addr(const Watch *w, int *ok)
{
    uint32_t a = w->chain[0];
    for (int i = 1; i < w->n; i++) {
        if (a < GUEST_NULL_GUARD || a >= g_guest_mem_size - 4u) {
            *ok = 0;
            return 0;
        }
        a = RD32(a) + w->chain[i];
    }
    *ok = a >= GUEST_NULL_GUARD && a < g_guest_mem_size - 8u;
    return a;
}

static void write_snapshot(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    static const uint8_t zero[4096];
    for (uint32_t a = GUEST_IMAGE_BASE; a < g_guest_mem_size && a >= GUEST_IMAGE_BASE; a += 4096u) {
        if (memcmp(g_mem + a, zero, 4096) == 0)
            continue;
        uint32_t hdr[2] = { a, 4096u };
        fwrite(hdr, 4, 2, f);
        fwrite(g_mem + a, 4096, 1, f);
    }
    fclose(f);
}

static int time_hit(double t, double dt, const double *list, int n, double *which)
{
    for (int i = 0; i < n; i++)
        if (t >= list[i] - dt * 0.5 && t < list[i] + dt * 0.5) {
            *which = list[i];
            return 1;
        }
    return 0;
}

static void apply_inputs(double t, double dt)
{
    for (int i = 0; i < n_inputs; i++) {
        Input *in = &inputs[i];
        int on = t >= in->t0 - 1e-9 && t < in->t1 - 1e-9;
        if (in->kind == 2 || in->kind == 3) {
            if (!on)
                continue;
            in->acc += in->rate * dt;
            int32_t d = (int32_t)in->acc;
            in->acc -= (double)d;
            if (in->kind == 2) {
                g_mouse.dx += d;
                g_mouse.x += d;
            } else {
                g_mouse.dy += d;
                g_mouse.y += d;
            }
            continue;
        }
        if (on == in->active)
            continue;
        in->active = on;
        if (in->kind == 1)
            g_mouse.buttons[0] = on ? 0x80u : 0u;
        else
            platform_key((uint8_t)in->code, on);
    }
}

void tick_test_scheduler(void)
{
    if (!tick_enabled() || !epoch)
        return;
    uint32_t clk = RD32(CLOCK_OBJ);
    uint32_t steps = RD32(clk + 0x34u), rate = RD32(clk + 0x3Cu);
    if (steps == last_steps || !rate)
        return;
    last_steps = steps;
    double dt = 1.0 / (double)rate;
    double t = (double)steps * dt;
    apply_inputs(t, dt);
    if (trace) {
        fprintf(trace, "%.4f %u", t, steps);
        for (int i = 0; i < n_watches; i++) {
            const Watch *w = &watches[i];
            int ok;
            uint32_t a = watch_addr(w, &ok);
            if (!ok) {
                fprintf(trace, " -");
                continue;
            }
            switch (w->type) {
            case 'f': {
                float v;
                memcpy(&v, g_mem + a, 4);
                fprintf(trace, " %.6g", (double)v);
                break;
            }
            case 'd': {
                double v;
                memcpy(&v, g_mem + a, 8);
                fprintf(trace, " %.9g", v);
                break;
            }
            case 'i':
                fprintf(trace, " %d", (int32_t)RD32(a));
                break;
            case 'h':
                fprintf(trace, " %d", (int16_t)RD16(a));
                break;
            default:
                fprintf(trace, " %08X", RD32(a));
                break;
            }
        }
        fprintf(trace, "\n");
    }
    double at;
    if (time_hit(t, dt, snaps, n_snaps, &at)) {
        char path[600];
        snprintf(path, sizeof(path), "%s_snap_%g.bin", prefix, at);
        write_snapshot(path);
        rt_log("tick test: snapshot %s (step %u)", path, steps);
    }
    if (time_hit(t, dt, shots, n_shots, &at)) {
        snprintf(shot_name, sizeof(shot_name), "%s_shot_%g.bmp", prefix, at);
        shot_pending = 1;
    }
    if (end_t >= 0.0 && t >= end_t) {
        if (trace)
            fclose(trace);
        rt_log("tick test: end at t %.3f (step %u)", t, steps);
        platform_shutdown();
        exit(0);
    }
}

const char *tick_test_shot_due(void)
{
    if (!shot_pending)
        return NULL;
    shot_pending = 0;
    return shot_name;
}

#else

int tick_test_enabled(void) { return 0; }
void tick_test_set_rate(uint32_t ret, uint32_t hz) { (void)ret; (void)hz; }
void tick_test_scheduler(void) {}
const char *tick_test_shot_due(void) { return NULL; }

#endif
