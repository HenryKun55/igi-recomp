/* The port's run-time settings, backed by igi.cfg. See shim/include/vita_settings.h.
 *
 * Each setting is a small enum (a choice among a few values). The table below
 * maps a setting to the igi.cfg key(s) it owns, the choices it cycles through
 * (with their file text), and an apply() that pushes the live value into the
 * subsystem that uses it. Reading parses the file once; writing rewrites it,
 * keeping every line the port does not own so a hand-edited file survives. */
#include "vita_settings.h"

#include "pad_input.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* cp1252 is the game's menu encoding; the labels here are plain ASCII. */

/* ---- current values ---- */

static int s_loaded;
static int s_value[VS_COUNT];     /* the current choice of each setting */

/* ---- per-choice data ---- */

/* A choice: the value written to the file and its menu label per language.
 * Only English and Portuguese are given; other languages fall back to English
 * (idx 0), matching the controls screen. */
#define L_EN 0
#define L_PT 1
#define N_SET_LANGS 2

typedef struct Choice {
    const char *file;             /* value as written to igi.cfg (primary key) */
    const char *label[N_SET_LANGS];
} Choice;

/* Right-stick sensitivity steps (file value is the multiplier). */
static const Choice sens_choices[] = {
    { "0.5",  { "x0.5",  "x0.5"  } },
    { "0.75", { "x0.75", "x0.75" } },
    { "1.0",  { "x1.0",  "x1.0"  } },
    { "1.5",  { "x1.5",  "x1.5"  } },
    { "2.0",  { "x2.0",  "x2.0"  } },
};
#define SENS_DEFAULT 2   /* x1.0 */

static const Choice aspect_choices[] = {
    { "fill", { "16:9", "16:9" } },
    { "keep", { "4:3",  "4:3"  } },
};

static const Choice touch_choices[] = {
    { "drag",  { "Drag",  "Arrastar" } },
    { "split", { "Split", "Dividido" } },
};

static const Choice onoff_choices[] = {
    { "0", { "Off", "Desl." } },
    { "1", { "On",  "Lig."  } },
};

/* Rear touchpad has three choices but is stored in two keys (rear_touch +
 * rear_zones); the file strings here are only placeholders (handled specially). */
static const Choice rear_choices[] = {
    { "zoom", { "Zoom",    "Zoom"     } },
    { "4",    { "4 zones", "4 zonas"  } },
    { "off",  { "Off",     "Desl."    } },
};

/* ---- the settings table ---- */

typedef struct SettingDef {
    const char *name[N_SET_LANGS];   /* the row's title */
    const Choice *choices;
    int count;
    int def;                         /* default choice index */
} SettingDef;

static const SettingDef defs[VS_COUNT] = {
    [VS_ASPECT] = {
        { "Aspect ratio", "Propor\xE7\xE3o" },
        aspect_choices, 2, 0 },
    [VS_TOUCH_MODE] = {
        { "Touch aim", "Mira por toque" },
        touch_choices, 2, 0 },
    [VS_ONSCREEN_BUTTONS] = {
        { "On-screen buttons", "Bot\xF5\x65s na tela" },
        onoff_choices, 2, 0 },
    [VS_REAR] = {
        { "Rear touchpad", "Touch traseiro" },
        rear_choices, 3, 0 },
    [VS_FRONT_TOUCH] = {
        { "Front touch", "Toque frontal" },
        onoff_choices, 2, 1 },
    [VS_SENSITIVITY] = {
        { "Look sensitivity", "Sensib. da mira" },
        sens_choices, 5, SENS_DEFAULT },
    [VS_OVERLAY] = {
        { "Performance overlay", "Painel de desempenho" },
        onoff_choices, 2, 0 },
    [VS_UPDATE_CHECK] = {
        { "Check for updates", "Buscar atualiza\xE7\xF5\x65s" },
        onoff_choices, 2, 1 },
};

/* ---- igi.cfg path ---- */

static const char *cfg_path(void)
{
#if defined(__vita__)
    return "ux0:data/igi/igi.cfg";
#else
    static char path[1100];
    const char *env = getenv("IGI_CFG");
    if (env && *env)
        return env;
    if (g_game_dir[0]) {
        snprintf(path, sizeof(path), "%s/igi.cfg", g_game_dir);
        return path;
    }
    return "igi.cfg";
#endif
}

/* ---- parsing ---- */

/* The value text of key=... in `line` (NUL-terminated line, no newline), or NULL. */
static const char *line_value(const char *line, const char *key)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0 || line[n] != '=')
        return NULL;
    return line + n + 1;
}

/* Choice index whose file text is a prefix of `val`, else -1. */
static int choice_of(const SettingDef *d, const char *val)
{
    for (int i = 0; i < d->count; i++)
        if (strncmp(val, d->choices[i].file, strlen(d->choices[i].file)) == 0)
            return i;
    return -1;
}

/* Rear touchpad is two keys: decide its choice from rear_touch + rear_zones. */
static int rear_choice(int rear_touch_on, int rear_zones)
{
    if (!rear_touch_on)
        return 2;             /* off */
    return rear_zones == 4 ? 1 : 0;   /* 4 zones, else zoom (2 zones) */
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void vita_settings_init(void)
{
    if (s_loaded)
        return;
    s_loaded = 1;
    for (int i = 0; i < VS_COUNT; i++)
        s_value[i] = defs[i].def;

    int rear_touch_on = 1, rear_zones = 2, have_rear = 0;
    FILE *f = fopen(cfg_path(), "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            const char *v;
            if ((v = line_value(line, "aspect"))) {
                int c = choice_of(&defs[VS_ASPECT], v);
                if (c >= 0)
                    s_value[VS_ASPECT] = c;
            } else if ((v = line_value(line, "touch_mode"))) {
                int c = choice_of(&defs[VS_TOUCH_MODE], v);
                if (c >= 0)
                    s_value[VS_TOUCH_MODE] = c;
            } else if ((v = line_value(line, "onscreen_buttons"))) {
                s_value[VS_ONSCREEN_BUTTONS] = v[0] != '0';
            } else if ((v = line_value(line, "front_touch"))) {
                s_value[VS_FRONT_TOUCH] = v[0] != '0';
            } else if ((v = line_value(line, "rear_touch"))) {
                rear_touch_on = v[0] != '0';
                have_rear = 1;
            } else if ((v = line_value(line, "rear_zones"))) {
                rear_zones = v[0] == '4' ? 4 : 2;
                have_rear = 1;
            } else if ((v = line_value(line, "stick_sensitivity"))) {
                float m = strtof(v, NULL);
                /* Nearest step; keeps a hand-written value within range. */
                int best = SENS_DEFAULT;
                float bestd = 1e9f;
                for (int i = 0; i < defs[VS_SENSITIVITY].count; i++) {
                    float d = strtof(defs[VS_SENSITIVITY].choices[i].file, NULL) - m;
                    if (d < 0)
                        d = -d;
                    if (d < bestd) {
                        bestd = d;
                        best = i;
                    }
                }
                s_value[VS_SENSITIVITY] = best;
            } else if ((v = line_value(line, "overlay"))) {
                s_value[VS_OVERLAY] = v[0] != '0';
            } else if ((v = line_value(line, "update_check"))) {
                s_value[VS_UPDATE_CHECK] = v[0] != '0';
            }
        }
        fclose(f);
    }
    if (have_rear)
        s_value[VS_REAR] = rear_choice(rear_touch_on, rear_zones);
    for (int i = 0; i < VS_COUNT; i++)
        s_value[i] = clampi(s_value[i], 0, defs[i].count - 1);
    rt_log("settings: aspect=%s touch=%s osb=%d rear=%s front=%d sens=%s overlay=%d update=%d",
           aspect_choices[s_value[VS_ASPECT]].file, touch_choices[s_value[VS_TOUCH_MODE]].file,
           s_value[VS_ONSCREEN_BUTTONS], rear_choices[s_value[VS_REAR]].file, s_value[VS_FRONT_TOUCH],
           sens_choices[s_value[VS_SENSITIVITY]].file, s_value[VS_OVERLAY], s_value[VS_UPDATE_CHECK]);
}

/* ---- typed getters ---- */

static int get(VitaSetting s)
{
    vita_settings_init();
    return s_value[s];
}

int vita_settings_aspect_keep(void) { return get(VS_ASPECT) == 1; }
int vita_settings_touch_mode(void) { return get(VS_TOUCH_MODE) == 1 ? PAD_TOUCH_SPLIT : PAD_TOUCH_DRAG; }
int vita_settings_onscreen_buttons(void) { return get(VS_ONSCREEN_BUTTONS); }
int vita_settings_front_touch(void) { return get(VS_FRONT_TOUCH); }
int vita_settings_rear_touch(void) { return get(VS_REAR) != 2; }
int vita_settings_rear_zones(void) { return get(VS_REAR) == 1 ? 4 : 2; }
int vita_settings_overlay(void) { return get(VS_OVERLAY); }
int vita_settings_update_check(void) { return get(VS_UPDATE_CHECK); }

float vita_settings_stick_sensitivity(void)
{
    return strtof(sens_choices[get(VS_SENSITIVITY)].file, NULL);
}

/* ---- menu-facing queries ---- */

int vita_setting_count(VitaSetting s)
{
    return s < VS_COUNT ? defs[s].count : 0;
}

int vita_setting_get(VitaSetting s)
{
    return s < VS_COUNT ? get(s) : 0;
}

static unsigned set_lang_col(unsigned lang)
{
    /* igi_controls.c LANG_PORTUGUESE == 6; everything else falls back to English. */
    return lang == 6u ? L_PT : L_EN;
}

const char *vita_setting_value_label(VitaSetting s, unsigned lang)
{
    if (s >= VS_COUNT)
        return "";
    return defs[s].choices[get(s)].label[set_lang_col(lang)];
}

const char *vita_setting_name(VitaSetting s, unsigned lang)
{
    if (s >= VS_COUNT)
        return "";
    return defs[s].name[set_lang_col(lang)];
}

const char *vita_setting_choice_label(VitaSetting s, int choice)
{
    if (s >= VS_COUNT || choice < 0 || choice >= defs[s].count)
        return "";
    return defs[s].choices[choice].label[L_EN];
}

/* ---- live apply ---- */

/* Each subsystem re-reads the getters when asked. These are weak so the module
 * builds and self-tests on hosts that leave a subsystem out; the real ones live
 * in render_gl.c and platform_vita.c. */
void render_apply_settings(void) __attribute__((weak));
void render_apply_settings(void) {}
void platform_vita_apply_settings(void) __attribute__((weak));
void platform_vita_apply_settings(void) {}

static void apply(VitaSetting s)
{
    switch (s) {
    case VS_ASPECT:
    case VS_OVERLAY:
        render_apply_settings();
        break;
    case VS_TOUCH_MODE:
    case VS_ONSCREEN_BUTTONS:
    case VS_REAR:
    case VS_FRONT_TOUCH:
    case VS_SENSITIVITY:
        platform_vita_apply_settings();
        break;
    case VS_UPDATE_CHECK:   /* read once at start-up; only the file matters */
    default:
        break;
    }
}

/* ---- write-back ---- */

/* The igi.cfg key(s) a setting owns and the value text to write for a choice.
 * Rear touchpad owns two keys, so this fills up to two (key,value) pairs. */
typedef struct KV {
    const char *key, *val;
} KV;

static int setting_keys(VitaSetting s, int choice, KV out[2])
{
    switch (s) {
    case VS_ASPECT:           out[0] = (KV){ "aspect", aspect_choices[choice].file }; return 1;
    case VS_TOUCH_MODE:       out[0] = (KV){ "touch_mode", touch_choices[choice].file }; return 1;
    case VS_ONSCREEN_BUTTONS: out[0] = (KV){ "onscreen_buttons", onoff_choices[choice].file }; return 1;
    case VS_FRONT_TOUCH:      out[0] = (KV){ "front_touch", onoff_choices[choice].file }; return 1;
    case VS_SENSITIVITY:      out[0] = (KV){ "stick_sensitivity", sens_choices[choice].file }; return 1;
    case VS_OVERLAY:          out[0] = (KV){ "overlay", onoff_choices[choice].file }; return 1;
    case VS_UPDATE_CHECK:     out[0] = (KV){ "update_check", onoff_choices[choice].file }; return 1;
    case VS_REAR:
        /* zoom -> rear_touch=1 rear_zones=2; 4 zones -> =1 =4; off -> rear_touch=0 (zones kept). */
        out[0] = (KV){ "rear_touch", choice == 2 ? "0" : "1" };
        out[1] = (KV){ "rear_zones", choice == 1 ? "4" : "2" };
        return choice == 2 ? 1 : 2;   /* off only rewrites rear_touch */
    default:
        return 0;
    }
}

/* All igi.cfg keys the module owns (used to drop stale lines on write). */
static const char *const owned_keys[] = {
    "aspect", "touch_mode", "onscreen_buttons", "front_touch", "rear_touch", "rear_zones",
    "stick_sensitivity", "overlay", "update_check",
};
#define N_OWNED (int)(sizeof(owned_keys) / sizeof(owned_keys[0]))

static int is_owned_key(const char *line)
{
    for (int i = 0; i < N_OWNED; i++) {
        size_t n = strlen(owned_keys[i]);
        if (strncmp(line, owned_keys[i], n) == 0 && line[n] == '=')
            return 1;
    }
    return 0;
}

/* Rewrite igi.cfg: keep every line the port does not own, in place, and emit
 * the owned keys with their current values (updating a line in place when it is
 * already there, else appending the missing ones at the end). */
static void write_cfg(void)
{
    const char *path = cfg_path();

    /* The owned keys and the value each must get now. */
    KV pairs[16];
    int np = 0;
    for (int s = 0; s < VS_COUNT; s++) {
        KV kv[2];
        int n = setting_keys((VitaSetting)s, s_value[s], kv);
        for (int i = 0; i < n && np < (int)(sizeof(pairs) / sizeof(pairs[0])); i++)
            pairs[np++] = kv[i];
    }

    /* Read the existing file (keeping unknown lines verbatim). */
    char (*lines)[512] = NULL;
    int nlines = 0, cap = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        char buf[512];
        while (fgets(buf, sizeof(buf), in)) {
            if (nlines == cap) {
                cap = cap ? cap * 2 : 64;
                char (*grown)[512] = realloc(lines, (size_t)cap * sizeof(*lines));
                if (!grown)
                    break;
                lines = grown;
            }
            buf[strcspn(buf, "\r\n")] = 0;
            snprintf(lines[nlines++], sizeof(lines[0]), "%s", buf);
        }
        fclose(in);
    }

    FILE *out = fopen(path, "w");
    if (!out) {
        rt_log("settings: cannot write %s", path);
        free(lines);
        return;
    }
    char wrote[16] = { 0 };   /* 1 once a pair has been emitted */
    for (int l = 0; l < nlines; l++) {
        if (is_owned_key(lines[l])) {
            /* An owned line: rewrite the first unwritten pair with this key in
             * place (keeping its position in the file); any other owned line
             * (a duplicate, or a key no longer written such as rear_zones when
             * the rear panel is off) is dropped. */
            for (int p = 0; p < np; p++) {
                size_t kn = strlen(pairs[p].key);
                if (!wrote[p] && strncmp(lines[l], pairs[p].key, kn) == 0 && lines[l][kn] == '=') {
                    fprintf(out, "%s=%s\n", pairs[p].key, pairs[p].val);
                    wrote[p] = 1;
                    break;
                }
            }
            continue;
        }
        fputs(lines[l], out);
        fputc('\n', out);
    }
    /* Append the owned keys not already present in the file. */
    for (int p = 0; p < np; p++)
        if (!wrote[p])
            fprintf(out, "%s=%s\n", pairs[p].key, pairs[p].val);
    fclose(out);
    free(lines);
}

void vita_setting_set(VitaSetting s, int value)
{
    vita_settings_init();
    if (s >= VS_COUNT)
        return;
    value = clampi(value, 0, defs[s].count - 1);
    if (value == s_value[s])
        return;
    s_value[s] = value;
    apply(s);
    write_cfg();
    rt_log("settings: %s -> %s", defs[s].name[L_EN], defs[s].choices[value].label[L_EN]);
}

void vita_setting_cycle(VitaSetting s, int delta)
{
    vita_settings_init();
    if (s >= VS_COUNT)
        return;
    int n = defs[s].count;
    int v = ((s_value[s] + delta) % n + n) % n;
    vita_setting_set(s, v);
}
