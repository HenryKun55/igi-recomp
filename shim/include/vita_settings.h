/* Central store for the port's run-time settings (ux0:data/igi/igi.cfg).
 *
 * One source of truth for every tunable the port exposes: the file is parsed
 * once into typed values (vita_settings_init), read through the getters below
 * (the renderer, the touch layer and the update check call them instead of
 * reading the file themselves) and changed through the setters, which apply the
 * value live where they can and write igi.cfg back, keeping every line the port
 * does not own (comments, blank lines and unknown keys) and the file's order.
 *
 * The native Vita options screen (runtime/src/vita_menu.c, reached from the
 * game's own Options menu) is only a front end over these setters; a
 * hand-edited igi.cfg stays valid and is read back the same way on the next
 * launch.
 *
 * The file path is "ux0:data/igi/igi.cfg" on the Vita; on the desktop it is
 * $IGI_CFG, or <game dir>/igi.cfg when that is set, so the same code can be
 * exercised without a handheld. */
#ifndef VITA_SETTINGS_H
#define VITA_SETTINGS_H

/* Setting ids, in the order the options screen shows them. */
typedef enum VitaSetting {
    VS_ASPECT,            /* 0 fill (16:9), 1 keep (4:3) */
    VS_TOUCH_MODE,        /* 0 drag, 1 split */
    VS_ONSCREEN_BUTTONS,  /* 0 off, 1 on */
    VS_REAR,              /* 0 zoom (2 zones), 1 four zones, 2 off */
    VS_FRONT_TOUCH,       /* 0 off, 1 on */
    VS_SENSITIVITY,       /* index into the sensitivity steps */
    VS_OVERLAY,           /* 0 off, 1 on */
    VS_UPDATE_CHECK,      /* 0 off, 1 on */
    VS_COUNT
} VitaSetting;

/* Parse igi.cfg into the typed values (defaults where a key is absent or bad).
 * Safe to call more than once and from any thread before the menus run; the
 * first getter call also triggers it. */
void vita_settings_init(void);

/* The number of choices a setting cycles through, and the current one (0-based,
 * always a valid index). */
int vita_setting_count(VitaSetting s);
int vita_setting_get(VitaSetting s);
/* Set the choice (clamped to range), apply it live where possible and write
 * igi.cfg. A no-op when the value does not change. */
void vita_setting_set(VitaSetting s, int value);
/* Step the choice by +1/-1 with wrap-around (what a menu click does). */
void vita_setting_cycle(VitaSetting s, int delta);

/* Short label of a setting's current choice, in the game's cp1252 encoding, for
 * the menu row (e.g. "16:9", "4 zones", "x1.5", "On"). `lang` is a game
 * language id (igi_controls.c LANG_*); unknown languages fall back to English. */
const char *vita_setting_value_label(VitaSetting s, unsigned lang);
/* English short label of a specific choice (0-based), for a ListBox's items. */
const char *vita_setting_choice_label(VitaSetting s, int choice);
/* The setting's own name for the menu row (e.g. "Aspect ratio"). */
const char *vita_setting_name(VitaSetting s, unsigned lang);

/* ---- typed getters used by the rest of the port ---- */

/* Aspect: 1 keep 4:3 (black bars), 0 fill the display (16:9). */
int vita_settings_aspect_keep(void);
/* Touch aim mode: 0 drag, 1 split (shim/include/pad_input.h PAD_TOUCH_*). */
int vita_settings_touch_mode(void);
int vita_settings_onscreen_buttons(void);   /* 1 draw the on-screen buttons */
int vita_settings_front_touch(void);         /* 1 front panel on */
int vita_settings_rear_touch(void);          /* 1 rear panel on */
int vita_settings_rear_zones(void);          /* 2 or 4 */
float vita_settings_stick_sensitivity(void); /* right-stick multiplier */
int vita_settings_overlay(void);             /* 1 show the performance overlay */
int vita_settings_update_check(void);        /* 1 check for updates at start-up */

#endif
