/* Game-specific behaviour fixes applied through entry hooks (rt_probe) that the
 * recompiler inserts into the functions listed in tools/recomp/gen.py GAME_HOOKS. */
#include "igi_fixes.h"
#include "runtime.h"

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
    if (++ui_frames > 2u)
        ui_state = IGI_UI_NONE;   /* no UI window drawn lately (videos, loading) */
}

uint32_t igi_ui_state(void)
{
    return ui_state;
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

/* Gameplay update/render rate override (0 = the game's own 30 Hz). */
static uint32_t game_hz(void)
{
    static int hz = -1;
    if (hz < 0) {
        hz = 0;
        const char *e = getenv("IGI_GAME_HZ");
        if (e)
            hz = atoi(e);
#if defined(__vita__)
        FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (strncmp(buf, "game_hz=", 8) == 0)
                    hz = atoi(buf + 8);
            fclose(f);
        }
#endif
        if (hz < 0 || hz > 240)
            hz = 0;
        if (hz)
            rt_log("gameplay rate: %d Hz", hz);
    }
    return (uint32_t)hz;
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
         * at 60). igi.cfg game_hz=N / IGI_GAME_HZ=N replaces the 30. Updates
         * stay fixed-step, so a rate the CPU can't sustain renders less often. */
        uint32_t hz = game_hz();
        if (hz && RD32(sp + 4u) == 30u)
            WR32(sp + 4u, hz);
        break;
    }
    case 0x00497700u:
        flushing_state = 0;
        break;
    case 0x00497860u:
        /* Called from the queue flush (0x49773D) with the node state of each entry. */
        flushing_state = RD32(sp) == 0x0049773Du ? RD32(sp + 4u) : flushing_state;
        break;
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
