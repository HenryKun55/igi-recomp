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
 * During an in-engine cutscene (the mission 1/2 ANYA fly-over briefing and the
 * end-of-mission scene) a UI window (render function 0x418970) sits in state 16.
 * Its render walks the window's embedded 2D queue node (window+0x266c), which in
 * the original game is empty while the scene plays. In the port that node still
 * holds the in-game menu's "Game Paused" screen (menusystem/ingamemenu.qvm): the
 * dark translucent fill and green gradient band (untextured) plus the "Game
 * Paused"/menu item glyphs (textured, font3.fnt), all drawn as pre-transformed
 * 2D over the letterboxed scene. None of this shows in the original game.
 *
 * The scene itself, the letterbox bars and the subtitle line are drawn through
 * other paths, so the whole 2D queue node of the cutscene window is suppressed
 * while it is in that state -- not just the untextured part, which left the menu
 * text visible. igi_skip_cutscene_2d() is the single authority; the draw backend
 * (shim/src/d3d7.c) calls it for every pre-transformed 2D draw. */

#define WINDOW_STATE_CUTSCENE 16u
#define WINDOW_STATE_GAMEPLAY 9u     /* the gameplay HUD window */
#define WINDOW_NODE_OFFSET 0x266Cu   /* 2D queue node embedded in the window */
#define NODE_STATE_OFFSET 0xCu       /* argument passed to the per-node setup */

/* A menu window keeps its own mouse cursor at window+0x27CC (WINDOW_MOUSE):
 * +0xC x, +0x10 y, in game-screen coordinates (the same space as the widget
 * rects and the controls list, see runtime/src/igi_controls.c). The menu moves
 * it only by the relative DirectInput mouse motion (window update 0x418415 adds
 * the read delta each frame; it ignores any absolute position), and every menu
 * widget is hit-tested against it by the shared 0x4248A0. So the front touch
 * must drive it by posting a relative delta to reach the finger, not by jumping
 * an absolute position -- analogous to the map computer (see
 * shim/src/platform_vita.c front_menu_touch and igi_computer_active). */
#define WINDOW_MOUSE 0x27CCu
#define WINDOW_MOUSE_X 0xCu
#define WINDOW_MOUSE_Y 0x10u

/* Every menu window holds its focusable widgets as an inline array of object
 * pointers at window+0x3C, with the count at window+0xBC and the index of the
 * widget in focus at window+0xC0 (confirmed in IGI.exe: FUN_004186e0 sets focus
 * via *(window+0x3C+index*4), FUN_004187f0 clears the array and count on reset,
 * FUN_00418fe0 finds a widget's index to focus it when the cursor enters it).
 * Each widget keeps its screen rectangle at +0x20 x, +0x24 y, +0x28 w, +0x2C h
 * (the same fields the controls list uses) and a back-pointer to its window at
 * +0x44; the shared hit-test FUN_004248A0 flags it hovered (+0x4b)/focused
 * (+0x4d) when the window cursor is inside that rect. So moving the window
 * cursor onto a widget's centre focuses it and a click there presses it -- the
 * port drives D-pad menu navigation this way (shim/src/pad_input.c). */
#define WINDOW_WIDGETS 0x3Cu       /* inline array of widget object pointers */
#define WINDOW_WIDGET_COUNT 0xBCu  /* number of focusable widgets */
#define WINDOW_FOCUS_INDEX 0xC0u   /* index of the widget in focus */
#define WIDGET_X 0x20u
#define WIDGET_Y 0x24u
#define WIDGET_W 0x28u
#define WIDGET_H 0x2Cu
#define WIDGET_MAX 64u             /* a sane cap on a window's widget array */

static uint32_t cutscene_window;     /* window object seen in the cutscene state */
static uint32_t ui_window, ui_state = IGI_UI_NONE;   /* last UI window rendered and its state */
static uint32_t ui_frames;           /* frames presented since a UI window was last rendered */
static uint32_t flushing_state;      /* node state of the 2D queue entry being drawn */
static uint32_t computer_frames = 3; /* frames presented since the map computer last updated */
#define COMPUTER_SEEN_FRAMES 2u

int igi_skip_cutscene_2d(void)
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

/* The current menu window's own mouse cursor (window+0x27CC+0xC/+0x10), in
 * game-screen coordinates, when a menu screen is up (the last window rendered is
 * a live menu window, not gameplay, a cutscene or none). 1 on success. Used by
 * the front touch to move the cursor to the touched point by a relative delta
 * (shim/src/platform_vita.c), so a tap lands exactly where the finger is with no
 * drift, whatever the resolution or aspect. */
int igi_menu_cursor(int32_t *x, int32_t *y)
{
    if (!ui_window || ui_state == IGI_UI_NONE || ui_state == WINDOW_STATE_GAMEPLAY ||
        ui_state == WINDOW_STATE_CUTSCENE)
        return 0;
    *x = (int32_t)RD32(ui_window + WINDOW_MOUSE + WINDOW_MOUSE_X);
    *y = (int32_t)RD32(ui_window + WINDOW_MOUSE + WINDOW_MOUSE_Y);
    return 1;
}

/* Press a focusable widget the way the game's own keyboard focus does: a widget
 * fires when its command field (+0x50) is -7 on the next update (FUN_00421070
 * checks exactly that, and FUN_00418580 sets it when Enter/Space activates the
 * focused widget). The D-pad walks the window's widgets and presses the one it is
 * on this way (shim/src/pad_input.c), which is exact -- it needs no pixel-perfect
 * cursor on a small button and so cannot land on the wrong one. `widget` must be
 * one of the objects igi_menu_widgets returned for the live window; 0 is ignored. */
#define WIDGET_COMMAND 0x50u
#define WIDGET_CMD_ACTIVATE 0xFFFFFFF9u   /* -7: "activate this widget" */
void igi_menu_activate_widget(uint32_t widget)
{
    int32_t cx, cy;
    if (!widget || !igi_menu_cursor(&cx, &cy))
        return;
    /* Confirm the object is still a focusable widget of the live window. */
    uint32_t count = RD32(ui_window + WINDOW_WIDGET_COUNT);
    if (count > WIDGET_MAX)
        return;
    for (uint32_t i = 0; i < count; i++)
        if (RD32(ui_window + WINDOW_WIDGETS + i * 4u) == widget) {
            WR32(widget + WIDGET_COMMAND, WIDGET_CMD_ACTIVATE);
            return;
        }
}

/* The current menu window's focusable widgets and their screen rectangles, for
 * the pad's D-pad navigation (shim/src/pad_input.c). Fills up to `max` rects in
 * document order (the window's own order, top to bottom on every stock screen),
 * returns the number of widgets, and reports the window's current focus index.
 * 0 when no menu window is up. A widget with a zero or negative size is skipped
 * (a hidden or placeholder entry), so the pad only lands on real, clickable
 * items. */
int igi_menu_widgets(IgiMenuWidget *out, int max, int *focus)
{
    int32_t cx, cy;
    if (!igi_menu_cursor(&cx, &cy))
        return 0;
    uint32_t count = RD32(ui_window + WINDOW_WIDGET_COUNT);
    if (count == 0u || count > WIDGET_MAX)
        return 0;
    int focus_idx = (int)RD32(ui_window + WINDOW_FOCUS_INDEX);
    int n = 0;
    for (uint32_t i = 0; i < count && n < max; i++) {
        uint32_t w = RD32(ui_window + WINDOW_WIDGETS + i * 4u);
        if (!w)
            continue;
        int32_t ww = (int32_t)RD32(w + WIDGET_W), wh = (int32_t)RD32(w + WIDGET_H);
        if (ww <= 0 || wh <= 0)
            continue;   /* hidden / placeholder */
        out[n].widget = w;
        out[n].index = (int)i;
        out[n].x = (int32_t)RD32(w + WIDGET_X);
        out[n].y = (int32_t)RD32(w + WIDGET_Y);
        out[n].w = ww;
        out[n].h = wh;
        n++;
    }
    if (focus)
        *focus = focus_idx;
    return n;
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

/* TEMP DIAGNOSTIC: dump the mission-select window so the ScrollListBox object,
 * its rect and its selected-index field can be found empirically. Enabled by
 * IGI_MISSION_DUMP=1. Removed before the final change. */
static void mission_dump(uint32_t window)
{
    static int on = -1;
    if (on < 0)
        on = getenv("IGI_MISSION_DUMP") != NULL;
    if (!on)
        return;
    static uint32_t last;
    static int frames;
    if (window != last) {
        last = window;
        frames = 0;
    }
    if (frames++ % 60u != 0u)
        return;
    uint32_t count = RD32(window + WINDOW_WIDGET_COUNT);
    rt_log("[mdump] window %08X state %u widgetcount %u focus %u cursor %d,%d | g539560=%d g539564=%d",
           window, RD32(window + 0x28u), count, RD32(window + WINDOW_FOCUS_INDEX),
           (int32_t)RD32(window + 0x27CCu + 0xCu), (int32_t)RD32(window + 0x27CCu + 0x10u),
           (int32_t)RD32(0x00539560u), (int32_t)RD32(0x00539564u));
    for (uint32_t i = 0; i < count && i < WIDGET_MAX; i++) {
        uint32_t w = RD32(window + WINDOW_WIDGETS + i * 4u);
        if (w)
            rt_log("[mdump]  widget[%u] %08X rect %d,%d %dx%d cmd %08X", i, w, (int32_t)RD32(w + WIDGET_X),
                   (int32_t)RD32(w + WIDGET_Y), (int32_t)RD32(w + WIDGET_W), (int32_t)RD32(w + WIDGET_H),
                   RD32(w + 0x50u));
    }
    /* window+0x30 is the screen's root container task (rect 0,0 640x480). Dump its
     * first 0x120 bytes as dwords so its child pointers (one being the
     * ScrollListBox) can be read; then follow each pointer-looking field one level
     * and print its rect, so the list-sized rect (x~140..315, y~210) stands out. */
    /* Every widget/task of this window keeps a back-pointer to the window at +0x44
     * (prior RE). Scan the heap around the window's own address for every object
     * whose +0x44 == window and print its rect plus candidate index fields, so the
     * ScrollListBox (a tall list-sized rect, x~140..315) and its selected-index
     * field are revealed. */
    uint32_t lo = window > 0x20000u ? window - 0x20000u : 0x02000000u;
    uint32_t hi = window + 0x20000u;
    if (hi > 0x03000000u)
        hi = 0x03000000u;
    for (uint32_t a = lo & ~3u; a < hi; a += 4u) {
        if (RD32(a + 0x44u) != window)
            continue;
        int32_t x = (int32_t)RD32(a + 0x20u), y = (int32_t)RD32(a + 0x24u);
        int32_t w = (int32_t)RD32(a + 0x28u), h = (int32_t)RD32(a + 0x2Cu);
        if (w <= 0 || w >= 640 || h <= 0 || h >= 480)
            continue;
        /* The mission ScrollListBox: a tall list at x~140, h~230 with the vtable's
         * item array at +0x180 ({index,nameptr} entries). Narrow to it. */
        if (x >= 130 && x <= 150 && w >= 150 && w <= 200 && h >= 200) {
            rt_log("[mdump]  SCROLLBOX %08X rect %d,%d %dx%d full dump:", a, x, y, w, h);
            for (uint32_t off = 0u; off < 0x200u; off += 0x10u)
                rt_log("[mdump]   +%03X: %08X %08X %08X %08X", off, RD32(a + off), RD32(a + off + 4u),
                       RD32(a + off + 8u), RD32(a + off + 0xCu));
            uint32_t vt = RD32(a);
            for (uint32_t s = 0u; s < 0x40u; s += 4u)
                rt_log("[mdump]   vt+%02X: %08X", s, RD32(vt + s));
            /* IGI_SB_SET=OFF:VAL writes VAL (decimal) to this object+OFF (hex) every
             * dumped frame, to find which field is nActiveID (the preview changes). */
            static int parsed;
            static uint32_t set_off;
            static int32_t set_val;
            static int set_on;
            if (!parsed) {
                parsed = 1;
                const char *e = getenv("IGI_SB_SET");
                if (e) {
                    set_off = (uint32_t)strtoul(e, NULL, 16);
                    const char *c2 = strchr(e, ':');
                    if (c2) {
                        set_val = atoi(c2 + 1);
                        set_on = 1;
                    }
                }
            }
            if (set_on) {
                WR32(a + set_off, (uint32_t)set_val);
                rt_log("[mdump]  SET +%03X = %d", set_off, set_val);
            }
        }
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
        if (state == WINDOW_STATE_ACTIVE)
            mission_dump(obj);
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
    case 0x00402FF0u:
        vita_menu_probe(c, func, sp);
        break;   /* once at start-up: register the Vita options script functions */
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
