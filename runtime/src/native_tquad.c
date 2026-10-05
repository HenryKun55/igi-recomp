/* Hand-decompiled polygon draw of IGI.exe's depth-sorted transparent queue.
 *
 *   0x0049F1C0  tquad_draw  draws one queued polygon of an object (the chain-link
 *                           fences, ...): render state, fog range, world matrix,
 *                           textures and one DrawIndexedPrimitiveVB
 *
 *   void tquad_draw(Item *it);   (cdecl, called from the queue flush 0x4B7100)
 *
 * Item: position (double[3] at +0), rotation (float[3][3] at +0x18, rows), scale
 * (float[3] at +0x40), vertex buffer descriptor (+0x54), polygon (+0x58), mesh
 * (+0x50), camera override (+0x5C: 0 runs a near-plane variant), lightmap set
 * (+0x60: nonzero selects the lightmapped states). Polygon: flags (byte +0x1C),
 * alpha (byte +0x1F), texture and lightmap indices (+0x10, +0x12), index count
 * (+0xC), first and count of vertices (+0x14, +0x16), indices (+0x20).
 *
 * In order: four cached state groups (table pointer, wanted index: the group's
 * setter [table + index * 4 + 0x14](0) runs when [table + 4] differs, after
 * storing the index there), the fog range of the polygon's centre (0x49B2D0 with
 * the mesh's radius), LIGHTING/COLOROP by the lightmap set, alpha blending,
 * TEXTUREFACTOR from the polygon's alpha, the world matrix (camera relative
 * position and rotation, both scaled by the global unit, built by 0x4950F0 into
 * the frame), the textures and the draw. The variants the levels checked never
 * take (near-plane camera, polygon flag 1, no texture) are left to the
 * recompiled code, and so is a device other than the shim's.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every x87
 * register a double, rounded to float where the game stores a float): the same
 * operations in the same order. The device calls go straight to the shim's
 * direct entries (runtime/include/d3d_direct.h) in the guest's order, so the
 * device ends in the same state and records the same draw. Beyond memory and
 * the returned registers, the guest calls that remain (state setters, fog
 * range) see the same registers, stack and arguments, and the frame left below
 * the entry esp is the guest's (saved registers, locals, call arguments and
 * return addresses, the frame of 0x4950F0, which is done inline here). Assumed
 * (as the game uses it): the item and the polygon do not change during the
 * call, and the state setters leave the x87 stack as they found it. */
#include "native.h"
#include "d3d_direct.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_TQUAD        0x0049F1C0u
#define FN_FOG_RANGE    0x0049B2D0u
#define FN_MATRIX       0x004950F0u

#define G_DEVICE        0x005CA114u
#define G_CAM_POS       0x00BCAB08u   /* double[3] */
#define G_UNIT          0x00BCABECu   /* float */

/* State groups: table pointer and wanted index (by lightmap set or not). */
#define G_TBL1          0x00BCADE8u
#define G_TBL2          0x00BCAD7Cu
#define G_TBL3          0x00BCADACu
#define G_TBL4          0x00BCADC4u
#define G_IDX1_LM       0x00723504u
#define G_IDX1          0x007205B0u
#define G_IDX2          0x007205B8u
#define G_IDX3_LM       0x00723508u
#define G_IDX3          0x007205BCu
#define G_IDX4          0x00BCACECu

/* Return addresses of the guest's calls. */
#define RET_G1_LM       0x0049F20Du
#define RET_G2_LM       0x0049F234u
#define RET_G3_LM       0x0049F25Bu
#define RET_G4_LM       0x0049F287u
#define RET_G1          0x0049F2ADu
#define RET_G2          0x0049F2D4u
#define RET_G3          0x0049F2FCu
#define RET_G4          0x0049F323u
#define RET_FOG         0x0049F3ABu
#define RET_Y_LIGHT_LM  0x0049F423u
#define RET_Y_LIGHT     0x0049F439u
#define RET_Y_TSS       0x0049F44Du
#define RET_Y_BLEND     0x0049F45Cu
#define RET_Y_SRC       0x0049F46Bu
#define RET_Y_DST       0x0049F47Au
#define RET_TFACTOR     0x0049F493u
#define RET_MATRIX      0x0049F58Cu
#define RET_WORLD       0x0049F5A4u
#define RET_TEX0        0x0049F676u
#define RET_TEX1        0x0049F69Fu
#define RET_DRAW        0x0049F6DDu

#define FRAME_SIZE      0x200u        /* S = entry esp - 0x200 after the four pushes */

typedef struct Regs {
    Ctx *c;
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
} Regs;

/* A guest call with the arguments already below esp: returns esp after it. */
static inline uint32_t gcall(Regs *r, uint32_t fn, uint32_t esp, uint32_t ret_addr, const GuestFunc **ic)
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
    return (uint32_t)(rv >> 32);
}

static inline uint32_t ld(const uint8_t *gm, uint32_t a) { return rt_ld32(gm + a); }
static inline void st(uint8_t *gm, uint32_t a, uint32_t v) { rt_st32(gm + a, v); }

/* What a device call leaves below S: its arguments (pushed last first, the
 * device last) and its return address. The call itself goes to the shim's
 * direct entry; it returns D3D_OK in eax and keeps every other register. */
static inline void dev_frame(Regs *r, uint8_t *gm, uint32_t S, const uint32_t *args, int n, uint32_t ret_addr)
{
    uint32_t sp = S;
    for (int i = n - 1; i >= 0; i--) {
        sp -= 4u;
        st(gm, sp, args[i]);
    }
    st(gm, sp - 4u, ret_addr);
    r->eax = D3DD_OK;
}

int rt_native_tquad_draw(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_TQUAD);
    uint8_t *const gm = g_mem;
    const uint32_t E = esp, S = E - FRAME_SIZE;
    const uint32_t it = ld(gm, E + 4u);
    const uint32_t poly = ld(gm, it + 0x58u);
    /* Only the variant the game's levels use (checked against the recompiled code):
     * no near-plane camera, polygon flag 1 clear, a texture. */
    if (ld(gm, it + 0x5Cu) == 0u || (gm[poly + 0x1Cu] & 1u) || rt_ld16(gm + poly + 0x10u) == 0xFFFFu)
        return 0;
    /* The device calls go to the shim's direct entries (the device is not
     * replaced during the call: the state setters only set states). */
    const uint32_t dev0 = ld(gm, G_DEVICE);
    if (!dev0 || !g_d3dd_vtbl || ld(gm, dev0) != g_d3dd_vtbl)
        return 0;

    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    Regs r = { c, eax, ecx, c->r[2], it, 0, esi0, edi0 };
    /* Prologue: saved registers and the first locals. */
    st(gm, S + 0x0Cu, ebx0);
    st(gm, S + 0x08u, ebp0);
    st(gm, S + 0x04u, esi0);
    st(gm, S + 0x50u, ld(gm, it + 0x54u));
    r.ebp = ld(gm, it + 0x58u);
    st(gm, S + 0x00u, edi0);
    const uint32_t lm = ld(gm, it + 0x60u);
    r.eax = ld(gm, G_TBL1);

    /* Cached state groups. */
    if (lm) {
        static const GuestFunc *ic1 = g_guest_func_hash, *ic2 = g_guest_func_hash, *ic3 = g_guest_func_hash,
                               *ic4 = g_guest_func_hash;
        r.ecx = ld(gm, G_IDX1_LM);
        r.edx = ld(gm, r.eax + 4u);
        if (r.edx != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.ecx = ld(gm, G_TBL1);
            r.edx = ld(gm, G_IDX1_LM);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.ecx + r.edx * 4u + 0x14u), S - 4u, RET_G1_LM, &ic1);
        }
        r.eax = ld(gm, G_TBL2);
        r.ecx = ld(gm, G_IDX2);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.eax = ld(gm, G_TBL2);
            r.ecx = ld(gm, G_IDX2);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.eax + r.ecx * 4u + 0x14u), S - 4u, RET_G2_LM, &ic2);
        }
        r.eax = ld(gm, G_TBL3);
        r.ecx = ld(gm, G_IDX3_LM);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.edx = ld(gm, G_TBL3);
            r.eax = ld(gm, G_IDX3_LM);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.edx + r.eax * 4u + 0x14u), S - 4u, RET_G3_LM, &ic3);
        }
        r.eax = ld(gm, G_TBL4);
        r.ecx = ld(gm, G_IDX4);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.ecx = ld(gm, G_TBL4);
            r.edx = ld(gm, G_IDX4);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.ecx + r.edx * 4u + 0x14u), S - 4u, RET_G4_LM, &ic4);
        }
    } else {
        static const GuestFunc *ic1 = g_guest_func_hash, *ic2 = g_guest_func_hash, *ic3 = g_guest_func_hash,
                               *ic4 = g_guest_func_hash;
        r.ecx = ld(gm, G_IDX1);
        r.edx = ld(gm, r.eax + 4u);
        if (r.edx != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.eax = ld(gm, G_TBL1);
            r.ecx = ld(gm, G_IDX1);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.eax + r.ecx * 4u + 0x14u), S - 4u, RET_G1, &ic1);
        }
        r.eax = ld(gm, G_TBL2);
        r.ecx = ld(gm, G_IDX2);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.edx = ld(gm, G_TBL2);
            r.eax = ld(gm, G_IDX2);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.edx + r.eax * 4u + 0x14u), S - 4u, RET_G2, &ic2);
        }
        r.eax = ld(gm, G_TBL3);
        r.ecx = ld(gm, G_IDX3);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.ecx = ld(gm, G_TBL3);
            r.edx = ld(gm, G_IDX3);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.ecx + r.edx * 4u + 0x14u), S - 4u, RET_G3, &ic3);
        }
        r.eax = ld(gm, G_TBL4);
        r.ecx = ld(gm, G_IDX4);
        if (ld(gm, r.eax + 4u) != r.ecx) {
            st(gm, r.eax + 4u, r.ecx);
            r.eax = ld(gm, G_TBL4);
            r.ecx = ld(gm, G_IDX4);
            st(gm, S - 4u, 0u);
            gcall(&r, ld(gm, r.eax + r.ecx * 4u + 0x14u), S - 4u, RET_G4, &ic4);
        }
    }

    /* The polygon's centre: rotation rows times the mesh's centre, plus the position. */
    {
        static const GuestFunc *ic = g_guest_func_hash;
        r.ecx = ld(gm, it + 0x50u);
        r.esi = it + 0x18u;
        r.edx = ld(gm, r.ecx + 0x0Cu);
        r.eax = ld(gm, r.edx + 0x0Cu);
        const uint32_t m = r.eax;
        const double cx = rt_ldf32(gm + m + 0x30u), cy = rt_ldf32(gm + m + 0x34u), cz = rt_ldf32(gm + m + 0x38u);
        const double x = (rt_ldf32(gm + it + 0x20u) * cz + rt_ldf32(gm + it + 0x1Cu) * cy) +
                         rt_ldf32(gm + it + 0x18u) * cx;
        const double y = (rt_ldf32(gm + it + 0x2Cu) * cz + rt_ldf32(gm + it + 0x28u) * cy) +
                         rt_ldf32(gm + it + 0x24u) * cx;
        rt_stf32(gm + S + 0x14u, y);
        const double z = (rt_ldf32(gm + it + 0x38u) * cz + rt_ldf32(gm + it + 0x34u) * cy) +
                         rt_ldf32(gm + it + 0x30u) * cx;
        rt_stf32(gm + S + 0x18u, z);
        rt_stf64(gm + S + 0x58u, x + rt_ldf64(gm + it));
        rt_stf64(gm + S + 0x60u, rt_ldf32(gm + S + 0x14u) + rt_ldf64(gm + it + 8u));
        rt_stf64(gm + S + 0x68u, rt_ldf32(gm + S + 0x18u) + rt_ldf64(gm + it + 0x10u));
        r.eax = ld(gm, r.ecx + 0x0Cu);
        r.ecx = ld(gm, r.eax + 0x0Cu);
        r.eax = S + 0x58u;
        r.edx = ld(gm, r.ecx + 0x3Cu);
        st(gm, S - 4u, r.edx);
        st(gm, S - 8u, r.eax);
        gcall(&r, FN_FOG_RANGE, S - 8u, RET_FOG, &ic);
    }

    /* Lighting and colour operation (by the lightmap set) and blending. */
    const uint32_t flags = gm[r.ebp + 0x1Cu];
    r.eax = (r.eax & 0xFFFFFF00u) | flags;
    {
        r.eax = ld(gm, it + 0x60u);
        uint32_t dev = ld(gm, G_DEVICE);
        const int lmset = r.eax != 0u;
        r.eax = dev;
        r.edx = ld(gm, dev);
        uint32_t op;
        if (lmset) {
            dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x89u, 0u }, 3, RET_Y_LIGHT_LM);
            d3dd_set_render_state(0x89u, 0u);
            op = 2u;
        } else {
            dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x89u, 1u }, 3, RET_Y_LIGHT);
            d3dd_set_render_state(0x89u, 1u);
            op = 4u;
        }
        dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.ecx = ld(gm, dev);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0u, 1u, op }, 4, RET_Y_TSS);
        d3dd_set_tss(0u, 1u, op);
        dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.edx = ld(gm, dev);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x1Bu, 1u }, 3, RET_Y_BLEND);
        d3dd_set_render_state(0x1Bu, 1u);
        dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.ecx = ld(gm, dev);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x13u, 5u }, 3, RET_Y_SRC);
        d3dd_set_render_state(0x13u, 5u);
        dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.edx = ld(gm, dev);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x14u, 6u }, 3, RET_Y_DST);
        d3dd_set_render_state(0x14u, 6u);
    }

    /* TEXTUREFACTOR: the polygon's alpha (0 opaque .. 255) as 255 - a in the top byte. */
    {
        const uint32_t dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.edx = (uint32_t)(-(int32_t)((uint32_t)gm[r.ebp + 0x1Fu] + 1u)) << 24;
        r.ecx = ld(gm, dev);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0x3Cu, r.edx }, 3, RET_TFACTOR);
        d3dd_set_render_state(0x3Cu, r.edx);
    }

    /* World matrix: camera-relative position and the rotation, scaled by the unit. */
    {
        const double a = rt_ldf64(gm + it) - rt_ldf64(gm + G_CAM_POS);
        rt_stf32(gm + S + 0x20u, rt_ldf64(gm + it + 8u) - rt_ldf64(gm + G_CAM_POS + 8u));
        rt_stf32(gm + S + 0x24u, rt_ldf64(gm + it + 0x10u) - rt_ldf64(gm + G_CAM_POS + 0x10u));
        const double k = rt_ldf32(gm + G_UNIT);
        const double ka = k * a;
        for (uint32_t i = 0; i < 10u; i++)
            st(gm, S + 0x28u + 4u * i, ld(gm, it + 0x18u + 4u * i));
        r.esi = it + 0x40u;
        r.edi = S + 0x50u;
        r.ecx = 0u;
        rt_stf32(gm + S + 0x1Cu, ka);
        rt_stf32(gm + S + 0x20u, k * rt_ldf32(gm + S + 0x20u));
        const double kc = k * rt_ldf32(gm + S + 0x24u);
        r.eax = S + 0x1Cu;
        r.ecx = S + 0x28u;
        st(gm, S - 4u, r.eax);
        r.edx = S + 0x118u;
        st(gm, S - 8u, r.ecx);
        st(gm, S - 12u, r.edx);
        rt_stf32(gm + S + 0x24u, kc);
        const double q0 = k * rt_ldf32(gm + it + 0x40u);
        rt_stf32(gm + S + 0x14u, k * rt_ldf32(gm + it + 0x44u));
        rt_stf32(gm + S + 0x18u, k * rt_ldf32(gm + it + 0x48u));
        const double q1 = rt_ldf32(gm + S + 0x14u), q2 = rt_ldf32(gm + S + 0x18u);
        for (uint32_t row = 0; row < 3u; row++) {
            const uint32_t p = S + 0x28u + 12u * row;
            rt_stf32(gm + p, rt_ldf32(gm + p) * q0);
            rt_stf32(gm + p + 4u, rt_ldf32(gm + p + 4u) * q1);
            rt_stf32(gm + p + 8u, rt_ldf32(gm + p + 8u) * q2);
        }
        /* 0x4950F0(dst = S + 0x118, rot = S + 0x28, pos = S + 0x1C), inline: the
         * transposed rotation and the position as a 4x4 row-major matrix. */
        st(gm, S - 16u, RET_MATRIX);
        st(gm, S - 20u, r.esi);
        const uint32_t dst = S + 0x118u, rot = S + 0x28u, pos = S + 0x1Cu;
        static const uint8_t from[12] = { 0, 3, 6, 0xFF, 1, 4, 7, 0xFF, 2, 5, 8, 0xFF };
        for (uint32_t i = 0; i < 12u; i++)
            st(gm, dst + 4u * i, from[i] == 0xFFu ? 0u : ld(gm, rot + 4u * from[i]));
        st(gm, dst + 0x30u, ld(gm, pos));
        r.edx = ld(gm, pos + 4u);
        st(gm, dst + 0x34u, r.edx);
        r.ecx = ld(gm, pos + 8u);
        st(gm, dst + 0x38u, r.ecx);
        st(gm, dst + 0x3Cu, 0x3F800000u);
        r.eax = dst;

        const uint32_t dev = ld(gm, G_DEVICE);
        r.eax = dev;
        r.ecx = ld(gm, dev);
        r.edx = S + 0x118u;
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 1u, r.edx }, 3, RET_WORLD);
        d3dd_set_transform(1u, r.edx);
    }

    /* Textures and the draw. */
    r.eax = ld(gm, it + 0x5Cu);
    const uint32_t tex = rt_ld16(gm + r.ebp + 0x10u);
    r.eax = (r.eax & 0xFFFF0000u) | tex;
    {
        r.esi = ld(gm, it + 0x50u);
        uint32_t dev = ld(gm, G_DEVICE);
        r.ecx = dev;
        r.eax = tex;
        r.esi = ld(gm, r.esi + 0x10u);
        r.edx = ld(gm, dev);
        r.eax = ld(gm, r.esi + tex * 4u);
        r.eax = ld(gm, r.eax + 0x20u);
        const uint32_t tex0 = r.eax;
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, 0u, tex0 }, 3, RET_TEX0);
        d3dd_set_texture(0u, tex0);
        r.ecx = ld(gm, it + 0x60u);
        if (r.ecx) {
            const uint32_t lmi = rt_ld16(gm + r.ebp + 0x12u);
            dev = ld(gm, G_DEVICE);
            r.edx = dev;
            r.esi = ld(gm, dev);
            r.edi = lmi * 5u;
            r.eax = lmi + r.edi * 2u;
            r.ecx = ld(gm, r.ecx + r.eax * 4u + 0x5Cu);
            dev_frame(&r, gm, S, (const uint32_t[]){ dev, 1u, r.ecx }, 3, RET_TEX1);
            d3dd_set_texture(1u, r.ecx);
        }
        const uint32_t ni = rt_ld16(gm + r.ebp + 0x0Cu), nv = rt_ld16(gm + r.ebp + 0x16u);
        const uint32_t first = rt_ld16(gm + r.ebp + 0x14u);
        const uint32_t vbd = ld(gm, S + 0x50u);
        dev = ld(gm, G_DEVICE);
        r.eax = dev;
        const uint32_t start = rt_ld16(gm + vbd + 0x2Cu) + first;
        r.edx = ld(gm, dev);
        r.edi = first;
        r.esi = ld(gm, ld(gm, vbd + 0x30u));
        r.ecx = ld(gm, vbd);
        dev_frame(&r, gm, S, (const uint32_t[]){ dev, r.ecx, r.esi, start, nv, r.ebp + 0x20u, ni, 0u }, 8, RET_DRAW);
        d3dd_draw_indexed_vb(r.ecx, r.esi, start, nv, r.ebp + 0x20u, ni);
    }

    /* Epilogue. */
    r.eax = ld(gm, it + 0x5Cu);
    c->r[1] = r.ecx;
    c->r[2] = r.edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    *ret = ((uint64_t)(E + 4u) << 32) | r.eax;
    return 1;
}
