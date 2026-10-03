/* Hand-decompiled per-object fog range of IGI.exe.
 *
 *   0x0049B2D0  fog_range   sets the device's linear fog range so that an
 *                           object's near and far depth get the fog of the
 *                           game's exponential model
 *
 *   void fog_range(const double pos[3], float r);   (cdecl)
 *
 * The depth d of pos along the view axis (floats 0xBCAAF8, 0xBCAAFC, 0xBCAB00;
 * relative to the camera 0xBCAB08, with y and z rounded to float first) gives the
 * near and far depths a = d - r, b = d + r (floats). Their fog factors are
 * fa = 1 / 2^(k a F log2 e) and fb likewise (k: float 0xBCABEC, F: float
 * 0xB46D04, the 1 a float at 0x5333E0), computed with the x87 idiom frndint,
 * f2xm1, fscale. The linear fog through both, with m = (fb a - fa b) / (fb - fa),
 * goes to the device: FOGSTART (0x24) = (m - (m - a) / fa) k, FOGEND (0x25) =
 * k m, by two SetRenderState calls (vtable +0x50 of the device at 0x5CA114).
 * When fb - fa or fa is zero (or unordered, against the float at 0x5333EC) both
 * are 0. The argument slots get the far value (pos) and the near one (r); the
 * frame keeps fb, b, fb - fa, y and z as floats.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float where the game stores a float, the
 * same libm exp2), so the results are bit-identical to the recompiled code
 * (tools/recomp/nativebench.py). The game calls it for every polygon of its
 * depth-sorted transparent queue, mostly several times in a row with the same
 * object position: the results of the last call are kept with all its inputs
 * (position, radius, view, fog constants, rounding mode; compared bit for bit)
 * and reused when they are the same. The device calls are made every time. */
#include "native.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_FOG_RANGE    0x0049B2D0u
#define RET_FOG_START   0x0049B419u
#define RET_FOG_END     0x0049B42Bu

#define G_FOG_SCALE     0x00B46D04u   /* F */
#define G_VIEW_AXIS     0x00BCAAF8u   /* x, y, z factors (floats) */
#define G_CAMERA        0x00BCAB08u   /* double[3] */
#define G_UNIT          0x00BCABECu   /* k */
#define G_DEVICE        0x005CA114u
#define C_ONE           0x005333E0u
#define C_ZERO          0x005333ECu

#define RS_FOGSTART     0x24u
#define RS_FOGEND       0x25u

#define FSW_CC          (FSW_C0 | FSW_C1 | FSW_C2 | FSW_C3)

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }

static inline uint32_t f2u(float v)
{
    uint32_t b;
    memcpy(&b, &v, 4);
    return b;
}

/* The guest's 2^x: frndint (control word rounding), f2xm1, fld1/faddp, fscale. */
static inline double guest_exp2(const Ctx *c, double x)
{
    double n = rt_fround(c, x);
    return rt_fscale((exp2(x - n) - 1.0) + 1.0, n);
}

/* Inputs of the last call (bits) and what it left: the frame words fb, b,
 * fb - fa, y, z, the argument slots and the condition codes of its last x87
 * comparison. */
enum { K_POS = 0, K_R = 6, K_F = 7, K_AXIS = 8, K_CAM = 11, K_UNIT = 17, K_ONE = 18, K_ZERO = 19, K_RC = 20,
       K_WORDS = 21 };
static struct {
    int valid;
    uint32_t key[K_WORDS];
    uint32_t fb, b, df, y, z, arg_pos, arg_r, cc;
} memo;

static void compute(Ctx *c, const uint8_t *gm, const uint32_t *key)
{
    float yf, zf;
    double px, py, pz, cam[3];
    memcpy(&px, key + K_POS, 8);
    memcpy(&py, key + K_POS + 2, 8);
    memcpy(&pz, key + K_POS + 4, 8);
    memcpy(cam, key + K_CAM, 24);
    yf = (float)py;
    zf = (float)pz;
    const double xr = px - cam[0];
    const double yr = (double)yf - cam[1];
    const double zr = (double)zf - cam[2];
    const double d = (ldf(gm, G_VIEW_AXIS + 8u) * zr + ldf(gm, G_VIEW_AXIS + 4u) * yr) + ldf(gm, G_VIEW_AXIS) * xr;
    float rf;
    memcpy(&rf, key + K_R, 4);
    const double r = (double)rf;
    const float af = (float)(d - r);
    const float bf = (float)(d + r);
    const double F = ldf(gm, G_FOG_SCALE);
    const double k = ldf(gm, G_UNIT);
    const double one = ldf(gm, C_ONE), zero = ldf(gm, C_ZERO);
    const double x1 = ((k * (double)af) * F) * 1.4426950408889634;
    const float fa = (float)(one / guest_exp2(c, x1));
    const double x2 = ((k * (double)bf) * F) * 1.4426950408889634;
    const double q = one / guest_exp2(c, x2);
    const float fb = (float)q;
    const double diff = q - (double)fa;
    const float dfl = (float)diff;
    double ca = diff;
    float out_pos = 0.0f, out_r = 0.0f;
    int ranged = 0;
    if (diff < zero || diff > zero) {
        ca = (double)fa;
        ranged = (double)fa < zero || (double)fa > zero;
    }
    if (ranged) {
        const double m = ((double)fb * (double)af - (double)fa * (double)bf) / (double)dfl;
        const double u = (m - (double)af) / (double)fa;
        out_r = (float)((m - u) * k);
        out_pos = (float)(k * m);
    }
    memo.fb = f2u(fb);
    memo.b = f2u(bf);
    memo.df = f2u(dfl);
    memo.y = f2u(yf);
    memo.z = f2u(zf);
    memo.arg_pos = ranged ? f2u(out_pos) : 0u;
    memo.arg_r = ranged ? f2u(out_r) : 0u;
    memo.cc = rt_fcom(0u, ca, zero);
}

int rt_native_fog_range(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_FOG_RANGE);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    /* Frame: E = entry esp, S = E - 0x18: fb at S, b at S+4, fb - fa at S+8,
     * y and z at S+0x10, S+0x14; the argument slots E+4 and E+8. */
    const uint32_t E = esp, S = E - 0x18u;
    const uint32_t pos = rt_ld32(gm + E + 4u);
    uint32_t key[K_WORDS];
    for (uint32_t i = 0; i < 6u; i++)
        key[K_POS + i] = rt_ld32(gm + pos + 4u * i);
    key[K_R] = rt_ld32(gm + E + 8u);
    key[K_F] = rt_ld32(gm + G_FOG_SCALE);
    for (uint32_t i = 0; i < 3u; i++)
        key[K_AXIS + i] = rt_ld32(gm + G_VIEW_AXIS + 4u * i);
    for (uint32_t i = 0; i < 6u; i++)
        key[K_CAM + i] = rt_ld32(gm + G_CAMERA + 4u * i);
    key[K_UNIT] = rt_ld32(gm + G_UNIT);
    key[K_ONE] = rt_ld32(gm + C_ONE);
    key[K_ZERO] = rt_ld32(gm + C_ZERO);
    key[K_RC] = c->fcw & 0x0C00u;
    uint32_t diff = !memo.valid;
    for (uint32_t i = 0; i < K_WORDS; i++)
        diff |= key[i] ^ memo.key[i];
    if (diff) {
        rt_copy_small(memo.key, key, sizeof(key));   /* no SceLibc memcpy on the Vita */
        memo.valid = 1;
        compute(c, gm, key);
    }
    rt_st32(gm + S, memo.fb);
    rt_st32(gm + S + 4u, memo.b);
    rt_st32(gm + S + 8u, memo.df);
    rt_st32(gm + S + 0x10u, memo.y);
    rt_st32(gm + S + 0x14u, memo.z);
    rt_st32(gm + E + 4u, memo.arg_pos);
    rt_st32(gm + E + 8u, memo.arg_r);

    /* SetRenderState(device, FOGSTART, [E+8]), then (device, FOGEND, [E+4]). */
    static const GuestFunc *ic_start = g_guest_func_hash, *ic_end = g_guest_func_hash;
    uint32_t dev = rt_ld32(gm + G_DEVICE);
    uint32_t edx = memo.arg_r;
    rt_st32(gm + S - 4u, edx);
    rt_st32(gm + S - 8u, RS_FOGSTART);
    uint32_t vt = rt_ld32(gm + dev);
    rt_st32(gm + S - 12u, dev);
    c->r[2] = edx;
    c->fsw = (c->fsw & ~FSW_CC) | memo.cc;
    uint64_t rv = native_call_ic(c, rt_ld32(gm + vt + 0x50u), S - 12u, RET_FOG_START, vt, dev, &ic_start);
    uint32_t sp = (uint32_t)(rv >> 32);
    dev = rt_ld32(gm + G_DEVICE);
    edx = rt_ld32(gm + sp + 0x1Cu);
    rt_st32(gm + sp - 4u, edx);
    rt_st32(gm + sp - 8u, RS_FOGEND);
    vt = rt_ld32(gm + dev);
    rt_st32(gm + sp - 12u, dev);
    c->r[2] = edx;
    rv = native_call_ic(c, rt_ld32(gm + vt + 0x50u), sp - 12u, RET_FOG_END, vt, dev, &ic_end);
    sp = (uint32_t)(rv >> 32);
    *ret = ((uint64_t)(sp + 0x1Cu) << 32) | (uint32_t)rv;
    return 1;
}
