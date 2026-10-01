/* Game-specific behaviour fixes applied through entry hooks (rt_probe) that the
 * recompiler inserts into the functions listed in tools/recomp/gen.py GAME_HOOKS. */
#include "igi_fixes.h"
#include "runtime.h"

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
 * port also saves whenever a menu screen closes (the window goes from state 6,
 * closing, to 0 before the next screen opens), at most once every two seconds. */
#define WINDOW_STATE_CLOSING 6u
#define WINDOW_STATE_SWITCH 0u
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
        if (obj == ui_window && ui_state == WINDOW_STATE_CLOSING && state == WINDOW_STATE_SWITCH)
            save_config(c);   /* a menu screen closed (e.g. leaving the options) */
        ui_window = obj;
        ui_state = state;
        ui_frames = 0;
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
