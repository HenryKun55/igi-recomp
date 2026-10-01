/* Hand-decompiled deformed object renderer of IGI.exe.
 *
 *   0x0049BE10  bend_draw   puts the vertices of a deformable object's mesh,
 *                           rotated or bent along its x axis (like 0x49CC80),
 *                           to view space and the screen (with outcodes) in a
 *                           global vertex array and passes the triangles that
 *                           need clipping to 0x4A0340
 *
 *   void bend_draw(Object *obj, DrawCtx *ctx);   (cdecl)
 *
 * The bounding sphere (the object's position plus an optional offset record at
 * obj+0x5A0 with its own radius, else the radius at obj+0x44) is tested first
 * (0x4D03D0); ctx->zmax (+8) is the far limit of the pass on entry and the
 * nearest depth drawn on exit. Only meshes of type 0 are drawn. The mesh's
 * vertices (0x20 bytes) are first deformed in object space into the global
 * array (rigid: x relative to the middle of the extent, rotated by the
 * object's matrix; bent: per segment, see native_deform.c), then transformed
 * in place to view space. Triangles are only clipped when the sphere reaches
 * in front of the near plane.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every x87
 * register a double, rounded to float wherever the game stores a float): the
 * same operations in the same order, so the results are bit-identical to the
 * recompiled code (tools/recomp/nativebench.py). Beyond memory and the returned
 * registers, the guest calls see the same registers and x87 status word, and
 * the frame the guest leaves below its entry esp (saved registers, call
 * arguments, last values of its locals) is reproduced, since later guest code
 * reads uninitialised locals: this function itself writes three of its stale
 * locals into the vertex array for vertices behind the near plane.
 *
 * Assumed (the guest data always satisfies it): the global vertex array does
 * not overlap the mesh, the object, the camera or the stack; no operation
 * meets two different NaNs (which one results depends on the compiler's
 * operand order, in the recompiled code too). */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_BEND_DRAW     0x0049BE10u
#define FN_SPHERE_TEST   0x004D03D0u   /* nonzero: the sphere is outside the view */
#define FN_CLIP_TRIANGLE 0x004A0340u   /* (Vert v0, Vert v1, Vert v2, float *zmin, float near) */

#define G_CAM_POS        0x00BCAB08u   /* double[3] */
#define G_CAM_ROT        0x00BCAAE0u   /* float[9], world to view rows */
#define G_FRAME          0x00BCAB04u
#define G_NEAR           0x00BCADC0u
#define G_VERTS          0x00723518u   /* transformed vertices (NV_VERT_SIZE bytes) */
#define C_HALF           0x00533504u   /* 0.5f */
#define C_ONE            0x005333E0u   /* 1.0f */
#define C_ZERO           0x005333ECu   /* 0.0f */

/* Object. */
#define OBJ_POS          0x20u         /* double[3] */
#define OBJ_RADIUS       0x44u
#define OBJ_MODEL        0x6Cu
#define OBJ_ROT          0x70u         /* float[9] rows */
#define OBJ_STAMP        0x94u
#define OBJ_X0           0xFCu
#define OBJ_X1           0x100u
#define OBJ_NSEG         0x59Cu
#define OBJ_SPHERE       0x5A0u        /* NULL or {float offset[3], radius} */
#define OBJ_RIGID        0x5B4u
#define OBJ_REVERSED     0x5B5u
/* Mesh (model->[0]->0xC->0xC) and its geometry. */
#define MESH_TYPE        0x20u
#define MESH_GEOM        0x24u
#define GEOM_NVERTS      0x0Cu
#define GEOM_VERTS       0x10u         /* 0x20 bytes each: position, normal, uv */
#define GEOM_BLOCKS      0x14u         /* triangle blocks */
#define SRC_SIZE         0x20u

/* Return addresses of the guest's calls. */
#define RET_SPHERE       0x0049BFC5u
#define RET_CLIP         0x0049C75Fu

/* The guest frame: S is esp after the prologue (entry esp - 0x84); offsets from S. */
#define FRAME_SIZE       0x84u
#define F_ZMIN           0x10u         /* float, passed by pointer to the clipper */
#define F_14             0x14u         /* radius; inverse extent, segment; X; block */
#define F_18             0x18u         /* high word of the segment; Y */
#define F_1C             0x1Cu         /* Z */
#define F_NEAR           0x20u
#define F_COUNT          0x24u         /* bent vertex count, then triangle index */
#define F_GEOM           0x28u
#define F_CENTER         0x2Cu         /* view-space sphere centre (passed to the test) */
#define F_SX             0x38u         /* screen x, y and 1/z of the last vertex in front */
#define F_P              0x44u         /* centre, rotated positions of the last vertex */
#define F_T              0x50u         /* half extent (rigid), then the translation to view space */
#define F_W              0x5Cu         /* rotation to view space */
#define F_STAMP          0x80u

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline float f32(double v) { return (float)v; }
static inline void stw(uint8_t *gm, uint32_t a, float v) { memcpy(gm + a, &v, 4); }

/* Rigid deformation: x relative to the middle of the extent, rotated. */
static void bend_rigid(uint8_t *gm, uint32_t S, uint32_t obj, uint32_t geom)
{
    double m[9];
    for (int k = 0; k < 9; k++)
        m[k] = ldf(gm, obj + OBJ_ROT + 4u * (uint32_t)k);
    float h = f32((ldf(gm, obj + OBJ_X1) + ldf(gm, obj + OBJ_X0)) * ldf(gm, C_HALF));
    stw(gm, S + F_T, h);
    uint32_t src = rt_ld32(gm + geom + GEOM_VERTS), dst = G_VERTS;
    float p0 = 0, p1 = 0, p2 = 0;
    int32_t i = 0;
    for (; i < (int32_t)rt_ld32(gm + geom + GEOM_NVERTS); i++, src += SRC_SIZE, dst += NV_VERT_SIZE) {
        double x = ldf(gm, src) - (double)h, y = ldf(gm, src + 4u), z = ldf(gm, src + 8u);
        p0 = f32((z * m[2] + x * m[0]) + y * m[1]);
        p1 = f32((z * m[5] + x * m[3]) + y * m[4]);
        p2 = f32((z * m[8] + x * m[6]) + y * m[7]);
        stw(gm, dst, p0);
        stw(gm, dst + 4u, p1);
        stw(gm, dst + 8u, p2);
    }
    if (i > 0) {
        stw(gm, S + F_P, p0);
        stw(gm, S + F_P + 4u, p1);
        stw(gm, S + F_P + 8u, p2);
    }
}

/* Bent deformation: segment k by x (rounded by the x87 control word), y and z
 * by segments k and k + 1, interpolated by the clamped fraction. */
static void bend_segments(Ctx *c, uint8_t *gm, uint32_t S, uint32_t obj, uint32_t geom)
{
    int rev = gm[obj + OBJ_REVERSED] != 0;
    float inv = f32(ldf(gm, C_ONE) / (ldf(gm, obj + OBJ_X1) - ldf(gm, obj + OBJ_X0)));
    stw(gm, S + F_14, inv);
    double nt = -ldf(gm, obj + (rev ? OBJ_X1 : OBJ_X0));
    double scale = (double)(int32_t)rt_ld32(gm + obj + OBJ_NSEG) * (double)inv;
    if (rev)
        scale = -scale;
    rt_st32(gm + S + F_COUNT, 0);
    const double zero = ldf(gm, C_ZERO), one = ldf(gm, C_ONE);
    uint32_t src = rt_ld32(gm + geom + GEOM_VERTS), dst = G_VERTS;
    float a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    int64_t q = 0;
    uint32_t k = 0;
    /* The segments' data converted once (the object does not change during the
     * call); segments out of that range are read per vertex. */
    static NativeSeg conv[NATIVE_SEG_CONV];
    uint32_t nconv = rt_ld32(gm + obj + OBJ_NSEG);
    nconv = nconv <= NATIVE_SEG_CONV ? nconv : 0;
    if ((int32_t)rt_ld32(gm + geom + GEOM_NVERTS) <= 0)
        nconv = 0;
    for (uint32_t j = 0; j < nconv; j++)
        native_seg_load(obj, j, &conv[j]);
    int32_t i = 0;
    for (; i < (int32_t)rt_ld32(gm + geom + GEOM_NVERTS); i++, src += SRC_SIZE, dst += NV_VERT_SIZE) {
        double f = (nt + ldf(gm, src)) * scale;
        uint32_t nseg = rt_ld32(gm + obj + OBJ_NSEG);
        q = rt_fist64(c, f);
        k = (uint32_t)q;
        if (k == nseg)
            k--;
        double frac = f - (double)(int32_t)k;
        if (X87_ABOVE(zero, frac))
            frac = zero;
        else if (!(one >= frac))
            frac = one;
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
        stw(gm, dst, f32((b0 - a0) * frac + a0));
        stw(gm, dst + 4u, f32(((double)b1 - (double)a1) * frac + (double)a1));
        stw(gm, dst + 8u, f32(((double)b2 - (double)a2) * frac + (double)a2));
    }
    if (i > 0) {
        rt_st32(gm + S + F_14, k);
        rt_st32(gm + S + F_18, (uint32_t)((uint64_t)q >> 32));
        rt_st32(gm + S + F_COUNT, (uint32_t)i);
        stw(gm, S + F_SX + 4u, a1);
        stw(gm, S + F_SX + 8u, a2);
        stw(gm, S + F_P + 4u, b1);
        stw(gm, S + F_P + 8u, b2);
    }
}

int rt_native_bend_draw(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_BEND_DRAW);
    uint8_t *const gm = g_mem;
    const uint32_t S = esp - FRAME_SIZE;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    const uint64_t done = (uint64_t)(esp + 4u) << 32;
    /* Prologue: saved registers and the near plane. */
    rt_st32(gm + S + 0x0Cu, ebx0);
    rt_st32(gm + S + 0x08u, ebp0);
    rt_st32(gm + S + 0x04u, esi0);
    rt_st32(gm + S + 0x00u, edi0);
    rt_st32(gm + S + F_NEAR, rt_ld32(gm + G_NEAR));
    const uint32_t obj = rt_ld32(gm + esp + 4u);
    uint32_t model = rt_ld32(gm + obj + OBJ_MODEL);
    if (!model) {
        c->r[1] = ecx;
        *ret = done;
        return 1;
    }
    const uint32_t mesh = rt_ld32(gm + rt_ld32(gm + rt_ld32(gm + model) + 0x0Cu) + 0x0Cu);
    const uint32_t sph = rt_ld32(gm + obj + OBJ_SPHERE);
    double d[3], R[9];
    for (uint32_t i = 0; i < 3; i++)
        d[i] = rt_ldf64(gm + obj + OBJ_POS + 8u * i) - rt_ldf64(gm + G_CAM_POS + 8u * i);
    for (uint32_t i = 0; i < 9; i++)
        R[i] = ldf(gm, G_CAM_ROT + 4u * i);
    /* Sphere centre in view space. */
    float ctr[3];
    uint32_t radius;
    if (sph) {
        double X = (double)f32((double)f32(d[0]) + ldf(gm, sph));
        double Y = (double)f32((double)f32(d[1]) + ldf(gm, sph + 4u));
        double Z = (double)f32(d[2]) + ldf(gm, sph + 8u);
        for (int r = 0; r < 3; r++)
            ctr[r] = f32((R[3 * r + 2] * Z + R[3 * r + 1] * Y) + R[3 * r] * X);
        radius = rt_ld32(gm + sph + 0x0Cu);
        eax = sph;
    } else {
        for (int r = 0; r < 3; r++)
            ctr[r] = f32((R[3 * r + 2] * d[2] + R[3 * r + 1] * d[1]) + R[3 * r] * d[0]);
        radius = rt_ld32(gm + obj + OBJ_RADIUS);
        eax = radius;
    }
    memcpy(gm + S + F_P, ctr, 12);
    memcpy(gm + S + F_CENTER, ctr, 12);
    rt_st32(gm + S + F_14, radius);

    /* sphere_test(&centre, radius, 0) */
    rt_st32(gm + S - 4u, 0);
    rt_st32(gm + S - 8u, radius);
    rt_st32(gm + S - 12u, S + F_CENTER);
    rt_st32(gm + S - 16u, RET_SPHERE);
    c->r[2] = S + F_CENTER;
    c->r[6] = obj;
    c->r[7] = mesh;
    eax = (uint32_t)f_004D03D0(c, S - 16u, radius, eax);
    ecx = c->r[1];
    uint32_t edx = c->r[2];
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);
    if (eax != 0)
        goto out;
    const uint32_t ctx = rt_ld32(gm + esp + 8u);
    double nearz = ldf(gm, S + F_CENTER + 8u) - ldf(gm, S + F_14);
    double zmax = ldf(gm, ctx + 8u);
    native_fcom(&fsw, nearz, zmax);
    ecx = ctx;
    if (X87_ABOVE(nearz, zmax)) {
        eax = rt_fstsw(native_fsw(&fsw), (c->ftop - 1u) & 7u) & 0xFFFFu;
        goto out;
    }
    eax = rt_ld32(gm + mesh + MESH_TYPE);
    if (eax != 0)
        goto out;

    /* Object-space deformation into the vertex array. */
    rt_st32(gm + S + F_ZMIN, rt_ld32(gm + ctx + 8u));
    const uint32_t geom = rt_ld32(gm + mesh + MESH_GEOM);
    rt_st32(gm + S + F_GEOM, geom);
    if (gm[obj + OBJ_RIGID])
        bend_rigid(gm, S, obj, geom);
    else
        bend_segments(c, gm, S, obj, geom);

    /* View-space translation and rotation. */
    float t[3], W[9];
    for (int r = 0; r < 3; r++)
        t[r] = f32((d[2] * R[3 * r + 2] + d[1] * R[3 * r + 1]) + d[0] * R[3 * r]);
    double O[9];
    for (uint32_t i = 0; i < 9; i++)
        O[i] = ldf(gm, obj + OBJ_ROT + 4u * i);
    for (int r = 0; r < 3; r++)
        for (int j = 0; j < 3; j++)
            W[3 * r + j] = f32((R[3 * r + 2] * O[6 + j] + R[3 * r] * O[j]) + R[3 * r + 1] * O[3 + j]);
    memcpy(gm + S + F_T, t, 12);
    memcpy(gm + S + F_W, W, 36);
    rt_st32(gm + S + F_STAMP, rt_ld32(gm + obj + OBJ_STAMP) + rt_ld32(gm + G_FRAME) + 1u);
    edx = geom;

    /* Vertices to view space and the screen. */
    NativeVerts lp;
    lp.zmin = rt_ld32(gm + S + F_ZMIN);
    for (uint32_t i = 0; i < 3; i++)
        lp.scr[i] = rt_ld32(gm + S + F_SX + 4u * i);
    uint32_t n = rt_ld32(gm + geom + GEOM_NVERTS);
    if ((int32_t)n > 0) {
        NativeView v;
        for (int i = 0; i < 9; i++)
            v.m[i] = W[i];
        for (int i = 0; i < 3; i++)
            v.t[i] = t[i];
        v.near_z = ldf(gm, S + F_NEAR);
        native_view_load(&v);
        native_view_transform(&v, G_VERTS, NV_VERT_SIZE, n, G_VERTS, NV_PROJ_SY_FIRST, &lp);
        memcpy(gm + S + F_P, lp.p, 12);
        memcpy(gm + S + F_14, lp.xyz, 12);
        memcpy(gm + S + F_SX, lp.scr, 12);
        rt_st32(gm + S + F_ZMIN, lp.zmin);
    }

    /* Triangles, when the sphere reaches in front of the near plane: those
     * crossing the screen or near plane with a vertex nearer than zmin go to
     * the clipper. */
    double nearp = ldf(gm, S + F_NEAR);
    native_fcom(&fsw, nearz, nearp);
    if (X87_BELOW(nearz, nearp)) {
        uint32_t blk = rt_ld32(gm + geom + GEOM_BLOCKS);
        rt_st32(gm + S + F_14, blk);
        while (blk) {
            uint32_t ntri = rt_ld16(gm + blk + 0x0Cu) / 3u;
            uint32_t idx = blk + 0x1Cu;
            rt_st32(gm + S + F_COUNT, 0);
            for (uint32_t j = 0; j < ntri; j++, idx += 6u) {
                uint32_t base = rt_ld16(gm + blk + 0x12u);
                uint32_t v0 = G_VERTS + (rt_ld16(gm + idx) + base) * NV_VERT_SIZE;
                uint32_t v1 = G_VERTS + (rt_ld16(gm + idx + 2u) + base) * NV_VERT_SIZE;
                uint32_t v2 = G_VERTS + (rt_ld16(gm + idx + 4u) + base) * NV_VERT_SIZE;
                uint32_t c0 = rt_ld32(gm + v0 + NV_VERT_CODE), c1 = rt_ld32(gm + v1 + NV_VERT_CODE),
                         c2 = rt_ld32(gm + v2 + NV_VERT_CODE);
                if ((c0 | c1 | c2) != 0 && (c2 & c0 & c1) == 0) {
                    double zl = ldf(gm, S + F_ZMIN);
                    double z0 = ldf(gm, v0 + 8u);
                    native_fcom(&fsw, z0, zl);
                    int clip = X87_BELOW(z0, zl);
                    if (!clip) {
                        double z1 = ldf(gm, v1 + 8u);
                        native_fcom(&fsw, z1, zl);
                        clip = X87_BELOW(z1, zl);
                        if (!clip) {
                            double z2 = ldf(gm, v2 + 8u);
                            native_fcom(&fsw, z2, zl);
                            clip = X87_BELOW(z2, zl);
                        }
                    }
                    if (clip) {
                        /* clip(v0, v1, v2, &zmin, near): vertices by value. */
                        uint32_t sp = S - 0x60u;
                        rt_st32(gm + S - 4u, rt_ld32(gm + S + F_NEAR));
                        rt_st32(gm + S - 8u, S + F_ZMIN);
                        memmove(gm + S - 0x24u, gm + v2, NV_VERT_SIZE);
                        memmove(gm + S - 0x40u, gm + v1, NV_VERT_SIZE);
                        memmove(gm + S - 0x5Cu, gm + v0, NV_VERT_SIZE);
                        rt_st32(gm + sp, RET_CLIP);
                        c->r[2] = v0;
                        c->r[3] = idx;
                        c->r[5] = v1;
                        c->r[6] = v0 + NV_VERT_SIZE;
                        c->r[7] = S - 0x40u;
                        c->fsw = native_fsw(&fsw);
                        f_004A0340(c, sp, 0, S + F_ZMIN);
                        native_fsw_sync(&fsw, c);
                    }
                }
                rt_st32(gm + S + F_COUNT, j + 1u);
            }
            edx = ntri;
            uint32_t next = rt_ld16(gm + blk + 0x0Eu);
            if (next == 0xFFFFu)
                break;
            blk += next + 0x1Cu;
            rt_st32(gm + S + F_14, blk);
        }
    }
    eax = rt_ld32(gm + S + F_ZMIN);
    rt_st32(gm + ctx + 8u, eax);
out:
    c->fsw = native_fsw(&fsw);   /* the guest code stores its status word on return */
    c->r[1] = ecx;
    c->r[2] = edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    *ret = done | eax;
    return 1;
}
