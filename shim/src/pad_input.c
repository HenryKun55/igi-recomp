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

/* ---- general menu navigation --------------------------------------------------
 *
 * Every menu screen (main menu, Options, the Vita settings, the in-game menu and
 * its Yes/No dialogs) keeps its focusable widgets with their screen rectangles
 * (runtime/src/igi_fixes.c igi_menu_widgets). The D-pad walks that list: up/down
 * step to the previous/next widget, wrapping, and the window cursor is steered
 * onto it so the game highlights it (the shared hit-test marks whichever widget
 * the cursor is inside). Cross then presses the widget the D-pad walked to
 * directly, by its command field (igi_menu_activate_widget), the way the game's
 * keyboard focus does -- so a small button (Yes/No, OK/Cancel) is always pressed
 * exactly, with no dependence on the cursor landing pixel-perfect on it. Left and
 * right fall through to the game as arrow keys for a value widget (a ListBox) to
 * consume.
 *
 * The mission select is the one screen whose main content (the mission list) is a
 * ScrollListBox that is not a focusable widget and does not answer the arrow keys,
 * so the D-pad cannot reach its rows. There the player moves the cursor onto a
 * mission with the right stick or the touch screen and R left-clicks under the
 * cursor to pick it (updating the preview), then presses OK with the D-pad and
 * Cross. R is a plain "click here" in the menus, useful on any screen. No screen
 * needs special handling: Yes/No dialogs are ordinary two-widget windows.
 *
 * Steering the cursor is only for the highlight: the menu scales the relative
 * motion it reads and reads it a frame after it moves, so the cursor is nudged
 * towards the widget over a few polls (menu_seek) rather than jumped, which also
 * keeps R's click landing where the cursor visibly is.
 *
 * The controls screen keeps its own path (list_control). */

/* Where the window cursor effectively is: the game's own cursor plus the motion
 * posted but not yet read (dx/dy), as the controls list reckons it. */
static void menu_cursor(int32_t cursor_x, int32_t cursor_y, int32_t *x, int32_t *y)
{
    *x = cursor_x + g_mouse.dx;
    *y = cursor_y + g_mouse.dy;
}

/* The point the D-pad is steering the window cursor to (game-screen coordinates),
 * or MENU_GOAL_NONE when it is not steering. The menu moves its cursor only by
 * the relative mouse motion and scales that motion by its own cursor speed, so a
 * single posted delta does not land exactly: a far target (Yes vs No in a narrow
 * dialog) overshoots and misses a small button. menu_seek (run every input poll
 * from pad_tick) closes the gap over a few frames, reading the game's own cursor
 * each time, so it settles on the target whatever the cursor speed -- the same
 * closed loop the front touch uses to land a tap (shim/src/platform_vita.c). */
#define MENU_GOAL_NONE (-100000)
#define MENU_SEEK_STEPS 20      /* give up after this many correction steps (jitter guard) */
static int32_t menu_goal_x = MENU_GOAL_NONE, menu_goal_y;   /* the widget's centre, steered to */
static int32_t menu_goal_rx, menu_goal_ry, menu_goal_rw, menu_goal_rh;   /* its rectangle */
static int menu_seek_left;      /* correction steps left before giving up */
static int32_t menu_seek_from_x, menu_seek_from_y;   /* the cursor when the last step was posted */
static int menu_seek_posted;    /* a step is posted and the cursor has not yet moved from it */
/* The widget object the D-pad last stepped to, so consecutive presses step
 * through the widgets in order even before the cursor has reached one -- more
 * reliable than re-reading the moving cursor, which can be mid-flight or a pixel
 * off a small button. 0 when not set (reset on a click or on leaving the menus);
 * revalidated against the live widget array each press, so a screen change (a
 * different array) falls back to the cursor. */
static uint32_t menu_cur_widget;

/* Steer the window cursor one step toward the goal and stop once it is inside the
 * target widget. Called when the D-pad sets a goal and then once per poll until it
 * arrives. The correction is measured against the game's OWN cursor, not the
 * effective position (cursor + pending delta): the menu scales the motion it
 * reads, so a posted delta does not move the cursor by that much, and the next
 * correction has to start from where the cursor actually ended up. A new step is
 * posted only once the previous one has visibly moved the cursor, so steps do not
 * stack. This converges in a few steps whatever the menu's cursor speed. */
static void menu_seek(void)
{
    if (menu_goal_x == MENU_GOAL_NONE)
        return;
    int32_t cur_x, cur_y;
    if (!igi_menu_cursor(&cur_x, &cur_y))
        return;   /* no live menu window this frame (e.g. a dialog over gameplay
                   * whose gameplay window drew last): keep the goal, retry next poll */
    /* Wait until the last step's motion has actually moved the cursor before
     * posting the next one. The game reads the mouse delta (clearing g_mouse.dx)
     * at a different point than it moves its cursor, so "delta read" is not "cursor
     * moved"; posting again too early stacks deltas and the cursor shoots past.
     * Seeing the cursor at a new position is the reliable signal the step landed. */
    if (menu_seek_posted) {
        if (g_mouse.dx || g_mouse.dy || (cur_x == menu_seek_from_x && cur_y == menu_seek_from_y))
            return;   /* still in flight */
        menu_seek_posted = 0;
    }
    if (cur_x >= menu_goal_rx && cur_x < menu_goal_rx + menu_goal_rw && cur_y >= menu_goal_ry &&
        cur_y < menu_goal_ry + menu_goal_rh) {
        menu_goal_x = MENU_GOAL_NONE;   /* the cursor is inside the target widget: done */
        return;
    }
    if (--menu_seek_left <= 0) {
        menu_goal_x = MENU_GOAL_NONE;   /* out of tries: steer no more */
        return;
    }
    /* Post about half the gap towards the centre: the menu scales the motion it
     * reads, so posting the full error overshoots and oscillates; half converges
     * smoothly whatever the scale, and the loop stops as soon as the cursor is
     * anywhere inside the widget above. */
    int32_t ex = menu_goal_x - cur_x, ey = menu_goal_y - cur_y;
    int32_t dx = ex / 2, dy = ey / 2;
    if (dx == 0)
        dx = ex;   /* close the last pixel or two */
    if (dy == 0)
        dy = ey;
    g_mouse.dx += dx;
    g_mouse.dy += dy;
    g_mouse.x = cur_x + dx;
    g_mouse.y = cur_y + dy;
    menu_seek_from_x = cur_x;
    menu_seek_from_y = cur_y;
    menu_seek_posted = 1;
    user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
}

/* Steer the window cursor onto widget w: aim for its centre but stop as soon as
 * the cursor is anywhere inside it. Takes the first step now. */
static void menu_cursor_to(const IgiMenuWidget *w)
{
    menu_goal_x = w->x + w->w / 2;
    menu_goal_y = w->y + w->h / 2;
    menu_goal_rx = w->x;
    menu_goal_ry = w->y;
    menu_goal_rw = w->w;
    menu_goal_rh = w->h;
    menu_seek_left = MENU_SEEK_STEPS;
    menu_seek_posted = 0;
    menu_seek();
}

/* The widget (index into v) the cursor is inside, else -1. */
static int menu_widget_at(const IgiMenuWidget *v, int n, int32_t cx, int32_t cy)
{
    for (int i = 0; i < n; i++)
        if (cx >= v[i].x && cx < v[i].x + v[i].w && cy >= v[i].y && cy < v[i].y + v[i].h)
            return i;
    return -1;
}

/* A control on a menu screen other than the controls screen; 1 if handled.
 * Cross presses the widget the D-pad last walked to (by its command field); with
 * nothing walked to yet, and always for R, it left-clicks wherever the cursor is
 * (a mission row the right stick or touch put it on, or any hovered item). The
 * D-pad walks the window's widgets: up/down to the previous/next one, wrapping,
 * steering the cursor onto it for the highlight. Left/right are left to the game
 * (arrow keys a value widget consumes). When nothing has been walked to, the step
 * starts from the widget the cursor sits on, else the one in focus, else the
 * first, so a first press always lands somewhere sensible. */
/* The nearest widget to the left/right of widget `at` whose vertical span overlaps
 * it (the same row); -1 if none. Lets left/right step between side-by-side buttons
 * (Sim/Não, OK/Cancelar) but fall through on a lone value widget (a ListBox). */
static int menu_side_widget(const IgiMenuWidget *v, int n, int at, int right)
{
    int32_t ay0 = v[at].y, ay1 = v[at].y + v[at].h, acx = v[at].x + v[at].w / 2;
    int best = -1;
    int32_t best_d = 0;
    for (int i = 0; i < n; i++) {
        if (i == at || v[i].y + v[i].h <= ay0 || v[i].y >= ay1)
            continue;   /* same widget, or a different row (no vertical overlap) */
        int32_t cx = v[i].x + v[i].w / 2;
        if (right ? cx <= acx : cx >= acx)
            continue;
        int32_t d = right ? cx - acx : acx - cx;
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}

static int menu_nav(PadControl control, uint8_t *mouse_sent)
{
    if (control == PAD_CROSS || control == PAD_R) {
        menu_goal_x = MENU_GOAL_NONE;   /* stop steering */
        if (control == PAD_CROSS && menu_cur_widget) {
            /* Cross presses the widget the D-pad walked to, by its command field,
             * exactly as the game's keyboard focus does -- no dependence on the
             * cursor sitting pixel-perfect on a small button (Yes/No, OK/Cancel). */
            igi_menu_activate_widget(menu_cur_widget);
            menu_cur_widget = 0;
            return 1;
        }
        /* R (and Cross with nothing walked to) clicks where the cursor rests: the
         * mission the right stick or touch put it on, or any hovered item. */
        menu_cur_widget = 0;
        pad_mouse_button(0, 1);
        *mouse_sent = 1;
        return 1;
    }
    int up = control == PAD_DPAD_UP || control == PAD_LSTICK_UP;
    int down = control == PAD_DPAD_DOWN || control == PAD_LSTICK_DOWN;
    int left = control == PAD_DPAD_LEFT || control == PAD_LSTICK_LEFT;
    int right = control == PAD_DPAD_RIGHT || control == PAD_LSTICK_RIGHT;
    if (!up && !down && !left && !right)
        return 0;   /* other controls: fall through */
    IgiMenuWidget v[32];
    int focus = 0;
    int n = igi_menu_widgets(v, (int)(sizeof(v) / sizeof(v[0])), &focus);
    if (n <= 0)
        return 0;   /* no menu window: let menu_key send arrow keys as before */
    int32_t cur_x, cur_y;
    if (!igi_menu_cursor(&cur_x, &cur_y))
        return 0;
    /* Where we are in the widget list: the one the last press stepped to if it is
     * still in this window's array, else the one the cursor sits on, else none. */
    int at = -1;
    if (menu_cur_widget)
        for (int i = 0; i < n; i++)
            if (v[i].widget == menu_cur_widget) {
                at = i;
                break;
            }
    if (at < 0) {
        int32_t cx, cy;
        menu_cursor(cur_x, cur_y, &cx, &cy);
        at = menu_widget_at(v, n, cx, cy);
    }
    int target;
    if (at < 0) {
        /* Not on any widget: go to the one in focus, else the first. */
        target = 0;
        for (int i = 0; i < n; i++)
            if (v[i].index == focus) {
                target = i;
                break;
            }
    } else if (up || down) {
        target = at + (down ? 1 : -1);
        if (target < 0)
            target = n - 1;
        else if (target >= n)
            target = 0;
    } else {
        /* Left/right: step to the nearest widget on that side of the same row, so
         * side-by-side buttons (Sim/Não, OK/Cancelar) move horizontally; if there
         * is none (a lone value widget like a ListBox), let the arrow key through so
         * that widget changes its value instead. */
        target = menu_side_widget(v, n, at, right);
        if (target < 0)
            return 0;
    }
    menu_cur_widget = v[target].widget;
    menu_cursor_to(&v[target]);
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
            if (list && igi_controls_pad_scheme()) {
                if (list_control(&l, control, &s->mouse))
                    return;
            } else if (menu_nav(control, &s->mouse)) {
                return;   /* any other menu screen: the cursor walked its widgets */
            }
            key = menu_key(control);
        }
    }
    pad_key(key, 1);
    s->key = key;
}

void pad_tick(void)
{
    menu_seek();   /* steer the window cursor to the D-pad's goal, a step per poll */
    /* The controls screen opening: the cursor on its first row, for the D-pad. */
    static int list_up;
    IgiControlsList l;
    int up = igi_controls_list(&l);
    if (up && !list_up && igi_controls_pad_scheme())
        list_point_row(&l, 0);
    list_up = up;
}
