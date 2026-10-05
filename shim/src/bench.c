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
 * then the process exits. With IGI_PROFILE the profile covers the measured part only.
 *
 * The current phase's name is kept in bench_phase.txt for external tools. With a
 * hold time (IGI_BENCH_HOLD=<seconds>, or a third number in bench.txt) the game
 * thread also sleeps that long, with "hold: <where>" in bench_phase.txt, when the main
 * menu shows, before the mission is started and when gameplay begins: the boot and the
 * mission load then stand apart in an external profiler (tools/profile_vita3k.sh).
 *
 * Mission and views (render comparisons between platforms): a fourth number in
 * bench.txt (IGI_BENCH_MISSION) picks the mission from the list (1 = first), and a
 * fifth (IGI_BENCH_VIEWS=k) replaces the movement pattern: the player stands still
 * and turns a full circle in k steps; each view is traced (per-draw log) and
 * captured as screenshot <view>, then the process exits. The run time is ignored.
 *
 * Frame breakdown: every measured frame also records the game's scheduler passes
 * that led to its present (update steps: count and time; the render step's time,
 * see bench_step) and the file reads and heap allocations since the previous
 * present. bench_frames.csv gets one line per frame, and the summary has a line
 * for the frames whose busy time reaches the slow threshold (a sixth number in
 * bench.txt, IGI_BENCH_SLOW=<ms>; default 16 ms). With IGI_PROFILE and a threshold
 * given, the profile keeps only the samples of those slow frames (a negative
 * threshold: of the frames faster than that, for comparisons). A seventh number
 * (IGI_BENCH_STILL=1) keeps the player still, facing the start direction (a
 * heavy view of mission 1), for profiles of slow frames only. An eighth number
 * (IGI_BENCH_SLOW_UPDATE=<ms>) makes the profile keep the frames whose update
 * steps took at least that long instead. */
#include "bench.h"
#include "igi_controls.h"
#include "igi_fixes.h"
#include "platform.h"
#include "runtime.h"
#include "tick_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__vita__)
#define BENCH_CFG "ux0:data/igi/bench.txt"
#define BENCH_RESULT "ux0:data/igi/bench_result.txt"
#define BENCH_RUNNING "ux0:data/igi/bench_running.txt"
#define BENCH_PHASE "ux0:data/igi/bench_phase.txt"
#define BENCH_FRAMES "ux0:data/igi/bench_frames.csv"
#define BENCH_PLATFORM "vita"
#else
#define BENCH_RESULT "bench_result.txt"
#define BENCH_RUNNING "bench_running.txt"
#define BENCH_PHASE "bench_phase.txt"
#define BENCH_FRAMES "bench_frames.csv"
#define BENCH_PLATFORM "desktop"
#endif

#define DIK_ESCAPE 0x01u
#define DIK_RETURN 0x1Cu
#define DIK_UP 0xC8u
#define DIK_DOWN 0xD0u

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
    /* Scheduler passes that led to this frame's present (see bench_step). */
    float upd_ms, rnd_ms;
    uint32_t upd_n;
    /* Events since the previous present (BenchCounters). */
    float read_ms;
    uint32_t opens, reads, read_bytes, allocs;
} Sample;

BenchCounters g_bench_ctr;

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
static int shot_every;          /* screenshot every N measured frames (0: none) */
static int hold_secs;           /* sleep at the phase boundaries (see the top) */
static int mission = 1;         /* mission list entry to start (1 = first) */
static int views;               /* views mode: number of traced views (0: movement pattern) */
static int still;               /* 1: no movement pattern, the view stays on the start direction */
static int slow_update_ms;      /* > 0: the profile keeps the frames whose update steps took that long */
static int slow_ms;             /* slow frame threshold, ms (0: SLOW_DEFAULT_MS, profile keeps every frame;
                                 * negative: profile keeps the frames faster than -slow_ms) */
#define SLOW_DEFAULT_MS 16
static int view, view_frames, view_pending;
#define VIEW_TURN_FRAMES 10u
#define VIEW_SETTLE_FRAMES 10u
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
            if (fgets(buf, sizeof(buf), f)) {
                char *end;
                bench_secs = (int)strtol(buf, &end, 10);
                shot_every = (int)strtol(end, &end, 10);   /* optional second number */
                hold_secs = (int)strtol(end, &end, 10);    /* and third */
                int m = (int)strtol(end, &end, 10);
                views = (int)strtol(end, &end, 10);
                slow_ms = (int)strtol(end, &end, 10);
                still = (int)strtol(end, &end, 10);
                slow_update_ms = (int)strtol(end, &end, 10);
                if (m > 0)
                    mission = m;
            }
            fclose(f);
        }
#else
        const char *e = getenv("IGI_BENCH");
        if (e)
            bench_secs = atoi(e);
        e = getenv("IGI_BENCH_SHOT");
        if (e)
            shot_every = atoi(e);
        e = getenv("IGI_BENCH_HOLD");
        if (e)
            hold_secs = atoi(e);
        e = getenv("IGI_BENCH_MISSION");
        if (e && atoi(e) > 0)
            mission = atoi(e);
        e = getenv("IGI_BENCH_VIEWS");
        if (e)
            views = atoi(e);
        e = getenv("IGI_BENCH_SLOW_UPDATE");
        if (e)
            slow_update_ms = atoi(e);
        e = getenv("IGI_BENCH_STILL");
        if (e)
            still = atoi(e);
        e = getenv("IGI_BENCH_SLOW");
        if (e)
            slow_ms = atoi(e);
#endif
        if (bench_secs > 0)
            rt_log("bench: enabled, %d s of gameplay, mission %d, %d views", bench_secs, mission, views);
        else
            bench_secs = 0;
    }
    return bench_secs > 0;
}

static void write_phase(const char *name)
{
    FILE *f = fopen(BENCH_PHASE, "w");
    if (f) {
        fprintf(f, "%s\n", name);
        fclose(f);
    }
}

/* Sleeps hold_secs on the game thread (no guest work for a profiler to see). */
static void hold(const char *name)
{
    if (hold_secs <= 0)
        return;
    char line[64];
    snprintf(line, sizeof(line), "hold: %s", name);
    rt_log("bench: %s, %d s", line, hold_secs);
    write_phase(line);
    usleep((useconds_t)hold_secs * 1000000u);
}

static void set_phase(Phase p, uint64_t now)
{
    static const char *const names[] = { "boot", "main menu", "wait missions", "missions", "loading", "settle", "run" };
    rt_log("bench: phase %s (ui state %d, %.1f s, frame %u)", names[p], (int)igi_ui_state(),
           (double)(now - start_us) / 1e6, g_frame_serial);
    if (p == PH_MAIN_MENU || p == PH_SETTLE)
        hold(names[p]);
    write_phase(names[p]);
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
    static uint8_t key;
    if (on != moving_forward) {
        moving_forward = on;
        if (on) {
            /* With the pad's bindings, forward is a pad key (left stick up by default). */
            key = igi_controls_pad_scheme() ? igi_controls_key_for(IGI_ACTION_MOVE_UP) : 0;
            if (!key)
                key = DIK_UP;
        }
        platform_key(key, on);
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

/* Views mode: turn, settle, trace and capture, view after view (see the top). */
static void run_views(void)
{
    if (view_pending) {
        if (g_trace_next_frame == 0) {   /* the traced frame was presented */
            view_pending = 0;
            view++;
            view_frames = 0;
        }
        return;
    }
    /* The turn is fed in small steps: the game limits the movement per frame. */
    int32_t step = (int32_t)(TURN_UNITS / (double)views) / (int32_t)VIEW_TURN_FRAMES;
    if (view > 0 && view_frames < (int)VIEW_TURN_FRAMES)
        g_mouse.dx += step;
    if (++view_frames >= (int)(VIEW_TURN_FRAMES + VIEW_SETTLE_FRAMES) && !g_trace_next_frame) {
        rt_log("bench: view %d", view);
        g_trace_next_frame = 1;
        view_pending = 1;
    }
}

/* Update steps since the last render step: they belong to the frame it presents. */
static uint64_t pend_upd_us;
static uint32_t pend_upd_n;

void bench_step(uint32_t kind, uint64_t us)
{
    if (phase != PH_RUN || !samples) {
        pend_upd_us = 0;
        pend_upd_n = 0;
        return;
    }
    if (kind & BENCH_STEP_UPDATE)
        pend_upd_n++;
    if (!(kind & BENCH_STEP_RENDER)) {
        pend_upd_us += us;
        return;
    }
    /* The render step presented the frame recorded last (bench_frame runs at its present). */
    if (n_samples) {
        Sample *s = &samples[n_samples - 1u];
        s->upd_ms += (float)pend_upd_us / 1000.0f;
        s->upd_n += pend_upd_n;
        s->rnd_ms += (float)us / 1000.0f;
    }
    pend_upd_us = 0;
    pend_upd_n = 0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* Averages over the frames whose busy time is at least min_ms (all frames: 0). */
static void breakdown(char *out, size_t size, const char *name, float min_ms)
{
    double n = 0, busy = 0, upd = 0, rnd = 0, upd_n = 0, draws = 0, reads = 0, kb = 0, read_ms = 0, allocs = 0,
           opens = 0;
    uint32_t hist[4] = { 0 };   /* frames with 0, 1, 2, 3+ update steps */
    for (uint32_t i = 0; i < n_samples; i++) {
        const Sample *s = &samples[i];
        float b = s->frame_ms - s->wait_ms;
        if (b < min_ms)
            continue;
        n++;
        busy += b;
        upd += s->upd_ms;
        rnd += s->rnd_ms;
        upd_n += s->upd_n;
        draws += s->draws;
        reads += s->reads;
        kb += s->read_bytes / 1024.0;
        read_ms += s->read_ms;
        allocs += s->allocs;
        opens += s->opens;
        hist[s->upd_n < 3u ? s->upd_n : 3u]++;
    }
    double d = n > 0 ? n : 1.0;
    snprintf(out, size,
             "%s: %.0f frames | busy %.2f = update %.2f (%.2f steps) + render %.2f + other %.2f ms | "
             "steps 0/1/2/3+: %u/%u/%u/%u | %.0f draws | %.2f opens %.2f reads %.1f KB %.2f ms | %.2f heap allocs",
             name, n, busy / d, upd / d, upd_n / d, rnd / d, (busy - upd - rnd) / d, hist[0], hist[1], hist[2],
             hist[3], draws / d, opens / d, reads / d, kb / d, read_ms / d, allocs / d);
}

static void write_frames(void)
{
    FILE *f = fopen(BENCH_FRAMES, "w");
    if (!f)
        return;
    fprintf(f, "frame,busy_ms,wait_ms,update_ms,update_steps,render_ms,draw_ms,swap_ms,draws,verts,opens,reads,"
               "read_bytes,read_ms,heap_allocs\n");
    for (uint32_t i = 0; i < n_samples; i++) {
        const Sample *s = &samples[i];
        fprintf(f, "%u,%.2f,%.2f,%.2f,%u,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u,%.2f,%u\n", i,
                (double)(s->frame_ms - s->wait_ms), (double)s->wait_ms, (double)s->upd_ms, s->upd_n,
                (double)s->rnd_ms, (double)s->draw_ms, (double)s->swap_ms, s->draws, s->verts, s->opens, s->reads,
                s->read_bytes, (double)s->read_ms, s->allocs);
    }
    fclose(f);
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
             "busy ms avg %.2f p50 %.2f p95 %.2f p99 %.2f (%.1f fps uncapped) | game %.2f (cap wait %.2f) draw %.2f swap %.2f ms | "
             "%.0f draws %.0f gpu draws %.0f verts",
             BENCH_PLATFORM, secs, n, n / (secs > 0 ? secs : 1.0), sum_frame / dn, PCT(ft, 0.50), PCT(ft, 0.95),
             PCT(ft, 0.99), PCT(ft, 1.0), busy, PCT(bt, 0.50), PCT(bt, 0.95), PCT(bt, 0.99), busy > 0 ? 1000.0 / busy : 0.0,
             (sum_frame - sum_draw - sum_swap) / dn, sum_wait / dn, sum_draw / dn, sum_swap / dn, sum_draws / dn,
             sum_gpu / dn, sum_verts / dn);
#undef PCT
    free(ft);
    free(bt);
    char all[512], slow[512], slow_name[32];
    breakdown(all, sizeof(all), "all frames", 0.0f);
    int thr = slow_ms > 0 ? slow_ms : slow_ms < 0 ? -slow_ms : SLOW_DEFAULT_MS;
    snprintf(slow_name, sizeof(slow_name), "busy >= %d ms", thr);
    breakdown(slow, sizeof(slow), slow_name, (float)thr);
    write_frames();
    rt_log("%s", line);
    rt_log("bench %s", all);
    rt_log("bench %s", slow);
    FILE *f = fopen(BENCH_RESULT, "w");
    if (f) {
        fprintf(f, "%s\n%s\n%s\n", line, all, slow);
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

int bench_shot_due(void)
{
    if (views > 0)
        return bench_enabled() && phase == PH_RUN && view_pending && g_trace_next_frame == 2 ? view : -1;
    if (!bench_enabled() || phase != PH_RUN || shot_every <= 0 || !samples || n_samples % (uint32_t)shot_every)
        return -1;
    return (int)n_samples;
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
        if (steps >= (uint32_t)mission && state != UI_MENU_IDLE)
            set_phase(PH_LOADING, now);
        else if (steps < (uint32_t)mission && !held_key && phase_frames >= SETTLE_FRAMES * (steps + 1u)) {
            if (steps + 1u < (uint32_t)mission) {
                tap(DIK_DOWN, now);
                steps++;
                break;
            }
            hold("mission start");
            tap(DIK_RETURN, now);
            steps = (uint32_t)mission;
        }
        break;
    case PH_LOADING:
        if (tick_test_enabled())
            break;   /* the update-rate harness takes over once a level loads */
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
            memset(&g_bench_ctr, 0, sizeof(g_bench_ctr));
#ifdef IGI_PROFILE
            rt_prof_reset(slow_ms != 0 || slow_update_ms > 0);
#endif
            run_start_us = now;
            set_phase(PH_RUN, now);
            /* Tells external tools (tools/profile_vita3k.sh) the measured part began:
             * Vita3K only writes igi.log out when the game exits. */
            FILE *f = fopen(BENCH_RUNNING, "w");
            if (f)
                fclose(f);
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
            s->opens = g_bench_ctr.file_opens;
            s->reads = g_bench_ctr.file_reads;
            s->read_bytes = g_bench_ctr.read_bytes;
            s->read_ms = (float)g_bench_ctr.read_us / 1000.0f;
            s->allocs = g_bench_ctr.heap_allocs;
#ifdef IGI_PROFILE
            float b = s->frame_ms - s->wait_ms;
            /* The update steps of this frame ran before its render step, now running. */
            if (slow_update_ms > 0)
                rt_prof_frame_end(pend_upd_us >= (uint64_t)slow_update_ms * 1000u);
            else
                rt_prof_frame_end(slow_ms == 0 || (slow_ms > 0 ? b >= (float)slow_ms : b < (float)-slow_ms));
#endif
        }
        memset(&g_bench_ctr, 0, sizeof(g_bench_ctr));
        last_idle_us = g_idle_us;
        if (views > 0) {
            run_views();
            if (view >= views)
                finish(now);
            break;
        }
        double t = (double)(now - run_start_us) / 1e6;
        if (t >= (double)bench_secs)
            finish(now);
        if (!still)
            run_pattern(t, dt);
        break;
    }
    }
}
