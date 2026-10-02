/* Gameplay update rate (see docs/tick_rate.md).
 *
 * The game's scheduler (0x402260) runs fixed update steps at the rate set by
 * 0x402820: 30 Hz in gameplay, 60 Hz in the menus. igi.cfg game_hz=N /
 * IGI_GAME_HZ=N replaces the gameplay 30 with N (31..240), and everything in the
 * game that counts in 1/30 s steps is kept at its 30 Hz real-time speed by the
 * recompiler's update-rate hooks (tools/recomp/tick_rate.py):
 *   - gated update handlers of logic objects keep running 30 times a second;
 *   - constant slots hold step-length dependent constants scaled to the rate,
 *     some only in the player scope (the player's update, which runs every step
 *     so that movement and view are smooth);
 *   - global step counters advance on 30 Hz ticks, GetStepCount returns ticks.
 * At 30 Hz (no override) none of this is active and the game runs unchanged. */
#include "runtime.h"
#include "tick_rate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_OBJ 0x00567C8Cu        /* game clock object: +0x34 steps since SetRate */
#define CHILD_UPDATE 0x00488700u     /* updates an object's children (QTask list at +8) */
#define TASK_TYPES 0x00AF5EE0u       /* task type table: 0x30 bytes per type, name first */
#define TASK_TYPE_COUNT 0x180u
#define ANIM_STEP 0x00A54658u        /* animation time units (1/4800 s) per step, 4800/30 */
#define ANIM_UNITS_PER_SECOND 4800.0f

#define GATE_CHILDREN 1u
#define GATE_NOT_PLAYER 2u
#define GATE_PLAYER_SCOPE 4u

#define SCOPE_GAMEPLAY 0
#define SCOPE_PLAYER 1
#define MODE_POW 1

/* SetRate call sites (return addresses) of the menu save/restore pair. */
#define RET_MENU_OPEN 0x00418CD3u    /* SetRate(60) after saving the current rate */
#define RET_MENU_CLOSE 0x00417AAFu   /* SetRate(saved rate) */
#define ORIGINAL_GAMEPLAY_HZ 30u

static int game_hz = -1;
static uint32_t cur_hz = ORIGINAL_GAMEPLAY_HZ;
static int gameplay;                 /* the current rate is the (overridden) gameplay rate */
static int saved_gameplay;           /* mode before the menu opened */
static int active;                   /* gameplay at a rate other than 30 */
static int in_player_scope;
static uint32_t passthrough;         /* gated function being called by its own gate */

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

/* Slot value for the given scaling state. */
static uint64_t slot_value(const TickSlot *s, int scaled)
{
    uint64_t bits = s->bits;
    if (s->orig)
        memcpy(&bits, g_mem + s->orig, (size_t)s->size);
    if (!scaled)
        return bits;
    double ratio = (double)ORIGINAL_GAMEPLAY_HZ / (double)cur_hz;
    if (s->kind == 1) {
        double v;
        if (s->size == 4) {
            float f;
            memcpy(&f, &bits, 4);
            v = f;
        } else {
            memcpy(&v, &bits, 8);
        }
        v = s->mode == MODE_POW ? pow(v, ratio) : v * pow(ratio, (double)s->exp);
        if (s->size == 4) {
            float f = (float)v;
            memcpy(&bits, &f, 4);
        } else {
            memcpy(&bits, &v, 8);
        }
    } else {
        int32_t v = (int32_t)(uint32_t)bits;
        double d = s->mode == MODE_POW ? pow((double)v, ratio) : (double)v * pow(ratio, (double)s->exp);
        bits = (uint32_t)(int32_t)lround(d);
    }
    return bits;
}

/* Fills the slots of one scope (-1: all) for the current state. */
static void fill_slots(int scope)
{
    for (uint32_t i = 0; i < g_tick_slot_count; i++) {
        const TickSlot *s = &g_tick_slots[i];
        if (!s->addr || (scope >= 0 && s->scope != scope))
            continue;
        int scaled = active && (s->scope == SCOPE_GAMEPLAY || in_player_scope);
        uint64_t bits = slot_value(s, scaled);
        memcpy(g_mem + s->addr, &bits, 8);
    }
}

void tick_rate_init(void)
{
    tick_game_hz();
    fill_slots(-1);
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
    fill_slots(-1);
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

static int is_player(uint32_t node)
{
    static uint32_t player_type;
    if (!player_type)
        player_type = task_type("HumanPlayer");
    return RD16(node + 0x1Cu) == player_type;
}

static uint32_t steps_done(void)
{
    return RD32(RD32(CLOCK_OBJ) + 0x34u);
}

/* The update step being run is the first of a 30 Hz tick: the step where the
 * tick count floor(steps * 30 / rate) (GetStepCount below) changes, so gated
 * code and the step-count guards of ungated code (e.g. Train 0x42B700 running
 * its children once per count) agree on which step runs a tick. */
static uint32_t ticks_this_step(void)
{
    uint32_t steps = steps_done();
    if (!steps)
        return 1;
    return ((uint64_t)steps * ORIGINAL_GAMEPLAY_HZ) / cur_hz !=
           ((uint64_t)(steps - 1u) * ORIGINAL_GAMEPLAY_HZ) / cur_hz;
}

int rt_tick_now(void)
{
    return !active || ticks_this_step();
}

/* The player's update (its whole subtree: body, view, input, weapons) runs every
 * step with the player-scope constants scaled and the animation step set for
 * the rate, so animations it starts and advances keep their real-time speed. */
static void run_player_scope(Ctx *c, uint32_t fn, uint32_t esp, uint32_t node)
{
    uint32_t anim_step = RD32(ANIM_STEP);
    float f = ANIM_UNITS_PER_SECOND / (float)cur_hz;
    uint32_t scaled;
    memcpy(&scaled, &f, 4);
    in_player_scope = 1;
    fill_slots(SCOPE_PLAYER);
    WR32(ANIM_STEP, scaled);
    c->r[4] = esp;
    passthrough = fn;
    guest_call(c, fn, 1, &node);
    passthrough = 0;
    WR32(ANIM_STEP, anim_step);
    in_player_scope = 0;
    fill_slots(SCOPE_PLAYER);
}

int rt_tick_gate(Ctx *c, uint32_t fn, uint32_t esp, uint32_t flags, uint64_t *ret)
{
    if (!active || fn == passthrough)
        return 0;
    uint32_t node = RD32(esp + 4u);
    if ((flags & (GATE_NOT_PLAYER | GATE_PLAYER_SCOPE)) && is_player(node)) {
        if (!(flags & GATE_PLAYER_SCOPE) || in_player_scope)
            return 0;
        run_player_scope(c, fn, esp, node);
        *ret = (uint64_t)(esp + 4u) << 32;
        return 1;
    }
    if (ticks_this_step())
        return 0;
    /* Skipped step: the children still get their own update. */
    if (flags & GATE_CHILDREN) {
        c->r[4] = esp;
        guest_call(c, CHILD_UPDATE, 1, &node);
    }
    *ret = (uint64_t)(esp + 4u) << 32;
    return 1;
}

/* GetStepCount (0x4028B0): step-count timers (now + N steps, seconds * 30) and
 * once-per-step guards see 30 Hz ticks. */
int rt_tick_step_count(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c;
    (void)ecx;
    (void)eax;
    if (!active)
        return 0;
    uint32_t ticks = (uint32_t)(((uint64_t)steps_done() * ORIGINAL_GAMEPLAY_HZ) / cur_hz);
    *ret = ((uint64_t)(esp + 4u) << 32) | ticks;
    return 1;
}

/* Camera shake (0x465970, args: human, amplitude x, y, duration in steps): the
 * player's view fades it out per step. */
int rt_tick_camera_shake(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)c;
    (void)ecx;
    (void)eax;
    (void)ret;
    if (active && in_player_scope) {
        int32_t steps = (int32_t)RD32(esp + 0x10u);
        WR32(esp + 0x10u, (uint32_t)(int32_t)lround((double)steps * cur_hz / ORIGINAL_GAMEPLAY_HZ));
    }
    return 0;
}

/* Cutscene clock (CutScene 0x4F51D0): nTick [+0x64] counts 30 Hz ticks (it is
 * visible to the level scripts) and 0x4F5400 positions the cutscene (camera
 * splines, keys, end) at time nTick / [0xA76C54] (ticks per second, 30). At
 * another rate the call sees nTick and the ticks per second rescaled so that the
 * time also includes the steps run since the last tick: the camera moves every
 * step. A new nTick written by the call (the end of the cutscene) is converted back. */
#define CUTSCENE_TIME 0x004F5400u
#define CUTSCENE_TICKS_PER_SECOND 0x00A76C54u

int rt_tick_cutscene_time(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx;
    (void)eax;
    if (!active || passthrough == CUTSCENE_TIME)
        return 0;
    uint32_t obj = RD32(esp + 4u), rate = RD32(CUTSCENE_TICKS_PER_SECOND);
    if (rate != ORIGINAL_GAMEPLAY_HZ)
        return 0;
    uint32_t steps = steps_done();
    uint32_t tick = (uint32_t)(((uint64_t)steps * ORIGINAL_GAMEPLAY_HZ) / cur_hz);
    uint32_t first = (uint32_t)(((uint64_t)tick * cur_hz + ORIGINAL_GAMEPLAY_HZ - 1u) / ORIGINAL_GAMEPLAY_HZ);
    uint32_t sub = steps - first;              /* steps since this tick's first step */
    uint32_t orig = RD32(obj + 0x64u);
    int32_t n = (int32_t)orig;
    if (sub && n > 0)
        n--;                                   /* the tick's increment already happened */
    int32_t scaled = n * (int32_t)cur_hz + (int32_t)(sub * ORIGINAL_GAMEPLAY_HZ);
    WR32(obj + 0x64u, (uint32_t)scaled);
    WR32(CUTSCENE_TICKS_PER_SECOND, ORIGINAL_GAMEPLAY_HZ * cur_hz);
    c->r[4] = esp;
    passthrough = CUTSCENE_TIME;
    guest_call(c, CUTSCENE_TIME, 1, &obj);
    passthrough = 0;
    WR32(CUTSCENE_TICKS_PER_SECOND, rate);
    int32_t after = (int32_t)RD32(obj + 0x64u);
    WR32(obj + 0x64u, after == scaled ? orig : (uint32_t)(int32_t)lround((double)after / cur_hz));
    *ret = (uint64_t)(esp + 4u) << 32;
    return 1;
}
