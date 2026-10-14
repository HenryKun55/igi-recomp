/* PlayStation Vita implementation of the platform layer: vitaGL for video,
 * sceCtrl and the touch panels sent through IGI's key bindings, SDL for audio. */
#if defined(__vita__)
#include "platform.h"
#include "bench.h"
#include "igi_fixes.h"
#include "pad_input.h"
#include "runtime.h"
#include "vita_bootui.h"
#include "vita_settings.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#include "render_backend.h"   /* ButtonsCmd (after the GL header) */

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

/* ---- suspend / resume ----------------------------------------------------
 * The PS button (background the app) and sleep (power button or auto-sleep)
 * suspend the process. The game breaks only if its render thread keeps
 * submitting GPU scenes (sceGxmBeginScene/EndScene, sceDisplaySetFrameBuf) while
 * the system has the display suspended -- that can hang the GPU and force a
 * reboot. The GXM context and GPU resources themselves live in RAM, which is
 * preserved across background/sleep, so there is nothing to rebuild.
 *
 * So we treat it as a plain pause: on SUSPEND we park the game loop at its input
 * checkpoint (the game thread stops producing frames, so the render thread drains
 * its queue and idles -- no new GPU work) and pause audio. On RESUME we unpark
 * the loop and resume audio, and the game continues exactly where it was. No
 * process exit, no context recreation.
 *
 * The callback is a kernel callback: it only runs while a thread sits in a
 * *CB syscall, so a dedicated thread loops on sceKernelDelayThreadCB. */

static _Atomic int g_suspended;      /* 1 while the app is backgrounded/asleep */
static SceUID power_cb_id = -1;

/* Bits that mean "going away" and "coming back" in the ScePower callback arg. */
#define PWR_SUSPEND_BITS (SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND)
#define PWR_RESUME_BITS \
    (SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME | \
     SCE_POWER_CB_APP_RESUMING | SCE_POWER_CB_SYSTEM_RESUMING)

static int power_callback(int notify_id, int notify_count, int arg, void *user)
{
    (void)notify_id;
    (void)notify_count;
    (void)user;
    if (arg & PWR_SUSPEND_BITS) {
        if (!atomic_exchange(&g_suspended, 1)) {
            rt_log("power: suspend (arg %08X) -- pausing", (unsigned)arg);
            dsound_audio_pause(1);
        }
    }
    if (arg & PWR_RESUME_BITS) {
        /* A resume only counts if we were actually suspended first. scePowerRegisterCallback
         * fires once at registration with the current power state (resume bits set), and the
         * system sends resume notifications at other times too; treating those as a real
         * resume used to make the game react at launch. Only act when a suspend we observed
         * is now ending: unpark the game loop and resume audio, continuing where we were. */
        if (atomic_exchange(&g_suspended, 0)) {
            rt_log("power: resume (arg %08X) -- continuing", (unsigned)arg);
            dsound_audio_pause(0);
        }
    }
    return 0;
}

static int power_cb_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    power_cb_id = sceKernelCreateCallback("igi_power_cb", 0, power_callback, NULL);
    if (power_cb_id < 0 || scePowerRegisterCallback(power_cb_id) < 0) {
        rt_log("power: could not register callback (%08X)", (unsigned)power_cb_id);
        return 0;
    }
    rt_log("power: suspend/resume callback registered");
    for (;;)
        sceKernelDelayThreadCB(1000 * 1000);   /* pump callbacks; wakes to run them */
    return 0;
}

static void power_init(void)
{
    SceUID t = sceKernelCreateThread("igi_power", power_cb_thread, 0x10000100, 0x2000, 0,
                                     SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
    else
        rt_log("power: could not start callback thread (%08X)", (unsigned)t);
}

/* Called from the game thread's input pump: while the app is suspended, park
 * here so the game thread stops producing frames (no new GPU work is queued).
 * On resume the flag clears and the loop simply continues where it was. */
static void vita_power_checkpoint(void)
{
    while (atomic_load(&g_suspended))
        sceKernelDelayThread(50 * 1000);
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
    power_init();
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
 * the menu cursor speed is fixed).
 *
 * Opt-in touch helpers (both off/default, nothing changes for current players):
 *   touch_mode=drag   (default) a drag anywhere on the front screen aims.
 *   touch_mode=split  CS 1.6 style: the RIGHT half of the front screen is a
 *                     relative aim pad (like a right thumb), the LEFT half is a
 *                     virtual movement stick driving the Move actions. Both halves
 *                     work at once (multi-touch); menus and the map computer are
 *                     unchanged (absolute cursor + tap, trackpad).
 *   onscreen_buttons=1  draw a small set of semi-transparent touch buttons over the
 *                     HUD in gameplay (Jump/Crouch/Use/Reload/next Weapon bottom-left,
 *                     Fire bottom-right). They press the same actions the pad does, so
 *                     Options -> Controls bindings apply. Default 0. */

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
static int touch_mode = PAD_TOUCH_DRAG;   /* igi.cfg touch_mode= (drag/split) */
static int onscreen_buttons_on;           /* igi.cfg onscreen_buttons= */

/* ---- on-screen touch buttons ----
 * A fixed layout along the bottom of the screen, clear of the crosshair (centre)
 * and the HUD, and clear of the right-half aim pad: Jump/Crouch/Use/Reload/
 * Next-weapon are a row at the bottom-left, Fire is a larger pad at the bottom
 * right corner (still inside the aim half, but hit-tested before aiming). Each
 * button presses the same control the pad uses, so remapping still applies.
 * Rects are in drawable pixels (960x544); the touch panel maps 2:1 to them. */
typedef struct VitaButton {
    float x, y, w, h;   /* drawable pixels */
    char label;
    PadControl control;
} VitaButton;

#define BTN_H 70.0f
#define BTN_W 70.0f
#define BTN_GAP 10.0f
#define BTN_MARGIN 14.0f
static const VitaButton onscreen_layout[] = {
    /* bottom-left cluster (clear of the right-half aim pad) */
    { BTN_MARGIN + 0 * (BTN_W + BTN_GAP), SCREEN_H - BTN_MARGIN - BTN_H, BTN_W, BTN_H, 'J', PAD_CROSS },     /* Jump */
    { BTN_MARGIN + 1 * (BTN_W + BTN_GAP), SCREEN_H - BTN_MARGIN - BTN_H, BTN_W, BTN_H, 'C', PAD_L },         /* Crouch */
    { BTN_MARGIN + 2 * (BTN_W + BTN_GAP), SCREEN_H - BTN_MARGIN - BTN_H, BTN_W, BTN_H, 'U', PAD_CIRCLE },    /* Use */
    { BTN_MARGIN + 0 * (BTN_W + BTN_GAP), SCREEN_H - BTN_MARGIN - 2 * (BTN_H + BTN_GAP) + BTN_GAP, BTN_W, BTN_H, 'R', PAD_SQUARE },   /* Reload */
    { BTN_MARGIN + 1 * (BTN_W + BTN_GAP), SCREEN_H - BTN_MARGIN - 2 * (BTN_H + BTN_GAP) + BTN_GAP, BTN_W, BTN_H, 'W', PAD_TRIANGLE }, /* next Weapon */
    /* bottom-right: Fire, a larger pad */
    { SCREEN_W - BTN_MARGIN - 100.0f, SCREEN_H - BTN_MARGIN - 100.0f, 100.0f, 100.0f, 'F', PAD_R },          /* Fire */
};
#define N_ONSCREEN (int)(sizeof(onscreen_layout) / sizeof(onscreen_layout[0]))

/* Pull the touch/aim settings from the central store (vita_settings.c), which
 * owns igi.cfg. Called at start-up and again whenever the in-game options
 * screen changes one, so a change applies without a relaunch. */
void platform_vita_apply_settings(void)
{
    vita_settings_init();
    front_touch_on = vita_settings_front_touch();
    rear_touch_on = vita_settings_rear_touch();
    rear_zones = vita_settings_rear_zones();
    aspect_keep = vita_settings_aspect_keep();
    stick_sensitivity = vita_settings_stick_sensitivity();
    touch_mode = vita_settings_touch_mode();
    onscreen_buttons_on = vita_settings_onscreen_buttons();
    if (front_touch_on)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    else
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    if (rear_touch_on)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    else
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
}

static void touch_init(void)
{
    platform_vita_apply_settings();
    rt_log("right stick: sensitivity x%.2f", (double)stick_sensitivity);
    rt_log("front touch: mode %s, on-screen buttons %s", touch_mode == PAD_TOUCH_SPLIT ? "split" : "drag",
           onscreen_buttons_on ? "on" : "off");
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

/* Front panel position -> drawable (960x544) pixels: the panel is twice the
 * drawable in each axis (1920x1088), so the on-screen buttons and the split line
 * are placed in drawable pixels and hit-tested there, independent of the game
 * image's letterbox. */
static void touch_to_screen(int tx, int ty, int *sx, int *sy)
{
    *sx = (int)((int64_t)tx * SCREEN_W / front_w);
    *sy = (int)((int64_t)ty * SCREEN_H / front_h);
}

/* The on-screen button a drawable point is inside, or -1. */
static int onscreen_hit(int sx, int sy)
{
    for (int i = 0; i < N_ONSCREEN; i++) {
        const VitaButton *b = &onscreen_layout[i];
        if ((float)sx >= b->x && (float)sx < b->x + b->w && (float)sy >= b->y && (float)sy < b->y + b->h)
            return i;
    }
    return -1;
}

/* Which buttons are held this frame (for the overlay highlight). */
static uint8_t onscreen_pressed[N_ONSCREEN > 0 ? N_ONSCREEN : 1];

/* Menus and the map computer: the first touch only, as before. In the menus the
 * cursor goes where the finger is and a tap clicks; the map computer keeps its
 * own cursor and the finger works like a trackpad (a drag moves it, a short tap
 * clicks under it). On-screen buttons and the split stick never apply here. */
#define TAP_MOVE 24          /* a touch that moves within this (panel units) is a tap */
#define TAP_FRAMES 20        /* ... and lasts at most this many input polls */
#define TAP_CLICK_HOLD 4     /* input polls the tap click is held down (the game must poll it) */
static int tap_click;        /* > 0: a computer tap click is held, released after this many polls */

/* Menu/computer touch state (file scope so a mode flip mid-touch can reset it). */
static int menu_active, menu_clicking, menu_last_x, menu_last_y, menu_moved, menu_frames;
static float menu_acc_x, menu_acc_y;

/* Release a held menu click and forget the touch, so a later menu touch starts
 * fresh (used when the game leaves the menus while a finger is still down). */
static void front_menu_reset(void)
{
    if (menu_clicking)
        pad_mouse_button(0, 0);
    menu_active = menu_clicking = menu_moved = menu_frames = 0;
    menu_acc_x = menu_acc_y = 0.0f;
}

static void front_menu_touch(const SceTouchData *t, int computer)
{
    int down = t->reportNum > 0;
    if (!down) {
        if (menu_clicking)
            pad_mouse_button(0, 0);
        if (menu_active && computer && !menu_clicking && !tap_click && menu_moved <= TAP_MOVE &&
            menu_frames <= TAP_FRAMES && igi_computer_active()) {
            pad_mouse_button(0, 1);
            tap_click = TAP_CLICK_HOLD;
        }
        menu_active = menu_clicking = 0;
        return;
    }
    int x = t->report[0].x, y = t->report[0].y;
    if (!menu_active) {
        menu_active = 1;
        menu_last_x = x;
        menu_last_y = y;
        menu_moved = menu_frames = 0;
        menu_acc_x = menu_acc_y = 0.0f;
    }
    if (!computer) {
        /* Menus (main menu, options, mission select, the controls screen, the
         * Vita options) keep their own cursor and move it only by the relative
         * DirectInput mouse motion, hit-testing every widget against it (see
         * runtime/src/igi_fixes.c igi_menu_cursor). To make a tap land exactly
         * under the finger with no drift, read that cursor's current position
         * and post the delta that takes it to the touched point, as the controls
         * screen already does for the D-pad (shim/src/pad_input.c list_cursor_to).
         * touch_to_game honours mode_w/mode_h, aspect_keep and the real reported
         * front-panel size, so it stays correct at any resolution or aspect.
         * Fallback to the old absolute set if no menu cursor is readable. */
        int32_t gx, gy, cx, cy;
        touch_to_game(x, y, &gx, &gy);
        /* Where the menu cursor effectively is: the game's own cursor plus the
         * motion posted but not yet read (dx/dy), as the controls screen reckons
         * it (shim/src/pad_input.c list_cursor). Targeting the delta against this
         * keeps several touch polls before the game consumes the motion from
         * doubling it, and leaves no drift. If no menu cursor is readable (a rare
         * transient window state), fall back to the port's own position. */
        if (!igi_menu_cursor(&cx, &cy)) {
            cx = g_mouse.x;
            cy = g_mouse.y;
        }
        cx += g_mouse.dx;
        cy += g_mouse.dy;
        if (gx != cx || gy != cy) {
            g_mouse.dx += gx - cx;
            g_mouse.dy += gy - cy;
            g_mouse.x = gx;
            g_mouse.y = gy;
            post_mouse_move();
        } else if (!menu_clicking && !pad_capturing_key()) {
            /* (no click while the controls screen waits for a key: it would
             * bind the mouse button, which the pad has not in play) */
            menu_clicking = 1;
            pad_mouse_button(0, 1);
        }
        return;
    }
    /* Map computer: relative motion from the finger's drag (trackpad). */
    int dx = x - menu_last_x, dy = y - menu_last_y;
    menu_moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    menu_frames++;
    menu_acc_x += (float)dx * TOUCH_LOOK_SCALE;
    menu_acc_y += (float)dy * TOUCH_LOOK_SCALE;
    menu_last_x = x;
    menu_last_y = y;
    int32_t mx = (int32_t)menu_acc_x, my = (int32_t)menu_acc_y;
    menu_acc_x -= (float)mx;
    menu_acc_y -= (float)my;
    mouse_motion(mx, my);
}

/* Gameplay: multi-touch. Each finger is classified by where it is (tracked by
 * its stable touch id across frames):
 *   - inside an on-screen button (if enabled): presses that button's control;
 *   - SPLIT mode, left half: a virtual movement stick anchored where it landed;
 *   - otherwise (DRAG mode anywhere, or SPLIT right half): relative aim, reusing
 *     the look path (TOUCH_LOOK_SCALE, then the game's Look Sensitivity).
 * Aim stays with whichever finger already owned it, so adding a second finger
 * (stick or button) never jumps the view. */
#define STICK_RADIUS 90.0f   /* drawable px from the anchor for full deflection */
/* The virtual stick's desired per-direction state (up/down/left/right), merged
 * with the physical left stick in platform_pump so both drive the Move controls
 * through a single, non-conflicting press (pad_control is not reference counted). */
static uint8_t vstick_want[4];
/* Gameplay touch state (file scope so a mode flip mid-touch can reset it). */
static int gp_aim_id = -1, gp_aim_active, gp_aim_x, gp_aim_y;  /* the finger that drives the view */
static int gp_stick_id = -1, gp_stick_ax, gp_stick_ay;         /* the virtual-stick finger + its anchor */
static float gp_acc_x, gp_acc_y;

/* Forget the gameplay touch so re-entry re-anchors fresh; releases the stick. */
static void front_gameplay_reset(void)
{
    gp_aim_id = -1;
    gp_aim_active = 0;
    gp_stick_id = -1;
    gp_acc_x = gp_acc_y = 0.0f;
    vstick_want[0] = vstick_want[1] = vstick_want[2] = vstick_want[3] = 0;
}

static void front_gameplay_touch(const SceTouchData *t)
{
    int n = t->reportNum;

    /* 1) On-screen buttons: pressed if any finger is inside this frame. */
    if (onscreen_buttons_on) {
        uint8_t want[N_ONSCREEN > 0 ? N_ONSCREEN : 1] = { 0 };
        for (int i = 0; i < n; i++) {
            int sx, sy;
            touch_to_screen(t->report[i].x, t->report[i].y, &sx, &sy);
            int hit = onscreen_hit(sx, sy);
            if (hit >= 0)
                want[hit] = 1;
        }
        for (int i = 0; i < N_ONSCREEN; i++) {
            if (want[i] != onscreen_pressed[i]) {
                onscreen_pressed[i] = want[i];
                pad_control(onscreen_layout[i].control, want[i]);
            }
        }
    }

    /* 2) Find this frame's aim finger (prefer the previous one) and stick finger,
     *    skipping fingers that are on a button. */
    int new_aim_id = -1, new_aim_x = 0, new_aim_y = 0;
    int new_stick_id = -1, stick_x = 0, stick_y = 0;
    int have_stick = 0;
    for (int i = 0; i < n; i++) {
        int px = t->report[i].x, py = t->report[i].y, id = t->report[i].id;
        int sx, sy;
        touch_to_screen(px, py, &sx, &sy);
        if (onscreen_buttons_on && onscreen_hit(sx, sy) >= 0)
            continue;
        int left = touch_mode == PAD_TOUCH_SPLIT && sx < SCREEN_W / 2;
        if (left) {
            if (new_stick_id < 0 || id == gp_stick_id) {
                new_stick_id = id;
                stick_x = sx;
                stick_y = sy;
                have_stick = 1;
            }
        } else {
            if (id == gp_aim_id) {   /* keep the existing aim finger */
                new_aim_id = id;
                new_aim_x = px;
                new_aim_y = py;
            } else if (new_aim_id < 0) {
                new_aim_id = id;
                new_aim_x = px;
                new_aim_y = py;
            }
        }
    }

    /* 3) Aim: relative motion of the aim finger. Motion is applied only when the
     *    SAME finger drove the view last frame too (gp_aim_active); a finger newly
     *    promoted to aim just re-anchors, so recycled touch ids can never make the
     *    view jump against a stale anchor. */
    if (new_aim_id >= 0 && gp_aim_active && new_aim_id == gp_aim_id) {
        int dx = new_aim_x - gp_aim_x, dy = new_aim_y - gp_aim_y;
        gp_acc_x += (float)dx * TOUCH_LOOK_SCALE;
        gp_acc_y += (float)dy * TOUCH_LOOK_SCALE;
        int32_t mx = (int32_t)gp_acc_x, my = (int32_t)gp_acc_y;
        gp_acc_x -= (float)mx;
        gp_acc_y -= (float)my;
        mouse_motion(mx, my);
    } else {
        gp_acc_x = gp_acc_y = 0.0f;
    }
    gp_aim_active = new_aim_id >= 0;
    gp_aim_id = new_aim_id;
    gp_aim_x = new_aim_x;
    gp_aim_y = new_aim_y;

    /* 4) Virtual movement stick (SPLIT left half): publish the wanted directions;
     *    platform_pump merges them with the physical stick. */
    if (have_stick) {
        if (new_stick_id != gp_stick_id) {   /* a fresh finger: anchor it here */
            gp_stick_ax = stick_x;
            gp_stick_ay = stick_y;
        }
        gp_stick_id = new_stick_id;
        int ox = stick_x - gp_stick_ax, oy = stick_y - gp_stick_ay;
        int thr = (int)(STICK_RADIUS * 0.4f);   /* ~40% deflection, as the pad's 40/128 */
        vstick_want[0] = oy < -thr;
        vstick_want[1] = oy > thr;
        vstick_want[2] = ox < -thr;
        vstick_want[3] = ox > thr;
    } else {
        gp_stick_id = -1;
        vstick_want[0] = vstick_want[1] = vstick_want[2] = vstick_want[3] = 0;
    }
}

static void front_touch(void)
{
    /* Release a held map-computer tap click once the game has had polls to see
     * it -- every frame, in any mode, so a pending click is never orphaned if
     * the game leaves the menus right after a tap. */
    if (tap_click && --tap_click == 0)
        pad_mouse_button(0, 0);
    SceTouchData t;
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) < 0)
        t.reportNum = 0;
    int computer = igi_computer_active();
    int gameplay = !computer && pad_in_gameplay();
    /* On a change of mode, let go of anything the old mode held and forget its
     * touch, so nothing sticks and the new mode starts fresh. */
    static int was_gameplay = -1;
    if (gameplay != was_gameplay) {
        if (was_gameplay == 1) {   /* was gameplay: release buttons + stick */
            for (int i = 0; i < N_ONSCREEN; i++)
                if (onscreen_pressed[i]) {
                    onscreen_pressed[i] = 0;
                    pad_control(onscreen_layout[i].control, 0);
                }
            front_gameplay_reset();
        } else if (was_gameplay == 0) {   /* was menu/computer: release a held click */
            front_menu_reset();
        }
        was_gameplay = gameplay;
    }
    if (gameplay)
        front_gameplay_touch(&t);
    else
        front_menu_touch(&t, computer);
}

/* Build the on-screen button overlay for this frame (drawable pixels + pressed
 * state). Returns 0 when nothing should be drawn. */
static int onscreen_buttons_build(ButtonsCmd *c)
{
    if (!onscreen_buttons_on || !pad_in_gameplay() || igi_computer_active())
        return 0;
    c->n = N_ONSCREEN < BUTTONS_MAX ? N_ONSCREEN : BUTTONS_MAX;
    for (int i = 0; i < c->n; i++) {
        c->b[i].x = onscreen_layout[i].x;
        c->b[i].y = onscreen_layout[i].y;
        c->b[i].w = onscreen_layout[i].w;
        c->b[i].h = onscreen_layout[i].h;
        c->b[i].label = onscreen_layout[i].label;
        c->b[i].pressed = onscreen_pressed[i];
    }
    return 1;
}

/* Called once per presented frame (platform_vita_draw_overlays). */
void platform_vita_draw_overlays(void)
{
    ButtonsCmd c;
    memset(&c, 0, sizeof(c));
    if (onscreen_buttons_build(&c))
        render_onscreen_buttons(&c);
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
    vita_power_checkpoint();   /* park the loop while suspended; exit after a resume */
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

    /* Front/rear touch first: the split-mode virtual stick (vstick_want) is set
     * here, then merged with the physical left stick below. */
    if (front_touch_on)
        front_touch();
    if (rear_touch_on)
        rear_touch();

    /* Left stick: four controls, one per direction. The physical stick and the
     * split-mode virtual stick drive the same controls, so they are OR'd into a
     * single press per direction (pad_control is not reference counted; two
     * independent sources pressing it would otherwise fight over the key). */
    int lx = deadzone(pad.lx), ly = deadzone(pad.ly);
    uint8_t want[4] = { ly < -40, ly > 40, lx < -40, lx > 40 };
    for (int i = 0; i < 4; i++) {
        uint8_t w = want[i] || vstick_want[i];
        if (w != stick_held[i]) {
            stick_held[i] = w;
            pad_control(stick_controls[i], w);
        }
    }

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
