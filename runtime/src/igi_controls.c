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
 * player's config.qvm.
 *
 * With the pad scheme an action can also have a second control: the screen's
 * list keeps the first one of each action ([obj+0x254], the key cell's text
 * [obj+0x154]), the port keeps the second (second[], loaded with the list) and
 * shows both in the key cell ("R / Rear touch Up", 0x423770 replaced). A click
 * on a row (Cross) sets the first control as before; Triangle on a row starts
 * the same key capture for the second one: a control already in that row
 * removes the second, a control of another action is swapped with it (as the
 * game swaps first controls). Applying the list (0x423EB0) writes the second
 * controls as extra GOInRemap entries after the first ones, which play reads
 * like any other entry; "Reset to Default" clears them. */
#include "igi_controls.h"
#include "recomp.h"
#include "runtime.h"
#include "vita_menu.h"

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
/* The game's language ids (runtime/src/igi_language.c): ENGLISH GERMAN FRENCH
 * ITALIAN SPANISH USA, 0..5, plus the port's PORTUGUESE at 6. */
#define LANG_ENGLISH 0u
#define LANG_GERMAN 1u
#define LANG_FRENCH 2u
#define LANG_ITALIAN 3u
#define LANG_SPANISH 4u
#define LANG_USA 5u
#define LANG_PORTUGUESE 6u
#define WINDOW_LANGUAGE 0x27E4u     /* language index in a menu window */

/* Column of the name tables below for each game language (USA shares English). */
#define L_EN 0
#define L_PT 1
#define L_FR 2
#define L_DE 3
#define L_IT 4
#define L_ES 5
#define N_LANGS 6

static unsigned lang_col(uint32_t lang)
{
    switch (lang) {
    case LANG_PORTUGUESE: return L_PT;
    case LANG_FRENCH:     return L_FR;
    case LANG_GERMAN:     return L_DE;
    case LANG_ITALIAN:    return L_IT;
    case LANG_SPANISH:    return L_ES;
    default:              return L_EN;   /* ENGLISH, USA, anything else */
    }
}

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
#define LIST_KEY_TEXT 0x154u       /* per row: the key cell's text object */
#define LIST_KEY 0x254u            /* per row: index of its key among the bindable keys */
#define LIST_SELECTED 0x35Cu       /* the row whose key is being captured */
#define MAX_ROWS 64u
#define KEY_ENTRY_SIZE 36u
#define KEY_STATE 0x00C28C62u      /* DirectInput keyboard state, by scan code */
#define DIK_ESCAPE 0x01u
#define GAME_SET_TEXT 0x004B6E90u  /* set_text(text object, const char *): copies it */
#define GAME_MENU_TEXT 0x00418EA0u
#define GAME_PLAY_SOUND 0x004E7180u
#define GAME_MOUSE_RESET 0x0048A8C0u
#define GAME_WINDOW_IDLE 0x00418FD0u   /* window(obj): back from capturing a key */
#define TEXT_SELECT_KEY 0x0053B8D0u    /* "Select a key or button" */
#define SOUND_SELECT 0x0053B5D8u       /* "menu_sel2" */

/* Each name has a column per language (L_EN L_PT L_FR L_DE L_IT L_ES); accented
 * letters are cp1252, the encoding of the game's menu fonts. */
typedef struct VKey {
    uint8_t code;
    const char *id;             /* constant name (config.qvm stores the number) */
    const char *label[N_LANGS]; /* shown in the controls screen */
    const char *brief[N_LANGS]; /* the same, shorter, in a cell with two controls */
} VKey;

static const VKey vkeys[] = {
    { IGI_VK_CROSS, "PAD_CROSS",
      { "Cross", "Xis", "Croix", "Kreuz", "Croce", "Cruz" },
      { "Cross", "Xis", "Croix", "Kreuz", "Croce", "Cruz" } },
    { IGI_VK_CIRCLE, "PAD_CIRCLE",
      { "Circle", "C\xEDrculo", "Rond", "Kreis", "Cerchio", "C\xEDrculo" },
      { "Circle", "C\xEDrculo", "Rond", "Kreis", "Cerchio", "C\xEDrculo" } },
    { IGI_VK_SQUARE, "PAD_SQUARE",
      { "Square", "Quadrado", "Carr\xE9", "Viereck", "Quadrato", "Cuadrado" },
      { "Square", "Quadrado", "Carr\xE9", "Viereck", "Quadrato", "Cuadrado" } },
    { IGI_VK_TRIANGLE, "PAD_TRIANGLE",
      { "Triangle", "Tri\xE2ngulo", "Triangle", "Dreieck", "Triangolo", "Tri\xE1ngulo" },
      { "Triangle", "Tri\xE2ngulo", "Triangle", "Dreieck", "Triangolo", "Tri\xE1ngulo" } },
    { IGI_VK_L, "PAD_L",
      { "L", "L", "L", "L", "L", "L" }, { "L", "L", "L", "L", "L", "L" } },
    { IGI_VK_R, "PAD_R",
      { "R", "R", "R", "R", "R", "R" }, { "R", "R", "R", "R", "R", "R" } },
    { IGI_VK_SELECT, "PAD_SELECT",
      { "Select", "Select", "Select", "Select", "Select", "Select" },
      { "Select", "Select", "Select", "Select", "Select", "Select" } },
    { IGI_VK_DPAD_UP, "PAD_DPAD_UP",
      { "D-pad Up", "Direcional cima", "Pad dir. haut", "Steuerkreuz hoch", "Freccia su", "Cruceta arriba" },
      { "D-pad Up", "Dir. cima", "Pad haut", "Kreuz hoch", "Freccia su", "Cruceta arr." } },
    { IGI_VK_DPAD_DOWN, "PAD_DPAD_DOWN",
      { "D-pad Down", "Direcional baixo", "Pad dir. bas", "Steuerkreuz runter", "Freccia gi\xF9",
        "Cruceta abajo" },
      { "D-pad Down", "Dir. baixo", "Pad bas", "Kreuz runter", "Freccia gi\xF9", "Cruceta ab." } },
    { IGI_VK_DPAD_LEFT, "PAD_DPAD_LEFT",
      { "D-pad Left", "Direcional esquerda", "Pad dir. gauche", "Steuerkreuz links", "Freccia sinistra",
        "Cruceta izquierda" },
      { "D-pad Left", "Dir. esq.", "Pad gauche", "Kreuz links", "Freccia sin.", "Cruceta izq." } },
    { IGI_VK_DPAD_RIGHT, "PAD_DPAD_RIGHT",
      { "D-pad Right", "Direcional direita", "Pad dir. droite", "Steuerkreuz rechts", "Freccia destra",
        "Cruceta derecha" },
      { "D-pad Right", "Dir. dir.", "Pad droite", "Kreuz rechts", "Freccia des.", "Cruceta der." } },
    { IGI_VK_LSTICK_UP, "PAD_LSTICK_UP",
      { "Left stick Up", "Anal\xF3gico esq. cima", "Stick gauche haut", "Linker Stick hoch",
        "Levetta sin. su", "Stick izq. arriba" },
      { "Stick Up", "Anal. cima", "Stick haut", "Stick hoch", "Lev. su", "Stick arr." } },
    { IGI_VK_LSTICK_DOWN, "PAD_LSTICK_DOWN",
      { "Left stick Down", "Anal\xF3gico esq. baixo", "Stick gauche bas", "Linker Stick runter",
        "Levetta sin. gi\xF9", "Stick izq. abajo" },
      { "Stick Down", "Anal. baixo", "Stick bas", "Stick runter", "Lev. gi\xF9", "Stick ab." } },
    { IGI_VK_LSTICK_LEFT, "PAD_LSTICK_LEFT",
      { "Left stick Left", "Anal\xF3gico esq. esquerda", "Stick gauche gauche", "Linker Stick links",
        "Levetta sin. sinistra", "Stick izq. izquierda" },
      { "Stick Left", "Anal. esq.", "Stick gauche", "Stick links", "Lev. sin.", "Stick izq." } },
    { IGI_VK_LSTICK_RIGHT, "PAD_LSTICK_RIGHT",
      { "Left stick Right", "Anal\xF3gico esq. direita", "Stick gauche droite", "Linker Stick rechts",
        "Levetta sin. destra", "Stick izq. derecha" },
      { "Stick Right", "Anal. dir.", "Stick droite", "Stick rechts", "Lev. des.", "Stick der." } },
    { IGI_VK_REAR_UP, "PAD_REAR_UP",
      { "Rear touch Up", "Touch traseiro cima", "Toucher arri\xE8re haut", "R\xFC\x63kseite hoch",
        "Touch post. su", "T\xE1\x63til tras. arriba" },
      { "Rear Up", "Tras. cima", "Arr. haut", "R\xFC\x63k. hoch", "Post. su", "Tras. arr." } },
    { IGI_VK_REAR_DOWN, "PAD_REAR_DOWN",
      { "Rear touch Down", "Touch traseiro baixo", "Toucher arri\xE8re bas", "R\xFC\x63kseite runter",
        "Touch post. gi\xF9", "T\xE1\x63til tras. abajo" },
      { "Rear Down", "Tras. baixo", "Arr. bas", "R\xFC\x63k. runter", "Post. gi\xF9", "Tras. ab." } },
    /* Spare inputs (unbound by default). */
    { IGI_VK_L2, "PAD_L2",
      { "L2", "L2", "L2", "L2", "L2", "L2" }, { "L2", "L2", "L2", "L2", "L2", "L2" } },
    { IGI_VK_R2, "PAD_R2",
      { "R2", "R2", "R2", "R2", "R2", "R2" }, { "R2", "R2", "R2", "R2", "R2", "R2" } },
    { IGI_VK_L3, "PAD_L3",
      { "L3", "L3", "L3", "L3", "L3", "L3" }, { "L3", "L3", "L3", "L3", "L3", "L3" } },
    { IGI_VK_R3, "PAD_R3",
      { "R3", "R3", "R3", "R3", "R3", "R3" }, { "R3", "R3", "R3", "R3", "R3", "R3" } },
    { IGI_VK_REAR_UL, "PAD_REAR_UL",
      { "Rear touch up-left", "Touch tras. sup. esq.", "Arri\xE8re haut-gauche", "R\xFC\x63kseite oben links",
        "Post. alto-sin.", "T\xE1\x63til tras. sup. izq." },
      { "Rear up-left", "Tras. sup. esq.", "Arr. h-g.", "R\xFC\x63k. o-li.", "Post. a-si.", "Tras. sup. izq." } },
    { IGI_VK_REAR_UR, "PAD_REAR_UR",
      { "Rear touch up-right", "Touch tras. sup. dir.", "Arri\xE8re haut-droite", "R\xFC\x63kseite oben rechts",
        "Post. alto-des.", "T\xE1\x63til tras. sup. der." },
      { "Rear up-right", "Tras. sup. dir.", "Arr. h-d.", "R\xFC\x63k. o-re.", "Post. a-de.", "Tras. sup. der." } },
    { IGI_VK_REAR_DL, "PAD_REAR_DL",
      { "Rear touch down-left", "Touch tras. inf. esq.", "Arri\xE8re bas-gauche", "R\xFC\x63kseite unten links",
        "Post. basso-sin.", "T\xE1\x63til tras. inf. izq." },
      { "Rear down-left", "Tras. inf. esq.", "Arr. b-g.", "R\xFC\x63k. u-li.", "Post. b-si.", "Tras. inf. izq." } },
    { IGI_VK_REAR_DR, "PAD_REAR_DR",
      { "Rear touch down-right", "Touch tras. inf. dir.", "Arri\xE8re bas-droite", "R\xFC\x63kseite unten rechts",
        "Post. basso-des.", "T\xE1\x63til tras. inf. der." },
      { "Rear down-right", "Tras. inf. dir.", "Arr. b-d.", "R\xFC\x63k. u-re.", "Post. b-de.", "Tras. inf. der." } },
    { IGI_VK_NONE, "PAD_NONE",
      { "Not set", "Nenhum", "Aucun", "Nicht belegt", "Nessuno", "Sin asignar" },
      { "Not set", "Nenhum", "Aucun", "Nicht belegt", "Nessuno", "Sin asig." } },
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

/* Menu texts the pad scheme renames (a column per language, cp1252): the pad has
 * no mouse, so the game's mouse wording becomes the pad's (aim/look, a button). */
static const struct {
    const char *key, *text[N_LANGS];
} pad_texts[] = {
    { "Invert Mouse",
      { "Invert Look", "Inverter mira", "Inversion de la vis\xE9\x65", "Zielen umkehren", "Inverti mira",
        "Invertir visi\xF3n" } },
    { "Mouse Sensitivity",
      { "Look Sensitivity", "Sensibilidade da mira", "Sensibilit\xE9 de la vis\xE9\x65", "Ziel-Empfindlichkeit",
        "Sensibilit\xE0 mira", "Sensibilidad de visi\xF3n" } },
    { "Select a key or button",
      { "Press a button", "Pressione um bot\xE3o", "Appuyez sur un bouton", "Taste dr\xFC\x63ken",
        "Premi un pulsante", "Pulsa un bot\xF3n" } },
};
#define N_PAD_TEXTS (sizeof(pad_texts) / sizeof(pad_texts[0]))

static int valid, scheme;
static uint32_t id_ptr[N_VKEYS], label_ptr[N_VKEYS][N_LANGS], text_ptr[N_PAD_TEXTS][N_LANGS];
static uint32_t list_obj, list_frames;   /* the screen's object, frames since its last update */
static uint32_t text_buf;                /* guest copy of a key cell's text */
#define TEXT_BUF_SIZE 160u

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
        for (unsigned l = 0; l < N_LANGS; l++)
            label_ptr[i][l] = scratch_strdup(vkeys[i].label[l]);
    }
    for (unsigned i = 0; i < N_PAD_TEXTS; i++)
        for (unsigned l = 0; l < N_LANGS; l++)
            text_ptr[i][l] = scratch_strdup(pad_texts[i].text[l]);
    text_buf = scratch_alloc(TEXT_BUF_SIZE, 4);
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
    unsigned l = lang_col(lang);
    for (unsigned i = 0; i < N_VKEYS && !text; i++)
        if (key == id_ptr[i])
            text = label_ptr[i][l];
    if (!text && scheme) {
        for (unsigned i = 0; i < N_PAD_TEXTS && !text; i++)
            if (guest_streq(key, pad_texts[i].key))
                text = text_ptr[i][l];
    }
    if (!text)
        text = vita_menu_text(key, lang);   /* the Vita options screen's own texts */
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

/* A guest function called from a hook at a function's entry (sp: its stack pointer). */
static uint32_t call_guest(Ctx *c, uint32_t sp, uint32_t fn, int nargs, const uint32_t *args)
{
    const uint32_t saved_esp = c->r[4];
    c->r[4] = sp - 64u;   /* below the caller's frame */
    uint32_t r = guest_call(c, fn, nargs, args);
    c->r[4] = saved_esp;
    return r;
}

/* The list's bindings into the player's table, as the screen's OK does. */
static void apply_list(Ctx *c, uint32_t sp, uint32_t obj)
{
    call_guest(c, sp, GAME_LIST_APPLY, 1, &obj);
}

/* ---- second controls ------------------------------------------------------------ */

static uint32_t second_obj;        /* the list second[] belongs to */
static uint8_t second[MAX_ROWS];   /* per row: key code of the action's second control, 0 none */
static int second_request;         /* updates left for a requested second capture to start */
static int second_row = -1;        /* row whose second control is being captured */

static uint32_t row_count(void)
{
    uint32_t n = RD32(ACTION_COUNT_FUNC + 1u);
    return n < MAX_ROWS ? n : MAX_ROWS;
}

static uint32_t row_action(uint32_t row)
{
    return RD32(ACTION_TABLE + row * KEY_ENTRY_SIZE + 32u);
}

/* Keyboard code of a bindable key (index into the key table, pad keys
 * included); 0 for none or another device. */
static uint8_t index_code(uint32_t idx)
{
    uint32_t v;
    if (idx < KEY_COUNT)
        v = RD32(KEY_TABLE + idx * KEY_ENTRY_SIZE + 32u);
    else if (idx - KEY_COUNT < N_VKEYS)
        v = DEVICE_KEYBOARD << 30 | vkeys[idx - KEY_COUNT].code;
    else
        return 0;
    return v >> 30 == DEVICE_KEYBOARD && (v & 0x3FFFFFFFu) < 256u ? (uint8_t)v : 0u;
}

static int code_index(uint8_t code)
{
    for (unsigned i = 0; i < N_VKEYS; i++)
        if (vkeys[i].code == code)
            return (int)(KEY_COUNT + i);
    for (uint32_t i = 0; i < KEY_COUNT; i++)
        if (RD32(KEY_TABLE + i * KEY_ENTRY_SIZE + 32u) == (DEVICE_KEYBOARD << 30 | code))
            return (int)i;
    return -1;
}

static uint8_t row_first(uint32_t obj, uint32_t row)
{
    return index_code(RD32(obj + LIST_KEY + row * 4u));
}

static void set_row_first(uint32_t obj, uint32_t row, uint8_t code)
{
    int i = code_index(code);
    if (i >= 0)
        WR32(obj + LIST_KEY + row * 4u, (uint32_t)i);
}

/* A menu text (key: guest string) into out. */
static void menu_text(Ctx *c, uint32_t sp, uint32_t window, uint32_t key, char *out, size_t n)
{
    const uint32_t args[2] = { window, key };
    uint32_t t = key ? call_guest(c, sp, GAME_MENU_TEXT, 2, args) : 0u;
    snprintf(out, n, "%.60s", t >= 0x10000u ? (const char *)(g_mem + t) : "");
}

static uint32_t index_name(uint32_t idx)
{
    if (idx < KEY_COUNT)
        return KEY_TABLE + idx * KEY_ENTRY_SIZE;
    return idx - KEY_COUNT < N_VKEYS ? id_ptr[idx - KEY_COUNT] : 0u;
}

/* A bindable key's name in a cell with two controls: the pad's brief ones. */
static void brief_name(Ctx *c, uint32_t sp, uint32_t window, uint32_t idx, char *out, size_t n)
{
    if (idx >= KEY_COUNT && idx - KEY_COUNT < N_VKEYS) {
        unsigned l = lang_col(window ? RD32(window + WINDOW_LANGUAGE) : 0u);
        snprintf(out, n, "%s", vkeys[idx - KEY_COUNT].brief[l]);
    } else {
        menu_text(c, sp, window, index_name(idx), out, n);
    }
}

/* The row's key cell: its first control and the second one after a slash, or
 * "+ " and the prompt while the second one is captured. */
static void show_row(Ctx *c, uint32_t sp, uint32_t obj, uint32_t row)
{
    uint32_t text = RD32(obj + LIST_KEY_TEXT + row * 4u), window = RD32(obj + LIST_WINDOW);
    uint32_t first = RD32(obj + LIST_KEY + row * 4u);
    if (!text || !text_buf)
        return;
    char a[64], b[64], s[TEXT_BUF_SIZE];
    if ((int)row == second_row) {
        menu_text(c, sp, window, TEXT_SELECT_KEY, b, sizeof(b));
        snprintf(s, sizeof(s), "+ %s", b);
    } else if (second[row] && code_index(second[row]) >= 0) {
        brief_name(c, sp, window, first, a, sizeof(a));
        brief_name(c, sp, window, (uint32_t)code_index(second[row]), b, sizeof(b));
        snprintf(s, sizeof(s), "%s / %s", a, b);
    } else {
        menu_text(c, sp, window, index_name(first), s, sizeof(s));
    }
    memcpy(g_mem + text_buf, s, strlen(s) + 1u);
    const uint32_t args[2] = { text, text_buf };
    call_guest(c, sp, GAME_SET_TEXT, 2, args);
}

/* 0x423770: void set_key(obj, row, index): the row's key and its cell's text. */
int rt_controls_set_key(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)eax;
    uint32_t obj = RD32(esp + 4u), row = RD32(esp + 8u);
    if (!valid || !scheme || obj != second_obj || row >= MAX_ROWS)
        return 0;
    if ((int)row == second_row)
        second_row = -1;   /* the game ended that capture itself (Esc, a mouse button) */
    if (!second[row])
        return 0;
    WR32(obj + LIST_KEY + row * 4u, RD32(esp + 12u));
    show_row(c, esp, obj, row);
    c->r[1] = ecx;
    *ret = RET(esp + 4u, 0u);
    return 1;
}

void igi_controls_request_second(void)
{
    if (scheme)
        second_request = 30;   /* the click is seen within some ten updates */
}

static uint32_t player_table(uint32_t *count_at)
{
    uint32_t p = RD32(CURRENT_PLAYER);
    if (p >= MAX_PLAYERS)
        return 0;
    *count_at = PLAYER_COUNT + p * PLAYER_STRIDE;
    return PLAYER_BINDINGS + p * PLAYER_STRIDE;
}

/* The player's second controls, as the list gets loaded: an action's second
 * keyboard entry, when it is a control used nowhere else. */
static void load_second(uint32_t obj)
{
    second_obj = obj;
    second_row = -1;
    second_request = 0;
    memset(second, 0, sizeof(second));
    uint32_t count_at = 0, table = player_table(&count_at);
    if (!table)
        return;
    uint32_t count = RD32(count_at), rows = row_count();
    if (count > MAX_BINDINGS)
        count = MAX_BINDINGS;
    uint8_t first[MAX_ROWS] = { 0 };
    for (uint32_t r = 0; r < rows; r++)
        for (uint32_t i = 0; i < count; i++)
            if (RD32(table + i * 12u) == row_action(r)) {
                if (RD32(table + i * 12u + 4u) == DEVICE_KEYBOARD && RD32(table + i * 12u + 8u) < 256u)
                    first[r] = (uint8_t)RD32(table + i * 12u + 8u);
                break;
            }
    for (uint32_t r = 0; r < rows; r++) {
        int seen_first = 0;
        for (uint32_t i = 0; i < count && !second[r]; i++) {
            uint32_t e = table + i * 12u, code = RD32(e + 8u);
            if (RD32(e) != row_action(r))
                continue;
            if (!seen_first) {
                seen_first = 1;
                continue;
            }
            if (RD32(e + 4u) != DEVICE_KEYBOARD || code == 0u || code >= 256u || code == IGI_VK_NONE ||
                code_index((uint8_t)code) < 0)
                continue;
            int used = 0;
            for (uint32_t j = 0; j < rows && !used; j++)
                used = first[j] == code || second[j] == code;
            if (!used)
                second[r] = (uint8_t)code;
        }
    }
}

/* Before the list goes into the player's table: an action's extra keyboard
 * entries make way for its second control, written after all the first ones
 * (the game updates each action's first entry, adding none: an action without
 * one gets "Not set"). */
static void write_second(uint32_t obj)
{
    uint32_t count_at = 0, table = player_table(&count_at);
    if (obj != second_obj || !table)
        return;
    uint32_t count = RD32(count_at), rows = row_count(), n = 0, seen = 0;
    if (count > MAX_BINDINGS)
        count = MAX_BINDINGS;
    uint32_t out[MAX_BINDINGS][3];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t e = table + i * 12u, action = RD32(e);
        if (RD32(e + 4u) == DEVICE_KEYBOARD && (seen & action))
            continue;
        seen |= action;
        out[n][0] = action;
        out[n][1] = RD32(e + 4u);
        out[n][2] = RD32(e + 8u);
        n++;
    }
    for (uint32_t r = 0; r < rows && n < MAX_BINDINGS; r++)
        if (!(seen & row_action(r))) {
            seen |= row_action(r);
            out[n][0] = row_action(r);
            out[n][1] = DEVICE_KEYBOARD;
            out[n][2] = IGI_VK_NONE;
            n++;
        }
    for (uint32_t r = 0; r < rows && n < MAX_BINDINGS; r++)
        if (second[r]) {
            out[n][0] = row_action(r);
            out[n][1] = DEVICE_KEYBOARD;
            out[n][2] = second[r];
            n++;
        }
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < 3u; k++)
            WR32(table + i * 12u + k * 4u, out[i][k]);
    WR32(count_at, n);
}

/* While the second control of second_row is captured: the first key down (Esc
 * left to the game, which cancels). A control of that row clears the second
 * one; a control of another action is swapped with this row's second (another
 * action's first control left without one takes that action's second, or Not
 * set). 1 if the capture ended. */
static int capture_second(Ctx *c, uint32_t sp, uint32_t obj)
{
    uint32_t row = (uint32_t)second_row, rows = row_count();
    uint8_t k = 0;
    for (uint32_t i = 0; i < KEY_COUNT + N_VKEYS && !k; i++) {
        uint8_t code = index_code(i);
        if (code && code != DIK_ESCAPE && code != 0xDBu && code != 0xDCu && (g_mem[KEY_STATE + code] & 0x80u))
            k = code;
    }
    if (!k || row >= rows)
        return 0;
    uint8_t old = second[row];
    int other = -1;
    if (k == row_first(obj, row) || k == old) {
        second[row] = 0;
    } else {
        for (uint32_t j = 0; j < rows && other < 0; j++) {
            if (j == row)
                continue;
            if (row_first(obj, j) == k) {
                other = (int)j;
                if (old) {
                    set_row_first(obj, j, old);
                } else if (second[j]) {
                    set_row_first(obj, j, second[j]);
                    second[j] = 0;
                } else {
                    set_row_first(obj, j, IGI_VK_NONE);
                }
            } else if (second[j] == k) {
                other = (int)j;
                second[j] = old;
            }
        }
        second[row] = k;
    }
    second_row = -1;
    g_mem[obj + LIST_CAPTURING] = 0;
    WR32(obj + LIST_SELECTED, 0xFFFFFFFFu);
    const uint32_t window = RD32(obj + LIST_WINDOW), sound[3] = { SOUND_SELECT, 0u, 0u };
    call_guest(c, sp, GAME_MOUSE_RESET, 0, NULL);
    if (window)
        call_guest(c, sp, GAME_WINDOW_IDLE, 1, &window);
    call_guest(c, sp, GAME_PLAY_SOUND, 3, sound);
    show_row(c, sp, obj, row);
    if (other >= 0)
        show_row(c, sp, obj, (uint32_t)other);
    rt_log("controls: row %u second control %02X", row, second[row]);
    return 1;
}

/* A first control captured by the game: when it was another row's second, that
 * row takes this row's old first control instead (as the game swaps first ones). */
static void first_captured(Ctx *c, uint32_t sp, uint32_t obj, uint32_t row, uint8_t before)
{
    uint8_t k = row_first(obj, row);
    if (!k || k == before)
        return;
    for (uint32_t j = 0; j < row_count(); j++)
        if (second[j] == k) {
            second[j] = before != IGI_VK_NONE ? before : 0u;
            show_row(c, sp, obj, j);
            break;
        }
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
        static uint32_t capture_row;
        static uint8_t first_before;
        uint32_t obj = RD32(sp + 4u);
        int capturing = g_mem[obj + LIST_CAPTURING] != 0;
        if (obj != list_obj)
            was_capturing = reset_done = 0;
        if (scheme && obj == second_obj) {
            if (g_mem[obj + LIST_RESET]) {   /* "Reset to Default": single controls */
                memset(second, 0, sizeof(second));
                second_row = -1;
            }
            if (capturing && !was_capturing) {
                capture_row = RD32(obj + LIST_SELECTED);
                first_before = capture_row < row_count() ? row_first(obj, capture_row) : 0u;
                if (second_request && capture_row < row_count()) {
                    second_row = (int)capture_row;
                    show_row(c, sp, obj, capture_row);
                }
                second_request = 0;
            } else if (!capturing && second_request) {
                second_request--;
            }
            if (capturing && second_row >= 0) {
                if (capture_second(c, sp, obj))
                    capturing = 0;
            } else if (was_capturing && !capturing && capture_row < row_count()) {
                first_captured(c, sp, obj, capture_row, first_before);
            }
        }
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
        if (scheme) {
            replace_keyboard_bindings();
            load_second(RD32(sp + 4u));
        }
        break;
    case 0x00423EB0u:   /* controls screen: the list into the player's bindings(obj) */
        if (scheme)
            write_second(RD32(sp + 4u));
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
