/* Pad controls as keys of their own in IGI's key bindings (runtime/src/igi_controls.c).
 *
 * Every control of the pad is sent to the game as a key code no keyboard
 * produces (IGI_VK_*). The game's controls screen offers them like any other
 * key, shows them by the pad's names and saves them in pc/config.qvm, so the
 * player binds the pad's controls to IGI's actions in Options -> Controls. */
#ifndef IGI_CONTROLS_H
#define IGI_CONTROLS_H

#include <stdint.h>

/* DirectInput scan codes that no keyboard sends and that the game has no name
 * for (DIK_F12 0x58 and DIK_F13 0x64 surround the first range). */
#define IGI_VK_CROSS 0x59u
#define IGI_VK_CIRCLE 0x5Au
#define IGI_VK_SQUARE 0x5Bu
#define IGI_VK_TRIANGLE 0x5Cu
#define IGI_VK_L 0x5Du
#define IGI_VK_R 0x5Eu
#define IGI_VK_SELECT 0x5Fu
#define IGI_VK_DPAD_UP 0x61u
#define IGI_VK_DPAD_DOWN 0x62u
#define IGI_VK_DPAD_LEFT 0x63u
#define IGI_VK_DPAD_RIGHT 0x67u
#define IGI_VK_LSTICK_UP 0x68u
#define IGI_VK_LSTICK_DOWN 0x69u
#define IGI_VK_LSTICK_LEFT 0x6Au
#define IGI_VK_LSTICK_RIGHT 0x6Bu
#define IGI_VK_REAR_UP 0x6Cu
#define IGI_VK_REAR_DOWN 0x6Du
#define IGI_VK_NONE 0x6Eu   /* "Not set": what actions without a pad control are bound to */

/* 1 when the pad's virtual keys are the default bindings and keyboard bindings
 * get replaced (the Vita; IGI_PAD_CONTROLS=1 elsewhere, for testing). */
int igi_controls_pad_scheme(void);

/* The keyboard key (DIK code; pad keys included) the active player has bound to
 * an action (bit of the game's action table: 4 = MoveUp), 0 for none. */
uint8_t igi_controls_key_for(uint32_t action);
#define IGI_ACTION_MOVE_UP 0x00000004u

/* Called once the executable is in memory, before it runs. */
void igi_controls_init(void);

/* The controls screen's binding list, while that screen is up. */
typedef struct IgiControlsList {
    int capturing;        /* waiting for the key of the selected action */
    int32_t x, y, w, h;   /* the list on the screen, in mouse coordinates */
    int row_h;            /* height of a row */
    int rows;             /* number of actions */
    int offset;           /* first row shown */
    int32_t cursor_x, cursor_y;   /* the game's mouse cursor */
} IgiControlsList;
int igi_controls_list(IgiControlsList *out);
/* Scrolls the list so that it shows rows from `offset` on (clamped). */
void igi_controls_list_scroll(int offset);

/* Called once per presented frame. */
void igi_controls_end_frame(void);

/* Recompiler hooks (tools/recomp/gen.py GAME_HOOKS, via rt_probe). */
struct Ctx;
void igi_controls_probe(struct Ctx *c, uint32_t func, uint32_t sp);

#endif
