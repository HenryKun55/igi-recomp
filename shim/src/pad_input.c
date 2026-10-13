/* Pad controls (see pad_input.h).
 *
 * In play and while the controls screen waits for a key, a control presses its
 * own key (IGI_VK_*); the player's bindings in guest memory, edited in Options ->
 * Controls and saved in pc/config.qvm, turn it into an action
 * (runtime/src/igi_controls.c adds those keys to the game's tables). An action
 * bound to no pad key simply has no control, so nothing breaks: the controls
 * screen and its "Reset to Default" stay reachable with the menu controls.
 *
 * The controls screen only answers the mouse in the game. With the pad scheme
 * the D-pad (or left stick) moves the cursor over the binding list's rows,
 * scrolling it at the ends, and Cross clicks under the cursor (the right stick
 * and the touchscreen reach the screen's other items); Triangle on a row clicks
 * it to capture the action's second control. */
#include "pad_input.h"
#include "igi_controls.h"
#include "igi_fixes.h"
#include "platform.h"
#include "runtime.h"

#define UI_STATE_GAMEPLAY 9u
#define DIK_ESCAPE 0x01
#define DIK_RETURN 0x1C
#define DIK_UP 0xC8
#define DIK_DOWN 0xD0
#define DIK_LEFT 0xCB
#define DIK_RIGHT 0xCD

static const uint8_t control_key[PAD_CONTROL_COUNT] = {
    [PAD_CROSS] = IGI_VK_CROSS,
    [PAD_CIRCLE] = IGI_VK_CIRCLE,
    [PAD_SQUARE] = IGI_VK_SQUARE,
    [PAD_TRIANGLE] = IGI_VK_TRIANGLE,
    [PAD_L] = IGI_VK_L,
    [PAD_R] = IGI_VK_R,
    [PAD_SELECT] = IGI_VK_SELECT,
    [PAD_START] = DIK_ESCAPE,
    [PAD_DPAD_UP] = IGI_VK_DPAD_UP,
    [PAD_DPAD_DOWN] = IGI_VK_DPAD_DOWN,
    [PAD_DPAD_LEFT] = IGI_VK_DPAD_LEFT,
    [PAD_DPAD_RIGHT] = IGI_VK_DPAD_RIGHT,
    [PAD_LSTICK_UP] = IGI_VK_LSTICK_UP,
    [PAD_LSTICK_DOWN] = IGI_VK_LSTICK_DOWN,
    [PAD_LSTICK_LEFT] = IGI_VK_LSTICK_LEFT,
    [PAD_LSTICK_RIGHT] = IGI_VK_LSTICK_RIGHT,
    [PAD_REAR_UP] = IGI_VK_REAR_UP,
    [PAD_REAR_DOWN] = IGI_VK_REAR_DOWN,
    [PAD_L2] = IGI_VK_L2,
    [PAD_R2] = IGI_VK_R2,
    [PAD_L3] = IGI_VK_L3,
    [PAD_R3] = IGI_VK_R3,
    [PAD_REAR_UL] = IGI_VK_REAR_UL,
    [PAD_REAR_UR] = IGI_VK_REAR_UR,
    [PAD_REAR_DL] = IGI_VK_REAR_DL,
    [PAD_REAR_DR] = IGI_VK_REAR_DR,
};

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

int pad_in_gameplay(void)
{
    return igi_ui_state() == UI_STATE_GAMEPLAY;
}

int pad_capturing_key(void)
{
    IgiControlsList l;
    return igi_controls_list(&l) && l.capturing;
}

/* ---- the controls screen's list ----------------------------------------------- */

/* The menus move their cursor by the mouse's relative motion (DirectInput), so
 * the cursor is where the game has it plus the motion it has not read yet. */
static void list_cursor(const IgiControlsList *l, int32_t *x, int32_t *y)
{
    *x = l->cursor_x + g_mouse.dx;
    *y = l->cursor_y + g_mouse.dy;
}

static void list_cursor_to(const IgiControlsList *l, int32_t x, int32_t y)
{
    int32_t cx, cy;
    list_cursor(l, &cx, &cy);
    g_mouse.dx += x - cx;
    g_mouse.dy += y - cy;
    g_mouse.x = x;
    g_mouse.y = y;
    user32_post_message(0x0200, 0, ((uint32_t)y << 16) | ((uint32_t)x & 0xFFFFu));
}

static void list_point_row(const IgiControlsList *l, int row)
{
    list_cursor_to(l, l->x + l->w / 3, l->y + row * l->row_h + l->row_h / 2);
}

/* Controls on the controls screen; 1 if handled. Cross clicks wherever the cursor
 * is (a row, or what the right stick or a tap put it on: Invert Look, Look
 * Sensitivity, Reset to Default, OK); Triangle on a row clicks it for the
 * action's second control; the D-pad walks the rows, scrolling at the ends, and
 * brings a cursor that is elsewhere back to the list. */
static int list_control(const IgiControlsList *l, PadControl control, uint8_t *mouse_sent)
{
    int32_t mx, my;
    list_cursor(l, &mx, &my);
    int visible = l->h / l->row_h;
    int on_row = mx >= l->x && mx < l->x + l->w && my >= l->y && my < l->y + visible * l->row_h;
    if (control == PAD_CROSS || (control == PAD_TRIANGLE && on_row)) {
        if (control == PAD_TRIANGLE)
            igi_controls_request_second();   /* the click's capture sets the second control */
        pad_mouse_button(0, 1);
        *mouse_sent = 1;
        return 1;
    }
    int up = control == PAD_DPAD_UP || control == PAD_LSTICK_UP;
    int down = control == PAD_DPAD_DOWN || control == PAD_LSTICK_DOWN;
    if (!up && !down)
        return 0;
    if (!on_row) {
        list_point_row(l, my < l->y ? 0 : my >= l->y + visible * l->row_h ? visible - 1 : (my - l->y) / l->row_h);
        return 1;
    }
    int row = (my - l->y) / l->row_h + (down ? 1 : -1);
    if (row < 0) {
        igi_controls_list_scroll(l->offset - 1);
        row = 0;
    } else if (row >= visible) {
        igi_controls_list_scroll(l->offset + 1);
        row = visible - 1;
    }
    list_point_row(l, row);
    return 1;
}

/* ---- controls ----------------------------------------------------------------- */

typedef struct Sent {
    uint8_t key;     /* key pressed by the control, 0 none */
    uint8_t mouse;   /* left button pressed by the control */
    uint8_t down;
} Sent;
static Sent sent[PAD_CONTROL_COUNT];

static uint8_t menu_key(PadControl control)
{
    switch (control) {
    case PAD_CROSS: return DIK_RETURN;
    case PAD_CIRCLE: return DIK_ESCAPE;
    case PAD_DPAD_UP: case PAD_LSTICK_UP: return DIK_UP;
    case PAD_DPAD_DOWN: case PAD_LSTICK_DOWN: return DIK_DOWN;
    case PAD_DPAD_LEFT: case PAD_LSTICK_LEFT: return DIK_LEFT;
    case PAD_DPAD_RIGHT: case PAD_LSTICK_RIGHT: return DIK_RIGHT;
    default: return control_key[control];
    }
}

void pad_control(PadControl control, int down)
{
    if ((unsigned)control >= PAD_CONTROL_COUNT)
        return;
    Sent *s = &sent[control];
    if (!down) {
        if (s->key)
            pad_key(s->key, 0);
        if (s->mouse)
            pad_mouse_button(0, 0);
        s->key = s->mouse = s->down = 0;
        return;
    }
    if (s->down)
        return;
    s->down = 1;
    uint8_t key = control_key[control];
    if (control != PAD_START && !pad_in_gameplay()) {
        IgiControlsList l;
        int list = igi_controls_list(&l);
        if (!(list && l.capturing)) {
            if (list && igi_controls_pad_scheme() && list_control(&l, control, &s->mouse))
                return;
            key = menu_key(control);
        }
    }
    pad_key(key, 1);
    s->key = key;
}

void pad_tick(void)
{
    /* The controls screen opening: the cursor on its first row, for the D-pad. */
    static int list_up;
    IgiControlsList l;
    int up = igi_controls_list(&l);
    if (up && !list_up && igi_controls_pad_scheme())
        list_point_row(&l, 0);
    list_up = up;
}
