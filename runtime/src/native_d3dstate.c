/* Hand-decompiled render state setters of IGI.exe.
 *
 * The object renderers select their device state through cached state groups:
 * a table (pointer at 0xBCADE8, 0xBCAD7C, 0xBCADAC, 0xBCADC4) whose current
 * index is at +4 and whose setters are at +0x14 + 4 * index. A setter runs
 * when the wanted index differs from the current one, which in the
 * depth-sorted transparent queue (lightmapped and plain polygons interleaved)
 * is a few hundred times a frame. Each setter is a fixed sequence of
 * SetRenderState / SetTextureStageState calls on the device (0x5CA114), some
 * with values derived from the level's fog and light settings:
 *
 *   void setter(void);   (cdecl; the callers push one ignored argument)
 *
 *   0x0049D730  tss_plain      texture stage states: one texture
 *   0x0049D960  tss_lightmap   texture stage states: texture and lightmap
 *   0x0049DB10  no_blend       ALPHABLENDENABLE off
 *   0x0049D810  rs_fog         fog colour and density, z, culling, no lighting
 *   0x0049DDF0  rs_fog_blend   the same with alpha blending
 *   0x0049D480  rs_lit         the level's directional lights, a white material,
 *                              ambient and lighting on, then as rs_fog
 *   0x0049DB20  rs_lit_blend   the same with alpha blending
 *   0x00493A60  lighting_on    LIGHTING on (error report if the call fails)
 *   0x00493AC0  lighting_off   LIGHTING off
 *
 * The native versions make the same device calls in the same order through
 * the shim's direct entries (shim/include/d3d_direct.h), so the device ends
 * in the same state, and leave what the guest leaves: the frame below the
 * entry esp (saved registers, locals, the last call's arguments and return
 * address; the earlier calls' words are overwritten by the later ones at the
 * same addresses), eax = D3D_OK, ecx and edx as the guest's last loads left
 * them. They decline when the device is not the shim's. Fog colour and
 * ambient follow the guest's x87 code (float products rounded by fistp in the
 * control word's mode, rt_fist64). */
#include "native.h"
#include "d3d_direct.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define G_DEVICE        0x005CA114u
#define G_FOG_DENSITY   0x00B46D04u   /* float, RS FOGDENSITY */
#define G_FOG_RGB       0x00B818ECu   /* float[3] */
#define G_AMBIENT_RGB   0x00B818ACu   /* float[3] */
#define C_SCALE_RG      0x005342D8u   /* float: fog red and green to 0..255 */
#define C_SCALE_B       0x005336D4u   /* float: fog blue and the ambient to 0..255 */
#define G_ZFUNC         0x005470B4u   /* RS ZFUNC value */
#define G_CULL          0x00BCAD78u   /* RS ZENABLE value */
#define G_UNIT          0x00BCABECu
/* The level's lights: count, index table, base and record size. */
#define G_LIGHT_BASE    0x00B041E0u
#define G_LIGHT_INDEX   0x00B041E4u
#define G_LIGHT_COUNT   0x00B041E8u
#define G_LIGHT_SIZE    0x00B041F0u

static inline uint32_t ld(const uint8_t *gm, uint32_t a) { return rt_ld32(gm + a); }
static inline void st(uint8_t *gm, uint32_t a, uint32_t v) { rt_st32(gm + a, v); }

/* The device object when it is the shim's (its vtable in *vt), else 0. */
static inline uint32_t shim_device(const uint8_t *gm, uint32_t *vt)
{
    const uint32_t dev = ld(gm, G_DEVICE);
    *vt = dev ? ld(gm, dev) : 0u;
    return *vt && *vt == g_d3dd_vtbl ? dev : 0u;
}

/* Return of a setter: eax = D3D_OK, ecx = edx = the vtable (unless noted). */
static inline int done(Ctx *c, uint64_t *ret, uint32_t E, uint32_t ecx, uint32_t edx)
{
    c->r[1] = ecx;
    c->r[2] = edx;
    *ret = ((uint64_t)(E + 4u) << 32) | D3DD_OK;
    return 1;
}

/* ---- texture stage states ---- */

typedef struct Tss {
    uint8_t stage, type, value;
} Tss;

static const Tss tss_plain[] = {
    { 0, 0x11, 2 }, { 0, 0x10, 2 }, { 0, 0x0C, 1 }, { 0, 0x12, 2 }, { 0, 0x0B, 0 }, { 0, 4, 2 },
    { 0, 5, 2 },    { 0, 1, 4 },    { 0, 2, 2 },    { 0, 3, 0 },    { 1, 1, 1 },
};

static const Tss tss_lightmap[] = {
    { 0, 0x11, 2 }, { 0, 0x10, 2 }, { 0, 0x0C, 1 }, { 0, 0x12, 2 }, { 0, 0x0B, 0 }, { 0, 4, 2 },
    { 0, 5, 2 },    { 0, 1, 2 },    { 0, 2, 2 },    { 1, 0x0C, 3 }, { 1, 0x12, 1 }, { 1, 0x0B, 1 },
    { 1, 5, 2 },    { 1, 6, 1 },    { 1, 2, 2 },    { 1, 3, 1 },    { 1, 1, 4 },    { 1, 4, 3 },
    { 1, 0x11, 2 }, { 1, 0x10, 2 }, { 2, 1, 1 },
};

/* Frame of the calls: value at E-4, type at E-8, stage at E-0xC, the device
 * at E-0x10, the return address at E-0x14. */
static int tss_setter(Ctx *c, uint32_t E, uint64_t *ret, const Tss *seq, unsigned n, uint32_t ret_last)
{
    uint8_t *const gm = g_mem;
    uint32_t vt;
    const uint32_t dev = shim_device(gm, &vt);
    if (!dev)
        return 0;
    for (unsigned i = 0; i < n; i++)
        d3dd_set_tss(seq[i].stage, seq[i].type, seq[i].value);
    const Tss *l = &seq[n - 1u];
    st(gm, E - 0x04u, l->value);
    st(gm, E - 0x08u, l->type);
    st(gm, E - 0x0Cu, l->stage);
    st(gm, E - 0x10u, dev);
    st(gm, E - 0x14u, ret_last);
    return done(c, ret, E, vt, vt);
}

int rt_native_tss_plain(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049D730u);
    (void)ecx;
    (void)eax;
    return tss_setter(c, esp, ret, tss_plain, sizeof(tss_plain) / sizeof(tss_plain[0]), 0x0049D80Cu);
}

int rt_native_tss_lightmap(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049D960u);
    (void)ecx;
    (void)eax;
    return tss_setter(c, esp, ret, tss_lightmap, sizeof(tss_lightmap) / sizeof(tss_lightmap[0]), 0x0049DB04u);
}

/* ---- single render states ---- */

/* Frame: value at E-4, state at E-8, the device at E-0xC, the return address
 * at E-0x10 (below the setter's own frame of `local` bytes). */
static int rs_one(Ctx *c, uint32_t E, uint64_t *ret, uint32_t local, uint32_t state, uint32_t value,
                  uint32_t ret_addr)
{
    uint8_t *const gm = g_mem;
    uint32_t vt;
    const uint32_t dev = shim_device(gm, &vt);
    if (!dev)
        return 0;
    d3dd_set_render_state(state, value);
    const uint32_t sp = E - local;
    st(gm, sp - 0x04u, value);
    st(gm, sp - 0x08u, state);
    st(gm, sp - 0x0Cu, dev);
    st(gm, sp - 0x10u, ret_addr);
    return done(c, ret, E, vt, c->r[2]);
}

int rt_native_no_blend(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049DB10u);
    (void)ecx;
    (void)eax;
    return rs_one(c, esp, ret, 0u, 0x1Bu, 0u, 0x0049DB1Fu);
}

/* sub esp, 0x400; push esi: the saved esi at E-0x404 (esi is 0 after a
 * successful call, so the error report is never reached). */
static int lighting(Ctx *c, uint32_t E, uint64_t *ret, uint32_t on, uint32_t ret_addr)
{
    if (!rs_one(c, E, ret, 0x404u, 0x88u, on, ret_addr))
        return 0;
    st(g_mem, E - 0x404u, c->r[6]);
    return 1;
}

int rt_native_lighting_on(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x00493A60u);
    (void)ecx;
    (void)eax;
    return lighting(c, esp, ret, 1u, 0x00493A79u);
}

int rt_native_lighting_off(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x00493AC0u);
    (void)ecx;
    (void)eax;
    return lighting(c, esp, ret, 0u, 0x00493AD9u);
}

/* ---- fog and lighting render states ---- */

/* Values of the render state lists besides constants. */
enum { V_FOG_COLOR = 0x100, V_FOG_DENSITY, V_ZFUNC, V_ZENABLE, V_AMBIENT };

typedef struct Rs {
    uint8_t state;
    uint16_t value;
} Rs;

/* The fog colour as the guest computes it (fistp of float products), the
 * last product (blue) left in the 8 bytes at t. */
static uint32_t fog_color(Ctx *c, uint8_t *gm, uint32_t t)
{
    const double rg = rt_ldf32(gm + C_SCALE_RG), b = rt_ldf32(gm + C_SCALE_B);
    const uint32_t r = (uint32_t)rt_fist64(c, rt_ldf32(gm + G_FOG_RGB) * rg);
    const uint32_t g = (uint32_t)rt_fist64(c, rt_ldf32(gm + G_FOG_RGB + 4u) * rg);
    const int64_t bl = rt_fist64(c, rt_ldf32(gm + G_FOG_RGB + 8u) * b);
    rt_st64(gm + t, (uint64_t)bl);
    return (uint32_t)bl - (((r << 8) + g) << 8);
}

/* The ambient colour (0x00RRGGBB), the blue product left at t. */
static uint32_t ambient(Ctx *c, uint8_t *gm, uint32_t t)
{
    const double k = rt_ldf32(gm + C_SCALE_B);
    const uint32_t r = (uint32_t)rt_fist64(c, rt_ldf32(gm + G_AMBIENT_RGB) * k);
    const uint32_t g = (uint32_t)rt_fist64(c, rt_ldf32(gm + G_AMBIENT_RGB + 4u) * k);
    const int64_t b = rt_fist64(c, rt_ldf32(gm + G_AMBIENT_RGB + 8u) * k);
    rt_st64(gm + t, (uint64_t)b);
    return (((r << 8) | g) << 8) | (uint32_t)b;
}

/* Sets the states of seq; the frame gets the last call's words below sp. */
static void rs_list(uint8_t *gm, uint32_t sp, uint32_t dev, const Rs *seq, unsigned n, uint32_t color, uint32_t amb,
                    uint32_t ret_last)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) {
        switch (seq[i].value) {
        case V_FOG_COLOR: v = color; break;
        case V_FOG_DENSITY: v = ld(gm, G_FOG_DENSITY); break;
        case V_ZFUNC: v = ld(gm, G_ZFUNC); break;
        case V_ZENABLE: v = ld(gm, G_CULL); break;
        case V_AMBIENT: v = amb; break;
        default: v = seq[i].value; break;
        }
        d3dd_set_render_state(seq[i].state, v);
    }
    st(gm, sp - 0x04u, v);
    st(gm, sp - 0x08u, seq[n - 1u].state);
    st(gm, sp - 0x0Cu, dev);
    st(gm, sp - 0x10u, ret_last);
}

#define RS_FOG_HEAD                                                                                          \
    { 0x22, V_FOG_COLOR }, { 0x26, V_FOG_DENSITY }, { 0x23, 0 }, { 0x8C, 3 }, { 0x1C, V_ZFUNC }, { 0x2F, 3 }, \
        { 0x1D, 0 }

static const Rs rs_fog[] = {
    RS_FOG_HEAD, { 0x16, 3 }, { 0x89, 0 }, { 0x8D, 0 }, { 0x1A, 1 }, { 0x0F, 0 }, { 7, V_ZENABLE }, { 0x0E, 1 },
    { 0x17, 4 },
};

static const Rs rs_fog_blend[] = {
    RS_FOG_HEAD, { 0x16, 3 }, { 0x89, 0 }, { 0x8D, 0 }, { 0x1A, 1 }, { 0x0F, 1 }, { 7, V_ZENABLE }, { 0x0E, 0 },
    { 0x17, 4 }, { 0x18, 1 }, { 0x19, 5 },
};

static const Rs rs_lit[] = {
    RS_FOG_HEAD, { 0x8B, V_AMBIENT }, { 0x16, 3 }, { 0x89, 1 }, { 0x8D, 0 }, { 0x1A, 1 }, { 0x0F, 0 },
    { 7, V_ZENABLE }, { 0x0E, 1 }, { 0x17, 4 },
};

static const Rs rs_lit_blend[] = {
    RS_FOG_HEAD, { 0x8B, V_AMBIENT }, { 0x16, 3 }, { 0x89, 1 }, { 0x8D, 0 }, { 0x1A, 1 }, { 0x0F, 1 },
    { 0x18, 1 }, { 0x19, 5 }, { 7, V_ZENABLE }, { 0x0E, 0 }, { 0x17, 4 },
};

/* sub esp, 0xC; push esi: the saved esi at E-0x10, the fog density at E-0xC,
 * the fistp temporary at E-8; the calls below E-0x10. */
static int fog_setter(Ctx *c, uint32_t E, uint64_t *ret, const Rs *seq, unsigned n, uint32_t ret_last)
{
    uint8_t *const gm = g_mem;
    uint32_t vt;
    const uint32_t dev = shim_device(gm, &vt);
    if (!dev)
        return 0;
    st(gm, E - 0x10u, c->r[6]);
    st(gm, E - 0x0Cu, ld(gm, G_FOG_DENSITY));
    const uint32_t color = fog_color(c, gm, E - 8u);
    rs_list(gm, E - 0x10u, dev, seq, n, color, 0u, ret_last);
    return done(c, ret, E, vt, vt);
}

int rt_native_rs_fog(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049D810u);
    (void)ecx;
    (void)eax;
    return fog_setter(c, esp, ret, rs_fog, sizeof(rs_fog) / sizeof(rs_fog[0]), 0x0049D959u);
}

int rt_native_rs_fog_blend(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049DDF0u);
    (void)ecx;
    (void)eax;
    return fog_setter(c, esp, ret, rs_fog_blend, sizeof(rs_fog_blend) / sizeof(rs_fog_blend[0]), 0x0049DF57u);
}

/* sub esp, 0xB8; push esi; push edi: the fistp temporary at E-0xB8, the fog
 * density at E-0xB0, the material (D3DMATERIAL7) at E-0xAC, the light
 * (D3DLIGHT7) at E-0x68, the saved esi and edi at E-0xBC, E-0xC0; the calls
 * below E-0xC0. Every light of the level is set as a directional light of its
 * colour times the unit, shining along its negated direction, and enabled. */
static int lit_setter(Ctx *c, uint32_t E, uint64_t *ret, const Rs *seq, unsigned n, uint32_t ret_last)
{
    uint8_t *const gm = g_mem;
    uint32_t vt;
    const uint32_t dev = shim_device(gm, &vt);
    if (!dev)
        return 0;
    const uint32_t mat = E - 0xACu, light = E - 0x68u;
    st(gm, E - 0xBCu, c->r[6]);
    st(gm, E - 0xC0u, c->r[7]);
    st(gm, E - 0xB0u, ld(gm, G_FOG_DENSITY));
    for (uint32_t i = 0; i < 26u; i++)
        st(gm, light + 4u * i, 0u);
    const int32_t count = (int32_t)ld(gm, G_LIGHT_COUNT);
    for (int32_t i = 0; i < count; i++) {
        const uint32_t l = ld(gm, ld(gm, G_LIGHT_INDEX) + 4u * (uint32_t)i) * ld(gm, G_LIGHT_SIZE) + ld(gm, G_LIGHT_BASE);
        const double k = rt_ldf32(gm + G_UNIT);
        st(gm, light, 3u);
        rt_stf32(gm + light + 4u, k * rt_ldf32(gm + l + 0x0Cu));
        rt_stf32(gm + light + 8u, k * rt_ldf32(gm + l + 0x10u));
        rt_stf32(gm + light + 12u, k * rt_ldf32(gm + l + 0x14u));
        rt_stf32(gm + light + 0x40u, -rt_ldf32(gm + l));
        rt_stf32(gm + light + 0x44u, -rt_ldf32(gm + l + 4u));
        rt_stf32(gm + light + 0x48u, -rt_ldf32(gm + l + 8u));
        d3dd_set_light((uint32_t)i, light);
        d3dd_light_enable((uint32_t)i, 1u);
    }
    for (uint32_t i = 0; i < 17u; i++)
        st(gm, mat + 4u * i, 0u);
    st(gm, mat, 0x3F800000u);
    st(gm, mat + 4u, 0x3F800000u);
    st(gm, mat + 8u, 0x3F800000u);
    st(gm, mat + 0x10u, 0x3F800000u);
    st(gm, mat + 0x14u, 0x3F800000u);
    st(gm, mat + 0x18u, 0x3F800000u);
    d3dd_set_material(mat);
    const uint32_t color = fog_color(c, gm, E - 0xB8u);
    /* The ambient is computed after the first seven states, which do not read
     * it: its fistp temporary is the fog colour's. */
    const uint32_t amb = ambient(c, gm, E - 0xB8u);
    rs_list(gm, E - 0xC0u, dev, seq, n, color, amb, ret_last);
    return done(c, ret, E, vt, vt);
}

int rt_native_rs_lit(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049D480u);
    (void)ecx;
    (void)eax;
    return lit_setter(c, esp, ret, rs_lit, sizeof(rs_lit) / sizeof(rs_lit[0]), 0x0049D727u);
}

int rt_native_rs_lit_blend(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x0049DB20u);
    (void)ecx;
    (void)eax;
    return lit_setter(c, esp, ret, rs_lit_blend, sizeof(rs_lit_blend) / sizeof(rs_lit_blend[0]), 0x0049DDE5u);
}
