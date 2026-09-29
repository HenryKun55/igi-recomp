/* ddraw.dll: DirectDraw 1/2/4/7 on top of the GL renderer. */
#include "ddraw_priv.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>

int g_dd_width = 640, g_dd_height = 480, g_dd_bpp = 16;
Surface *g_primary;
Surface *g_backbuffer;

extern uint32_t d3d7_create(void);

/* ---- pixel formats ------------------------------------------------------ */

PixFmt pixfmt_rgb(int bits)
{
    PixFmt pf = { DDPF_RGB, (uint32_t)bits, 0, 0, 0, 0 };
    if (bits == 16) {
        pf.r = 0xF800u;
        pf.g = 0x07E0u;
        pf.b = 0x001Fu;
    } else {
        pf.r = 0x00FF0000u;
        pf.g = 0x0000FF00u;
        pf.b = 0x000000FFu;
    }
    return pf;
}

void pixfmt_write(uint32_t a, const PixFmt *pf)
{
    WR32(a + 0u, 32);
    WR32(a + 4u, pf->flags);
    WR32(a + 8u, 0);
    WR32(a + 12u, pf->bits);
    WR32(a + 16u, pf->r);
    WR32(a + 20u, pf->g);
    WR32(a + 24u, pf->b);
    WR32(a + 28u, pf->a);
}

void pixfmt_read(uint32_t a, PixFmt *pf)
{
    pf->flags = RD32(a + 4u);
    pf->bits = RD32(a + 12u);
    pf->r = RD32(a + 16u);
    pf->g = RD32(a + 20u);
    pf->b = RD32(a + 24u);
    pf->a = RD32(a + 28u);
}

static uint32_t extract(uint32_t v, uint32_t mask)
{
    if (!mask)
        return 255;
    int shift = __builtin_ctz(mask);
    uint32_t m = mask >> shift;
    uint32_t x = (v & mask) >> shift;
    return (x * 255u + m / 2u) / m;
}

static uint32_t insert(uint32_t c8, uint32_t mask)
{
    if (!mask)
        return 0;
    int shift = __builtin_ctz(mask);
    uint32_t m = mask >> shift;
    return (((c8 * m + 127u) / 255u) << shift) & mask;
}

uint32_t pixel_to_rgba(const PixFmt *pf, const uint8_t *p)
{
    uint32_t v;
    if (pf->bits == 16)
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    else if (pf->bits == 24)
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    else if (pf->bits == 8)
        v = p[0];
    else
        v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t a = (pf->flags & DDPF_ALPHAPIXELS) ? extract(v, pf->a) : 255u;
    return extract(v, pf->r) | (extract(v, pf->g) << 8) | (extract(v, pf->b) << 16) | (a << 24);
}

uint32_t rgba_to_pixel(const PixFmt *pf, uint32_t c)
{
    uint32_t v = insert(c & 0xFFu, pf->r) | insert((c >> 8) & 0xFFu, pf->g) | insert((c >> 16) & 0xFFu, pf->b);
    if (pf->flags & DDPF_ALPHAPIXELS)
        v |= insert(c >> 24, pf->a);
    return v;
}

/* ---- surfaces ----------------------------------------------------------- */

static uint32_t surface_vtbl[8];

uint32_t surface_obj(Surface *s, int version)
{
    if (!s->objs[version]) {
        s->objs[version] = com_new(surface_vtbl[version], s);
        s->refs++;
    }
    return s->objs[version];
}

Surface *surface_from_obj(uint32_t obj)
{
    return obj ? (Surface *)com_host(obj) : NULL;
}

void surface_alloc_mem(Surface *s)
{
    if (s->mem)
        return;
    uint32_t size = s->pitch * s->h;
    s->mem = heap_alloc(size ? size : 4u);
    if (!s->mem)
        rt_fatal(NULL, "out of guest memory for a %ux%u surface", s->w, s->h);
    memset(gptr(s->mem), 0, size);
}

static void surface_destroy(Surface *s)
{
    if (s->attached && s->attached->parent == s)
        surface_destroy(s->attached);
    render_texture_release(s);
    for (int i = 0; i < 8; i++) {
        if (s->objs[i]) {
            uint32_t o = s->objs[i];
            WR32(o + 8u, 1);
            com_release(o);
        }
    }
    if (s->mem)
        heap_free(s->mem);
    if (s == g_primary)
        g_primary = NULL;
    if (s == g_backbuffer)
        g_backbuffer = NULL;
    free(s);
}

static void surface_release(Surface *s)
{
    if (--s->refs <= 0)
        surface_destroy(s);
}

static Surface *surface_new(uint32_t w, uint32_t h, const PixFmt *pf, uint32_t caps)
{
    Surface *s = calloc(1, sizeof(*s));
    s->w = w;
    s->h = h;
    s->pf = *pf;
    s->caps = caps;
    s->pitch = (w * (pf->bits / 8u) + 3u) & ~3u;
    return s;
}

/* Fill a DDSURFACEDESC / DDSURFACEDESC2 (size taken from the guest's dwSize). */
static void surface_desc_write(Surface *s, uint32_t d)
{
    uint32_t size = RD32(d);
    if (size != 108u && size != 124u)
        size = 124u;
    memset(gptr(d), 0, size);
    WR32(d + 0u, size);
    WR32(d + 4u, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT |
                 (s->has_ck ? DDSD_CKSRCBLT : 0u) | (s->mip_levels ? DDSD_MIPMAPCOUNT : 0u));
    WR32(d + 8u, s->h);
    WR32(d + 12u, s->w);
    WR32(d + 16u, s->pitch);
    WR32(d + 24u, (uint32_t)s->mip_levels);
    if (s->has_ck) {
        WR32(d + 64u, s->ck_lo);
        WR32(d + 68u, s->ck_hi);
    }
    pixfmt_write(d + 72u, &s->pf);
    WR32(d + 104u, s->caps);
    if (size == 124u)
        WR32(d + 108u, s->caps2);
}

/* ---- IDirectDrawSurface methods ----------------------------------------- */

static void surf_QueryInterface(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    Guid iid;
    guid_read(ARG(1), &iid);
    int v = -1;
    if (guid_eq(&iid, "6C14DB81-A733-11CE-A521-0020AF0BE560")) v = 1;
    else if (guid_eq(&iid, "57805885-6EEC-11CF-9441-A82303C10E27")) v = 2;
    else if (guid_eq(&iid, "DA044E00-69B2-11D0-A1D5-00AA00B8DFBB")) v = 3;
    else if (guid_eq(&iid, "0B2B8630-AD35-11D0-8EA6-00609797EA5B")) v = 4;
    else if (guid_eq(&iid, "06675A80-3B9B-11D2-B92F-00609797EA5B")) v = 7;
    else if (guid_eq(&iid, "00000000-0000-0000-C000-000000000046")) v = 7;
    if (v < 0) {
        rt_log("IDirectDrawSurface::QueryInterface(%s): no interface", guid_str(&iid));
        WR32(ARG(2), 0);
        stdcall_ret(c, E_NOINTERFACE, 3);
        return;
    }
    int existed = s->objs[v] != 0;
    uint32_t o = surface_obj(s, v);
    if (existed)
        s->refs++;
    WR32(ARG(2), o);
    stdcall_ret(c, S_OK, 3);
}

static void surf_AddRef(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    stdcall_ret(c, (uint32_t)++s->refs, 1);
}

static void surf_Release(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    int n = s->refs - 1;
    surface_release(s);
    stdcall_ret(c, (uint32_t)(n < 0 ? 0 : n), 1);
}

static void surf_AddAttachedSurface(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    Surface *a = surface_from_obj(ARG(1));
    if (a && a->is_z) {
        s->zbuf = a;
        a->refs++;
    } else if (a) {
        s->attached = a;
        a->refs++;
    }
    stdcall_ret(c, DD_OK, 2);
}

static void surf_DeleteAttachedSurface(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    Surface *a = surface_from_obj(ARG(2));
    if (a && s->zbuf == a) {
        s->zbuf = NULL;
        surface_release(a);
    }
    stdcall_ret(c, DD_OK, 3);
}

static void rect_read(uint32_t a, Rect *r, const Surface *s)
{
    if (a) {
        r->l = (int32_t)RD32(a);
        r->t = (int32_t)RD32(a + 4u);
        r->r = (int32_t)RD32(a + 8u);
        r->b = (int32_t)RD32(a + 12u);
    } else {
        r->l = 0;
        r->t = 0;
        r->r = (int32_t)s->w;
        r->b = (int32_t)s->h;
    }
}

static int dd_trace_left = 12;   /* first surface operations are logged (video path diagnosis) */

static int is_render_target(const Surface *s)
{
    return s == g_backbuffer || s == g_primary;
}

/* Software blit between two memory surfaces (same pixel format assumed). */
static void soft_blit(Surface *dst, const Rect *dr, Surface *src, const Rect *sr, int use_ck)
{
    surface_alloc_mem(dst);
    surface_alloc_mem(src);
    uint32_t bpp = dst->pf.bits / 8u;
    int32_t dw = dr->r - dr->l, dh = dr->b - dr->t;
    int32_t sw = sr->r - sr->l, sh = sr->b - sr->t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return;
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr->t + y, sy = sr->t + y * sh / dh;
        if (dy < 0 || dy >= (int32_t)dst->h || sy < 0 || sy >= (int32_t)src->h)
            continue;
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr->l + x, sx = sr->l + x * sw / dw;
            if (dx < 0 || dx >= (int32_t)dst->w || sx < 0 || sx >= (int32_t)src->w)
                continue;
            uint8_t *sp = g_mem + src->mem + (uint32_t)sy * src->pitch + (uint32_t)sx * bpp;
            uint8_t *dp = g_mem + dst->mem + (uint32_t)dy * dst->pitch + (uint32_t)dx * bpp;
            if (use_ck && src->has_ck) {
                uint32_t v = 0;
                memcpy(&v, sp, bpp);
                if (v >= src->ck_lo && v <= src->ck_hi)
                    continue;
            }
            memcpy(dp, sp, bpp);
        }
    }
    dst->dirty = 1;
}

static void soft_fill(Surface *dst, const Rect *r, uint32_t v)
{
    surface_alloc_mem(dst);
    uint32_t bpp = dst->pf.bits / 8u;
    for (int32_t y = r->t; y < r->b; y++) {
        if (y < 0 || y >= (int32_t)dst->h)
            continue;
        for (int32_t x = r->l; x < r->r; x++) {
            if (x < 0 || x >= (int32_t)dst->w)
                continue;
            memcpy(g_mem + dst->mem + (uint32_t)y * dst->pitch + (uint32_t)x * bpp, &v, bpp);
        }
    }
    dst->dirty = 1;
}

#define DDBLT_COLORFILL 0x400u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
#define DDBLT_DEPTHFILL 0x2000000u

static void surf_Blt(Ctx *c)
{
    Surface *dst = surface_from_obj(THIS);
    if (dd_trace_left > 0) {
        Surface *s2 = surface_from_obj(ARG(2));
        dd_trace_left--;
        rt_log("Blt to %ux%u (rt %d) from %ux%u flags %08X", dst->w, dst->h, is_render_target(dst), s2 ? s2->w : 0u,
               s2 ? s2->h : 0u, ARG(4));
    }
    Rect dr, sr;
    rect_read(ARG(1), &dr, dst);
    Surface *src = surface_from_obj(ARG(2));
    uint32_t flags = ARG(4), fx = ARG(5);
    if (flags & DDBLT_DEPTHFILL) {
        stdcall_ret(c, DD_OK, 6);
        return;
    }
    if (flags & DDBLT_COLORFILL) {
        uint32_t color = fx ? RD32(fx + 80u) : 0u;   /* DDBLTFX.dwFillColor */
        if (is_render_target(dst))
            render_fill(&dr, pixel_to_rgba(&dst->pf, (const uint8_t *)&color));
        else
            soft_fill(dst, &dr, color);
        stdcall_ret(c, DD_OK, 6);
        return;
    }
    if (!src) {
        stdcall_ret(c, DD_OK, 6);
        return;
    }
    rect_read(ARG(3), &sr, src);
    int ck = (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0;
    if (is_render_target(dst))
        render_blit(src, &sr, &dr, ck);
    else
        soft_blit(dst, &dr, src, &sr, ck);
    stdcall_ret(c, DD_OK, 6);
}

static void surf_BltFast(Ctx *c)
{
    Surface *dst = surface_from_obj(THIS);
    if (dd_trace_left > 0) {
        dd_trace_left--;
        rt_log("BltFast to %ux%u (rt %d) at %d,%d", dst->w, dst->h, is_render_target(dst), (int32_t)ARG(1), (int32_t)ARG(2));
    }
    int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2);
    Surface *src = surface_from_obj(ARG(3));
    uint32_t trans = ARG(5);
    Rect sr, dr;
    rect_read(ARG(4), &sr, src);
    dr.l = x;
    dr.t = y;
    dr.r = x + (sr.r - sr.l);
    dr.b = y + (sr.b - sr.t);
    int ck = (trans & 1u) != 0;     /* DDBLTFAST_SRCCOLORKEY */
    if (is_render_target(dst))
        render_blit(src, &sr, &dr, ck);
    else
        soft_blit(dst, &dr, src, &sr, ck);
    stdcall_ret(c, DD_OK, 6);
}

static void surf_Flip(Ctx *c)
{
    render_present();
    stdcall_ret(c, DD_OK, 3);
}

static void surf_GetAttachedSurface(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    uint32_t caps = RD32(ARG(1));
    Surface *a = NULL;
    if (caps & DDSCAPS_ZBUFFER)
        a = s->zbuf;
    else if (caps & (DDSCAPS_BACKBUFFER | DDSCAPS_MIPMAP | DDSCAPS_FLIP))
        a = s->attached;
    if (!a) {
        WR32(ARG(2), 0);
        stdcall_ret(c, DDERR_NOTFOUND, 3);
        return;
    }
    /* Use the same interface version as the caller. */
    int v = 7;
    for (int i = 0; i < 8; i++)
        if (s->objs[i] == THIS)
            v = i;
    int existed = a->objs[v] != 0;
    uint32_t o = surface_obj(a, v);
    if (existed)
        a->refs++;
    WR32(ARG(2), o);
    stdcall_ret(c, DD_OK, 3);
}

static void surf_GetCaps(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    uint32_t p = ARG(1);
    WR32(p, s->caps);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetColorKey(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    WR32(ARG(2), s->ck_lo);
    WR32(ARG(2) + 4u, s->ck_hi);
    stdcall_ret(c, s->has_ck ? DD_OK : 0x88760208u, 3);   /* DDERR_NOCOLORKEY */
}

static void surf_SetColorKey(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    uint32_t flags = ARG(1), key = ARG(2);
    if (flags & 0x8u) {           /* DDCKEY_SRCBLT */
        if (key) {
            s->has_ck = 1;
            s->ck_lo = RD32(key);
            s->ck_hi = RD32(key + 4u);
        } else {
            s->has_ck = 0;
        }
        s->dirty = 1;
    }
    stdcall_ret(c, DD_OK, 3);
}

static void surf_GetPixelFormat(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    pixfmt_write(ARG(1), &s->pf);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetSurfaceDesc(Ctx *c)
{
    surface_desc_write(surface_from_obj(THIS), ARG(1));
    stdcall_ret(c, DD_OK, 2);
}

static void surf_IsLost(Ctx *c) { stdcall_ret(c, DD_OK, 1); }
static void surf_Restore(Ctx *c) { stdcall_ret(c, DD_OK, 1); }

static void surf_Lock(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    if (dd_trace_left > 0 && !s->is_texture) {
        dd_trace_left--;
        rt_log("Lock %ux%u %ubpp primary %d back %d mem %08X", s->w, s->h, s->pf.bits, s->is_primary, s->is_back, s->mem);
    }
    uint32_t rect = ARG(1), desc = ARG(2);
    surface_alloc_mem(s);
    if (is_render_target(s)) {
        render_readback(s);
        s->lock_readback = 1;
    }
    s->locked = 1;
    surface_desc_write(s, desc);
    uint32_t p = s->mem;
    if (rect)
        p += RD32(rect + 4u) * s->pitch + RD32(rect) * (s->pf.bits / 8u);
    WR32(desc + 4u, RD32(desc + 4u) | DDSD_LPSURFACE);
    WR32(desc + 36u, p);
    stdcall_ret(c, DD_OK, 5);
}

static void surf_Unlock(Ctx *c)
{
    Surface *s = surface_from_obj(THIS);
    s->locked = 0;
    s->dirty = 1;
    if (s->lock_readback) {
        render_writeback(s);
        s->lock_readback = 0;
    }
    stdcall_ret(c, DD_OK, 2);
}

static void surf_ok1(Ctx *c) { stdcall_ret(c, DD_OK, 1); }
static void surf_ok2(Ctx *c) { stdcall_ret(c, DD_OK, 2); }

static void surf_GetDC(Ctx *c)
{
    WR32(ARG(1), 0x00020020u);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetDDInterface(Ctx *c)
{
    extern uint32_t g_ddraw7_obj;
    WR32(ARG(1), g_ddraw7_obj);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetPriority(Ctx *c)
{
    WR32(ARG(1), 0);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetLOD(Ctx *c)
{
    WR32(ARG(1), 0);
    stdcall_ret(c, DD_OK, 2);
}

static void surf_GetUniquenessValue(Ctx *c)
{
    WR32(ARG(1), 1);
    stdcall_ret(c, DD_OK, 2);
}

static const ComMethod surface_methods[] = {
    { "QueryInterface", surf_QueryInterface },
    { "AddRef", surf_AddRef },
    { "Release", surf_Release },
    { "AddAttachedSurface", surf_AddAttachedSurface },
    { "AddOverlayDirtyRect", NULL },
    { "Blt", surf_Blt },
    { "BltBatch", NULL },
    { "BltFast", surf_BltFast },
    { "DeleteAttachedSurface", surf_DeleteAttachedSurface },
    { "EnumAttachedSurfaces", NULL },
    { "EnumOverlayZOrders", NULL },
    { "Flip", surf_Flip },
    { "GetAttachedSurface", surf_GetAttachedSurface },
    { "GetBltStatus", surf_ok2 },
    { "GetCaps", surf_GetCaps },
    { "GetClipper", NULL },
    { "GetColorKey", surf_GetColorKey },
    { "GetDC", surf_GetDC },
    { "GetFlipStatus", surf_ok2 },
    { "GetOverlayPosition", NULL },
    { "GetPalette", NULL },
    { "GetPixelFormat", surf_GetPixelFormat },
    { "GetSurfaceDesc", surf_GetSurfaceDesc },
    { "Initialize", NULL },
    { "IsLost", surf_IsLost },
    { "Lock", surf_Lock },
    { "ReleaseDC", surf_ok2 },
    { "Restore", surf_Restore },
    { "SetClipper", surf_ok2 },
    { "SetColorKey", surf_SetColorKey },
    { "SetOverlayPosition", NULL },
    { "SetPalette", surf_ok2 },
    { "Unlock", surf_Unlock },
    { "UpdateOverlay", NULL },
    { "UpdateOverlayDisplay", NULL },
    { "UpdateOverlayZOrder", NULL },
    /* v2 */
    { "GetDDInterface", surf_GetDDInterface },
    { "PageLock", surf_ok2 },
    { "PageUnlock", surf_ok2 },
    /* v3 */
    { "SetSurfaceDesc", NULL },
    /* v4 */
    { "SetPrivateData", NULL },
    { "GetPrivateData", NULL },
    { "FreePrivateData", NULL },
    { "GetUniquenessValue", surf_GetUniquenessValue },
    { "ChangeUniquenessValue", surf_ok1 },
    /* v7 */
    { "SetPriority", surf_ok2 },
    { "GetPriority", surf_GetPriority },
    { "SetLOD", surf_ok2 },
    { "GetLOD", surf_GetLOD },
};

/* ---- IDirectDraw -------------------------------------------------------- */

uint32_t g_ddraw7_obj;
static uint32_t ddraw_vtbl[8];
static uint32_t ddraw_objs[8];
static int ddraw_refs;

static uint32_t ddraw_obj(int v)
{
    if (!ddraw_objs[v])
        ddraw_objs[v] = com_new(ddraw_vtbl[v], NULL);
    ddraw_refs++;
    return ddraw_objs[v];
}

static int ddraw_version(uint32_t obj)
{
    for (int i = 0; i < 8; i++)
        if (ddraw_objs[i] == obj)
            return i;
    return 7;
}

static void dd_QueryInterface(Ctx *c)
{
    Guid iid;
    guid_read(ARG(1), &iid);
    uint32_t o = 0;
    if (guid_eq(&iid, "6C14DB80-A733-11CE-A521-0020AF0BE560"))
        o = ddraw_obj(1);
    else if (guid_eq(&iid, "B3A6F3E0-2B43-11CF-A2DE-00AA00B93356"))
        o = ddraw_obj(2);
    else if (guid_eq(&iid, "9C59509A-39BD-11D1-8C4A-00C04FD930C5"))
        o = ddraw_obj(4);
    else if (guid_eq(&iid, "15E65EC0-3B9C-11D2-B92F-00609797EA5B"))
        o = ddraw_obj(7);
    else if (guid_eq(&iid, "F5049E77-4861-11D2-A407-00A0C90629A8"))
        o = d3d7_create();
    if (!o) {
        rt_log("IDirectDraw::QueryInterface(%s): no interface", guid_str(&iid));
        WR32(ARG(2), 0);
        stdcall_ret(c, E_NOINTERFACE, 3);
        return;
    }
    WR32(ARG(2), o);
    stdcall_ret(c, S_OK, 3);
}

static void dd_AddRef(Ctx *c) { stdcall_ret(c, (uint32_t)++ddraw_refs, 1); }

static void dd_Release(Ctx *c)
{
    if (ddraw_refs > 0)
        ddraw_refs--;
    stdcall_ret(c, (uint32_t)ddraw_refs, 1);
}

static void dd_SetCooperativeLevel(Ctx *c)
{
    rt_log("SetCooperativeLevel(flags %08X)", ARG(2));
    stdcall_ret(c, DD_OK, 3);
}

static void set_mode(int w, int h, int bpp)
{
    rt_log("SetDisplayMode(%d, %d, %d)", w, h, bpp);
    g_dd_width = w;
    g_dd_height = h;
    g_dd_bpp = bpp;
    platform_set_video_mode(w, h);
    render_init(w, h);
}

static void dd_SetDisplayMode1(Ctx *c)
{
    set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    stdcall_ret(c, DD_OK, 4);
}

static void dd_SetDisplayMode(Ctx *c)
{
    set_mode((int)ARG(1), (int)ARG(2), (int)ARG(3));
    stdcall_ret(c, DD_OK, 6);
}

static void dd_RestoreDisplayMode(Ctx *c) { stdcall_ret(c, DD_OK, 1); }

static void dd_CreateSurface(Ctx *c)
{
    int version = ddraw_version(THIS);
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = RD32(d + 4u);
    uint32_t caps = (flags & DDSD_CAPS) ? RD32(d + 104u) : 0u;
    uint32_t size = RD32(d);
    uint32_t caps2 = (size == 124u && (flags & DDSD_CAPS)) ? RD32(d + 108u) : 0u;
    uint32_t w = (flags & DDSD_WIDTH) ? RD32(d + 12u) : 0u;
    uint32_t h = (flags & DDSD_HEIGHT) ? RD32(d + 8u) : 0u;
    PixFmt pf = pixfmt_rgb(g_dd_bpp);
    if (flags & DDSD_PIXELFORMAT)
        pixfmt_read(d + 72u, &pf);
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = (uint32_t)g_dd_width;
        h = (uint32_t)g_dd_height;
        pf = pixfmt_rgb(g_dd_bpp);
    }
    if (caps & DDSCAPS_ZBUFFER) {
        uint32_t zbits = (flags & DDSD_PIXELFORMAT) ? pf.bits : RD32(d + 20u);
        if (!zbits)
            zbits = 16;
        pf.flags = DDPF_ZBUFFER;
        pf.bits = zbits;
        pf.r = zbits == 16 ? 0xFFFFu : 0xFFFFFFu;
        pf.g = pf.b = pf.a = 0;
    }
    if (!w || !h) {
        w = (uint32_t)g_dd_width;
        h = (uint32_t)g_dd_height;
    }
    Surface *s = surface_new(w, h, &pf, caps | DDSCAPS_LOCALVIDMEM | DDSCAPS_VIDEOMEMORY);
    s->caps2 = caps2;
    if (flags & DDSD_CKSRCBLT) {
        s->has_ck = 1;
        s->ck_lo = RD32(d + 64u);
        s->ck_hi = RD32(d + 68u);
    }
    s->is_texture = (caps & DDSCAPS_TEXTURE) != 0;
    s->is_z = (caps & DDSCAPS_ZBUFFER) != 0;
    uint32_t obj = surface_obj(s, version);
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        s->is_primary = 1;
        s->caps |= DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE;
        g_primary = s;
        g_backbuffer = s;
        uint32_t backs = (flags & DDSD_BACKBUFFERCOUNT) ? RD32(d + 20u) : 0u;
        if (backs) {
            Surface *b = surface_new(w, h, &pf, DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_FLIP |
                                                DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
            b->is_back = 1;
            b->parent = s;
            b->refs = 1;   /* held by the flip chain */
            s->attached = b;
            g_backbuffer = b;
        }
    } else if (s->is_texture && (flags & DDSD_MIPMAPCOUNT) && RD32(d + 24u) > 1u) {
        /* Mip chain: each level attached to the previous one. */
        uint32_t levels = RD32(d + 24u);
        s->mip_levels = (int)levels;
        Surface *prev = s;
        for (uint32_t i = 1; i < levels; i++) {
            uint32_t mw = w >> i, mh = h >> i;
            Surface *m = surface_new(mw ? mw : 1u, mh ? mh : 1u, &pf, s->caps);
            m->caps2 = caps2 | 0x10000u;   /* DDSCAPS2_MIPMAPSUBLEVEL */
            m->is_texture = 1;
            m->parent = prev;
            m->refs = 1;   /* held by the mip chain */
            m->mip_levels = (int)(levels - i);
            prev->attached = m;
            prev = m;
        }
    }
    WR32(out, obj);
    stdcall_ret(c, DD_OK, 4);
}

static void dd_GetCaps(Ctx *c)
{
    for (int k = 1; k <= 2; k++) {
        uint32_t p = ARG(k);
        if (!p)
            continue;
        uint32_t size = RD32(p);
        memset(gptr(p), 0, size);
        WR32(p, size);
        WR32(p + 4u, 0x00000001u | 0x00000040u | 0x00000200u | 0x00400000u | 0x04000000u);
        WR32(p + 8u, 0x00080000u);            /* DDCAPS2_WIDESURFACES etc. */
        WR32(p + 12u, 0x00000200u);           /* DDCKEYCAPS_SRCBLT */
        WR32(p + 56u, 0x400u | 0x200u | 0x100u);
        WR32(p + 60u, 64u << 20);
        WR32(p + 64u, 60u << 20);
        if (size >= 380u)
            WR32(p + 364u, DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_FLIP |
                           DDSCAPS_BACKBUFFER | DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY |
                           DDSCAPS_LOCALVIDMEM | DDSCAPS_MIPMAP);
        else if (size >= 108u)
            WR32(p + size - 4u, DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER);
    }
    stdcall_ret(c, DD_OK, 3);
}

static void write_mode_desc(uint32_t d, uint32_t size, int w, int h, int bpp)
{
    memset(gptr(d), 0, size);
    WR32(d, size);
    WR32(d + 4u, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    WR32(d + 8u, (uint32_t)h);
    WR32(d + 12u, (uint32_t)w);
    WR32(d + 16u, (uint32_t)(w * bpp / 8));
    WR32(d + 24u, 60);
    PixFmt pf = pixfmt_rgb(bpp);
    pixfmt_write(d + 72u, &pf);
}

static void dd_GetDisplayMode(Ctx *c)
{
    uint32_t d = ARG(1);
    uint32_t size = RD32(d) == 108u ? 108u : 124u;
    write_mode_desc(d, size, g_dd_width, g_dd_height, g_dd_bpp);
    stdcall_ret(c, DD_OK, 2);
}

static void dd_EnumDisplayModes(Ctx *c)
{
    int version = ddraw_version(THIS);
    uint32_t ctx = ARG(3), cb = ARG(4);
    static const int modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 960 }, { 1280, 1024 } };
    static const int depths[] = { 16, 32 };
    uint32_t size = version >= 4 ? 124u : 108u;
    uint32_t d = heap_alloc(size);
    for (unsigned b = 0; b < 2; b++) {
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
            write_mode_desc(d, size, modes[i][0], modes[i][1], depths[b]);
            uint32_t args[2] = { d, ctx };
            if (!guest_call(c, cb, 2, args))
                goto done;
        }
    }
done:
    heap_free(d);
    stdcall_ret(c, DD_OK, 5);
}

static void dd_GetAvailableVidMem(Ctx *c)
{
    if (ARG(2))
        WR32(ARG(2), 64u << 20);
    if (ARG(3))
        WR32(ARG(3), 60u << 20);
    stdcall_ret(c, DD_OK, 4);
}

static void dd_WaitForVerticalBlank(Ctx *c) { stdcall_ret(c, DD_OK, 3); }
static void dd_TestCooperativeLevel(Ctx *c) { stdcall_ret(c, DD_OK, 1); }
static void dd_RestoreAllSurfaces(Ctx *c) { stdcall_ret(c, DD_OK, 1); }
static void dd_FlipToGDISurface(Ctx *c) { stdcall_ret(c, DD_OK, 1); }

static void dd_GetDeviceIdentifier(Ctx *c)
{
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 1072);
    snprintf((char *)g_mem + p, 512, "display");
    snprintf((char *)g_mem + p + 512u, 512, "IGI GL renderer");
    stdcall_ret(c, DD_OK, 3);
}

static void dd_GetMonitorFrequency(Ctx *c)
{
    WR32(ARG(1), 60);
    stdcall_ret(c, DD_OK, 2);
}

static void dd_GetFourCCCodes(Ctx *c)
{
    if (ARG(1))
        WR32(ARG(1), 0);
    stdcall_ret(c, DD_OK, 3);
}

/* ---- IDirectDrawClipper ------------------------------------------------- */

static uint32_t clipper_vtbl;

static void clip_AddRef(Ctx *c) { stdcall_ret(c, com_addref(THIS), 1); }
static void clip_Release(Ctx *c) { stdcall_ret(c, com_release(THIS), 1); }
static void clip_ok2(Ctx *c) { stdcall_ret(c, DD_OK, 2); }
static void clip_ok3(Ctx *c) { stdcall_ret(c, DD_OK, 3); }

static void clip_GetHWnd(Ctx *c)
{
    WR32(ARG(1), RD32(THIS + 12u));
    stdcall_ret(c, DD_OK, 2);
}

static void clip_SetHWnd(Ctx *c)
{
    WR32(THIS + 12u, ARG(2));
    stdcall_ret(c, DD_OK, 3);
}

static const ComMethod clipper_methods[] = {
    { "QueryInterface", NULL },
    { "AddRef", clip_AddRef },
    { "Release", clip_Release },
    { "GetClipList", NULL },
    { "GetHWnd", clip_GetHWnd },
    { "Initialize", clip_ok3 },
    { "IsClipListChanged", clip_ok2 },
    { "SetClipList", clip_ok3 },
    { "SetHWnd", clip_SetHWnd },
};

static void dd_CreateClipper(Ctx *c)
{
    if (!clipper_vtbl)
        clipper_vtbl = com_vtable("IDirectDrawClipper", clipper_methods, 9);
    WR32(ARG(2), com_new(clipper_vtbl, NULL));
    stdcall_ret(c, DD_OK, 4);
}

static const ComMethod ddraw_methods[] = {
    { "QueryInterface", dd_QueryInterface },
    { "AddRef", dd_AddRef },
    { "Release", dd_Release },
    { "Compact", NULL },
    { "CreateClipper", dd_CreateClipper },
    { "CreatePalette", NULL },
    { "CreateSurface", dd_CreateSurface },
    { "DuplicateSurface", NULL },
    { "EnumDisplayModes", dd_EnumDisplayModes },
    { "EnumSurfaces", NULL },
    { "FlipToGDISurface", dd_FlipToGDISurface },
    { "GetCaps", dd_GetCaps },
    { "GetDisplayMode", dd_GetDisplayMode },
    { "GetFourCCCodes", dd_GetFourCCCodes },
    { "GetGDISurface", NULL },
    { "GetMonitorFrequency", dd_GetMonitorFrequency },
    { "GetScanLine", NULL },
    { "GetVerticalBlankStatus", NULL },
    { "Initialize", NULL },
    { "RestoreDisplayMode", dd_RestoreDisplayMode },
    { "SetCooperativeLevel", dd_SetCooperativeLevel },
    { "SetDisplayMode", dd_SetDisplayMode },
    { "WaitForVerticalBlank", dd_WaitForVerticalBlank },
    /* v2 */
    { "GetAvailableVidMem", dd_GetAvailableVidMem },
    /* v4 */
    { "GetSurfaceFromDC", NULL },
    { "RestoreAllSurfaces", dd_RestoreAllSurfaces },
    { "TestCooperativeLevel", dd_TestCooperativeLevel },
    { "GetDeviceIdentifier", dd_GetDeviceIdentifier },
    /* v7 */
    { "StartModeTest", NULL },
    { "EvaluateMode", NULL },
};

static int ddraw_ready;

static void ddraw_init(void)
{
    if (ddraw_ready)
        return;
    ddraw_ready = 1;
    static const int surf_counts[8] = { 0, 36, 39, 40, 45, 0, 0, 49 };
    for (int v = 1; v < 8; v++) {
        if (!surf_counts[v])
            continue;
        char name[32];
        snprintf(name, sizeof(name), "IDirectDrawSurface%d", v);
        surface_vtbl[v] = com_vtable(strdup(name), surface_methods, surf_counts[v]);
    }
    ddraw_vtbl[1] = com_vtable("IDirectDraw", ddraw_methods, 23);
    ddraw_vtbl[2] = com_vtable("IDirectDraw2", ddraw_methods, 24);
    ddraw_vtbl[4] = com_vtable("IDirectDraw4", ddraw_methods, 28);
    ddraw_vtbl[7] = com_vtable("IDirectDraw7", ddraw_methods, 30);
    /* IDirectDraw (v1) SetDisplayMode takes three arguments. */
    static const ComMethod v1_mode = { "SetDisplayMode", dd_SetDisplayMode1 };
    WR32(ddraw_vtbl[1] + 21u * 4u, thunk_register_unique(v1_mode.fn, "IDirectDraw::SetDisplayMode"));
}

/* ---- exports ------------------------------------------------------------ */

static void DirectDrawCreate(Ctx *c)
{
    ddraw_init();
    WR32(ARG(1), ddraw_obj(1));
    stdcall_ret(c, DD_OK, 3);
}

SHIM(ddraw, DirectDrawCreateEx)
{
    ddraw_init();
    g_ddraw7_obj = ddraw_obj(7);
    WR32(ARG(1), g_ddraw7_obj);
    stdcall_ret(c, DD_OK, 4);
}

SHIM(ddraw, DirectDrawEnumerateExA)
{
    uint32_t cb = ARG(0), ctx = ARG(1);
    static uint32_t desc, name;
    if (!desc) {
        desc = scratch_strdup("Primary Display Driver");
        name = scratch_strdup("display");
    }
    uint32_t args[5] = { 0, desc, name, ctx, 0 };
    guest_call(c, cb, 5, args);
    stdcall_ret(c, DD_OK, 3);
}

uint32_t ddraw_get_proc(const char *name)
{
    if (strcmp(name, "DirectDrawCreate") == 0)
        return thunk_register(DirectDrawCreate, "DirectDrawCreate");
    return 0;
}
