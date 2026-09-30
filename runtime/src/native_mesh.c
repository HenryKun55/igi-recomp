/* Hand-decompiled object mesh renderer of IGI.exe.
 *
 *   0x004A1570  mesh_draw   transforms the vertices of one object's mesh to view
 *                           space and the screen (with outcodes) into a global
 *                           vertex array, passes the triangles that need clipping
 *                           to 0x4A0340 and then draws the object's children
 *
 *   void mesh_draw(Object *obj, DrawCtx *ctx);   (cdecl)
 *
 * The mesh's bounding sphere is tested first (0x4D03D0); ctx->zmax (+8) is the
 * far limit of the pass on entry and the nearest depth drawn on exit. Meshes of
 * type 0 (vertices of 0x20 bytes, the object's scale applied) and type 3
 * (vertices of 0x28 bytes, unscaled) are handled, other types only have their
 * children drawn.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every x87
 * register a double, rounded to float wherever the game stores a float): the
 * same operations in the same order, so the results are bit-identical to the
 * recompiled code (tools/recomp/nativebench.py). Beyond memory and the returned
 * registers, the guest calls see the same registers and x87 status word, and
 * the frame the guest leaves below its entry esp (saved registers, call
 * arguments, last values of its locals) is reproduced, since later guest code
 * reads uninitialised locals: this function itself copies three of its stale
 * locals into the vertex array for vertices behind the near plane.
 *
 * Assumed (the guest data always satisfies it): the global vertex arrays do not
 * overlap the meshes, the object or the camera. */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_MESH_DRAW     0x004A1570u
#define FN_SPHERE_TEST   0x004D03D0u   /* nonzero: the sphere is outside the view */
#define FN_CLIP_TRIANGLE 0x004A0340u   /* (Vert v0, Vert v1, Vert v2, float *zmin, float near) */
#define FN_DRAW_MODE     0x004D14D0u   /* returns the draw mode in al */

/* Camera and projection. */
#define G_CAM_POS        0x00BCAB08u   /* double[3] */
#define G_CAM_ROT        0x00BCAAE0u   /* float[9], world to view rows */
#define G_FRAME          0x00BCAB04u
#define G_PROJ_SX        0x00BCAB28u
#define G_PROJ_SY        0x00BCAB2Cu
#define G_PROJ_CX        0x00BCAB30u
#define G_PROJ_CY        0x00BCAB34u
#define G_CLIP_LEFT      0x00BCABD0u
#define G_CLIP_TOP       0x00BCABD4u
#define G_CLIP_RIGHT     0x00BCABD8u
#define G_CLIP_BOTTOM    0x00BCABDCu
#define G_NEAR           0x00BCADC0u
#define K_ONE            0x005333E0u   /* float constant, the numerator of 1/z */
/* Transformed vertices (Vert, 0x1C bytes: view x, y, z, screen x, y, 1/z, outcode). */
#define G_VERTS0         0x007AF1E8u   /* type 0 meshes */
#define G_VERTS3         0x008AD068u   /* type 3 meshes */
#define VERT_SIZE        0x1Cu
#define VERT_CODE        0x18u
/* Children: callbacks per draw mode and node type, and a global stack of nodes. */
#define G_CHILD_FN       0x00A96AE0u   /* fn[mode * 0x180 + type](node, ctx) */
#define G_NODE_STACK     0x00AFA6E0u
#define G_NODE_DEPTH     0x00AFA7E0u

/* Object. */
#define OBJ_POS          0x20u         /* double[3] */
#define OBJ_MODEL        0x6Cu
#define OBJ_ROT          0x70u         /* float[9] rows */
#define OBJ_STAMP        0x94u
#define OBJ_SCALE        0x98u         /* float[3] */
#define OBJ_CHILDREN     0xE8u
#define OBJ_VISIBLE      0xECu
/* Mesh (model->[0]->0xC->0xC). */
#define MESH_TYPE        0x20u
#define MESH_GEOM        0x24u
#define MESH_CENTER      0x30u         /* float[3] */
#define MESH_RADIUS      0x3Cu
/* Outcodes. */
#define OUT_LEFT         0x01u
#define OUT_TOP          0x02u
#define OUT_RIGHT        0x04u
#define OUT_BOTTOM       0x08u
#define OUT_NEAR         0x10u

/* Return addresses of the guest's calls. */
#define RET_SPHERE       0x004A175Au
#define RET_CLIP0        0x004A1D4Fu
#define RET_CLIP3        0x004A22C8u
#define RET_MODE         0x004A2375u
#define RET_CHILD        0x004A2397u

/* The guest frame: S is esp after the prologue (entry esp - 0xB0); offsets from S. */
#define FRAME_SIZE       0xB0u
#define F_ZMIN           0x10u         /* float, passed by pointer to the clipper */
#define F_RADIUS         0x14u         /* radius, then vertex index (type 0) or block */
#define F_18             0x18u         /* geometry (type 0) or vertex index (type 3), then triangle index */
#define F_X              0x1Cu         /* X, Y, Z of the last vertex */
#define F_P0             0x28u         /* view centre, then the last vertex's rotated position */
#define F_T              0x34u         /* translation to view space */
#define F_SX             0x40u         /* screen x, y and 1/z of the last vertex in front */
#define F_4C             0x4Cu         /* type 3: geometry, then c0 | c1 of the last triangle */
#define F_NEAR           0x50u
#define F_W              0x54u         /* type 0: scaled matrix (+ stamp), type 3: matrix */
#define F_STAMP3         0x78u
#define F_CENTER         0x7Cu         /* view-space sphere centre (passed to the test) */
#define F_V              0x88u         /* type 0: matrix before scaling, then the stamp */

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline uint32_t f2u(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }

/* Per-call constants of the vertex loops. */
typedef struct View {
    double m[9];                /* rows of the matrix applied to the vertices (float values) */
    double t[3];                /* translation (float values) */
    double near_z, one;
    double psx, psy, pcx, pcy;
    double left, top, right, bottom;
} View;

/* Result of a vertex loop: what the guest leaves in its locals and registers. */
typedef struct Loop {
    float p[3];                 /* F_P0: rotated position of the last vertex */
    float xyz[3];               /* F_X */
    uint32_t scr[3];            /* F_SX: screen x, y, 1/z of the last vertex in front (bits) */
    uint32_t zmin;              /* F_ZMIN (bits) */
    uint32_t end;               /* vertex pointer after the loop */
} Loop;

/* One vertex: outcode and screen position as both loops of the guest compute
 * them (type 0 multiplies y by (psy * rz), type 3 by (rz * y) * psy). */
static inline uint32_t project(const View *v, double x, double y, double zd, float z, int type3, uint32_t *scr)
{
    if (X87_BELOW(zd, v->near_z))
        return OUT_NEAR;
    double rz = v->one / (double)z;
    float rzf = (float)rz;
    double sx = rz * x * v->psx + v->pcx;
    double sy = type3 ? (double)rzf * y * v->psy + v->pcy : v->psy * rzf * y + v->pcy;
    float syf = (float)sy;
    scr[0] = f2u((float)sx);
    scr[1] = f2u(syf);
    scr[2] = f2u(rzf);
    return (uint32_t)X87_BELOW(sx, v->left) | (uint32_t)X87_BELOW(sy, v->top) << 1 |
           (uint32_t)X87_ABOVE(sx, v->right) << 2 | (uint32_t)X87_ABOVE((double)syf, v->bottom) << 3;
}

static void transform(const View *v, uint32_t src, uint32_t stride, uint32_t n, uint32_t dst, int type3, Loop *lp)
{
    uint8_t *const gm = g_mem;
    const double *m = v->m;
    uint32_t zmin = lp->zmin;
    float zminf;
    memcpy(&zminf, &zmin, 4);
    uint32_t scr[3] = { lp->scr[0], lp->scr[1], lp->scr[2] };
    float p0 = 0, p1 = 0, p2 = 0, x = 0, y = 0, z = 0;
    for (; n != 0; n--, src += stride, dst += VERT_SIZE) {
        double vx = ldf(gm, src), vy = ldf(gm, src + 4u), vz = ldf(gm, src + 8u);
        if (type3) {
            p0 = (float)((m[2] * vz + m[1] * vy) + m[0] * vx);
            p1 = (float)((m[5] * vz + m[4] * vy) + m[3] * vx);
            p2 = (float)((m[8] * vz + m[7] * vy) + m[6] * vx);
        } else {
            p0 = (float)((m[0] * vx + m[2] * vz) + m[1] * vy);
            p1 = (float)((m[3] * vx + m[5] * vz) + m[4] * vy);
            p2 = (float)((m[6] * vx + m[8] * vz) + m[7] * vy);
        }
        x = (float)((double)p0 + v->t[0]);
        y = (float)((double)p1 + v->t[1]);
        double zd = (double)p2 + v->t[2];
        z = (float)zd;
        uint32_t code = project(v, x, y, zd, z, type3, scr);
        uint8_t *d = gm + dst;
        memcpy(d, &x, 4);
        memcpy(d + 4, &y, 4);
        memcpy(d + 8, &z, 4);
        memcpy(d + 12, scr, 12);
        rt_st32(d + VERT_CODE, code);
        if (code == 0 && X87_BELOW(z, zminf))
            zminf = z;
    }
    lp->p[0] = p0;
    lp->p[1] = p1;
    lp->p[2] = p2;
    lp->xyz[0] = x;
    lp->xyz[1] = y;
    lp->xyz[2] = z;
    memcpy(lp->scr, scr, sizeof(scr));
    lp->zmin = f2u(zminf);
    lp->end = src;
}

/* State shared by the parts of one call. */
typedef struct Call {
    Ctx *c;
    uint32_t S;                 /* guest esp after the prologue */
    uint32_t obj, ctx;
    NativeFrame f;
    NativeFsw fsw;
    uint32_t ebp;               /* the guest's ebp (callee-saved, visible to callees) */
    uint32_t ecx, edx;          /* the guest's ecx and edx at the end of the geometry part */
} Call;

/* Triangles of the blocks from blk: those crossing the screen or near plane
 * with a vertex nearer than *zmin go to the clipper. */
static void triangles(Call *k, uint32_t blk, uint32_t verts, int type3)
{
    Ctx *c = k->c;
    const uint32_t S = k->S, zmin = S + F_ZMIN;
    const uint32_t hdr_base = type3 ? 0x14u : 0x12u, hdr_idx = type3 ? 0x20u : 0x1Cu;
    native_frame_put(&k->f, F_RADIUS, blk);
    for (;;) {
        uint32_t cnt = RD16(blk + 0x0Cu);
        uint32_t idx = blk + hdr_idx;
        uint32_t j = 0;
        k->ebp = idx;
        k->ecx = type3 ? cnt : 0;
        for (; (int32_t)j < (int32_t)(RD16(blk + 0x0Cu) / 3u); j++, idx += 6u) {
            uint32_t base = RD16(blk + hdr_base);
            uint32_t v0 = verts + (RD16(idx) + base) * VERT_SIZE;
            uint32_t v1 = verts + (RD16(idx + 2u) + base) * VERT_SIZE;
            uint32_t v2 = verts + (RD16(idx + 4u) + base) * VERT_SIZE;
            uint32_t c0 = RD32(v0 + VERT_CODE), c1 = RD32(v1 + VERT_CODE), c2 = RD32(v2 + VERT_CODE);
            if (type3) {
                native_frame_put(&k->f, 0x2Cu, v1);
                native_frame_put(&k->f, 0x30u, v2);
                native_frame_put(&k->f, F_4C, c0 | c1);
            } else {
                native_frame_put(&k->f, 0x38u, v1);
            }
            k->ecx = 0;
            k->ebp = idx + 6u;
            if ((c0 | c1 | c2) == 0 || (c2 & c0 & c1) != 0)
                continue;
            double zl = native_ldf(zmin);
            double z0 = native_ldf(v0 + 8u), z1, z2;
            native_fcom(&k->fsw, z0, zl);
            if (!X87_BELOW(z0, zl)) {
                z1 = native_ldf(v1 + 8u);
                native_fcom(&k->fsw, z1, zl);
                if (!X87_BELOW(z1, zl)) {
                    z2 = native_ldf(v2 + 8u);
                    native_fcom(&k->fsw, z2, zl);
                    if (!X87_BELOW(z2, zl))
                        continue;
                }
            }
            /* clip(v0, v1, v2, &zmin, near): vertices by value. */
            uint32_t sp = S - 0x60u;
            WR32(S - 4u, RD32(S + F_NEAR));
            WR32(S - 8u, zmin);
            memmove(g_mem + S - 0x24u, g_mem + v2, VERT_SIZE);
            memmove(g_mem + S - 0x40u, g_mem + v1, VERT_SIZE);
            memmove(g_mem + S - 0x5Cu, g_mem + v0, VERT_SIZE);
            WR32(sp, type3 ? RET_CLIP3 : RET_CLIP0);
            c->r[2] = v0;
            c->r[3] = k->obj;
            c->r[5] = idx;
            c->r[6] = v0 + VERT_SIZE;
            c->r[7] = S - 0x40u;
            c->fsw = native_fsw(&k->fsw);
            f_004A0340(c, sp, 0, zmin);
            native_fsw_sync(&k->fsw, c);
        }
        native_frame_put(&k->f, F_18, j);
        uint32_t next = RD16(blk + 0x0Eu);
        if (next == 0xFFFFu)
            break;
        blk += next + hdr_idx;
        native_frame_put(&k->f, F_RADIUS, blk);
    }
}

/* The mesh (after the visibility tests): vertices, then triangles to clip. */
static void geometry(Call *k, uint32_t mesh, double nearz, int type3)
{
    uint8_t *const gm = g_mem;
    const uint32_t obj = k->obj, S = k->S;
    NativeFrame *f = &k->f;
    /* Object position relative to the camera, rotated to view space. */
    double dx = rt_ldf64(gm + obj + OBJ_POS) - rt_ldf64(gm + G_CAM_POS);
    double dy = rt_ldf64(gm + obj + OBJ_POS + 8u) - rt_ldf64(gm + G_CAM_POS + 8u);
    double dz = rt_ldf64(gm + obj + OBJ_POS + 16u) - rt_ldf64(gm + G_CAM_POS + 16u);
    double R[9], O[9];
    for (int i = 0; i < 9; i++) {
        R[i] = ldf(gm, G_CAM_ROT + 4u * (uint32_t)i);
        O[i] = ldf(gm, obj + OBJ_ROT + 4u * (uint32_t)i);
    }
    View v;
    float t[3];
    t[0] = (float)((dz * R[2] + dy * R[1]) + dx * R[0]);
    t[1] = (float)((dz * R[5] + dy * R[4]) + dx * R[3]);
    t[2] = (float)((dz * R[8] + dy * R[7]) + dx * R[6]);
    for (int i = 0; i < 3; i++) {
        v.t[i] = t[i];
        native_frame_putf(f, F_T + 4u * (uint32_t)i, t[i]);
    }
    uint32_t geom = RD32(mesh + MESH_GEOM);
    uint32_t zmax = RD32(k->ctx + 8u);
    native_frame_put(f, F_ZMIN, zmax);
    /* Camera rotation times the object's rotation. */
    float V[9];
    for (int r = 0; r < 3; r++) {
        const double *Rr = &R[3 * r];
        V[3 * r] = (float)((Rr[1] * O[3] + Rr[0] * O[0]) + Rr[2] * O[6]);
        V[3 * r + 1] = (float)((Rr[1] * O[4] + Rr[2] * O[7]) + Rr[0] * O[1]);
        V[3 * r + 2] = (float)((Rr[1] * O[5] + Rr[2] * O[8]) + Rr[0] * O[2]);
    }
    uint32_t stamp = RD32(obj + OBJ_STAMP) + RD32(G_FRAME) + 1u;
    uint32_t src, stride, n, dst;
    Loop lp;
    if (!type3) {
        native_frame_put(f, F_18, geom);
        for (int i = 0; i < 9; i++)
            native_frame_putf(f, F_V + 4u * (uint32_t)i, V[i]);
        native_frame_put(f, F_V + 0x24u, stamp);
        native_frame_put(f, F_W + 0x24u, stamp);
        for (int i = 0; i < 9; i++) {
            float w = (float)((double)V[i] * ldf(gm, obj + OBJ_SCALE + 4u * (uint32_t)(i % 3)));
            native_frame_putf(f, F_W + 4u * (uint32_t)i, w);
            v.m[i] = w;
        }
        n = RD32(geom + 0x0Cu);
        src = RD32(geom + 0x10u);
        stride = 0x20u;
        dst = G_VERTS0;
        native_frame_put(f, F_RADIUS, 0);
    } else {
        native_frame_put(f, F_4C, geom);
        for (int i = 0; i < 9; i++) {
            native_frame_putf(f, F_W + 4u * (uint32_t)i, V[i]);
            v.m[i] = V[i];
        }
        native_frame_put(f, F_STAMP3, stamp);
        src = RD32(geom + 0x14u);
        n = RD32(geom + 0x10u);
        stride = 0x28u;
        dst = G_VERTS3;
        native_frame_put(f, F_18, 0);
    }
    k->ebp = src;
    k->ecx = n;
    lp.zmin = zmax;
    lp.end = src;
    /* Screen values of the last vertex in front: the guest's stale locals at first. */
    for (int i = 0; i < 3; i++)
        lp.scr[i] = RD32(S + F_SX + 4u * (uint32_t)i);
    if ((int32_t)n > 0) {
        v.near_z = native_ldf(S + F_NEAR);   /* F_NEAR holds G_NEAR */
        v.one = native_ldf(K_ONE);
        v.psx = native_ldf(G_PROJ_SX);
        v.psy = native_ldf(G_PROJ_SY);
        v.pcx = native_ldf(G_PROJ_CX);
        v.pcy = native_ldf(G_PROJ_CY);
        v.left = native_ldf(G_CLIP_LEFT);
        v.top = native_ldf(G_CLIP_TOP);
        v.right = native_ldf(G_CLIP_RIGHT);
        v.bottom = native_ldf(G_CLIP_BOTTOM);
        transform(&v, src, stride, n, dst, type3, &lp);
        k->ebp = lp.end;
        k->ecx = type3 ? n : geom;
        for (int i = 0; i < 3; i++) {
            native_frame_putf(f, F_P0 + 4u * (uint32_t)i, lp.p[i]);
            native_frame_putf(f, F_X + 4u * (uint32_t)i, lp.xyz[i]);
        }
        /* Written only when a vertex was in front, else the stale values stay. */
        for (int i = 0; i < 3; i++)
            if (lp.scr[i] != RD32(S + F_SX + 4u * (uint32_t)i))
                native_frame_put(f, F_SX + 4u * (uint32_t)i, lp.scr[i]);
        native_frame_put(f, F_ZMIN, lp.zmin);
        native_frame_put(f, type3 ? F_18 : F_RADIUS, n);
    }
    if (type3)
        k->ecx = k->ctx;

    /* Triangles, when the sphere reaches in front of the far limit. */
    WR32(S + F_ZMIN, lp.zmin);
    double zlim = native_ldf(k->ctx + 8u);
    native_fcom(&k->fsw, nearz, zlim);
    if (X87_BELOW(nearz, zlim)) {
        uint32_t blk = RD32(geom + (type3 ? 0x1Cu : 0x14u));
        native_frame_put(f, F_RADIUS, blk);
        if (blk)
            triangles(k, blk, dst, type3);
    }
    uint32_t z = RD32(S + F_ZMIN);
    native_frame_put(f, F_ZMIN, z);
    WR32(k->ctx + 8u, z);
    k->edx = z;
}

int rt_native_mesh_draw(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_MESH_DRAW);
    Call k;
    const uint32_t S = esp - FRAME_SIZE;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    k.c = c;
    k.S = S;
    k.f.base = S;
    k.f.set = 0;
    native_fsw_sync(&k.fsw, c);
    const uint32_t obj = RD32(esp + 4u), ctx = RD32(esp + 8u);
    k.obj = obj;
    k.ctx = ctx;
    uint32_t edx = c->r[2];
    const uint64_t done = (uint64_t)(esp + 4u) << 32;
    /* Prologue: saved registers and the near plane. */
    uint8_t *const gm = g_mem;
    uint8_t *const fr = gm + S;          /* the guest frame (written directly up to the first call) */
    rt_st32(fr + 0x0Cu, ebx0);
    rt_st32(fr + 0x08u, ebp0);
    rt_st32(fr + 0x04u, esi0);
    rt_st32(fr + 0x00u, edi0);
    rt_st32(fr + F_NEAR, rt_ld32(gm + G_NEAR));

    eax = RD32(obj + OBJ_VISIBLE);
    uint32_t model = RD32(obj + OBJ_MODEL);
    if (eax != 1u || model == 0 || RD32(model) == 0) {
        if (eax == 1u) {
            eax = model;
            if (model)
                ecx = 0;
        }
        c->r[1] = ecx;
        c->r[2] = edx;
        *ret = done | eax;
        return 1;
    }
    uint32_t mesh = RD32(RD32(RD32(model) + 0x0Cu) + 0x0Cu);
    /* Mesh centre in view space. */
    float d[3];
    for (uint32_t i = 0; i < 3; i++)
        d[i] = (float)(rt_ldf64(gm + obj + OBJ_POS + 8u * i) - rt_ldf64(gm + G_CAM_POS + 8u * i));
    double mc0 = ldf(gm, mesh + MESH_CENTER), mc1 = ldf(gm, mesh + MESH_CENTER + 4u),
           mc2 = ldf(gm, mesh + MESH_CENTER + 8u);
    double O[9];
    for (int i = 0; i < 9; i++)
        O[i] = ldf(gm, obj + OBJ_ROT + 4u * (uint32_t)i);
    double T0 = (mc2 * O[2] + mc1 * O[1]) + O[0] * mc0;
    float t1 = (float)((O[5] * mc2 + O[3] * mc0) + mc1 * O[4]);
    float t2 = (float)((mc1 * O[7] + O[6] * mc0) + mc2 * O[8]);
    double X = T0 + d[0], Y = (double)t1 + d[1], Z = (double)t2 + d[2];
    float ctr[3];
    for (int r = 0; r < 3; r++) {
        double r0 = ldf(gm, G_CAM_ROT + 12u * (uint32_t)r), r1 = ldf(gm, G_CAM_ROT + 12u * (uint32_t)r + 4u),
               r2 = ldf(gm, G_CAM_ROT + 12u * (uint32_t)r + 8u);
        ctr[r] = (float)((r2 * Z + r1 * Y) + r0 * X);
    }
    memcpy(fr + 0x38u, &t1, 4);
    memcpy(fr + 0x3Cu, &t2, 4);
    memcpy(fr + F_P0, ctr, 12);
    memcpy(fr + F_CENTER, ctr, 12);
    /* Radius: the largest scale (the guest's comparisons, NaN included) times the mesh's. */
    double sx = ldf(gm, obj + OBJ_SCALE), sy = ldf(gm, obj + OBJ_SCALE + 4u), sz = ldf(gm, obj + OBJ_SCALE + 8u);
    double a = X87_BELOW(sy, sx) ? sx : sy, scale;
    if (X87_ABOVE(a, sz)) {
        native_fcom(&k.fsw, sy, sx);
        scale = a;
    } else {
        native_fcom(&k.fsw, a, sz);
        scale = sz;
    }
    float radius = (float)(scale * ldf(gm, mesh + MESH_RADIUS));
    memcpy(fr + F_RADIUS, &radius, 4);

    /* sphere_test(&centre, radius, 0) */
    WR32(S - 4u, 0);
    WR32(S - 8u, f2u(radius));
    WR32(S - 12u, S + F_CENTER);
    WR32(S - 16u, RET_SPHERE);
    uint32_t sw = native_fsw(&k.fsw);
    c->r[2] = S + F_CENTER;
    c->r[3] = obj;
    c->r[6] = mesh;
    c->fsw = sw;
    uint64_t rv = f_004D03D0(c, S - 16u, f2u(radius), (f2u(ctr[2]) & 0xFFFF0000u) |
                             (rt_fstsw(sw, c->ftop) & 0xFFFFu));
    native_fsw_sync(&k.fsw, c);
    ecx = c->r[1];
    edx = c->r[2];
    eax = (uint32_t)rv;
    if (eax != 0) {
        c->r[3] = ebx0;
        c->r[6] = esi0;
        *ret = done | eax;
        return 1;
    }
    double nearz = ldf(gm, S + F_CENTER + 8u) - ldf(gm, S + F_RADIUS);
    double zmax = native_ldf(ctx + 8u);
    native_fcom(&k.fsw, nearz, zmax);
    k.ebp = ebp0;
    uint32_t type = RD32(mesh + MESH_TYPE);
    if (X87_ABOVE(nearz, zmax)) {
        eax = (ctx & 0xFFFF0000u) | (rt_fstsw(native_fsw(&k.fsw), (c->ftop - 1u) & 7u) & 0xFFFFu);
    } else if (type == 0 || type == 3) {
        geometry(&k, mesh, nearz, type == 3);
        eax = ctx;
        ecx = k.ecx;
        edx = k.edx;
    } else {
        eax = type;
    }
    native_frame_flush(&k.f);
    if (X87_ABOVE(nearz, zmax))
        goto out;

    /* Children: the object's node list, through a global node stack. */
    uint32_t list = RD32(obj + OBJ_CHILDREN);
    if (list) {
        uint32_t node = RD32(list + 8u);
        if (RD32(node) != 0 && node != 0) {
            uint32_t ebx = list;
            do {
                uint32_t next = RD32(node);
                uint32_t push = next && RD32(next) ? next : 0;
                uint32_t depth = RD32(G_NODE_DEPTH);
                WR32(G_NODE_STACK + 4u * depth, push);
                WR32(G_NODE_DEPTH, depth + 1u);
                WR32(S - 4u, RET_MODE);
                c->r[2] = next;
                c->r[3] = ebx;
                c->r[5] = k.ebp;
                c->r[6] = node;
                c->r[7] = ctx;
                c->fsw = native_fsw(&k.fsw);
                uint32_t mode = (uint32_t)f_004D14D0(c, S - 4u, depth + 1u, push) & 0xFFu;
                native_fsw_sync(&k.fsw, c);
                edx = RD16(node + 0x1Cu);
                ecx = mode * 0x180u + edx;
                eax = RD32(G_CHILD_FN + 4u * ecx);
                if (eax) {
                    static const GuestFunc *ic = g_guest_func_hash;
                    WR32(S - 4u, ctx);
                    WR32(S - 8u, node);
                    c->r[2] = edx;
                    eax = (uint32_t)native_call_ic(c, eax, S - 8u, RET_CHILD, ecx, eax, &ic);
                    native_fsw_sync(&k.fsw, c);
                    edx = c->r[2];
                }
                ecx = RD32(G_NODE_DEPTH) - 1u;
                WR32(G_NODE_DEPTH, ecx);
                node = RD32(G_NODE_STACK + 4u * ecx);
            } while (node);
        }
    }
out:
    c->fsw = native_fsw(&k.fsw);   /* the guest code stores its status word on return */
    c->r[1] = ecx;
    c->r[2] = edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    *ret = done | eax;
    return 1;
}
