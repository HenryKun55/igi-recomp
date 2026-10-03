/* PlayStation Vita implementation of the platform layer: vitaGL for video,
 * sceCtrl mapped onto IGI's default keyboard/mouse bindings, SDL for audio. */
#if defined(__vita__)
#include "platform.h"
#include "bench.h"
#include "runtime.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
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
}

void platform_init(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    boot_picture_show();
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
/* SDL_Quit is skipped: after a fatal error (MessageBox + ExitProcess) it was
 * seen not to return on hardware, leaving the process alive in the background
 * (the shell could not start another app until a reboot). Exiting the process
 * stops every thread, the audio one included, before the memory goes away. */
void platform_shutdown(void)
{
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

/* ---- controls ----------------------------------------------------------- */

/* Pad buttons -> DirectInput keys (0 = none), following IGI's default bindings. */
typedef struct ButtonMap {
    uint32_t button;
    uint8_t dik;
    int mouse_button;   /* -1 = none */
} ButtonMap;

static const ButtonMap button_map[] = {
    { SCE_CTRL_RTRIGGER, 0, 0 },          /* Fire (mouse 1) */
    { SCE_CTRL_TRIANGLE, 0, 1 },          /* Next weapon (mouse 2) */
    { SCE_CTRL_LTRIGGER, 0x9D, -1 },      /* Crouch (right ctrl) */
    { SCE_CTRL_CROSS, 0x52, -1 },         /* Jump (numpad 0) */
    { SCE_CTRL_CIRCLE, 0x36, -1 },        /* Activate (right shift) */
    { SCE_CTRL_SQUARE, 0x1C, -1 },        /* Reload / menu confirm (enter) */
    { SCE_CTRL_UP, 0x39, -1 },            /* Binoculars (space) */
    { SCE_CTRL_DOWN, 0x11, -1 },          /* Toggle walk/run (W) */
    { SCE_CTRL_LEFT, 0x18, -1 },          /* Previous weapon (O) */
    { SCE_CTRL_RIGHT, 0x2E, -1 },         /* Map computer (C) */
    { SCE_CTRL_START, 0x01, -1 },         /* Escape */
    { SCE_CTRL_SELECT, 0x4F, -1 },        /* Peek (numpad 1) */
};

static uint32_t prev_buttons;
static uint8_t stick_keys[4];            /* up, down, left, right currently held by the stick */

static void set_key(uint8_t dik, int down)
{
    if (!dik || ((g_key_state[dik] != 0) == (down != 0)))
        return;
    platform_key(dik, down);
}

static void set_mouse_button(int b, int down)
{
    uint8_t v = down ? 0x80 : 0;
    if (g_mouse.buttons[b] == v)
        return;
    g_mouse.buttons[b] = v;
    static const uint32_t msgs[2][2] = { { 0x0202, 0x0201 }, { 0x0205, 0x0204 } };
    user32_post_message(msgs[b][down ? 1 : 0], 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
}

static int deadzone(int v)
{
    v -= 128;
    if (v > -28 && v < 28)
        return 0;
    return v;
}

void platform_pump(void)
{
    static uint64_t last_look, last_pump;
    platform_scripted_input();
    if (bench_enabled())
        return;   /* the benchmark drives the game; the pad would disturb it */
    /* The game polls input and messages many times per frame; the pad only
     * needs sampling every couple of milliseconds. */
    uint64_t t = sceKernelGetProcessTimeWide();
    if (t - last_pump < 2000u)
        return;
    last_pump = t;
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0)
        return;
    for (unsigned i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
        const ButtonMap *m = &button_map[i];
        int down = (pad.buttons & m->button) != 0;
        int was = (prev_buttons & m->button) != 0;
        if (down == was)
            continue;
        if (m->mouse_button >= 0)
            set_mouse_button(m->mouse_button, down);
        else
            set_key(m->dik, down);
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

    /* Left stick: arrow keys (move / strafe). */
    int lx = deadzone(pad.lx), ly = deadzone(pad.ly);
    uint8_t want[4] = { ly < -40, ly > 40, lx < -40, lx > 40 };
    static const uint8_t arrows[4] = { 0xC8, 0xD0, 0xCB, 0xCD };
    for (int i = 0; i < 4; i++) {
        if (want[i] != stick_keys[i]) {
            stick_keys[i] = want[i];
            set_key(arrows[i], want[i]);
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
        acc_x += (float)rx * dt * 9.0f;
        acc_y += (float)ry * dt * 9.0f;
        int32_t mx = (int32_t)acc_x, my = (int32_t)acc_y;
        acc_x -= (float)mx;
        acc_y -= (float)my;
        if (mx || my) {
            g_mouse.dx += mx;
            g_mouse.dy += my;
            g_mouse.x += mx;
            g_mouse.y += my;
            if (g_mouse.x < 0) g_mouse.x = 0;
            if (g_mouse.y < 0) g_mouse.y = 0;
            if (g_mouse.x >= mode_w) g_mouse.x = mode_w - 1;
            if (g_mouse.y >= mode_h) g_mouse.y = mode_h - 1;
            user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
        }
    }
    last_look = now;
}

void platform_wait(void)
{
    SDL_Delay(5);
    platform_pump();
}

#endif
