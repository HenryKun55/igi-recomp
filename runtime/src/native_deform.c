/* Hand-decompiled deformed mesh transform of IGI.exe.
 *
 *   0x0049CC80  deform_mesh   writes the vertices of a mesh, rotated or bent
 *                             along its x axis, into the shared dynamic vertex
 *                             buffer and returns the index of the first one
 *
 *   int deform_mesh(Mesh *mesh, Object *obj);   (cdecl)
 *
 * Mesh: +0x0C vertex count, +0x10 vertices (0x20 bytes: position, normal, uv).
 * Object: +0x5B4 rigid flag, +0x5B5 reversed flag, +0xFC/+0x100 extent along x,
 * +0x59C segment count, +0x70 rotation (3x3 floats), +0x104 + 12 * k segment
 * origin, +0x200 + 40 * k segment rotation (3x3 floats, one spare).
 *
 * Rigid: positions with x relative to the middle of the extent, rotated; the
 * normal and uv are copied. Bent: each vertex falls into segment k by its x
 * (scaled to the segment count, rounded by the x87 control word, the last
 * segment's end taken into the last segment) with a fraction clamped to [0, 1];
 * its y and z are rotated and offset by segment k and by segment k + 1 and the
 * results interpolated by the fraction; the normal is rotated by segment k.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float where the game stores a float): the
 * same operations in the same order, so the results are bit-identical to the
 * recompiled code (tools/recomp/nativebench.py). The guest's locals, saved
 * registers, call arguments and the values it leaves in its argument slots
 * (the locked pointer, then the vertex buffer position; the inverse extent,
 * then the last segment) are reproduced, since later guest code reads
 * uninitialised locals. The segments' data is converted to double once per
 * call (the guest converts it per vertex, and consecutive vertices rarely
 * share a segment).
 *
 * Assumed (the guest data always satisfies it): the locked vertex buffer, a
 * separate allocation, overlaps neither the mesh, its vertices nor the
 * object; no operation meets two different NaNs (which one results depends on
 * the compiler's operand order, in the recompiled code too). Left to the
 * recompiled code: a call with values on the x87 stack (the bent path uses
 * seven registers). */
#include "native.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_DEFORM_MESH    0x0049CC80u

#define G_VB              0x00BCAD90u   /* IDirect3DVertexBuffer7 * */
#define G_VB_CAP          0x00BCAD94u   /* vertices */
#define G_VB_USED         0x00BCAD98u   /* vertices written this frame */
#define C_HALF            0x00533504u   /* 0.5f */
#define C_ONE             0x005333E0u   /* 1.0f */
#define C_ZERO            0x005333ECu   /* 0.0f */

#define D3DVB_LOCK        0x0Cu
#define D3DVB_UNLOCK      0x10u
#define LOCK_APPEND       0x1020u       /* DDLOCK_NOOVERWRITE | DDLOCK_WAIT */
#define LOCK_DISCARD      0x2020u       /* DDLOCK_DISCARDCONTENTS | DDLOCK_WAIT */

#define MESH_NVERTS       0x0Cu
#define MESH_VERTS        0x10u
#define VERT_SIZE         0x20u

#define OBJ_ROT           0x70u
#define OBJ_X0            0xFCu
#define OBJ_X1            0x100u
#define OBJ_NSEG          0x59Cu
#define OBJ_RIGID         0x5B4u
#define OBJ_REVERSED      0x5B5u

#define RET_LOCK_APPEND   0x0049CCB5u
#define RET_LOCK_DISCARD  0x0049CCEEu
#define RET_UNLOCK        0x0049D095u

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline float f32(double v) { return (float)v; }

static inline void stw(uint8_t *gm, uint32_t a, float v)
{
    memcpy(gm + a, &v, 4);
}

/* Rigid path. Writes the last position to the locals; returns the last vertex's
 * normal y word (the guest's final ebx), or ebx when there is no vertex. */
static uint32_t deform_rigid(uint8_t *gm, uint32_t mesh, uint32_t src, uint32_t *pptr, uint32_t E, uint32_t obj,
                             uint32_t ebx)
{
    double m[9];
    for (int k = 0; k < 9; k++)
        m[k] = ldf(gm, obj + OBJ_ROT + 4u * (uint32_t)k);
    float h = f32((ldf(gm, obj + OBJ_X1) + ldf(gm, obj + OBJ_X0)) * ldf(gm, C_HALF));
    stw(gm, E - 0x0Cu, h);
    uint32_t ptr = *pptr;
    float px = 0, py = 0, pz = 0;
    int any = 0;
    for (int32_t i = 0; i < (int32_t)rt_ld32(gm + mesh + MESH_NVERTS); i++, src += VERT_SIZE, ptr += VERT_SIZE) {
        double x = ldf(gm, src) - (double)h, y = ldf(gm, src + 4u), z = ldf(gm, src + 8u);
        px = f32((z * m[2] + y * m[1]) + x * m[0]);
        py = f32((x * m[3] + z * m[5]) + y * m[4]);
        pz = f32((z * m[8] + y * m[7]) + x * m[6]);
        stw(gm, ptr, px);
        stw(gm, ptr + 4u, py);
        stw(gm, ptr + 8u, pz);
        ebx = rt_ld32(gm + src + 0x10u);
        memcpy(gm + ptr + 0x0Cu, gm + src + 0x0Cu, 20);
        any = 1;
    }
    if (any) {
        stw(gm, E - 0x18u, px);
        stw(gm, E - 0x14u, py);
        stw(gm, E - 0x10u, pz);
    }
    *pptr = ptr;
    return ebx;
}

/* Bent path. Returns the guest's final ebx (3 * last segment + 0x45, or ebx). */
static uint32_t deform_bent(Ctx *c, uint8_t *gm, uint32_t mesh, uint32_t src, uint32_t *pptr, uint32_t E,
                            uint32_t obj, uint32_t ebx, NativeFsw *fsw)
{
    int rev = gm[obj + OBJ_REVERSED] != 0;
    float inv = f32(ldf(gm, C_ONE) / (ldf(gm, obj + OBJ_X1) - ldf(gm, obj + OBJ_X0)));
    stw(gm, E + 8u, inv);
    double nt = -ldf(gm, obj + (rev ? OBJ_X1 : OBJ_X0));
    double scale = (double)(int32_t)rt_ld32(gm + obj + OBJ_NSEG) * (double)inv;
    if (rev)
        scale = -scale;
    rt_st32(gm + E - 0x30u, 0);
    uint32_t ptr = *pptr;
    const double zero = ldf(gm, C_ZERO), one = ldf(gm, C_ONE);
    float a1 = 0, a2 = 0, b1 = 0, b2 = 0, n0 = 0, n1 = 0, n2 = 0;
    int64_t q = 0;
    int32_t i;
    /* The segments' data converted once (the object does not change during the
     * call); segments out of that range are read per vertex. */
    static NativeSeg conv[NATIVE_SEG_CONV];
    uint32_t nconv = rt_ld32(gm + obj + OBJ_NSEG);
    nconv = nconv <= NATIVE_SEG_CONV ? nconv : 0;
    if ((int32_t)rt_ld32(gm + mesh + MESH_NVERTS) <= 0)
        nconv = 0;
    for (uint32_t k = 0; k < nconv; k++)
        native_seg_load(obj, k, &conv[k]);
    for (i = 0; i < (int32_t)rt_ld32(gm + mesh + MESH_NVERTS); i++, src += VERT_SIZE, ptr += VERT_SIZE) {
        double f = (nt + ldf(gm, src)) * scale;
        uint32_t nseg = rt_ld32(gm + obj + OBJ_NSEG);
        q = rt_fist64(c, f);
        uint32_t k = (uint32_t)q;
        if (k == nseg)
            k--;
        double frac = f - (double)(int32_t)k;
        native_fcom(fsw, zero, frac);
        if (X87_ABOVE(zero, frac)) {
            frac = zero;
        } else {
            native_fcom(fsw, one, frac);
            if (!(one >= frac))
                frac = one;
        }
        /* m[j]: the float at obj + 40 * k + 0x200 + 4 * j (segments k and k + 1),
         * t[j]: at obj + 12 * k + 0x104 + 4 * j. */
        NativeSeg sb;
        const NativeSeg *sg = &sb;
        if (k < nconv)
            sg = &conv[k];
        else
            native_seg_load(obj, k, &sb);
        const double *m = sg->m, *t = sg->t;
        double y = ldf(gm, src + 4u), z = ldf(gm, src + 8u);
        double a0 = m[2] * z + m[1] * y;
        a1 = f32(m[5] * z + m[4] * y);
        a2 = f32(m[8] * z + m[7] * y);
        a0 = a0 + t[0];
        a1 = f32((double)a1 + t[1]);
        a2 = f32((double)a2 + t[2]);
        double b0 = m[12] * z + m[11] * y;
        b1 = f32(m[15] * z + m[14] * y);
        b2 = f32(m[18] * z + m[17] * y);
        b0 = b0 + t[3];
        b1 = f32((double)b1 + t[4]);
        b2 = f32((double)b2 + t[5]);
        stw(gm, ptr, f32((b0 - a0) * frac + a0));
        stw(gm, ptr + 4u, f32(((double)b1 - (double)a1) * frac + (double)a1));
        stw(gm, ptr + 8u, f32(((double)b2 - (double)a2) * frac + (double)a2));
        double nx = ldf(gm, src + 0x0Cu), ny = ldf(gm, src + 0x10u), nz = ldf(gm, src + 0x14u);
        n0 = f32((m[2] * nz + m[1] * ny) + nx * m[0]);
        n1 = f32((m[4] * ny + m[3] * nx) + m[5] * nz);
        n2 = f32((m[7] * ny + m[6] * nx) + m[8] * nz);
        stw(gm, ptr + 0x0Cu, n0);
        stw(gm, ptr + 0x10u, n1);
        stw(gm, ptr + 0x14u, n2);
        memcpy(gm + ptr + 0x18u, gm + src + 0x18u, 8);
        rt_st32(gm + E + 8u, k);
        ebx = k * 3u + 0x45u;
    }
    if (i > 0) {
        rt_st32(gm + E - 0x30u, (uint32_t)i);
        memcpy(gm + E - 0x2Cu, &q, 8);
        stw(gm, E - 0x20u, a1);
        stw(gm, E - 0x1Cu, a2);
        stw(gm, E - 0x14u, b1);
        stw(gm, E - 0x10u, b2);
        stw(gm, E - 0x0Cu, n0);
        stw(gm, E - 0x08u, n1);
        stw(gm, E - 0x04u, n2);
    }
    *pptr = ptr;
    return ebx;
}

int rt_native_deform_mesh(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_DEFORM_MESH);
    (void)ecx;
    (void)eax;
    if (c->ftop != 0)
        return 0;
    uint8_t *const gm = g_mem;
    const uint32_t E = esp, ptr_slot = esp + 4u;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    const uint64_t done = (uint64_t)(esp + 4u) << 32;
    uint32_t mesh = rt_ld32(gm + ptr_slot);
    /* sub esp, 0x34; push ebp, esi, edi; Lock(vb, flags, &ptr_slot, 0). */
    rt_st32(gm + E - 0x38u, ebp0);
    rt_st32(gm + E - 0x3Cu, esi0);
    rt_st32(gm + E - 0x40u, edi0);
    int append = (int32_t)(rt_ld32(gm + G_VB_USED) + rt_ld32(gm + mesh + MESH_NVERTS)) <=
                 (int32_t)rt_ld32(gm + G_VB_CAP);
    uint32_t vb = rt_ld32(gm + G_VB), vt = rt_ld32(gm + vb);
    rt_st32(gm + E - 0x44u, 0);
    rt_st32(gm + E - 0x48u, ptr_slot);
    rt_st32(gm + E - 0x4Cu, append ? LOCK_APPEND : LOCK_DISCARD);
    rt_st32(gm + E - 0x50u, vb);
    c->r[2] = ptr_slot;
    c->r[5] = mesh;
    uint32_t hr = (uint32_t)native_call(c, rt_ld32(gm + vt + D3DVB_LOCK), E - 0x50u,
                                        append ? RET_LOCK_APPEND : RET_LOCK_DISCARD, vt, vb);
    uint32_t base, edi = edi0, edx;
    if (append) {
        base = rt_ld32(gm + G_VB_USED);
        edi = rt_ld32(gm + ptr_slot) + (base << 5);
        edx = rt_ld32(gm + mesh + MESH_NVERTS);
        ecx = base + edx;
        rt_st32(gm + E - 0x34u, base);
        rt_st32(gm + ptr_slot, edi);
        rt_st32(gm + G_VB_USED, ecx);
    } else {
        ecx = rt_ld32(gm + mesh + MESH_NVERTS);
        edx = c->r[2];
        base = 0;
        rt_st32(gm + E - 0x34u, 0);
        rt_st32(gm + G_VB_USED, ecx);
    }
    if (hr != 0 || rt_ld32(gm + ptr_slot) == 0) {
        c->r[1] = ecx;
        c->r[2] = edx;
        c->r[5] = ebp0;
        *ret = done | base;
        return 1;
    }

    uint32_t obj = rt_ld32(gm + E + 8u), src = rt_ld32(gm + mesh + MESH_VERTS);
    rt_st32(gm + E - 0x44u, ebx0);   /* push ebx */
    uint32_t ptr = rt_ld32(gm + ptr_slot), ebx;
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);
    /* edi: the final pointer after a vertex, else the vertex count (rigid) or
     * what it was (bent). */
    if (gm[obj + OBJ_RIGID]) {
        int32_t n = (int32_t)rt_ld32(gm + mesh + MESH_NVERTS);
        ebx = deform_rigid(gm, mesh, src, &ptr, E, obj, ebx0);
        edi = n > 0 ? ptr : (uint32_t)n;
    } else {
        int32_t n = (int32_t)rt_ld32(gm + mesh + MESH_NVERTS);
        ebx = deform_bent(c, gm, mesh, src, &ptr, E, obj, ebx0, &fsw);
        if (n > 0)
            edi = ptr;
    }
    rt_st32(gm + ptr_slot, ptr);

    /* Unlock(vb). */
    vb = rt_ld32(gm + G_VB);
    vt = rt_ld32(gm + vb);
    rt_st32(gm + E - 0x48u, vb);
    c->r[2] = obj;
    c->r[3] = ebx;
    c->r[6] = base;
    c->r[7] = edi;
    c->fsw = native_fsw(&fsw);
    native_call(c, rt_ld32(gm + vt + D3DVB_UNLOCK), E - 0x48u, RET_UNLOCK, vt, vb);
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    *ret = done | base;
    return 1;
}
