/* PlayStation Vita implementation of the platform layer: vitaGL for video,
 * sceCtrl and the touch panels sent through IGI's key bindings, SDL for audio. */
#if defined(__vita__)
#include "platform.h"
#include "bench.h"
#include "igi_fixes.h"
#include "pad_input.h"
#include "runtime.h"
#include "vita_bootui.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#define SCREEN_W 960
#define SCREEN_H 544

static int mode_w = 640, mode_h = 480;
static int gl_ready;

/* ---- boot picture --------------------------------------------------------
 * The game loads for a few seconds before its first frame (the intro video), and
 * vitaGL only starts with the display mode. Meanwhile the loading screen picture
 * (LOCAL:menusystem/loading.pic in menusystem/loadingscreen.res, the art of
 * sce_sys/pic0.png and in the same place) is shown from a plain display buffer,
 * released once vitaGL has presented two frames. */
#define BOOT_FB_SIZE 0x200000u   /* 960 x 544 x 4 rounded up to the CDRAM granularity */
#define PIC_W 640
#define PIC_H 480

static SceUID boot_fb_block = -1;
static uint32_t *boot_fb_base;
static int boot_fb_swaps;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The loading picture as 640x480 BGRA (malloc'd), or NULL. A .res archive is a
 * chain of (tag, size, alignment, offset of the next) chunks after a 20-byte
 * header, a NAME chunk before each BODY; a .pic body is a table of 64x32 BGRA
 * tiles (offset, format 3, flags, width, height, pitch), 10 per row here. */
static uint8_t *boot_picture_load(void)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/menusystem/loadingscreen.res", g_game_dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    size_t cap = 0x140000u;   /* the first picture is the first entry: 1.2 MB in */
    uint8_t *res = malloc(cap);
    size_t n = res ? fread(res, 1, cap, f) : 0;
    fclose(f);
    uint8_t *out = NULL;
    if (n < 20u || memcmp(res, "ILFF", 4) || memcmp(res + 16, "IRES", 4))
        goto done;
    const char *name = "";
    for (size_t pos = 20; pos + 16u <= n;) {
        uint32_t size = le32(res + pos + 4), next = le32(res + pos + 12);
        const uint8_t *body = res + pos + 16;
        if (pos + 16u + size > n)
            break;
        if (!memcmp(res + pos, "NAME", 4)) {
            name = (const char *)body;
        } else if (!memcmp(res + pos, "BODY", 4) && !strcmp(name, "LOCAL:menusystem/loading.pic") &&
                   size >= 0x34u && !memcmp(body, "LOOP", 4)) {
            uint32_t tiles = le32(body + 0x20);
            if (tiles != (PIC_W / 64) * (PIC_H / 32) || 0x34u + tiles * 0x20u > size)
                break;
            out = malloc(PIC_W * PIC_H * 4);
            for (uint32_t i = 0; out && i < tiles; i++) {
                const uint8_t *e = body + 0x34u + i * 0x20u;
                uint32_t off = le32(e), w = (uint32_t)e[10] | (uint32_t)e[11] << 8,
                         h = (uint32_t)e[12] | (uint32_t)e[13] << 8;
                if (le32(e + 4) != 3u || w != 64u || h != 32u || off + 64u * 32u * 4u > size) {
                    free(out);
                    out = NULL;
                    break;
                }
                uint32_t x0 = (i % (PIC_W / 64)) * 64u, y0 = (i / (PIC_W / 64)) * 32u;
                for (uint32_t y = 0; y < 32u; y++)
                    memcpy(out + ((y0 + y) * PIC_W + x0) * 4u, body + off + y * 64u * 4u, 64u * 4u);
            }
            break;
        }
        if (!next)
            break;
        pos += next;
    }
done:
    free(res);
    return out;
}

static void boot_picture_show(void)
{
    uint8_t *pic = boot_picture_load();
    if (!pic) {
        rt_log("boot picture: menusystem/loadingscreen.res not usable");
        return;
    }
    SceUID block = sceKernelAllocMemBlock("igi_boot_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, BOOT_FB_SIZE, NULL);
    void *base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
        free(pic);
        return;
    }
    /* Height fitted (725x544, aspect kept) and centered, bilinear, like pic0.png;
     * the picture's edges are black already. 16.16 fixed point, no divisions per pixel. */
    const int dw = PIC_W * SCREEN_H / PIC_H, dx0 = (SCREEN_W - dw) / 2;
    const uint32_t step_x = (uint32_t)(((uint64_t)PIC_W << 16) / (uint32_t)dw);
    const uint32_t step_y = (uint32_t)(((uint64_t)PIC_H << 16) / SCREEN_H);
    uint32_t *fb = base;
    memset(fb, 0, BOOT_FB_SIZE);
    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t fy = (uint32_t)y * step_y + step_y / 2u - 0x8000u;
        uint32_t sy = fy >> 16, wy = (fy >> 8) & 0xFFu;
        if ((int32_t)fy < 0)
            sy = 0, wy = 0;
        uint32_t sy1 = sy + 1u < PIC_H ? sy + 1u : sy;
        uint32_t *row = fb + (size_t)y * SCREEN_W + dx0;
        for (int x = 0; x < dw; x++) {
            uint32_t fx = (uint32_t)x * step_x + step_x / 2u - 0x8000u;
            uint32_t sx = fx >> 16, wx = (fx >> 8) & 0xFFu;
            if ((int32_t)fx < 0)
                sx = 0, wx = 0;
            uint32_t sx1 = sx + 1u < PIC_W ? sx + 1u : sx;
            const uint8_t *p00 = pic + (sy * PIC_W + sx) * 4u, *p01 = pic + (sy * PIC_W + sx1) * 4u;
            const uint8_t *p10 = pic + (sy1 * PIC_W + sx) * 4u, *p11 = pic + (sy1 * PIC_W + sx1) * 4u;
            uint32_t rgba = 0xFF000000u;
            for (int ch = 0; ch < 3; ch++) {   /* BGRA in, ABGR (R first in memory) out */
                uint32_t top = p00[ch] * (256u - wx) + p01[ch] * wx;
                uint32_t bot = p10[ch] * (256u - wx) + p11[ch] * wx;
                uint32_t v = (top * (256u - wy) + bot * wy) >> 16;
                rgba |= v << (8 * (2 - ch));
            }
            row[x] = rgba;
        }
    }
    free(pic);
    SceDisplayFrameBuf dfb;
    memset(&dfb, 0, sizeof(dfb));
    dfb.size = sizeof(dfb);
    dfb.base = base;
    dfb.pitch = SCREEN_W;
    dfb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    dfb.width = SCREEN_W;
    dfb.height = SCREEN_H;
    if (sceDisplaySetFrameBuf(&dfb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0) {
        sceKernelFreeMemBlock(block);
        return;
    }
    boot_fb_block = block;
    boot_fb_base = base;
}

void platform_init(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);   /* for L2/R2/L3/R3 (Ext2 buffer) */
    boot_picture_show();
    /* The version and the update check over the boot picture (vita_bootui.c). */
    vita_bootui_run(boot_fb_base);
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
        rt_fatal(NULL, "SDL_Init: %s", SDL_GetError());
}

void platform_create_window(int w, int h, const char *title)
{
    (void)title;
    mode_w = w;
    mode_h = h;
}

void platform_show_window(int show) { (void)show; }
void platform_set_title(const char *title) { (void)title; }
static int exit_watchdog(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitProcess(0);
    return 0;
}

/* SDL_Quit is skipped: after a fatal error (MessageBox + ExitProcess) it was
 * seen not to return on hardware, leaving the process alive in the background
 * (the shell could not start another app until a reboot). Exiting the process
 * stops every thread, the audio one included, before the memory goes away.
 * The render thread is stopped first (its GPU work must end before the memory goes);
 * should that never finish (a stuck GPU job), a watchdog ends the process anyway. */
void platform_shutdown(void)
{
    SceUID w = sceKernelCreateThread("igi_exit_watchdog", exit_watchdog, 0x10000100, 0x4000, 0, 0, NULL);
    if (w >= 0)
        sceKernelStartThread(w, 0, NULL);
    render_shutdown();
    sceKernelExitProcess(0);
}

void platform_show_cursor(int show) { (void)show; }
void platform_capture_mouse(int on) { (void)on; }

void platform_set_video_mode(int w, int h)
{
    mode_w = w;
    mode_h = h;
}

/* On the thread that makes every GL call (see render_queue.c). */
void platform_gl_init(void)
{
    if (!gl_ready) {
        /* 8 MiB for immediate-mode/legacy state; textures go to vitaGL's own pools. */
        vglInitExtended(0x800000, SCREEN_W, SCREEN_H, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
        gl_ready = 1;
    }
}

void platform_gl_release(void) {}
void platform_service_main(void) {}

void platform_get_video_mode(int *w, int *h)
{
    *w = mode_w;
    *h = mode_h;
}

void platform_get_drawable_size(int *w, int *h)
{
    *w = SCREEN_W;
    *h = SCREEN_H;
}

void platform_swap(void)
{
    vglSwapBuffers(GL_FALSE);
    /* vitaGL's buffers are on screen: the boot picture's can go. */
    if (boot_fb_block >= 0 && ++boot_fb_swaps >= 2) {
        sceDisplayWaitVblankStart();
        sceKernelFreeMemBlock(boot_fb_block);
        boot_fb_block = -1;
    }
}

/* ---- controls -----------------------------------------------------------
 * Every button, the left stick's four directions and the rear panel's halves
 * are keys of their own that the player binds in Options -> Controls; in the
 * menus they work as a console's (pad_input.c). Defaults (runtime/src/igi_controls.c):
 *
 *   left stick  move / strafe          right stick  look (mouse)
 *   R  fire                            L  crouch
 *   X  jump                            O  activate
 *   [] reload                          /\ next weapon
 *   up  binoculars                     down  walk / run
 *   left  previous weapon              right  map computer
 *   SELECT  peek                       START  Esc (menu), not bindable
 *   rear touch   upper half zoom in, lower half zoom out (scope, binoculars)
 *   front touch  drag to look; in the menus, tap to click
 *
 * igi.cfg: front_touch=0 / rear_touch=0 turn a touch panel off;
 * stick_sensitivity=<multiplier> scales the right stick in gameplay (default 1.0;
 * the menu cursor speed is fixed). */

static const struct {
    uint32_t button;
    PadControl control;
} button_map[] = {
    { SCE_CTRL_CROSS, PAD_CROSS },
    { SCE_CTRL_CIRCLE, PAD_CIRCLE },
    { SCE_CTRL_SQUARE, PAD_SQUARE },
    { SCE_CTRL_TRIANGLE, PAD_TRIANGLE },
    { SCE_CTRL_LTRIGGER, PAD_L },
    { SCE_CTRL_RTRIGGER, PAD_R },
    { SCE_CTRL_SELECT, PAD_SELECT },
    { SCE_CTRL_START, PAD_START },
    { SCE_CTRL_UP, PAD_DPAD_UP },
    { SCE_CTRL_DOWN, PAD_DPAD_DOWN },
    { SCE_CTRL_LEFT, PAD_DPAD_LEFT },
    { SCE_CTRL_RIGHT, PAD_DPAD_RIGHT },
};
#define N_BUTTONS (sizeof(button_map) / sizeof(button_map[0]))

/* Spare external-controller buttons, read with sceCtrlPeekBufferPositiveExt2.
 * On that buffer the Vita's own L/R triggers appear as L1/R1 (bits 0x400/0x800,
 * already PAD_L/PAD_R above), so L2/R2/L3/R3 never fire on a handheld and only
 * reach a player who plugs in a controller / uses a PS TV. Unbound by default. */
static const struct {
    uint32_t button;
    PadControl control;
} ext_button_map[] = {
    { SCE_CTRL_L2, PAD_L2 },
    { SCE_CTRL_R2, PAD_R2 },
    { SCE_CTRL_L3, PAD_L3 },
    { SCE_CTRL_R3, PAD_R3 },
};
#define N_EXT_BUTTONS (sizeof(ext_button_map) / sizeof(ext_button_map[0]))

/* Left stick directions: up, down, left, right. */
static const PadControl stick_controls[4] = { PAD_LSTICK_UP, PAD_LSTICK_DOWN, PAD_LSTICK_LEFT, PAD_LSTICK_RIGHT };

static uint32_t prev_buttons, prev_ext_buttons;
static uint8_t stick_held[4];

#define DEADZONE 28
static int deadzone(int v)
{
    v -= 128;
    if (v > -DEADZONE && v < DEADZONE)
        return 0;
    return v;
}

static void post_mouse_move(void)
{
    user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
}

/* Relative mouse motion (looking), the cursor kept on the screen. */
static void mouse_motion(int32_t mx, int32_t my)
{
    if (!mx && !my)
        return;
    g_mouse.dx += mx;
    g_mouse.dy += my;
    g_mouse.x += mx;
    g_mouse.y += my;
    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x >= mode_w) g_mouse.x = mode_w - 1;
    if (g_mouse.y >= mode_h) g_mouse.y = mode_h - 1;
    post_mouse_move();
}

/* ---- touch panels ---- */

#define TOUCH_LOOK_SCALE 0.5f   /* mouse counts per front panel unit (1920 across) */

static int front_touch_on = 1, rear_touch_on = 1, aspect_keep;
static int rear_mid_y = 544;     /* rear panel: above = zoom in, below = zoom out */
static int rear_mid_x = 960;     /* four-zone mode: left / right of this */
static int rear_zones = 2;       /* igi.cfg rear_zones= (2 up/down, or 4 quadrants) */
static int front_w = 1920, front_h = 1088;
static float stick_sensitivity = 1.0f;   /* igi.cfg stick_sensitivity= */

static void touch_init(void)
{
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "front_touch=", 12))
                front_touch_on = line[12] != '0';
            else if (!strncmp(line, "rear_touch=", 11))
                rear_touch_on = line[11] != '0';
            else if (!strncmp(line, "rear_zones=", 11))
                rear_zones = line[11] == '4' ? 4 : 2;
            else if (!strncmp(line, "aspect=", 7))
                aspect_keep = !strncmp(line + 7, "keep", 4);   /* as render_gl.c: fill unless keep */
            else if (!strncmp(line, "stick_sensitivity=", 18))
                stick_sensitivity = strtof(line + 18, NULL);
        }
        fclose(f);
    }
    if (!(stick_sensitivity >= 0.1f && stick_sensitivity <= 10.0f))
        stick_sensitivity = 1.0f;
    rt_log("right stick: sensitivity x%.2f", (double)stick_sensitivity);
    SceTouchPanelInfo info;
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &info) >= 0 && info.maxAaX > info.minAaX &&
        info.maxAaY > info.minAaY) {
        front_w = info.maxAaX + 1;
        front_h = info.maxAaY + 1;
    }
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &info) >= 0 && info.maxAaY > info.minAaY) {
        rear_mid_y = (info.minAaY + info.maxAaY) / 2;
        if (info.maxAaX > info.minAaX)
            rear_mid_x = (info.minAaX + info.maxAaX) / 2;
    }
    if (front_touch_on)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    if (rear_touch_on)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    rt_log("touch: front %s (%dx%d), rear %s (%d zones, split y=%d x=%d)", front_touch_on ? "on" : "off", front_w,
           front_h, rear_touch_on ? "on" : "off", rear_zones, rear_mid_y, rear_mid_x);
}

/* Front panel position -> game screen position, through the image's place on
 * the display (stretched, or 4:3 with bars with aspect=keep). */
static void touch_to_game(int tx, int ty, int32_t *gx, int32_t *gy)
{
    int x0 = 0, w = front_w;
    if (aspect_keep) {
        w = front_h * 4 / 3;
        x0 = (front_w - w) / 2;
    }
    int x = (int)((int64_t)(tx - x0) * mode_w / w), y = (int)((int64_t)ty * mode_h / front_h);
    *gx = x < 0 ? 0 : x >= mode_w ? mode_w - 1 : x;
    *gy = y < 0 ? 0 : y >= mode_h ? mode_h - 1 : y;
}

/* How the finger drives the mouse while the touch is down. */
enum FrontMode { FRONT_LOOK, FRONT_MENU, FRONT_COMPUTER };

/* Front: in play a drag turns the view like the mouse; in the menus the cursor
 * goes where the finger is and a tap clicks (button down once the cursor is
 * there, up when the finger lifts). The map computer keeps its own cursor and
 * moves it only by relative motion (igi_computer_active, igi_fixes.h), so there
 * the finger works like a trackpad: a drag moves that cursor, a tap (a short
 * touch that barely moved) clicks under it, landing on the computer's tabs. */
#define TAP_MOVE 24          /* a touch that moves within this (panel units) is a tap */
#define TAP_FRAMES 20        /* ... and lasts at most this many input polls */
#define TAP_CLICK_HOLD 4     /* input polls the tap click is held down (the game must poll it) */
static int tap_click;        /* > 0: a computer tap click is held, released after this many polls */
static void front_touch(void)
{
    static int active, clicking, last_x, last_y, moved, frames;
    static enum FrontMode mode;
    static float acc_x, acc_y;
    /* Release a held computer tap click once the game has had polls to see it. */
    if (tap_click && --tap_click == 0)
        pad_mouse_button(0, 0);
    SceTouchData t;
    int down = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) >= 0 && t.reportNum > 0;
    if (!down) {
        if (clicking)
            pad_mouse_button(0, 0);
        /* A tap in the computer clicks on release, where the cursor ended up; the
         * button is held a few polls so the game's own input poll catches it. */
        if (active && mode == FRONT_COMPUTER && !clicking && !tap_click && moved <= TAP_MOVE &&
            frames <= TAP_FRAMES && igi_computer_active()) {
            pad_mouse_button(0, 1);
            tap_click = TAP_CLICK_HOLD;
        }
        active = clicking = 0;
        return;
    }
    int x = t.report[0].x, y = t.report[0].y;
    if (!active) {
        active = 1;
        mode = igi_computer_active() ? FRONT_COMPUTER : pad_in_gameplay() ? FRONT_LOOK : FRONT_MENU;
        last_x = x;
        last_y = y;
        moved = frames = 0;
        acc_x = acc_y = 0.0f;
        if (mode == FRONT_LOOK)
            return;
    }
    if (mode == FRONT_MENU) {
        int32_t gx, gy;
        touch_to_game(x, y, &gx, &gy);
        if (gx != g_mouse.x || gy != g_mouse.y) {
            g_mouse.dx += gx - g_mouse.x;
            g_mouse.dy += gy - g_mouse.y;
            g_mouse.x = gx;
            g_mouse.y = gy;
            post_mouse_move();
        } else if (!clicking && !pad_capturing_key()) {
            /* (no click while the controls screen waits for a key: it would
             * bind the mouse button, which the pad has not in play) */
            clicking = 1;
            pad_mouse_button(0, 1);
        }
        return;
    }
    /* FRONT_LOOK and FRONT_COMPUTER: relative motion from the finger's drag. */
    int dx = x - last_x, dy = y - last_y;
    moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    frames++;
    acc_x += (float)dx * TOUCH_LOOK_SCALE;
    acc_y += (float)dy * TOUCH_LOOK_SCALE;
    last_x = x;
    last_y = y;
    int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
    acc_x -= (float)mx;
    acc_y -= (float)my;
    mouse_motion(mx, my);
}

/* Rear: two zones (upper/lower halves, defaults zoom in / zoom out) or, with
 * rear_zones=4, four quadrant controls (unbound by default, see igi_controls.h). */
static void rear_touch(void)
{
    static int held = -1;
    SceTouchData t;
    int want = -1;
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &t, 1) >= 0 && t.reportNum > 0) {
        int up = t.report[0].y < rear_mid_y;
        if (rear_zones == 4) {
            int left = t.report[0].x < rear_mid_x;
            want = up ? (left ? PAD_REAR_UL : PAD_REAR_UR) : (left ? PAD_REAR_DL : PAD_REAR_DR);
        } else {
            want = up ? PAD_REAR_UP : PAD_REAR_DOWN;
        }
    }
    if (want == held)
        return;
    if (held >= 0)
        pad_control((PadControl)held, 0);
    if (want >= 0)
        pad_control((PadControl)want, 1);
    held = want;
}

/* Right stick deflection (deadzone(): 0, or 28 to about 100 at full tilt, signed)
 * -> mouse counts per second.
 *   - Gameplay: linear, 9 counts/s per unit (about 900/s at full tilt), times
 *     stick_sensitivity; the game's mouse sensitivity (Options -> Controls) applies
 *     on top, so players tune the look speed there.
 *   - Menus: a fixed, slow cursor (a curve that starts slow and reaches
 *     MENU_CURSOR_MAX at full tilt), not affected by the settings. */
#define LOOK_PER_UNIT 9.0f
#define MENU_CURSOR_MAX 650.0f
static float look_speed(int v)
{
    if (!v)
        return 0.0f;
    if (pad_in_gameplay())
        return (float)v * LOOK_PER_UNIT * stick_sensitivity;
    float n = (float)((v < 0 ? -v : v) - DEADZONE) / (float)(100 - DEADZONE);
    if (n > 1.0f)
        n = 1.0f;
    float s = MENU_CURSOR_MAX * (0.25f * n + 0.75f * n * n);
    return v < 0 ? -s : s;
}

void platform_pump(void)
{
    static uint64_t last_look, last_pump;
    static int touch_ready;
    platform_scripted_input();
    if (bench_enabled())
        return;   /* the benchmark drives the game; the pad would disturb it */
    /* The game polls input and messages many times per frame; the pad only
     * needs sampling every couple of milliseconds. */
    uint64_t t = sceKernelGetProcessTimeWide();
    if (t - last_pump < 2000u)
        return;
    last_pump = t;
    if (!touch_ready) {
        touch_ready = 1;
        touch_init();
    }
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;
    for (unsigned i = 0; i < N_BUTTONS; i++) {
        uint32_t b = button_map[i].button;
        int down = (pad.buttons & b) != 0;
        if (down != ((prev_buttons & b) != 0))
            pad_control(button_map[i].control, down);
    }
    /* Debug combos, only with the performance overlay on (overlay=1 in igi.cfg). */
    const uint32_t diag = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
    const int debug = render_overlay_enabled();
    if (debug && (pad.buttons & diag) == diag && (prev_buttons & diag) != diag)
        render_diag_cycle();
    const uint32_t trace = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    if (debug && (pad.buttons & trace) == trace && (prev_buttons & trace) != trace && !g_trace_next_frame)
        g_trace_next_frame = 1;   /* log the next frame's draws and save its image */
    prev_buttons = pad.buttons;

    /* Spare external-controller buttons (L2/R2/L3/R3), from the extended buffer. */
    SceCtrlData ext;
    if (sceCtrlPeekBufferPositiveExt2(0, &ext, 1) >= 0) {
        for (unsigned i = 0; i < N_EXT_BUTTONS; i++) {
            uint32_t b = ext_button_map[i].button;
            int down = (ext.buttons & b) != 0;
            if (down != ((prev_ext_buttons & b) != 0))
                pad_control(ext_button_map[i].control, down);
        }
        prev_ext_buttons = ext.buttons;
    }

    /* Left stick: four controls, one per direction. */
    int lx = deadzone(pad.lx), ly = deadzone(pad.ly);
    uint8_t want[4] = { ly < -40, ly > 40, lx < -40, lx > 40 };
    for (int i = 0; i < 4; i++) {
        if (want[i] != stick_held[i]) {
            stick_held[i] = want[i];
            pad_control(stick_controls[i], want[i]);
        }
    }

    if (front_touch_on)
        front_touch();
    if (rear_touch_on)
        rear_touch();

    /* Right stick: relative mouse motion, scaled by elapsed time. */
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_look) {
        float dt = (float)(now - last_look) / 1000000.0f;
        if (dt > 0.1f)
            dt = 0.1f;
        int rx = deadzone(pad.rx), ry = deadzone(pad.ry);
        static float acc_x, acc_y;
        acc_x += look_speed(rx) * dt;
        acc_y += look_speed(ry) * dt;
        int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
        acc_x -= (float)mx;
        acc_y -= (float)my;
        mouse_motion(mx, my);
    }
    last_look = now;
}

void platform_wait(void)
{
    SDL_Delay(5);
    platform_pump();
}

#endif
