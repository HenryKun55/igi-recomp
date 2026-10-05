/* Pad controls as keys of their own in IGI's key bindings (see igi_controls.h).
 *
 * How the game handles bindings (IGI.exe):
 *   - the keys a binding can use are a table of script constants at 0x536998,
 *     127 entries {char name[32]; u32 device << 30 | code} ("KEY_ESC" = keyboard
 *     DIK_ESCAPE); 0x406270 returns their number and 0x406240(i, &value, &name)
 *     entry i. Only the controls screen goes through these two;
 *   - the controls screen (task "ControlsMenu", update 0x4234E0) keeps for each
 *     action the index of its key in that table. While it waits for a key
 *     ([obj+0x359]) it takes the first entry whose key is down in the
 *     DirectInput keyboard state (or whose mouse button is pressed), Esc
 *     cancelling; a key already used by another action is swapped with it;
 *   - a key's name on the screen is the menu text "LOCAL:<language>/<constant
 *     name>" (menusystem.res), looked up by 0x418EA0(window, name); a missing
 *     text shows as "RM: <name>";
 *   - the player's bindings (one table per player, see shim/src/pad_input.c)
 *     are written to pc/config.qvm as GOInRemap(action, device, key) lines, the
 *     key by its constant name or, without one, as a number; play reads every
 *     entry (an action can have several), the controls screen the first one of
 *     each action;
 *   - the defaults are the 27 entries at 0x536410, used for new players, for
 *     actions missing from a config, and by "Reset to Default".
 *
 * The port adds the pad's virtual keys after the 127 entries (0x406270 and
 * 0x406240 replaced), names them by the pad's controls (0x418EA0 replaced for
 * those names) and, with the pad scheme, makes them the defaults and replaces
 * keyboard bindings (useless on a Vita) by them once, keeping a copy of the
 * player's config.qvm. */
#include "igi_controls.h"
#include "recomp.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RET(esp, eax) (((uint64_t)(esp) << 32) | (uint32_t)(eax))

#define ACTION_TABLE 0x00536558u    /* {char name[32]; u32 bit}, "MoveLeft" first */
#define KEY_TABLE 0x00536998u       /* {char name[32]; u32 device << 30 | code} */
#define KEY_COUNT 127u
#define DEFAULT_BINDINGS 0x00536410u
#define DEFAULT_COUNT 27u
#define ACTION_COUNT_FUNC 0x004062B0u   /* mov eax, <number of actions>; ret */
#define CURRENT_PLAYER 0x00BC2384u
#define PLAYER_BINDINGS 0x00BC25BCu
#define PLAYER_COUNT 0x00BC28BCu
#define PLAYER_STRIDE 0x564u
#define MAX_PLAYERS 8u
#define MAX_BINDINGS 64u
#define DEVICE_KEYBOARD 1u
#define LANG_ENGLISH 0u              /* language indices (runtime/src/igi_language.c) */
#define LANG_USA 5u
#define LANG_PORTUGUESE 6u
#define WINDOW_LANGUAGE 0x27E4u     /* language index in a menu window */

/* Controls screen object. */
#define LIST_X 0x20u
#define LIST_Y 0x24u
#define LIST_W 0x28u
#define LIST_H 0x2Cu
#define LIST_OFFSET 0x354u
#define LIST_CAPTURING 0x359u
#define LIST_RESET 0x35Au            /* "Reset to Default" confirmed: the next update resets */
#define GAME_LIST_APPLY 0x00423EB0u  /* apply(obj): the list into the player's bindings (OK) */
#define LIST_ROW_H 20
#define LIST_WINDOW 0x44u          /* its menu window */
#define WINDOW_MOUSE 0x27CCu       /* a menu window's mouse state: +0xC x, +0x10 y */
#define LIST_SEEN_FRAMES 2u

typedef struct VKey {
    uint8_t code;
    const char *id;      /* constant name (config.qvm stores the number) */
    const char *label[2];   /* shown in the controls screen: English, Portuguese (cp1252) */
} VKey;

static const VKey vkeys[] = {
    { IGI_VK_CROSS, "PAD_CROSS", { "Cross", "Xis" } },
    { IGI_VK_CIRCLE, "PAD_CIRCLE", { "Circle", "C\xEDrculo" } },
    { IGI_VK_SQUARE, "PAD_SQUARE", { "Square", "Quadrado" } },
    { IGI_VK_TRIANGLE, "PAD_TRIANGLE", { "Triangle", "Tri\xE2ngulo" } },
    { IGI_VK_L, "PAD_L", { "L", "L" } },
    { IGI_VK_R, "PAD_R", { "R", "R" } },
    { IGI_VK_SELECT, "PAD_SELECT", { "Select", "Select" } },
    { IGI_VK_DPAD_UP, "PAD_DPAD_UP", { "D-pad Up", "Direcional cima" } },
    { IGI_VK_DPAD_DOWN, "PAD_DPAD_DOWN", { "D-pad Down", "Direcional baixo" } },
    { IGI_VK_DPAD_LEFT, "PAD_DPAD_LEFT", { "D-pad Left", "Direcional esquerda" } },
    { IGI_VK_DPAD_RIGHT, "PAD_DPAD_RIGHT", { "D-pad Right", "Direcional direita" } },
    { IGI_VK_LSTICK_UP, "PAD_LSTICK_UP", { "Left stick Up", "Anal\xF3gico esq. cima" } },
    { IGI_VK_LSTICK_DOWN, "PAD_LSTICK_DOWN", { "Left stick Down", "Anal\xF3gico esq. baixo" } },
    { IGI_VK_LSTICK_LEFT, "PAD_LSTICK_LEFT", { "Left stick Left", "Anal\xF3gico esq. esquerda" } },
    { IGI_VK_LSTICK_RIGHT, "PAD_LSTICK_RIGHT", { "Left stick Right", "Anal\xF3gico esq. direita" } },
    { IGI_VK_REAR_UP, "PAD_REAR_UP", { "Rear touch Up", "Touch traseiro cima" } },
    { IGI_VK_REAR_DOWN, "PAD_REAR_DOWN", { "Rear touch Down", "Touch traseiro baixo" } },
    { IGI_VK_NONE, "PAD_NONE", { "Not set", "Nenhum" } },
};
#define N_VKEYS (sizeof(vkeys) / sizeof(vkeys[0]))

/* The pad scheme's defaults, by action bit; actions not listed: Not set. */
static const struct {
    uint32_t action;
    uint8_t code;
} pad_defaults[] = {
    { 0x00000001u, IGI_VK_LSTICK_LEFT },   /* MoveLeft */
    { 0x00000002u, IGI_VK_LSTICK_RIGHT },  /* MoveRight */
    { 0x00000004u, IGI_VK_LSTICK_UP },     /* MoveUp */
    { 0x00000008u, IGI_VK_LSTICK_DOWN },   /* MoveDown */
    { 0x00000100u, IGI_VK_R },             /* Fire */
    { 0x00000200u, IGI_VK_SQUARE },        /* Reload */
    { 0x00000400u, IGI_VK_CIRCLE },        /* Activate */
    { 0x00000800u, IGI_VK_L },             /* Crouch */
    { 0x00001000u, IGI_VK_CROSS },         /* Jump */
    { 0x00002000u, IGI_VK_TRIANGLE },      /* NextWeapon */
    { 0x00008000u, IGI_VK_SELECT },        /* Peek */
    { 0x00010000u, IGI_VK_DPAD_UP },       /* Binoculars */
    { 0x00020000u, IGI_VK_DPAD_RIGHT },    /* MapComputer */
    { 0x00040000u, IGI_VK_DPAD_DOWN },     /* ToggleWalkRun */
    { 0x00080000u, IGI_VK_DPAD_LEFT },     /* PrevWeapon */
    { 0x00100000u, IGI_VK_REAR_UP },       /* ZoomIn */
    { 0x00200000u, IGI_VK_REAR_DOWN },     /* ZoomOut */
};

/* Menu texts the pad scheme renames: English, Portuguese (other languages keep theirs). */
static const struct {
    const char *key, *text[2];
} pad_texts[] = {
    { "Invert Mouse", { "Invert Look", "Inverter mira" } },
    { "Mouse Sensitivity", { "Look Sensitivity", "Sensibilidade da mira" } },
    { "Select a key or button", { "Press a button", "Pressione um bot\xE3o" } },
};
#define N_PAD_TEXTS (sizeof(pad_texts) / sizeof(pad_texts[0]))

static int valid, scheme;
static uint32_t id_ptr[N_VKEYS], label_ptr[N_VKEYS][2], text_ptr[N_PAD_TEXTS][2];
static uint32_t list_obj, list_frames;   /* the screen's object, frames since its last update */

int igi_controls_pad_scheme(void)
{
    return scheme;
}

static int expected_exe(void)
{
    return memcmp(g_mem + ACTION_TABLE, "MoveLeft", 9) == 0 && memcmp(g_mem + KEY_TABLE, "KEY_ANGLE", 10) == 0 &&
           RD32(KEY_TABLE + 0x20u) == 0x40000056u && RD32(DEFAULT_BINDINGS) == 0x00000001u &&
           RD32(DEFAULT_BINDINGS + 4u) == DEVICE_KEYBOARD && RD32(DEFAULT_BINDINGS + 8u) == 0xCBu &&
           g_mem[ACTION_COUNT_FUNC] == 0xB8u;
}

static int is_vkey(uint32_t code)
{
    for (unsigned i = 0; i < N_VKEYS; i++)
        if (vkeys[i].code == code)
            return 1;
    return 0;
}

static uint8_t pad_default(uint32_t action)
{
    for (unsigned i = 0; i < sizeof(pad_defaults) / sizeof(pad_defaults[0]); i++)
        if (pad_defaults[i].action == action)
            return pad_defaults[i].code;
    return IGI_VK_NONE;
}

void igi_controls_init(void)
{
    valid = expected_exe();
    if (!valid) {
        rt_log("controls: binding tables not found, pad keys not added");
        return;
    }
#if defined(__vita__)
    scheme = 1;
#else
    const char *e = getenv("IGI_PAD_CONTROLS");
    scheme = e && *e && *e != '0';
#endif
    for (unsigned i = 0; i < N_VKEYS; i++) {
        id_ptr[i] = scratch_strdup(vkeys[i].id);
        label_ptr[i][0] = scratch_strdup(vkeys[i].label[0]);
        label_ptr[i][1] = scratch_strdup(vkeys[i].label[1]);
    }
    for (unsigned i = 0; i < N_PAD_TEXTS; i++)
        for (unsigned l = 0; l < 2; l++)
            text_ptr[i][l] = scratch_strdup(pad_texts[i].text[l]);
    if (scheme) {
        /* The game copies these into new players and uses them for "Reset to Default". */
        for (uint32_t i = 0; i < DEFAULT_COUNT; i++) {
            uint32_t e = DEFAULT_BINDINGS + i * 12u;
            WR32(e + 4u, DEVICE_KEYBOARD);
            WR32(e + 8u, pad_default(RD32(e)));
        }
    }
    rt_log("controls: %u pad keys added, pad scheme %s", (unsigned)N_VKEYS, scheme ? "on" : "off");
}

/* ---- replaced game functions ------------------------------------------------- */

/* 0x406270: int key_count(void) { return 127; } */
int rt_controls_key_count(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    if (!valid)
        return 0;
    c->r[1] = ecx;
    *ret = RET(esp + 4u, KEY_COUNT + N_VKEYS);
    return 1;
}

/* 0x406240: void key_entry(int i, u32 *value, const char **name) */
int rt_controls_key_entry(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    uint32_t i = RD32(esp + 4u) - KEY_COUNT;
    if (!valid || i >= N_VKEYS)
        return 0;   /* the game's own entries */
    uint32_t value = RD32(esp + 8u), name = RD32(esp + 12u);
    if (name)
        WR32(name, id_ptr[i]);
    if (value)
        WR32(value, DEVICE_KEYBOARD << 30 | vkeys[i].code);
    c->r[1] = ecx;
    *ret = RET(esp + 4u, value);
    return 1;
}

static int guest_streq(uint32_t p, const char *s)
{
    return p >= 0x10000u && memcmp(g_mem + p, s, strlen(s) + 1u) == 0;
}

/* 0x418EA0: const char *menu_text(window, const char *key) */
int rt_controls_text(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    if (!valid)
        return 0;
    uint32_t key = RD32(esp + 8u), window = RD32(esp + 4u), text = 0;
    uint32_t lang = window ? RD32(window + WINDOW_LANGUAGE) : 0u;
    unsigned l = lang == LANG_PORTUGUESE ? 1u : 0u;
    for (unsigned i = 0; i < N_VKEYS && !text; i++)
        if (key == id_ptr[i])
            text = label_ptr[i][l];
    if (!text && scheme && (lang == LANG_ENGLISH || lang == LANG_USA || lang == LANG_PORTUGUESE)) {
        for (unsigned i = 0; i < N_PAD_TEXTS && !text; i++)
            if (guest_streq(key, pad_texts[i].key))
                text = text_ptr[i][l];
    }
    if (!text)
        return 0;
    c->r[1] = ecx;
    *ret = RET(esp + 4u, text);
    return 1;
}

/* ---- keyboard bindings replaced by the pad's ------------------------------------ */

static void backup_config(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    char src[1100], dst[1100];
    snprintf(src, sizeof(src), "%s/config.qvm", g_game_dir);
    snprintf(dst, sizeof(dst), "%s/config_keyboard.qvm", g_game_dir);
    FILE *out = fopen(dst, "rb");
    if (out) {
        fclose(out);
        return;   /* kept from an earlier replacement */
    }
    FILE *in = fopen(src, "rb");
    if (!in)
        return;
    out = fopen(dst, "wb");
    if (out) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
            fwrite(buf, 1, n, out);
        fclose(out);
        rt_log("controls: keyboard bindings kept in config_keyboard.qvm");
    }
    fclose(in);
}

/* A player whose bindings use no pad key brought them from a keyboard (a PC
 * install, an earlier version of the port): they become the pad's defaults. */
static void replace_keyboard_bindings(void)
{
    for (uint32_t p = 0; p < MAX_PLAYERS; p++) {
        uint32_t table = PLAYER_BINDINGS + p * PLAYER_STRIDE, count_at = PLAYER_COUNT + p * PLAYER_STRIDE;
        uint32_t count = RD32(count_at);
        if (count == 0u || count > MAX_BINDINGS)
            continue;
        int pad = 0;
        for (uint32_t i = 0; i < count && !pad; i++)
            pad = RD32(table + i * 12u + 4u) == DEVICE_KEYBOARD && is_vkey(RD32(table + i * 12u + 8u));
        if (pad)
            continue;
        backup_config();
        memcpy(g_mem + table, g_mem + DEFAULT_BINDINGS, DEFAULT_COUNT * 12u);
        WR32(count_at, DEFAULT_COUNT);
        rt_log("controls: player %u had keyboard bindings (%u), replaced by the pad's defaults", p, count);
    }
}

uint8_t igi_controls_key_for(uint32_t action)
{
    if (!valid)
        return 0;
    uint32_t p = RD32(CURRENT_PLAYER);
    if (p >= MAX_PLAYERS)
        return 0;
    uint32_t table = PLAYER_BINDINGS + p * PLAYER_STRIDE, count = RD32(PLAYER_COUNT + p * PLAYER_STRIDE);
    for (uint32_t i = 0; i < count && i < MAX_BINDINGS; i++) {
        uint32_t e = table + i * 12u;
        if (RD32(e) == action && RD32(e + 4u) == DEVICE_KEYBOARD && RD32(e + 8u) > 0u && RD32(e + 8u) < 256u)
            return (uint8_t)RD32(e + 8u);
    }
    return 0;
}

/* ---- the controls screen's list ----------------------------------------------------- */

int igi_controls_list(IgiControlsList *out)
{
    if (!valid || !list_obj || list_frames > LIST_SEEN_FRAMES)
        return 0;
    out->capturing = g_mem[list_obj + LIST_CAPTURING] != 0;
    out->x = (int32_t)RD32(list_obj + LIST_X);
    out->y = (int32_t)RD32(list_obj + LIST_Y);
    out->w = (int32_t)RD32(list_obj + LIST_W);
    out->h = (int32_t)RD32(list_obj + LIST_H);
    out->row_h = LIST_ROW_H;
    out->rows = (int)RD32(ACTION_COUNT_FUNC + 1u);
    out->offset = (int)RD32(list_obj + LIST_OFFSET);
    uint32_t window = RD32(list_obj + LIST_WINDOW);
    out->cursor_x = window ? (int32_t)RD32(window + WINDOW_MOUSE + 0xCu) : 0;
    out->cursor_y = window ? (int32_t)RD32(window + WINDOW_MOUSE + 0x10u) : 0;
    return out->w > 0 && out->h >= LIST_ROW_H && out->rows > 0;
}

void igi_controls_list_scroll(int offset)
{
    IgiControlsList l;
    if (!igi_controls_list(&l))
        return;
    int max = l.rows - l.h / l.row_h;
    if (offset > max)
        offset = max;
    if (offset < 0)
        offset = 0;
    WR32(list_obj + LIST_OFFSET, (uint32_t)offset);
}

/* The list's bindings into the player's table, as the screen's OK does. */
static void apply_list(Ctx *c, uint32_t sp, uint32_t obj)
{
    const uint32_t saved_esp = c->r[4];
    c->r[4] = sp - 64u;   /* below the caller's frame */
    guest_call(c, GAME_LIST_APPLY, 1, &obj);
    c->r[4] = saved_esp;
}

void igi_controls_end_frame(void)
{
    if (list_frames <= LIST_SEEN_FRAMES)
        list_frames++;
}

void igi_controls_probe(Ctx *c, uint32_t func, uint32_t sp)
{
    (void)c;
    if (!valid)
        return;
    switch (func) {
    case 0x004234E0u: { /* controls screen update(obj) */
        static int was_capturing, reset_done;
        uint32_t obj = RD32(sp + 4u);
        int capturing = g_mem[obj + LIST_CAPTURING] != 0;
        if (obj != list_obj)
            was_capturing = reset_done = 0;
        /* With the pad scheme a change counts at once (the screen's Cancel
         * keeps it): Circle goes back like on a console, keeping the bindings. */
        if (scheme && ((was_capturing && !capturing) || reset_done))
            apply_list(c, sp, obj);
        reset_done = g_mem[obj + LIST_RESET] != 0;   /* this update resets the list */
        was_capturing = capturing;
        list_obj = obj;
        list_frames = 0;
        break;
    }
    case 0x00423DB0u:   /* controls screen: bindings into the list(obj) */
        if (scheme)
            replace_keyboard_bindings();
        break;
    case 0x004ED5E0u: { /* input: bind(port, count, table, ...) for play */
        if (!scheme)
            break;
        uint32_t table = RD32(sp + 12u);
        replace_keyboard_bindings();
        for (uint32_t p = 0; p < MAX_PLAYERS; p++)
            if (table == PLAYER_BINDINGS + p * PLAYER_STRIDE)
                WR32(sp + 8u, RD32(PLAYER_COUNT + p * PLAYER_STRIDE));
        break;
    }
    default:
        break;
    }
}
