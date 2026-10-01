/* Hand-decompiled screen-space polygon recorder of IGI.exe.
 *
 *   0x0050D4C0  poly2d_record   copies a polygon of pre-transformed vertices
 *                               into a draw record: screen position (pixel
 *                               centre at the half), depth and 1/w, packed
 *                               colour and scaled texture coordinates
 *
 *   int poly2d_record(const Poly2D *p);   (cdecl)
 *
 * Poly2D: +0 material, +4 flags, +8 queue (negative: the record goes to the
 * static buffer at 0xA7CEDC and is drawn now), +0x0C vertex count, +0x10
 * vertices (0x24 bytes: x, y, z, u, v, alpha, r, g, b). Flags 0x60000 (unless
 * 0x40000000) with depth sorting on (0xA84A4C) allocate the record from the
 * sorted list (0x4B72E0, keyed by the first vertex's z), else from 0x497800;
 * the record is then drawn (0x497860) or queued (0x4978B0) unless it was
 * sorted. Record: +4 the value at 0x54E890, +0x0C texture (0xA84A50), +0x10
 * (0xA84A58), +0x14 flags, +0x18 alpha byte, +0x1C count, +0x20 material,
 * +0x24 vertex pointer, then the vertices (0x1C bytes: x, y, z, 1/w, colour,
 * u, v). Vertices in front (z > 0) get a depth from the fog/depth range when
 * enabled (0x6E5BB0), else a constant 0.5 and 1/w of the far distance.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float wherever the game stores a float),
 * with the control word's rounding for fistp: the same operations in the same
 * order, so the results are bit-identical to the recompiled code
 * (tools/recomp/nativebench.py). The guest calls see the same registers,
 * arguments and x87 status word, and the frame the guest leaves below its
 * entry esp (saved registers, call arguments, the fistp temporaries, the loop
 * counters) and the value it leaves in its argument slot are reproduced.
 *
 * Assumed (the guest data always satisfies it): the record does not overlap
 * the polygon, its vertices or the stack. */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_POLY2D        0x0050D4C0u
#define FN_SORTED_ALLOC  0x004B72E0u   /* (callback, size, key) */
#define FN_ALLOC         0x00497800u   /* (size) */
#define FN_DRAW          0x00497860u   /* (record) */
#define FN_QUEUE         0x004978B0u   /* (record, queue) */
#define CB_SORTED        0x0050D7E0u
#define RET_SORTED_ALLOC 0x0050D511u
#define RET_ALLOC        0x0050D545u
#define RET_DRAW         0x0050D7BBu
#define RET_QUEUE        0x0050D7CAu

#define G_SORTING        0x00A84A4Cu
#define G_TEXTURE        0x00A84A50u
#define G_STATE          0x00A84A58u
#define G_STAMP          0x0054E890u
#define G_STATIC_REC     0x00A7CEDCu
#define G_DEPTH_ON       0x006E5BB0u
#define G_NEAR           0x00BCADA0u
#define G_FAR            0x00BCADC8u
#define G_Z_SCALE        0x00BCABECu
#define G_Z_MUL          0x00BCAB6Cu
#define G_Z_LO           0x00BCADB4u
#define G_Z_HI           0x00BCAD8Cu
#define C_HALF           0x00533504u   /* 0.5f */
#define C_ZERO           0x005333ECu   /* 0.0f */
#define C_ONE            0x005333E0u   /* 1.0f */
#define C_NEAR_SCALE     0x005342DCu   /* 1.01f */
#define C_255            0x005336D4u   /* 255.0f */
#define C_M255           0x005342D8u   /* -255.0f */
#define C_QUARTER        0x00533518u   /* 0.25f */
#define C_TEX_SCALE      0x00534BBCu   /* 1 / 1024.0f */

#define COLOR_FLAGS      0x10880024u

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }

/* fistp qword [tmp] of v; returns the low word. */
static inline uint32_t fist_tmp(Ctx *c, uint8_t *gm, uint32_t tmp, double v)
{
    int64_t q = rt_fist64(c, v);
    rt_st64(gm + tmp, (uint64_t)q);
    return (uint32_t)q;
}

/* rt_fist64 with the rounding control (fcw & 0xC00) loaded once. */
static inline int64_t fist(const Ctx *c, uint32_t mode, double v)
{
    if (mode == 0u) {
        uint64_t b = rt_round_bits(v);
        if ((uint32_t)(b >> 52) == 0x433u)
            return (int64_t)(b - RT_ROUND_MAGIC_BITS);
    } else if (mode == 0x0C00u && RT_FIST_SMALL(v)) {
        return (int32_t)v;
    }
    return rt_fist64_slow(c, v);
}

int rt_native_poly2d(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_POLY2D);
    uint8_t *const gm = g_mem;
    /* Frame (E = entry esp): flag at E-0x0C, fistp temporary / loop counter at
     * E-0x14, vertex offset at E-0x18, saved esi E-0x1C, edi E-0x20, ebx E-0x24,
     * ebp E-0x28, colour temporary at E-8; the argument slot E+4 is reused. */
    const uint32_t E = esp;
    const uint32_t esi0 = c->r[6], edi0 = c->r[7];
    const uint32_t p = rt_ld32(gm + E + 4u);
    uint32_t edx = c->r[2];
    rt_st32(gm + E - 0x1Cu, esi0);
    rt_st32(gm + E - 0x20u, edi0);
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);

    uint32_t flags = rt_ld32(gm + p + 4u);
    uint32_t rec;
    c->r[7] = p;
    if ((flags & 0x60000u) && rt_ld32(gm + G_SORTING) && !(flags & 0x40000000u)) {
        rt_st32(gm + E - 0x0Cu, 1);
        uint32_t key = rt_ld32(gm + rt_ld32(gm + p + 0x10u) + 8u);
        uint32_t n = rt_ld32(gm + p + 0x0Cu);
        edx = n * 7u;
        uint32_t size = edx * 4u + 0x28u;
        rt_st32(gm + E - 0x24u, key);
        rt_st32(gm + E - 0x28u, size);
        rt_st32(gm + E - 0x2Cu, CB_SORTED);
        rt_st32(gm + E - 0x30u, RET_SORTED_ALLOC);
        c->r[2] = edx;
        c->fsw = native_fsw(&fsw);
        rec = (uint32_t)f_004B72E0(c, E - 0x30u, key, size);
        ecx = c->r[1];
        edx = c->r[2];
        native_fsw_sync(&fsw, c);
    } else {
        rt_st32(gm + E - 0x0Cu, 0);
        eax = rt_ld32(gm + p + 8u);
        if ((int32_t)eax < 0) {
            rec = G_STATIC_REC;
        } else {
            uint32_t n = rt_ld32(gm + p + 0x0Cu);
            ecx = n * 7u;
            edx = ecx * 4u + 0x28u;
            rt_st32(gm + E - 0x24u, edx);
            rt_st32(gm + E - 0x28u, RET_ALLOC);
            c->r[2] = edx;
            c->fsw = native_fsw(&fsw);
            rec = (uint32_t)f_00497800(c, E - 0x28u, ecx, n);
            ecx = c->r[1];
            edx = c->r[2];
            native_fsw_sync(&fsw, c);
        }
    }
    eax = rec;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5];

    if (rec) {
        rt_st32(gm + rec + 4u, rt_ld32(gm + G_STAMP));
        rt_st32(gm + rec + 0x0Cu, rt_ld32(gm + G_TEXTURE));
        rt_st32(gm + rec + 0x10u, rt_ld32(gm + G_STATE));
        rt_st32(gm + rec + 0x14u, rt_ld32(gm + p + 4u));
        uint32_t n = rt_ld32(gm + p + 0x0Cu);
        rt_st32(gm + rec + 0x1Cu, n);
        rt_st32(gm + rec + 0x20u, rt_ld32(gm + p));
        uint32_t src = rt_ld32(gm + p + 0x10u);
        edx = rec + 0x28u;
        rt_st32(gm + rec + 0x24u, edx);
        uint32_t a = fist_tmp(c, gm, E - 0x14u, ldf(gm, src + 0x14u) * ldf(gm, C_255));
        rt_st32(gm + E - 0x14u, 0);
        gm[rec + 0x18u] = (uint8_t)a;
        ecx = (n & 0xFFFFFF00u) | (a & 0xFFu);
        eax = rt_ld32(gm + rec + 0x1Cu);
        if ((int32_t)eax > 0) {
            rt_st32(gm + E - 0x24u, ebx0);
            rt_st32(gm + E - 0x28u, ebp0);
            /* Loop invariants: the guest reloads them, but the vertices written
             * overlap neither the record's header, the polygon nor the globals. */
            const double half = ldf(gm, C_HALF), zero = ldf(gm, C_ZERO);
            const double m255 = ldf(gm, C_M255), p255 = ldf(gm, C_255);
            const uint32_t depth_on = rt_ld32(gm + G_DEPTH_ON);
            const uint32_t dst = rt_ld32(gm + rec + 0x24u);
            const uint32_t fl = rt_ld32(gm + rec + 0x14u), texr = rt_ld32(gm + rec + 0x0Cu);
            const uint32_t cnt = eax;
            const uint32_t mode = c->fcw & 0x0C00u;
            double tu = 0.0, tv = 0.0, tex = 0.0;
            if (texr) {
                tu = ldf(gm, texr + 0x10u);
                tv = ldf(gm, texr + 0x14u);
                tex = ldf(gm, C_TEX_SCALE);
            }
            int64_t q = 0;                  /* last fistp (the colour temporary) */
            int have_b = 0;
            float last_b = 0.0f;
            uint32_t i = 0;
            for (;;) {
                const uint32_t s = src + 0x24u * i;
                uint8_t *const d = gm + dst + 0x1Cu * i;
                float o[7];
                o[0] = (float)(ldf(gm, s) - half);
                o[1] = (float)(ldf(gm, s + 4u) - half);
                const double z = ldf(gm, s + 8u);
                native_fcom(&fsw, z, zero);
                if (X87_ABOVE(z, zero) && depth_on) {
                    const double one = ldf(gm, C_ONE), nearv = ldf(gm, G_NEAR), farv = ldf(gm, G_FAR);
                    const double zmul = ldf(gm, G_Z_MUL), zlo = ldf(gm, G_Z_LO), zhi = ldf(gm, G_Z_HI);
                    const double zscale = ldf(gm, G_Z_SCALE);
                    double v = nearv * ldf(gm, C_NEAR_SCALE);
                    const float bf = (float)(zscale * z);
                    const double b = (double)bf;
                    last_b = bf;
                    have_b = 1;
                    native_fcom(&fsw, v, b);
                    if (!X87_ABOVE(v, b)) {
                        native_fcom(&fsw, farv, b);
                        v = X87_BELOW(farv, b) ? farv : b;
                    }
                    const double r = one / v;
                    o[3] = (float)r;
                    const double t = (one - r * nearv) * zmul;
                    o[2] = (float)((zhi - zlo) * t + zlo);
                } else {
                    o[2] = 0.5f;
                    o[3] = (float)(ldf(gm, C_ONE) / ((ldf(gm, G_FAR) - ldf(gm, G_NEAR)) * half));
                }
                uint32_t col;
                if (fl & COLOR_FLAGS) {
                    double alpha = (fl & 4u) ? half : (fl & 0x20u) ? ldf(gm, C_QUARTER) : ldf(gm, s + 0x14u);
                    uint32_t acc = (uint32_t)fist(c, mode, alpha * m255) << 8;
                    acc = (acc + (uint32_t)fist(c, mode, ldf(gm, s + 0x18u) * m255)) << 8;
                    acc = (acc + (uint32_t)fist(c, mode, ldf(gm, s + 0x1Cu) * m255)) << 8;
                    q = fist(c, mode, ldf(gm, s + 0x20u) * p255);
                    col = (uint32_t)q - acc;
                } else {
                    col = (0xFF00u - (uint32_t)fist(c, mode, ldf(gm, s + 0x18u) * m255)) << 8;
                    col = (col - (uint32_t)fist(c, mode, ldf(gm, s + 0x1Cu) * m255)) << 8;
                    q = fist(c, mode, ldf(gm, s + 0x20u) * m255);
                    col -= (uint32_t)q;
                }
                memcpy(o + 4, &col, 4);
                if (texr) {
                    o[5] = (float)(tu * ldf(gm, s + 0x0Cu) + tex);
                    o[6] = (float)(tv * ldf(gm, s + 0x10u) + tex);
                    memcpy(d, o, 28);
                } else {
                    memcpy(d, o, 20);
                }
                if ((int32_t)++i >= (int32_t)cnt)
                    break;
            }
            ecx = cnt;
            const uint32_t doff = 0x1Cu * i;
            if (have_b)
                memcpy(gm + E + 4u, &last_b, 4);
            rt_st64(gm + E - 8u, (uint64_t)q);
            rt_st32(gm + E - 0x14u, i);
            rt_st32(gm + E - 0x18u, doff);
            eax = i;
            edx = doff;
        }
    }

    c->r[3] = ebx0;
    c->r[5] = ebp0;
    uint64_t rv;
    if (rt_ld32(gm + E - 0x0Cu)) {
        rv = ((uint64_t)(E + 4u) << 32) | rt_ld32(gm + E - 0x0Cu);
        c->r[1] = ecx;
        c->r[2] = edx;
    } else {
        eax = rt_ld32(gm + p + 8u);
        rt_st32(gm + E - 0x24u, rec);
        c->r[2] = edx;
        c->r[6] = rec;
        c->fsw = native_fsw(&fsw);
        if ((int32_t)eax < 0) {
            rt_st32(gm + E - 0x28u, RET_DRAW);
            rv = f_00497860(c, E - 0x28u, ecx, eax);
        } else {
            rt_st32(gm + E - 0x28u, eax);
            rt_st32(gm + E - 0x2Cu, RET_QUEUE);
            rv = f_004978B0(c, E - 0x2Cu, ecx, eax);
        }
        rv = ((uint64_t)(E + 4u) << 32) | (uint32_t)rv;
        *ret = rv;
        c->r[7] = rt_ld32(gm + E - 0x20u);
        c->r[6] = rt_ld32(gm + E - 0x1Cu);
        return 1;
    }
    c->fsw = native_fsw(&fsw);
    c->r[6] = rt_ld32(gm + E - 0x1Cu);
    c->r[7] = rt_ld32(gm + E - 0x20u);
    *ret = rv;
    return 1;
}
