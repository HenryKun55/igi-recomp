/* Host platform layer (SDL2): window, GL context, input state, audio device. */
#ifndef IGI_PLATFORM_H
#define IGI_PLATFORM_H

#include <stdint.h>

void platform_init(void);
void platform_create_window(int w, int h, const char *title);
void platform_show_window(int show);
void platform_set_title(const char *title);

/* Switch the game's back-buffer resolution; creates the GL context on first use. */
void platform_set_video_mode(int w, int h);
void platform_get_video_mode(int *w, int *h);
/* Drawable size in pixels (may differ from the game resolution). */
void platform_get_drawable_size(int *w, int *h);
void platform_swap(void);

/* Pump host events: posts Win32 messages and updates the input state below. */
void platform_pump(void);
/* Block until at least one event is available, then pump. */
void platform_wait(void);
void platform_show_cursor(int show);
/* Keep the host cursor inside the window and report relative motion. */
void platform_capture_mouse(int on);

/* Keyboard state indexed by DirectInput DIK_* code. */
extern uint8_t g_key_state[256];
/* Set by the Cmd+P hotkey: log every draw of the next frame (1 = armed, 2 = tracing). */
extern int g_trace_next_frame;

/* Buffered keyboard events for DirectInput (DIK code, pressed). */
typedef struct KeyEvent {
    uint8_t dik;
    uint8_t down;
    uint32_t time;
} KeyEvent;
int platform_pop_key_event(KeyEvent *out);
void platform_push_key(uint8_t dik, int down);
/* Feed IGI_INPUT scripted events (testing aid); called from platform_pump. */
void platform_scripted_input(void);

/* Mouse: accumulated relative motion since the last read, absolute position, buttons. */
typedef struct MouseState {
    int32_t dx, dy, dz;
    int32_t x, y;
    uint8_t buttons[4];
} MouseState;
extern MouseState g_mouse;

/* Called by the platform for every translated window message. */
void user32_post_message(uint32_t msg, uint32_t wparam, uint32_t lparam);

#endif
