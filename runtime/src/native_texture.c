/* Hand-decompiled texture loading loops of IGI.exe (the mission load decodes
 * every texture into 32-bit ARGB, builds its mip levels and converts them to
 * the surface format; in Vita3K this was a third of the load's CPU time).
 *
 *   0x00517B00  mip_halve      one mip level: 2x2 box filter of 32-bit ARGB
 *   0x00517C90  expand_1555    16-bit 1555 texels (a rectangle of them) to ARGB
 *   0x00517FD0  expand_pal8    8-bit palette indices (and an optional alpha
 *                              plane) to ARGB
 *
 *   void mip_halve(uint32_t *dst, const uint32_t *src, int w, int h);
 *   void expand_1555(uint32_t *dst, int dw, int dh, const uint16_t *src, int sw, int sh, const Rect *r);
 *   void expand_pal8(uint32_t *dst, int dw, int dh, const uint8_t *alpha, const uint8_t *src,
 *                    const uint8_t *pal, int sw, int sh, const Rect *r);          (all cdecl)
 *
 * The expanders read the source rectangle r (x, y, w, h; NULL: all of it),
 * clipped to the source, into a dw x dh destination; texels outside it are 0.
 * Like every native function (runtime/include/native.h) these leave guest
 * memory, the frame and argument slots they reuse, and eax, ecx, edx as the
 * recompiled code does. Destinations that overlap their sources, the frame or
 * the rectangle, and sizes past guest memory, are left to the recompiled code. */
#include "native.h"

#define FN_MIP_HALVE    0x00517B00u
#define FN_EXPAND_1555  0x00517C90u
#define FN_EXPAND_PAL8  0x00517FD0u

/* Guest memory as unaligned 16 / 32-bit words (loops the compiler can vectorize). */
typedef uint32_t u32_un __attribute__((aligned(1), may_alias));
typedef uint16_t u16_un __attribute__((aligned(1), may_alias));

static inline int overlaps(uint32_t a, uint64_t an, uint32_t b, uint64_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

static inline int in_memory(uint32_t a, uint64_t n)
{
    return a >= GUEST_NULL_GUARD && (uint64_t)a + n <= g_guest_mem_size;
}

/* ---- 0x517B00 ----------------------------------------------------------------
 * dst gets (w/2) x (h/2) texels; source rows are w texels. Per channel the sum
 * of the four texels plus 2, divided by 4; the alpha byte adds 2 << 22 before
 * the division instead, and blue (the low byte) keeps no mask: the guest's
 * rounding, reproduced bit for bit. Frame (esp0 = entry esp): -4 w*8, -8 w/2,
 * -0xc end of the destination row, -0x10 / -0x14 the two source row pointers,
 * -0x18.. the saved ebx, ebp, esi, edi; the argument slots become the row count
 * (+4), the second (+8) and first (+0xc) source row and the destination (+0x10). */
static inline uint32_t mip_texel(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t g = ((c & 0xFF00u) + (b & 0xFF00u) + (a & 0xFF00u) + (d & 0xFF00u) + 2u) & 0x3FC00u;
    uint32_t r = ((d & 0xFF0000u) + (c & 0xFF0000u) + (b & 0xFF0000u) + (a & 0xFF0000u) + 2u) & 0x3FC0000u;
    uint32_t bl = (d & 0xFFu) + (c & 0xFFu) + (b & 0xFFu) + (a & 0xFFu) + 2u;
    uint32_t al = (((d >> 2) & 0x3FC00000u) + ((c >> 2) & 0x3FC00000u) + ((b >> 2) & 0x3FC00000u) +
                   ((a >> 2) & 0x3FC00000u) + 0x800000u) & 0xFF000000u;
    return ((g | r | bl) >> 2) | al;
}

int rt_native_mip_halve(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_MIP_HALVE);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    const uint32_t dst = rt_ld32(gm + esp + 4u), src = rt_ld32(gm + esp + 8u);
    const int32_t w = (int32_t)rt_ld32(gm + esp + 12u), h = (int32_t)rt_ld32(gm + esp + 16u);
    const int32_t w2 = w >> 1, h2 = h >> 1;
    if (h2 > 0 && w2 > 0) {
        uint64_t dn = (uint64_t)(uint32_t)w2 * (uint32_t)h2 * 4u;
        uint64_t sn = (uint64_t)(uint32_t)w * (uint32_t)(2 * h2) * 4u;
        if (w2 > 0x100000 || h2 > 0x100000 || !in_memory(dst, dn) || !in_memory(src, sn) ||
            overlaps(dst, dn, src, sn) || overlaps(dst, dn, esp - 0x24u, 0x38u) ||
            overlaps(src, sn, esp - 0x24u, 0x38u))
            return 0;
    }
    rt_st32(gm + esp - 8u, (uint32_t)w2);
    rt_st32(gm + esp - 0x18u, c->r[3]);   /* push ebx */
    if (h2 <= 0) {
        c->r[1] = (uint32_t)w;
        *ret = ((uint64_t)(esp + 4u) << 32) | (uint32_t)h2;
        return 1;
    }
    rt_st32(gm + esp - 0x1Cu, c->r[5]);   /* push ebp, esi, edi */
    rt_st32(gm + esp - 0x20u, c->r[6]);
    rt_st32(gm + esp - 0x24u, c->r[7]);
    const uint32_t pitch = (uint32_t)w * 4u;
    rt_st32(gm + esp - 4u, (uint32_t)w * 8u);
    uint32_t row0 = src, row1 = src + pitch, d = dst;
    for (int32_t y = 0; y < h2; y++) {
        const uint32_t end = d + (uint32_t)w2 * 4u;
        const uint8_t *s0 = gm + row0, *s1 = gm + row1;
        uint8_t *o = gm + d;
        for (int32_t x = 0; x < w2; x++, s0 += 8, s1 += 8, o += 4)
            rt_st32(o, mip_texel(rt_ld32(s0), rt_ld32(s0 + 4), rt_ld32(s1), rt_ld32(s1 + 4)));
        if (w2 > 0) {
            rt_st32(gm + esp - 0x14u, row0 + (uint32_t)w2 * 8u);
            rt_st32(gm + esp - 0x10u, row1 + (uint32_t)w2 * 8u);
        }
        d = end;
        row0 += (uint32_t)w * 8u;
        row1 += (uint32_t)w * 8u;
    }
    const uint32_t last_end = dst + (uint32_t)h2 * (uint32_t)w2 * 4u;
    rt_st32(gm + esp - 0xCu, last_end);
    rt_st32(gm + esp + 4u, 0);
    rt_st32(gm + esp + 8u, row1);
    rt_st32(gm + esp + 12u, row0);
    rt_st32(gm + esp + 16u, w2 > 0 ? last_end : dst);
    c->r[1] = last_end;
    c->r[2] = last_end;
    *ret = ((uint64_t)(esp + 4u) << 32) | 0u;
    return 1;
}

/* ---- 0x517C90 ----------------------------------------------------------------
 * A texel: alpha 0xFF if bit 15, else 0; red bits 10-14, green 5-9, blue 0-4,
 * each shifted to the top of its byte. Frame: -4 the source pitch in bytes (the
 * saved ecx's slot, popped into ecx), -8.. the saved ebx, esi, edi, ebp; the
 * argument slots become the destination row + 2 (+4), the source row (+0x10),
 * the clipped width (+0x14) and height (+0x18) and the row count (+0x1c). */
int rt_native_expand_1555(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_EXPAND_1555);
    (void)eax;
    uint8_t *const gm = g_mem;
    const uint32_t dst = rt_ld32(gm + esp + 4u);
    const int32_t dw = (int32_t)rt_ld32(gm + esp + 8u), dh = (int32_t)rt_ld32(gm + esp + 12u);
    const uint32_t src = rt_ld32(gm + esp + 16u);
    const int32_t sw = (int32_t)rt_ld32(gm + esp + 20u), sh = (int32_t)rt_ld32(gm + esp + 24u);
    const uint32_t rect = rt_ld32(gm + esp + 28u);
    int32_t x0 = 0, y0 = 0, cw = sw, ch = sh;
    if (rect) {
        if (!in_memory(rect, 16u))
            return 0;
        x0 = (int32_t)rt_ld32(gm + rect);
        y0 = (int32_t)rt_ld32(gm + rect + 4u);
        int32_t rw = (int32_t)rt_ld32(gm + rect + 8u), rh = (int32_t)rt_ld32(gm + rect + 12u);
        cw = rw < sw - x0 ? rw : sw - x0;
        ch = rh < sh - y0 ? rh : sh - y0;
    }
    const uint32_t srow0 = src + ((uint32_t)y0 * (uint32_t)sw + (uint32_t)x0) * 2u;
    if (dh > 0 && dw > 0) {
        uint64_t dn = (uint64_t)(uint32_t)dw * (uint32_t)dh * 4u;
        if (dw > 0x100000 || dh > 0x100000 || !in_memory(dst, dn) || overlaps(dst, dn, esp - 0x14u, 0x34u) ||
            (rect && overlaps(dst, dn, rect, 16u)))
            return 0;
        /* The texels read: rows below ch and columns below cw, at most dh x dw. */
        int32_t rows = ch < dh ? ch : dh, cols = cw < dw ? cw : dw;
        if (rows > 0 && cols > 0) {
            uint64_t span = ((uint64_t)(uint32_t)(rows - 1) * (uint32_t)sw + (uint32_t)cols) * 2u;
            if (sw < 0 || !in_memory(srow0, span) || overlaps(dst, dn, srow0, span))
                return 0;
        }
    }
    rt_st32(gm + esp - 4u, ecx);          /* push ecx */
    rt_st32(gm + esp - 8u, c->r[3]);      /* push ebx, esi, edi */
    rt_st32(gm + esp - 0xCu, c->r[6]);
    rt_st32(gm + esp - 0x10u, c->r[7]);
    rt_st32(gm + esp + 20u, (uint32_t)cw);
    rt_st32(gm + esp + 24u, (uint32_t)ch);
    rt_st32(gm + esp + 28u, 0);
    if (dh <= 0) {
        c->r[1] = ecx;
        c->r[2] = (uint32_t)dh;
        *ret = ((uint64_t)(esp + 4u) << 32) | srow0;
        return 1;
    }
    const uint32_t spitch = (uint32_t)sw * 2u;
    rt_st32(gm + esp - 4u, spitch);
    rt_st32(gm + esp - 0x14u, c->r[5]);   /* push ebp */
    /* Per row: texels below min(cw, dw) converted, the rest 0 (no branch per texel). */
    const int32_t ncols = cw < 0 ? 0 : cw < dw ? cw : dw;
    uint32_t srow = srow0, drow = dst;
    for (int32_t y = 0; y < dh; y++, srow += spitch, drow += (uint32_t)dw * 4u) {
        const u16_un *s = (const u16_un *)(const void *)(gm + srow);
        u32_un *o = (u32_un *)(void *)(gm + drow);
        const int32_t n = y < ch ? ncols : 0;
        for (int32_t x = 0; x < n; x++) {
            uint32_t t = s[x];
            o[x] = ((t & 0x1Fu) << 3) | ((t << 6) & 0xF800u) | ((t << 9) & 0xF80000u) | ((uint32_t)-(int32_t)(t >> 15) << 24);
        }
        for (int32_t x = n; x < dw; x++)
            o[x] = 0;
    }
    rt_st32(gm + esp + 4u, dst + 2u + (uint32_t)dh * (uint32_t)dw * 4u);
    rt_st32(gm + esp + 16u, srow0 + (uint32_t)dh * spitch);
    rt_st32(gm + esp + 28u, (uint32_t)dh);
    c->r[1] = spitch;
    c->r[2] = (uint32_t)dw;
    *ret = ((uint64_t)(esp + 4u) << 32) | (uint32_t)dh;
    return 1;
}

/* ---- 0x517FD0 ----------------------------------------------------------------
 * A texel: blue, green, red from palette bytes 0, 1, 2 of the index; alpha from
 * the alpha plane (same layout as the indices) or 0xFF for a non-zero index.
 * Frame: -4 the row count, -8 the alpha row (written with an alpha plane only),
 * -0xc.. the saved ebx, ebp, esi, edi; the argument slots become the destination
 * row (+4), the source row (+0x14), the clipped height (+0x20) and width (+0x24). */
int rt_native_expand_pal8(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_EXPAND_PAL8);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    const uint32_t dst = rt_ld32(gm + esp + 4u);
    const int32_t dw = (int32_t)rt_ld32(gm + esp + 8u), dh = (int32_t)rt_ld32(gm + esp + 12u);
    const uint32_t alpha = rt_ld32(gm + esp + 16u), src = rt_ld32(gm + esp + 20u), pal = rt_ld32(gm + esp + 24u);
    const int32_t sw = (int32_t)rt_ld32(gm + esp + 28u), sh = (int32_t)rt_ld32(gm + esp + 32u);
    const uint32_t rect = rt_ld32(gm + esp + 36u);
    int32_t x0 = 0, y0 = 0, cw = sw, ch = sh;
    if (rect) {
        if (!in_memory(rect, 16u))
            return 0;
        x0 = (int32_t)rt_ld32(gm + rect);
        y0 = (int32_t)rt_ld32(gm + rect + 4u);
        int32_t rw = (int32_t)rt_ld32(gm + rect + 8u), rh = (int32_t)rt_ld32(gm + rect + 12u);
        cw = rw < sw - x0 ? rw : sw - x0;
        ch = rh < sh - y0 ? rh : sh - y0;
    }
    const uint32_t off0 = (uint32_t)y0 * (uint32_t)sw + (uint32_t)x0;
    const uint32_t srow0 = src + off0;
    if (dh > 0 && dw > 0) {
        uint64_t dn = (uint64_t)(uint32_t)dw * (uint32_t)dh * 4u;
        if (dw > 0x100000 || dh > 0x100000 || !in_memory(dst, dn) || overlaps(dst, dn, esp - 0x18u, 0x40u) ||
            (rect && overlaps(dst, dn, rect, 16u)) || !in_memory(pal, 1024u) || overlaps(dst, dn, pal, 1024u))
            return 0;
        int32_t rows = ch < dh ? ch : dh, cols = cw < dw ? cw : dw;
        if (rows > 0 && cols > 0) {
            uint64_t span = (uint64_t)(uint32_t)(rows - 1) * (uint32_t)sw + (uint32_t)cols;
            if (sw < 0 || !in_memory(srow0, span) || overlaps(dst, dn, srow0, span) ||
                (alpha && (!in_memory(alpha + off0, span) || overlaps(dst, dn, alpha + off0, span))))
                return 0;
        }
    }
    rt_st32(gm + esp - 0xCu, c->r[3]);    /* push ebx, ebp, esi, edi */
    rt_st32(gm + esp - 0x10u, c->r[5]);
    rt_st32(gm + esp - 0x14u, c->r[6]);
    rt_st32(gm + esp - 0x18u, c->r[7]);
    rt_st32(gm + esp + 36u, (uint32_t)cw);
    rt_st32(gm + esp + 32u, (uint32_t)ch);
    if (alpha)
        rt_st32(gm + esp - 8u, alpha + off0);
    rt_st32(gm + esp - 4u, 0);
    rt_st32(gm + esp + 20u, srow0);
    rt_st32(gm + esp + 4u, dst);
    if (dh <= 0) {
        c->r[1] = srow0;
        c->r[2] = 0;
        *ret = ((uint64_t)(esp + 4u) << 32) | dst;
        return 1;
    }
    const int32_t ncols = cw < 0 ? 0 : cw < dw ? cw : dw;
    const uint8_t *const p = gm + pal;
    uint32_t srow = srow0, arow = alpha + off0, drow = dst;
    for (int32_t y = 0; y < dh; y++, srow += (uint32_t)sw, arow += (uint32_t)sw, drow += (uint32_t)dw * 4u) {
        const uint8_t *s = gm + srow, *a = gm + arow;
        u32_un *o = (u32_un *)(void *)(gm + drow);
        const int32_t n = y < ch ? ncols : 0;
        for (int32_t x = 0; x < n; x++) {
            const uint8_t *e = p + 4u * s[x];
            uint32_t al = alpha ? a[x] : (s[x] ? 0xFFu : 0u);
            o[x] = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | al << 24;
        }
        for (int32_t x = n; x < dw; x++)
            o[x] = 0;
    }
    if (alpha)
        rt_st32(gm + esp - 8u, alpha + off0 + (uint32_t)dh * (uint32_t)sw);
    rt_st32(gm + esp - 4u, (uint32_t)dh);
    const uint32_t send = srow0 + (uint32_t)dh * (uint32_t)sw, dend = dst + (uint32_t)dh * (uint32_t)dw * 4u;
    rt_st32(gm + esp + 20u, send);
    rt_st32(gm + esp + 4u, dend);
    c->r[1] = send;
    c->r[2] = (uint32_t)dh;
    *ret = ((uint64_t)(esp + 4u) << 32) | dend;
    return 1;
}

/* ---- 0x50B4F0: conversion to the surface format ------------------------------
 * texture_upload(surface, w, h, argb, sw, sh) reads the surface's description,
 * creates a w x h system memory surface of its format, locks it, converts the
 * sw x sh ARGB texels into it (each row padded to w with its last texel, the
 * rows past sh repeating the last source row), unlocks it, Blts it to the
 * surface and releases it. Only the conversion is native: a block hook
 * (tools/recomp/gen.py BLOCK_HOOKS) at 0x50B6FB, once the guest code has locked
 * the surface and derived the shifts, continues at the unlock (0x50BAC4).
 *
 * Frame (ebp): -0x5c bits per pixel, -0x58 / -0x54 / -0x50 the red, green and
 * blue masks, -0x34 the locked bits, -0x11c the pitch; shifts: blue >> [-0x14]
 * << [-0x24], green >> [-0x18] << [-0x28], red >> edx << [-0x2c], alpha >> [-0xc]
 * << [-0x30] (counts modulo 32, as x86 takes them). Arguments: +0xc w, +0x10 h,
 * +0x14 texels, +0x18 sw, +0x1c sh. The loops leave -8 the row, -0x10 the texel
 * count down, -0x1c / -0x20 source pointers, and, past sh, +0x14 the last source
 * row and +0x1c the source pointer. Registers other than ebp and esp are dead
 * at 0x50BAC4 (set anyway to what the guest leaves in the common cases).
 *
 * Declined (the guest code runs): formats other than 16 and 32 bits, empty or
 * huge sizes, and destinations that overlap the source or the frame. */

typedef struct TexShifts {
    uint32_t sb, lb, sg, lg, sr, lr, sa, la;
} TexShifts;

static inline uint32_t tex_conv(uint32_t s, const TexShifts *k)
{
    return (((s & 0xFFu) >> k->sb) << k->lb) | (((s & 0xFF0000u) >> k->sr) << k->lr) |
           (((s & 0xFF00u) >> k->sg) << k->lg) | (((s & 0xFF000000u) >> k->sa) << k->la);
}

static void tex_row32(uint8_t *dst, const uint8_t *src, int32_t sw, int32_t w, const TexShifts *k)
{
    u32_un *o = (u32_un *)(void *)dst;
    const u32_un *s = (const u32_un *)(const void *)src;
    const TexShifts kk = *k;
    for (int32_t x = 0; x < sw; x++)
        o[x] = tex_conv(s[x], &kk);
    const uint32_t last = o[sw - 1];
    for (int32_t x = sw; x < w; x++)
        o[x] = last;
}

static void tex_row16(uint8_t *dst, const uint8_t *src, int32_t sw, int32_t w, const TexShifts *k)
{
    u16_un *o = (u16_un *)(void *)dst;
    const u32_un *s = (const u32_un *)(const void *)src;
    const TexShifts kk = *k;
    for (int32_t x = 0; x < sw; x++)
        o[x] = (uint16_t)tex_conv(s[x], &kk);
    const uint16_t last = o[sw - 1];
    for (int32_t x = sw; x < w; x++)
        o[x] = last;
}

int rt_block_tex_convert(Ctx *c)
{
    if (!g_igi_native)
        return 0;
    uint8_t *const gm = g_mem;
    const uint32_t ebp = c->r[5];
    const uint32_t bits = rt_ld32(gm + ebp - 0x5Cu);
    const uint32_t surf = rt_ld32(gm + ebp - 0x34u), pitch = rt_ld32(gm + ebp - 0x11Cu);
    const int32_t w = (int32_t)rt_ld32(gm + ebp + 0xCu), h = (int32_t)rt_ld32(gm + ebp + 0x10u);
    const uint32_t src = rt_ld32(gm + ebp + 0x14u);
    const int32_t sw = (int32_t)rt_ld32(gm + ebp + 0x18u), sh = (int32_t)rt_ld32(gm + ebp + 0x1Cu);
    if ((bits != 32u && bits != 16u) || sw < 1 || sh < 1 || w < 0 || h < 1 || sw > 0x4000 || sh > 0x4000 ||
        w > 0x4000 || h > 0x4000 || pitch > 0x40000u)
        return 0;
    const uint32_t bpp = bits / 8u, row_bytes = (uint32_t)(sw > w ? sw : w) * bpp;
    const uint64_t dn = (uint64_t)pitch * (uint32_t)(h - 1) + row_bytes;
    const uint64_t sn = (uint64_t)(uint32_t)sw * (uint32_t)sh * 4u;
    if (pitch < row_bytes || !in_memory(surf, dn) || !in_memory(src, sn) || overlaps(surf, dn, src, sn) ||
        overlaps(surf, dn, ebp - 0x560u, 0x580u))
        return 0;
    const uint32_t mr = rt_ld32(gm + ebp - 0x58u), mg = rt_ld32(gm + ebp - 0x54u), mb = rt_ld32(gm + ebp - 0x50u);
    if (bits == 32u && mr == 0xFF0000u && mg == 0xFF00u && mb == 0xFFu && sw == w && sh == h &&
        pitch == (uint32_t)w * 4u) {
        /* The guest's rep movsd: a plain copy. */
        const uint32_t n = (uint32_t)w * (uint32_t)h * 4u;
        memcpy(gm + surf, gm + src, n);
        c->r[0] = (uint32_t)h;
        c->r[1] = 0;
        c->r[2] = n;
        c->r[6] = src + n;
        c->r[7] = surf + n;
        return 1;
    }
    TexShifts k;
    k.sb = rt_ld32(gm + ebp - 0x14u) & 31u;
    k.lb = rt_ld32(gm + ebp - 0x24u) & 31u;
    k.sg = rt_ld32(gm + ebp - 0x18u) & 31u;
    k.lg = rt_ld32(gm + ebp - 0x28u) & 31u;
    k.sr = c->r[2] & 31u;
    k.lr = rt_ld32(gm + ebp - 0x2Cu) & 31u;
    k.sa = rt_ld32(gm + ebp - 0xCu) & 31u;
    k.la = rt_ld32(gm + ebp - 0x30u) & 31u;
    for (int32_t y = 0; y < h; y++) {
        const uint32_t s = src + (uint32_t)(y < sh ? y : sh - 1) * (uint32_t)sw * 4u;
        uint8_t *d = gm + surf + (uint32_t)y * pitch;
        if (bits == 32u)
            tex_row32(d, gm + s, sw, w, &k);
        else
            tex_row16(d, gm + s, sw, w, &k);
    }
    const uint32_t end = src + (uint32_t)sw * (uint32_t)sh * 4u;
    const uint32_t yend = sh < h ? (uint32_t)h : (uint32_t)sh;
    rt_st32(gm + ebp - 8u, yend);
    rt_st32(gm + ebp - 0x10u, 0);
    rt_st32(gm + ebp - 0x1Cu, end);
    rt_st32(gm + ebp - 0x20u, end);
    if (sh < h) {
        const uint32_t last = src + (uint32_t)(sh - 1) * (uint32_t)sw * 4u;
        rt_st32(gm + ebp + 0x14u, last);
        rt_st32(gm + ebp + 0x1Cu, last + (uint32_t)sw * 4u);
    }
    c->r[0] = yend;
    c->r[3] = (uint32_t)w;
    c->r[6] = (uint32_t)sw;
    c->r[7] = yend;
    return 1;
}
