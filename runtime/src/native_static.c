/* Hand-decompiled static object renderer of IGI.exe.
 *
 *   0x0049DF60  static_draw  draws one static object (buildings, props): view
 *                            volume test, level of detail, world matrix, the
 *                            state groups, then per face group the fog range,
 *                            textures and one DrawIndexedPrimitiveVB, or a
 *                            depth-sorted queue entry for transparent groups
 *
 *   int static_draw(Object **ref);   (cdecl)
 *
 * Object: position (double[3] at +0x20), model (+0x6C: level-of-detail meshes),
 * rotation (float[3][3] at +0x70, rows), stamp (+0x94), scale (float[3] at
 * +0x98), lightmap sets per level of detail (+0xDC, 0x24 bytes apart), camera
 * mode (+0xEC: 0 draws with the near camera, left to the recompiled code).
 * Meshes of type 0 (face groups of 0x1C bytes plus indices) and type 3
 * (lightmapped, groups of 0x20 bytes) are drawn; groups are chained by their
 * size at +0xE (0xFFFF ends).
 *
 * The device calls (SetTransform, SetTexture, SetTextureStageState,
 * DrawIndexedPrimitiveVB) go straight to the shim's direct entries
 * (runtime/include/d3d_direct.h) in the guest's order, so the device ends in the
 * same state and records the same draws. The world matrix builder 0x4950F0
 * and the per-group fog helper 0x49CBE0 (the world position of the mesh's
 * sphere, computed once per object here) are done inline. The other guest
 * calls remain (view volume test, level of detail, texture swaps, state
 * setters, fog range 0x49B2D0, queue entries, the lightmap coordinates
 * 0x49EB50) and see the same registers, stack and arguments. The frame left below the entry esp is the guest's (saved
 * registers, locals, call arguments and return addresses).
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float where the game stores a float): the
 * same operations in the same order. Assumed (as the game uses it): the
 * object, its model and meshes do not change during the call (the guest
 * calls do not write them), and the guest calls leave the x87 stack as they
 * found it. */
#include "native.h"
#include "d3d_direct.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_STATIC_DRAW  0x0049DF60u
#define FN_OBJECT_CULL  0x004D0510u
#define FN_LOD          0x004CFCB0u
#define FN_TEX_SWAP     0x004D0DE0u   /* (lodmesh): the object's texture swaps */
#define FN_TEX_RESTORE  0x004D0E20u
#define FN_GROUP_FOG    0x0049CBE0u   /* (obj, mesh): fog range of the mesh's sphere (inline) */
#define FN_FOG_RANGE    0x0049B2D0u
#define RET_FOG_IN_GROUP 0x0049CC6Fu
#define FN_SWAP_SIZE    0x004D0E50u
#define FN_SWAP_SAVE    0x004D0E60u
#define FN_QUEUE        0x004B72E0u   /* (callback, size, depth): depth-sorted queue entry */
#define FN_DRAW_MODE    0x004CFCA0u
#define FN_LM_COORDS    0x0049EB50u
#define CB_PLAIN        0x0049EC70u   /* queue callbacks */
#define CB_LIGHTMAP     0x0049F1C0u

#define G_DEVICE        0x005CA114u
#define G_CAM_POS       0x00BCAB08u   /* double[3] */
#define G_VIEW_AXIS     0x00BCAAF8u   /* float[3] */
#define G_FRAME         0x00BCAB04u
#define G_UNIT          0x00BCABECu
#define G_CHILD_FN      0x00A96AE0u   /* fn[mode * 0x180 + type](obj, 0) */

/* State groups: table pointers and wanted indices. */
#define G_TBL1          0x00BCADE8u
#define G_TBL2          0x00BCAD7Cu
#define G_TBL3          0x00BCADACu
#define G_TBL4          0x00BCADC4u
#define G_IDX1_PLAIN    0x007205B0u
#define G_IDX1_LM       0x00723504u
#define G_IDX2          0x007AC09Cu
#define G_IDX3_PLAIN    0x007234B8u
#define G_IDX3_LM       0x007ADDD4u
#define G_IDX4_IN       0x00BCADE0u   /* the object is inside the view volume */
#define G_IDX4_OUT      0x00BCACECu

#define FRAME_SIZE      0x238u        /* S = entry esp - 0x238 after the four pushes */

/* Locals (offsets from S). */
#define L_LOD           0x10u         /* level of detail, then the face list or lightmap set or base */
#define L_BASE          0x14u
#define L_LODMESH       0x18u
#define L_CENTRE        0x1Cu         /* float[3]: a transparent group's centre */
#define L_SCALE_Y       0x20u
#define L_SCALE_Z       0x24u
#define L_MESH          0x28u
#define L_GROUP         0x2Cu
#define L_INSIDE        0x30u         /* 0x4D0510's result flag */
#define L_POS           0x34u         /* float[3]: scaled camera-relative position */
#define L_ROT           0x40u         /* float[10]: scaled rotation (and the stamp) */
#define L_DEPTH         0x70u
#define L_AXIS          0x8Cu         /* float[3]: view axis in object space */
#define L_STAMP         0x98u
#define L_TDEPTH        0xA4u
#define L_WORLD         0x150u        /* float[16] */

typedef struct Regs {
    Ctx *c;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
} Regs;

/* A guest call with the arguments already below esp. */
static inline void gcall(Regs *r, uint32_t fn, uint32_t esp, uint32_t ret_addr, const GuestFunc **ic)
{
    Ctx *c = r->c;
    c->r[2] = r->edx;
    c->r[3] = r->ebx;
    c->r[5] = r->ebp;
    c->r[6] = r->esi;
    c->r[7] = r->edi;
    uint64_t rv = native_call_ic(c, fn, esp, ret_addr, r->ecx, r->eax, ic);
    r->eax = (uint32_t)rv;
    r->ecx = c->r[1];
    r->edx = c->r[2];
    r->ebx = c->r[3];
    r->ebp = c->r[5];
    r->esi = c->r[6];
    r->edi = c->r[7];
}

static inline uint32_t ld(const uint8_t *gm, uint32_t a) { return rt_ld32(gm + a); }
static inline void st(uint8_t *gm, uint32_t a, uint32_t v) { rt_st32(gm + a, v); }
static inline uint32_t ld16(const uint8_t *gm, uint32_t a) { return rt_ld16(gm + a); }
static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline void stf(uint8_t *gm, uint32_t a, double v) { rt_stf32(gm + a, v); }

/* What a device call leaves below sp: its arguments (the device last pushed)
 * and the return address; it returns D3D_OK and keeps the other registers. */
static inline void dev_frame(Regs *r, uint8_t *gm, uint32_t sp, const uint32_t *args, int n, uint32_t ret_addr)
{
    for (int i = n - 1; i >= 0; i--) {
        sp -= 4u;
        st(gm, sp, args[i]);
    }
    st(gm, sp - 4u, ret_addr);
    r->eax = D3DD_OK;
}

/* A state group's setter, run when the wanted index differs from the current
 * one (which the caller has stored): push 0; call [table + 4 * index + 0x14],
 * with the registers the guest loads at each site set by the caller. */
static inline void group_call(Regs *r, uint8_t *gm, uint32_t S, uint32_t tbl, uint32_t idx, uint32_t ret_addr,
                              const GuestFunc **ic)
{
    st(gm, S - 4u, 0u);
    gcall(r, ld(gm, tbl + idx * 4u + 0x14u), S - 4u, ret_addr, ic);
}

/* The mesh's sphere centre in world space as 0x49CBE0 computes it (bits):
 * rotation rows times the centre (y, z rounded to float) plus the position,
 * and the radius. The same for every group of the object. */
typedef struct Centre {
    uint32_t y, z, radius;
    uint64_t x, yd, zd;
} Centre;

static void centre_load(const uint8_t *gm, uint32_t obj, uint32_t mesh, Centre *k)
{
    const uint32_t m = obj + 0x70u;
    const double cx = ldf(gm, mesh + 0x30u), cy = ldf(gm, mesh + 0x34u), cz = ldf(gm, mesh + 0x38u);
    const double x = (ldf(gm, m + 8u) * cz + ldf(gm, m + 4u) * cy) + ldf(gm, m) * cx;
    const float y = (float)((ldf(gm, m + 0x14u) * cz + ldf(gm, m + 0x10u) * cy) + ldf(gm, m + 0x0Cu) * cx);
    const float z = (float)((ldf(gm, m + 0x20u) * cz + ldf(gm, m + 0x1Cu) * cy) + ldf(gm, m + 0x18u) * cx);
    const double xd = x + rt_ldf64(gm + obj + 0x20u);
    const double yd = (double)y + rt_ldf64(gm + obj + 0x28u);
    const double zd = (double)z + rt_ldf64(gm + obj + 0x30u);
    memcpy(&k->y, &y, 4);
    memcpy(&k->z, &z, 4);
    memcpy(&k->x, &xd, 8);
    memcpy(&k->yd, &yd, 8);
    memcpy(&k->zd, &zd, 8);
    k->radius = ld(gm, mesh + 0x3Cu);
}

static void draw_plain(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k);
static void draw_lightmap(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k);
static void draw_lm_none(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k);

/* The world matrix (0x49E0B1..0x49E1D1): camera-relative position and the
 * rotation rows times the scale, both times the unit, built by 0x4950F0
 * (inline) and set as WORLD. */
static void world_matrix(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj)
{
    st(gm, S - 4u, S + L_POS);
    stf(gm, S + L_POS, rt_ldf64(gm + obj + 0x20u) - rt_ldf64(gm + G_CAM_POS));
    stf(gm, S + L_POS + 4u, rt_ldf64(gm + obj + 0x28u) - rt_ldf64(gm + G_CAM_POS + 8u));
    const double dz = rt_ldf64(gm + obj + 0x30u) - rt_ldf64(gm + G_CAM_POS + 16u);
    const double k = ldf(gm, G_UNIT);
    stf(gm, S + L_POS, k * ldf(gm, S + L_POS));
    stf(gm, S + L_POS + 4u, k * ldf(gm, S + L_POS + 4u));
    stf(gm, S + L_POS + 8u, k * dz);
    for (uint32_t i = 0; i < 10u; i++)
        st(gm, S + L_ROT + 4u * i, ld(gm, obj + 0x70u + 4u * i));
    const double ksx = k * ldf(gm, obj + 0x98u);
    stf(gm, S + L_SCALE_Y, k * ldf(gm, obj + 0x9Cu));
    st(gm, S - 8u, S + L_ROT);
    st(gm, S - 12u, S + L_WORLD);
    stf(gm, S + L_SCALE_Z, k * ldf(gm, obj + 0xA0u));
    const double ky = ldf(gm, S + L_SCALE_Y), kz = ldf(gm, S + L_SCALE_Z);
    for (uint32_t row = 0; row < 3u; row++) {
        const uint32_t p = S + L_ROT + 12u * row;
        stf(gm, p, ldf(gm, p) * ksx);
        stf(gm, p + 4u, ldf(gm, p + 4u) * ky);
        stf(gm, p + 8u, ldf(gm, p + 8u) * kz);
    }
    /* 0x4950F0(dst, rot, pos): the transposed rotation and the position as a
     * row-major 4x4 matrix; it saves esi (obj + 0x98 after the copy). */
    st(gm, S - 16u, 0x0049E1B9u);
    st(gm, S - 20u, obj + 0x98u);
    const uint32_t dst = S + L_WORLD, rot = S + L_ROT, pos = S + L_POS;
    static const uint8_t from[12] = { 0, 3, 6, 0xFF, 1, 4, 7, 0xFF, 2, 5, 8, 0xFF };
    for (uint32_t i = 0; i < 12u; i++)
        st(gm, dst + 4u * i, from[i] == 0xFFu ? 0u : ld(gm, rot + 4u * from[i]));
    st(gm, dst + 0x30u, ld(gm, pos));
    st(gm, dst + 0x34u, ld(gm, pos + 4u));
    st(gm, dst + 0x38u, ld(gm, pos + 8u));
    st(gm, dst + 0x3Cu, 0x3F800000u);
    r->esi = obj + 0x98u;
    r->edi = S + L_ROT + 40u;
    const uint32_t dev = ld(gm, G_DEVICE);
    r->ecx = ld(gm, dev);
    r->edx = dst;
    dev_frame(r, gm, S, (const uint32_t[]){ dev, 1u, dst }, 3, 0x0049E1D1u);
    d3dd_set_transform(1u, dst);
}

int rt_native_static_draw(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_STATIC_DRAW);
    uint8_t *const gm = g_mem;
    const uint32_t E = esp, S = E - FRAME_SIZE;
    const uint32_t arg = ld(gm, E + 4u);
    const uint32_t obj = ld(gm, arg);
    const uint32_t model = ld(gm, obj + 0x6Cu);
    /* Left to the recompiled code: no model, the error report of an empty
     * model, the near camera, a device other than the shim's. */
    if (!model || ld16(gm, model + 0x3Cu) == 0u || ld(gm, obj + 0xECu) == 0u)
        return 0;
    const uint32_t dev0 = ld(gm, G_DEVICE);
    if (!dev0 || !g_d3dd_vtbl || ld(gm, dev0) != g_d3dd_vtbl)
        return 0;

    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    Regs r = { c, arg, ecx, c->r[2], obj, ebp0, model, edi0 };
    (void)eax;
    st(gm, E - 0x22Cu, ebx0);
    st(gm, E - 0x230u, esi0);
    {
        static const GuestFunc *ic = g_guest_func_hash;
        r.ecx = S + L_INSIDE;
        st(gm, E - 0x234u, r.ecx);
        st(gm, E - 0x238u, obj);
        gcall(&r, FN_OBJECT_CULL, E - 0x238u, 0x0049DF9Eu, &ic);
    }
    if (r.eax != 0u)
        goto out;
    {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, E - 0x234u, obj);
        gcall(&r, FN_LOD, E - 0x234u, 0x0049DFAFu, &ic);
    }
    r.ecx = ld(gm, obj + 0xECu);
    st(gm, S + L_LOD, r.eax);
    if (r.eax == 0xFFFFFFFFu)
        goto out;
    r.eax = ld(gm, model + r.eax * 4u);
    st(gm, S + L_LODMESH, r.eax);
    r.edx = ld(gm, r.eax + 0x0Cu);
    r.ecx = ld(gm, r.edx + 0x0Cu);
    st(gm, S + L_MESH, r.ecx);
    r.edx = ld(gm, r.ecx + 0x24u);
    if (r.edx == 0u)
        goto out;
    st(gm, E - 0x234u, r.ebp);
    st(gm, E - 0x238u, r.edi);
    {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, S - 4u, r.eax);
        gcall(&r, FN_TEX_SWAP, S - 4u, 0x0049DFF5u, &ic);
    }
    world_matrix(&r, gm, S, obj);

    /* The view axis in object space (for the queue depths) and the object's depth. */
    {
        const double a0 = ldf(gm, G_VIEW_AXIS), a1 = ldf(gm, G_VIEW_AXIS + 4u), a2 = ldf(gm, G_VIEW_AXIS + 8u);
        const uint32_t m = obj + 0x70u;
        r.ecx = ld(gm, G_FRAME);
        r.edx = ld(gm, obj + 0x94u) + r.ecx + 1u;
        st(gm, S + L_STAMP, r.edx);
        stf(gm, S + L_AXIS, (a2 * ldf(gm, m + 0x18u) + a1 * ldf(gm, m + 0x0Cu)) + a0 * ldf(gm, m));
        stf(gm, S + L_AXIS + 4u, (a2 * ldf(gm, m + 0x1Cu) + a1 * ldf(gm, m + 0x10u)) + a0 * ldf(gm, m + 4u));
        stf(gm, S + L_AXIS + 8u, (a2 * ldf(gm, m + 0x20u) + a1 * ldf(gm, m + 0x14u)) + a0 * ldf(gm, m + 8u));
        const double dx = rt_ldf64(gm + obj + 0x20u) - rt_ldf64(gm + G_CAM_POS);
        const double dy = rt_ldf64(gm + obj + 0x28u) - rt_ldf64(gm + G_CAM_POS + 8u);
        const double dz = rt_ldf64(gm + obj + 0x30u) - rt_ldf64(gm + G_CAM_POS + 16u);
        stf(gm, S + L_DEPTH, (dz * a2 + dy * a1) + dx * a0);
    }
    r.ebp = obj + 0x70u;
    Centre k;
    centre_load(gm, obj, ld(gm, S + L_MESH), &k);
    r.eax = ld(gm, ld(gm, S + L_MESH) + 0x20u);
    if (r.eax == 0u) {
        draw_plain(&r, gm, S, obj, &k);
    } else if (r.eax == 3u) {
        r.eax = ld(gm, obj + 0xDCu);
        if (r.eax) {
            r.edi = ld(gm, S + L_LOD) * 36u;
            r.eax = ld(gm, r.edi + r.eax);
            st(gm, S + L_LOD, r.eax);
        }
        if (r.eax)
            draw_lightmap(&r, gm, S, obj, &k);
        else
            draw_lm_none(&r, gm, S, obj, &k);
    }

    /* 0x49EB17: the texture swaps restored. */
    r.ebp = ebp0;
    r.edi = edi0;
    {
        static const GuestFunc *ic = g_guest_func_hash;
        r.eax = ld(gm, S + L_LODMESH);
        st(gm, E - 0x234u, r.eax);
        gcall(&r, FN_TEX_RESTORE, E - 0x234u, 0x0049EB3Du, &ic);
    }
out:
    c->r[1] = r.ecx;
    c->r[2] = r.edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    *ret = ((uint64_t)(E + 4u) << 32) | r.eax;
    return 1;
}

/* The fog range of the object's mesh before each opaque group: 0x49CBE0(obj,
 * mesh) inline, the fog range 0x49B2D0 of the mesh's sphere (centre in world
 * space, radius) called from its frame (F, its entry esp: y and z as floats at
 * F-0x20, F-0x1C, the centre as doubles at F-0x18, the radius and the centre's
 * address as the arguments). The caller has the guest's registers (the mesh
 * in eax or edx) set. */
static inline void group_fog(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k, uint32_t ret_addr)
{
    static const GuestFunc *ic = g_guest_func_hash;
    const uint32_t F = S - 12u;
    st(gm, S - 4u, ld(gm, S + L_MESH));
    st(gm, S - 8u, obj);
    st(gm, F, ret_addr);
    st(gm, F - 0x20u, k->y);
    st(gm, F - 0x1Cu, k->z);
    rt_st64(gm + F - 0x18u, k->x);
    rt_st64(gm + F - 0x10u, k->yd);
    rt_st64(gm + F - 0x08u, k->zd);
    st(gm, F - 0x28u, k->radius);
    st(gm, F - 0x2Cu, F - 0x18u);
    r->eax = k->radius;
    r->ecx = F - 0x18u;
    gcall(r, FN_FOG_RANGE, F - 0x2Cu, RET_FOG_IN_GROUP, &ic);
}

/* DrawIndexedPrimitiveVB of a face group (the guest's registers after it). */
static inline void group_draw(Regs *r, uint8_t *gm, uint32_t S, uint32_t sub, uint32_t vb_off, uint32_t ni,
                              uint32_t idx, uint32_t nv, uint32_t start, uint32_t ret_addr)
{
    const uint32_t dev = ld(gm, G_DEVICE);
    const uint32_t vb = ld(gm, ld(gm, sub + vb_off)), prim = ld(gm, sub);
    r->ecx = ld(gm, dev);
    r->edx = prim;
    dev_frame(r, gm, S, (const uint32_t[]){ dev, prim, vb, start, nv, idx, ni, 0u }, 8, ret_addr);
    d3dd_draw_indexed_vb(prim, vb, start, nv, idx, ni);
}

/* SetTexture(stage, tex). */
static inline void set_texture(Regs *r, uint8_t *gm, uint32_t S, uint32_t stage, uint32_t tex, uint32_t ret_addr)
{
    const uint32_t dev = ld(gm, G_DEVICE);
    dev_frame(r, gm, S, (const uint32_t[]){ dev, stage, tex }, 3, ret_addr);
    d3dd_set_texture(stage, tex);
}

/* SetTextureStageState(0, COLOROP, op). */
static inline void set_colorop(Regs *r, uint8_t *gm, uint32_t S, uint32_t op, uint32_t ret_addr)
{
    const uint32_t dev = ld(gm, G_DEVICE);
    dev_frame(r, gm, S, (const uint32_t[]){ dev, 0u, 1u, op }, 4, ret_addr);
    d3dd_set_tss(0u, 1u, op);
}

/* The queue entry's copy of the object (position, rotation, scale) and of
 * the draw's inputs (0x49E51C.., 0x49E7DF.., 0x49EA9A..). */
static inline void queue_copy(uint8_t *gm, uint32_t item, uint32_t obj)
{
    for (uint32_t i = 0; i < 6u; i++)
        st(gm, item + 4u * i, ld(gm, obj + 0x20u + 4u * i));
    for (uint32_t i = 0; i < 10u; i++)
        st(gm, item + 0x18u + 4u * i, ld(gm, obj + 0x70u + 4u * i));
    st(gm, item + 0x40u, ld(gm, obj + 0x98u));
    st(gm, item + 0x44u, ld(gm, obj + 0x9Cu));
    st(gm, item + 0x48u, ld(gm, obj + 0xA0u));
    st(gm, item + 0x4Cu, ld(gm, obj + 0xD8u));
}

/* Type 0 meshes (0x49E2B1): face groups of 0x1C bytes from sub + 0x14. */
static void draw_plain(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k)
{
    r->ecx = ld(gm, S + L_MESH);
    r->edi = ld(gm, r->ecx + 0x24u);
    r->ecx = ld(gm, G_IDX1_PLAIN);
    st(gm, S + L_LOD, r->edi);
    r->eax = (r->eax & 0xFFFF0000u) | ld16(gm, r->edi + 0x24u);
    r->esi = ld(gm, r->edi + 0x14u);
    st(gm, S + L_BASE, r->eax);
    r->eax = ld(gm, G_TBL1);
    st(gm, S + L_GROUP, r->esi);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->edx = ld(gm, G_TBL1);
        r->eax = ld(gm, G_IDX1_PLAIN);
        group_call(r, gm, S, r->edx, r->eax, 0x0049E2EFu, &ic);
    }
    r->eax = ld(gm, G_TBL2);
    r->ecx = ld(gm, G_IDX2);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->ecx = ld(gm, G_TBL2);
        r->edx = ld(gm, G_IDX2);
        group_call(r, gm, S, r->ecx, r->edx, 0x0049E317u, &ic);
    }
    r->eax = ld(gm, G_TBL3);
    r->ecx = ld(gm, G_IDX3_PLAIN);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->eax = ld(gm, G_TBL3);
        r->ecx = ld(gm, G_IDX3_PLAIN);
        group_call(r, gm, S, r->eax, r->ecx, 0x0049E33Eu, &ic);
    }
    r->edx = ld(gm, S + L_INSIDE);
    r->eax = ld(gm, G_IDX4_OUT);
    r->ecx = ld(gm, G_IDX4_IN);
    if (!r->edx)
        r->ecx = r->eax;
    r->ebp = ld(gm, G_TBL4);
    if (ld(gm, r->ebp + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        if (r->edx)
            r->eax = ld(gm, G_IDX4_IN);
        st(gm, r->ebp + 4u, r->eax);
        r->eax = ld(gm, S + L_INSIDE) ? ld(gm, G_IDX4_IN) : ld(gm, G_IDX4_OUT);
        r->edx = ld(gm, G_TBL4);
        group_call(r, gm, S, r->edx, r->eax, 0x0049E38Bu, &ic);
    }
    if (!r->esi)
        return;
    for (;;) {
        const uint32_t g = r->esi;
        if (ld16(gm, g + 0x16u) == 0u) {
            r->eax = ld(gm, S + L_MESH);
            group_fog(r, gm, S, obj, k, 0x0049E3ACu);
            const uint32_t tex = ld16(gm, g + 0x10u);
            r->eax = (r->eax & 0xFFFF0000u) | tex;
            const uint32_t ni = ld16(gm, g + 0x0Cu), nv = ld16(gm, g + 0x14u);
            const uint32_t base = ld(gm, S + L_BASE), start = ld16(gm, g + 0x12u) + base;
            if (tex != 0xFFFFu) {
                const uint32_t dev = ld(gm, G_DEVICE);
                r->ebp = ld(gm, ld(gm, S + L_LODMESH) + 0x10u);
                r->ecx = dev;
                r->edx = ld(gm, dev);
                const uint32_t t = ld(gm, ld(gm, r->ebp + tex * 4u) + 0x20u);
                set_texture(r, gm, S, 0u, t, 0x0049E3DEu);
                r->ebp = base;
                group_draw(r, gm, S, r->edi, 0x28u, ni, g + 0x1Cu, nv, start, 0x0049E416u);
            } else {
                set_colorop(r, gm, S, 3u, 0x0049E42Fu);
                set_texture(r, gm, S, 0u, 0u, 0x0049E441u);
                r->ebp = base;
                group_draw(r, gm, S, r->edi, 0x28u, ni, g + 0x1Cu, nv, start, 0x0049E479u);
                set_colorop(r, gm, S, 4u, 0x0049E48Du);
                r->ecx = ld(gm, ld(gm, G_DEVICE));
            }
        } else {
            /* Transparent: a queue entry drawn by 0x49EC70 at the group centre's depth. */
            const uint32_t gp = ld(gm, S + L_GROUP);
            st(gm, S + L_CENTRE, ld(gm, gp));
            r->eax = ld(gm, S + L_LODMESH);
            r->ecx = ld(gm, gp + 4u);
            st(gm, S - 4u, r->eax);
            st(gm, S + L_CENTRE + 4u, r->ecx);
            r->edx = ld(gm, gp + 8u);
            st(gm, S + L_CENTRE + 8u, r->edx);
            stf(gm, S + L_TDEPTH,
                (ldf(gm, S + L_CENTRE + 8u) * ldf(gm, S + L_AXIS + 8u) +
                 ldf(gm, S + L_CENTRE + 4u) * ldf(gm, S + L_AXIS + 4u)) +
                    ldf(gm, S + L_CENTRE) * ldf(gm, S + L_AXIS));
            {
                static const GuestFunc *ic = g_guest_func_hash;
                gcall(r, FN_SWAP_SIZE, S - 4u, 0x0049E4E0u, &ic);
            }
            stf(gm, S - 4u, ldf(gm, S + L_TDEPTH) + ldf(gm, S + L_DEPTH));
            r->esi = r->eax;
            r->ecx = r->esi + 0x68u;
            st(gm, S - 8u, r->ecx);
            st(gm, S - 12u, CB_PLAIN);
            {
                static const GuestFunc *ic = g_guest_func_hash;
                gcall(r, FN_QUEUE, S - 12u, 0x0049E4FEu, &ic);
            }
            r->ebp = r->eax;
            r->eax = r->esi ? r->ebp + 0x68u : 0u;
            r->edx = ld(gm, S + L_LODMESH);
            st(gm, S - 4u, r->eax);
            st(gm, S - 8u, r->edx);
            st(gm, r->ebp + 0x60u, r->eax);
            {
                static const GuestFunc *ic = g_guest_func_hash;
                gcall(r, FN_SWAP_SAVE, S - 8u, 0x0049E51Cu, &ic);
            }
            const uint32_t item = r->ebp;
            queue_copy(gm, item, obj);
            st(gm, item + 0x50u, ld(gm, S + L_LODMESH));
            r->eax = ld(gm, S + L_LOD);
            r->ecx = ld(gm, S + L_GROUP);
            st(gm, item + 0x54u, r->eax);
            st(gm, item + 0x58u, r->ecx);
            r->edx = ld(gm, obj + 0xECu);
            r->esi = r->ecx;
            st(gm, item + 0x5Cu, r->edx);
            r->edi = r->eax;
        }
        /* 0x49E57D: the next group. */
        const uint32_t next = ld16(gm, r->esi + 0x0Eu);
        r->eax = (r->eax & 0xFFFF0000u) | next;
        if (next == 0xFFFFu)
            return;
        r->eax = next;
        r->esi = r->esi + next + 0x1Cu;
        st(gm, S + L_GROUP, r->esi);
    }
}

/* Transparent group of a type 3 mesh (0x49E790, 0x49EA4B): a queue entry
 * drawn by 0x49F1C0 at the group centre's depth. Returns the entry. */
static uint32_t queue_lightmap(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, uint32_t g, uint32_t lmset,
                               uint32_t ret_addr)
{
    static const GuestFunc *ic = g_guest_func_hash;
    const uint32_t x = ld(gm, g), y = ld(gm, g + 4u), z = ld(gm, g + 8u);
    st(gm, S + L_CENTRE, x);
    st(gm, S + L_CENTRE + 4u, y);
    st(gm, S + L_CENTRE + 8u, z);
    stf(gm, S - 4u,
        ((ldf(gm, S + L_CENTRE + 8u) * ldf(gm, S + L_AXIS + 8u) +
          ldf(gm, S + L_CENTRE + 4u) * ldf(gm, S + L_AXIS + 4u)) +
         ldf(gm, S + L_CENTRE) * ldf(gm, S + L_AXIS)) +
            ldf(gm, S + L_DEPTH));
    st(gm, S - 8u, 0x68u);
    st(gm, S - 12u, CB_LIGHTMAP);
    gcall(r, FN_QUEUE, S - 12u, ret_addr, &ic);
    const uint32_t item = r->eax;
    queue_copy(gm, item, obj);
    st(gm, item + 0x50u, ld(gm, S + L_LODMESH));
    st(gm, item + 0x54u, ld(gm, S + L_BASE));
    st(gm, item + 0x58u, g);
    st(gm, item + 0x5Cu, ld(gm, obj + 0xECu));
    st(gm, item + 0x60u, lmset);
    r->ecx = ld(gm, obj + 0xECu);
    return item;
}

/* Type 3 meshes with a lightmap set (0x49E5CD): face groups of 0x20 bytes
 * from sub + 0x1C, texture and lightmap. */
static void draw_lightmap(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k)
{
    r->esi = ld(gm, ld(gm, S + L_MESH) + 0x24u);
    st(gm, S + L_BASE, r->esi);
    r->eax = ld16(gm, r->esi + 0x2Cu);
    r->ebp = ld(gm, r->esi + 0x1Cu);
    st(gm, S + L_GROUP, r->eax);
    {
        static const GuestFunc *ic = g_guest_func_hash;
        gcall(r, FN_DRAW_MODE, S, 0x0049E5EAu, &ic);
    }
    {
        static const GuestFunc *ic = g_guest_func_hash;
        r->eax &= 0xFFu;
        r->edx = ld16(gm, obj + 0x1Cu);
        st(gm, S - 4u, 0u);
        r->ecx = r->eax * 0x180u + r->edx;
        st(gm, S - 8u, obj);
        r->eax = ld(gm, r->ecx * 4u + G_CHILD_FN);
        gcall(r, r->eax, S - 8u, 0x0049E609u, &ic);
    }
    {
        static const GuestFunc *ic = g_guest_func_hash;
        r->edi += ld(gm, obj + 0xDCu);
        st(gm, S - 12u, r->edi);
        st(gm, S - 16u, r->esi);
        gcall(r, FN_LM_COORDS, S - 16u, 0x0049E616u, &ic);
    }
    r->eax = ld(gm, G_TBL1);
    r->ecx = ld(gm, G_IDX1_LM);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->eax = ld(gm, G_TBL1);
        r->ecx = ld(gm, G_IDX1_LM);
        group_call(r, gm, S, r->eax, r->ecx, 0x0049E63Du, &ic);
    }
    r->eax = ld(gm, G_TBL2);
    r->ecx = ld(gm, G_IDX2);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->edx = ld(gm, G_TBL2);
        r->eax = ld(gm, G_IDX2);
        group_call(r, gm, S, r->edx, r->eax, 0x0049E664u, &ic);
    }
    r->eax = ld(gm, G_TBL3);
    r->ecx = ld(gm, G_IDX3_LM);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->ecx = ld(gm, G_TBL3);
        r->edx = ld(gm, G_IDX3_LM);
        group_call(r, gm, S, r->ecx, r->edx, 0x0049E68Cu, &ic);
    }
    r->edx = ld(gm, S + L_INSIDE);
    r->eax = ld(gm, G_IDX4_OUT);
    r->ecx = ld(gm, G_IDX4_IN);
    if (!r->edx)
        r->ecx = r->eax;
    r->edi = ld(gm, G_TBL4);
    if (ld(gm, r->edi + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        if (r->edx)
            r->eax = ld(gm, G_IDX4_IN);
        st(gm, r->edi + 4u, r->eax);
        r->eax = ld(gm, S + L_INSIDE) ? ld(gm, G_IDX4_IN) : ld(gm, G_IDX4_OUT);
        r->ecx = ld(gm, G_TBL4);
        group_call(r, gm, S, r->ecx, r->eax, 0x0049E6D9u, &ic);
    }
    for (;;) {
        const uint32_t g = r->ebp;
        if (ld16(gm, g + 0x10u) != 0xFFFFu) {
            if (ld16(gm, g + 0x18u) == 0u) {
                r->edx = ld(gm, S + L_MESH);
                group_fog(r, gm, S, obj, k, 0x0049E6FEu);
                const uint32_t lmi = ld16(gm, g + 0x12u), tex = ld16(gm, g + 0x10u);
                r->edi = ld(gm, S + L_LOD) + lmi * 44u + 0x3Cu;
                const uint32_t t = ld(gm, ld(gm, ld(gm, ld(gm, S + L_LODMESH) + 0x10u) + tex * 4u) + 0x20u);
                r->ecx = t;
                set_texture(r, gm, S, 0u, t, 0x0049E73Fu);
                const uint32_t lt = ld(gm, r->edi + 0x20u);
                set_texture(r, gm, S, 1u, lt, 0x0049E753u);
                r->edi = ld(gm, S + L_GROUP);
                group_draw(r, gm, S, r->esi, 0x30u, ld16(gm, g + 0x0Cu), g + 0x20u, ld16(gm, g + 0x16u),
                           ld16(gm, g + 0x14u) + r->edi, 0x0049E78Bu);
            } else {
                r->edx = ld(gm, g + 4u);
                r->eax = ld(gm, g + 8u);
                r->ecx = ld(gm, g);
                const uint32_t item = queue_lightmap(r, gm, S, obj, g, ld(gm, S + L_LOD), 0x0049E7DFu);
                r->eax = item;
                r->edx = ld(gm, S + L_LOD);
                r->esi = ld(gm, obj + 0x9Cu);
                r->edi = item + 0x40u;
            }
        }
        /* 0x49E842: the next group. */
        const uint32_t next = ld16(gm, g + 0x0Eu);
        r->eax = (r->eax & 0xFFFF0000u) | next;
        if (next == 0xFFFFu)
            return;
        r->esi = ld(gm, S + L_BASE);
        r->eax = next;
        r->ebp = next + g + 0x20u;
    }
}

/* Type 3 meshes without lightmaps (0x49E862): as type 3, one texture. */
static void draw_lm_none(Regs *r, uint8_t *gm, uint32_t S, uint32_t obj, const Centre *k)
{
    r->eax = ld(gm, S + L_MESH);
    st(gm, S - 4u, 0u);
    r->esi = ld(gm, r->eax + 0x24u);
    st(gm, S - 8u, r->esi);
    st(gm, S + L_BASE, r->esi);
    r->edi = ld16(gm, r->esi + 0x2Cu);
    r->ebp = ld(gm, r->esi + 0x1Cu);
    st(gm, S + L_LOD, r->edi);
    {
        static const GuestFunc *ic = g_guest_func_hash;
        gcall(r, FN_LM_COORDS, S - 8u, 0x0049E882u, &ic);
    }
    r->eax = ld(gm, G_TBL1);
    r->ecx = ld(gm, G_IDX1_PLAIN);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->ecx = ld(gm, G_TBL1);
        r->edx = ld(gm, G_IDX1_PLAIN);
        group_call(r, gm, S, r->ecx, r->edx, 0x0049E8AAu, &ic);
    }
    r->eax = ld(gm, G_TBL2);
    r->ecx = ld(gm, G_IDX2);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->eax = ld(gm, G_TBL2);
        r->ecx = ld(gm, G_IDX2);
        group_call(r, gm, S, r->eax, r->ecx, 0x0049E8D1u, &ic);
    }
    r->eax = ld(gm, G_TBL3);
    r->ecx = ld(gm, G_IDX3_PLAIN);
    if (ld(gm, r->eax + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        st(gm, r->eax + 4u, r->ecx);
        r->edx = ld(gm, G_TBL3);
        r->eax = ld(gm, G_IDX3_PLAIN);
        group_call(r, gm, S, r->edx, r->eax, 0x0049E8F8u, &ic);
    }
    r->eax = ld(gm, S + L_INSIDE);
    r->ecx = r->eax ? ld(gm, G_IDX4_IN) : ld(gm, G_IDX4_OUT);
    r->edx = ld(gm, G_TBL4);
    if (ld(gm, r->edx + 4u) != r->ecx) {
        static const GuestFunc *ic = g_guest_func_hash;
        r->eax = r->eax ? ld(gm, G_IDX4_IN) : ld(gm, G_IDX4_OUT);
        st(gm, r->edx + 4u, r->eax);
        r->eax = ld(gm, S + L_INSIDE) ? ld(gm, G_IDX4_IN) : ld(gm, G_IDX4_OUT);
        r->ecx = ld(gm, G_TBL4);
        group_call(r, gm, S, r->ecx, r->eax, 0x0049E949u, &ic);
    }
    if (!r->ebp)
        return;
    for (;;) {
        const uint32_t g = r->ebp;
        if (ld16(gm, g + 0x18u) == 0u) {
            r->edx = ld(gm, S + L_MESH);
            group_fog(r, gm, S, obj, k, 0x0049E96Au);
            const uint32_t tex = ld16(gm, g + 0x10u);
            r->eax = (r->eax & 0xFFFF0000u) | tex;
            const uint32_t ni = ld16(gm, g + 0x0Cu), nv = ld16(gm, g + 0x16u), first = ld16(gm, g + 0x14u);
            if (tex != 0xFFFFu) {
                const uint32_t dev = ld(gm, G_DEVICE);
                r->edi = ld(gm, ld(gm, S + L_LODMESH) + 0x10u);
                const uint32_t t = ld(gm, ld(gm, r->edi + tex * 4u) + 0x20u);
                r->ecx = dev;
                set_texture(r, gm, S, 0u, t, 0x0049E99Bu);
                r->edi = ld(gm, S + L_LOD);
                group_draw(r, gm, S, r->esi, 0x30u, ni, g + 0x20u, nv, first + r->edi, 0x0049E9D3u);
            } else {
                set_colorop(r, gm, S, 3u, 0x0049E9ECu);
                set_texture(r, gm, S, 0u, 0u, 0x0049E9FEu);
                group_draw(r, gm, S, r->esi, 0x30u, ni, g + 0x20u, nv, first + r->edi, 0x0049EA32u);
                set_colorop(r, gm, S, 4u, 0x0049EA46u);
                r->ecx = ld(gm, ld(gm, G_DEVICE));
            }
        } else {
            r->eax = ld(gm, g);
            r->ecx = ld(gm, g + 4u);
            r->edx = ld(gm, g + 8u);
            const uint32_t item = queue_lightmap(r, gm, S, obj, g, 0u, 0x0049EA9Au);
            r->eax = item;
            r->edi = ld(gm, S + L_LOD);
            r->edx = ld(gm, S + L_BASE);
            r->esi = r->edx;
        }
        /* 0x49EAFF: the next group. */
        const uint32_t next = ld16(gm, g + 0x0Eu);
        r->eax = (r->eax & 0xFFFF0000u) | next;
        if (next == 0xFFFFu)
            return;
        r->eax = next;
        r->ebp = next + g + 0x20u;
    }
}
