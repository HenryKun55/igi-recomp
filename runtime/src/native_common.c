/* Switches and shared helpers of the hand-decompiled game functions
 * (runtime/include/native.h). */
#include "native.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

/* IGI_NATIVE=0 / native=0 in igi.cfg (runtime/src/main.c): all decline. */
int g_igi_native = 1;
int g_terrain_exact;
NativeHook g_native_hook;
uint32_t g_native_decline;

/* Camera projection and clip rectangle. */
#define G_PROJ_SX        0x00BCAB28u
#define G_PROJ_SY        0x00BCAB2Cu
#define G_PROJ_CX        0x00BCAB30u
#define G_PROJ_CY        0x00BCAB34u
#define G_CLIP_LEFT      0x00BCABD0u
#define G_CLIP_TOP       0x00BCABD4u
#define G_CLIP_RIGHT     0x00BCABD8u
#define G_CLIP_BOTTOM    0x00BCABDCu
#define K_ONE            0x005333E0u   /* float constant, the numerator of 1/z */

static inline uint32_t f2u(float v)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    return b;
}

void native_view_load(NativeView *v)
{
    v->one = native_ldf(K_ONE);
    v->psx = native_ldf(G_PROJ_SX);
    v->psy = native_ldf(G_PROJ_SY);
    v->pcx = native_ldf(G_PROJ_CX);
    v->pcy = native_ldf(G_PROJ_CY);
    v->left = native_ldf(G_CLIP_LEFT);
    v->top = native_ldf(G_CLIP_TOP);
    v->right = native_ldf(G_CLIP_RIGHT);
    v->bottom = native_ldf(G_CLIP_BOTTOM);
}

/* One vertex: outcode and screen position (zd: z before rounding, as compared
 * with the near plane). */
static inline uint32_t project(const NativeView *v, double x, double y, double zd, float z, unsigned flags,
                               uint32_t *scr)
{
    if (X87_BELOW(zd, v->near_z))
        return NV_OUT_NEAR;
    double rz = v->one / (double)z;
    float rzf = (float)rz;
    double sx = rz * x * v->psx + v->pcx;
    double sy = (flags & NV_PROJ_SY_FIRST) ? v->psy * rzf * y + v->pcy : (double)rzf * y * v->psy + v->pcy;
    float syf = (float)sy;
    scr[0] = f2u((float)sx);
    scr[1] = f2u(syf);
    scr[2] = f2u(rzf);
    return (uint32_t)X87_BELOW(sx, v->left) | (uint32_t)X87_BELOW(sy, v->top) << 1 |
           (uint32_t)X87_ABOVE(sx, v->right) << 2 | (uint32_t)X87_ABOVE((double)syf, v->bottom) << 3;
}

static inline __attribute__((always_inline)) void transform(const NativeView *v, uint32_t src, uint32_t stride,
                                                            uint32_t n, uint32_t dst, unsigned flags,
                                                            NativeVerts *lp)
{
    uint8_t *const gm = g_mem;
    const double *m = v->m;
    uint32_t zmin = lp->zmin;
    float zminf;
    memcpy(&zminf, &zmin, 4);
    uint32_t scr[3] = { lp->scr[0], lp->scr[1], lp->scr[2] };
    float p0 = 0, p1 = 0, p2 = 0, x = 0, y = 0, z = 0;
    for (; n != 0; n--, src += stride, dst += NV_VERT_SIZE) {
        double vx = rt_ldf32(gm + src), vy = rt_ldf32(gm + src + 4u), vz = rt_ldf32(gm + src + 8u);
        if (flags & NV_ROT_XZY) {
            p0 = (float)((m[0] * vx + m[2] * vz) + m[1] * vy);
            p1 = (float)((m[3] * vx + m[5] * vz) + m[4] * vy);
            p2 = (float)((m[6] * vx + m[8] * vz) + m[7] * vy);
        } else {
            p0 = (float)((m[2] * vz + m[1] * vy) + m[0] * vx);
            p1 = (float)((m[5] * vz + m[4] * vy) + m[3] * vx);
            p2 = (float)((m[8] * vz + m[7] * vy) + m[6] * vx);
        }
        x = (float)((double)p0 + v->t[0]);
        y = (float)((double)p1 + v->t[1]);
        double zd = (double)p2 + v->t[2];
        z = (float)zd;
        uint32_t code = project(v, x, y, zd, z, flags, scr);
        uint8_t *d = gm + dst;
        memcpy(d, &x, 4);
        memcpy(d + 4, &y, 4);
        memcpy(d + 8, &z, 4);
        memcpy(d + 12, scr, 12);
        rt_st32(d + NV_VERT_CODE, code);
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

void native_view_transform(const NativeView *v, uint32_t src, uint32_t stride, uint32_t n, uint32_t dst,
                           unsigned flags, NativeVerts *lp)
{
    switch (flags & 3u) {
    case NV_ROT_XZY | NV_PROJ_SY_FIRST:
        transform(v, src, stride, n, dst, NV_ROT_XZY | NV_PROJ_SY_FIRST, lp);
        break;
    case NV_PROJ_SY_FIRST:
        transform(v, src, stride, n, dst, NV_PROJ_SY_FIRST, lp);
        break;
    case NV_ROT_XZY:
        transform(v, src, stride, n, dst, NV_ROT_XZY, lp);
        break;
    default:
        transform(v, src, stride, n, dst, 0, lp);
        break;
    }
}

void native_seg_load(uint32_t obj, uint32_t k, NativeSeg *s)
{
    const uint8_t *gm = g_mem;
    const uint32_t r = obj + k * 40u + 0x200u, o = obj + k * 12u + 0x104u;
    for (uint32_t j = 0; j < 19u; j++)
        s->m[j] = rt_ldf32(gm + r + 4u * j);
    for (uint32_t j = 0; j < 6u; j++)
        s->t[j] = rt_ldf32(gm + o + 4u * j);
}
