/* Game-specific behaviour fixes applied through entry hooks (rt_probe) that the
 * recompiler inserts into the functions listed in tools/recomp/gen.py GAME_HOOKS. */
#include "igi_fixes.h"
#include "igi_controls.h"
#include "bench.h"
#include "runtime.h"
#include "tick_rate.h"
#include "tick_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- cutscene window ------------------------------------------------------
 * During in-engine cutscenes a UI window (render function 0x418970) sits in
 * state 16 and its background (dark translucent fill, green gradient, title
 * line) is drawn over the whole letterboxed scene. It does not show up in the
 * original game; until the reason is understood, the port skips that window's
 * pre-transformed, untextured geometry while it is in that state. */

#define WINDOW_STATE_CUTSCENE 16u
#define WINDOW_NODE_OFFSET 0x266Cu   /* 2D queue node embedded in the window */
#define NODE_STATE_OFFSET 0xCu       /* argument passed to the per-node setup */

static uint32_t cutscene_window;     /* window object seen in the cutscene state */
static uint32_t ui_window, ui_state = IGI_UI_NONE;   /* last UI window rendered and its state */
static uint32_t ui_frames;           /* frames presented since a UI window was last rendered */
static uint32_t flushing_state;      /* node state of the 2D queue entry being drawn */
static uint32_t computer_frames = 3; /* frames presented since the map computer last updated */
#define COMPUTER_SEEN_FRAMES 2u

int igi_skip_untextured_2d(void)
{
    if (!cutscene_window || !flushing_state)
        return 0;
    if (flushing_state != cutscene_window + WINDOW_NODE_OFFSET + NODE_STATE_OFFSET)
        return 0;
    return RD32(cutscene_window + 0x28u) == WINDOW_STATE_CUTSCENE;
}

void igi_end_frame(void)
{
    flushing_state = 0;
    igi_controls_end_frame();
    if (++ui_frames > 2u)
        ui_state = IGI_UI_NONE;   /* no UI window drawn lately (videos, loading) */
    if (computer_frames <= COMPUTER_SEEN_FRAMES)
        computer_frames++;
}

uint32_t igi_ui_state(void)
{
    return ui_state;
}

int igi_computer_active(void)
{
    return computer_frames <= COMPUTER_SEEN_FRAMES;
}

/* ---- saving the options ---------------------------------------------------
 * The game writes its settings (pc/config.qvm, via 0x405BD0 "LOCAL:config.qsc")
 * only at a normal exit; the options screens just change them in memory. On the
 * Vita the app is closed with the PS button, so settings were never kept. The
 * port also saves whenever a menu screen is left: the window leaves state 2
 * (screen active) for 6 (main menu screen closing) or 14 (in-game menu switching
 * screens). At most once every two seconds. */
#define WINDOW_STATE_ACTIVE 2u
uint32_t win32_tick_count(void);   /* shim/src/kernel32.c */

#define GAME_SAVE_CONFIG 0x00405BD0u
#define GAME_CONFIG_PATH 0x005362F4u   /* "LOCAL:config.qsc" */

static void save_config(Ctx *c)
{
    static uint32_t last_ms;
    uint32_t now = win32_tick_count();
    if (last_ms && now - last_ms < 2000u)
        return;
    last_ms = now;
    if (memcmp(g_mem + GAME_CONFIG_PATH, "LOCAL:config.qsc", 17) != 0)
        return;   /* not the expected executable */
    const uint32_t arg = GAME_CONFIG_PATH;
    guest_call(c, GAME_SAVE_CONFIG, 1, &arg);
}

/* ---- idle time ------------------------------------------------------------
 * The main loop calls the scheduler (0x402260) over and over; a loop pass in
 * which it neither advanced the step counter ([*0x567C8C]+0x34) nor presented a
 * frame was spent waiting for the next fixed step. Benchmarks and the overlay
 * report busy time as frame time minus this. */
extern uint64_t g_idle_us;        /* shim/src/kernel32.c */
extern uint32_t g_frame_serial;   /* shim/src/render_gl.c: presented frames */
uint64_t win32_perf_counter(void);

static void scheduler_idle(void)
{
    static uint64_t last_us;
    static uint32_t last_steps, last_renders, last_frame;
    uint64_t now = win32_perf_counter();
    uint32_t g = RD32(0x00567C8Cu);
    uint32_t steps = g ? RD32(g + 0x34u) : 0u;
    uint32_t renders = g ? RD32(g + 0x38u) : 0u;
    if (last_us && steps == last_steps && g_frame_serial == last_frame)
        g_idle_us += now - last_us;
    else if (last_us && bench_enabled())
        /* The previous pass ran an update ([+0x34]), a render ([+0x38]) or both. */
        bench_step((steps != last_steps ? BENCH_STEP_UPDATE : 0u) |
                   (renders != last_renders ? BENCH_STEP_RENDER : 0u), now - last_us);
    last_us = now;
    last_steps = steps;
    last_renders = renders;
    last_frame = g_frame_serial;
}

/* ---- loading screen -------------------------------------------------------
 * 0x48A440(steps) puts the loading screen up, 0x48A620(screen, n) advances its
 * progress bar by n steps and 0x48A5D0(screen) takes it down. Only logged here
 * (load times); the pictures reach the screen through the front buffer Blts
 * (shim/src/ddraw.c). */
static void loading_screen(uint32_t func, uint32_t sp)
{
    static uint64_t start_us, last_us;
    static uint32_t done;
    uint64_t now = win32_perf_counter();
    if (func == 0x0048A440u) {
        start_us = last_us = now;
        done = 0;
        rt_log("loading screen: up, %u steps", RD32(sp + 4u));
    } else if (func == 0x0048A620u) {
        done += RD32(sp + 8u);
        if (done > 0u)
            rt_log("loading screen: step %u after %.2f s (+%.2f s)", done, (double)(now - start_us) / 1e6,
                   (double)(now - last_us) / 1e6);
        last_us = now;
    } else if (start_us) {
        rt_log("loading screen: down after %.2f s", (double)(now - start_us) / 1e6);
        start_us = 0;
    }
}

void rt_probe(Ctx *c, uint32_t func)
{
    uint32_t sp = c->r[4];
    switch (func) {
    case 0x00418970u: {
        uint32_t obj = RD32(sp + 4u);
        uint32_t state = RD32(obj + 0x28u);
        if (state == WINDOW_STATE_CUTSCENE)
            cutscene_window = obj;
        static int log_states = -1;
        if (log_states < 0)
            log_states = getenv("IGI_STATE_LOG") != NULL;
        if (log_states && (obj != ui_window || state != ui_state))
            rt_log("ui window %08X state %u", obj, state);
        if (obj == ui_window && ui_state == WINDOW_STATE_ACTIVE && state != WINDOW_STATE_ACTIVE)
            save_config(c);   /* a menu screen is being left (OK/back on the options, main or in-game menu) */
        ui_window = obj;
        ui_state = state;
        ui_frames = 0;
        break;
    }
    case 0x00402820u: {
        /* SetRate(hz): gameplay runs the game's update and render at 30 Hz (menus
         * at 60). igi.cfg game_hz=N / IGI_GAME_HZ=N replaces the 30 (see
         * runtime/src/tick_rate.c). Updates stay fixed-step, so a rate the CPU
         * can't sustain renders less often. */
        WR32(sp + 4u, tick_set_rate(RD32(sp), RD32(sp + 4u)));
        tick_test_set_rate(RD32(sp), RD32(sp + 4u));
        break;
    }
    case 0x00402260u:
        tick_test_scheduler();
        scheduler_idle();
        break;
    case 0x00497700u:
        flushing_state = 0;
        break;
    case 0x00497860u:
        /* Called from the queue flush (0x49773D) with the node state of each entry. */
        flushing_state = RD32(sp) == 0x0049773Du ? RD32(sp + 4u) : flushing_state;
        break;
    case 0x0048A440u:
    case 0x0048A620u:
    case 0x0048A5D0u:
        loading_screen(func, sp);
        break;
    case 0x0046AC30u:
        computer_frames = 0;   /* the map computer screen is updating */
        break;
    case 0x004234E0u:
        igi_controls_probe(c, func, sp);
        return;   /* every frame of the controls screen */
    case 0x00423DB0u:
    case 0x00423EB0u:
    case 0x004ED5E0u:
        igi_controls_probe(c, func, sp);
        break;
    case 0x004A53B3u:
    case 0x004B8810u:
        igi_language_probe(c, func, sp);
        return;   /* frequent: kept out of the probe log */
    default:
        break;
    }
    static int log_probes = -1;
    if (log_probes < 0)
        log_probes = getenv("IGI_PROBE_LOG") != NULL;
    if (log_probes)
        rt_log("probe %08X from %08X args %08X %08X %08X %08X", func, RD32(sp), RD32(sp + 4u), RD32(sp + 8u),
               RD32(sp + 12u), RD32(sp + 16u));
}
