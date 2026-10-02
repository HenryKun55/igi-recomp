/* Hand-decompiled object visibility test of IGI.exe.
 *
 *   0x004D0510  object_cull   bounding sphere of an object in view space,
 *                             tested against the view volume
 *
 *   int object_cull(const Object *obj, int *inside);   (cdecl)
 *
 * The mesh (obj+0x6C -> +0 -> +0x0C -> +0x0C) has its sphere centre at +0x30
 * and radius at +0x3C. The centre, rotated by the object's matrix (floats at
 * obj+0x70) and moved to the object's position (doubles at obj+0x20) relative
 * to the camera (0xBCAB08), is turned to view space by the camera rotation
 * (0xBCAAE0); the radius is scaled by the largest of the object's scales
 * (obj+0x98, +0x9C, +0xA0, as the guest's comparisons pick it). Returns the
 * result of the view-space sphere test 0x4D03D0(centre, radius, inside).
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float where the game stores a float), so
 * the results are bit-identical to the recompiled code
 * (tools/recomp/nativebench.py). The call sees the same stack, registers and
 * x87 status word; the guest's frame and the radius it leaves in its argument
 * slot are reproduced. */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_OBJECT_CULL  0x004D0510u
#define RET_VIEW_SPHERE 0x004D06B6u

#define G_CAM_ROT       0x00BCAAE0u   /* 3x3 floats */
#define G_CAMERA        0x00BCAB08u   /* 3 doubles */

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }

static inline void stf(uint8_t *gm, uint32_t a, float v)
{
    memcpy(gm + a, &v, 4);
}

int rt_native_object_cull(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_OBJECT_CULL);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    /* Frame: E = entry esp, S = E - 0x18: the view centre at S (relative
     * position first), its view x again at S+0x0C, the rotated offset's y, z
     * (then view y, z) at S+0x10, S+0x14; the argument slot E+4 gets the
     * radius. */
    const uint32_t E = esp, S = E - 0x18u;
    const uint32_t obj = rt_ld32(gm + E + 4u);
    const float rx = (float)(rt_ldf64(gm + obj + 0x20u) - rt_ldf64(gm + G_CAMERA));
    const float ry = (float)(rt_ldf64(gm + obj + 0x28u) - rt_ldf64(gm + G_CAMERA + 8u));
    const float rz = (float)(rt_ldf64(gm + obj + 0x30u) - rt_ldf64(gm + G_CAMERA + 16u));
    const uint32_t model = rt_ld32(gm + obj + 0x6Cu);
    const uint32_t mesh = rt_ld32(gm + rt_ld32(gm + rt_ld32(gm + model) + 0x0Cu) + 0x0Cu);
    const double mx = ldf(gm, mesh + 0x30u), my = ldf(gm, mesh + 0x34u), mz = ldf(gm, mesh + 0x38u);
    const uint32_t m = obj + 0x70u;
    const double c0 = (ldf(gm, m + 0x08u) * mz + ldf(gm, m + 0x04u) * my) + ldf(gm, m) * mx;
    const float c1 = (float)((ldf(gm, m + 0x14u) * mz + ldf(gm, m + 0x10u) * my) + ldf(gm, m + 0x0Cu) * mx);
    const float c2 = (float)((ldf(gm, m + 0x20u) * mz + ldf(gm, m + 0x1Cu) * my) + ldf(gm, m + 0x18u) * mx);
    const double X = c0 + (double)rx, Y = (double)c1 + (double)ry, Z = (double)c2 + (double)rz;
    const uint32_t r = G_CAM_ROT;
    const float vx = (float)((ldf(gm, r + 0x08u) * Z + ldf(gm, r + 0x04u) * Y) + ldf(gm, r) * X);
    const float vy = (float)((ldf(gm, r + 0x14u) * Z + ldf(gm, r + 0x10u) * Y) + ldf(gm, r + 0x0Cu) * X);
    const float vz = (float)((ldf(gm, r + 0x20u) * Z + ldf(gm, r + 0x1Cu) * Y) + ldf(gm, r + 0x18u) * X);
    stf(gm, S, vx);
    stf(gm, S + 4u, vy);
    stf(gm, S + 8u, vz);
    stf(gm, S + 0x0Cu, vx);
    stf(gm, S + 0x10u, vy);
    stf(gm, S + 0x14u, vz);
    /* The largest scale, as the guest's comparisons pick it. */
    const double s0 = ldf(gm, obj + 0x98u), s1 = ldf(gm, obj + 0x9Cu), s2 = ldf(gm, obj + 0xA0u);
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);
    native_fcom(&fsw, s1, s0);
    double big = X87_BELOW(s1, s0) ? s0 : s1;
    native_fcom(&fsw, big, s2);
    if (X87_ABOVE(big, s2)) {
        native_fcom(&fsw, s1, s0);
        big = X87_BELOW(s1, s0) ? s0 : s1;
    } else {
        big = s2;
    }
    const float rad = (float)(big * ldf(gm, mesh + 0x3Cu));
    stf(gm, E + 4u, rad);
    uint32_t radb;
    memcpy(&radb, &rad, 4);
    const uint32_t inside = rt_ld32(gm + E + 8u);
    rt_st32(gm + S - 4u, inside);
    rt_st32(gm + S - 8u, radb);
    rt_st32(gm + S - 12u, S);
    rt_st32(gm + S - 16u, RET_VIEW_SPHERE);
    c->r[2] = radb;
    c->fsw = native_fsw(&fsw);
    const uint64_t rv = f_004D03D0(c, S - 16u, inside, S);
    *ret = ((uint64_t)(E + 4u) << 32) | (uint32_t)rv;
    return 1;
}
