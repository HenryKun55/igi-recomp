/* Hand-decompiled terrain patch builder of IGI.exe.
 *
 *   0x00510010  patch_build   builds the terrain patches requested by the
 *                             octree walk (0x4D14E0) this frame
 *
 *   void patch_build(void);
 *
 * Each request (0xB16CE0, 0x30 bytes: origin, level, flags, packed source,
 * height and material sources, the patch's cache entry) gets a block from the
 * first of three pools with room, which goes to the frame's patch list. The
 * block receives the origin, the vertices decoded from the packed source
 * (6-bit offsets scaled by the level, mirrored by the flags, raised by the
 * bilinearly interpolated height map), then the per-material headers and two
 * vertex lists per material (texture coordinates from the material's planar
 * mappings, blended along the patch's edges), and the light map coordinates.
 * Vertex and material scratch records live in the guest frame. A block too
 * small for the result is moved to a pool of larger blocks (or the request is
 * dropped) and the old block returned to its pool.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float wherever the game stores a float,
 * conversions with the control word's rounding): the same operations in the
 * same order, so the results are bit-identical to the recompiled code
 * (tools/recomp/nativebench.py). The function makes no calls and no
 * comparisons on the x87; its frame (3 KiB, the scratch records included) is
 * left as the guest leaves it: every store to it is reproduced.
 *
 * Declined (left to the recompiled code): requests with no vertices or more
 * than the frame's 64 vertex records (the guest would loop for ever or
 * overwrite its return address), the direction flag set. */
#include "native.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_PATCH_BUILD   0x00510010u

#define G_REQUESTS       0x00B16CE0u   /* 0x30 bytes each */
#define G_REQUEST_COUNT  0x00B16CA4u
#define G_BUILT_TOTAL    0x00C31090u
#define G_PATCH_COUNT    0x00B16C9Cu
#define G_PATCH_LIST     0x00B12C60u
#define G_POOL0          0x00B12B60u   /* base, index array, used, capacity, stride, offset of the position */
#define G_POOL1          0x00B12C40u
#define G_POOL2          0x00B16CC0u
#define G_HEIGHTS        0x00BA2030u   /* 0x30 bytes: +0xC shift, +0x10 scale, +0x18/+0x20 origin, +0x28 map */
#define G_MATSRC         0x00BA5050u   /* 0x30 bytes: value, +0xC shift, +0x10 scale, +0x18/+0x20 origin, +0x28 map */
#define G_LIGHTS         0x00BA80B0u   /* 0x30 bytes */
#define G_MATERIALS      0x00C28FE4u   /* 900 bytes per material, 28 per level */
#define G_TEXSETS        0x00C30060u
#define G_TEXBYTES       0x00C30080u
#define G_ORIGIN_D       0x00B12BA0u   /* double[3] */
#define K_DIV            0x0054EBA0u   /* double */
#define K_ZERO           0x005333ECu
#define K_ONE            0x005333E0u
#define K_HALF           0x00533504u
#define K_UV             0x00534BE0u
#define K_HBIAS          0x00534AE0u   /* double */
#define K_HSCALE         0x005342E8u

#define FRAME_SIZE       0xC20u
#define MAX_VERTS        64u           /* vertex records at S+0x330, 0x24 bytes each */

#define GR8S(a) ((int32_t)(int8_t)gm[(uint32_t)(a)])
#define GR16S(a) ((int32_t)(int16_t)rt_ld16(gm + (uint32_t)(a)))
#define GR32(a) rt_ld32(gm + (uint32_t)(a))
#define GW32(a, v) rt_st32(gm + (uint32_t)(a), (uint32_t)(v))
#define GRF(a) rt_ldf32(gm + (uint32_t)(a))
#define GRD(a) rt_ldf64(gm + (uint32_t)(a))
#define GWF(a, v) stf(gm + (uint32_t)(a), (float)(v))
/* The guest frame (S: esp after the prologue). */
#define FW(off, v) GW32(S + (off), (v))
#define FWF(off, v) GWF(S + (off), (v))
#define FR(off) GR32(S + (off))
#define FRF(off) GRF(S + (off))

static inline void stf(uint8_t *p, float v) { memcpy(p, &v, 4); }
static inline void std64(uint8_t *p, double v) { memcpy(p, &v, 8); }
static inline void st64(uint8_t *p, int64_t v) { memcpy(p, &v, 8); }
static inline int32_t sar(uint32_t v, uint32_t n) { return (int32_t)v >> (n & 31u); }
static inline double u2d(uint32_t b) { float v; memcpy(&v, &b, 4); return (double)v; }
static inline uint32_t f2u(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }

/* A block from a pool: its position goes into the block, the pool's count up. */
static uint32_t pool_take(uint8_t *gm, uint32_t pool)
{
    uint32_t used = GR32(pool + 8u);
    uint32_t blk = GR32(GR32(pool + 4u) + used * 4u) * GR32(pool + 0x10u) + GR32(pool);
    GW32(GR32(pool + 0x14u) + blk, used);
    GW32(pool + 8u, GR32(pool + 8u) + 1u);
    return blk;
}

/* Returns blk to its pool: the pool's last block takes its position. */
static void pool_free(uint8_t *gm, uint32_t S, uint32_t pool, uint32_t blk)
{
    uint32_t last = GR32(pool + 8u) - 1u;
    uint32_t pos = GR32(GR32(pool + 0x14u) + blk);
    GW32(pool + 8u, last);
    if (pos == last)
        return;
    uint32_t idx = GR32(pool + 4u);
    uint32_t lblk = GR32(idx + last * 4u) * GR32(pool + 0x10u) + GR32(pool);
    uint32_t t = GR32(idx + pos * 4u);
    FW(0x30u, t);
    uint32_t lpos = GR32(lblk + GR32(pool + 0x14u));
    GW32(idx + pos * 4u, GR32(idx + lpos * 4u));
    GW32(GR32(pool + 4u) + lpos * 4u, t);
    GW32(lblk + GR32(pool + 0x14u), pos);
}

/* The frame's temporaries of the vertex loops (S+0x14, S+0x1C, S+0x24 and
 * the high words of the conversions at S+0x20 and S+0x28), written after
 * the loops: nothing reads them in between. */
typedef struct Tmp {
    uint32_t s14, s1c, s24, s20, s28;
} Tmp;

/* The height map of a request. */
typedef struct HMap {
    double ox, oy, scale, bias, hscale;
    uint32_t sh, base;
} HMap;

/* Height map value at (x, y): the guest's bilinear interpolation (the
 * result before its last rounding). */
static inline double height(Ctx *c, const uint8_t *gm, const HMap *hm, double x, double y, Tmp *t)
{
    double u = (x - hm->ox) * hm->scale;
    float vf = (float)((y - hm->oy) * hm->scale);
    int64_t qu = rt_fist64(c, u);
    int32_t iu = (int32_t)qu;
    int64_t qv = rt_fist64(c, (double)vf);
    int32_t iv = (int32_t)qv;
    t->s28 = (uint32_t)((uint64_t)qu >> 32);
    t->s20 = (uint32_t)((uint64_t)qv >> 32);
    const uint32_t sh = hm->sh & 31u, base = hm->base;
    double fu = u - (double)iu;
    uint32_t row = (uint32_t)iv + ((uint32_t)iv << sh);
    int32_t h00 = GR8S(base + row + (uint32_t)iu);
    int32_t h10 = GR8S(row + (uint32_t)iu + 1u + base);
    double fv = (double)vf - (double)iv;
    uint32_t row1 = ((uint32_t)(iv + 1) << sh) + (uint32_t)(iv + 1) + (uint32_t)(iu + 1);
    int32_t h01 = GR8S(base + row1 - 1u);
    int32_t h11 = GR8S(base + row1);
    float h10f = (float)h10, h01f = (float)h01;
    double a = ((double)h10f - (double)h00) * fu + (double)h00;
    float af = (float)a;
    t->s24 = (uint32_t)h11;
    t->s14 = f2u(h01f);
    t->s1c = f2u(af);
    double b = fu * ((double)h11 - (double)h01f) + (double)h01f;
    return ((b - (double)af) * fv + (double)af - hm->bias) * hm->hscale;
}

/* The decoded offsets of a packed vertex, mirrored by the flags (the frame's
 * S+0x14, S+0x1C, S+0x24 hold them as integers). */
static inline void unpack(uint32_t v, uint32_t flags, uint32_t f4, uint32_t f1, int32_t *ox, int32_t *oy,
                          int32_t *oz, Tmp *t)
{
    int32_t z = (sar(v, 14) & 0x3F) - 0x18;
    int32_t x = (sar(v, 20) & 0x3F) - 0x18;
    int32_t y = (sar(v, 26) & 0x3F) - 0x18;
    int32_t s1c = y, s14 = x;
    if (f4)
        y = -y, s1c = y;
    if (f1) {
        if (flags & 2u) {
            s1c = x;
            s14 = -y;
        } else {
            s1c = -x;
            s14 = y;
        }
    } else if (flags & 2u) {
        s1c = -y;
        s14 = -x;
    }
    t->s24 = (uint32_t)z;
    t->s1c = (uint32_t)s1c;
    t->s14 = (uint32_t)s14;
    *ox = s1c;
    *oy = s14;
    *oz = z;
}

/* Builds one request's patch; returns the guest's edx afterwards. */
static uint32_t build(Ctx *c, uint8_t *gm, uint32_t S, uint32_t req, uint32_t *f0_cache, double kdiv)
{
    const uint32_t lod = GR32(req + 0x18u), r24 = GR32(req + 0x24u), flags = GR32(req + 0x1Cu);
    FW(0x44u, lod);
    FW(0x2Cu, flags);
    memcpy(gm + S + 0x114u, gm + req, 0x18u);
    const uint32_t slot = GR32(req + 0x2Cu);
    const uint32_t f1 = flags & 1u;
    FW(0x60u, r24);
    const uint32_t hsrc = (r24 >> 16) & 0xFFu, light = r24 & 0xFFFFu;
    FW(0x94u, f1);
    FW(0x30u, slot);
    FW(0x18u, hsrc);

    /* A block from the first pool with room. */
    uint32_t pool, pn;
    if ((int32_t)GR32(G_POOL0 + 8u) <= (int32_t)(GR32(G_POOL0 + 0xCu) - 3u))
        pool = G_POOL0, pn = 0;
    else if ((int32_t)GR32(G_POOL1 + 8u) <= (int32_t)(GR32(G_POOL1 + 0xCu) - 3u))
        pool = G_POOL1, pn = 1;
    else if ((int32_t)GR32(G_POOL2 + 8u) <= (int32_t)(GR32(G_POOL2 + 0xCu) - 3u))
        pool = G_POOL2, pn = 2;
    else {
        uint32_t edx = GR32(G_POOL2 + 8u);
        GW32(slot + 0x10u, 0xFFFFFFFFu);
        return edx;
    }
    uint32_t blk = pool_take(gm, pool);
    GW32(slot + 0x18u, blk);
    GW32(slot + 0x14u, pn);
    uint32_t cnt = GR32(G_PATCH_COUNT);
    GW32(G_PATCH_LIST + cnt * 4u, GR32(slot + 0x18u));
    GW32(G_PATCH_COUNT, cnt + 1u);
    blk = GR32(slot + 0x18u);
    const uint32_t hdr = GR32(req + 0x20u);
    const uint32_t pk0 = (uint32_t)GR16S(hdr + 2u) + hdr + 8u;
    FW(0x64u, hdr);
    FW(0x70u, pk0);
    FW(0x12Cu, pk0);
    FW(0x60u, 0);
    const int lit = light != 0xFFFFu;
    if (lit) {
        FW(0x60u, 1);
        const uint32_t e = G_LIGHTS + light * 0x30u;
        FWF(0xF4u, GRF(e) * GRF(K_UV));
        FWF(0x144u, GRD(e + 8u));
        FWF(0x140u, GRD(e + 0x10u));
        FW(0x13Cu, GR32(e + 0x18u));
        FWF(0xDCu, GRF(e + 4u) * GRF(K_UV));
    }
    uint32_t tsrc = hsrc;
    if ((int32_t)lod < 0xE) {
        FW(0x18u, 0);
        tsrc = 0;
    } else if (hsrc) {
        const uint32_t e = G_HEIGHTS + hsrc * 0x30u;
        FWF(0xE8u, GRD(e + 0x18u));
        FW(0x7Cu, GR32(e + 0x28u));
        FWF(0xECu, GRD(e + 0x20u));
        FW(0x98u, GR32(e + 0x10u));
        FW(0x90u, GR32(e + 0xCu) + 3u);
    }
    /* Material sources (kept from the previous request when the same). */
    const uint32_t r28 = GR32(req + 0x28u);
    if (r28 != *f0_cache) {
        *f0_cache = r28;
        FW(0xF0u, r28);
        uint32_t m = r28;
        for (uint32_t k = 0; k < 4u; k++, m = (uint32_t)sar(m, 8)) {
            const uint32_t b = m & 0xFFu, rec = S + 0x148u + 0x18u * k;
            if (b) {
                const uint32_t p = G_MATSRC + b * 0x30u;
                GW32(rec + 0x14u, GR32(p));
                GW32(rec + 0x10u, GR32(p + 0x28u));
                GWF(rec + 0xCu, GRD(p + 0x20u));
                GWF(rec + 8u, GRD(p + 0x18u));
                GW32(rec + 4u, GR32(p + 0x10u));
                GW32(rec, GR32(p + 0xCu));
            } else {
                GW32(rec, 0);
            }
        }
        FW(0x78u, 0);
    }

    /* Block header and vertices. */
    const int32_t c1 = GR16S(hdr + 4u);
    const int32_t total = c1 + GR16S(hdr + 6u);
    FW(0x6Cu, c1);
    FW(0x3Cu, (uint32_t)total * 20u - (uint32_t)c1 * 8u);
    memcpy(gm + blk, gm + req, 0x18u);
    GW32(blk + 0x18u, ((uint32_t)c1 << 16) | (uint32_t)total);
    GW32(blk + 0x1Cu, lod);
    const uint32_t one_sh = 1u << ((0x1Bu - lod) & 31u);
    FW(0x48u, blk + 0x20u);
    FW(0x78u, total);
    FW(0x58u, 0);
    FW(0x24u, one_sh);
    const uint32_t f4 = flags & 4u;
    FW(0x34u, f4);
    const double scale = (double)(int32_t)one_sh;
    std64(gm + S + 0x50u, scale);
    const double o0 = GRD(S + 0x114u), o1 = GRD(S + 0x11Cu), o2 = GRD(S + 0x124u);
    const double zero = GRF(K_ZERO);
    HMap hm;
    if (tsrc) {
        hm.ox = FRF(0xE8u);
        hm.oy = FRF(0xECu);
        hm.scale = FRF(0x98u);
        hm.sh = FR(0x90u);
        hm.base = FR(0x7Cu);
        hm.bias = GRD(K_HBIAS);
        hm.hscale = GRF(K_HSCALE);
    }
    uint32_t pk = pk0, cur = blk + 0x30u;
    int32_t j = 0;
    Tmp t = { 0, 0, 0, 0, 0 };
    do {
        uint32_t v = GR32(pk);
        int32_t ix, iy, iz;
        unpack(v, flags, f4, f1, &ix, &iy, &iz, &t);
        double X = (double)ix * scale / kdiv + o0;
        double Y = (double)iy * scale / kdiv + o1;
        double Z = (double)iz * scale / kdiv + o2;
        double w = zero;
        if (tsrc) {
            double h = height(c, gm, &hm, X, Y, &t);
            float hf = (float)h;
            t.s14 = f2u(hf);
            if ((int32_t)lod > 0xE)
                Z = Z + (double)hf;
            else
                w = (double)hf;
        }
        GWF(cur - 4u, w);
        GWF(cur - 8u, Z);
        GWF(cur - 12u, Y);
        GWF(cur - 16u, X);
        pk += 4u;
        cur += 0x10u;
        j++;
    } while (j < c1);
    if (total != c1) {
        do {
            uint32_t v = GR32(pk);
            GW32(cur - 4u, v & 0x3Fu);
            int32_t ix, iy, iz;
            unpack(v, flags, f4, f1, &ix, &iy, &iz, &t);
            double X = (double)ix * scale / kdiv + o0;
            double Y = (double)iy * scale / kdiv + o1;
            double Z = (double)iz * scale / kdiv + o2;
            if (tsrc)
                Z = Z + height(c, gm, &hm, X, Y, &t);
            GWF(cur - 8u, Z);
            GWF(cur - 12u, Y);
            GWF(cur - 16u, X);
            pk += 4u;
            cur += 0x10u;
            j++;
        } while (j < total);
    }
    FW(0x14u, t.s14);
    FW(0x1Cu, t.s1c);
    FW(0x24u, t.s24);
    if (tsrc) {
        FW(0x20u, t.s20);
        FW(0x28u, t.s28);
    }
    FW(0x70u, pk);
    FW(0x58u, (uint32_t)j);

    /* Vertex records: light map coordinates, material, position relative to the origin. */
    cur -= 0x10u;
    FW(0x1Cu, cur);
    FW(0x58u, 0);
    FW(0x34u, 0);
    const double sF4 = FRF(0xF4u), sDC = FRF(0xDCu), s144 = FRF(0x144u), s140 = FRF(0x140u);
    uint32_t mask = 0, vp = blk + 0x20u, rec = S + 0x330u;
    /* Frame values of the last vertex (written after the loop; set: which were). */
    uint32_t ybits = 0, zbits = 0, f18 = 0, f14 = 0, f48 = 0, fset = 0;
    int64_t f50 = 0;
    j = 0;
    do {
        const double X = GRF(vp);
        ybits = GR32(vp + 4u);
        zbits = GR32(vp + 8u);
        const double Yf = u2d(ybits);
        GWF(rec + 0x1Cu, (X - s144) * sF4 + sDC);
        GWF(rec + 0x20u, (Yf - s140) * sF4 + sDC);
        uint32_t mat = 0, lo = 0xFFu;
        const uint32_t r0n = FR(0x148u);
        if (r0n) {
            double dx = X - FRF(0x150u);
            float dyf = (float)(Yf - FRF(0x154u));
            f18 = f2u(dyf);
            double sc = FRF(0x14Cu);
            int64_t q = rt_fist64(c, sc * dx);
            uint32_t ix = (uint32_t)q;
            f14 = ix;
            f48 = (uint32_t)sar(ix, 3);
            fset = 1;
            uint32_t bm = FR(0x158u);
            q = rt_fist64(c, sc * (double)dyf);
            f50 = q;
            uint32_t a = (uint32_t)sar(ix, 3) + bm + ((uint32_t)q << (r0n & 31u));
            uint32_t p = S + 0x15Cu;
            if (!((gm[a] >> (ix & 7u)) & 1u)) {
                for (;;) {
                    uint32_t n2 = GR32(p + 4u);
                    p += 8u;
                    if (!n2) {
                        p = 0;
                        break;
                    }
                    double sc2 = GRF(p);
                    p += 4u;
                    double dx2 = X - GRF(p);
                    float dy2 = (float)(Yf - GRF(p + 4u));
                    p += 8u;
                    f18 = f2u(dy2);
                    uint32_t bm2 = GR32(p);
                    p += 4u;
                    q = rt_fist64(c, dx2 * sc2);
                    uint32_t ix2 = (uint32_t)q;
                    f14 = ix2;
                    q = rt_fist64(c, (double)dy2 * sc2);
                    f50 = q;
                    uint32_t a2 = (uint32_t)sar(ix2, 3) + bm2 + ((uint32_t)q << (n2 & 31u));
                    if ((gm[a2] >> (ix2 & 7u)) & 1u)
                        break;
                }
            }
            if (p)
                mat = GR32(p);
            if (j >= c1) {
                uint32_t code = GR32(vp + 12u);
                GW32(rec, code);
                lo = (uint32_t)sar(GR32(S + code * 36u + 0x344u), 16);
                if (lo == mat)
                    lo = 0xFFu;
            }
        } else if (j >= c1) {
            GW32(rec, GR32(vp + 12u));
        }
        GWF(rec + 4u, X - o0);
        mask |= 1u << (mat & 31u);
        GWF(rec + 8u, Yf - o1);
        GWF(rec + 0xCu, u2d(zbits) - o2);
        GW32(rec + 0x14u, (mat << 16) | lo);
        vp += 0x10u;
        rec += 0x24u;
        j++;
    } while (j < total);
    FW(0x10Cu, ybits);
    FW(0x110u, zbits);
    FW(0x24u, 0xFFu);
    if (fset) {
        FW(0x18u, f18);
        FW(0x14u, f14);
        FW(0x48u, f48);
        st64(gm + S + 0x50u, f50);
    }
    FW(0x34u, mask);
    FW(0x58u, (uint32_t)j);

    /* Materials used. */
    std64(gm + S + 0x50u, o0 + GRD(G_ORIGIN_D));
    FW(0x14u, 0);
    FW(0x1C0u, 0);
    std64(gm + S + 0x70u, o1 + GRD(G_ORIGIN_D + 8u));
    std64(gm + S + 0x58u, o2 + GRD(G_ORIGIN_D + 16u));
    uint32_t nm = 0;
    if (mask) {
        uint32_t p = S + 0x1C0u, m = 0, b = mask;
        do {
            if (b & 1u) {
                GW32(p, m);
                p += 4u;
                nm++;
            }
            m++;
            b = (uint32_t)sar(b, 1);
        } while (b);
        FW(0x14u, nm);
    }
    const uint32_t stride = FR(0x3Cu) + 4u;
    FW(0x24u, stride);
    const int32_t size = (int32_t)(FR(0x3Cu) + (stride * nm + (uint32_t)total * 8u) * 2u + 0x3Cu);
    uint32_t ok = size < 0x640;
    if (!ok) {
        const uint32_t pool_now = GR32(slot + 0x14u);
        FW(0x34u, pool_now);
        if (pool_now == 1u)
            ok = size < 0xC80;
        else if (pool_now == 2u)
            ok = size < 0x1F40;
        if (!ok) {
            /* Too big for its block: move it to a pool of larger blocks. */
            FW(0x18u, 0);
            const uint32_t old = GR32(slot + 0x18u);
            uint32_t to = 0, tn = 0;
            if ((int32_t)pool_now < 1 && size < 0xC80 &&
                (int32_t)GR32(G_POOL1 + 8u) < (int32_t)(GR32(G_POOL1 + 0xCu) - 2u))
                to = G_POOL1, tn = 1;
            else if ((int32_t)pool_now < 2 && size < 0x1F40 &&
                     (int32_t)GR32(G_POOL2 + 8u) < (int32_t)(GR32(G_POOL2 + 0xCu) - 2u))
                to = G_POOL2, tn = 2;
            if (to) {
                const uint32_t len = cur - old;
                const uint32_t nb = pool_take(gm, to);
                GW32(slot + 0x18u, nb);
                memmove(gm + nb, gm + old, len);
                const uint32_t nblk = GR32(slot + 0x18u);
                GW32(slot + 0x14u, tn);
                cur = len + nblk;
                FW(0x18u, 1);
                GW32(G_PATCH_LIST - 4u + GR32(G_PATCH_COUNT) * 4u, nblk);
            }
            if (pool_now == 0)
                pool_free(gm, S, G_POOL0, old);
            else if (pool_now == 1u)
                pool_free(gm, S, G_POOL1, old);
            else if (pool_now == 2u)
                pool_free(gm, S, G_POOL2, old);
            if (!to) {
                GW32(slot + 0x18u, 0);
                GW32(slot + 0x10u, 0xFFFFFFFFu);
                GW32(G_PATCH_COUNT, GR32(G_PATCH_COUNT) - 1u);
                return slot;
            }
            nm = FR(0x14u);
        }
    }

    /* Header and the materials' texture mappings (records at S+0x240). */
    GW32(cur, FR(0x2Cu));
    GW32(cur + 4u, hdr + 8u);
    FW(0x2Cu, 0);
    GW32(cur + 8u, nm);
    GW32(cur + 12u, (uint32_t)GR16S(hdr));
    cur += 16u;
    const uint32_t sh1 = (0x17u - lod) & 31u, sh2 = (0x16u - lod) & 31u;
    FW(0x18u, 0x16u - lod);
    FW(0x1Cu, cur);
    FW(0x30u, lod * 28u);
    FW(0x48u, 0x17u - lod);
    const uint32_t kk = (uint32_t)sar(0x40000000u, lod);
    const double K = (double)(int32_t)kk;
    const double d50 = GRD(S + 0x50u), d70 = GRD(S + 0x70u), d58 = GRD(S + 0x58u);
    const double one = GRF(K_ONE), cuv = GRF(K_UV), half = GRF(K_HALF);
    {
        uint32_t lp = S + 0x1C0u, out = S + 0x268u;
        uint32_t m = 0;
        do {
            const uint32_t id = GR32(lp);
            lp += 4u;
            FW(0x64u, lp);
            const uint32_t mr = lod * 28u + id * 900u + G_MATERIALS;
            FW(0x9Cu, GR32(mr + 0x14u));
            const double A = GRF(mr + 0xCu);
            FW(0x3Cu, mr);
            int64_t q = rt_fist64(c, A * d50);
            const uint32_t e1 = (uint32_t)sar((uint32_t)q, sh1) & 0x7FFu;
            q = rt_fist64(c, A * d70);
            const uint32_t e2 = (uint32_t)sar((uint32_t)q, sh1) & 0x7FFu;
            const double B = FRF(0x9Cu);
            q = rt_fist64(c, B * d50);
            const uint32_t e3 = (uint32_t)sar((uint32_t)q, sh2) & 0x7FFu;
            q = rt_fist64(c, B * d70);
            st64(gm + S + 0x34u, q);
            const uint32_t e4 = (uint32_t)sar((uint32_t)q, sh2) & 0x7FFu;
            FW(0x34u, e4);
            const double hA = A * half / K;
            uint32_t b1 = gm[(e1 >> 8) + ((e2 >> 8) + GR32(mr + 0x10u) * 8u) * 8u + G_TEXBYTES];
            GW32(out - 4u, b1);
            const uint32_t r18 = GR32(mr + 0x18u);
            uint32_t b2 = gm[(e3 >> 8) + ((e4 >> 8) + r18 * 8u) * 8u + G_TEXBYTES];
            GW32(out - 8u, b2);
            GWF(out - 12u, hA);
            GWF(out - 16u, ((double)(int32_t)(e1 & 0xFFu) + one) * cuv);
            GWF(out - 20u, ((double)(int32_t)(e2 & 0xFFu) + one) * cuv);
            q = rt_fist64(c, A * d58);
            const uint32_t e5 = (uint32_t)sar((uint32_t)q, sh1) & 0xFFu;
            GWF(out - 24u, ((double)(int32_t)e5 + one) * cuv);
            GWF(out - 28u, B / K);
            GWF(out - 32u, ((double)(int32_t)(e3 & 0xFFu) + one) * cuv);
            GWF(out - 36u, ((double)(int32_t)(e4 & 0xFFu) + one) * cuv);
            q = rt_fist64(c, B * d58);
            st64(gm + S + 0x3Cu, q);
            const uint32_t e6 = (uint32_t)sar((uint32_t)q, sh2) & 0xFFu;
            FW(0x3Cu, e6);
            FW(0x2Cu, m + 1u);
            GWF(out - 40u, ((double)(int32_t)e6 + one) * cuv);
            out += 40u;
            m++;
        } while ((int32_t)m < (int32_t)nm);
    }
    uint32_t edx = nm;

    /* Two vertex lists per material. */
    if ((int32_t)nm > 0) {
        uint32_t lp = S + 0x1C0u, mr = S + 0x240u;
        uint32_t left = nm;
        FW(0x2Cu, nm);
        FW(0x18u, lp);
        do {
            const double r0 = GRF(mr), r1 = GRF(mr + 4u), r2 = GRF(mr + 8u), r3 = GRF(mr + 12u),
                         r4 = GRF(mr + 16u);
            FW(0x58u, 0);
            const uint32_t r5b = GR32(mr + 20u), r6b = GR32(mr + 24u), r7b = GR32(mr + 28u);
            FW(0xC0u, r5b);
            FW(0xBCu, r6b);
            FW(0x4Cu, r7b);
            const double r5 = u2d(r5b), r6 = u2d(r6b), r7 = u2d(r7b);
            FW(0x14u, (uint32_t)total);
            const uint32_t mid = GR32(lp);
            FW(0x30u, mid);
            const uint32_t sets = GR32(G_TEXSETS);
            uint32_t l1 = cur, l2 = cur + stride;
            GW32(l1, sets + GR32(mr + 32u) * 36u + 0xCu);
            const uint32_t h2 = sets + GR32(mr + 36u) * 36u + 0xCu;
            uint32_t pkp = FR(0x12Cu);
            FW(0x70u, pkp);
            cur = cur + 2u * stride;
            FW(0x1Cu, cur);
            l1 += 4u;
            uint32_t vr = S + 0x334u;
            FW(0x44u, vr);
            GW32(l2, h2);
            l2 += 4u;
            int32_t jj = 0;
            uint32_t remain = (uint32_t)total;
            /* The frame's temporaries of the last vertex, written after the
             * loop (e4, f68, f80: kept when no vertex wrote them). */
            uint32_t b89 = 0, hi = 0, xb = 0, yb = 0, zb = 0, ref = 0, rxb = 0, ryb = 0, rzb = 0;
            uint32_t fe4 = FR(0xE4u), f68 = FR(0x68u), f80 = FR(0x80u);
            float sF8 = 0, sC8 = 0, sC4 = 0, s104 = 0, sCC = 0, sD0 = 0;
            do {
                const uint32_t pkv = GR32(pkp), rm = GR32(vr + 0x10u);
                b89 = (uint32_t)sar(pkv, 8) & 3u;
                hi = (uint32_t)sar(rm, 16);
                xb = GR32(vr);
                yb = GR32(vr + 4u);
                zb = GR32(vr + 8u);
                const uint32_t lo = rm & 0xFFu;
                const uint32_t b67 = (uint32_t)sar(pkv, 6) & 3u;
                uint32_t rp;
                if (jj >= c1) {
                    ref = GR32(vr - 4u);
                    rp = S + ref * 36u + 0x334u;
                } else {
                    ref = (uint32_t)jj;
                    rp = vr;
                }
                rxb = GR32(rp);
                ryb = GR32(rp + 4u);
                rzb = GR32(rp + 8u);
                const double x = u2d(xb), y = u2d(yb), z = u2d(zb), rx = u2d(rxb), ry = u2d(ryb),
                             rz = u2d(rzb);
                double U, V;
                if (b89) {
                    double e, s68;
                    if ((b89 ^ f1) & 1u) {
                        fe4 = yb;
                        f80 = ryb;
                        e = y;
                        s68 = r1;
                    } else {
                        fe4 = xb;
                        f80 = rxb;
                        e = x;
                        s68 = r2;
                    }
                    const float s68f = (float)s68;
                    f68 = f2u(s68f);
                    U = z * r3 + r0;
                    sF8 = (float)(e * r3 + (double)s68f);
                    sC8 = (float)((rz * r3 + r0) - U);
                    V = u2d(f80) * r3 + (double)s68f;
                } else {
                    U = y * r3 + r1;
                    sF8 = (float)(x * r3 + r2);
                    sC8 = (float)((ry * r3 + r1) - U);
                    V = rx * r3 + r2;
                }
                sC4 = (float)(V - (double)sF8);
                double W1, V2;
                if (b67) {
                    double e;
                    if ((b67 ^ f1) & 1u) {
                        f80 = ryb;
                        e = y;
                        f68 = r5b;
                    } else {
                        f80 = rxb;
                        e = x;
                        f68 = r6b;
                    }
                    const double s68 = u2d(f68);
                    W1 = e * r7 + s68;
                    s104 = (float)(z * r7 + r4);
                    sCC = (float)((u2d(f80) * r7 + s68) - W1);
                    V2 = rz * r7 + r4;
                } else {
                    W1 = x * r7 + r6;
                    s104 = (float)(y * r7 + r5);
                    sCC = (float)((rx * r7 + r6) - W1);
                    V2 = ry * r7 + r5;
                }
                sD0 = (float)(V2 - (double)s104);
                uint32_t fb = 0;
                if (mid == lo)
                    fb = 8u;
                else if (mid == hi)
                    fb = lo != 0xFFu ? 0x10u : 0x18u;
                const uint32_t packed = ((ref << 5) + fb) | (ref << 16);
                GW32(l1, packed);
                GW32(l1 + 4u, f2u(sF8));
                GWF(l1 + 8u, U);
                l1 += 12u;
                GW32(l2, packed + 4u);
                GWF(l2 + 4u, W1);
                GW32(l2 + 8u, f2u(s104));
                l2 += 12u;
                if (jj >= c1) {
                    GW32(l1, f2u(sC4));
                    GW32(l1 + 4u, f2u(sC8));
                    l1 += 8u;
                    GW32(l2, f2u(sCC));
                    GW32(l2 + 4u, f2u(sD0));
                    l2 += 8u;
                }
                pkp += 4u;
                vr += 0x24u;
                jj++;
                remain--;
            } while (remain);
            FW(0x64u, b89);
            FW(0x3Cu, hi);
            FW(0xA4u, xb);
            FW(0xA8u, yb);
            FW(0xACu, zb);
            FW(0x34u, ref);
            FW(0x84u, rxb);
            FW(0x88u, ryb);
            FW(0x8Cu, rzb);
            FW(0xE4u, fe4);
            FW(0x68u, f68);
            FW(0x80u, f80);
            FWF(0xF8u, sF8);
            FWF(0xC8u, sC8);
            FWF(0xC4u, sC4);
            FWF(0x104u, s104);
            FWF(0xCCu, sCC);
            FWF(0xD0u, sD0);
            FW(0x44u, vr);
            FW(0x70u, pkp);
            FW(0x58u, (uint32_t)jj);
            FW(0x14u, 0);
            edx = 4u;
            lp += 4u;
            FW(0x18u, lp);
            left--;
            FW(0x2Cu, left);
            mr += 40u;
        } while (left);
    }

    /* Light map coordinates. */
    if (!FR(0x60u)) {
        GW32(cur, 0);
        return edx;
    }
    GW32(cur, FR(0x13Cu));
    cur += 4u;
    FW(0x60u, (uint32_t)total);
    uint32_t left = (uint32_t)total, r = S + 0x330u;
    int32_t k = 0;
    do {
        const uint32_t d4 = GR32(r + 0x1Cu), d8 = GR32(r + 0x20u);
        FW(0xD4u, d4);
        FW(0xD8u, d8);
        const uint32_t code = k >= c1 ? GR32(r) : (uint32_t)k;
        const uint32_t rr = S + code * 36u;
        const uint32_t b134 = GR32(rr + 0x34Cu), b138 = GR32(rr + 0x350u);
        FW(0x134u, b134);
        FW(0x138u, b138);
        cur += 12u;
        const float d1 = (float)(u2d(b138) - u2d(d8));
        GW32(cur - 12u, (code << 5) | (code << 16));
        GW32(cur - 8u, d4);
        const float d0 = (float)(u2d(b134) - u2d(d4));
        GW32(cur - 4u, d8);
        if (k >= c1) {
            stf(gm + cur, d0);
            stf(gm + cur + 4u, d1);
            cur += 8u;
        }
        left--;
        FW(0x60u, left);
        r += 0x24u;
        k++;
    } while (left);
    return (uint32_t)k;
}

int rt_native_patch_build(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_PATCH_BUILD);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    if (c->df)
        return 0;
    const uint32_t n = GR32(G_REQUEST_COUNT);
    for (uint32_t i = 0; (int32_t)i < (int32_t)n; i++) {
        const uint32_t hdr = GR32(G_REQUESTS + 0x30u * i + 0x20u);
        const int32_t total = GR16S(hdr + 4u) + GR16S(hdr + 6u);
        if (total <= 0 || total > (int32_t)MAX_VERTS)
            return 0;
    }
    /* push ebp; mov ebp, esp; and esp, -8; sub esp, 0xC20; push ebx, ebp, esi, edi. */
    const uint32_t E = esp - 4u;
    const uint32_t A = (E & ~7u) - FRAME_SIZE;
    const uint32_t S = A - 0x10u;
    GW32(E, c->r[5]);
    GW32(A - 4u, c->r[3]);
    GW32(A - 8u, E);
    GW32(A - 12u, c->r[6]);
    GW32(A - 16u, c->r[7]);
    FW(0xB0u, GR32(K_DIV));
    FW(0xB4u, GR32(K_DIV + 4u));
    GW32(G_BUILT_TOTAL, GR32(G_BUILT_TOTAL) + n);
    FW(0x1A8u, 0);
    FW(0xF0u, 0xFFFFFFFFu);
    FW(0xA0u, G_REQUESTS);
    FW(0x130u, n);
    FW(0xE0u, 0);
    uint32_t edx = c->r[2];
    ecx = 0;
    if ((int32_t)n > 0) {
        const double kdiv = GRD(S + 0xB0u);
        uint32_t f0 = 0xFFFFFFFFu;
        for (uint32_t i = 0; (int32_t)i < (int32_t)n; i++) {
            edx = build(c, gm, S, G_REQUESTS + 0x30u * i, &f0, kdiv);
            FW(0xE0u, i + 1u);
            FW(0xA0u, G_REQUESTS + 0x30u * (i + 1u));
        }
        ecx = n;
    }
    c->r[1] = ecx;
    c->r[2] = edx;
    *ret = (uint64_t)(esp + 4u) << 32 | n;
    return 1;
}
