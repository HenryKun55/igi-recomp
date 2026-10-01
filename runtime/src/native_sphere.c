/* Hand-decompiled sphere visibility test of IGI.exe.
 *
 *   0x0049FEB0  sphere_cull   classifies a view-space bounding sphere against
 *                             the view volume
 *
 *   void sphere_cull(SphereTest *t);   (cdecl)
 *
 * SphereTest: +0 centre (3 floats, view space), +4 radius, +8 int *inside
 * (optional), +0x0C result. result = 1 when the sphere is outside: in front of
 * the near distance (0xBCADDC), beyond the far one (0xBCADCC), or outside one of
 * the four side planes (x and y against z, by the slopes and scales at
 * 0xB46CE4/0xB817A4, 0xB46D1C/0xB46D20, 0xB816FC/0xB46D18, 0xB8179C/0xB46CE8),
 * or behind the camera; *inside = 1 when it is entirely within the side planes,
 * between near and far and not too close (radius + 409.6 below z).
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float where the game stores a float): the
 * same operations in the same order, with the comparisons' unordered cases, so
 * the results are bit-identical to the recompiled code
 * (tools/recomp/nativebench.py). The guest's frame (the radius, its negation,
 * the four plane distances, the near and far distances, the flag bytes and the
 * saved registers), the plane distance it leaves in its argument slot and the
 * x87 status word of the last comparison are reproduced. */
#include "native.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_SPHERE_CULL  0x0049FEB0u

#define G_NEAR          0x00BCADDCu
#define G_FAR           0x00BCADCCu
#define G_LEFT_K        0x00B46CE4u
#define G_LEFT_S        0x00B817A4u
#define G_RIGHT_K       0x00B46D1Cu
#define G_RIGHT_S       0x00B46D20u
#define G_TOP_K         0x00B816FCu
#define G_TOP_S         0x00B46D18u
#define G_BOTTOM_K      0x00B8179Cu
#define G_BOTTOM_S      0x00B46CE8u
#define C_CLOSE         0x005338D0u   /* 409.6f */

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline float f32(double v) { return (float)v; }

static inline void stw(uint8_t *gm, uint32_t a, float v)
{
    memcpy(gm + a, &v, 4);
}

int rt_native_sphere_cull(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_SPHERE_CULL);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    /* Frame: E = esp; locals E-0x20..E-1, saved ebp at E-0x24, esi at E-0x28,
     * edi at E-0x2C, ebx at E-0x30. */
    const uint32_t E = esp;
    const uint32_t t = rt_ld32(gm + E + 4u);
    rt_st32(gm + E - 0x24u, c->r[5]);
    rt_st32(gm + E - 0x28u, c->r[6]);
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);

    const uint32_t s = rt_ld32(gm + t);
    const uint32_t rbits = rt_ld32(gm + t + 4u);
    rt_st32(gm + E - 0x18u, rbits);
    float rf;
    memcpy(&rf, &rbits, 4);
    const double r = (double)rf;
    const float nrf = f32(-r);
    const double nr = (double)nrf;
    stw(gm, E - 0x14u, nrf);
    const double z = ldf(gm, s + 8u);
    native_fcom(&fsw, z, nr);
    uint8_t behind = X87_BELOW(z, nr);
    gm[E - 0x19u] = behind;
    const uint32_t nearb = rt_ld32(gm + G_NEAR), farb = rt_ld32(gm + G_FAR);
    rt_st32(gm + E - 0x08u, nearb);
    rt_st32(gm + E - 0x04u, farb);
    float nearf, farf;
    memcpy(&nearf, &nearb, 4);
    memcpy(&farf, &farb, 4);
    const double nearv = (double)nearf, farv = (double)farf;

    uint32_t ecx_out = nearb, edx_out = farb, eax_out;
    const uint32_t pin = rt_ld32(gm + t + 8u);
    double v = r + z;
    native_fcom(&fsw, v, nearv);
    int outside_depth = X87_BELOW(v, nearv);
    if (!outside_depth) {
        v = z - r;
        native_fcom(&fsw, v, farv);
        outside_depth = X87_ABOVE(v, farv);
    }
    if (outside_depth) {
        if (pin)
            rt_st32(gm + pin, 0);
        rt_st32(gm + t + 0x0Cu, 1);
        eax_out = pin;
    } else {
        const double x = ldf(gm, s), y = ldf(gm, s + 4u);
        const double a = (x - ldf(gm, G_LEFT_K) * z) * ldf(gm, G_LEFT_S);
        const double b = (x - ldf(gm, G_RIGHT_K) * z) * ldf(gm, G_RIGHT_S);
        const float bf = f32(b);
        stw(gm, E + 4u, bf);
        const float cf = f32((y - ldf(gm, G_TOP_K) * z) * ldf(gm, G_TOP_S));
        stw(gm, E - 0x10u, cf);
        const float df = f32((y - ldf(gm, G_BOTTOM_K) * z) * ldf(gm, G_BOTTOM_S));
        stw(gm, E - 0x0Cu, df);
        native_fcom(&fsw, a, nr);
        uint8_t left = X87_BELOW(a, nr);
        gm[E - 0x1Du] = left;
        native_fcom(&fsw, b, r);
        uint8_t right = X87_ABOVE(b, r);
        gm[E - 0x1Cu] = right;
        native_fcom(&fsw, (double)cf, nr);
        uint8_t top = X87_BELOW((double)cf, nr);
        gm[E - 0x1Bu] = top;
        native_fcom(&fsw, (double)df, r);
        uint8_t bottom = X87_ABOVE((double)df, r);
        gm[E - 0x1Au] = bottom;
        rt_st32(gm + E - 0x2Cu, c->r[7]);   /* push edi */
        if (pin) {
            rt_st32(gm + E - 0x30u, c->r[3]);   /* push ebx */
            native_fcom(&fsw, a, r);
            int in_left = X87_ABOVE(a, r);
            native_fcom(&fsw, (double)bf, nr);
            uint8_t in_right = X87_BELOW((double)bf, nr);
            gm[E + 4u] = in_right;
            native_fcom(&fsw, (double)cf, r);
            int in_top = X87_ABOVE((double)cf, r);
            native_fcom(&fsw, (double)df, nr);
            int in_bottom = X87_BELOW((double)df, nr);
            double rc = r + ldf(gm, C_CLOSE);
            native_fcom(&fsw, rc, z);
            int far_enough = X87_BELOW(rc, z);
            uint32_t inside = in_left && in_right && in_top && in_bottom && far_enough;
            rt_st32(gm + pin, inside);
            edx_out = (farb & 0xFFFFFF00u) | (uint32_t)in_top;
            /* Then cleared unless strictly beyond near and short of far. */
            v = z - r;
            native_fcom(&fsw, v, nearv);
            uint32_t keep = 0;
            if (X87_ABOVE(v, nearv)) {
                v = r + z;
                native_fcom(&fsw, v, farv);
                keep = X87_BELOW(v, farv);
            }
            rt_st32(gm + pin, rt_ld32(gm + pin) & keep);
            ecx_out = keep;
        }
        eax_out = (left || right || top || bottom || behind) ? 1u : 0u;
        rt_st32(gm + t + 0x0Cu, eax_out);
    }
    c->r[1] = ecx_out;
    c->r[2] = edx_out;
    c->fsw = native_fsw(&fsw);
    *ret = ((uint64_t)(E + 4u) << 32) | eax_out;
    return 1;
}
