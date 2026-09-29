/* SDL2 implementation of the platform layer (desktop). */
#if !defined(__vita__)
#include "platform.h"
#include "runtime.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>


static SDL_Window *window;
static SDL_GLContext gl;
static int mode_w = 640, mode_h = 480;
static int window_shown;


/* ---- key tables --------------------------------------------------------- */

static uint8_t sdl_to_dik(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) {
        static const uint8_t letters[26] = {
            0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
            0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
        };
        return letters[s - SDL_SCANCODE_A];
    }
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0)
        return (uint8_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10)
        return (uint8_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;
    case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
    case SDL_SCANCODE_RETURN: return 0x1C;
    case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45;
    case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47;
    case SDL_SCANCODE_KP_8: return 0x48;
    case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4A;
    case SDL_SCANCODE_KP_4: return 0x4B;
    case SDL_SCANCODE_KP_5: return 0x4C;
    case SDL_SCANCODE_KP_6: return 0x4D;
    case SDL_SCANCODE_KP_PLUS: return 0x4E;
    case SDL_SCANCODE_KP_1: return 0x4F;
    case SDL_SCANCODE_KP_2: return 0x50;
    case SDL_SCANCODE_KP_3: return 0x51;
    case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53;
    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: return 0x9C;
    case SDL_SCANCODE_RCTRL: return 0x9D;
    case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
    case SDL_SCANCODE_RALT: return 0xB8;
    case SDL_SCANCODE_PAUSE: return 0xC5;
    case SDL_SCANCODE_HOME: return 0xC7;
    case SDL_SCANCODE_UP: return 0xC8;
    case SDL_SCANCODE_PAGEUP: return 0xC9;
    case SDL_SCANCODE_LEFT: return 0xCB;
    case SDL_SCANCODE_RIGHT: return 0xCD;
    case SDL_SCANCODE_END: return 0xCF;
    case SDL_SCANCODE_DOWN: return 0xD0;
    case SDL_SCANCODE_PAGEDOWN: return 0xD1;
    case SDL_SCANCODE_INSERT: return 0xD2;
    case SDL_SCANCODE_DELETE: return 0xD3;
    case SDL_SCANCODE_LGUI: return 0xDB;
    case SDL_SCANCODE_RGUI: return 0xDC;
    default: return 0;
    }
}

static uint32_t sdl_to_vk(SDL_Scancode s)
{
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z)
        return 'A' + (uint32_t)(s - SDL_SCANCODE_A);
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9)
        return '1' + (uint32_t)(s - SDL_SCANCODE_1);
    if (s == SDL_SCANCODE_0)
        return '0';
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12)
        return 0x70u + (uint32_t)(s - SDL_SCANCODE_F1);
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9)
        return 0x61u + (uint32_t)(s - SDL_SCANCODE_KP_1);
    switch (s) {
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}

/* ---- window ------------------------------------------------------------- */

void platform_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
        rt_fatal(NULL, "SDL_Init: %s", SDL_GetError());
}

void platform_create_window(int w, int h, const char *title)
{
    if (window)
        return;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    mode_w = w;
    mode_h = h;
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w * 2, h * 2,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!window)
        rt_fatal(NULL, "SDL_CreateWindow: %s", SDL_GetError());
}

void platform_show_window(int show)
{
    if (!window || show == window_shown)
        return;
    window_shown = show;
    if (show)
        SDL_ShowWindow(window);
    else
        SDL_HideWindow(window);
}

void platform_set_title(const char *title)
{
    if (window)
        SDL_SetWindowTitle(window, title);
}

void platform_set_video_mode(int w, int h)
{
    if (!window)
        platform_create_window(w, h, "Project IGI");
    if (w != mode_w || h != mode_h) {
        mode_w = w;
        mode_h = h;
        SDL_SetWindowSize(window, w * 2, h * 2);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    if (!gl) {
        gl = SDL_GL_CreateContext(window);
        if (!gl)
            rt_fatal(NULL, "SDL_GL_CreateContext: %s", SDL_GetError());
        const char *vs = getenv("IGI_VSYNC");
        SDL_GL_SetSwapInterval(vs && vs[0] == '0' ? 0 : 1);
    }
    platform_show_window(1);
}

void platform_get_video_mode(int *w, int *h)
{
    *w = mode_w;
    *h = mode_h;
}

void platform_get_drawable_size(int *w, int *h)
{
    SDL_GL_GetDrawableSize(window, w, h);
}

void platform_swap(void)
{
    /* Frame rate and average frame time in the title, refreshed once a second. */
    static Uint64 last, freq;
    static unsigned frames;
    if (!freq) {
        freq = SDL_GetPerformanceFrequency();
        last = SDL_GetPerformanceCounter();
    }
    frames++;
    Uint64 now = SDL_GetPerformanceCounter();
    if (now - last >= freq) {
        double secs = (double)(now - last) / (double)freq;
        char title[96];
        snprintf(title, sizeof(title), "Project IGI - %.1f fps (%.1f ms)", frames / secs, secs * 1000.0 / frames);
        SDL_SetWindowTitle(window, title);
        frames = 0;
        last = now;
    }
    SDL_GL_SwapWindow(window);
}

/* Mouse capture requested by DirectInput; re-applied whenever the window gets focus,
 * because macOS ignores relative mode for an unfocused window. */
static int mouse_capture_wanted;

static void apply_mouse_capture(void)
{
    if (!window)
        return;
    int on = mouse_capture_wanted && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS);
    SDL_SetWindowGrab(window, on ? SDL_TRUE : SDL_FALSE);
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
}

void platform_capture_mouse(int on)
{
    mouse_capture_wanted = on;
    apply_mouse_capture();
}

void platform_show_cursor(int show)
{
    SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

/* ---- events ------------------------------------------------------------- */

/* Window coordinates -> game resolution. */
static void to_game(int wx, int wy, int32_t *gx, int32_t *gy)
{
    int ww, wh;
    SDL_GetWindowSize(window, &ww, &wh);
    *gx = ww ? (int32_t)((int64_t)wx * mode_w / ww) : wx;
    *gy = wh ? (int32_t)((int64_t)wy * mode_h / wh) : wy;
}

static void handle_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        user32_post_message(0x0010, 0, 0);   /* WM_CLOSE */
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_p && (e->key.keysym.mod & KMOD_GUI)) {
            g_trace_next_frame = 1;    /* Cmd+P: dump the next frame's draws */
            break;
        }
        int down = e->type == SDL_KEYDOWN;
        uint8_t dik = sdl_to_dik(e->key.keysym.scancode);
        if (dik) {
            if (!e->key.repeat) {
                g_key_state[dik] = down ? 0x80 : 0;
                platform_push_key(dik, down);
            }
        }
        uint32_t vk = sdl_to_vk(e->key.keysym.scancode);
        if (vk) {
            uint32_t lparam = 1u | ((uint32_t)(dik & 0x7F) << 16) | (dik & 0x80 ? 1u << 24 : 0u);
            if (!down)
                lparam |= 0xC0000000u;
            else if (e->key.repeat)
                lparam |= 0x40000000u;
            int sys = (e->key.keysym.mod & KMOD_ALT) != 0;
            user32_post_message(down ? (sys ? 0x0104u : 0x0100u) : (sys ? 0x0105u : 0x0101u), vk, lparam);
            /* Control characters that SDL does not deliver as text input. */
            if (down && (vk == 0x08 || vk == 0x0D || vk == 0x1B || vk == 0x09))
                user32_post_message(0x0102, vk, lparam);
        }
        break;
    }
    case SDL_TEXTINPUT:
        for (const unsigned char *p = (const unsigned char *)e->text.text; *p; p++)
            if (*p >= 0x20 && *p < 0x80)
                user32_post_message(0x0102, *p, 1);
        break;
    case SDL_MOUSEMOTION:
        g_mouse.dx += e->motion.xrel;
        g_mouse.dy += e->motion.yrel;
        to_game(e->motion.x, e->motion.y, &g_mouse.x, &g_mouse.y);
        user32_post_message(0x0200, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int down = e->type == SDL_MOUSEBUTTONDOWN;
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 :
                e->button.button == SDL_BUTTON_MIDDLE ? 2 : 3;
        g_mouse.buttons[b] = down ? 0x80 : 0;
        to_game(e->button.x, e->button.y, &g_mouse.x, &g_mouse.y);
        static const uint32_t msgs[3][2] = { { 0x0202, 0x0201 }, { 0x0205, 0x0204 }, { 0x0208, 0x0207 } };
        if (b < 3)
            user32_post_message(msgs[b][down], 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
        break;
    }
    case SDL_MOUSEWHEEL:
        g_mouse.dz += e->wheel.y * 120;
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            apply_mouse_capture();
            user32_post_message(0x001C, 1, 0);   /* WM_ACTIVATEAPP */
            user32_post_message(0x0006, 1, 0);   /* WM_ACTIVATE */
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            memset(g_key_state, 0, sizeof(g_key_state));
        }
        break;
    default:
        break;
    }
}

void platform_pump(void)
{
    /* Pumping Cocoa events is expensive; the game asks many times per frame. */
    static Uint64 last_pump;
    Uint64 now = SDL_GetPerformanceCounter();
    if (last_pump && now - last_pump < SDL_GetPerformanceFrequency() / 1000u)
        return;
    last_pump = now;
    platform_scripted_input();
    SDL_Event e;
    while (SDL_PollEvent(&e))
        handle_event(&e);
}

void platform_wait(void)
{
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 10))
        handle_event(&e);
    platform_pump();
}

#endif
