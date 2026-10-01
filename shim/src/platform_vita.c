/* PlayStation Vita implementation of the platform layer: vitaGL for video,
 * sceCtrl mapped onto IGI's default keyboard/mouse bindings, SDL for audio. */
#if defined(__vita__)
#include "platform.h"
#include "bench.h"
#include "runtime.h"

#include <SDL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <stdio.h>
#include <stdlib.h>
#include <vitaGL.h>

#define SCREEN_W 960
#define SCREEN_H 544

static int mode_w = 640, mode_h = 480;
static int gl_ready;

void platform_init(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
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
/* Stop the audio thread (it reads guest memory) before the process goes away. */
void platform_shutdown(void)
{
    render_shutdown();
    SDL_Quit();
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
    const uint32_t diag = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
    if ((pad.buttons & diag) == diag && (prev_buttons & diag) != diag)
        render_diag_cycle();
    const uint32_t trace = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    if ((pad.buttons & trace) == trace && (prev_buttons & trace) != trace && !g_trace_next_frame)
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
