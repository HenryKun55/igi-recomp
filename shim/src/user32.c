/* user32.dll + gdi32.dll: one top-level window driven by the platform layer. */
#include "platform.h"
#include "shim.h"

#include <stdio.h>

#include <strings.h>

#define HWND_MAIN 0x00010010u
#define HDC_FAKE 0x00020020u
#define HICON_FAKE 0x00030030u
#define HCURSOR_FAKE 0x00030040u

#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_NCCREATE 0x0081u

typedef struct WndClass {
    char name[64];
    uint32_t wndproc;
} WndClass;

static WndClass classes[16];

static struct {
    int exists;
    uint32_t wndproc;
    uint32_t style, exstyle;
    int32_t x, y, w, h;
} wnd;

/* Posted message queue. */
typedef struct Msg {
    uint32_t hwnd, message, wparam, lparam, time;
} Msg;

#define MSGQ 512
static Msg msgq[MSGQ];
static unsigned msg_head, msg_tail;
static int quit_posted;
static uint32_t quit_code;

void user32_post_message(uint32_t message, uint32_t wparam, uint32_t lparam)
{
    unsigned next = (msg_tail + 1u) % MSGQ;
    if (next == msg_head)
        return;
    msgq[msg_tail].hwnd = HWND_MAIN;
    msgq[msg_tail].message = message;
    msgq[msg_tail].wparam = wparam;
    msgq[msg_tail].lparam = lparam;
    msgq[msg_tail].time = win32_tick_count();
    msg_tail = next;
}

static uint32_t send_message(Ctx *c, uint32_t message, uint32_t wparam, uint32_t lparam)
{
    if (!wnd.exists || !wnd.wndproc)
        return 0;
    uint32_t args[4] = { HWND_MAIN, message, wparam, lparam };
    return guest_call(c, wnd.wndproc, 4, args);
}

static int get_message(uint32_t out, uint32_t remove)
{
    if (msg_head == msg_tail) {
        if (!quit_posted)
            return 0;
        WR32(out + 0u, 0);
        WR32(out + 4u, WM_QUIT);
        WR32(out + 8u, quit_code);
        WR32(out + 12u, 0);
        WR32(out + 16u, win32_tick_count());
        WR32(out + 20u, 0);
        WR32(out + 24u, 0);
        if (remove)
            quit_posted = 0;
        return 1;
    }
    Msg *m = &msgq[msg_head];
    WR32(out + 0u, m->hwnd);
    WR32(out + 4u, m->message);
    WR32(out + 8u, m->wparam);
    WR32(out + 12u, m->lparam);
    WR32(out + 16u, m->time);
    WR32(out + 20u, (uint32_t)g_mouse.x);
    WR32(out + 24u, (uint32_t)g_mouse.y);
    if (remove)
        msg_head = (msg_head + 1u) % MSGQ;
    return 1;
}

SHIM(user32, RegisterClassA)
{
    uint32_t wc = ARG(0);
    const char *name = gstr(RD32(wc + 36u));
    for (int i = 0; i < 16; i++) {
        if (!classes[i].name[0]) {
            snprintf(classes[i].name, sizeof(classes[i].name), "%s", name);
            classes[i].wndproc = RD32(wc + 4u);
            rt_log("RegisterClassA(%s) wndproc=%08X", name, classes[i].wndproc);
            stdcall_ret(c, 0xC000u + (uint32_t)i, 1);
            return;
        }
    }
    stdcall_ret(c, 0, 1);
}

SHIM(user32, UnregisterClassA) { stdcall_ret(c, 1, 2); }

SHIM(user32, CreateWindowExA)
{
    uint32_t exstyle = ARG(0), cls = ARG(1), title = ARG(2), style = ARG(3);
    int32_t x = (int32_t)ARG(4), y = (int32_t)ARG(5), w = (int32_t)ARG(6), h = (int32_t)ARG(7);
    const char *cname = cls >= 0x10000u ? gstr(cls) : NULL;
    uint32_t proc = 0;
    for (int i = 0; i < 16; i++) {
        if ((cname && strcasecmp(classes[i].name, cname) == 0) || (!cname && cls == 0xC000u + (uint32_t)i))
            proc = classes[i].wndproc;
    }
    if (w <= 0 || (uint32_t)w == 0x80000000u)
        w = 640;
    if (h <= 0 || (uint32_t)h == 0x80000000u)
        h = 480;
    rt_log("CreateWindowExA(%s, \"%s\", style %08X, %dx%d)", cname ? cname : "#atom", title ? gstr(title) : "",
           style, w, h);
    wnd.exists = 1;
    wnd.wndproc = proc;
    wnd.style = style;
    wnd.exstyle = exstyle;
    wnd.x = x;
    wnd.y = y;
    wnd.w = w;
    wnd.h = h;
    platform_create_window(w, h, title ? gstr(title) : "Project IGI");

    /* WM_NCCREATE / WM_CREATE with a CREATESTRUCTA. */
    uint32_t cs = heap_alloc(48);
    WR32(cs + 0u, ARG(11));
    WR32(cs + 4u, ARG(10));
    WR32(cs + 8u, ARG(9));
    WR32(cs + 12u, ARG(8));
    WR32(cs + 16u, (uint32_t)h);
    WR32(cs + 20u, (uint32_t)w);
    WR32(cs + 24u, (uint32_t)y);
    WR32(cs + 28u, (uint32_t)x);
    WR32(cs + 32u, style);
    WR32(cs + 36u, title);
    WR32(cs + 40u, cls);
    WR32(cs + 44u, exstyle);
    send_message(c, WM_NCCREATE, 0, cs);
    uint32_t r = send_message(c, WM_CREATE, 0, cs);
    heap_free(cs);
    if (r == 0xFFFFFFFFu) {
        wnd.exists = 0;
        stdcall_ret(c, 0, 12);
        return;
    }
    stdcall_ret(c, HWND_MAIN, 12);
}

SHIM(user32, DestroyWindow)
{
    send_message(c, WM_DESTROY, 0, 0);
    wnd.exists = 0;
    stdcall_ret(c, 1, 1);
}

SHIM(user32, ShowWindow)
{
    uint32_t cmd = ARG(1);
    platform_show_window(cmd != 0);
    send_message(c, WM_SHOWWINDOW, cmd != 0, 0);
    if (cmd) {
        send_message(c, WM_ACTIVATEAPP, 1, 0);
        send_message(c, WM_ACTIVATE, 1, 0);
        send_message(c, WM_SETFOCUS, 0, 0);
        send_message(c, WM_SIZE, 0, ((uint32_t)wnd.h << 16) | (uint32_t)wnd.w);
    }
    stdcall_ret(c, 0, 2);
}

SHIM(user32, UpdateWindow) { stdcall_ret(c, 1, 1); }
SHIM(user32, InvalidateRect) { stdcall_ret(c, 1, 3); }
SHIM(user32, SetFocus) { stdcall_ret(c, HWND_MAIN, 1); }
SHIM(user32, SetForegroundWindow) { stdcall_ret(c, 1, 1); }
SHIM(user32, GetForegroundWindow) { stdcall_ret(c, HWND_MAIN, 0); }

SHIM(user32, SetWindowTextA)
{
    platform_set_title(gstr(ARG(1)));
    stdcall_ret(c, 1, 2);
}

SHIM(user32, SetWindowPos)
{
    uint32_t flags = ARG(6);
    if (!(flags & 0x1u)) {    /* !SWP_NOSIZE */
        wnd.w = (int32_t)ARG(4);
        wnd.h = (int32_t)ARG(5);
    }
    stdcall_ret(c, 1, 7);
}

SHIM(user32, GetWindowLongA)
{
    int32_t idx = (int32_t)ARG(1);
    uint32_t r = 0;
    switch (idx) {
    case -4: r = wnd.wndproc; break;
    case -16: r = wnd.style; break;
    case -20: r = wnd.exstyle; break;
    case -6: r = GUEST_IMAGE_BASE; break;
    default: break;
    }
    stdcall_ret(c, r, 2);
}

SHIM(user32, GetClientRect)
{
    uint32_t r = ARG(1);
    WR32(r + 0u, 0);
    WR32(r + 4u, 0);
    WR32(r + 8u, (uint32_t)wnd.w);
    WR32(r + 12u, (uint32_t)wnd.h);
    stdcall_ret(c, 1, 2);
}

SHIM(user32, ClientToScreen) { stdcall_ret(c, 1, 2); }
SHIM(user32, AdjustWindowRect) { stdcall_ret(c, 1, 3); }

SHIM(user32, GetSystemMetrics)
{
    uint32_t i = ARG(0);
    uint32_t r = 0;
    if (i == 0)
        r = 1024;         /* SM_CXSCREEN */
    else if (i == 1)
        r = 768;          /* SM_CYSCREEN */
    stdcall_ret(c, r, 1);
}

SHIM(user32, GetDC) { stdcall_ret(c, HDC_FAKE, 1); }
SHIM(user32, ReleaseDC) { stdcall_ret(c, 1, 2); }
SHIM(user32, LoadIconA) { stdcall_ret(c, HICON_FAKE, 2); }
SHIM(user32, LoadCursorA) { stdcall_ret(c, HCURSOR_FAKE, 2); }

static int cursor_count;

SHIM(user32, ShowCursor)
{
    cursor_count += ARG(0) ? 1 : -1;
    platform_show_cursor(cursor_count >= 0);
    stdcall_ret(c, (uint32_t)cursor_count, 1);
}

SHIM(user32, MessageBoxA)
{
    rt_log("MessageBoxA(\"%s\", \"%s\")", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    stdcall_ret(c, 1, 4);   /* IDOK */
}

SHIM(user32, DefWindowProcA)
{
    uint32_t msg = ARG(1);
    uint32_t r = 0;
    if (msg == WM_CLOSE) {
        uint32_t args[4] = { HWND_MAIN, WM_DESTROY, 0, 0 };
        if (wnd.wndproc)
            guest_call(c, wnd.wndproc, 4, args);
    } else if (msg == WM_NCCREATE) {
        r = 1;
    }
    stdcall_ret(c, r, 4);
}

SHIM(user32, PostMessageA)
{
    user32_post_message(ARG(1), ARG(2), ARG(3));
    stdcall_ret(c, 1, 4);
}

SHIM(user32, PostQuitMessage)
{
    quit_posted = 1;
    quit_code = ARG(0);
    stdcall_ret(c, 0, 1);
}

SHIM(user32, PeekMessageA)
{
    uint32_t out = ARG(0), remove = ARG(4) & 1u;
    platform_pump();
    stdcall_ret(c, (uint32_t)get_message(out, remove), 5);
}

SHIM(user32, GetMessageA)
{
    uint32_t out = ARG(0);
    while (!get_message(out, 1))
        platform_wait();
    stdcall_ret(c, RD32(out + 4u) != WM_QUIT, 4);
}

SHIM(user32, TranslateMessage) { stdcall_ret(c, 0, 1); }

SHIM(user32, DispatchMessageA)
{
    uint32_t m = ARG(0);
    uint32_t r = send_message(c, RD32(m + 4u), RD32(m + 8u), RD32(m + 12u));
    stdcall_ret(c, r, 1);
}

/* ---- gdi32 -------------------------------------------------------------- */

SHIM(gdi32, GetDeviceCaps)
{
    uint32_t idx = ARG(1);
    uint32_t r = 0;
    switch (idx) {
    case 8: r = 1024; break;      /* HORZRES */
    case 10: r = 768; break;      /* VERTRES */
    case 12: r = 32; break;       /* BITSPIXEL */
    case 14: r = 1; break;        /* PLANES */
    case 116: r = 60; break;      /* VREFRESH */
    default: break;
    }
    stdcall_ret(c, r, 2);
}

SHIM(gdi32, CreateICA) { stdcall_ret(c, HDC_FAKE, 4); }
SHIM(gdi32, DeleteDC) { stdcall_ret(c, 1, 1); }
