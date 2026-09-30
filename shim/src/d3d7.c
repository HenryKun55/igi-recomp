/* Direct3D 7: IDirect3D7, IDirect3DDevice7 and IDirect3DVertexBuffer7. */
#include "igi_fixes.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>

#define D3D_OK 0u
#define D3DERR_INVALIDVERTEXTYPE 0x88760809u

static uint32_t d3d_vtbl, device_vtbl, vb_vtbl;
static uint32_t d3d_obj, device_obj;

/* ---- device description ------------------------------------------------- */

static const char *const HAL_GUID = "84E63DE0-46AA-11CF-816F-0000C020156E";
static const char *const TNL_GUID = "F5049E78-4861-11D2-A407-00A0C90629A8";

static void write_guid(uint32_t a, const char *text)
{
    unsigned d1, d2, d3, b[8];
    sscanf(text, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5],
           &b[6], &b[7]);
    WR32(a, d1);
    WR16(a + 4u, d2);
    WR16(a + 6u, d3);
    for (int i = 0; i < 8; i++)
        WR8(a + 8u + (uint32_t)i, b[i]);
}

static void write_primcaps(uint32_t p)
{
    WR32(p + 0u, 56);
    WR32(p + 4u, 0x000000F2u);   /* MISCCAPS: MASKZ, CULLNONE, CULLCW, CULLCCW */
    WR32(p + 8u, 0x0007FFFFu);   /* RASTERCAPS */
    WR32(p + 12u, 0xFFu);        /* ZCMPCAPS */
    WR32(p + 16u, 0x1FFFu);      /* SRCBLENDCAPS */
    WR32(p + 20u, 0x1FFFu);      /* DESTBLENDCAPS */
    WR32(p + 24u, 0xFFu);        /* ALPHACMPCAPS */
    WR32(p + 28u, 0x001FFFFFu);  /* SHADECAPS */
    WR32(p + 32u, 0x0000000Fu);  /* TEXTURECAPS: PERSPECTIVE POW2 ALPHA TRANSPARENCY */
    WR32(p + 36u, 0x0F03FF3Fu);  /* TEXTUREFILTERCAPS */
    WR32(p + 40u, 0x000000FFu);  /* TEXTUREBLENDCAPS */
    WR32(p + 44u, 0x1Fu);        /* TEXTUREADDRESSCAPS */
    WR32(p + 48u, 0);
    WR32(p + 52u, 0);
}

static void write_device_desc(uint32_t d, const char *guid, int tnl)
{
    memset(gptr(d), 0, 236);
    WR32(d + 0u, 0x0001FFF1u | (tnl ? 0x10000u : 0u) | 0x80000u);
    write_primcaps(d + 4u);
    write_primcaps(d + 60u);
    WR32(d + 116u, 0x400u | 0x100u);           /* render depths 16/32 */
    WR32(d + 120u, 0x400u | 0x200u | 0x100u);  /* z depths 16/24/32 */
    WR32(d + 124u, 1);
    WR32(d + 128u, 1);
    WR32(d + 132u, 2048);
    WR32(d + 136u, 2048);
    WR32(d + 140u, 8192);
    WR32(d + 144u, 2048);
    WR32(d + 148u, 1);
    float gb[4] = { -2048.0f, -2048.0f, 2047.0f, 2047.0f };
    memcpy(gptr(d + 152u), gb, 16);
    WR32(d + 176u, 0x00080008u);               /* FVFCAPS: 8 texcoords, DONOTSTRIPELEMENTS */
    WR32(d + 180u, 0x00FFFFFFu);               /* TEXTUREOPCAPS */
    WR16(d + 184u, MAX_STAGES);
    WR16(d + 186u, MAX_STAGES);
    WR32(d + 188u, MAX_LIGHTS);
    float maxw = 1e10f;
    memcpy(gptr(d + 192u), &maxw, 4);
    write_guid(d + 196u, guid);
    WR16(d + 212u, 0);
    WR16(d + 214u, 0);
    WR32(d + 216u, tnl ? 0x3Bu : 0u);
}

/* ---- IDirect3D7 --------------------------------------------------------- */

static void d3d_QueryInterface(Ctx *c)
{
    Guid iid;
    guid_read(ARG(1), &iid);
    rt_log("IDirect3D7::QueryInterface(%s)", guid_str(&iid));
    WR32(ARG(2), 0);
    stdcall_ret(c, E_NOINTERFACE, 3);
}

static void d3d_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void d3d_Release(Ctx *c) { stdcall_ret(c, 1, 1); }

static void d3d_EnumDevices(Ctx *c)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    static uint32_t hal_desc, hal_name, tnl_desc, tnl_name;
    if (!hal_desc) {
        hal_desc = scratch_strdup("Direct3D HAL");
        hal_name = scratch_strdup("Direct3D HAL");
        tnl_desc = scratch_strdup("Direct3D T&L HAL");
        tnl_name = scratch_strdup("Direct3D T&L HAL");
    }
    uint32_t d = heap_alloc(236);
    write_device_desc(d, HAL_GUID, 0);
    uint32_t args[4] = { hal_desc, hal_name, d, ctx };
    if (guest_call(c, cb, 4, args)) {
        write_device_desc(d, TNL_GUID, 1);
        uint32_t args2[4] = { tnl_desc, tnl_name, d, ctx };
        guest_call(c, cb, 4, args2);
    }
    heap_free(d);
    stdcall_ret(c, D3D_OK, 3);
}

static void d3d_CreateDevice(Ctx *c)
{
    Guid g;
    guid_read(ARG(1), &g);
    Surface *rt = surface_from_obj(ARG(2));
    rt_log("IDirect3D7::CreateDevice(%s, %ux%u)", guid_str(&g), rt ? rt->w : 0u, rt ? rt->h : 0u);
    if (rt)
        g_backbuffer = rt;
    if (!device_obj)
        device_obj = com_new(device_vtbl, NULL);
    WR32(ARG(3), device_obj);
    stdcall_ret(c, D3D_OK, 4);
}

static void d3d_EnumZBufferFormats(Ctx *c)
{
    uint32_t cb = ARG(2), ctx = ARG(3);
    uint32_t pf = heap_alloc(32);
    static const uint32_t depths[3][2] = { { 16, 0xFFFFu }, { 24, 0xFFFFFFu }, { 32, 0xFFFFFFu } };
    for (int i = 0; i < 3; i++) {
        PixFmt f = { DDPF_ZBUFFER, depths[i][0], depths[i][1], 0, 0, 0 };
        if (i == 2) {
            f.flags |= 0x4000u;   /* DDPF_STENCILBUFFER */
            f.g = 0xFF000000u;
        }
        pixfmt_write(pf, &f);
        uint32_t args[2] = { pf, ctx };
        if (!guest_call(c, cb, 2, args))
            break;
    }
    heap_free(pf);
    stdcall_ret(c, D3D_OK, 4);
}

typedef struct VertexBuffer {
    uint32_t fvf, count, stride, mem;
    uint32_t gen;              /* bumped by every writable Lock */
    uint32_t lock_frame;       /* g_frame_serial at the last writable Lock */
    uint32_t cache_gen;        /* gen the decoded copy matches (0 = none) */
    struct RVertex *cache;     /* whole buffer decoded (static geometry is drawn every frame) */
    void *gl;                  /* renderer's GPU copy (render_draw_vb) */
    float lo[3], hi[3];        /* bounds of the decoded copy */
} VertexBuffer;

static uint32_t fvf_stride(uint32_t fvf);

static void d3d_CreateVertexBuffer(Ctx *c)
{
    uint32_t desc = ARG(1);
    VertexBuffer *vb = calloc(1, sizeof(*vb));
    vb->gen = 1;
    vb->fvf = RD32(desc + 8u);
    vb->count = RD32(desc + 12u);
    vb->stride = fvf_stride(vb->fvf);
    vb->mem = heap_alloc(vb->stride * vb->count + 16u);
    WR32(ARG(2), com_new(vb_vtbl, vb));
    stdcall_ret(c, D3D_OK, 4);
}

static void d3d_EvictManagedTextures(Ctx *c) { stdcall_ret(c, D3D_OK, 1); }

static const ComMethod d3d_methods[] = {
    { "QueryInterface", d3d_QueryInterface },
    { "AddRef", d3d_AddRef },
    { "Release", d3d_Release },
    { "EnumDevices", d3d_EnumDevices },
    { "CreateDevice", d3d_CreateDevice },
    { "CreateVertexBuffer", d3d_CreateVertexBuffer },
    { "EnumZBufferFormats", d3d_EnumZBufferFormats },
    { "EvictManagedTextures", d3d_EvictManagedTextures },
};

/* ---- vertex decoding ---------------------------------------------------- */

static uint32_t fvf_stride(uint32_t fvf)
{
    uint32_t s = 0;
    switch (fvf & 0x0Eu) {
    case 0x2u: s = 12; break;
    case 0x4u: s = 16; break;
    case 0x6u: s = 16; break;
    case 0x8u: s = 20; break;
    case 0xAu: s = 24; break;
    case 0xCu: s = 28; break;
    case 0xEu: s = 32; break;
    default: break;
    }
    if (fvf & 0x10u) s += 12;
    if (fvf & 0x20u) s += 4;
    if (fvf & 0x40u) s += 4;
    if (fvf & 0x80u) s += 4;
    uint32_t ntex = (fvf >> 8) & 0xFu;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t f = (fvf >> (16 + i * 2)) & 3u;
        s += f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
    }
    return s;
}

static RVertex *vbuf;
static uint32_t vbuf_cap;

static uint32_t d3dcolor_to_rgba(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb & 0xFFu) << 16) | ((argb >> 16) & 0xFFu);
}

typedef struct Decoded {
    int pretransformed, has_normal, has_color, has_spec, ntex;
} Decoded;

/* Decodes `count` vertices: vertex i comes from base + (src ? src[i] : i) * stride. */
static RVertex *decode_vertices(uint32_t fvf, uint32_t base, uint32_t stride, const uint32_t *src, uint32_t count,
                               Decoded *d);

static RVertex *decode(uint32_t fvf, uint32_t base, uint32_t stride, const uint32_t *src, uint32_t count, Decoded *d)
{
    PROF_SECTION(PROF_H_DECODE);
    RVertex *r = decode_vertices(fvf, base, stride, src, count, d);
    PROF_SECTION_END;
    return r;
}

/* Hands untransformed vertices to the renderer in the game's layout when it can
 * take them directly; otherwise decodes them to RVertex. */
static void emit(uint32_t prim, uint32_t fvf, uint32_t base, uint32_t stride, const uint32_t *src, uint32_t count,
                 const uint16_t *idx, uint32_t ni)
{
    uint32_t pos = fvf & 0x0Eu;
    {
        GuestVerts g;
        memset(&g, 0, sizeof(g));
        g.base = g_mem + base;
        g.stride = stride;
        g.src = src;
        g.has_normal = (fvf & 0x10u) != 0;
        g.has_color = (fvf & 0x40u) != 0;
        g.has_spec = (fvf & 0x80u) != 0;
        g.ntex = (int)((fvf >> 8) & 0xFu);
        g.pretransformed = pos == 0x4u;
        uint32_t o = pos == 0x4u ? 16u : pos == 0x2u ? 12u : 12u + ((pos - 0x4u) / 2u) * 4u;
        g.nrm_off = o;
        if (g.has_normal)
            o += 12;
        if (fvf & 0x20u)
            o += 4;
        g.col_off = o;
        if (g.has_color)
            o += 4;
        g.spec_off = o;
        if (g.has_spec)
            o += 4;
        for (int t = 0; t < g.ntex && t < MAX_STAGES; t++) {
            uint32_t f = (fvf >> (16 + t * 2)) & 3u;
            uint32_t sz = f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
            g.uv_off[t] = o;
            g.uv_bytes[t] = sz >= 8u ? 8u : 4u;
            o += sz;
        }
        if (render_draw_guest((int)prim, &g, count, idx, ni))
            return;
    }
    Decoded d;
    RVertex *v = decode(fvf, base, stride, src, count, &d);
    render_draw((int)prim, v, count, idx, ni, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

static RVertex *decode_vertices(uint32_t fvf, uint32_t base, uint32_t stride, const uint32_t *src, uint32_t count,
                               Decoded *d)
{
    if (count > vbuf_cap) {
        vbuf_cap = count + 256u;
        vbuf = realloc(vbuf, vbuf_cap * sizeof(RVertex));
    }
    uint32_t pos = fvf & 0x0Eu;
    d->pretransformed = pos == 0x4u;
    d->has_normal = (fvf & 0x10u) != 0;
    d->has_color = (fvf & 0x40u) != 0;
    d->has_spec = (fvf & 0x80u) != 0;
    d->ntex = (int)((fvf >> 8) & 0xFu);
    uint32_t pos_size = pos == 0x4u ? 16u : pos == 0x2u ? 12u : 12u + ((pos - 0x4u) / 2u) * 4u;
    /* Attribute offsets are the same for every vertex: work them out once. */
    uint32_t o = pos_size;
    uint32_t nrm_off = o;
    if (d->has_normal)
        o += 12;
    if (fvf & 0x20u)
        o += 4;
    uint32_t col_off = o;
    if (d->has_color)
        o += 4;
    uint32_t spec_off = o;
    if (d->has_spec)
        o += 4;
    uint32_t uv_off[MAX_STAGES], uv_bytes[MAX_STAGES];
    int nuv = d->ntex < MAX_STAGES ? d->ntex : MAX_STAGES;
    for (int t = 0; t < nuv; t++) {
        uint32_t f = (fvf >> (16 + t * 2)) & 3u;
        uint32_t sz = f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
        uv_off[t] = o;
        uv_bytes[t] = sz >= 8u ? 8u : 4u;
        o += sz;
    }
    const int pretransformed = d->pretransformed, has_normal = d->has_normal;
    const int has_color = d->has_color, has_spec = d->has_spec;
    const uint8_t *vb = g_mem + base;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = vb + (size_t)(src ? src[i] : i) * stride;
        RVertex *v = &vbuf[i];
        memcpy(&v->x, p, 12);
        if (pretransformed)
            memcpy(&v->w, p + 12, 4);
        else
            v->w = 1.0f;
        if (has_normal)
            memcpy(&v->nx, p + nrm_off, 12);
        uint32_t col = 0xFFFFFFFFu, spec = 0xFF000000u;
        if (has_color)
            memcpy(&col, p + col_off, 4);
        if (has_spec)
            memcpy(&spec, p + spec_off, 4);
        v->color = d3dcolor_to_rgba(col);
        v->spec = d3dcolor_to_rgba(spec);
        for (int t = 0; t < nuv; t++) {
            if (uv_bytes[t] == 8u) {
                memcpy(v->uv[t], p + uv_off[t], 8);
            } else {
                memcpy(v->uv[t], p + uv_off[t], 4);
                v->uv[t][1] = 0.0f;
            }
        }
    }
    return vbuf;
}

static uint32_t prim_vertex_count(uint32_t prim, uint32_t n)
{
    (void)prim;
    return n;
}

/* ---- IDirect3DDevice7 --------------------------------------------------- */

static int in_scene;

static void dev_QueryInterface(Ctx *c)
{
    Guid iid;
    guid_read(ARG(1), &iid);
    rt_log("IDirect3DDevice7::QueryInterface(%s)", guid_str(&iid));
    WR32(ARG(2), 0);
    stdcall_ret(c, E_NOINTERFACE, 3);
}

static void dev_AddRef(Ctx *c) { stdcall_ret(c, 2, 1); }
static void dev_Release(Ctx *c) { stdcall_ret(c, 1, 1); }

static void dev_GetCaps(Ctx *c)
{
    write_device_desc(ARG(1), TNL_GUID, 1);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_EnumTextureFormats(Ctx *c)
{
    uint32_t cb = ARG(1), ctx = ARG(2);
    uint32_t pf = heap_alloc(32);
    static const PixFmt fmts[] = {
        { DDPF_RGB, 16, 0xF800u, 0x07E0u, 0x001Fu, 0 },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00u, 0x03E0u, 0x001Fu, 0x8000u },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00u, 0x00F0u, 0x000Fu, 0xF000u },
        { DDPF_RGB, 16, 0x7C00u, 0x03E0u, 0x001Fu, 0 },
        { DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u },
        { DDPF_RGB, 32, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0 },
    };
    for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        pixfmt_write(pf, &fmts[i]);
        uint32_t args[2] = { pf, ctx };
        if (!guest_call(c, cb, 2, args))
            break;
    }
    heap_free(pf);
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_BeginScene(Ctx *c)
{
    in_scene = 1;
    stdcall_ret(c, D3D_OK, 1);
}

static void dev_EndScene(Ctx *c)
{
    render_flush();
    in_scene = 0;
    stdcall_ret(c, D3D_OK, 1);
}

static void dev_GetDirect3D(Ctx *c)
{
    WR32(ARG(1), d3d_obj);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_SetRenderTarget(Ctx *c)
{
    render_flush();
    Surface *s = surface_from_obj(ARG(1));
    if (s)
        g_backbuffer = s;
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_GetRenderTarget(Ctx *c)
{
    uint32_t o = g_backbuffer ? surface_obj(g_backbuffer, 7) : 0u;
    if (g_backbuffer)
        g_backbuffer->refs++;
    WR32(ARG(1), o);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_Clear(Ctx *c)
{
    uint32_t n = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4);
    float z;
    uint32_t zb = ARG(5);
    memcpy(&z, &zb, 4);
    Rect r[16];
    if (n > 16u)
        n = 16u;
    for (uint32_t i = 0; i < n && rects; i++) {
        r[i].l = (int32_t)RD32(rects + i * 16u);
        r[i].t = (int32_t)RD32(rects + i * 16u + 4u);
        r[i].r = (int32_t)RD32(rects + i * 16u + 8u);
        r[i].b = (int32_t)RD32(rects + i * 16u + 12u);
    }
    render_clear(rects ? n : 0u, r, flags, color, z);
    stdcall_ret(c, D3D_OK, 7);
}

static float *transform_slot(uint32_t type)
{
    switch (type) {
    case 1: return g_rs.world;       /* D3DTRANSFORMSTATE_WORLD */
    case 2: return g_rs.view;
    case 3: return g_rs.proj;
    case 256: return g_rs.world;     /* D3DTS_WORLD alias */
    default: return NULL;
    }
}

static void dev_SetTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m && memcmp(m, gptr(ARG(2)), 64)) {
        render_state_changing();
        memcpy(m, gptr(ARG(2)), 64);
    }
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_GetTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m)
        memcpy(gptr(ARG(2)), m, 64);
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_MultiplyTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m) {
        float b[16], r[16];
        memcpy(b, gptr(ARG(2)), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = m[i * 4] * b[j] + m[i * 4 + 1] * b[4 + j] + m[i * 4 + 2] * b[8 + j] + m[i * 4 + 3] * b[12 + j];
        if (memcmp(m, r, 64)) {
            render_state_changing();
            memcpy(m, r, 64);
        }
    }
    stdcall_ret(c, D3D_OK, 3);
}


static void dev_SetViewport(Ctx *c)
{
    uint32_t v = ARG(1);
    int32_t vi[4];
    float vz[2];
    for (int k = 0; k < 4; k++)
        vi[k] = (int32_t)RD32(v + 4u * (uint32_t)k);
    memcpy(vz, gptr(v + 16u), 8);
    if (vi[0] != g_rs.vp_x || vi[1] != g_rs.vp_y || vi[2] != g_rs.vp_w || vi[3] != g_rs.vp_h ||
        memcmp(&vz[0], &g_rs.vp_minz, 4) || memcmp(&vz[1], &g_rs.vp_maxz, 4)) {
        render_state_changing();
        g_rs.vp_x = vi[0];
        g_rs.vp_y = vi[1];
        g_rs.vp_w = vi[2];
        g_rs.vp_h = vi[3];
        g_rs.vp_minz = vz[0];
        g_rs.vp_maxz = vz[1];
    }
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_GetViewport(Ctx *c)
{
    uint32_t v = ARG(1);
    WR32(v, (uint32_t)g_rs.vp_x);
    WR32(v + 4u, (uint32_t)g_rs.vp_y);
    WR32(v + 8u, (uint32_t)g_rs.vp_w);
    WR32(v + 12u, (uint32_t)g_rs.vp_h);
    memcpy(gptr(v + 16u), &g_rs.vp_minz, 4);
    memcpy(gptr(v + 20u), &g_rs.vp_maxz, 4);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_SetMaterial(Ctx *c)
{
    uint32_t m = ARG(1);
    const uint8_t *g = gptr(m);
    if (memcmp(g_rs.mat_diffuse, g, 16) || memcmp(g_rs.mat_ambient, g + 16, 16) ||
        memcmp(g_rs.mat_specular, g + 32, 16) || memcmp(g_rs.mat_emissive, g + 48, 16) ||
        memcmp(&g_rs.mat_power, g + 64, 4)) {
        render_state_changing();
        memcpy(g_rs.mat_diffuse, g, 16);
        memcpy(g_rs.mat_ambient, g + 16, 16);
        memcpy(g_rs.mat_specular, g + 32, 16);
        memcpy(g_rs.mat_emissive, g + 48, 16);
        memcpy(&g_rs.mat_power, g + 64, 4);
    }
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_GetMaterial(Ctx *c)
{
    uint32_t m = ARG(1);
    memcpy(gptr(m), g_rs.mat_diffuse, 16);
    memcpy(gptr(m + 16u), g_rs.mat_ambient, 16);
    memcpy(gptr(m + 32u), g_rs.mat_specular, 16);
    memcpy(gptr(m + 48u), g_rs.mat_emissive, 16);
    memcpy(gptr(m + 64u), &g_rs.mat_power, 4);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_SetLight(Ctx *c)
{
    uint32_t i = ARG(1), p = ARG(2);
    if (i < MAX_LIGHTS) {
        Light n = g_rs.lights[i];
        n.type = RD32(p);
        memcpy(n.diffuse, gptr(p + 4u), 16);
        memcpy(n.specular, gptr(p + 20u), 16);
        memcpy(n.ambient, gptr(p + 36u), 16);
        memcpy(n.pos, gptr(p + 52u), 12);
        memcpy(n.dir, gptr(p + 64u), 12);
        memcpy(&n.range, gptr(p + 76u), 28);
        if (memcmp(&n, &g_rs.lights[i], sizeof(n))) {
            render_state_changing();
            g_rs.lights[i] = n;
        }
    }
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_LightEnable(Ctx *c)
{
    uint32_t i = ARG(1);
    if (i < MAX_LIGHTS && g_rs.lights[i].enabled != (ARG(2) != 0)) {
        render_state_changing();
        g_rs.lights[i].enabled = ARG(2) != 0;
    }
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_GetLightEnable(Ctx *c)
{
    uint32_t i = ARG(1);
    WR32(ARG(2), i < MAX_LIGHTS ? (uint32_t)g_rs.lights[i].enabled : 0u);
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_SetRenderState(Ctx *c)
{
    uint32_t s = ARG(1);
    if (s < 256u && g_rs.rs[s] != ARG(2)) {
        render_state_changing();
        g_rs.rs[s] = ARG(2);
    }
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_GetRenderState(Ctx *c)
{
    uint32_t s = ARG(1);
    WR32(ARG(2), s < 256u ? g_rs.rs[s] : 0u);
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_SetTexture(Ctx *c)
{
    uint32_t stage = ARG(1);
    if (stage < MAX_STAGES) {
        Surface *t = surface_from_obj(ARG(2));
        if (g_rs.stage[stage].texture != t) {
            render_state_changing();
            g_rs.stage[stage].texture = t;
        }
    }
    stdcall_ret(c, D3D_OK, 3);
}

static void dev_GetTexture(Ctx *c)
{
    uint32_t stage = ARG(1);
    Surface *s = stage < MAX_STAGES ? g_rs.stage[stage].texture : NULL;
    if (s)
        s->refs++;
    WR32(ARG(2), s ? surface_obj(s, 7) : 0u);
    stdcall_ret(c, D3D_OK, 3);
}

static uint32_t *tss_slot(StageState *st, uint32_t type)
{
    switch (type) {
    case 1: return &st->colorop;
    case 2: return &st->colorarg1;
    case 3: return &st->colorarg2;
    case 4: return &st->alphaop;
    case 5: return &st->alphaarg1;
    case 6: return &st->alphaarg2;
    case 11: return &st->texcoordindex;
    case 13: return &st->addressu;
    case 14: return &st->addressv;
    case 16: return &st->magfilter;
    case 17: return &st->minfilter;
    case 18: return &st->mipfilter;
    default: return NULL;
    }
}

static void dev_SetTextureStageState(Ctx *c)
{
    uint32_t stage = ARG(1), type = ARG(2), value = ARG(3);
    if (stage < MAX_STAGES) {
        StageState *st = &g_rs.stage[stage];
        if (type == 12u) {           /* ADDRESS sets both */
            if (st->addressu != value || st->addressv != value)
                render_state_changing();
            st->addressu = st->addressv = value;
        } else {
            uint32_t *p = tss_slot(st, type);
            if (p && *p != value) {
                render_state_changing();
                *p = value;
            }
        }
    }
    stdcall_ret(c, D3D_OK, 4);
}

static void dev_GetTextureStageState(Ctx *c)
{
    uint32_t stage = ARG(1), type = ARG(2);
    uint32_t v = 0;
    if (stage < MAX_STAGES) {
        uint32_t *p = tss_slot(&g_rs.stage[stage], type);
        if (p)
            v = *p;
    }
    WR32(ARG(3), v);
    stdcall_ret(c, D3D_OK, 4);
}

static void dev_ValidateDevice(Ctx *c)
{
    WR32(ARG(1), 1);
    stdcall_ret(c, D3D_OK, 2);
}

/* Vertices `count` from `src` (or consecutive) out of a decoded copy. */
static RVertex *gather(const RVertex *cache, const uint32_t *src, uint32_t count)
{
    static RVertex *g;
    static uint32_t g_cap;
    if (count > g_cap) {
        g_cap = count + 256u;
        g = realloc(g, g_cap * sizeof(RVertex));
    }
    for (uint32_t i = 0; i < count; i++)
        g[i] = cache[src[i]];
    return g;
}

static void draw_indexed(uint32_t prim, uint32_t fvf, uint32_t verts, uint32_t stride, uint32_t nv,
                         uint32_t indices, uint32_t ni, RVertex *cache, const Decoded *cd);

/* `cache`, when set, is the decoded copy of the vertices at `verts` (with `cd` its format). */
static void draw(uint32_t prim, uint32_t fvf, uint32_t verts, uint32_t stride, uint32_t nv,
                 uint32_t indices, uint32_t ni, RVertex *cache, const Decoded *cd)
{
    if ((fvf & 0x0Eu) == 0x4u && !g_rs.stage[0].texture && igi_skip_untextured_2d())
        return;
    PROF_SECTION(PROF_H_INDEX);
    draw_indexed(prim, fvf, verts, stride, nv, indices, ni, cache, cd);
    PROF_SECTION_END;
}

static void draw_indexed(uint32_t prim, uint32_t fvf, uint32_t verts, uint32_t stride, uint32_t nv,
                         uint32_t indices, uint32_t ni, RVertex *cache, const Decoded *cd)
{
    Decoded d;
    if (indices && ni) {
        /* D3D passes the whole vertex buffer size; decode only the range the
         * indices touch and rebase them (IGI draws small pieces of big buffers). */
        static uint16_t *rebased;
        static uint32_t rebased_cap;
        const uint16_t *ip = (const uint16_t *)(g_mem + indices);   /* little endian, ldrh handles alignment */
        uint32_t lo = 0xFFFFu, hi = 0;
        for (uint32_t i = 0; i < ni; i++) {
            uint32_t x = ip[i];
            lo = x < lo ? x : lo;
            hi = x > hi ? x : hi;
        }
        if (hi >= nv)
            hi = nv ? nv - 1u : 0u;
        if (lo > hi)
            return;
        if (ni > rebased_cap) {
            rebased_cap = ni + 1024u;
            rebased = realloc(rebased, rebased_cap * sizeof(uint16_t));
        }
        uint32_t range = hi - lo + 1u;
        if (range <= ni || render_diag_no_compact()) {
            for (uint32_t i = 0; i < ni; i++)
                rebased[i] = (uint16_t)(ip[i] - lo);
            if (!cache) {
                emit(prim, fvf, verts + lo * stride, stride, NULL, range, rebased, ni);
                return;
            }
            d = *cd;
            render_draw((int)prim, cache + lo, range, rebased, ni, d.pretransformed, d.has_normal, d.has_color,
                        d.has_spec, d.ntex);
            return;
        }
        /* Sparse use of a big range: decode only the referenced vertices, in
         * first-use order. slot_gen marks which range entries this draw mapped. */
        static uint32_t *slot, *slot_gen, *used;
        static uint32_t slot_cap, used_cap, gen;
        if (range > slot_cap) {
            slot_cap = range + 1024u;
            slot = realloc(slot, slot_cap * sizeof(uint32_t));
            slot_gen = realloc(slot_gen, slot_cap * sizeof(uint32_t));
            memset(slot_gen, 0, slot_cap * sizeof(uint32_t));
            gen = 0;
        }
        if (ni > used_cap) {
            used_cap = ni + 1024u;
            used = realloc(used, used_cap * sizeof(uint32_t));
        }
        if (++gen == 0) {
            memset(slot_gen, 0, slot_cap * sizeof(uint32_t));
            gen = 1;
        }
        uint32_t nu = 0;
        for (uint32_t i = 0; i < ni; i++) {
            uint32_t x = (uint32_t)ip[i] - lo;
            if (x >= range)
                x = range - 1u;
            if (slot_gen[x] != gen) {
                slot_gen[x] = gen;
                slot[x] = nu;
                used[nu++] = x;
            }
            rebased[i] = (uint16_t)slot[x];
        }
        if (!cache) {
            emit(prim, fvf, verts + lo * stride, stride, used, nu, rebased, ni);
            return;
        }
        d = *cd;
        render_draw((int)prim, gather(cache + lo, used, nu), nu, rebased, ni, d.pretransformed, d.has_normal,
                    d.has_color, d.has_spec, d.ntex);
        return;
    }
    if (!cache) {
        emit(prim, fvf, verts, stride, NULL, nv, NULL, 0);
        return;
    }
    d = *cd;
    render_draw((int)prim, cache, nv, NULL, 0, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

static void dev_DrawPrimitive(Ctx *c)
{
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), n = ARG(4);
    draw(prim, fvf, verts, fvf_stride(fvf), prim_vertex_count(prim, n), 0, 0, NULL, NULL);
    stdcall_ret(c, D3D_OK, 6);
}

static void dev_DrawIndexedPrimitive(Ctx *c)
{
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), nv = ARG(4), idx = ARG(5), ni = ARG(6);
    draw(prim, fvf, verts, fvf_stride(fvf), nv, idx, ni, NULL, NULL);
    stdcall_ret(c, D3D_OK, 8);
}

/* Decoded copy of the whole buffer, refreshed after a writable Lock. */
static RVertex *vb_cache(VertexBuffer *vb, Decoded *d)
{
    /* Only buffers left untouched since an earlier frame: dynamic buffers
     * (refilled every frame) are decoded per draw, just the part drawn. */
    if (render_diag_no_vb_cache() || !vb->count || vb->lock_frame == g_frame_serial)
        return NULL;
    if (vb->cache_gen != vb->gen || !vb->cache) {
        if (!vb->cache)
            vb->cache = malloc((size_t)vb->count * sizeof(RVertex));
        RVertex *v = decode(vb->fvf, vb->mem, vb->stride, NULL, vb->count, d);
        memcpy(vb->cache, v, (size_t)vb->count * sizeof(RVertex));
        vb->cache_gen = vb->gen;
        for (int k = 0; k < 3; k++) {
            vb->lo[k] = 1e30f;
            vb->hi[k] = -1e30f;
        }
        for (uint32_t i = 0; i < vb->count; i++) {
            const float *p = &vb->cache[i].x;
            for (int k = 0; k < 3; k++) {
                vb->lo[k] = p[k] < vb->lo[k] ? p[k] : vb->lo[k];
                vb->hi[k] = p[k] > vb->hi[k] ? p[k] : vb->hi[k];
            }
        }
    } else {
        decode(vb->fvf, vb->mem, vb->stride, NULL, 0, d);   /* format only */
    }
    return vb->cache;
}

static void dev_DrawPrimitiveVB(Ctx *c)
{
    uint32_t prim = ARG(1), start = ARG(3), n = ARG(4);
    VertexBuffer *vb = com_host(ARG(2));
    Decoded d;
    RVertex *cache = vb_cache(vb, &d);
    if (start + n > vb->count)
        cache = NULL;
    if (cache && !d.pretransformed &&
        !((vb->fvf & 0x0Eu) == 0x4u && !g_rs.stage[0].texture && igi_skip_untextured_2d()) &&
        render_draw_vb(&vb->gl, vb->cache_gen, cache, vb->count, start, (int)prim, n, NULL, 0, d.has_normal,
                       d.has_color, d.has_spec, d.ntex)) {
        stdcall_ret(c, D3D_OK, 6);
        return;
    }
    if (cache)
        render_set_bounds(vb->lo, vb->hi);
    draw(prim, vb->fvf, vb->mem + start * vb->stride, vb->stride, n, 0, 0, cache ? cache + start : NULL, &d);
    render_set_bounds(NULL, NULL);
    stdcall_ret(c, D3D_OK, 6);
}

static void dev_DrawIndexedPrimitiveVB(Ctx *c)
{
    uint32_t prim = ARG(1), start = ARG(3), nv = ARG(4), idx = ARG(5), ni = ARG(6);
    VertexBuffer *vb = com_host(ARG(2));
    Decoded d;
    RVertex *cache = vb_cache(vb, &d);
    if (start + nv > vb->count)
        cache = NULL;
    if (cache && !d.pretransformed && idx && ni &&
        render_draw_vb(&vb->gl, vb->cache_gen, cache, vb->count, start, (int)prim, nv, (const uint16_t *)gptr(idx),
                       ni, d.has_normal, d.has_color, d.has_spec, d.ntex)) {
        stdcall_ret(c, D3D_OK, 8);
        return;
    }
    if (cache)
        render_set_bounds(vb->lo, vb->hi);
    draw(prim, vb->fvf, vb->mem + start * vb->stride, vb->stride, nv, idx, ni, cache ? cache + start : NULL, &d);
    render_set_bounds(NULL, NULL);
    stdcall_ret(c, D3D_OK, 8);
}

static void dev_SetClipStatus(Ctx *c) { stdcall_ret(c, D3D_OK, 2); }

static void dev_GetClipStatus(Ctx *c)
{
    memset(gptr(ARG(1)), 0, 24);
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_PreLoad(Ctx *c)
{
    render_texture_get(surface_from_obj(ARG(1)));
    stdcall_ret(c, D3D_OK, 2);
}

static void dev_Load(Ctx *c)
{
    render_flush();
    /* Copy the source texture (and mips) into the destination surface. */
    Surface *dst = surface_from_obj(ARG(1)), *src = surface_from_obj(ARG(3));
    for (; dst && src; dst = dst->attached, src = src->attached) {
        surface_alloc_mem(dst);
        surface_alloc_mem(src);
        uint32_t rows = dst->h < src->h ? dst->h : src->h;
        uint32_t bytes = dst->pitch < src->pitch ? dst->pitch : src->pitch;
        for (uint32_t y = 0; y < rows; y++)
            memcpy(g_mem + dst->mem + y * dst->pitch, g_mem + src->mem + y * src->pitch, bytes);
        dst->has_ck = src->has_ck;
        dst->ck_lo = src->ck_lo;
        dst->ck_hi = src->ck_hi;
        dst->dirty = 1;
        if (!dst->attached || !src->attached)
            break;
    }
    stdcall_ret(c, D3D_OK, 6);
}

static void dev_GetInfo(Ctx *c) { stdcall_ret(c, S_FALSE, 4); }

static void dev_ComputeSphereVisibility(Ctx *c)
{
    uint32_t n = ARG(3), out = ARG(5);
    for (uint32_t i = 0; i < n; i++)
        WR32(out + i * 4u, 0);
    stdcall_ret(c, D3D_OK, 6);
}

static const ComMethod device_methods[] = {
    { "QueryInterface", dev_QueryInterface },
    { "AddRef", dev_AddRef },
    { "Release", dev_Release },
    { "GetCaps", dev_GetCaps },
    { "EnumTextureFormats", dev_EnumTextureFormats },
    { "BeginScene", dev_BeginScene },
    { "EndScene", dev_EndScene },
    { "GetDirect3D", dev_GetDirect3D },
    { "SetRenderTarget", dev_SetRenderTarget },
    { "GetRenderTarget", dev_GetRenderTarget },
    { "Clear", dev_Clear },
    { "SetTransform", dev_SetTransform },
    { "GetTransform", dev_GetTransform },
    { "SetViewport", dev_SetViewport },
    { "MultiplyTransform", dev_MultiplyTransform },
    { "GetViewport", dev_GetViewport },
    { "SetMaterial", dev_SetMaterial },
    { "GetMaterial", dev_GetMaterial },
    { "SetLight", dev_SetLight },
    { "GetLight", NULL },
    { "SetRenderState", dev_SetRenderState },
    { "GetRenderState", dev_GetRenderState },
    { "BeginStateBlock", NULL },
    { "EndStateBlock", NULL },
    { "PreLoad", dev_PreLoad },
    { "DrawPrimitive", dev_DrawPrimitive },
    { "DrawIndexedPrimitive", dev_DrawIndexedPrimitive },
    { "SetClipStatus", dev_SetClipStatus },
    { "GetClipStatus", dev_GetClipStatus },
    { "DrawPrimitiveStrided", NULL },
    { "DrawIndexedPrimitiveStrided", NULL },
    { "DrawPrimitiveVB", dev_DrawPrimitiveVB },
    { "DrawIndexedPrimitiveVB", dev_DrawIndexedPrimitiveVB },
    { "ComputeSphereVisibility", dev_ComputeSphereVisibility },
    { "GetTexture", dev_GetTexture },
    { "SetTexture", dev_SetTexture },
    { "GetTextureStageState", dev_GetTextureStageState },
    { "SetTextureStageState", dev_SetTextureStageState },
    { "ValidateDevice", dev_ValidateDevice },
    { "ApplyStateBlock", NULL },
    { "CaptureStateBlock", NULL },
    { "DeleteStateBlock", NULL },
    { "CreateStateBlock", NULL },
    { "Load", dev_Load },
    { "LightEnable", dev_LightEnable },
    { "GetLightEnable", dev_GetLightEnable },
    { "SetClipPlane", NULL },
    { "GetClipPlane", NULL },
    { "GetInfo", dev_GetInfo },
};

/* ---- IDirect3DVertexBuffer7 --------------------------------------------- */

static void vb_AddRef(Ctx *c) { stdcall_ret(c, com_addref(THIS), 1); }

static void vb_Release(Ctx *c)
{
    uint32_t obj = THIS;
    VertexBuffer *vb = com_host(obj);
    uint32_t n = RD32(obj + 8u);
    if (n <= 1u) {
        heap_free(vb->mem);
        free(vb->cache);
        render_vb_release(&vb->gl);
        free(vb);
    }
    stdcall_ret(c, com_release(obj), 1);
}

static void vb_Lock(Ctx *c)
{
    VertexBuffer *vb = com_host(THIS);
    if (!(ARG(1) & 0x10u)) {   /* DDLOCK_READONLY */
        vb->gen++;
        vb->lock_frame = g_frame_serial;
    }
    WR32(ARG(2), vb->mem);
    if (ARG(3))
        WR32(ARG(3), vb->stride * vb->count);
    stdcall_ret(c, D3D_OK, 4);
}

static void vb_Unlock(Ctx *c) { stdcall_ret(c, D3D_OK, 1); }

static void vb_GetVertexBufferDesc(Ctx *c)
{
    VertexBuffer *vb = com_host(THIS);
    uint32_t d = ARG(1);
    WR32(d + 4u, 0);
    WR32(d + 8u, vb->fvf);
    WR32(d + 12u, vb->count);
    stdcall_ret(c, D3D_OK, 2);
}

static void vb_Optimize(Ctx *c) { stdcall_ret(c, D3D_OK, 3); }

static const ComMethod vb_methods[] = {
    { "QueryInterface", NULL },
    { "AddRef", vb_AddRef },
    { "Release", vb_Release },
    { "Lock", vb_Lock },
    { "Unlock", vb_Unlock },
    { "ProcessVertices", NULL },
    { "GetVertexBufferDesc", vb_GetVertexBufferDesc },
    { "Optimize", vb_Optimize },
    { "ProcessVerticesStrided", NULL },
};

uint32_t d3d7_create(void)
{
    if (!d3d_vtbl) {
        d3d_vtbl = com_vtable("IDirect3D7", d3d_methods, (int)(sizeof(d3d_methods) / sizeof(d3d_methods[0])));
        device_vtbl = com_vtable("IDirect3DDevice7", device_methods,
                                 (int)(sizeof(device_methods) / sizeof(device_methods[0])));
        vb_vtbl = com_vtable("IDirect3DVertexBuffer7", vb_methods, (int)(sizeof(vb_methods) / sizeof(vb_methods[0])));
        d3d_obj = com_new(d3d_vtbl, NULL);
    }
    return d3d_obj;
}
