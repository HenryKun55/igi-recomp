/* Gameplay update rate (see docs/tick_rate.md).
 *
 * The game's scheduler (0x402260) runs fixed update steps at the rate set by
 * 0x402820: 30 Hz in gameplay, 60 Hz in the menus. igi.cfg game_hz=N /
 * IGI_GAME_HZ=N replaces the gameplay 30 with N (30..240), and everything in the
 * game that counts in 1/30 s steps is kept at its 30 Hz real-time speed by the
 * recompiler's update-rate hooks (tools/recomp/tick_rate.py):
 *   - gated update handlers keep running 30 times a second of game time;
 *   - constant slots hold step-length dependent constants scaled to the rate.
 * At 30 Hz (no override) none of this is active and the game runs unchanged. */
#include "runtime.h"
#include "tick_rate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_OBJ 0x00567C8Cu        /* game clock object: +0x34 steps since SetRate */
#define CHILD_UPDATE 0x00488700u     /* updates an object's children (QTask list at +8) */
#define GATE_CHILDREN 1u
#define GATE_NOT_PLAYER 2u
#define TASK_TYPES 0x00AF5EE0u       /* task type table: 0x30 bytes per type, name first */
#define TASK_TYPE_COUNT 0x180u

/* SetRate call sites (return addresses) of the menu save/restore pair. */
#define RET_MENU_OPEN 0x00418CD3u    /* SetRate(60) after saving the current rate */
#define RET_MENU_CLOSE 0x00417AAFu   /* SetRate(saved rate) */
#define ORIGINAL_GAMEPLAY_HZ 30u

static int game_hz = -1;
static uint32_t cur_hz = ORIGINAL_GAMEPLAY_HZ;
static int gameplay;                 /* the current rate is the (overridden) gameplay rate */
static int saved_gameplay;           /* mode before the menu opened */
static int active;                   /* gameplay at a rate other than 30 */

uint32_t tick_game_hz(void)
{
    if (game_hz < 0) {
        game_hz = 0;
        const char *e = getenv("IGI_GAME_HZ");
        if (e)
            game_hz = atoi(e);
#if defined(__vita__)
        FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (strncmp(buf, "game_hz=", 8) == 0)
                    game_hz = atoi(buf + 8);
            fclose(f);
        }
#endif
        /* Below 30 Hz is not supported: gates only skip steps, never repeat them. */
        if (game_hz <= (int)ORIGINAL_GAMEPLAY_HZ)
            game_hz = 0;
        if (game_hz > 240)
            game_hz = 240;
        if (game_hz)
            rt_log("gameplay rate: %d Hz", game_hz);
    }
    return (uint32_t)game_hz;
}

/* Fills the constant slots for the current mode: the original values unless
 * gameplay runs at another rate. */
static void fill_slots(void)
{
    double ratio = active ? (double)ORIGINAL_GAMEPLAY_HZ / (double)cur_hz : 1.0;
    for (uint32_t i = 0; i < g_tick_slot_count; i++) {
        const TickSlot *s = &g_tick_slots[i];
        if (!s->addr)
            continue;
        uint64_t bits = s->bits;
        if (s->orig)
            memcpy(&bits, g_mem + s->orig, (size_t)s->size);
        if (active && s->exp) {
            double scale = pow(ratio, (double)s->exp);
            if (s->kind == 1 && s->size == 4) {
                float v;
                memcpy(&v, &bits, 4);
                v = (float)((double)v * scale);
                memcpy(&bits, &v, 4);
            } else if (s->kind == 1) {
                double v;
                memcpy(&v, &bits, 8);
                v *= scale;
                memcpy(&bits, &v, 8);
            } else {
                int32_t v = (int32_t)(uint32_t)bits;
                bits = (uint32_t)(int32_t)lround((double)v * scale);
            }
        }
        memcpy(g_mem + s->addr, &bits, 8);
    }
}

void tick_rate_init(void)
{
    tick_game_hz();
    fill_slots();
}

uint32_t tick_set_rate(uint32_t ret, uint32_t hz)
{
    uint32_t over = tick_game_hz();
    if (ret == RET_MENU_OPEN) {
        saved_gameplay = gameplay;
        gameplay = 0;
    } else if (ret == RET_MENU_CLOSE) {
        gameplay = saved_gameplay;
    } else {
        gameplay = hz == ORIGINAL_GAMEPLAY_HZ;
    }
    if (over && hz == ORIGINAL_GAMEPLAY_HZ)
        hz = over;
    cur_hz = hz;
    active = over && gameplay && hz != ORIGINAL_GAMEPLAY_HZ;
    fill_slots();
    return hz;
}

int tick_rate_active(void)
{
    return active;
}

/* Type id of the task type with that name, or 0xFFFF. */
static uint32_t task_type(const char *name)
{
    for (uint32_t t = 0; t < TASK_TYPE_COUNT; t++) {
        uint32_t e = TASK_TYPES + t * 0x30u, n = RD32(e);
        if (RD8(e + 0x25u) && n >= GUEST_IMAGE_BASE && n < g_guest_mem_size &&
            strncmp((const char *)g_mem + n, name, 64) == 0)
            return t;
    }
    return 0xFFFFu;
}

/* 30 Hz ticks that fall into the update step being run (0 or 1 above 30 Hz). */
static uint32_t ticks_this_step(void)
{
    uint32_t steps = RD32(RD32(CLOCK_OBJ) + 0x34u);
    return (uint32_t)(((uint64_t)(steps + 1u) * ORIGINAL_GAMEPLAY_HZ) / cur_hz -
                      ((uint64_t)steps * ORIGINAL_GAMEPLAY_HZ) / cur_hz);
}

int rt_tick_gate(Ctx *c, uint32_t fn, uint32_t esp, uint32_t flags, uint64_t *ret)
{
    (void)fn;
    if (!active || ticks_this_step())
        return 0;
    uint32_t node = RD32(esp + 4u);
    if (flags & GATE_NOT_PLAYER) {
        static uint32_t player_type;
        if (!player_type)
            player_type = task_type("HumanPlayer");
        if (RD16(node + 0x1Cu) == player_type)
            return 0;
    }
    /* Skipped step: the children still get their own update. */
    if (flags & GATE_CHILDREN) {
        c->r[4] = esp;
        guest_call(c, CHILD_UPDATE, 1, &node);
    }
    *ret = (uint64_t)(esp + 4u) << 32;
    return 1;
}
