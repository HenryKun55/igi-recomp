/* First-run patch of the game's compiled menus for the Vita options screen.
 *
 * Public users install only igi.vpk and copy the UNMODIFIED retail game data,
 * so the port cannot ship the edited menu scripts. Instead the eboot edits the
 * on-disk menusystem/mainmenu.qvm and ingamemenu.qvm the first time it runs
 * (keeping the original as <file>.orig, idempotent), adding the "Vita Settings"
 * screen (id 980) and a "Vita" entry to each Options screen -- the same change
 * tools/vita_menu/build_menu.py makes, reimplemented in C. The row titles are
 * supplied at run time by the menu-text hook (runtime/src/igi_controls.c), so
 * no language/<lang>/menusystem.res has to be touched.
 *
 * See runtime/src/igi_menu_patch.c. */
#ifndef IGI_MENU_PATCH_H
#define IGI_MENU_PATCH_H

/* Patch menusystem/mainmenu.qvm and ingamemenu.qvm under g_game_dir if they do
 * not already carry the Vita screen. Safe to call once at start-up; a missing
 * file, an unexpected layout or a parse failure leaves that file untouched. */
void igi_menu_patch_init(void);

#endif
