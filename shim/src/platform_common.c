/* Platform-independent input plumbing shared by the SDL and Vita backends. */
#include "platform.h"
#include "runtime.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_key_state[256];
int g_trace_next_frame;
MouseState g_mouse;

#define KEYQ 256
static KeyEvent keyq[KEYQ];
static unsigned keyq_head, keyq_tail;

void platform_push_key(uint8_t dik, int down)
{
    unsigned next = (keyq_tail + 1u) % KEYQ;
    if (next == keyq_head)
        return;
    keyq[keyq_tail].dik = dik;
    keyq[keyq_tail].down = (uint8_t)down;
    keyq[keyq_tail].time = SDL_GetTicks();
    keyq_tail = next;
}

int platform_pop_key_event(KeyEvent *out)
{
    if (keyq_head == keyq_tail)
        return 0;
    *out = keyq[keyq_head];
    keyq_head = (keyq_head + 1u) % KEYQ;
    return 1;
}

/* IGI_INPUT="ms:action,ms:action,...": scripted input for automated testing.
 * Actions: a DIK code in hex (tap), "mx+N"/"my+N" relative mouse motion, "lmb" click. */
void platform_scripted_input(void)
{
    static const char *script;
    static int init;
    static Uint32 t0;
    if (!init) {
        init = 1;
        script = getenv("IGI_INPUT");
#if defined(__vita__)
        /* No environment on the Vita: read the script from a file. */
        static char buf[4096];
        FILE *f = fopen("ux0:data/igi/input.txt", "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            buf[n] = 0;
            fclose(f);
            script = buf;
        }
#endif
        t0 = SDL_GetTicks();
    }
    static uint8_t release_dik;
    static int release_lmb;
    static Uint32 release_at;
    Uint32 now = SDL_GetTicks() - t0;
    static Uint32 dbg;
    if (getenv("IGI_INPUT_DEBUG") && now / 2000u != dbg) {
        dbg = now / 2000u;
        rt_log("[input-debug] now %u rel %u/%d at %u next '%.12s'", now, release_dik, release_lmb, release_at,
               script ? script : "(end)");
    }
    if ((release_dik || release_lmb) && now >= release_at) {
        if (release_dik) {
            g_key_state[release_dik] = 0;
            platform_push_key(release_dik, 0);
            release_dik = 0;
        }
        if (release_lmb) {
            g_mouse.buttons[0] = 0;
            user32_post_message(0x0202, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
            release_lmb = 0;
        }
    }
    while (script && *script && !release_dik && !release_lmb) {
        unsigned long at = strtoul(script, NULL, 10);
        if (now < at)
            return;
        const char *act = strchr(script, ':');
        if (!act)
            break;
        act++;
        const char *end = strchr(act, ',');
        size_t n = end ? (size_t)(end - act) : strlen(act);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*s", (int)n, act);
        rt_log("[input] t=%lu %s", at, buf);
        if (strcmp(buf, "trace") == 0) {
            g_trace_next_frame = 1;   /* log the next frame's draws and save it */
            script = end ? end + 1 : NULL;
            continue;
        }
        if (strcmp(buf, "lmb") == 0) {
            g_mouse.buttons[0] = 0x80;
            user32_post_message(0x0201, 0, ((uint32_t)g_mouse.y << 16) | ((uint32_t)g_mouse.x & 0xFFFFu));
            release_lmb = 1;
        } else if (buf[0] == 'm' && (buf[1] == 'x' || buf[1] == 'y')) {
            int v = atoi(buf + 2);
            if (buf[1] == 'x') {
                g_mouse.dx += v;
                g_mouse.x += v;
            } else {
                g_mouse.dy += v;
                g_mouse.y += v;
            }
        } else {
            uint8_t dik = (uint8_t)strtoul(buf, NULL, 16);
            g_key_state[dik] = 0x80;
            platform_push_key(dik, 1);
            release_dik = dik;
        }
        release_at = now + 100;
        script = end ? end + 1 : NULL;
    }
}

