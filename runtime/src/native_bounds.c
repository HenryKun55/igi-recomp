/* Hand-decompiled screen bounds of a mesh face group of IGI.exe.
 *
 *   0x004DD7F0  group_bounds   projects one face group of an object's mesh to
 *                              the screen and returns its clipped screen
 *                              rectangle when it overlaps a given one
 *
 *   int group_bounds(Object *obj, int model, int group, const float rmin[2],
 *                    const float rmax[2], float omin[2], float omax[2]);   (cdecl)
 *
 * The group (mesh +0x98, 0x14 bytes: first vertex, vertex count, first
 * triangle, triangle count, material) is skipped unless its material is
 * enabled (byte +0x14C of the material table at 0xA5E354). The plane of its
 * first triangle, in world space, is tested against the eye: when the eye is
 * behind it, the group is kept only if the eye is within reach of the plane
 * (0x4B2C70 gives the nearest point). The group's vertices are put to view
 * space and the screen with outcodes in a frame array (0x1C bytes: view x, y,
 * z, screen x, y, 1/z, outcode), each triangle is clipped and its screen
 * rectangle accumulated (0x4DAB50), and the rectangle is compared with
 * rmin/rmax.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every
 * x87 register a double, rounded to float wherever the game stores a float):
 * the same operations in the same order, so the results are bit-identical to
 * the recompiled code (tools/recomp/nativebench.py). The guest calls see the
 * same registers, arguments and x87 status word; the frame the guest leaves
 * below its entry esp (saved registers, the vertex array, the last values of
 * its locals, call arguments) is reproduced.
 *
 * Assumed (the guest data always satisfies it): the frame does not overlap
 * the object, the mesh or the globals read. */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_GROUP_BOUNDS  0x004DD7F0u
#define FN_PLANE_POINT   0x004B2C70u   /* (out, point, dir, normal) */
#define FN_CLIP_BOUNDS   0x004DAB50u   /* (Vert v0, Vert v1, Vert v2, float **rect, float near) */
#define RET_PLANE_POINT  0x004DDB8Cu
#define RET_CLIP_BOUNDS  0x004DDEC1u

#define G_MATERIALS      0x00A5E354u
#define G_CAM_ROT        0x00BCAAE0u   /* float[9], world to view rows */
#define G_CAM_POS        0x00BCAB08u   /* double[3] */
#define G_PROJ_SX        0x00BCAB28u
#define G_PROJ_SY        0x00BCAB2Cu
#define G_PROJ_CX        0x00BCAB30u
#define G_PROJ_CY        0x00BCAB34u
#define G_CLIP_LEFT      0x00BCABD0u
#define G_CLIP_TOP       0x00BCABD4u
#define G_CLIP_RIGHT     0x00BCABD8u
#define G_CLIP_BOTTOM    0x00BCABDCu
#define C_ONE            0x005333E0u   /* 1.0f */
#define C_ZERO           0x005333ECu   /* 0.0f */
#define C_ONE_D          0x005333F8u   /* 1.0 (double) */
#define C_NEAR           0x00534B2Cu   /* 0.04096f */
#define C_REACH          0x00534B30u   /* squared distance */
#define NEAR_BITS        0x3D27C5ACu   /* 0.04096f, the clipper's near plane */

#define OBJ_POS          0x20u         /* double[3] */
#define OBJ_MODELS       0x6Cu
#define OBJ_ROT          0x70u         /* float[9] rows */
#define MESH_TRIS        0x90u         /* uint32[3] vertex indices */
#define MESH_VERTS       0x94u         /* float[3] */
#define MESH_GROUPS      0x98u

/* Frame: F = entry esp - 0xB80 (saved edi at F, esi F+4, ebp F+8, ebx F+0x0C). */
#define FRAME            0xB80u
#define L_V              0x10u         /* float[3]: normal, later the view position */
#define L_RZ             0x1Cu
#define L_E2             0x20u         /* float[3] */
#define L_CODE0          0x2Cu
#define L_COUNT          0x30u
#define L_WN             0x34u         /* float[3]; later the rectangle pointers */
#define L_N              0x40u         /* float[3] */
#define L_ROT            0x4Cu         /* float[3] */
#define L_SY             0x58u
#define L_RMIN           0x5Cu         /* float[2] */
#define L_RMAX           0x64u         /* float[2] */
#define L_GROUP          0x6Cu
#define L_VISIBLE        0x70u
#define L_MESH           0x74u
#define L_OFS            0x78u         /* float[3] */
#define L_WPOS           0x80u         /* double y, z */
#define L_VERTS          0x90u         /* the vertex array */

static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }

static inline float f32(double v) { return (float)v; }

/* x87 "not equal" test as the guest branches on C3 (unordered counts as equal). */
static inline int nonzero(double v, double zero) { return v < zero || v > zero; }

int rt_native_group_bounds(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_GROUP_BOUNDS);
    (void)ecx;
    (void)eax;
    uint8_t *const gm = g_mem;
    const uint32_t E = esp, F = esp - FRAME;
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    const uint64_t done = (uint64_t)(E + 4u) << 32;
    rt_st32(gm + F + 0x0Cu, ebx0);
    rt_st32(gm + F + 0x08u, ebp0);
    rt_st32(gm + F + 0x04u, esi0);
    rt_st32(gm + F, edi0);
    rt_st32(gm + F + L_RMIN, 0x7F7FFFFFu);
    rt_st32(gm + F + L_RMIN + 4u, 0x7F7FFFFFu);
    rt_st32(gm + F + L_RMAX, 0xFF7FFFFFu);
    rt_st32(gm + F + L_RMAX + 4u, 0xFF7FFFFFu);
    rt_st32(gm + F + L_VISIBLE, 0);
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);

    const uint32_t obj = rt_ld32(gm + E + 4u);
    const uint32_t model = rt_ld32(gm + rt_ld32(gm + obj + OBJ_MODELS) + 4u * rt_ld32(gm + E + 8u));
    const uint32_t mesh = rt_ld32(gm + rt_ld32(gm + model + 0x0Cu) + 0x0Cu);
    rt_st32(gm + F + L_MESH, mesh);
    const uint32_t groups = rt_ld32(gm + mesh + MESH_GROUPS);
    const uint32_t grp = groups + 20u * rt_ld32(gm + E + 0x0Cu);
    rt_st32(gm + F + L_GROUP, grp);
    uint32_t ecx_out = rt_ld32(gm + G_MATERIALS);
    uint8_t enabled = gm[rt_ld32(gm + grp + 0x10u) + ecx_out + 0x14Cu];
    uint32_t edx_out = (groups & 0xFFFFFF00u) | enabled;
    uint32_t eax_out = 0;
    if (!enabled)
        goto out;

    {
        /* Plane of the first triangle. */
        const uint32_t tri = rt_ld32(gm + mesh + MESH_TRIS) + 12u * rt_ld32(gm + grp + 8u);
        const uint32_t verts = rt_ld32(gm + mesh + MESH_VERTS);
        const uint32_t v0 = verts + 12u * rt_ld32(gm + tri), v1 = verts + 12u * rt_ld32(gm + tri + 4u);
        const uint32_t v2 = verts + 12u * rt_ld32(gm + tri + 8u);
        const double e1x = ldf(gm, v1) - ldf(gm, v0);
        const double e1y = ldf(gm, v1 + 4u) - ldf(gm, v0 + 4u);
        const double e1z = ldf(gm, v1 + 8u) - ldf(gm, v0 + 8u);
        const float e2xf = f32(ldf(gm, v2) - ldf(gm, v0));
        const float e2yf = f32(ldf(gm, v2 + 4u) - ldf(gm, v0 + 4u));
        const double e2z = ldf(gm, v2 + 8u) - ldf(gm, v0 + 8u);
        const double e2x = e2xf, e2y = e2yf;
        rt_stf32(gm + F + L_E2, e2x);
        rt_stf32(gm + F + L_E2 + 4u, e2y);
        const float nxf = f32(e2z * e1y - e2y * e1z);
        const float nyf = f32(-(e2z * e1x - e2x * e1z));
        const double nz = e1x * e2y - e2x * e1y;
        const double nx = nxf, ny = nyf;
        double R[9];
        for (int k = 0; k < 9; k++)
            R[k] = ldf(gm, obj + OBJ_ROT + 4u * k);
        const float wnx = f32((nx * R[0] + nz * R[2]) + ny * R[1]);
        const float wny = f32((nx * R[3] + nz * R[5]) + ny * R[4]);
        const float wnz = f32((nx * R[6] + nz * R[8]) + ny * R[7]);
        float n3[3] = { wnx, wny, wnz };
        const double zero = ldf(gm, C_ZERO);
        native_fcom(&fsw, wnx, zero);
        int nz_any = nonzero(wnx, zero);
        if (!nz_any) {
            native_fcom(&fsw, wny, zero);
            nz_any = nonzero(wny, zero);
            if (!nz_any) {
                native_fcom(&fsw, wnz, zero);
                nz_any = nonzero(wnz, zero);
            }
        }
        if (nz_any) {
            const double len = rt_fsqrt(((double)wnx * wnx + (double)wny * wny) + (double)wnz * wnz);
            const double inv = rt_ldf64(gm + C_ONE_D) / len;
            n3[0] = f32((double)wnx * inv);
            n3[1] = f32((double)wny * inv);
            n3[2] = f32((double)wnz * inv);
        }
        rt_stf32(gm + F + L_V, nx);
        rt_stf32(gm + F + L_V + 4u, ny);
        memcpy(gm + F + L_WN, (const float[3]){ wnx, wny, wnz }, 12);
        memcpy(gm + F + L_N, n3, 12);

        /* First vertex in world space, relative to the eye. */
        const double x0 = ldf(gm, v0), y0 = ldf(gm, v0 + 4u), z0 = ldf(gm, v0 + 8u);
        const double X = (R[0] * x0 + R[2] * z0) + R[1] * y0;
        const float Yf = f32((y0 * R[4] + z0 * R[5]) + x0 * R[3]);
        const float Zf = f32((R[6] * x0 + R[8] * z0) + R[7] * y0);
        rt_stf32(gm + F + L_ROT + 4u, Yf);
        rt_stf32(gm + F + L_ROT + 8u, Zf);
        const double wx = X + rt_ldf64(gm + obj + OBJ_POS);
        const double wy = (double)Yf + rt_ldf64(gm + obj + OBJ_POS + 8u);
        const double wz = (double)Zf + rt_ldf64(gm + obj + OBJ_POS + 16u);
        rt_stf64(gm + F + L_WPOS, wy);
        rt_stf64(gm + F + L_WPOS + 8u, wz);
        double dx = wx - rt_ldf64(gm + G_CAM_POS);
        double dy = wy - rt_ldf64(gm + G_CAM_POS + 8u);
        double dz = wz - rt_ldf64(gm + G_CAM_POS + 16u);
        rt_stf32(gm + F + L_RZ, dx);
        rt_stf32(gm + F + L_COUNT, dy);
        rt_stf32(gm + F + L_CODE0, dz);
        native_fcom(&fsw, dx, zero);
        int d_any = nonzero(dx, zero);
        if (!d_any) {
            native_fcom(&fsw, dy, zero);
            d_any = nonzero(dy, zero);
            if (!d_any) {
                native_fcom(&fsw, dz, zero);
                d_any = nonzero(dz, zero);
            }
        }
        if (d_any) {
            const float lenf = f32(rt_fsqrt((dx * dx + dy * dy) + dz * dz));
            rt_stf32(gm + F + L_SY, lenf);
            const double inv = rt_ldf64(gm + C_ONE_D) / (double)lenf;
            dx = dx * inv;
            dy = dy * inv;
            dz = dz * inv;
        }
        const double dot = (dz * n3[2] + dy * n3[1]) + dx * n3[0];
        native_fcom(&fsw, dot, zero);
        if (!(dot >= zero)) {
            /* The eye is behind the plane: nearest point of the plane. */
            const uint32_t nb0 = rt_ld32(gm + F + L_N), nb1 = rt_ld32(gm + F + L_N + 4u);
            const uint32_t nb2 = rt_ld32(gm + F + L_N + 8u);
            rt_st32(gm + F + L_V, nb0);
            rt_st32(gm + F + L_V + 4u, nb1);
            rt_st32(gm + F + L_V + 8u, nb2);
            memcpy(gm + F + L_E2, gm + F + L_RZ, 4);
            rt_stf32(gm + F + L_V, -ldf(gm, F + L_N));
            rt_stf32(gm + F + L_V + 4u, -ldf(gm, F + L_V + 4u));
            memcpy(gm + F + L_E2 + 4u, gm + F + L_COUNT, 4);
            memcpy(gm + F + L_E2 + 8u, gm + F + L_CODE0, 4);
            rt_stf32(gm + F + L_V + 8u, -ldf(gm, F + L_V + 8u));
            rt_st32(gm + F - 4u, F + L_N);
            rt_st32(gm + F - 8u, F + L_V);
            rt_st32(gm + F - 12u, F + L_E2);
            rt_st32(gm + F - 16u, F + L_ROT);
            rt_st32(gm + F - 20u, RET_PLANE_POINT);
            c->r[2] = F + L_E2;
            c->r[3] = grp;
            c->r[5] = mesh;
            c->r[6] = obj;
            c->r[7] = v1;
            c->fsw = native_fsw(&fsw);
            f_004B2C70(c, F - 20u, F + L_V, F + L_ROT);
            ecx_out = c->r[1];
            edx_out = c->r[2];
            native_fsw_sync(&fsw, c);
            const double a = ldf(gm, F + L_E2 + 8u) - ldf(gm, F + L_ROT + 8u);
            const double b = ldf(gm, F + L_E2 + 4u) - ldf(gm, F + L_ROT + 4u);
            const double cc = ldf(gm, F + L_E2) - ldf(gm, F + L_ROT);
            const double d2 = (cc * cc + b * b) + a * a;
            const double reach = ldf(gm, C_REACH);
            native_fcom(&fsw, d2, reach);
            if (d2 > reach)
                goto out;
        } else {
            ecx_out = v0;
            edx_out = rt_ld32(gm + F + L_WN + 8u);
        }
    }

    {
        /* Object position relative to the eye. */
        const float ofs[3] = { f32(rt_ldf64(gm + obj + OBJ_POS) - rt_ldf64(gm + G_CAM_POS)),
                               f32(rt_ldf64(gm + obj + OBJ_POS + 8u) - rt_ldf64(gm + G_CAM_POS + 8u)),
                               f32(rt_ldf64(gm + obj + OBJ_POS + 16u) - rt_ldf64(gm + G_CAM_POS + 16u)) };
        memcpy(gm + F + L_OFS, ofs, 12);
        const int32_t n = (int32_t)rt_ld32(gm + grp + 4u);
        if (n <= 0)
            goto out;
        double R[9], C[9];
        for (int k = 0; k < 9; k++) {
            R[k] = ldf(gm, obj + OBJ_ROT + 4u * k);
            C[k] = ldf(gm, G_CAM_ROT + 4u * k);
        }
        const double ox = ofs[0], oy = ofs[1], oz = ofs[2];
        const double near = ldf(gm, C_NEAR), one = ldf(gm, C_ONE);
        const double psx = ldf(gm, G_PROJ_SX), psy = ldf(gm, G_PROJ_SY), pcx = ldf(gm, G_PROJ_CX);
        const double pcy = ldf(gm, G_PROJ_CY), left = ldf(gm, G_CLIP_LEFT), top = ldf(gm, G_CLIP_TOP);
        const double right = ldf(gm, G_CLIP_RIGHT), bottom = ldf(gm, G_CLIP_BOTTOM);
        uint32_t src = rt_ld32(gm + mesh + MESH_VERTS) + 12u * rt_ld32(gm + grp);
        uint32_t dst = F + L_VERTS;
        float a = 0, b = 0, cz = 0, vx = 0, vy = 0, vz = 0;
        uint32_t visible = 0, rz_bits = 0, code0 = 0, sy_bits = 0;
        for (int32_t i = 0; i < n; i++, src += 12u, dst += NV_VERT_SIZE) {
            const double x = ldf(gm, src), y = ldf(gm, src + 4u), z = ldf(gm, src + 8u);
            a = f32((y * R[1] + R[2] * z) + R[0] * x);
            b = f32((y * R[4] + R[3] * x) + z * R[5]);
            cz = f32((y * R[7] + R[8] * z) + R[6] * x);
            const double X = (double)a + ox, Y = (double)b + oy, Z = (double)cz + oz;
            vx = f32((Z * C[2] + Y * C[1]) + X * C[0]);
            vy = f32((Z * C[5] + Y * C[4]) + X * C[3]);
            vz = f32((Z * C[8] + Y * C[7]) + X * C[6]);
            uint8_t *d = gm + dst;
            float xyz[3] = { vx, vy, vz };
            memcpy(d, xyz, 12);
            native_fcom(&fsw, vz, near);
            if (!(vz >= near)) {
                rt_st32(d + NV_VERT_CODE, NV_OUT_NEAR);
                continue;
            }
            const float rz = f32(one / vz);
            visible = 1;
            const double sx = (double)rz * vx * psx + pcx;
            const double sy = (double)rz * vy * psy + pcy;
            const float syf = f32(sy);
            const float scr[3] = { f32(sx), syf, rz };
            memcpy(d + 12, scr, 12);
            memcpy(&rz_bits, &rz, 4);
            memcpy(&sy_bits, &syf, 4);
            native_fcom(&fsw, sx, left);
            code0 = X87_BELOW(sx, left);
            native_fcom(&fsw, sy, top);
            const uint32_t b1 = X87_BELOW(sy, top);
            native_fcom(&fsw, sx, right);
            const uint32_t b2 = X87_ABOVE(sx, right);
            native_fcom(&fsw, (double)syf, bottom);
            const uint32_t b3 = X87_ABOVE((double)syf, bottom);
            rt_st32(d + NV_VERT_CODE, ((b3 * 2u | b2) << 1 | b1) << 1 | code0);
        }
        /* The guest's locals after its loop. */
        const float last[3] = { vx, vy, vz };
        memcpy(gm + F + L_V, last, 12);
        const float rot3[3] = { a, b, cz };
        memcpy(gm + F + L_E2, rot3, 12);
        memcpy(gm + F + L_ROT, rot3, 12);
        rt_st32(gm + F + L_COUNT, 0);
        ecx_out = src;
        edx_out = dst + 0x10u;
        if (!visible)
            goto out;
        rt_st32(gm + F + L_VISIBLE, 1);
        rt_st32(gm + F + L_RZ, rz_bits);
        rt_st32(gm + F + L_CODE0, code0);
        rt_st32(gm + F + L_SY, sy_bits);
    }

    {
        /* Clip each triangle and grow the rectangle. */
        rt_st32(gm + F + L_WN, F + L_RMIN);
        rt_st32(gm + F + L_WN + 4u, F + L_RMAX);
        ecx_out = F + L_RMIN;
        edx_out = F + L_RMAX;
        int32_t j = 0;
        if ((int32_t)rt_ld32(gm + grp + 0x0Cu) > 0) do {
            const uint32_t g = rt_ld32(gm + F + L_GROUP);
            const uint32_t base = rt_ld32(gm + g);
            const uint32_t t = rt_ld32(gm + rt_ld32(gm + F + L_MESH) + MESH_TRIS) + 12u * (rt_ld32(gm + g + 8u) + (uint32_t)j);
            const uint32_t i0 = rt_ld32(gm + t) - base, i1 = rt_ld32(gm + t + 4u) - base;
            const uint32_t i2 = rt_ld32(gm + t + 8u) - base;
            rt_st32(gm + F - 4u, NEAR_BITS);
            rt_st32(gm + F - 8u, F + L_WN);
            rt_move_small(gm + F - 0x24u, gm + F + L_VERTS + NV_VERT_SIZE * i2, NV_VERT_SIZE);
            rt_move_small(gm + F - 0x40u, gm + F + L_VERTS + NV_VERT_SIZE * i1, NV_VERT_SIZE);
            rt_move_small(gm + F - 0x5Cu, gm + F + L_VERTS + NV_VERT_SIZE * i0, NV_VERT_SIZE);
            rt_st32(gm + F - 0x60u, RET_CLIP_BOUNDS);
            c->r[2] = i0 * 7u;
            c->r[3] = base;
            c->r[5] = (uint32_t)j;
            c->r[6] = F + L_VERTS + NV_VERT_SIZE * i0 + NV_VERT_SIZE;
            c->r[7] = F - 0x40u;
            c->fsw = native_fsw(&fsw);
            f_004DAB50(c, F - 0x60u, 0, i0);
            edx_out = c->r[2];
            native_fsw_sync(&fsw, c);
        } while (++j < (int32_t)rt_ld32(gm + rt_ld32(gm + F + L_GROUP) + 0x0Cu));
    }

    {
        /* Overlap with the given rectangle. */
        const uint32_t rmax = rt_ld32(gm + E + 0x14u);
        ecx_out = rmax;
        double v = ldf(gm, F + L_RMIN), w = ldf(gm, rmax);
        native_fcom(&fsw, v, w);
        if (v > w)
            goto out;
        v = ldf(gm, F + L_RMIN + 4u);
        w = ldf(gm, rmax + 4u);
        native_fcom(&fsw, v, w);
        if (v > w)
            goto out;
        const uint32_t rmin = rt_ld32(gm + E + 0x10u);
        ecx_out = rmin;
        v = ldf(gm, F + L_RMAX);
        w = ldf(gm, rmin);
        native_fcom(&fsw, v, w);
        if (!(v >= w))
            goto out;
        v = ldf(gm, F + L_RMAX + 4u);
        w = ldf(gm, rmin + 4u);
        native_fcom(&fsw, v, w);
        if (!(v >= w))
            goto out;
        const uint32_t omin = rt_ld32(gm + E + 0x18u), omax = rt_ld32(gm + E + 0x1Cu);
        rt_st32(gm + omin, rt_ld32(gm + F + L_RMIN));
        rt_st32(gm + omin + 4u, rt_ld32(gm + F + L_RMIN + 4u));
        ecx_out = rt_ld32(gm + F + L_RMAX);
        edx_out = rt_ld32(gm + F + L_RMAX + 4u);
        rt_st32(gm + omax, ecx_out);
        rt_st32(gm + omax + 4u, edx_out);
        eax_out = 1;
    }

out:
    c->r[1] = ecx_out;
    c->r[2] = edx_out;
    c->r[3] = rt_ld32(gm + F + 0x0Cu);
    c->r[5] = rt_ld32(gm + F + 0x08u);
    c->r[6] = rt_ld32(gm + F + 0x04u);
    c->r[7] = rt_ld32(gm + F);
    c->fsw = native_fsw(&fsw);
    *ret = done | eax_out;
    return 1;
}
