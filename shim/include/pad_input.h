/* Game controls driven by a pad (the Vita's buttons, sticks and touch panels):
 * each control stands for one of IGI's actions and is sent as the key or mouse
 * button the game has bound to that action at that moment, so the controls work
 * whatever bindings the player's pc/config.qvm holds. */
#ifndef PAD_INPUT_H
#define PAD_INPUT_H

#include <stdint.h>

/* IGI's actions, as the bits of the executable's action table (0x536558). */
#define IGI_ACT_MOVE_LEFT 0x00000001u
#define IGI_ACT_MOVE_RIGHT 0x00000002u
#define IGI_ACT_MOVE_UP 0x00000004u
#define IGI_ACT_MOVE_DOWN 0x00000008u
#define IGI_ACT_FIRE 0x00000100u
#define IGI_ACT_RELOAD 0x00000200u
#define IGI_ACT_ACTIVATE 0x00000400u
#define IGI_ACT_CROUCH 0x00000800u
#define IGI_ACT_JUMP 0x00001000u
#define IGI_ACT_NEXT_WEAPON 0x00002000u
#define IGI_ACT_ALTERNATE_FIRE 0x00004000u
#define IGI_ACT_PEEK 0x00008000u
#define IGI_ACT_BINOCULARS 0x00010000u
#define IGI_ACT_MAP_COMPUTER 0x00020000u
#define IGI_ACT_TOGGLE_WALK_RUN 0x00040000u
#define IGI_ACT_PREV_WEAPON 0x00080000u
#define IGI_ACT_ZOOM_IN 0x00100000u
#define IGI_ACT_ZOOM_OUT 0x00200000u

/* How an action is bound: device 1 = keyboard (code: DIK_* scan code),
 * 3 = mouse (code: 1, 2, 4, 8 buttons, 0x100 wheel up, 0x200 wheel down). */
typedef struct PadBinding {
    uint32_t device, code;
} PadBinding;
#define PAD_DEVICE_KEYBOARD 1u
#define PAD_DEVICE_MOUSE 3u

/* The binding the game uses for an action now: the active player's, or the
 * game's default one when the player's table has none that a keyboard or
 * mouse can produce (then the default is also put back into that table, so the
 * action works). */
PadBinding pad_binding(uint32_t action);
/* Press or release an action. A release lets go of what the press pushed, even
 * if the binding changed in between. */
void pad_action(uint32_t action, int down);
/* Press or release a key directly (menu navigation); counted together with the
 * actions' keys, so two controls holding the same key release it only when
 * both let go. */
void pad_key(uint8_t dik, int down);
/* Press or release mouse button b (0 left, 1 right, 2 middle), counted the same way. */
void pad_mouse_button(int b, int down);
/* Once per input poll: repeats the wheel steps of held actions bound to the wheel. */
void pad_tick(uint32_t now_ms);
/* 1 while the game is being played (the gameplay HUD window is up), 0 in the
 * menus, cutscenes, videos and loading screens. */
int pad_in_gameplay(void);

#endif
