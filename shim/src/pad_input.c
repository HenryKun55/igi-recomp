/* Pad controls sent through the game's own bindings (see pad_input.h).
 *
 * The bindings live in guest memory, one table per player profile:
 *   0xBC2384                 index of the active player
 *   0xBC25BC + 0x564 * i     up to 64 entries {action bit, device, code}
 *   0xBC28BC + 0x564 * i     number of entries
 * GOInRemap(action, device, key) in pc/config.qvm fills them (0x4037C0), the
 * options screen edits them, and the game's defaults are the 27 entries at
 * 0x536410 (copied in at start-up by 0x405370; 0x4061B0 returns them for "reset
 * to defaults"). Players who brought a config.qvm with other bindings (a PC
 * install, their own keys) used to get controls that did nothing: the port sent
 * the default keys. */
#include "pad_input.h"
#include "igi_fixes.h"
#include "platform.h"
#include "runtime.h"

#include <string.h>

#define ACTION_TABLE 0x00536558u     /* {char name[32]; u32 bit}, "MoveLeft" first */
#define DEFAULT_BINDINGS 0x00536410u
#define DEFAULT_COUNT 27u
#define CURRENT_PLAYER 0x00BC2384u
#define PLAYER_BINDINGS 0x00BC25BCu
#define PLAYER_COUNT 0x00BC28BCu
#define PLAYER_STRIDE 0x564u
#define MAX_PLAYERS 8u               /* the tables before the next field; more than the menu offers */
#define MAX_BINDINGS 64u
#define UI_STATE_GAMEPLAY 9u
#define WHEEL_REPEAT_MS 100u

/* The executable is the one whose tables these addresses describe. */
static int tables_valid(void)
{
    static int valid = -1;
    if (valid < 0) {
        valid = memcmp(g_mem + ACTION_TABLE, "MoveLeft", 9) == 0 && RD32(DEFAULT_BINDINGS) == IGI_ACT_MOVE_LEFT &&
                RD32(DEFAULT_BINDINGS + 4u) == PAD_DEVICE_KEYBOARD && RD32(DEFAULT_BINDINGS + 8u) == 0xCBu;
        if (!valid)
            rt_log("pad: binding tables not found, controls use the default keys");
    }
    return valid;
}

static int usable(PadBinding b)
{
    if (b.device == PAD_DEVICE_KEYBOARD)
        return b.code > 0u && b.code < 256u;
    if (b.device == PAD_DEVICE_MOUSE)
        return b.code == 1u || b.code == 2u || b.code == 4u || b.code == 8u || b.code == 0x100u || b.code == 0x200u;
    return 0;
}

static PadBinding default_binding(uint32_t action)
{
    PadBinding b = { 0, 0 };
    for (uint32_t i = 0; i < DEFAULT_COUNT; i++) {
        uint32_t e = DEFAULT_BINDINGS + i * 12u;
        if (RD32(e) == action) {
            b.device = RD32(e + 4u);
            b.code = RD32(e + 8u);
            break;
        }
    }
    return b;
}

PadBinding pad_binding(uint32_t action)
{
    PadBinding none = { 0, 0 };
    if (!tables_valid())
        return none;
    PadBinding def = default_binding(action);
    uint32_t player = RD32(CURRENT_PLAYER);
    if (player >= MAX_PLAYERS)
        return def;
    uint32_t table = PLAYER_BINDINGS + player * PLAYER_STRIDE, count_at = PLAYER_COUNT + player * PLAYER_STRIDE;
    uint32_t count = RD32(count_at), slot = MAX_BINDINGS;
    if (count > MAX_BINDINGS)
        return def;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = table + i * 12u;
        if (RD32(e) != action)
            continue;
        PadBinding b = { RD32(e + 4u), RD32(e + 8u) };
        if (usable(b))
            return b;
        slot = i;   /* bound to the joypad (not emulated) or to nothing usable */
    }
    if (!usable(def) || !pad_in_gameplay())
        return def;   /* the table is only changed in play, never while the game sets it up */
    /* Nothing a keyboard or mouse can press: the default goes back in. */
    if (slot == MAX_BINDINGS && count < MAX_BINDINGS) {
        slot = count;
        WR32(count_at, count + 1u);
    }
    if (slot < MAX_BINDINGS) {
        uint32_t e = table + slot * 12u;
        WR32(e, action);
        WR32(e + 4u, def.device);
        WR32(e + 8u, def.code);
        rt_log("pad: action %08X had no keyboard or mouse binding, default (device %u, code %02X) restored", action,
               def.device, def.code);
    }
    return def;
}

/* ---- output: keys and mouse buttons, reference counted -------------------- */

static uint8_t key_refs[256];
static uint8_t mouse_refs[4];

void pad_key(uint8_t dik, int down)
{
    if (!dik)
        return;
    if (down) {
        if (key_refs[dik]++ == 0u && !g_key_state[dik])
            platform_key(dik, 1);
    } else if (key_refs[dik] && --key_refs[dik] == 0u && g_key_state[dik]) {
        platform_key(dik, 0);
    }
}

void pad_mouse_button(int b, int down)
{
    if (down ? mouse_refs[b]++ != 0u : (!mouse_refs[b] || --mouse_refs[b] != 0u))
        return;
    g_mouse.buttons[b] = down ? 0x80 : 0;
    if (b < 3) {
        static const uint32_t msgs[3][2] = { { 0x0202, 0x0201 }, { 0x0205, 0x0204 }, { 0x0208, 0x0207 } };
        user32_post_message(msgs[b][down ? 1 : 0], 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
    }
}

static void wheel_step(uint32_t code)
{
    g_mouse.dz += code == 0x100u ? 120 : -120;
}

/* ---- actions ---------------------------------------------------------------- */

#define ACTION_SLOTS 32
static PadBinding held[ACTION_SLOTS];   /* what each held action pressed (device 0: not held) */
static uint32_t wheel_next[ACTION_SLOTS];
static uint32_t last_ms;

static int action_slot(uint32_t action)
{
    for (int i = 0; i < ACTION_SLOTS; i++)
        if (action == 1u << i)
            return i;
    return -1;
}

static void output(PadBinding b, int down)
{
    if (b.device == PAD_DEVICE_KEYBOARD) {
        pad_key((uint8_t)b.code, down);
    } else if (b.device == PAD_DEVICE_MOUSE) {
        if (b.code >= 0x100u) {
            if (down)
                wheel_step(b.code);
        } else {
            pad_mouse_button(b.code == 1u ? 0 : b.code == 2u ? 1 : b.code == 4u ? 2 : 3, down);
        }
    }
}

void pad_action(uint32_t action, int down)
{
    int s = action_slot(action);
    if (s < 0)
        return;
    if (down) {
        if (held[s].device)
            return;
        PadBinding b = pad_binding(action);
        if (!usable(b))
            return;
        held[s] = b;
        wheel_next[s] = last_ms + WHEEL_REPEAT_MS * 3u;   /* a pause before the repeats, as a key does */
        output(b, 1);
    } else if (held[s].device) {
        output(held[s], 0);
        held[s].device = 0;
    }
}

void pad_tick(uint32_t now_ms)
{
    last_ms = now_ms;
    for (int s = 0; s < ACTION_SLOTS; s++) {
        if (held[s].device == PAD_DEVICE_MOUSE && held[s].code >= 0x100u && (int32_t)(now_ms - wheel_next[s]) >= 0) {
            wheel_step(held[s].code);
            wheel_next[s] = now_ms + WHEEL_REPEAT_MS;
        }
    }
}

int pad_in_gameplay(void)
{
    return igi_ui_state() == UI_STATE_GAMEPLAY;
}
