/* Game-specific behaviour fixes applied through entry hooks (rt_probe) that the
 * recompiler inserts into the functions listed in tools/recomp/gen.py GAME_HOOKS. */
#include "igi_fixes.h"
#include "runtime.h"

#include <stdlib.h>

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
}

void rt_probe(Ctx *c, uint32_t func)
{
    uint32_t sp = c->r[4];
    switch (func) {
    case 0x00418970u: {
        uint32_t obj = RD32(sp + 4u);
        if (RD32(obj + 0x28u) == WINDOW_STATE_CUTSCENE)
            cutscene_window = obj;
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
