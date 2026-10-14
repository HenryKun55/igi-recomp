/* Game-specific fixes driven by recompiler entry hooks. */
#ifndef IGI_FIXES_H
#define IGI_FIXES_H

#include <stdint.h>

/* True while the 2D queue of the in-engine cutscene window (state 16) is being
 * drawn: its whole queue holds the stray in-game "Game Paused" menu overlay and
 * must stay hidden (the scene, letterbox and subtitles come through other paths). */
int igi_skip_cutscene_2d(void);
/* Called once per presented frame. */
void igi_end_frame(void);

/* State field of the UI window rendered last (5/2/6/0 menus, 16 in-engine
 * cutscene, 9 gameplay), or IGI_UI_NONE when none was drawn in the last frames. */
#define IGI_UI_NONE 0xFFFFFFFFu
uint32_t igi_ui_state(void);

/* 1 while the map computer screen is up. That screen keeps its own mouse cursor,
 * which it moves only by the relative mouse motion from DirectInput (it ignores
 * the absolute position and WM_MOUSEMOVE); its tabs are clicked with that cursor.
 * The front touch panel must therefore drive it by relative motion, not by
 * jumping the cursor to the finger (shim/src/platform_vita.c). */
int igi_computer_active(void);

/* The current menu screen's own mouse cursor (game-screen coordinates), when a
 * menu window is up: 1 with the position stored, else 0. The menus (main menu,
 * options, mission select, the controls screen) keep this cursor and move it
 * only by the relative DirectInput mouse motion, hit-testing every widget
 * against it. The front touch reads it and posts a relative delta to place it
 * exactly on the touched point, with no accumulated drift (platform_vita.c). */
int igi_menu_cursor(int32_t *x, int32_t *y);

/* One focusable widget of the current menu window, with its screen rectangle
 * (game-screen coordinates, the space of the menu cursor). */
typedef struct IgiMenuWidget {
    uint32_t widget;      /* the widget object */
    int index;            /* its index in the window's widget array */
    int32_t x, y, w, h;   /* its screen rectangle */
} IgiMenuWidget;

/* The current menu window's focusable widgets, in the window's own order (top to
 * bottom on the stock screens), with their screen rectangles. Fills up to `max`
 * entries, returns the count and stores the window's current focus index in
 * *focus (may be NULL). 0 when no live menu window is up. The pad's D-pad walks
 * these to move the game's own menu cursor between items, exactly as the
 * controls screen walks its binding rows (shim/src/pad_input.c). */
int igi_menu_widgets(IgiMenuWidget *out, int max, int *focus);

/* 1 when the current menu screen is a list dialog (the mission select): its only
 * focusable widgets are a small bottom button strip (OK/Cancel) and the content
 * is a non-focusable list the game scrolls with the arrow keys. On such a screen
 * the pad sends arrow keys for the list instead of walking the cursor between
 * the buttons. A normal menu (items spread down the window) returns 0. */
int igi_menu_is_list_screen(void);

/* Press a focusable widget by its index in the window's widget array, as the
 * game's keyboard focus does (sets the widget's command field so it fires on the
 * next update). Used for the mission-select OK button, which the shared cursor
 * hit-test cannot reach on that screen. 1 on success. */
int igi_menu_activate_widget(int index);

#endif
