/* Automated in-game benchmark.
 *
 * Enabled by IGI_BENCH=<seconds> (desktop) or ux0:data/igi/bench.txt containing
 * the seconds (Vita). The game then drives itself from boot into mission 1,
 * reacting to the UI window state (igi_ui_state) rather than to fixed timings so
 * it works at any speed:
 *   intro videos    Esc until the main menu (state 2) shows,
 *   main menu       Up, Up (the cursor rests on "Select Player"), Enter: "Play Project IGI",
 *   select mission  Enter (the first mission is preselected),
 *   briefing        Esc skips the in-engine cutscene (state 16),
 *   gameplay        once state 9 has held for a few seconds, a fixed camera and
 *                   movement pattern runs for the requested time while every
 *                   frame's timings are recorded.
 * The summary goes to bench_result.txt (ux0:data/igi/ on the Vita) and the log,
 * then the process exits. With IGI_PROFILE the profile covers the measured part only. */
#include "bench.h"
#include "igi_fixes.h"
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__vita__)
#define BENCH_CFG "ux0:data/igi/bench.txt"
#define BENCH_RESULT "ux0:data/igi/bench_result.txt"
#define BENCH_PLATFORM "vita"
#else
#define BENCH_RESULT "bench_result.txt"
#define BENCH_PLATFORM "desktop"
#endif

#define DIK_ESCAPE 0x01u
#define DIK_RETURN 0x1Cu
#define DIK_UP 0xC8u

#define UI_MENU_IDLE 2u        /* menu window idle (any menu screen) */
#define UI_CUTSCENE 16u
#define UI_GAMEPLAY 9u

#define SETTLE_FRAMES 10u      /* menu state must hold this long before a key */
#define KEY_HOLD_US 100000u    /* keys stay down at least this long ... */
#define KEY_HOLD_FRAMES 2u     /* ... and for this many frames */
#define GAMEPLAY_SETTLE_US 3000000u

extern uint64_t g_idle_us;      /* kernel32: waiting for the game's 30 fps cap */

extern uint32_t g_frame_serial;  /* render_gl.c: frames presented */

typedef struct Sample {
    float frame_ms, draw_ms, swap_ms, wait_ms;
    uint32_t draws, gpu_draws, verts;
} Sample;

typedef enum Phase {
    PH_BOOT,          /* intro videos: Esc until the main menu */
    PH_MAIN_MENU,     /* Up, Up, Enter */
    PH_WAIT_MISSIONS, /* leave the main menu, settle on the mission list */
    PH_MISSIONS,      /* Enter */
    PH_LOADING,       /* until the cutscene or gameplay */
    PH_SETTLE,        /* gameplay state held for GAMEPLAY_SETTLE_US */
    PH_RUN,
} Phase;

static int bench_secs = -1;
static Phase phase;
static uint64_t phase_start_us, start_us, run_start_us, last_us;
static uint32_t phase_frames, state_frames, last_state = IGI_UI_NONE, steps;

/* Key currently held by the driver. */
static uint8_t held_key;
static uint64_t held_since;
static uint32_t held_frames;

static Sample *samples;
static uint32_t n_samples, cap_samples;
static int moving_forward;
static double mouse_acc;
static uint64_t last_idle_us;

int bench_enabled(void)
{
    if (bench_secs < 0) {
        bench_secs = 0;
#if defined(__vita__)
        FILE *f = fopen(BENCH_CFG, "r");
        if (f) {
            char buf[32] = { 0 };
            if (fgets(buf, sizeof(buf), f))
                bench_secs = atoi(buf);
            fclose(f);
        }
#else
        const char *e = getenv("IGI_BENCH");
        if (e)
            bench_secs = atoi(e);
#endif
        if (bench_secs > 0)
            rt_log("bench: enabled, %d s of gameplay", bench_secs);
        else
            bench_secs = 0;
    }
    return bench_secs > 0;
}

static void set_phase(Phase p, uint64_t now)
{
    static const char *const names[] = { "boot", "main menu", "wait missions", "missions", "loading", "settle", "run" };
    rt_log("bench: phase %s (ui state %d, %.1f s, frame %u)", names[p], (int)igi_ui_state(),
           (double)(now - start_us) / 1e6, g_frame_serial);
    phase = p;
    phase_start_us = now;
    phase_frames = 0;
    steps = 0;
}

static void tap(uint8_t dik, uint64_t now)
{
    if (held_key)
        return;
    platform_key(dik, 1);
    held_key = dik;
    held_since = now;
    held_frames = 0;
}

static void release_keys(uint64_t now, int force)
{
    if (!held_key)
        return;
    if (!force && (++held_frames < KEY_HOLD_FRAMES || now - held_since < KEY_HOLD_US))
        return;
    platform_key(held_key, 0);
    held_key = 0;
}

static void set_forward(int on)
{
    if (on != moving_forward) {
        moving_forward = on;
        platform_key(DIK_UP, on);
    }
}

/* Camera and movement pattern, a function of wall time so every machine follows
 * the same path whatever its frame rate. A 20 s cycle: turn a full circle right,
 * walk forward, turn a full circle left while walking, stand still. */
#define TURN_UNITS 2400.0   /* mouse units for a full turn (with default sensitivity) */
static void run_pattern(double t, double dt)
{
    double c = t - 20.0 * (double)(int)(t / 20.0);
    double rate = 0.0;
    if (c < 8.0)
        rate = TURN_UNITS / 8.0;
    else if (c >= 11.0 && c < 19.0)
        rate = -TURN_UNITS / 8.0;
    set_forward((c >= 8.0 && c < 11.0) || (c >= 11.0 && c < 15.0));
    mouse_acc += rate * dt;
    int32_t dx = (int32_t)mouse_acc;
    mouse_acc -= (double)dx;
    g_mouse.dx += dx;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void finish(uint64_t now)
{
    set_forward(0);
    uint32_t n = n_samples;
    double secs = (double)(now - run_start_us) / 1e6;
    double sum_frame = 0, sum_draw = 0, sum_swap = 0, sum_wait = 0, sum_draws = 0, sum_gpu = 0, sum_verts = 0;
    float *ft = malloc(sizeof(float) * (n ? n : 1u));
    float *bt = malloc(sizeof(float) * (n ? n : 1u));
    for (uint32_t i = 0; i < n; i++) {
        const Sample *s = &samples[i];
        sum_frame += s->frame_ms;
        sum_draw += s->draw_ms;
        sum_swap += s->swap_ms;
        sum_wait += s->wait_ms;
        sum_draws += s->draws;
        sum_gpu += s->gpu_draws;
        sum_verts += s->verts;
        ft[i] = s->frame_ms;
        bt[i] = s->frame_ms - s->wait_ms;
    }
    qsort(ft, n, sizeof(float), cmp_float);
    qsort(bt, n, sizeof(float), cmp_float);
    double dn = n ? (double)n : 1.0;
#define PCT(a, p) (n ? (double)(a)[(uint32_t)((double)(n - 1u) * (p) + 0.5)] : 0.0)
    /* frame = game + draw + swap; the game part includes the frame rate cap's
     * wait, reported separately (busy = frame - wait: the fps without the cap). */
    double busy = (sum_frame - sum_wait) / dn;
    char line[512];
    snprintf(line, sizeof(line),
             "bench %s: %.1f s, %u frames, %.2f fps | frame ms avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f | "
             "busy ms avg %.2f p50 %.2f p95 %.2f (%.1f fps uncapped) | game %.2f (cap wait %.2f) draw %.2f swap %.2f ms | "
             "%.0f draws %.0f gpu draws %.0f verts",
             BENCH_PLATFORM, secs, n, n / (secs > 0 ? secs : 1.0), sum_frame / dn, PCT(ft, 0.50), PCT(ft, 0.95),
             PCT(ft, 0.99), PCT(ft, 1.0), busy, PCT(bt, 0.50), PCT(bt, 0.95), busy > 0 ? 1000.0 / busy : 0.0,
             (sum_frame - sum_draw - sum_swap) / dn, sum_wait / dn, sum_draw / dn, sum_swap / dn, sum_draws / dn,
             sum_gpu / dn, sum_verts / dn);
#undef PCT
    free(ft);
    free(bt);
    rt_log("%s", line);
    FILE *f = fopen(BENCH_RESULT, "w");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    } else {
        rt_log("bench: cannot write %s", BENCH_RESULT);
    }
#ifdef IGI_PROFILE
    rt_prof_flush();
#endif
    platform_shutdown();   /* exits the process on the Vita */
    exit(0);
}

void bench_frame(uint64_t now, uint64_t frame_us, uint64_t draw_us, uint64_t swap_us, uint32_t draws,
                 uint32_t gpu_draws, uint32_t verts)
{
    if (!bench_enabled())
        return;
    if (!start_us) {
        start_us = phase_start_us = last_us = now;
        set_phase(PH_BOOT, now);
    }
    double dt = (double)(now - last_us) / 1e6;
    last_us = now;
    uint32_t state = igi_ui_state();
    /* Esc must be up before gameplay starts, or it opens the pause menu there. */
    release_keys(now, held_key == DIK_ESCAPE && phase == PH_LOADING && state != UI_CUTSCENE);
    if (phase != PH_RUN)
        last_idle_us = g_idle_us;

    state_frames = state == last_state ? state_frames + 1u : 0u;
    last_state = state;
    phase_frames++;

    /* Heartbeat, so a stuck run shows where it is. */
    static uint64_t beat;
    if ((now - start_us) / 10000000u != beat) {
        beat = (now - start_us) / 10000000u;
        rt_log("bench: %.0f s, phase %d, ui state %d", (double)(now - start_us) / 1e6, (int)phase, (int)state);
    }

    switch (phase) {
    case PH_BOOT:
        if (state == UI_MENU_IDLE && state_frames >= SETTLE_FRAMES)
            set_phase(PH_MAIN_MENU, now);
        else if (state == IGI_UI_NONE && phase_frames % 30u == 0u)
            tap(DIK_ESCAPE, now);
        break;
    case PH_MAIN_MENU:
        /* Screen transitions are short: a few frames on a slow machine. */
        if (steps == 3u && state != UI_MENU_IDLE) {
            set_phase(PH_WAIT_MISSIONS, now);
            break;
        }
        if (held_key || state != UI_MENU_IDLE || state_frames < SETTLE_FRAMES || phase_frames % SETTLE_FRAMES)
            break;
        if (steps < 2u) {
            tap(DIK_UP, now);
            steps++;
        } else if (steps == 2u) {
            tap(DIK_RETURN, now);
            steps++;
        }
        break;
    case PH_WAIT_MISSIONS:
        /* The menu window cycles 6, 0, 5 while switching screens, then idles at 2. */
        if (state == UI_MENU_IDLE && state_frames >= SETTLE_FRAMES && !held_key)
            set_phase(PH_MISSIONS, now);
        break;
    case PH_MISSIONS:
        if (steps && state != UI_MENU_IDLE)
            set_phase(PH_LOADING, now);
        else if (!steps && !held_key && phase_frames >= SETTLE_FRAMES) {
            tap(DIK_RETURN, now);
            steps++;
        }
        break;
    case PH_LOADING:
        if (state == UI_GAMEPLAY)
            set_phase(PH_SETTLE, now);
        else if (state == UI_CUTSCENE && state_frames >= SETTLE_FRAMES &&
                 (state_frames - SETTLE_FRAMES) % 30u == 0u)
            tap(DIK_ESCAPE, now);
        break;
    case PH_SETTLE:
        if (state != UI_GAMEPLAY)
            set_phase(PH_LOADING, now);
        else if (now - phase_start_us >= GAMEPLAY_SETTLE_US && phase_frames >= 30u) {
            cap_samples = (uint32_t)bench_secs * 500u + 1024u;
            samples = calloc(cap_samples, sizeof(Sample));
            n_samples = 0;
            mouse_acc = 0;
#ifdef IGI_PROFILE
            rt_prof_reset();
#endif
            run_start_us = now;
            set_phase(PH_RUN, now);
        }
        break;
    case PH_RUN: {
        if (n_samples < cap_samples && samples) {
            Sample *s = &samples[n_samples++];
            s->frame_ms = (float)frame_us / 1000.0f;
            s->draw_ms = (float)draw_us / 1000.0f;
            s->swap_ms = (float)swap_us / 1000.0f;
            s->wait_ms = (float)(g_idle_us - last_idle_us) / 1000.0f;
            s->draws = draws;
            s->gpu_draws = gpu_draws;
            s->verts = verts;
        }
        last_idle_us = g_idle_us;
        double t = (double)(now - run_start_us) / 1e6;
        if (t >= (double)bench_secs)
            finish(now);
        run_pattern(t, dt);
        break;
    }
    }
}
