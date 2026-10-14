/* The port's own rows in the game's Options menu (runtime/src/vita_menu.c).
 *
 * The game drives its menus with compiled scripts (the menusystem .qvm). A menu
 * widget calls a native "script function" by name to read and write its value
 * (e.g. a ToggleBox calls "Config_GameOptionsInputGetInvertMouse()" /
 * "...Set...(this.isOn)"). These names live in a run-time dictionary that
 * 0x402FF0 ("DefineGameOptions") fills by calling the registrar 0x4B8890(name,
 * fnptr, 0, argCount) once per function.
 *
 * The port registers its own Vita_* script functions the same way (through a
 * host thunk the interpreter can call), so the Vita options rows added to the
 * .qvm screens (tools/vita_menu/build_menu.py) read and write the port's
 * settings (shim/include/vita_settings.h) with the game's own widgets. The rows
 * therefore look and behave exactly like the stock Options rows. */
#ifndef VITA_MENU_H
#define VITA_MENU_H

#include <stdint.h>

struct Ctx;

/* Registered from the hook on 0x402FF0 (runtime/src/igi_fixes.c), once the
 * executable has its config functions. */
void vita_menu_probe(struct Ctx *c, uint32_t func, uint32_t sp);

#endif
