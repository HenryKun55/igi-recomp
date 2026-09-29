/* dinput.dll: DirectInput 7 keyboard and mouse fed by the platform layer. */
#include "com.h"
#include "platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define DI_OK 0u
#define DI_BUFFEROVERFLOW 1u
#define DIERR_DEVICENOTREG 0x80040154u

#define KIND_KEYBOARD 1
#define KIND_MOUSE 2

typedef struct Device {
    int kind;
    int acquired;
    uint32_t buffer_size;
    uint32_t seq;
    int32_t last_x, last_y;
    uint8_t last_buttons[4];
} Device;

static uint32_t di_vtbl[8], dev_vtbl;
static uint32_t di_obj;

static const char *const GUID_SYSMOUSE = "6F1D2B60-D5A0-11CF-BFC7-444553540000";
static const char *const GUID_SYSKEYBOARD = "6F1D2B61-D5A0-11CF-BFC7-444553540000";

/* ---- device ------------------------------------------------------------- */

static Device *devp(uint32_t o) { return (Device *)com_host(o); }

static void d_QueryInterface(Ctx *c)
{
    com_addref(THIS);
    WR32(ARG(2), THIS);
    stdcall_ret(c, DI_OK, 3);
}

static void d_AddRef(Ctx *c) { stdcall_ret(c, com_addref(THIS), 1); }

static void d_Release(Ctx *c)
{
    uint32_t o = THIS;
    Device *d = devp(o);
    if (RD32(o + 8u) <= 1u) {
        if (d->kind == KIND_MOUSE && d->acquired)
            platform_capture_mouse(0);
        free(d);
    }
    stdcall_ret(c, com_release(o), 1);
}

static void d_GetCapabilities(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t p = ARG(1);
    uint32_t size = RD32(p);
    memset(gptr(p), 0, size);
    WR32(p, size);
    WR32(p + 4u, 0x5u);                                   /* ATTACHED | POLLEDDEVICE-free */
    WR32(p + 8u, d->kind == KIND_MOUSE ? 0x112u : 0x113u); /* DIDEVTYPE_MOUSE / KEYBOARD */
    WR32(p + 12u, d->kind == KIND_MOUSE ? 3u : 0u);       /* axes */
    WR32(p + 16u, d->kind == KIND_MOUSE ? 3u : 128u);     /* buttons */
    stdcall_ret(c, DI_OK, 2);
}

static void d_SetProperty(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t prop = ARG(1), hdr = ARG(2);
    if (prop == 1u)          /* DIPROP_BUFFERSIZE */
        d->buffer_size = RD32(hdr + 16u);
    stdcall_ret(c, DI_OK, 3);
}

static void d_GetProperty(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t prop = ARG(1), hdr = ARG(2);
    if (prop == 1u)
        WR32(hdr + 16u, d->buffer_size);
    else if (prop == 2u)     /* DIPROP_AXISMODE: relative */
        WR32(hdr + 16u, 1);
    stdcall_ret(c, DI_OK, 3);
}

static void d_Acquire(Ctx *c)
{
    Device *d = devp(THIS);
    if (!d->acquired && d->kind == KIND_MOUSE) {
        platform_capture_mouse(1);
        g_mouse.dx = g_mouse.dy = g_mouse.dz = 0;
    }
    d->acquired = 1;
    stdcall_ret(c, DI_OK, 1);
}

static void d_Unacquire(Ctx *c)
{
    Device *d = devp(THIS);
    if (d->acquired && d->kind == KIND_MOUSE)
        platform_capture_mouse(0);
    d->acquired = 0;
    stdcall_ret(c, DI_OK, 1);
}

static void d_GetDeviceState(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t size = ARG(1), p = ARG(2);
    platform_pump();
    if (d->kind == KIND_KEYBOARD) {
        memcpy(gptr(p), g_key_state, size < 256u ? size : 256u);
    } else {
        memset(gptr(p), 0, size);
        WR32(p + 0u, (uint32_t)g_mouse.dx);
        WR32(p + 4u, (uint32_t)g_mouse.dy);
        WR32(p + 8u, (uint32_t)g_mouse.dz);
        for (uint32_t i = 0; i < 4u && 12u + i < size; i++)
            WR8(p + 12u + i, g_mouse.buttons[i]);
        g_mouse.dx = g_mouse.dy = g_mouse.dz = 0;
    }
    stdcall_ret(c, DI_OK, 3);
}

static uint32_t put_event(uint32_t out, uint32_t stride, uint32_t n, uint32_t ofs, uint32_t data, uint32_t seq)
{
    uint32_t e = out + n * stride;
    WR32(e + 0u, ofs);
    WR32(e + 4u, data);
    WR32(e + 8u, win32_tick_count());
    WR32(e + 12u, seq);
    return n + 1u;
}

static void d_GetDeviceData(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t stride = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
    uint32_t max = RD32(inout);
    uint32_t n = 0;
    int peek = (flags & 1u) != 0;
    platform_pump();
    if (d->kind == KIND_KEYBOARD) {
        KeyEvent ev;
        while (n < max && !peek && platform_pop_key_event(&ev)) {
            if (out)
                n = put_event(out, stride, n, ev.dik, ev.down ? 0x80u : 0u, d->seq++);
            else
                n++;
        }
    } else {
        /* Report accumulated motion and button changes as buffered events. */
        if (n < max && g_mouse.dx) {
            n = out ? put_event(out, stride, n, 0, (uint32_t)g_mouse.dx, d->seq++) : n + 1u;
            if (!peek)
                g_mouse.dx = 0;
        }
        if (n < max && g_mouse.dy) {
            n = out ? put_event(out, stride, n, 4, (uint32_t)g_mouse.dy, d->seq++) : n + 1u;
            if (!peek)
                g_mouse.dy = 0;
        }
        if (n < max && g_mouse.dz) {
            n = out ? put_event(out, stride, n, 8, (uint32_t)g_mouse.dz, d->seq++) : n + 1u;
            if (!peek)
                g_mouse.dz = 0;
        }
        for (uint32_t b = 0; b < 4u && n < max; b++) {
            if (g_mouse.buttons[b] != d->last_buttons[b]) {
                n = out ? put_event(out, stride, n, 12u + b, g_mouse.buttons[b], d->seq++) : n + 1u;
                if (!peek)
                    d->last_buttons[b] = g_mouse.buttons[b];
            }
        }
    }
    WR32(inout, n);
    stdcall_ret(c, DI_OK, 5);
}

static void d_ok2(Ctx *c) { stdcall_ret(c, DI_OK, 2); }
static void d_ok3(Ctx *c) { stdcall_ret(c, DI_OK, 3); }
static void d_ok1(Ctx *c) { stdcall_ret(c, DI_OK, 1); }

static void d_GetDeviceInfo(Ctx *c)
{
    Device *d = devp(THIS);
    uint32_t p = ARG(1);
    uint32_t size = RD32(p);
    memset(gptr(p), 0, size);
    WR32(p, size);
    WR32(p + 36u, d->kind == KIND_MOUSE ? 0x112u : 0x113u);
    snprintf(gstr(p + 40u), 260, "%s", d->kind == KIND_MOUSE ? "Mouse" : "Keyboard");
    snprintf(gstr(p + 300u), 260, "%s", d->kind == KIND_MOUSE ? "Mouse" : "Keyboard");
    stdcall_ret(c, DI_OK, 2);
}

static const ComMethod device_methods[] = {
    { "QueryInterface", d_QueryInterface },
    { "AddRef", d_AddRef },
    { "Release", d_Release },
    { "GetCapabilities", d_GetCapabilities },
    { "EnumObjects", NULL },
    { "GetProperty", d_GetProperty },
    { "SetProperty", d_SetProperty },
    { "Acquire", d_Acquire },
    { "Unacquire", d_Unacquire },
    { "GetDeviceState", d_GetDeviceState },
    { "GetDeviceData", d_GetDeviceData },
    { "SetDataFormat", d_ok2 },
    { "SetEventNotification", d_ok2 },
    { "SetCooperativeLevel", d_ok3 },
    { "GetObjectInfo", NULL },
    { "GetDeviceInfo", d_GetDeviceInfo },
    { "RunControlPanel", NULL },
    { "Initialize", NULL },
    /* v2 */
    { "CreateEffect", NULL },
    { "EnumEffects", NULL },
    { "GetEffectInfo", NULL },
    { "GetForceFeedbackState", NULL },
    { "SendForceFeedbackCommand", NULL },
    { "EnumCreatedEffectObjects", NULL },
    { "Escape", NULL },
    { "Poll", d_ok1 },
    { "SendDeviceData", NULL },
    /* v7 */
    { "EnumEffectsInFile", NULL },
    { "WriteEffectToFile", NULL },
};

/* ---- IDirectInput ------------------------------------------------------- */

static void di_QueryInterface(Ctx *c)
{
    Guid iid;
    guid_read(ARG(1), &iid);
    rt_log("IDirectInput::QueryInterface(%s)", guid_str(&iid));
    WR32(ARG(2), di_obj);
    stdcall_ret(c, DI_OK, 3);
}

static void di_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void di_Release(Ctx *c) { stdcall_ret(c, 1, 1); }

static uint32_t create_device(uint32_t guid_addr)
{
    Guid g;
    guid_read(guid_addr, &g);
    Device *d = calloc(1, sizeof(*d));
    if (guid_eq(&g, GUID_SYSKEYBOARD)) {
        d->kind = KIND_KEYBOARD;
    } else if (guid_eq(&g, GUID_SYSMOUSE)) {
        d->kind = KIND_MOUSE;
    } else {
        rt_log("DirectInput CreateDevice(%s): unknown device", guid_str(&g));
        free(d);
        return 0;
    }
    return com_new(dev_vtbl, d);
}

static void di_CreateDevice(Ctx *c)
{
    uint32_t o = create_device(ARG(1));
    WR32(ARG(2), o);
    stdcall_ret(c, o ? DI_OK : DIERR_DEVICENOTREG, 4);
}

static void di_CreateDeviceEx(Ctx *c)
{
    uint32_t o = create_device(ARG(1));
    WR32(ARG(3), o);
    stdcall_ret(c, o ? DI_OK : DIERR_DEVICENOTREG, 5);
}

static void di_EnumDevices(Ctx *c)
{
    uint32_t type = ARG(1), cb = ARG(2), ctx = ARG(3);
    uint32_t inst = heap_alloc(580);
    static const struct {
        uint32_t type;
        const char *guid;
        const char *name;
    } devs[] = {
        { 0x113u, "6F1D2B61-D5A0-11CF-BFC7-444553540000", "Keyboard" },
        { 0x112u, "6F1D2B60-D5A0-11CF-BFC7-444553540000", "Mouse" },
    };
    for (unsigned i = 0; i < 2; i++) {
        if (type && type != (devs[i].type & 0xFFu))
            continue;
        memset(gptr(inst), 0, 580);
        WR32(inst, 580);
        unsigned d1, d2, d3, b[8];
        sscanf(devs[i].guid, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5], &b[6], &b[7]);
        for (uint32_t k = 0; k < 2u; k++) {
            uint32_t g = inst + 4u + k * 16u;
            WR32(g, d1);
            WR16(g + 4u, d2);
            WR16(g + 6u, d3);
            for (uint32_t j = 0; j < 8u; j++)
                WR8(g + 8u + j, b[j]);
        }
        WR32(inst + 36u, devs[i].type);
        snprintf(gstr(inst + 40u), 260, "%s", devs[i].name);
        snprintf(gstr(inst + 300u), 260, "%s", devs[i].name);
        uint32_t args[2] = { inst, ctx };
        if (!guest_call(c, cb, 2, args))
            break;
    }
    heap_free(inst);
    stdcall_ret(c, DI_OK, 5);
}

static void di_GetDeviceStatus(Ctx *c) { stdcall_ret(c, DI_OK, 2); }

static const ComMethod di_methods[] = {
    { "QueryInterface", di_QueryInterface },
    { "AddRef", di_AddRef },
    { "Release", di_Release },
    { "CreateDevice", di_CreateDevice },
    { "EnumDevices", di_EnumDevices },
    { "GetDeviceStatus", di_GetDeviceStatus },
    { "RunControlPanel", NULL },
    { "Initialize", NULL },
    { "FindDevice", NULL },
    { "CreateDeviceEx", di_CreateDeviceEx },
};

static void di_init(void)
{
    if (di_obj)
        return;
    di_vtbl[7] = com_vtable("IDirectInput7A", di_methods, 10);
    dev_vtbl = com_vtable("IDirectInputDevice7A", device_methods, 29);
    di_obj = com_new(di_vtbl[7], NULL);
}

SHIM(dinput, DirectInputCreateA)
{
    di_init();
    WR32(ARG(2), di_obj);
    stdcall_ret(c, DI_OK, 4);
}

SHIM(dinput, DirectInputCreateEx)
{
    di_init();
    WR32(ARG(3), di_obj);
    stdcall_ret(c, DI_OK, 5);
}
