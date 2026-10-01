/* Hand-decompiled segment query against an object's collision mesh (IGI.exe).
 *
 *   0x004C1840  mesh_raycast   intersects segments with the triangles of an
 *                              object's collision mesh and keeps the nearest
 *                              hit of each segment in its result record
 *
 *   void mesh_raycast(Object *obj, Query *q);   (cdecl)
 *
 * Query: +0 / +4 segment start and end points (double[3], 0x18 bytes apart),
 * +8 segment count, +0xC object class filter (0xFF: none; else a callback per
 * class and object type, 0xA96AE0, that may veto the object through the byte
 * at +0x10), +0x14 collision mesh index, +0x18 nonzero: also triangles of
 * materials with a surface, +0x20 result records (0x70 bytes per segment).
 *
 * Each segment is first rejected against the object's bounding sphere
 * (0x4D0950: the model's radius, times the largest scale), then moved into
 * the object's frame (rotation, then the x scale) and walked through the
 * mesh's sphere tree (explicit stack in the guest frame) with a shrinking box
 * around the part of the segment left. Triangles touching the box are
 * intersected (plane, then barycentric coordinates on the dominant axis
 * plane) and filtered by 0x4BD070; the nearest hit (near ties decided by
 * the side the triangle faces) goes into the result: world position and
 * normal, the local ones, the material (on the facing or back side), the
 * distance, the triangle and its barycentric coordinates.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float wherever the game stores a float):
 * the same operations in the same order, so the results are bit-identical to
 * the recompiled code (tools/recomp/nativebench.py). Every comparison updates
 * the modelled status word, which the guest calls see and the function
 * leaves; the guest calls see the same registers and stack, and the frame
 * the guest leaves below its entry esp is reproduced (every store to it). */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_MESH_RAYCAST  0x004C1840u
#define FN_MODEL_RADIUS  0x004D0950u   /* float (st0) radius of a model */
#define FN_HIT_FILTER    0x004BD070u   /* (mesh, triangle, v, u): nonzero rejects the hit */

#define G_CLASS_FN       0x00A96AE0u   /* fn[class * 0x180 + type](obj, arg) */
#define K_ZERO_D         0x005333B0u   /* double 0 */
#define K_ONE_D          0x005333F8u   /* double 1 */
#define K_ZERO           0x005333ECu   /* float 0 */
#define K_ONE            0x005333E0u   /* float 1 */
#define K_MARGIN         0x005333C8u   /* float, the box margin */
#define K_EPS_D          0x005337C8u   /* double, distance tie tolerance */

#define RET_CLASS        0x004C18F7u
#define RET_RADIUS       0x004C19D1u
#define RET_FILTER       0x004C298Du

#define FRAME_SIZE       0x2F4u
#define F_STACK          0x170u        /* node stack */

#define GR8(a) ((uint32_t)gm[(uint32_t)(a)])
#define GR16(a) rt_ld16(gm + (uint32_t)(a))
#define GR32(a) rt_ld32(gm + (uint32_t)(a))
#define GW8(a, v) (gm[(uint32_t)(a)] = (uint8_t)(v))
#define GW32(a, v) rt_st32(gm + (uint32_t)(a), (uint32_t)(v))
#define GRF(a) rt_ldf32(gm + (uint32_t)(a))
#define GRD(a) rt_ldf64(gm + (uint32_t)(a))
#define FW(off, v) GW32(S + (off), (v))
#define FW8(off, v) GW8(S + (off), (v))
#define FWF(off, v) stf(gm + S + (off), (v))
#define FWD(off, v) std64(gm + S + (off), (v))

static inline void stf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static inline void std64(uint8_t *p, double v) { memcpy(p, &v, 8); }
static inline uint32_t f2u(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static inline float u2f(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }

/* The comparisons of the guest: they record the status word. */
#define CMP(a, b) native_fcom(&fsw, (a), (b))

typedef struct Ray {
    double inv;                 /* S+0xE8: 1 / |d|^2 */
    float len;                  /* S+0xE4: |d| */
    float p0[3];                /* S+0x20: start in the mesh's frame */
    float d[3];                 /* S+0x84: direction */
    float lo[3], hi[3];         /* S+0x9C, S+0x90: box around the segment left */
} Ray;

int rt_native_mesh_raycast(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_MESH_RAYCAST);
    uint8_t *const gm = g_mem;
    const uint32_t top0 = c->ftop;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    uint32_t edx = c->r[2];
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);
    /* push ebp; mov ebp, esp; and esp, -8; sub esp, 0x2F4; push ebx, esi, edi. */
    const uint32_t E = esp - 4u;
    const uint32_t A = (E & ~7u) - FRAME_SIZE;
    const uint32_t S = A - 12u;
    GW32(E, ebp0);
    GW32(A - 4u, ebx0);
    GW32(A - 8u, esi0);
    GW32(A - 12u, edi0);
    const uint32_t obj = GR32(esp + 4u), q = GR32(esp + 8u);
    uint32_t p0a = GR32(q), p1a = GR32(q + 4u);
    uint32_t ebx = GR32(q + 0x20u);
    FW(0xC8u, p0a);
    eax = GR32(obj + 0x6Cu);
    FW(0xA8u, p1a);
    ecx = p1a;
    if (!eax)
        goto out;
    ecx = GR32(obj + 0xA4u);
    if (ecx == 0xFFFFFFFFu)
        goto out;
    const uint32_t m = GR32(eax + ecx * 4u);
    FW(0x13Cu, m);
    edx = GR32(m + 0xCu);
    const uint32_t mesh = GR32(edx + 0xCu);
    ecx = mesh;
    eax = GR32(mesh + 0x28u);
    if (eax)
        goto out;
    edx = GR32(mesh + 0x2Cu);
    const uint32_t coll = (GR32(q + 0x14u) << 5) + edx;
    eax = coll;
    FW(0x80u, coll);
    if (!coll || !GR32(coll))
        goto out;
    const uint32_t verts = GR32(coll + 0x14u);
    FW(0xB8u, verts);
    eax = (verts & 0xFFFFFF00u) | GR8(q + 0xCu);
    if ((eax & 0xFFu) != 0xFFu) {
        eax &= 0xFFu;
        edx = GR16(obj + 0x1Cu);
        ecx = eax * 0x180u + edx;
        eax = GR32(G_CLASS_FN + ecx * 4u);
        if (eax) {
            /* fn(obj, q->arg): a zero byte at q->arg vetoes the object. */
            static const GuestFunc *ic = g_guest_func_hash;
            ecx = GR32(q + 0x10u);
            GW32(S - 4u, ecx);
            GW32(S - 8u, obj);
            c->r[2] = edx;
            c->r[3] = ebx;
            c->r[5] = E;
            c->r[6] = obj;
            c->r[7] = q;
            c->fsw = native_fsw(&fsw);
            eax = (uint32_t)native_call_ic(c, eax, S - 8u, RET_CLASS, ecx, eax, &ic);
            native_fsw_sync(&fsw, c);
            ecx = c->r[1];
            edx = GR32(q + 0x10u);
            if (GR8(edx) == 0)
                goto out;
        }
    }
    eax = GR32(q + 8u);
    FW(0xF4u, 0);
    if ((int32_t)eax <= 0)
        goto out;

    const double sx = GRF(obj + 0x98u), sy = GRF(obj + 0x9Cu), sz = GRF(obj + 0xA0u);
    const double ox = GRD(obj + 0x20u), oy = GRD(obj + 0x28u), oz = GRD(obj + 0x30u);
    const uint32_t root = GR32(coll + 0x1Cu);
    /* Loaded when a segment gets past the sphere test (most do not). */
    double M[9], zero_d = 0, one_d = 0, zero = 0, one = 0, margin = 0, eps = 0;
    int loaded = 0;
    double best = 0;
    uint32_t best_tri = 0, hit_v = 0, hit_u = 0;
    uint32_t i = 0;
    do {
        /* Earlier hit of this segment and the sphere test. */
        uint32_t valid = GR32(ebx + 0x58u);
        uint32_t ahi = valid ? GR32(ebx + 0x4Cu) : 0;
        uint8_t back = valid ? GR32(ebx + 0x4Cu) != 0 : 0;   /* S+0x13 */
        uint8_t first = 1;                                    /* S+0x5F: the root is entered */
        FW8(0x5Fu, 1);
        FW8(0x13u, back);
        uint8_t have = GR8(ebx + 0x58u);                      /* S+0x2F */
        FW8(0x2Fu, have);
        CMP(sz, sy);
        FW8(0x5Du, 0);
        FW(0x54u, 0);
        FW(0xF0u, root);
        FW(F_STACK, root);
        uint8_t found = 0;                                    /* S+0x5D */
        double m1 = (sz >= sy) ? sz : sy;
        CMP(sx, m1);
        double scale;
        uint32_t sw;
        if (sx > m1) {
            sw = rt_fstsw(native_fsw(&fsw), top0 - 1u);
            scale = sx;
        } else {
            CMP(sz, sy);
            sw = rt_fstsw(native_fsw(&fsw), top0);
            scale = (sz >= sy) ? sz : sy;
        }
        eax = (ahi & 0xFFFF0000u) | (sw & 0xFFFFu);
        const float scalef = (float)scale;
        FWF(0x18u, scalef);
        edx = GR32(obj + 0x6Cu);
        GW32(S - 4u, edx);
        GW32(S - 8u, RET_RADIUS);
        c->r[2] = edx;
        c->r[3] = ebx;
        c->r[5] = E;
        c->r[6] = obj;
        c->r[7] = root;
        c->ftop = top0;
        c->fsw = native_fsw(&fsw);
        f_004D0950(c, S - 8u, coll, eax);
        native_fsw_sync(&fsw, c);
        const double r = c->st[(top0 + 7u) & 7u] * (double)scalef;
        p0a = GR32(S + 0xC8u);
        p1a = GR32(S + 0xA8u);
        const float rf = (float)r;
        FWF(0x14u, rf);
        /* The sphere against the segment's bounding box. */
        {
            double v = ox - r;
            CMP(v, GRD(p0a));
            if (v > GRD(p0a)) {
                CMP(v, GRD(p1a));
                if (v > GRD(p1a))
                    goto next;
            }
            v = (double)rf + ox;
            CMP(v, GRD(p0a));
            if (!(v >= GRD(p0a))) {
                CMP(v, GRD(p1a));
                if (!(v >= GRD(p1a)))
                    goto next;
            }
            v = oy - (double)rf;
            CMP(v, GRD(p0a + 8u));
            if (v > GRD(p0a + 8u)) {
                CMP(v, GRD(p1a + 8u));
                if (v > GRD(p1a + 8u))
                    goto next;
            }
            v = (double)rf + oy;
            CMP(v, GRD(p0a + 8u));
            if (!(v >= GRD(p0a + 8u))) {
                CMP(v, GRD(p1a + 8u));
                if (!(v >= GRD(p1a + 8u)))
                    goto next;
            }
            v = oz - (double)rf;
            CMP(v, GRD(p0a + 16u));
            if (v > GRD(p0a + 16u)) {
                CMP(v, GRD(p1a + 16u));
                if (v > GRD(p1a + 16u))
                    goto next;
            }
            v = (double)rf + oz;
            CMP(v, GRD(p0a + 16u));
            if (!(v >= GRD(p0a + 16u))) {
                CMP(v, GRD(p1a + 16u));
                if (!(v >= GRD(p1a + 16u)))
                    goto next;
            }
        }
        if (!loaded) {
            for (uint32_t k = 0; k < 9u; k++)
                M[k] = GRF(obj + 0x70u + 4u * k);
            zero_d = GRD(K_ZERO_D);
            one_d = GRD(K_ONE_D);
            zero = GRF(K_ZERO);
            one = GRF(K_ONE);
            margin = GRF(K_MARGIN);
            eps = GRD(K_EPS_D);
            loaded = 1;
        }
        Ray ry;
        float q1[3];
        {
            /* Start and end in the mesh's frame (rotation, then the x scale). */
            double dx = GRD(p0a) - ox, dy = GRD(p0a + 8u) - oy, dz = GRD(p0a + 16u) - oz;
            float a0 = (float)((dz * M[6] + dx * M[0]) + dy * M[3]);
            float a1 = (float)((dz * M[7] + dx * M[1]) + dy * M[4]);
            float a2 = (float)((dz * M[8] + dx * M[2]) + dy * M[5]);
            FWF(0x104u, a0);
            FWF(0x108u, a1);
            FWF(0x10Cu, a2);
            ry.p0[0] = (float)((double)a0 / sx);
            ry.p0[1] = (float)((double)a1 / sx);
            ry.p0[2] = (float)((double)a2 / sx);
            FWF(0x20u, ry.p0[0]);
            FWF(0x24u, ry.p0[1]);
            FWF(0x28u, ry.p0[2]);
            dx = GRD(p1a) - ox;
            dy = GRD(p1a + 8u) - oy;
            dz = GRD(p1a + 16u) - oz;
            float b0 = (float)((dy * M[3] + dz * M[6]) + dx * M[0]);
            float b1 = (float)((dz * M[7] + dx * M[1]) + dy * M[4]);
            float b2 = (float)((dy * M[5] + dz * M[8]) + dx * M[2]);
            FWF(0x11Cu, b0);
            FWF(0x120u, b1);
            FWF(0x124u, b2);
            q1[0] = (float)((double)b0 / sx);
            q1[1] = (float)((double)b1 / sx);
            double q1z = (double)b2 / sx;
            q1[2] = (float)q1z;
            FWF(0xCCu, q1[0]);
            FWF(0xD0u, q1[1]);
            FWF(0xD4u, q1[2]);
            double ddz = q1z - (double)ry.p0[2];
            FWD(0x48u, ddz);
            double ddy = (double)q1[1] - (double)ry.p0[1];
            FWD(0x60u, ddy);
            double ddx = (double)q1[0] - (double)ry.p0[0];
            double len2 = (ddx * ddx + ddy * ddy) + ddz * ddz;
            ry.len = (float)rt_fsqrt(len2);
            FWF(0xE4u, ry.len);
            float c30 = q1[0], c34 = q1[1], c38 = q1[2];
            if (have) {
                /* The earlier hit ends the segment. */
                double hx = GRD(ebx + 8u) - ox, hy = GRD(ebx + 16u) - oy;
                float hzf = (float)(GRD(ebx + 24u) - oz);
                FWF(0x38u, hzf);
                double hz = hzf;
                float h0 = (float)((hy * M[3] + hz * M[6]) + hx * M[0]);
                float h1 = (float)((hz * M[7] + hx * M[1]) + hy * M[4]);
                float h2 = (float)((hy * M[5] + hz * M[8]) + hx * M[2]);
                FWF(0x110u, h0);
                FWF(0x114u, h1);
                FWF(0x118u, h2);
                c30 = (float)((double)h0 / sx);
                c34 = (float)((double)h1 / sx);
                double c38d = (double)h2 / sx;
                c38 = (float)c38d;
                double ez = c38d - (double)ry.p0[2];
                double ey = (double)c34 - (double)ry.p0[1];
                double ex = (double)c30 - (double)ry.p0[0];
                FWD(0x60u, ex);
                float hl = (float)rt_fsqrt((ex * ex + ey * ey) + ez * ez);
                FWF(0x78u, hl);
                best = (double)hl;
                FWD(0xD8u, best);
            }
            /* Box around what is left of the segment. */
            double a, b;
            a = ry.p0[0], b = c30;
            CMP(a, b);
            ry.lo[0] = (float)(((a >= b) ? b : a) - margin);
            a = ry.p0[1], b = c34;
            CMP(a, b);
            ry.lo[1] = (float)(((a >= b) ? b : a) - margin);
            a = ry.p0[2], b = c38;
            CMP(a, b);
            ry.lo[2] = (float)(((a >= b) ? b : a) - margin);
            a = ry.p0[0], b = c30;
            CMP(a, b);
            ry.hi[0] = (float)(((a > b) ? a : b) + margin);
            a = ry.p0[1], b = c34;
            CMP(a, b);
            ry.hi[1] = (float)(((a > b) ? a : b) + margin);
            a = ry.p0[2], b = c38;
            CMP(a, b);
            ry.hi[2] = (float)(((a > b) ? a : b) + margin);
            FWF(0x9Cu, ry.lo[0]);
            FWF(0xA0u, ry.lo[1]);
            FWF(0xA4u, ry.lo[2]);
            FWF(0x90u, ry.hi[0]);
            FWF(0x94u, ry.hi[1]);
            FWF(0x98u, ry.hi[2]);
            FWF(0x30u, q1[0]);
            FWF(0x34u, q1[1]);
            FWF(0x38u, q1[2]);
            ry.inv = one_d / len2;
            FWD(0xE8u, ry.inv);
            ry.d[0] = (float)ddx;
            ry.d[1] = (float)ddy;
            ry.d[2] = (float)ddz;
            FWF(0x84u, ry.d[0]);
            FWF(0x88u, ry.d[1]);
            FWF(0x8Cu, ry.d[2]);
            CMP(ry.d[1], zero);
            FWF(0x78u, (ry.d[1] >= zero) ? ry.d[1] : -ry.d[1]);
            CMP(ry.d[0], zero);
            FWF(0x78u, (ry.d[0] >= zero) ? ry.d[0] : -ry.d[0]);
        }
        if (!GR32(coll + 0xCu))
            goto next;

        /* The sphere tree: the root first, then children (+0x14) and siblings
         * (+0x16) of the nodes whose sphere meets the line and the box. */
        {
            const uint32_t base = root;
            int32_t depth = 0;
            uint32_t node = root;
            const double d0 = ry.d[0], d1 = ry.d[1], d2 = ry.d[2];
            const double px = ry.p0[0], py = ry.p0[1], pz = ry.p0[2];
            for (;;) {
                if (!first) {
                    uint32_t sib = GR16(node + 0x16u);
                    if (!sib) {
                        depth--;
                        FW(0x54u, (uint32_t)depth);
                        if (depth < 0)
                            break;
                        node = GR32(S + (uint32_t)depth * 4u + F_STACK);
                        continue;
                    }
                    node = base + sib * 24u;
                    double nx = GRF(node) - px, ny = GRF(node + 4u) - py, nz = GRF(node + 8u) - pz;
                    GW32(S + (uint32_t)depth * 4u + F_STACK, node);
                    FWD(0x78u, nz);
                    FWD(0x48u, ny);
                    FWD(0x60u, nx);
                    double t = (nz * d2 + ny * d1) + nx * d0;
                    const float rad = u2f(GR32(node + 12u));
                    FWF(0x50u, rad);
                    double dist2 = (nx * nx + ny * ny) + nz * nz;
                    dist2 = dist2 - t * t * ry.inv;
                    double rr = (double)rad * (double)rad;
                    CMP(rr, dist2);
                    if (!(rr >= dist2))
                        continue;
                    double v = (double)rad + GRF(node);
                    CMP(v, ry.lo[0]);
                    if (!(v >= ry.lo[0]))
                        continue;
                    v = GRF(node + 4u) + (double)rad;
                    CMP(v, ry.lo[1]);
                    if (!(v >= ry.lo[1]))
                        continue;
                    v = GRF(node + 8u) + (double)rad;
                    CMP(v, ry.lo[2]);
                    if (!(v >= ry.lo[2]))
                        continue;
                    v = GRF(node) - (double)rad;
                    CMP(v, ry.hi[0]);
                    if (v > ry.hi[0])
                        continue;
                    v = GRF(node + 4u) - (double)rad;
                    CMP(v, ry.hi[1]);
                    if (v > ry.hi[1])
                        continue;
                    v = GRF(node + 8u) - (double)rad;
                    CMP(v, ry.hi[2]);
                    if (v > ry.hi[2])
                        continue;
                }
                /* Entered: down the first children while they pass (the
                 * last one tried, passing or not, has its triangles tested). */
                uint32_t child = GR16(node + 0x14u);
                first = 0;
                FW8(0x5Fu, 0);
                if (child) {
                    for (;;) {
                        depth++;
                        node = base + child * 24u;
                        double nx = GRF(node) - px, ny = GRF(node + 4u) - py, nz = GRF(node + 8u) - pz;
                        GW32(S + (uint32_t)depth * 4u + F_STACK, node);
                        FWD(0x78u, ny);
                        FWD(0x48u, nx);
                        FWD(0x60u, nz);
                        double t = (nz * d2 + ny * d1) + nx * d0;
                        const float rad = u2f(GR32(node + 12u));
                        FWF(0x50u, rad);
                        double dist2 = (nz * nz + nx * nx) + ny * ny;
                        dist2 = dist2 - t * t * ry.inv;
                        double rr = (double)rad * (double)rad;
                        CMP(rr, dist2);
                        if (!(rr >= dist2))
                            break;
                        double v = (double)rad + GRF(node);
                        CMP(v, ry.lo[0]);
                        if (!(v >= ry.lo[0]))
                            break;
                        v = GRF(node + 4u) + (double)rad;
                        CMP(v, ry.lo[1]);
                        if (!(v >= ry.lo[1]))
                            break;
                        v = GRF(node + 8u) + (double)rad;
                        CMP(v, ry.lo[2]);
                        if (!(v >= ry.lo[2]))
                            break;
                        v = GRF(node) - (double)rad;
                        CMP(v, ry.hi[0]);
                        if (v > ry.hi[0])
                            break;
                        v = GRF(node + 4u) - (double)rad;
                        CMP(v, ry.hi[1]);
                        if (v > ry.hi[1])
                            break;
                        v = GRF(node + 8u) - (double)rad;
                        CMP(v, ry.hi[2]);
                        if (v > ry.hi[2])
                            break;
                        child = GR16(node + 0x14u);
                        if (!child)
                            break;
                    }
                    FW(0x54u, (uint32_t)depth);
                }

                /* The node's triangles. */
                const uint32_t ntri = GR16(node + 0x12u);
                if (!ntri)
                    continue;
                uint32_t j = GR16(node + 0x10u);
                uint32_t tri = GR32(coll + 0x10u) + j * 8u;
                FW(0x78u, j);
                FW(0x50u, tri);
                if ((int32_t)j >= (int32_t)(ntri + j))
                    continue;
                for (;;) {
                    const uint32_t v0 = verts + GR16(tri) * 16u;
                    FW8(0x5Eu, 0);
                    const uint32_t v1 = verts + GR16(tri + 2u) * 16u;
                    const uint32_t v2 = verts + GR16(tri + 4u) * 16u;
                    FW(0x14u, v1);
                    FW(0x18u, v2);
                    const uint32_t mat = GR16(tri + 6u);
                    if (GR16(GR32(coll + 0x18u) + mat * 12u + 4u) != 0xFFFFu && !GR32(GR32(E + 0xCu) + 0x18u))
                        goto next_tri;
                    {
                        /* All three vertices on one side of the box: no hit. */
                        const double x0 = GRF(v0), y0 = GRF(v0 + 4u), z0 = GRF(v0 + 8u);
                        const double x1 = GRF(v1), y1 = GRF(v1 + 4u), z1 = GRF(v1 + 8u);
                        const double x2 = GRF(v2), y2 = GRF(v2 + 4u), z2 = GRF(v2 + 8u);
                        double lo, hi;
                        lo = ry.lo[0];
                        CMP(lo, x0);
                        if (lo > x0) {
                            CMP(lo, x1);
                            if (lo > x1) {
                                CMP(lo, x2);
                                if (lo > x2)
                                    goto next_tri;
                            }
                        }
                        lo = ry.lo[1];
                        CMP(lo, y0);
                        if (lo > y0) {
                            CMP(lo, y1);
                            if (lo > y1) {
                                CMP(lo, y2);
                                if (lo > y2)
                                    goto next_tri;
                            }
                        }
                        lo = ry.lo[2];
                        CMP(lo, z0);
                        if (lo > z0) {
                            CMP(lo, z1);
                            if (lo > z1) {
                                CMP(lo, z2);
                                if (lo > z2)
                                    goto next_tri;
                            }
                        }
                        hi = ry.hi[0];
                        CMP(hi, x0);
                        if (!(hi >= x0)) {
                            CMP(hi, x1);
                            if (!(hi >= x1)) {
                                CMP(hi, x2);
                                if (!(hi >= x2))
                                    goto next_tri;
                            }
                        }
                        hi = ry.hi[1];
                        CMP(hi, y0);
                        if (!(hi >= y0)) {
                            CMP(hi, y1);
                            if (!(hi >= y1)) {
                                CMP(hi, y2);
                                if (!(hi >= y2))
                                    goto next_tri;
                            }
                        }
                        hi = ry.hi[2];
                        CMP(hi, z0);
                        if (!(hi >= z0)) {
                            CMP(hi, z1);
                            if (!(hi >= z1)) {
                                CMP(hi, z2);
                                if (!(hi >= z2))
                                    goto next_tri;
                            }
                        }

                        /* The plane: normal n = e2 x e1 (e1 = v2 - v0, e2 = v1 - v0). */
                        const float e1x = (float)(x2 - x0), e1y = (float)(y2 - y0), e1z = (float)(z2 - z0);
                        FWF(0x3Cu, e1x);
                        FWF(0x40u, e1y);
                        FWF(0x44u, e1z);
                        const float e2x = (float)(x1 - x0), e2y = (float)(y1 - y0);
                        const double e2zd = z1 - z0;
                        const float e2z = (float)e2zd;
                        FWF(0xBCu, e2x);
                        FWF(0xC0u, e2y);
                        FWF(0xC4u, e2z);
                        const float nx = (float)(e2zd * e1y - (double)e2y * e1z);
                        FWF(0x18u, nx);
                        FWF(0xACu, nx);
                        const double nyd = (double)e2z * e1x - (double)e2x * e1z;
                        const float ny0 = (float)nyd;
                        FWF(0x58u, ny0);
                        const float ny = (float)-nyd;
                        FWF(0xB0u, ny);
                        const float nz = (float)((double)e2y * e1x - (double)e2x * e1y);
                        FWF(0x68u, nz);
                        FWF(0xB4u, nz);
                        const double den = ((double)nz * d2 + (double)ny * d1) + (double)nx * d0;
                        FWD(0x60u, den);
                        CMP(den, zero_d);
                        if (!(den < zero_d || den > zero_d))
                            goto next_tri;
                        const double tt = (((z0 - pz) * nz + (y0 - py) * ny) + (x0 - px) * nx) / den;
                        const float tf = (float)tt;
                        FWF(0x14u, tf);
                        CMP(tt, zero);
                        if (!(tt >= zero))
                            goto next_tri;
                        CMP(tf, one);
                        if ((double)tf > one)
                            goto next_tri;
                        const float hx = (float)((double)tf * d0 + px);
                        const float hy = (float)((double)tf * d1 + py);
                        const float hz = (float)((double)tf * d2 + pz);
                        FWF(0x6Cu, hx);
                        FWF(0x70u, hy);
                        FWF(0x74u, hz);

                        /* Barycentric coordinates on the plane of the dominant axis. */
                        const float anx = (nx >= zero) ? nx : -nx;
                        CMP(nx, zero);
                        FWF(0x14Cu, anx);
                        CMP(ny, zero);
                        const double any = (ny >= zero) ? (double)ny : -(double)ny;
                        CMP(nz, zero);
                        const double anz = (nz >= zero) ? (double)nz : -(double)nz;
                        double u, w = 0;
                        int axis;   /* 0: x, 1: y, 2: z */
                        CMP(anx, any);
                        if (anx > any) {
                            CMP(anx, anz);
                            axis = anx > anz ? 0 : -1;
                        } else {
                            axis = -1;
                        }
                        if (axis < 0) {
                            CMP(any, anz);
                            axis = any > anz ? 1 : 2;
                        }
                        float s18 = 0, s58 = 0, s68 = 0;
                        double a1, a2;
                        if (axis == 0) {
                            CMP(nx, zero_d);
                            if (!((double)nx < zero_d || (double)nx > zero_d))
                                goto next_tri;
                            s68 = (float)((double)hz - z0);
                            FWF(0x68u, s68);
                            s18 = (float)((double)hy - y0);
                            FWF(0x18u, s18);
                            u = ((double)s68 * e1y - (double)s18 * e1z) / nx;
                            FWD(0x48u, u);
                            CMP(u, zero_d);
                            if (!(u >= zero_d))
                                goto next_tri;
                            CMP(u, one_d);
                            if (u > one_d)
                                goto next_tri;
                            CMP(e1y, zero);
                            a1 = (e1y >= zero) ? (double)e1y : -(double)e1y;
                            CMP(e1z, zero);
                            a2 = (e1z >= zero) ? (double)e1z : -(double)e1z;
                            CMP(a1, a2);
                            if (a1 > a2)
                                w = ((double)s18 - (double)e2y * u) / e1y;
                            else
                                goto zaxis;
                        } else if (axis == 1) {
                            CMP(ny0, zero_d);
                            if (!((double)ny0 < zero_d || (double)ny0 > zero_d))
                                goto next_tri;
                            s68 = (float)((double)hz - z0);
                            FWF(0x68u, s68);
                            s58 = (float)((double)hx - x0);
                            FWF(0x58u, s58);
                            u = ((double)s68 * e1x - (double)s58 * e1z) / ny0;
                            FWD(0x48u, u);
                            CMP(u, zero_d);
                            if (!(u >= zero_d))
                                goto next_tri;
                            CMP(u, one_d);
                            if (u > one_d)
                                goto next_tri;
                            CMP(e1x, zero);
                            a1 = (e1x >= zero) ? (double)e1x : -(double)e1x;
                            CMP(e1z, zero);
                            a2 = (e1z >= zero) ? (double)e1z : -(double)e1z;
                            CMP(a1, a2);
                            if (a1 > a2)
                                w = ((double)s58 - (double)e2x * u) / e1x;
                            else
                                goto zaxis;
                        } else {
                            CMP(nz, zero_d);
                            if (!((double)nz < zero_d || (double)nz > zero_d))
                                goto next_tri;
                            s18 = (float)((double)hy - y0);
                            FWF(0x18u, s18);
                            s58 = (float)((double)hx - x0);
                            FWF(0x58u, s58);
                            u = ((double)s18 * e1x - (double)s58 * e1y) / nz;
                            FWD(0x48u, u);
                            CMP(u, zero_d);
                            if (!(u >= zero_d))
                                goto next_tri;
                            CMP(u, one_d);
                            if (u > one_d)
                                goto next_tri;
                            CMP(e1x, zero);
                            a1 = (e1x >= zero) ? (double)e1x : -(double)e1x;
                            CMP(e1y, zero);
                            a2 = (e1y >= zero) ? (double)e1y : -(double)e1y;
                            CMP(a1, a2);
                            if (a1 > a2) {
                                w = ((double)s58 - (double)e2x * u) / e1x;
                            } else {
                                CMP(e1y, zero);
                                if (!((double)e1y < zero || (double)e1y > zero))
                                    goto next_tri;
                                w = ((double)s18 - (double)e2y * u) / e1y;
                            }
                        }
                        if (0) {
                        zaxis:
                            CMP(e1z, zero);
                            if (!((double)e1z < zero || (double)e1z > zero))
                                goto next_tri;
                            w = ((double)s68 - (double)e2z * u) / e1z;
                        }
                        CMP(w, zero_d);
                        if (!(w >= zero_d))
                            goto next_tri;
                        CMP(w + u, one_d);
                        if (w + u > one_d)
                            goto next_tri;

                        /* The filter. */
                        const float uf = (float)u, wf = (float)w;
                        FWF(0x58u, uf);
                        FWF(0x18u, wf);
                        GW32(S - 4u, f2u(uf));
                        GW32(S - 8u, f2u(wf));
                        GW32(S - 12u, tri);
                        GW32(S - 16u, m);
                        GW32(S - 20u, RET_FILTER);
                        c->r[2] = tri;
                        c->r[3] = ebx;
                        c->r[5] = E;
                        c->r[6] = obj;
                        c->r[7] = node;
                        c->ftop = top0;
                        c->fsw = native_fsw(&fsw);
                        uint32_t rej = (uint32_t)f_004BD070(c, S - 20u, f2u(wf), m);
                        native_fsw_sync(&fsw, c);
                        if (rej)
                            goto next_tri;

                        /* Nearer than the best so far (ties: facing first). */
                        const double dist = (double)tf * (double)ry.len;
                        CMP(den, zero_d);
                        const uint8_t facing = !(den >= zero_d);
                        if (have) {
                            CMP(dist + eps, best);
                            if (dist + eps >= best) {
                                uint8_t ok;
                                CMP(dist, best);
                                if (!(dist >= best)) {
                                    if (facing || !back)
                                        ok = 1;
                                    else
                                        ok = GR8(S + 0x5Eu);
                                } else {
                                    CMP(dist - eps, best);
                                    if (dist - eps >= best)
                                        goto next_tri;
                                    if (facing && !back)
                                        ok = 1;
                                    else
                                        ok = GR8(S + 0x5Eu);
                                }
                                FW8(0x1Fu, 1);
                                if (!ok)
                                    goto next_tri;
                            } else {
                                FW8(0x1Fu, 0);
                            }
                        } else {
                            FW8(0x1Fu, 0);
                        }
                        best = dist;
                        FWD(0xD8u, best);
                        CMP(px, hx);
                        FW(0x34u, f2u(hy));
                        back = facing;
                        FW8(0x13u, back);
                        hit_u = f2u(uf);
                        FW(0x138u, hit_u);
                        best_tri = tri;
                        FW(0xE0u, best_tri);
                        FW(0x30u, f2u(hx));
                        found = 1;
                        FW8(0x5Du, 1);
                        have = 1;
                        FW8(0x2Fu, 1);
                        FW(0x38u, f2u(hz));
                        hit_v = f2u(wf);
                        FW(0x134u, hit_v);
                        /* The box shrinks to the segment's part before the hit. */
                        ry.lo[0] = (float)(((px >= hx) ? (double)hx : px) - margin);
                        CMP(py, hy);
                        ry.lo[1] = (float)(((py >= hy) ? (double)hy : py) - margin);
                        CMP(pz, hz);
                        ry.lo[2] = (float)(((pz >= hz) ? (double)hz : pz) - margin);
                        CMP(px, hx);
                        ry.hi[0] = (float)(((px > hx) ? px : (double)hx) + margin);
                        CMP(py, hy);
                        ry.hi[1] = (float)(((py > hy) ? py : (double)hy) + margin);
                        CMP(pz, hz);
                        ry.hi[2] = (float)(((pz > hz) ? pz : (double)hz) + margin);
                        FWF(0x9Cu, ry.lo[0]);
                        FWF(0xA0u, ry.lo[1]);
                        FWF(0xA4u, ry.lo[2]);
                        FWF(0x90u, ry.hi[0]);
                        FWF(0x94u, ry.hi[1]);
                        FWF(0x98u, ry.hi[2]);
                    }
                next_tri:
                    j++;
                    FW(0x78u, j);
                    tri += 8u;
                    FW(0x50u, tri);
                    if (!((int32_t)j < (int32_t)(GR16(node + 0x12u) + GR16(node + 0x10u))))
                        break;
                }
            }
        }

        /* The nearest hit into the segment's result. */
        if (!found)
            goto next;
        {
            const float h30 = u2f(GR32(S + 0x30u)), h34 = u2f(GR32(S + 0x34u)), h38 = u2f(GR32(S + 0x38u));
            if (GR32(obj + 0x98u) != 0x3F800000u || GR32(obj + 0x9Cu) != 0x3F800000u ||
                GR32(obj + 0xA0u) != 0x3F800000u) {
                /* Distance in world units. */
                const double wx = (double)h30 * sx, wy = (double)h34 * sy, wz = (double)h38 * sz;
                const float s164 = (float)((double)ry.p0[0] * sx), s168 = (float)((double)ry.p0[1] * sy);
                FWF(0x164u, s164);
                FWF(0x168u, s168);
                const double dz = (double)ry.p0[2] * sz - wz;
                FWD(0x60u, dz);
                const double dy = (double)s168 - wy;
                FWD(0xE8u, dy);
                const double dx = (double)s164 - wx;
                best = rt_fsqrt((dx * dx + dy * dy) + dz * dz);
                FWD(0xD8u, best);
            }
            if (GR32(ebx + 0x58u)) {
                double old = GRD(ebx + 0x60u);
                CMP(best, old);
                if (best >= old)
                    goto next;
            }
            const uint32_t v0 = verts + GR16(best_tri) * 16u, v1 = verts + GR16(best_tri + 2u) * 16u;
            const uint32_t v2 = verts + GR16(best_tri + 4u) * 16u;
            FW(0xE4u, v2);
            const uint32_t mat = GR16(GR32(coll + 0x18u) + GR16(best_tri + 6u) * 12u + 8u);
            const uint32_t matid = mat == 0xFFFFu ? GR32(GR32(obj + 0x68u) + 0xD40u)
                                                  : GR32(GR32(obj + 0x68u) + mat * 4u + 0xD3Cu);
            const double e1x = GRF(v2) - GRF(v0), e1y = GRF(v2 + 4u) - GRF(v0 + 4u), e1z = GRF(v2 + 8u) - GRF(v0 + 8u);
            const float e2x = (float)(GRF(v1) - GRF(v0)), e2y = (float)(GRF(v1 + 4u) - GRF(v0 + 4u));
            FWF(0x128u, e2x);
            FWF(0x12Cu, e2y);
            const double e2z = GRF(v1 + 8u) - GRF(v0 + 8u);
            const float nx = (float)(e2z * e1y - (double)e2y * e1z);
            FWF(0xF8u, nx);
            const float ny = (float)-(e2z * e1x - (double)e2x * e1z);
            FWF(0xFCu, ny);
            const float nz = (float)((double)e2y * e1x - (double)e2x * e1y);
            FWF(0x100u, nz);
            stf(gm + ebx + 0x38u, nx);
            stf(gm + ebx + 0x3Cu, ny);
            stf(gm + ebx + 0x40u, nz);
            stf(gm + ebx + 0x2Cu, nx);
            stf(gm + ebx + 0x30u, ny);
            stf(gm + ebx + 0x34u, nz);
            const float wnx = (float)((M[2] * nz + M[1] * ny) + M[0] * nx);
            const float wny = (float)(((double)nx * M[3] + M[5] * nz) + M[4] * ny);
            const float wnz = (float)((M[8] * nz + M[7] * ny) + M[6] * nx);
            FWF(0x140u, wnx);
            FWF(0x144u, wny);
            FWF(0x148u, wnz);
            stf(gm + ebx + 0x38u, wnx);
            stf(gm + ebx + 0x3Cu, wny);
            stf(gm + ebx + 0x40u, wnz);
            stf(gm + ebx + 0x20u, h30);
            stf(gm + ebx + 0x24u, h34);
            stf(gm + ebx + 0x28u, h38);
            const double lx = (double)h30 * sx, ly = (double)h34 * sy, lz = (double)h38 * sz;
            const float wx = (float)((ly * M[1] + lz * M[2]) + lx * M[0]);
            FWF(0x158u, wx);
            const float wy = (float)((lz * M[5] + lx * M[3]) + ly * M[4]);
            FWF(0x15Cu, wy);
            const double wz = (ly * M[7] + lz * M[8]) + lx * M[6];
            std64(gm + ebx + 8u, (double)wx + ox);
            std64(gm + ebx + 16u, (double)wy + oy);
            GW32(ebx + 0x50u, back ? 0 : matid);
            GW32(ebx + 0x4Cu, back ? matid : 0);
            std64(gm + ebx + 0x60u, best);
            std64(gm + ebx + 24u, wz + oz);
            GW32(ebx, obj);
            GW32(ebx + 0x54u, (uint32_t)(int32_t)(int8_t)GR8(S + 0x1Fu));
            GW32(ebx + 0x58u, 1);
            GW32(ebx + 0x48u, (uint32_t)((int32_t)(best_tri - GR32(coll + 0x10u)) >> 3));
            GW32(ebx + 0x68u, hit_v);
            GW32(ebx + 0x6Cu, hit_u);
        }
    next:
        FW(0xA8u, GR32(S + 0xA8u) + 0x18u);
        i++;
        ebx += 0x70u;
        FW(0xF4u, i);
        FW(0xC8u, GR32(S + 0xC8u) + 0x18u);
    } while ((int32_t)i < (int32_t)GR32(q + 8u));
    eax = i;
    ecx = q;
    edx = GR32(q + 8u);
out:
    c->r[1] = ecx;
    c->r[2] = edx;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    c->ftop = top0;
    c->fsw = native_fsw(&fsw);
    *ret = (uint64_t)(esp + 4u) << 32 | eax;
    return 1;
}
