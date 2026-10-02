/* Direct3D 7: IDirect3D7, IDirect3DDevice7 and IDirect3DVertexBuffer7. */
#include "igi_fixes.h"
#include "platform.h"
#include "render.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

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
    struct VBVersion *cache;   /* whole buffer decoded (static geometry is drawn every frame) */
    void *gl;                  /* renderer's GPU copy (render_draw_vb) */
    uint32_t lock_id;          /* unique id of the last writable Lock (all buffers) */
    uint32_t drawn_lock, drawn_start, drawn_nv;   /* lock_id and range of the last indexed draw */
    uint32_t drawn_count;      /* indexed draws in a row of that lock and range */
    int multi_drawn;           /* the previous lock's contents were drawn so at least twice */
    struct VBData *data;       /* pack thread: the contents recorded draws read in place */
    uint32_t spare_mem[2];     /* pack thread: memory the buffer had while draws read it ... */
    uint64_t spare_ref[2];     /* ... until these rq_mark()s */
    int spare_tried;
} VertexBuffer;
#define VB_SPARES 2
#define DDLOCK_DISCARDCONTENTS 0x2000u

static uint32_t vb_lock_serial;

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

/* The sampling profiler's section markers belong to the game thread: code that
 * runs on the render thread with the pack thread only sets them without it. */
#if defined(IGI_PROFILE)
#define PROF_RS(id)                          \
    const uint32_t prof_hprev = rt_prof_cur; \
    if (!g_pack_thread)                      \
    rt_prof_cur = (id)
#define PROF_RS_END                   \
    do {                              \
        if (!g_pack_thread)           \
            rt_prof_cur = prof_hprev; \
    } while (0)
#else
#define PROF_RS(id) ((void)0)
#define PROF_RS_END ((void)0)
#endif

/* Decodes `count` vertices into `out`: vertex i comes from vb + (src ? src[i] : i) * stride. */
static void decode_vertices(uint32_t fvf, const uint8_t *vb, uint32_t stride, const uint32_t *src, uint32_t count,
                            Decoded *d, RVertex *out);

/* Same into the draw scratch buffer (front end). */
static RVertex *decode(uint32_t fvf, const uint8_t *vb, uint32_t stride, const uint32_t *src, uint32_t count,
                       Decoded *d)
{
    PROF_RS(PROF_H_DECODE);
    if (count > vbuf_cap) {
        vbuf_cap = count + 256u;
        vbuf = realloc(vbuf, vbuf_cap * sizeof(RVertex));
    }
    decode_vertices(fvf, vb, stride, src, count, d, vbuf);
    PROF_RS_END;
    return vbuf;
}

/* The game's vertex layout for the renderer (vertex i at base + (src ? src[i] : i) * stride). */
static void guest_verts(uint32_t fvf, const uint8_t *base, uint32_t stride, const uint32_t *src, GuestVerts *g)
{
    uint32_t pos = fvf & 0x0Eu;
    memset(g, 0, sizeof(*g));
    g->base = base;
    g->stride = stride;
    g->src = src;
    g->has_normal = (fvf & 0x10u) != 0;
    g->has_color = (fvf & 0x40u) != 0;
    g->has_spec = (fvf & 0x80u) != 0;
    g->ntex = (int)((fvf >> 8) & 0xFu);
    g->pretransformed = pos == 0x4u;
    uint32_t o = pos == 0x4u ? 16u : pos == 0x2u ? 12u : 12u + ((pos - 0x4u) / 2u) * 4u;
    g->nrm_off = o;
    if (g->has_normal)
        o += 12;
    if (fvf & 0x20u)
        o += 4;
    g->col_off = o;
    if (g->has_color)
        o += 4;
    g->spec_off = o;
    if (g->has_spec)
        o += 4;
    for (int t = 0; t < g->ntex && t < MAX_STAGES; t++) {
        uint32_t f = (fvf >> (16 + t * 2)) & 3u;
        uint32_t sz = f == 0 ? 8u : f == 1 ? 12u : f == 2 ? 16u : 4u;
        g->uv_off[t] = o;
        g->uv_bytes[t] = sz >= 8u ? 8u : 4u;
        o += sz;
    }
}

/* Hands untransformed vertices to the renderer in the game's layout when it can
 * take them directly; otherwise decodes them to RVertex. */
static void emit(uint32_t prim, uint32_t fvf, const uint8_t *base, uint32_t stride, const uint32_t *src,
                 uint32_t count, const uint16_t *idx, uint32_t ni)
{
    {
        GuestVerts g;
        guest_verts(fvf, base, stride, src, &g);
        if (render_draw_guest((int)prim, &g, count, idx, ni))
            return;
    }
    Decoded d;
    RVertex *v = decode(fvf, base, stride, src, count, &d);
    render_draw((int)prim, v, count, idx, ni, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

static void decode_vertices(uint32_t fvf, const uint8_t *vb, uint32_t stride, const uint32_t *src, uint32_t count,
                            Decoded *d, RVertex *out)
{
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
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = vb + (size_t)(src ? src[i] : i) * stride;
        RVertex *v = &out[i];
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

/* Inequality of n bytes (n a multiple of 4) as 32-bit words: newlib's memcmp
 * is a byte loop, and these run on every state call. */
static int words_differ(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a, *pb = b;
    uint32_t acc = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t x, y;
        memcpy(&x, pa + i, 4);
        memcpy(&y, pb + i, 4);
        acc |= x ^ y;
    }
    return acc != 0;
}

static void dev_SetTransform(Ctx *c)
{
    float *m = transform_slot(ARG(1));
    if (m && words_differ(m, gptr(ARG(2)), 64)) {
        render_state_changing();
        g_rs_parts |= m == g_rs.world ? RS_PART_WORLD : RS_PART_XF;
        memcpy(m, gptr(ARG(2)), 64);
        if (m == g_rs.view)
            g_light_serial++;
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
        if (words_differ(m, r, 64)) {
            render_state_changing();
            g_rs_parts |= m == g_rs.world ? RS_PART_WORLD : RS_PART_XF;
            memcpy(m, r, 64);
            if (m == g_rs.view)
                g_light_serial++;
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
        g_rs_parts |= RS_PART_XF;
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
    if (words_differ(g_rs.mat_diffuse, g, 16) || words_differ(g_rs.mat_ambient, g + 16, 16) ||
        words_differ(g_rs.mat_specular, g + 32, 16) || words_differ(g_rs.mat_emissive, g + 48, 16) ||
        words_differ(&g_rs.mat_power, g + 64, 4)) {
        render_state_changing();
        g_light_serial++;
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
        if (words_differ(&n, &g_rs.lights[i], sizeof(n))) {
            render_state_changing();
            g_light_serial++;
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
        g_light_serial++;
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
        g_rs_parts |= RS_PART_RS;
        if (s == 139u || s == 141u || s == 143u || s == 145u)   /* AMBIENT, COLORVERTEX, NORMALIZENORMALS, DIFFUSEMATERIALSOURCE */
            g_light_serial++;
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
            g_rs_parts |= RS_PART_STAGE;
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
            if (st->addressu != value || st->addressv != value) {
                render_state_changing();
                g_rs_parts |= RS_PART_STAGE;
            }
            st->addressu = st->addressv = value;
        } else {
            uint32_t *p = tss_slot(st, type);
            if (p && *p != value) {
                render_state_changing();
                g_rs_parts |= RS_PART_STAGE;
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

/* Bounds of vertex ranges of a decoded buffer, remembered per range: static
 * geometry draws the same ranges every frame, and bounds that fit the draw
 * (rather than the whole buffer) usually show it in front of the near plane,
 * which spares the renderer its per-vertex depth test. */
typedef struct RangeBounds {
    uint32_t first, count, gen;
    float lo[3], hi[3];
} RangeBounds;
#define RANGE_BITS 10

/* A vertex buffer's contents decoded, for one version of them (gen). Without the
 * pack thread it is refreshed in place; with it the render thread may still draw
 * from it, so a new version is made instead and the old one freed by the render
 * thread once the draws recorded before are done. The range bounds are the
 * front end's (render thread with the pack thread). */
typedef struct VBVersion {
    RVertex *v;
    uint32_t count, gen;
    float lo[3], hi[3];        /* bounds of all vertices */
    Decoded d;
    RangeBounds *ranges;       /* bounds of drawn ranges (set_range_bounds) */
} VBVersion;

static void vb_version_free(VBVersion *c)
{
    free(c->v);
    free(c->ranges);
    free(c);
}

static void pk_free_version(const void *p)
{
    VBVersion *c;
    memcpy(&c, p, sizeof(c));
    vb_version_free(c);
}

/* The buffer no longer uses this version. */
static void vb_version_retire(VBVersion *c)
{
    if (g_pack_thread)
        rq_exec(pk_free_version, &c, sizeof(c));
    else
        vb_version_free(c);
}

static void draw_indexed(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t vfirst, uint32_t stride,
                         uint32_t nv, const uint16_t *ip, uint32_t ni, const RVertex *cache, const Decoded *cd);

static VBVersion *draw_vb;    /* decoded copy the draw in progress uses */
static uint32_t draw_vb_start;

/* Sets the renderer's bounds to those of vertices first .. first + count - 1 of
 * the draw's decoded vertices (relative to its start vertex). */
static void set_range_bounds(uint32_t first, uint32_t count)
{
    VBVersion *vb = draw_vb;
    if (!vb)
        return;
    first += draw_vb_start;
    if (!count || first + count > vb->count)
        return;
    if (!vb->ranges) {
        vb->ranges = calloc(1u << RANGE_BITS, sizeof(RangeBounds));
        if (!vb->ranges)
            return;
    }
    RangeBounds *e = &vb->ranges[((first * 2654435761u) ^ (count * 40503u)) >> (32 - RANGE_BITS)];
    if (e->gen != vb->gen || e->first != first || e->count != count) {
        for (int k = 0; k < 3; k++) {
            e->lo[k] = 1e30f;
            e->hi[k] = -1e30f;
        }
        for (uint32_t i = first; i < first + count; i++) {
            const float *p = &vb->v[i].x;
            for (int k = 0; k < 3; k++) {
                e->lo[k] = p[k] < e->lo[k] ? p[k] : e->lo[k];
                e->hi[k] = p[k] > e->hi[k] ? p[k] : e->hi[k];
            }
        }
        e->first = first;
        e->count = count;
        e->gen = vb->gen;
    }
    render_set_bounds(e->lo, e->hi);
}

/* Pre-transformed draws without a texture the game wants hidden (igi_fixes). */
static int skip_untextured_2d(uint32_t fvf)
{
    return (fvf & 0x0Eu) == 0x4u && !g_rs.stage[0].texture && igi_skip_untextured_2d();
}

/* `cache`, when set, is the decoded copy of the vertices at `verts` (with `cd` its format). */
static void draw(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t stride, uint32_t nv,
                 const uint16_t *ip, uint32_t ni, const RVertex *cache, const Decoded *cd)
{
    if (skip_untextured_2d(fvf))
        return;
    PROF_SECTION(PROF_H_INDEX);
    draw_indexed(prim, fvf, verts, 0, stride, nv, ip, ni, cache, cd);
    PROF_SECTION_END;
}

/* Vertex i of the draw is at verts + (i - vfirst) * stride (vfirst: the first
 * vertex the memory at `verts` holds). */
static void draw_indexed(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t vfirst, uint32_t stride,
                         uint32_t nv, const uint16_t *ip, uint32_t ni, const RVertex *cache, const Decoded *cd)
{
    Decoded d;
    if (ip && ni) {
        /* D3D passes the whole vertex buffer size; decode only the range the
         * indices touch and rebase them (IGI draws small pieces of big buffers). */
        static uint16_t *rebased;
        static uint32_t rebased_cap;
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
        const uint8_t *vlo = cache ? NULL : verts + (size_t)(lo - vfirst) * stride;
        if (range <= ni || render_diag_no_compact()) {
            for (uint32_t i = 0; i < ni; i++)
                rebased[i] = (uint16_t)(ip[i] - lo);
            if (!cache) {
                emit(prim, fvf, vlo, stride, NULL, range, rebased, ni);
                return;
            }
            d = *cd;
            set_range_bounds(lo, range);
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
            emit(prim, fvf, vlo, stride, used, nu, rebased, ni);
            return;
        }
        d = *cd;
        set_range_bounds(lo, range);
        render_draw((int)prim, gather(cache + lo, used, nu), nu, rebased, ni, d.pretransformed, d.has_normal,
                    d.has_color, d.has_spec, d.ntex);
        return;
    }
    if (!cache) {
        emit(prim, fvf, verts, stride, NULL, nv, NULL, 0);
        return;
    }
    d = *cd;
    set_range_bounds(0, nv);
    render_draw((int)prim, cache, nv, NULL, 0, d.pretransformed, d.has_normal, d.has_color, d.has_spec, d.ntex);
}

/* Decodes the buffer contents at `base` into the version, with their bounds. */
static void vb_version_decode(VBVersion *c, uint32_t fvf, const uint8_t *base, uint32_t stride)
{
    decode_vertices(fvf, base, stride, NULL, c->count, &c->d, c->v);
    for (int k = 0; k < 3; k++) {
        c->lo[k] = 1e30f;
        c->hi[k] = -1e30f;
    }
    for (uint32_t i = 0; i < c->count; i++) {
        const float *p = &c->v[i].x;
        for (int k = 0; k < 3; k++) {
            c->lo[k] = p[k] < c->lo[k] ? p[k] : c->lo[k];
            c->hi[k] = p[k] > c->hi[k] ? p[k] : c->hi[k];
        }
    }
}

static void pk_decode_later(VertexBuffer *vb, VBVersion *c);

/* Decoded copy of the whole buffer, refreshed after a writable Lock. */
static VBVersion *vb_cache(VertexBuffer *vb)
{
    /* Only buffers left untouched since an earlier frame: dynamic buffers
     * (refilled every frame) are decoded per draw, just the part drawn. */
    if (render_diag_no_vb_cache() || !vb->count || vb->lock_frame == g_frame_serial)
        return NULL;
    VBVersion *c = vb->cache;
    if (c && c->gen == vb->gen)
        return c;
    if (c && g_pack_thread) {
        vb_version_retire(c);
        c = NULL;
    }
    if (!c) {
        c = calloc(1, sizeof(*c));
        c->v = malloc((size_t)vb->count * sizeof(RVertex));
        c->count = vb->count;
        vb->cache = c;
    }
    c->gen = vb->gen;
    if (g_pack_thread) {
        pk_decode_later(vb, c);   /* by the render thread, before the draws using it */
        return c;
    }
    PROF_SECTION(PROF_H_DECODE);
    vb_version_decode(c, vb->fvf, g_mem + vb->mem, vb->stride);
    PROF_SECTION_END;
    return c;
}

/* ---- pack thread: draws recorded for the render thread ---------------------
 * The game's vertex and index memory may change right after the call, so a draw
 * records what it will read in a form nothing changes before the render thread
 * has drawn it:
 *  - indices and the vertices of DrawPrimitive/DrawIndexedPrimitive: copies;
 *  - unchanged vertex buffers: their decoded copy (VBVersion, never modified);
 *  - vertex buffers refilled this frame: the buffer memory itself, through a
 *    VBData. A writable Lock (or Release) while draws reading it are still to be
 *    executed first moves them to a copy of the bytes they read.
 * The render thread runs draw_indexed and the renderer's front end on them, as
 * the game thread does without the pack thread. */
typedef struct VBData {
    _Atomic uintptr_t base;    /* address of vertex 0 of the buffer's contents */
    _Atomic int busy;          /* the render thread is reading through base */
    uint64_t last_ref;         /* rq_mark() after the last draw recorded with it */
    uint32_t lo, hi;           /* bytes [lo, hi) of the buffer the draws read */
    uint8_t *copy;             /* after the move */
} VBData;

static void pk_free_data(const void *p)
{
    VBData *d;
    memcpy(&d, p, sizeof(d));
    free(d->copy);
    free(d);
}

/* The buffer's memory is about to change (lock_flags: of the writable Lock) or go
 * away (release): draws recorded with its current contents keep them. A Lock that
 * discards the contents (the terrain's, every frame) gives the game other memory
 * for the buffer while they are pending; otherwise they move to a copy. */
static void vb_data_detach(VertexBuffer *vb, uint32_t lock_flags, int release)
{
    VBData *d = vb->data;
    if (!d)
        return;
    vb->data = NULL;
    if (rq_passed(d->last_ref)) {
        free(d);
        return;
    }
    if (!release && (lock_flags & DDLOCK_DISCARDCONTENTS)) {
        for (int i = 0; i < VB_SPARES && vb->spare_mem[i]; i++) {
            if (!rq_passed(vb->spare_ref[i]))
                continue;
            uint32_t m = vb->spare_mem[i];
            vb->spare_mem[i] = vb->mem;   /* read by the pending draws until spare_ref */
            vb->spare_ref[i] = d->last_ref;
            vb->mem = m;
            rq_exec(pk_free_data, &d, sizeof(d));
            return;
        }
    }
    uint32_t n = d->hi > d->lo ? d->hi - d->lo : 0u;
    d->copy = malloc(n ? n : 1u);
    memcpy(d->copy, g_mem + vb->mem + d->lo, n);
    /* Sequentially consistent with the render thread's busy/base pair: after this
     * either it sees the copy, or it is still in the draw that read the buffer. */
    atomic_store(&d->base, (uintptr_t)d->copy - d->lo);
    while (atomic_load(&d->busy))
        ;
    rq_exec(pk_free_data, &d, sizeof(d));   /* after the draws recorded so far */
}

/* Release: the buffer's memory and spares, once no recorded draw reads them. */
static void vb_free_memory(VertexBuffer *vb)
{
    vb_data_detach(vb, 0, 1);
    for (int i = 0; i < VB_SPARES; i++)
        if (vb->spare_mem[i] && !rq_passed(vb->spare_ref[i]))
            rq_sync();
    heap_free(vb->mem);
    for (int i = 0; i < VB_SPARES; i++)
        heap_free(vb->spare_mem[i]);
}

static VBData *vb_data(VertexBuffer *vb, uint32_t first, uint32_t count)
{
    VBData *d = vb->data;
    uint32_t lo = first * vb->stride, hi = (first + count) * vb->stride;
    if (!d) {
        d = calloc(1, sizeof(*d));
        atomic_store(&d->base, (uintptr_t)(g_mem + vb->mem));
        d->lo = lo;
        d->hi = hi;
        vb->data = d;
    }
    d->lo = lo < d->lo ? lo : d->lo;
    d->hi = hi > d->hi ? hi : d->hi;
    return d;
}

typedef struct PkDecode {
    VBVersion *ver;
    VBData *data;
    uint32_t fvf, stride;
} PkDecode;

/* Render thread: decodes a buffer's new contents into its new version. */
static void pk_decode_cmd(const void *p)
{
    const PkDecode *k = p;
    VBData *d = k->data;
    PROF_RS(PROF_H_DECODE);
    atomic_store(&d->busy, 1);
    vb_version_decode(k->ver, k->fvf, (const uint8_t *)atomic_load(&d->base), k->stride);
    atomic_store(&d->busy, 0);
    PROF_RS_END;
}

static void pk_decode_later(VertexBuffer *vb, VBVersion *c)
{
    VBData *d = vb_data(vb, 0, vb->count);
    PkDecode *k = rq_begin(pk_decode_cmd, sizeof(PkDecode));
    k->ver = c;
    k->data = d;
    k->fvf = vb->fvf;
    k->stride = vb->stride;
    rq_end();
    d->last_ref = rq_mark();
}

enum { PK_RAW, PK_CACHE, PK_VB };

typedef struct PkDraw {
    uint8_t kind, diag, trace, dyn;   /* dyn (PK_VB): the terrain path (render_draw_dynvb) */
    uint32_t prim, fvf, stride;
    uint32_t nv, ni;
    uint32_t vfirst;           /* PK_RAW: the vertex `verts` starts with */
    uint32_t start, lock_id;   /* PK_CACHE, PK_VB: start vertex in the buffer; lock_id: its Lock */
    const uint8_t *verts;      /* PK_RAW: copied vertices */
    const uint16_t *idx;       /* copied indices, NULL: none */
    VBVersion *cache;          /* PK_CACHE */
    VBData *data;              /* PK_VB */
} PkDraw;

/* Render thread: one recorded draw. */
static void pk_exec(const void *p)
{
    const PkDraw *c = p;
    render_pk_draw_begin(c->diag, c->trace);
    if (c->kind == PK_CACHE) {
        VBVersion *ver = c->cache;
        render_set_bounds(ver->lo, ver->hi);
        draw_vb = ver;
        draw_vb_start = c->start;
        draw_indexed(c->prim, c->fvf, NULL, 0, c->stride, c->nv, c->idx, c->ni, ver->v + c->start, &ver->d);
        draw_vb = NULL;
        render_set_bounds(NULL, NULL);
        return;
    }
    if (c->kind == PK_RAW) {
        draw_indexed(c->prim, c->fvf, c->verts, c->vfirst, c->stride, c->nv, c->idx, c->ni, NULL, NULL);
        return;
    }
    VBData *d = c->data;
    atomic_store(&d->busy, 1);
    const uint8_t *verts = (const uint8_t *)atomic_load(&d->base) + (size_t)c->start * c->stride;
    GuestVerts g;
    if (c->dyn) {
        guest_verts(c->fvf, verts, c->stride, NULL, &g);
        if (render_draw_dynvb(c->lock_id, c->start, &g, c->nv, (int)c->prim, c->idx, c->ni)) {
            atomic_store(&d->busy, 0);
            return;
        }
    }
    draw_indexed(c->prim, c->fvf, verts, 0, c->stride, c->nv, c->idx, c->ni, NULL, NULL);
    atomic_store(&d->busy, 0);
}

/* Small copies inline: on the Vita memcpy is a call into SceLibKernel. */
static void copy_bytes(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n > 256u) {
        memcpy(d, s, n);
        return;
    }
    size_t i = 0;
    for (; i + 4u <= n; i += 4u) {
        uint32_t w;
        memcpy(&w, s + i, 4);
        memcpy(d + i, &w, 4);
    }
    for (; i < n; i++)
        d[i] = s[i];
}

static const uint8_t *pk_bytes(const uint8_t *s, size_t n)
{
    uint8_t *d = rq_data(n);
    copy_bytes(d, s, n);
    return d;
}

/* Copies n indices from the game and returns their lowest and highest. */
static uint16_t *pk_indices(const uint16_t *s, uint32_t n, uint32_t *lo_out, uint32_t *hi_out)
{
    uint16_t *d = rq_data((size_t)n * sizeof(uint16_t));
    uint32_t i = 0, lo = 0xFFFFu, hi = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if (n >= 8u && !((uintptr_t)s & 1u)) {
        uint16x8_t a = vdupq_n_u16(0xFFFFu), b = vdupq_n_u16(0);
        for (; i + 8u <= n; i += 8u) {
            uint16x8_t v = vld1q_u16(s + i);
            vst1q_u16(d + i, v);
            a = vminq_u16(a, v);
            b = vmaxq_u16(b, v);
        }
        uint16x4_t a4 = vmin_u16(vget_low_u16(a), vget_high_u16(a));
        uint16x4_t b4 = vmax_u16(vget_low_u16(b), vget_high_u16(b));
        a4 = vpmin_u16(a4, a4);
        a4 = vpmin_u16(a4, a4);
        b4 = vpmax_u16(b4, b4);
        b4 = vpmax_u16(b4, b4);
        lo = vget_lane_u16(a4, 0);
        hi = vget_lane_u16(b4, 0);
    }
#endif
    for (; i < n; i++) {
        uint16_t x;
        memcpy(&x, s + i, 2);   /* the game's indices may be 2-byte aligned only */
        d[i] = x;
        lo = x < lo ? x : lo;
        hi = x > hi ? x : hi;
    }
    *lo_out = lo;
    *hi_out = hi;
    return d;
}

/* The draw command, after the state it needs (render_pk_prepare). */
static PkDraw *pk_begin(uint32_t kind, uint32_t prim, uint32_t fvf, uint32_t stride, uint32_t nv)
{
    render_pk_prepare((fvf & 0x0Eu) == 0x4u, (fvf & 0x10u) != 0);
    PkDraw *c = rq_begin(pk_exec, sizeof(PkDraw));
    c->kind = (uint8_t)kind;
    c->diag = (uint8_t)render_diag_mode();
    c->trace = g_trace_next_frame == 2;
    c->dyn = 0;
    c->prim = prim;
    c->fvf = fvf;
    c->stride = stride;
    c->nv = nv;
    c->ni = 0;
    c->vfirst = 0;
    c->start = 0;
    c->lock_id = 0;
    c->verts = NULL;
    c->idx = NULL;
    c->cache = NULL;
    c->data = NULL;
    return c;
}

static void pk_end(void)
{
    rq_end();
    render_pk_recorded();
}

/* Records a draw of the game's vertices at `verts` (vertex 0 of the draw) with
 * indices `ip` (or none): the same choices as draw_indexed, so the render thread
 * gets the vertices draw_indexed would read. */
static void pk_record_raw_ic(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t stride, uint32_t nv,
                             uint16_t *ic, uint32_t ni, uint32_t lo, uint32_t hi);

static void pk_record_raw(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t stride, uint32_t nv,
                          const uint16_t *ip, uint32_t ni)
{
    uint32_t lo = 0, hi = 0;
    uint16_t *ic = ip && ni ? pk_indices(ip, ni, &lo, &hi) : NULL;
    pk_record_raw_ic(prim, fvf, verts, stride, nv, ic, ni, lo, hi);
}

/* Same with the indices copied already (ic, NULL: none; lo, hi: their lowest and highest). */
static void pk_record_raw_ic(uint32_t prim, uint32_t fvf, const uint8_t *verts, uint32_t stride, uint32_t nv,
                             uint16_t *ic, uint32_t ni, uint32_t lo, uint32_t hi)
{
    if (!(ic && ni)) {
        const uint8_t *v = pk_bytes(verts, (size_t)nv * stride);
        PkDraw *c = pk_begin(PK_RAW, prim, fvf, stride, nv);
        c->verts = v;
        pk_end();
        return;
    }
    if (hi >= nv)
        hi = nv ? nv - 1u : 0u;
    if (lo > hi)
        return;
    uint32_t range = hi - lo + 1u;
    if (range <= ni || render_pk_no_compact()) {
        const uint8_t *v = pk_bytes(verts + (size_t)lo * stride, (size_t)range * stride);
        PkDraw *c = pk_begin(PK_RAW, prim, fvf, stride, nv);
        c->verts = v;
        c->vfirst = lo;
        c->idx = ic;
        c->ni = ni;
        pk_end();
        return;
    }
    /* Sparse use of a big range: only the referenced vertices, in first-use order,
     * with the indices rebased to them (what draw_indexed would make of it). */
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
        uint32_t x = (uint32_t)ic[i] - lo;
        if (x >= range)
            x = range - 1u;
        if (slot_gen[x] != gen) {
            slot_gen[x] = gen;
            slot[x] = nu;
            used[nu++] = x;
        }
        ic[i] = (uint16_t)slot[x];
    }
    uint8_t *v = rq_data((size_t)nu * stride);
    const uint8_t *s = verts + (size_t)lo * stride;
    for (uint32_t i = 0; i < nu; i++)
        copy_bytes(v + (size_t)i * stride, s + (size_t)used[i] * stride, stride);
    PkDraw *c = pk_begin(PK_RAW, prim, fvf, stride, nu);
    c->verts = v;
    c->idx = ic;
    c->ni = ni;
    pk_end();
}

/* Records a draw from a vertex buffer (DrawPrimitiveVB: no indices). */
static void pk_draw_vb(VertexBuffer *vb, uint32_t prim, uint32_t start, uint32_t nv, const uint16_t *ip, uint32_t ni)
{
    if (skip_untextured_2d(vb->fvf))
        return;
    VBVersion *ver = vb_cache(vb);
    if (ver && start + nv > vb->count)
        ver = NULL;
    uint16_t *ic = NULL;
    uint32_t lo = 0, hi = nv ? nv - 1u : 0u;
    if (ip && ni)
        ic = pk_indices(ip, ni, &lo, &hi);
    else
        ni = 0;
    if (ver) {
        PkDraw *c = pk_begin(PK_CACHE, prim, vb->fvf, vb->stride, nv);
        c->start = start;
        c->cache = ver;
        c->idx = ic;
        c->ni = ni;
        pk_end();
        return;
    }
    /* A pre-transformed buffer filled for this frame and drawn again over the
     * same range (the terrain): render_draw_dynvb packs it once for all such
     * draws (see dev_DrawIndexedPrimitiveVB). */
    int dyn = 0;
    if (ni && vb->lock_id && (vb->fvf & 0x0Eu) == 0x4u && start + nv <= vb->count) {
        int first = vb->drawn_lock != vb->lock_id;
        int same = !first && vb->drawn_start == start && vb->drawn_nv == nv;
        if (first)
            vb->multi_drawn = vb->drawn_count >= 2u;
        vb->drawn_count = same ? vb->drawn_count + 1u : 1u;
        vb->drawn_lock = vb->lock_id;
        vb->drawn_start = start;
        vb->drawn_nv = nv;
        dyn = same || (first && vb->multi_drawn);
    }
    if (!dyn) {
        /* Buffers refilled between draws (sprites: a Lock per quad): copies of
         * what each draw reads cost less than moving pending draws at each Lock. */
        pk_record_raw_ic(prim, vb->fvf, g_mem + vb->mem + (size_t)start * vb->stride, vb->stride, nv, ic, ni, lo, hi);
        return;
    }
    if (!vb->spare_tried) {
        vb->spare_tried = 1;
        /* For the discarding Lock that follows (vb_data_detach). Allocated at a
         * point every run reaches the same way: the guest heap stays the same. */
        uint32_t bytes = vb->stride * vb->count + 16u;
        uint32_t a = heap_alloc(bytes), b = heap_alloc(bytes);
        if (a && b) {
            vb->spare_mem[0] = a;
            vb->spare_mem[1] = b;
            vb->spare_ref[0] = vb->spare_ref[1] = 0;
        } else {
            heap_free(a);
            heap_free(b);
        }
    }
    /* The terrain path packs all nv vertices at once. */
    VBData *d = vb_data(vb, start, nv);
    PkDraw *c = pk_begin(PK_VB, prim, vb->fvf, vb->stride, nv);
    c->dyn = (uint8_t)dyn;
    c->start = start;
    c->lock_id = vb->lock_id;
    c->idx = ic;
    c->ni = ni;
    c->data = d;
    pk_end();
    d->last_ref = rq_mark();
}

static void dev_DrawPrimitive(Ctx *c)
{
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), n = ARG(4);
    if (g_pack_thread) {
        if (!skip_untextured_2d(fvf)) {
            PROF_SECTION(PROF_H_INDEX);
            pk_record_raw(prim, fvf, g_mem + verts, fvf_stride(fvf), prim_vertex_count(prim, n), NULL, 0);
            PROF_SECTION_END;
        }
        stdcall_ret(c, D3D_OK, 6);
        return;
    }
    draw(prim, fvf, g_mem + verts, fvf_stride(fvf), prim_vertex_count(prim, n), NULL, 0, NULL, NULL);
    stdcall_ret(c, D3D_OK, 6);
}

static void dev_DrawIndexedPrimitive(Ctx *c)
{
    uint32_t prim = ARG(1), fvf = ARG(2), verts = ARG(3), nv = ARG(4), idx = ARG(5), ni = ARG(6);
    const uint16_t *ip = idx ? (const uint16_t *)gptr(idx) : NULL;   /* little endian, ldrh handles alignment */
    if (g_pack_thread) {
        if (!skip_untextured_2d(fvf)) {
            PROF_SECTION(PROF_H_INDEX);
            pk_record_raw(prim, fvf, g_mem + verts, fvf_stride(fvf), nv, ip, ni);
            PROF_SECTION_END;
        }
        stdcall_ret(c, D3D_OK, 8);
        return;
    }
    draw(prim, fvf, g_mem + verts, fvf_stride(fvf), nv, ip, ni, NULL, NULL);
    stdcall_ret(c, D3D_OK, 8);
}

static void dev_DrawPrimitiveVB(Ctx *c)
{
    uint32_t prim = ARG(1), start = ARG(3), n = ARG(4);
    VertexBuffer *vb = com_host(ARG(2));
    if (g_pack_thread) {
        PROF_SECTION(PROF_H_INDEX);
        pk_draw_vb(vb, prim, start, n, NULL, 0);
        PROF_SECTION_END;
        stdcall_ret(c, D3D_OK, 6);
        return;
    }
    VBVersion *ver = vb_cache(vb);
    if (ver && start + n > vb->count)
        ver = NULL;
    if (ver && !ver->d.pretransformed && !skip_untextured_2d(vb->fvf) &&
        render_draw_vb(&vb->gl, ver->gen, ver->v, vb->count, start, (int)prim, n, NULL, 0, ver->d.has_normal,
                       ver->d.has_color, ver->d.has_spec, ver->d.ntex)) {
        stdcall_ret(c, D3D_OK, 6);
        return;
    }
    if (ver)
        render_set_bounds(ver->lo, ver->hi);
    draw_vb = ver;
    draw_vb_start = start;
    draw(prim, vb->fvf, g_mem + vb->mem + start * vb->stride, vb->stride, n, NULL, 0, ver ? ver->v + start : NULL,
         ver ? &ver->d : NULL);
    draw_vb = NULL;
    render_set_bounds(NULL, NULL);
    stdcall_ret(c, D3D_OK, 6);
}

static void dev_DrawIndexedPrimitiveVB(Ctx *c)
{
    uint32_t prim = ARG(1), start = ARG(3), nv = ARG(4), idx = ARG(5), ni = ARG(6);
    VertexBuffer *vb = com_host(ARG(2));
    const uint16_t *ip = idx ? (const uint16_t *)gptr(idx) : NULL;
    if (g_pack_thread) {
        PROF_SECTION(PROF_H_INDEX);
        pk_draw_vb(vb, prim, start, nv, ip, ni);
        PROF_SECTION_END;
        stdcall_ret(c, D3D_OK, 8);
        return;
    }
    VBVersion *ver = vb_cache(vb);
    if (ver && start + nv > vb->count)
        ver = NULL;
    if (ver && !ver->d.pretransformed && ip && ni &&
        render_draw_vb(&vb->gl, ver->gen, ver->v, vb->count, start, (int)prim, nv, ip, ni, ver->d.has_normal,
                       ver->d.has_color, ver->d.has_spec, ver->d.ntex)) {
        stdcall_ret(c, D3D_OK, 8);
        return;
    }
    /* A pre-transformed buffer filled for this frame and drawn again over the
     * same range (the terrain: one draw per texture batch): packed once. A
     * buffer whose previous contents were drawn that way takes this path from
     * its first draw. */
    if (!ver && ip && ni && vb->lock_id && (vb->fvf & 0x0Eu) == 0x4u && start + nv <= vb->count &&
        !skip_untextured_2d(vb->fvf)) {
        int first = vb->drawn_lock != vb->lock_id;
        int same = !first && vb->drawn_start == start && vb->drawn_nv == nv;
        if (first)
            vb->multi_drawn = vb->drawn_count >= 2u;
        vb->drawn_count = same ? vb->drawn_count + 1u : 1u;
        vb->drawn_lock = vb->lock_id;
        vb->drawn_start = start;
        vb->drawn_nv = nv;
        if (same || (first && vb->multi_drawn)) {
            GuestVerts g;
            guest_verts(vb->fvf, g_mem + vb->mem + start * vb->stride, vb->stride, NULL, &g);
            if (render_draw_dynvb(vb->lock_id, start, &g, nv, (int)prim, ip, ni)) {
                stdcall_ret(c, D3D_OK, 8);
                return;
            }
        }
    }
    if (ver)
        render_set_bounds(ver->lo, ver->hi);
    draw_vb = ver;
    draw_vb_start = start;
    draw(prim, vb->fvf, g_mem + vb->mem + start * vb->stride, vb->stride, nv, ip, ni, ver ? ver->v + start : NULL,
         ver ? &ver->d : NULL);
    draw_vb = NULL;
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
        vb_free_memory(vb);
        if (vb->cache)
            vb_version_retire(vb->cache);
        render_vb_release(&vb->gl);
        free(vb);
    }
    stdcall_ret(c, com_release(obj), 1);
}

static void vb_Lock(Ctx *c)
{
    VertexBuffer *vb = com_host(THIS);
    if (!(ARG(1) & 0x10u)) {   /* DDLOCK_READONLY */
        vb_data_detach(vb, ARG(1), 0);
        vb->gen++;
        vb->lock_frame = g_frame_serial;
        if (++vb_lock_serial == 0)
            vb_lock_serial = 1;
        vb->lock_id = vb_lock_serial;
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
