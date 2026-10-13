/* Game controls driven by a pad (the Vita's buttons, sticks and touch panels).
 *
 * In play every control is a key of its own (IGI_VK_*, runtime/include/igi_controls.h)
 * that the player binds to IGI's actions in Options -> Controls; the game's
 * binding table does the mapping. In the menus the controls work like a
 * console's: D-pad and left stick = arrow keys, Cross = Enter, Circle = Esc
 * (back); while the controls screen waits for a key they send their own keys
 * again, so that pressing a control binds it. Start is always Esc. */
#ifndef PAD_INPUT_H
#define PAD_INPUT_H

#include <stdint.h>

typedef enum PadControl {
    PAD_CROSS,
    PAD_CIRCLE,
    PAD_SQUARE,
    PAD_TRIANGLE,
    PAD_L,
    PAD_R,
    PAD_SELECT,
    PAD_START,
    PAD_DPAD_UP,
    PAD_DPAD_DOWN,
    PAD_DPAD_LEFT,
    PAD_DPAD_RIGHT,
    PAD_LSTICK_UP,
    PAD_LSTICK_DOWN,
    PAD_LSTICK_LEFT,
    PAD_LSTICK_RIGHT,
    PAD_REAR_UP,
    PAD_REAR_DOWN,
    /* Spare inputs, unbound by default (see igi_controls.h): external pad
     * buttons and the four rear-touch zones (rear_zones=4). */
    PAD_L2,
    PAD_R2,
    PAD_L3,
    PAD_R3,
    PAD_REAR_UL,
    PAD_REAR_UR,
    PAD_REAR_DL,
    PAD_REAR_DR,
    PAD_CONTROL_COUNT
} PadControl;

/* Front-touch aim mode (igi.cfg touch_mode=): DRAG = a drag anywhere aims
 * (default, the original behaviour); SPLIT = the right half of the screen is a
 * relative aim pad and the left half is a virtual movement stick. Menus and the
 * map computer behave the same in both modes. */
typedef enum PadTouchMode { PAD_TOUCH_DRAG, PAD_TOUCH_SPLIT } PadTouchMode;

/* A control going down or up. A release lets go of what the press sent, even
 * if the game moved between play and the menus in between. */
void pad_control(PadControl control, int down);
/* Press or release a key directly; counted together with the controls' keys,
 * so two controls holding the same key release it only when both let go. */
void pad_key(uint8_t dik, int down);
/* Press or release mouse button b (0 left, 1 right, 2 middle), counted the same way. */
void pad_mouse_button(int b, int down);
/* 1 while the game is being played (the gameplay HUD window is up), 0 in the
 * menus, cutscenes, videos and loading screens. */
int pad_in_gameplay(void);
/* 1 while the controls screen waits for the key of an action. */
int pad_capturing_key(void);
/* Once per input poll (follows the controls screen). */
void pad_tick(void);

#endif
