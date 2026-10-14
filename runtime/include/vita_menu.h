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

/* The Vita screen's own menu texts (the row titles, the "Vita" entry and the
 * screen title) come from the port, not menusystem.res, so no language file has
 * to be edited to ship the screen. The menu-text hook (0x418EA0, served by
 * runtime/src/igi_controls.c) calls this for a key it does not recognise: when
 * `key` (a guest string pointer) is one of those texts, it returns a guest
 * pointer to the text for the game language id `lang`, otherwise 0. */
uint32_t vita_menu_text(uint32_t key, uint32_t lang);

#endif
