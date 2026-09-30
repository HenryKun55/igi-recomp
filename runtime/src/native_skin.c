/* Hand-decompiled skinned mesh transform of IGI.exe.
 *
 *   0x0049B700  skin_mesh   transforms the vertices of a mesh by its bones into
 *                           the shared dynamic vertex buffer and returns the index
 *                           of the first one
 *
 *   int skin_mesh(Mesh *mesh, Object *obj, Skeleton *skel, int rigid);   (cdecl)
 *
 * Each vertex has one bone; with rigid == 0 its position is also scaled by its
 * weight and the mesh's extra influences (position, weight, bone, vertex) are
 * added to the transformed positions afterwards. The vertex buffer is locked for
 * appending when the vertices fit behind the ones already used this frame, else
 * with its contents discarded.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every x87
 * register a double, rounded to float wherever the game stores a float): the same
 * operations in the same order, so the results are bit-identical to the
 * recompiled code (tools/recomp/nativebench.py). The guest also leaves its
 * results in two argument slots of the caller's frame (the locked pointer and
 * the first index; the last weight with rigid == 0), and so does this; and its
 * dead frame (saved registers, call arguments, the last values of its locals)
 * is reproduced too, since later guest code reads uninitialised locals.
 *
 * Assumed (the guest data always satisfies it): the locked vertex buffer, a
 * separate allocation, overlaps neither the mesh nor the skeleton. A mesh of
 * more than 4000 vertices (the guest reports it and hangs) is left to the
 * recompiled code. */
#include "native.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_SKIN_MESH   0x0049B700u

#define G_VB           0x00BCAD90u   /* IDirect3DVertexBuffer7 * */
#define G_VB_CAP       0x00BCAD94u   /* vertices */
#define G_VB_USED      0x00BCAD98u   /* vertices written this frame */

#define D3DVB_LOCK     0x0Cu
#define D3DVB_UNLOCK   0x10u
#define LOCK_APPEND    0x1020u       /* DDLOCK_NOOVERWRITE | DDLOCK_WAIT */
#define LOCK_DISCARD   0x2020u       /* DDLOCK_DISCARDCONTENTS | DDLOCK_WAIT */

/* Mesh. */
#define MESH_NEXTRA    0x0Cu         /* extra influences */
#define MESH_NVERTS    0x10u
#define MESH_VERTS     0x18u         /* SkinVert[nverts] followed by SkinInfl[nextra] */
#define MAX_VERTS      4000

/* SkinVert (0x28 bytes): position +0, normal +0x0C, uv +0x18, weight +0x20, bone +0x26 (u16).
 * SkinInfl (0x28 bytes): position +0, weight +0x20, vertex +0x24 (u16), bone +0x26 (u16). */
#define VERT_SIZE      0x28u
#define V_NORMAL       0x0Cu
#define V_UV           0x18u
#define V_WEIGHT       0x20u
#define V_VERTEX       0x24u
#define V_BONE         0x26u

/* Skeleton: +8 bone array, 0x84 bytes per bone, translation at +0x18 and the
 * rotation rows at +0x24 (12 floats from BONE_T). */
#define SKEL_BONES     0x08u
#define BONE_SIZE      0x84u
#define BONE_T         0x18u

/* Output vertex (0x20 bytes): position, normal, uv. */
#define OUT_SIZE       0x20u

/* Return addresses of the guest's calls. */
#define RET_LOCK_APPEND   0x0049B755u
#define RET_LOCK_DISCARD  0x0049B797u
#define RET_UNLOCK_SKIN   0x0049BA12u   /* after extra influences */
#define RET_UNLOCK_RIGID  0x0049BB2Bu

/* Loads and stores through a local copy of g_mem: a store through a byte pointer
 * could otherwise modify g_mem itself, which would then be reloaded every time. */
static inline double ldf(const uint8_t *gm, uint32_t a) { return rt_ldf32(gm + a); }
static inline void stf(uint8_t *gm, uint32_t a, double v) { rt_stf32(gm + a, v); }

/* A bone's translation (m[0..2]) and rotation rows (m[3..11]) as doubles, kept
 * while consecutive vertices use the same bone. */
typedef struct BoneCache {
    uint32_t addr;
    double m[12];
} BoneCache;

static inline const double *bone(const uint8_t *gm, BoneCache *bc, uint32_t skel, uint32_t v)
{
    uint32_t b = rt_ld32(gm + skel + SKEL_BONES) + rt_ld16(gm + v + V_BONE) * BONE_SIZE + BONE_T;
    if (b != bc->addr) {
        bc->addr = b;
        for (int k = 0; k < 12; k++)
            bc->m[k] = rt_ldf32(gm + b + 4u * (uint32_t)k);
    }
    return bc->m;
}

/* The guest's locals at esp + 0x10..0x30 (esp after all four pushes), E - 0x24 ..
 * E - 0x04 for the entry esp E. Their last values stay on the stack, where later
 * guest code may read them as uninitialised locals, so they are written back. */
enum { L_XC, L_YC, L_ZC, L_X, L_Y, L_Z, L_NX, L_NY, L_NZ, L_COUNT };
typedef struct Locals {
    float v[L_COUNT];
    uint32_t set;               /* bit k: v[k] was written */
} Locals;

static void write_locals(const Locals *l, uint32_t entry_esp)
{
    for (int k = 0; k < L_COUNT; k++)
        if (l->set & (1u << k))
            native_stf(entry_esp - 0x24u + 4u * (uint32_t)k, l->v[k]);
}

/* Normal rotated by the bone (both loops of the guest compute it the same way). */
static inline void rotate_normal(uint8_t *gm, uint32_t v, uint32_t p, const double *m, float *nrm)
{
    double n0 = ldf(gm, v + V_NORMAL), n1 = ldf(gm, v + V_NORMAL + 4u), n2 = ldf(gm, v + V_NORMAL + 8u);
    nrm[0] = (float)((m[4] * n1 + m[5] * n2) + n0 * m[3]);
    nrm[1] = (float)((n2 * m[8] + n1 * m[7]) + n0 * m[6]);
    nrm[2] = (float)((m[9] * n0 + m[11] * n2) + m[10] * n1);
    memcpy(gm + p + 0x0Cu, nrm, 12);
}

/* Rounding to float, as a store and reload of the x87 value. */
static inline double f32(double v) { return (double)(float)v; }

/* rigid == 0: position by the bone, scaled by the vertex weight. */
static void skin_weighted(uint32_t verts, uint32_t n, uint32_t skel, uint32_t p, Locals *l)
{
    uint8_t *const gm = g_mem;
    BoneCache bc = { 0xFFFFFFFFu, { 0 } };
    float px = 0, py = 0, pz = 0, ty = 0, tz = 0, nrm[3] = { 0, 0, 0 };
    for (uint32_t v = verts; n != 0; n--, v += VERT_SIZE, p += OUT_SIZE) {
        const double *m = bone(gm, &bc, skel, v);
        double w = ldf(gm, v + V_WEIGHT);
        double x = ldf(gm, v), y = ldf(gm, v + 4u), z = ldf(gm, v + 8u);
        px = (float)((x * m[3] + z * m[5]) + y * m[4]);
        py = (float)((x * m[6] + z * m[8]) + y * m[7]);
        pz = (float)((z * m[11] + m[10] * y) + m[9] * x);
        ty = (float)((double)py + m[1]);
        tz = (float)((double)pz + m[2]);
        stf(gm, p, ((double)px + m[0]) * w);
        stf(gm, p + 4u, (double)ty * w);
        stf(gm, p + 8u, (double)tz * w);
        rt_st32(gm + p + 0x18u, rt_ld32(gm + v + V_UV));
        rt_st32(gm + p + 0x1Cu, rt_ld32(gm + v + V_UV + 4u));
        rotate_normal(gm, v, p, m, nrm);
    }
    const float last[L_COUNT] = { px, ty, tz, px, py, pz, nrm[0], nrm[1], nrm[2] };
    memcpy(l->v, last, sizeof(last));
    l->set = 0x1FFu;
}

/* Extra influences: weighted positions added to vertices already written. The
 * guest keeps each weight in the skeleton argument's slot (arg_slot). Returns
 * the last destination vertex (the guest's final ecx). */
static uint32_t skin_extra(uint32_t r, uint32_t n, uint32_t skel, uint32_t ptr_slot, uint32_t arg_slot, Locals *l)
{
    uint8_t *const gm = g_mem;
    BoneCache bc = { 0xFFFFFFFFu, { 0 } };
    uint32_t dst = 0;
    float px = 0, py = 0, pz = 0, ty = 0, tz = 0;
    for (; n != 0; n--, r += VERT_SIZE) {
        const double *m = bone(gm, &bc, skel, r);
        uint32_t ptr = rt_ld32(gm + ptr_slot);
        double x = ldf(gm, r), y = ldf(gm, r + 4u), z = ldf(gm, r + 8u);
        px = (float)((m[5] * z + m[4] * y) + m[3] * x);
        py = (float)((m[8] * z + m[7] * y) + m[6] * x);
        pz = (float)((m[11] * z + m[10] * y) + m[9] * x);
        double tx = (double)px + m[0];
        ty = (float)((double)py + m[1]);
        rt_st32(gm + arg_slot, rt_ld32(gm + r + V_WEIGHT));
        dst = ptr + rt_ld16(gm + r + V_VERTEX) * OUT_SIZE;
        tz = (float)((double)pz + m[2]);
        double w = ldf(gm, arg_slot);
        stf(gm, dst, w * tx + ldf(gm, dst));
        stf(gm, dst + 4u, w * ty + ldf(gm, dst + 4u));
        stf(gm, dst + 8u, w * tz + ldf(gm, dst + 8u));
    }
    const float last[6] = { px, ty, tz, px, py, pz };
    memcpy(l->v, last, sizeof(last));
    l->set |= 0x3Fu;
    return dst;
}

/* rigid != 0: position by the bone only. Returns the last vertex's v (the
 * guest's final ecx). */
static uint32_t skin_rigid(uint32_t verts, uint32_t n, uint32_t skel, uint32_t p, Locals *l)
{
    uint8_t *const gm = g_mem;
    BoneCache bc = { 0xFFFFFFFFu, { 0 } };
    uint32_t last = 0;
    float py = 0, pz = 0, nrm[3] = { 0, 0, 0 };
    for (uint32_t v = verts; n != 0; n--, v += VERT_SIZE, p += OUT_SIZE) {
        const double *m = bone(gm, &bc, skel, v);
        double x = ldf(gm, v), y = ldf(gm, v + 4u), z = ldf(gm, v + 8u);
        double sx = (x * m[3] + z * m[5]) + y * m[4];
        py = (float)((x * m[6] + z * m[8]) + y * m[7]);
        pz = (float)((z * m[11] + m[10] * y) + m[9] * x);
        stf(gm, p, sx + m[0]);
        stf(gm, p + 4u, (double)py + m[1]);
        stf(gm, p + 8u, (double)pz + m[2]);
        rotate_normal(gm, v, p, m, nrm);
        rt_st32(gm + p + 0x18u, rt_ld32(gm + v + V_UV));
        last = rt_ld32(gm + v + V_UV + 4u);
        rt_st32(gm + p + 0x1Cu, last);
    }
    l->v[L_Y] = py;
    l->v[L_Z] = pz;
    memcpy(&l->v[L_NX], nrm, sizeof(nrm));
    l->set = 0x1F0u;
    return last;
}

int rt_native_skin_mesh(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_SKIN_MESH);
    /* Guest frame: arguments at esp + 4.., locals below; sp after the prologue
     * (sub esp, 0x24 and three pushes). */
    const uint32_t mesh_slot = esp + 4u, obj_slot = esp + 8u, skel_slot = esp + 12u, rigid_slot = esp + 16u;
    const uint32_t sp = esp - 0x30u;
    uint32_t mesh = RD32(mesh_slot);
    if ((int32_t)RD32(mesh + MESH_NVERTS) > MAX_VERTS)
        return 0;
    const uint64_t done = (uint64_t)(esp + 4u) << 32;
    /* Registers saved by the prologue (the guest leaves them on its dead stack). */
    WR32(esp - 0x28u, c->r[3]);
    WR32(esp - 0x2Cu, c->r[6]);
    WR32(esp - 0x30u, c->r[7]);

    /* Lock(vb, flags, &mesh_slot, 0): the locked pointer lands in the mesh argument's slot. */
    uint32_t n = RD32(mesh + MESH_NVERTS);
    int append = (int32_t)(n + RD32(G_VB_USED)) <= (int32_t)RD32(G_VB_CAP);
    uint32_t vb = RD32(G_VB), vt = RD32(vb);
    WR32(sp - 4u, 0);
    WR32(sp - 8u, mesh_slot);
    WR32(sp - 12u, append ? LOCK_APPEND : LOCK_DISCARD);
    WR32(sp - 16u, vb);
    c->r[2] = append ? vt : mesh_slot;
    uint32_t hr = (uint32_t)native_call(c, RD32(vt + D3DVB_LOCK), sp - 16u,
                                        append ? RET_LOCK_APPEND : RET_LOCK_DISCARD, append ? mesh_slot : vt, vb);
    uint32_t base;
    if (append) {
        uint32_t used = RD32(G_VB_USED);
        base = used;
        uint32_t ptr = RD32(mesh_slot) + (used << 5);
        uint32_t cnt = RD32(mesh + MESH_NVERTS);
        ecx = used + cnt;
        c->r[2] = cnt;
        WR32(obj_slot, base);
        WR32(mesh_slot, ptr);
        WR32(G_VB_USED, ecx);
    } else {
        ecx = RD32(mesh + MESH_NVERTS);
        base = 0;
        WR32(obj_slot, 0);
        WR32(G_VB_USED, ecx);
    }
    c->r[1] = ecx;
    if (hr != 0) {
        *ret = done | base;
        return 1;
    }
    uint32_t ptr = RD32(mesh_slot);
    if (ptr == 0) {
        c->r[2] = 0;
        *ret = done | base;
        return 1;
    }

    WR32(sp - 4u, c->r[5]);   /* push ebp */
    Locals l = { { 0 }, 0 };
    uint32_t rigid = RD32(rigid_slot);
    uint32_t cnt = RD32(mesh + MESH_NVERTS);
    uint32_t verts = RD32(mesh + MESH_VERTS);
    uint32_t skel = RD32(skel_slot);
    uint32_t ret_addr;
    if (rigid == 0) {
        if ((int32_t)cnt > 0) {
            skin_weighted(verts, cnt, skel, ptr, &l);
            base = RD32(obj_slot);
        }
        uint32_t nextra = RD32(mesh + MESH_NEXTRA);
        verts = RD32(mesh + MESH_VERTS);
        uint32_t r = verts + RD32(mesh + MESH_NVERTS) * VERT_SIZE;
        ecx = verts;
        ret_addr = RET_UNLOCK_RIGID;   /* the guest shares the rigid path's Unlock without influences */
        if ((int32_t)nextra > 0) {
            ecx = skin_extra(r, nextra, skel, mesh_slot, skel_slot, &l);
            ret_addr = RET_UNLOCK_SKIN;
        }
    } else {
        ecx = cnt;
        if ((int32_t)cnt > 0)
            ecx = skin_rigid(verts, cnt, skel, ptr, &l);
        ret_addr = RET_UNLOCK_RIGID;
    }

    /* Unlock(vb), with ebp pushed as well. */
    vb = RD32(G_VB);
    vt = RD32(vb);
    WR32(sp - 8u, vb);
    c->r[2] = vt;
    write_locals(&l, esp);
    native_call(c, RD32(vt + D3DVB_UNLOCK), sp - 8u, ret_addr, ecx, vb);
    *ret = done | base;
    return 1;
}
