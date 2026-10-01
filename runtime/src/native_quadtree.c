/* Hand-decompiled terrain octree traversal of IGI.exe.
 *
 *   0x004D14E0  terrain_collect   walks the terrain's octree around the camera
 *                                 and builds the frame's list of terrain patches
 *
 *   void terrain_collect(Terrain *t);   (cdecl)
 *
 * The walk is depth first with an explicit stack of {node, flags} in the guest
 * frame. Each cell (eight children per node, visited in a camera-dependent
 * order) is tested against the view pyramid in integer world coordinates; cells
 * deep enough and far enough become patches: a cache (0xB04340, 2048 entries
 * hashed by position) maps them to patches built on earlier frames, which go to
 * the patch list (0xB12C60), or to requests for new ones (0xB16CE0). Up to five
 * object layers of the terrain are walked in step with the octree (callbacks per
 * object when a node is entered and left). At the end, cache entries not used
 * this frame are evicted and their patches returned to their pools.
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (the same
 * libm calls for fpatan and fcos, conversions with the control word's rounding,
 * the same operations in the same order), so the results are bit-identical to
 * the recompiled code (tools/recomp/nativebench.py). Guest calls see the same
 * registers, stack and x87 status word, the status word is stored on return,
 * and the frame the guest leaves below its entry esp is reproduced (every local
 * is a variable initialised from the stale stack contents and written back).
 * The guest's error paths (inconsistent cache entries or pools: a message, then
 * an endless loop) are reproduced as well. */
#include "native.h"
#include "recomp_gen.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

#define FN_TERRAIN_COLLECT 0x004D14E0u
#define FN_RESET_COUNTERS  0x004C5810u
#define FN_FATAL           0x004AF7B0u

/* Globals. */
#define G_SETTINGS       0x00A44344u   /* ->+0xDFC: double height limit (optional) */
#define G_FRAME          0x00C10380u
#define G_OVERFLOW       0x00B12B80u   /* requests dropped (list full) */
#define G_PATCH_COUNT    0x00B16C9Cu
#define G_REQUEST_COUNT  0x00B16CA4u
#define G_LOD_DIST       0x00B16C6Cu
#define G_PATCH_LIST     0x00B12C60u
#define G_REQUESTS_END   0x00B16D10u   /* request records (0x30 bytes, written downwards per record) */
#define G_TAN_X          0x00BCAB20u
#define G_TAN_Y          0x00BCAB24u
#define G_ORDER          0x00548060u   /* child visiting orders: 8 x 8 bytes, one per dword */
#define G_ORDER2         0x00B12BE0u   /* 16 dwords */
#define G_CAM_POS        0x00BCAB08u   /* double[3] */
#define G_CAM_ROT        0x00BCAAE0u   /* float[10] */
#define G_ORIGIN         0x00B12BC0u   /* int[3] */
#define G_LAYER_INDEX    0x00548444u   /* signed bytes: terrain slot of each of the 5 layers */
#define G_LAYER_SAVE     0x00A4EF38u   /* 5 x 0x40: the layers' heads, restored at the end */
#define G_LAYER_CURSOR   0x00A53740u   /* 5 x 0x40: walk cursors (+0xC the layer head) */
#define G_ENTER_FN       0x00A9C4E0u   /* object callbacks when a node is entered */
#define G_LEAVE_FN       0x00A9CAE0u   /* ... and left */
#define G_OBJ_LIST       0x00B12C58u   /* {u8 kind, obj} records */
#define G_OBJ_COUNT      0x00B12C20u
#define G_B12BB8         0x00B12BB8u   /* cleared when a node is entered; nonzero after the callbacks: no patches */
#define G_B97D24         0x00B97D24u
#define G_B97B1C         0x00B97B1Cu
#define G_BA2058         0x00BA2058u
#define G_BA5060         0x00BA5060u
#define G_CACHE          0x00B04340u   /* 2048 x 0x1C: x, y, z, dist, frame, pool, patch */
#define CACHE_SIZE       0x1Cu
#define CACHE_COUNT      0x800u
/* Patch pools per cache entry type: base, index array, count, stride, offset of a patch's own position. */
#define G_POOL0          0x00B12B60u
#define G_POOL1          0x00B12C40u
#define G_POOL2          0x00B16CC0u
/* Constants. */
#define K_TAN_NUM        0x00534B10u
#define K_TAN_MUL        0x00534B0Cu
#define K_HALF           0x00533504u
#define K_NEAR           0x00534AE0u   /* double */
#define K_LOD            0x00534B08u
/* Messages of the error paths. */
#define MSG_NO_PATCH     0x00548490u
#define MSG_STALE_PATCH  0x00548464u
#define MSG_BAD_POOL     0x00548450u

/* Return addresses of the guest's calls. */
#define RET_RESET        0x004D157Eu
#define RET_LEAVE        0x004D1AEAu
#define RET_ENTER        0x004D1CB2u
#define RET_FATAL0       0x004D242Au
#define RET_FATAL1       0x004D2439u
#define RET_FATAL2       0x004D2530u

/* Guest memory through the function's local copy of g_mem. */
#define GR8(a) ((uint32_t)gm[(uint32_t)(a)])
#define GR16(a) rt_ld16(gm + (uint32_t)(a))
#define GR32(a) rt_ld32(gm + (uint32_t)(a))
#define GW8(a, v) (gm[(uint32_t)(a)] = (uint8_t)(v))
#define GW32(a, v) rt_st32(gm + (uint32_t)(a), (v))

static inline int32_t sar(uint32_t v, uint32_t n) { return (int32_t)v >> (n & 31u); }
static inline uint32_t f2u(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static inline float u2f(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
static inline uint32_t iabs(uint32_t v) { return (int32_t)v < 0 ? 0u - v : v; }

/* Registers of a guest call (the callee may push any of them). */
typedef struct Regs {
    uint32_t eax, ecx, edx, ebx, ebp, esi, edi;
} Regs;

static inline __attribute__((always_inline)) uint64_t call_guest(Ctx *c, NativeFsw *fsw, uint32_t fn, uint32_t esp,
                                                                  uint32_t ret_addr, const Regs *r,
                                                                  const GuestFunc **ic)
{
    c->r[2] = r->edx;
    c->r[3] = r->ebx;
    c->r[5] = r->ebp;
    c->r[6] = r->esi;
    c->r[7] = r->edi;
    c->fsw = native_fsw(fsw);
    uint64_t rv = ic ? native_call_ic(c, fn, esp, ret_addr, r->ecx, r->eax, ic)
                     : native_call(c, fn, esp, ret_addr, r->ecx, r->eax);
    native_fsw_sync(fsw, c);
    return rv;
}

/* The guest's error report, which never returns (it waits forever after it). */
static void fatal(Ctx *c, uint32_t sw, uint32_t S, uint32_t msg, uint32_t ret_addr, const Regs *r)
{
    NativeFsw fsw = { sw, 0, 0.0, 0.0 };
    WR32(S - 4u, msg);
    call_guest(c, &fsw, FN_FATAL, S - 4u, ret_addr, r, NULL);
    for (;;) {
    }
}

/* Removes a patch from its pool: the last one takes its place. */
static void pool_remove(uint32_t pool, uint32_t p)
{
    uint8_t *const gm = g_mem;
    uint32_t posoff = GR32(pool + 0x14u);
    uint32_t pos = GR32(posoff + p);
    uint32_t last = GR32(pool + 8u) - 1u;
    GW32(pool + 8u, last);
    if (pos == last)
        return;
    uint32_t idx = GR32(pool + 4u);
    uint32_t lp = GR32(idx + 4u * last) * GR32(pool + 0x10u) + GR32(pool);
    uint32_t moved = GR32(idx + 4u * pos);
    uint32_t lpos = GR32(posoff + lp);
    GW32(idx + 4u * pos, GR32(idx + 4u * lpos));
    GW32(GR32(pool + 4u) + 4u * lpos, moved);
    GW32(GR32(pool + 0x14u) + lp, pos);
}

/* Appends {kind, obj} to the object list. */
static void list_object(uint32_t obj, uint32_t kind)
{
    uint8_t *const gm = g_mem;
    GW32(GR32(G_OBJ_LIST) + 4u, obj);
    GW8(GR32(G_OBJ_LIST), kind);
    GW32(G_OBJ_LIST, GR32(G_OBJ_LIST) + 8u);
    GW32(G_OBJ_COUNT, GR32(G_OBJ_COUNT) + 1u);
}

int rt_native_terrain_collect(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(FN_TERRAIN_COLLECT);
    uint8_t *const gm = g_mem;
    const uint32_t ebp = esp - 4u;                          /* push ebp; mov ebp, esp */
    const uint32_t S = ((esp - 4u) & ~7u) - 0x640u;         /* and esp, -8; sub esp, 0x634; 3 pushes */
    const uint32_t ebx0 = c->r[3], ebp0 = c->r[5], esi0 = c->r[6], edi0 = c->r[7];
    GW32(ebp, ebp0);
    GW32(S + 8u, ebx0);
    GW32(S + 4u, esi0);
    GW32(S + 0u, edi0);

    /* The guest's locals (named by frame offset), starting with the stale stack
     * contents; all are written back at the end. */
#define LD(off) rt_ld32(gm + S + (off))
    uint32_t s0c = LD(0x0C), s10 = LD(0x10), s14 = LD(0x14), s18 = LD(0x18), s1c = LD(0x1C), s20 = LD(0x20);
    uint32_t s24 = LD(0x24), s28 = LD(0x28), s2c = LD(0x2C), s30 = LD(0x30), s34 = LD(0x34), s38 = LD(0x38);
    uint32_t s3c = LD(0x3C), s40 = LD(0x40), s44 = LD(0x44), s48 = LD(0x48), s4c = LD(0x4C), s50 = LD(0x50);
    uint32_t s54 = LD(0x54), s58 = LD(0x58), s5c = LD(0x5C), s60 = LD(0x60), s64 = LD(0x64), s68 = LD(0x68);
    uint32_t s6c = LD(0x6C), s70 = LD(0x70), s74 = LD(0x74), s78 = LD(0x78), s7c = LD(0x7C), s80 = LD(0x80);
    uint32_t s84 = LD(0x84), s88 = LD(0x88), s8c = LD(0x8C), s90 = LD(0x90), s94 = LD(0x94), s98 = LD(0x98);
    uint32_t s9c = LD(0x9C), sa0 = LD(0xA0), sbc = LD(0xBC), sc0 = LD(0xC0), sc4 = LD(0xC4), sc8 = LD(0xC8);
    uint32_t scc = LD(0xCC), sd0 = LD(0xD0), sd4 = LD(0xD4);
    uint32_t cur[5];                                        /* 0xA4..0xB4: layer cursors (also scratch) */
    for (int j = 0; j < 5; j++)
        cur[j] = LD(0xA4u + 4u * (uint32_t)j);
#undef LD
    NativeFsw fsw;
    native_fsw_sync(&fsw, c);
    Regs r;
    int64_t q;

    /* ---- setup ---- */
    uint32_t lim = GR32(GR32(G_SETTINGS) + 0xDFCu);
    if (lim) {
        q = rt_fist64(c, native_ldd(lim));
        s14 = (uint32_t)q;
        s18 = (uint32_t)((uint64_t)q >> 32);
        s8c = s14;
    } else {
        s8c = 0xE0000000u;
    }
    const uint32_t terrain = GR32(esp + 4u);
    uint32_t frame = GR32(G_FRAME);
    sa0 = G_REQUESTS_END;
    GW32(G_OVERFLOW, 0);
    s64 = frame & 0xFFFFu;
    s2c = 0;
    s74 = 0;
    uint32_t base = GR32(terrain + 0x0Cu);
    s9c = G_PATCH_LIST;
    s98 = (frame - 1u) & 0xFFFFu;
    sd0 = base;
    sc8 = GR32(G_LOD_DIST);
    r = (Regs){ sc8, s98, base, terrain, ebp, esi0, edi0 };
    call_guest(c, &fsw, FN_RESET_COUNTERS, S, RET_RESET, &r, NULL);

    s84 = GR32(G_TAN_X);
    s90 = GR32(G_TAN_Y);
    double t = atan2((double)u2f(s84), 1.0);
    memmove(gm + S + 0x200u, gm + G_ORDER2, 0x40);
    t = cos(t);
    fsw.sw &= ~0x400u;
    s6c = f2u((float)(native_ldf(K_TAN_NUM) / t * native_ldf(K_TAN_MUL)));
    t = atan2((double)u2f(s90), 1.0);
    t = cos(t);
    fsw.sw &= ~0x400u;
    s50 = f2u((float)(native_ldf(K_TAN_NUM) / t * native_ldf(K_TAN_MUL)));
    for (uint32_t i = 0; i < 0x40u; i++)
        gm[S + 0x1C0u + i] = gm[G_ORDER + 4u * i];

    double camx = native_ldd(G_CAM_POS);
    cur[1] = f2u((float)native_ldd(G_CAM_POS + 8u));       /* 0xA8 */
    cur[2] = f2u((float)native_ldd(G_CAM_POS + 16u));   /* 0xAC */
    memmove(gm + S + 0xD8u, gm + G_CAM_ROT, 0x28);
    double R[9];
    for (int i = 0; i < 9; i++)
        R[i] = native_ldf(G_CAM_ROT + 4u * (uint32_t)i);
    q = rt_fist64(c, camx);
    sbc = (uint32_t)q;
    s38 = 0u - (sbc + GR32(G_ORIGIN));
    q = rt_fist64(c, (double)u2f(cur[1]));
    sc0 = (uint32_t)q;
    s34 = 0u - (GR32(G_ORIGIN + 4u) + sc0);
    q = rt_fist64(c, (double)u2f(cur[2]));
    s14 = (uint32_t)q;
    s18 = (uint32_t)((uint64_t)q >> 32);
    sc4 = s14;
    s3c = 0u - (sc4 + GR32(G_ORIGIN + 8u));
    {
        double ox = (int32_t)s38, oy = (int32_t)s34, oz = (int32_t)s3c;
        cur[0] = f2u((float)((R[2] * oz + R[1] * oy) + R[0] * ox));
        cur[1] = f2u((float)((R[5] * oz + R[4] * oy) + R[3] * ox));
        double z = (R[8] * oz + R[7] * oy) + R[6] * ox;
        s44 = (uint32_t)rt_fist64(c, (double)u2f(cur[0]));
        s48 = (uint32_t)rt_fist64(c, (double)u2f(cur[1]));
        q = rt_fist64(c, z);
        s40 = (uint32_t)q;
    }
    /* The cell's eight corners (+-2^30) and the same rotated to view space. */
    int32_t corner[8][3], rcorner[8][3];
    for (uint32_t k = 0; k < 8; k++) {
        for (uint32_t a = 0; a < 3; a++)
            corner[k][a] = (k >> a & 1u) ? 0x40000000 : (int32_t)0xC0000000u;
        double A = corner[k][0], B = corner[k][1], C = corner[k][2];
        cur[0] = f2u((float)((C * R[2] + B * R[1]) + A * R[0]));
        cur[1] = f2u((float)((C * R[5] + B * R[4]) + A * R[3]));
        double z = (C * R[8] + B * R[7]) + A * R[6];
        rcorner[k][0] = (int32_t)rt_fist64(c, (double)u2f(cur[0]));
        rcorner[k][1] = (int32_t)rt_fist64(c, (double)u2f(cur[1]));
        q = rt_fist64(c, z);
        rcorner[k][2] = (int32_t)q;
        for (uint32_t a = 0; a < 3; a++) {
            GW32(S + 0x100u + 12u * k + 4u * a, (uint32_t)corner[k][a]);
            GW32(S + 0x160u + 12u * k + 4u * a, (uint32_t)rcorner[k][a]);
        }
    }
    s14 = (uint32_t)q;
    s18 = (uint32_t)((uint64_t)q >> 32);

    /* Root of the walk. */
    GW32(S + 0x244u, 0);
    if (base) {
        uint32_t b = GR8(base + 0x38u) & 0xF0u;
        GW32(S + 0x240u, base + 0x20u);
        GW8(base + 0x38u, b);
        GW32(S + 0x244u, 2);
    }
    /* Layers: heads saved and reversed for the walk, cursors set. */
    s1c = 0x0Au;
    s0c = S + 0xA4u;
    s28 = 0;
    s24 = G_LAYER_INDEX - 0x0Au;
    for (uint32_t idx = 0x0Au; idx - 0x0Au < 5u; idx++) {
        int32_t k = (int8_t)GR8(G_LAYER_INDEX + idx - 0x0Au);
        uint32_t head = GR32(terrain + 0x10u + 4u * (uint32_t)k);
        uint32_t off = 0x40u * (idx - 0x0Au);
        if (head) {
            memmove(gm + G_LAYER_SAVE + off, gm + head, 0x40);
            for (uint32_t j = 0; j < 8; j++)
                GW32(head + 0x0Cu + 4u * j, GR32(G_LAYER_SAVE + off + 0x28u - 4u * j));
            uint32_t b = GR8(head + 0x30u), rev = 0;
            for (uint32_t j = 0; j < 8; j++)
                rev |= (b >> j & 1u) << (7u - j);
            GW8(head + 0x30u, rev);
            GW32(G_LAYER_CURSOR + 0x0Cu + off, head);
            GW32(S + 0x244u, GR32(S + 0x244u) | 1u << idx);
            cur[idx - 0x0Au] = G_LAYER_CURSOR + off;
        }
        s0c += 4u;
        s1c = idx + 1u;
        s28 = off + 0x40u;
    }
    s44 -= (uint32_t)rcorner[0][0];
    s48 -= (uint32_t)rcorner[0][1];
    s40 -= (uint32_t)rcorner[0][2];
    s34 -= (uint32_t)corner[0][1];
    s38 -= (uint32_t)corner[0][0];
    s3c -= (uint32_t)corner[0][2];
    uint32_t sp = S + 0x240u;
    s30 = sp;
    uint32_t level = 0xFFFFFFFFu;
    s68 = 0;

    const double k_half = native_ldf(K_HALF), k_near = native_ldd(K_NEAR), k_lod = native_ldf(K_LOD);
    static const GuestFunc *ic_leave = g_guest_func_hash, *ic_enter = g_guest_func_hash;

    /* ---- the walk ---- */
    do {
        uint32_t fl = GR32(sp + 4u);
        uint32_t ci = (uint32_t)sar(fl, 7) & 7u;
        s60 = (uint32_t)sar(fl, 10);
        s24 = ci;
        s4c = S + 0x160u + 12u * ci;
        s54 = S + 0x100u + 12u * ci;
        if (fl & 1u) {
            /* Second visit: leave the node. */
            if (s60) {
                s30 = S + 0xA4u;
                s0c = 1;
                for (s1c = 5; s1c != 0;) {
                    if (s60 & s0c) {
                        uint32_t j = (s30 - (S + 0xA4u)) / 4u;
                        uint32_t cu = cur[j];
                        s24 = cu;
                        cur[j] = GR32(cu + 8u);
                        if (GR16(cu + 0x38u) & 0x100u) {
                            uint32_t obj = GR32(cu + 0x2Cu);
                            do {
                                if (GR16(obj + 0x5Cu) & 0x100u) {
                                    uint32_t id = GR16(obj + 0x1Cu);
                                    GW32(S - 4u, GR32(obj + 0x48u));
                                    GW32(S - 8u, obj);
                                    r = (Regs){ cu, id, GR32(obj + 0x48u), level, ebp, sp, obj };
                                    call_guest(c, &fsw, GR32(G_LEAVE_FN + 4u * id), S - 8u, RET_LEAVE, &r, &ic_leave);
                                }
                                obj = GR32(obj + 0x58u);
                            } while (obj);
                        }
                        if (GR8(cu + 0x38u) & 2u) {
                            uint32_t o = GR32(cu + 0x2Cu);
                            do {
                                if ((GR8(o + 0x5Cu) & 2u) && GR32(o + 0x60u) == 0)
                                    list_object(o, 5);
                                o = GR32(o + 0x58u);
                            } while (o);
                        }
                    }
                    s0c <<= 1;
                    s30 += 4u;
                    s1c--;
                }
            }
            s44 -= (uint32_t)sar((uint32_t)rcorner[ci][0], level);
            s6c = f2u((float)((double)u2f(s6c) + (double)u2f(s6c)));
            s48 -= (uint32_t)sar((uint32_t)rcorner[ci][1], level);
            s50 = f2u((float)((double)u2f(s50) + (double)u2f(s50)));
            s40 -= (uint32_t)sar((uint32_t)rcorner[ci][2], level);
            s38 -= (uint32_t)sar((uint32_t)corner[ci][0], level);
            s34 -= (uint32_t)sar((uint32_t)corner[ci][1], level);
            s3c -= (uint32_t)sar((uint32_t)corner[ci][2], level);
            level--;
            s68++;
            sp -= 8u;
            s30 = sp;
            continue;
        }

        /* First visit: enter the node, then test its children. */
        fl |= 1u;
        GW32(sp + 4u, fl);
        s10 = 0;
        s70 = 0;
        if (fl & 2u) {
            uint32_t node = GR32(sp);
            s10 = (uint32_t)sar(fl, 4) & 7u;
            scc = node;
            s70 = GR8(node + 0x18u);
        }
        s7c = 0;
        GW32(G_B12BB8, 0);
        if (s60) {
            s0c = 1;
            s1c = S + 0xA4u;
            for (s28 = 5; s28 != 0;) {
                if (s60 & s0c) {
                    uint32_t j = (s1c - (S + 0xA4u)) / 4u;
                    uint32_t child = GR32(cur[j] + 0x0Cu + 4u * ci);
                    cur[j] = child;
                    s20 = child;
                    s7c |= GR8(child + 0x30u);
                    uint32_t ecx_ = GR8(child + 0x38u);
                    if (ecx_ & 0x80u) {
                        uint32_t obj = GR32(child + 0x2Cu);
                        do {
                            if (GR8(obj + 0x5Cu) & 0x80u) {
                                uint32_t id = GR16(obj + 0x1Cu);
                                GW32(S - 4u, GR32(obj + 0x48u));
                                GW32(S - 8u, obj);
                                r = (Regs){ GR32(obj + 0x48u), ecx_, id, level, ebp, sp, obj };
                                call_guest(c, &fsw, GR32(G_ENTER_FN + 4u * id), S - 8u, RET_ENTER, &r, &ic_enter);
                                ecx_ = c->r[1];
                            }
                            obj = GR32(obj + 0x58u);
                        } while (obj);
                    }
                    if (GR8(child + 0x38u) & 1u) {
                        uint32_t o = GR32(child + 0x2Cu);
                        do {
                            if ((GR8(o + 0x5Cu) & 1u) && GR32(o + 0x60u) == 0)
                                list_object(o, 4);
                            o = GR32(o + 0x58u);
                        } while (o);
                    }
                }
                s0c <<= 1;
                s1c += 4u;
                s28--;
            }
        }
        s68--;
        level++;
        s58 = level;
        s44 += (uint32_t)sar((uint32_t)rcorner[ci][0], level);
        s6c = f2u((float)((double)u2f(s6c) * k_half));
        s48 += (uint32_t)sar((uint32_t)rcorner[ci][1], level);
        s40 += (uint32_t)sar((uint32_t)rcorner[ci][2], level);
        s38 += (uint32_t)sar((uint32_t)corner[ci][0], level);
        s50 = f2u((float)((double)u2f(s50) * k_half));
        s34 += (uint32_t)sar((uint32_t)corner[ci][1], level);
        s3c += (uint32_t)sar((uint32_t)corner[ci][2], level);
        s88 = S + 0x1C0u + 8u * s10;
        sd4 = S + 0x200u + 8u * s10;
        if (GR32(G_B12BB8) != 0)
            s70 = 0;
        s4c = S + 0x160u;
        s24 = 0xFFFFFFA0u;                                   /* corners - rotated corners */
        s28 = 0;
        s0c = 1;
        const uint32_t sh = level + 1u;
        const double ax = u2f(s6c), ay = u2f(s50), tanx = u2f(s84), tany = u2f(s90);
        for (s1c = 0x0Cu; s1c < 0x2Cu; s1c += 4u, s0c <<= 1, s28++, s4c += 12u) {
            const uint32_t k = s28;
            int32_t ci2 = (int8_t)GR8(s88);
            s88++;
            s80 = (uint32_t)ci2;
            s78 = 1u << ((uint32_t)ci2 & 31u);
            s5c = s0c & s7c;
            if (s5c == 0 && (s70 & s78) == 0)
                continue;
            /* In front of the camera? */
            s20 = (uint32_t)sar((uint32_t)rcorner[k][2], sh) + s40;
            double zc = (int32_t)s20;
            s10 = f2u((float)zc);
            double zn = k_near - ay;
            native_fcom(&fsw, zn, zc);
            if (!X87_BELOW(zn, zc))
                continue;
            /* Inside the view pyramid horizontally and vertically? */
            s20 = (uint32_t)sar((uint32_t)rcorner[k][0], sh) + s44;
            double xc = (int32_t)s20;
            double lx = (double)u2f(s10) * tanx + ax;
            native_fcom(&fsw, xc, lx);
            if (!X87_BELOW(xc, lx))
                continue;
            native_fcom(&fsw, xc, -lx);
            if (!X87_ABOVE(xc, -lx))
                continue;
            s20 = (uint32_t)sar((uint32_t)rcorner[k][1], sh) + s48;
            double yc = (int32_t)s20;
            double ly = (double)u2f(s10) * tany + ay;
            native_fcom(&fsw, yc, ly);
            if (!X87_BELOW(yc, ly))
                continue;
            native_fcom(&fsw, yc, -ly);
            if (!X87_ABOVE(yc, -ly))
                continue;
            /* Above the height limit? */
            uint32_t cz = (uint32_t)sar((uint32_t)corner[k][2], sh) + s3c;
            s20 = (uint32_t)sar(0x40000000u, sh);
            if ((int32_t)(s20 + cz + sc4) < (int32_t)s8c)
                continue;
            uint32_t cx = (uint32_t)sar((uint32_t)corner[k][0], sh) + s38;
            uint32_t cy = (uint32_t)sar((uint32_t)corner[k][1], sh) + s34;
            s14 = cy;
            /* Level of detail: distance in cells (largest coordinate). */
            uint32_t m = iabs(cx);
            if ((int32_t)iabs(cy) > (int32_t)m)
                m = iabs(cy);
            if ((int32_t)iabs(cz) > (int32_t)m)
                m = iabs(cz);
            s10 = 0;
            uint32_t h = s20;
            s20 = 0;
            s54 = (uint32_t)sar(m - h, s68 + 0x19u);
            if (s5c != 0) {
                double d = (double)(int32_t)s54 * native_ldf(G_TAN_X);
                native_fcom(&fsw, d, k_lod);
                if (X87_BELOW(d, k_lod)) {
                    uint32_t bit = 1;
                    for (s5c = 5; s5c != 0; s5c--, bit <<= 1) {
                        uint32_t j = 5u - s5c;
                        if (!(s60 & bit) || !(GR8(cur[j] + 0x30u) & (s0c & 0xFFu)))
                            continue;
                        uint32_t ch = GR32(cur[j] + s1c);
                        if ((GR16(ch + 0x3Au) | GR16(ch + 0x38u)) & 0x183u) {
                            s10 |= bit << 10;
                            s20 = 1;
                        }
                    }
                }
            }
            uint32_t push_node = 0;
            int push = 0;
            if (s70 & s78) {
                uint32_t child = GR16(scc + 2u * s80) * 32u + sd0;
                s94 = child;
                uint32_t ord = GR8(scc + s80 + 0x10u);
                s80 = (uint32_t)(int32_t)(int8_t)GR8(sd4 + ord);
                if ((int32_t)s54 < (int32_t)sc8 && (int32_t)(s58 + 1u) < 16) {
                    s10 |= s80 << 4 | 2u;
                    push = 1;
                    push_node = child;
                } else if ((int32_t)(s58 + 1u) > 6) {
                    /* A patch: from the cache, or a request for a new one. */
                    uint32_t px = cx + sbc, pz = cz + sc4, py = s14 + sc0;
                    s5c = px;
                    uint32_t hsh = (uint32_t)sar(((uint32_t)sar(pz, 2) & 0x1FFC000u) ^ (px & 0x1FFC000u), 3);
                    hsh ^= py & 0x3FF800u;
                    s78 = py;
                    hsh = (uint32_t)sar(hsh, 5);
                    s14 = pz;
                    hsh ^= (pz ^ py ^ px) & 0x1FFC0u;
                    hsh = (uint32_t)sar(hsh, 6);
                    uint32_t e = G_CACHE + hsh * CACHE_SIZE;
                    for (;;) {
                        uint32_t ef = GR32(e + 0x10u);
                        if (ef == 0xFFFFFFFFu ||
                            (s98 == ef && GR32(e) == px && GR32(e + 4u) == py && GR32(e + 8u) == pz))
                            break;
                        hsh = (hsh + 1u) & 0x7FFu;
                        e = G_CACHE + hsh * CACHE_SIZE;
                    }
                    int fresh = 1;
                    if (s98 == GR32(e + 0x10u)) {
                        uint32_t patch = GR32(e + 0x18u);
                        GW32(e + 0x10u, s64);
                        fresh = 0;
                        if (patch == 0) {
                            r = (Regs){ e, 0, 0, pz, ebp, px, py };
                            fatal(c, native_fsw(&fsw), S, MSG_NO_PATCH, RET_FATAL0, &r);
                        }
                        GW32(s9c, patch);
                        s9c += 4u;
                        s74++;
                    } else {
                        uint32_t patch = GR32(e + 0x18u);
                        if (patch != 0) {
                            r = (Regs){ e, patch, 1, pz, ebp, px, py };
                            fatal(c, native_fsw(&fsw), S, MSG_STALE_PATCH, RET_FATAL1, &r);
                        }
                        GW32(e + 0x14u, 3);
                        GW32(e + 0x0Cu, s54);
                        GW32(e, px);
                        GW32(e + 4u, py);
                        GW32(e + 8u, pz);
                        GW32(e + 0x10u, s64);
                    }
                    if ((int32_t)s2c > 0xFFF) {
                        GW32(G_OVERFLOW, GR32(G_OVERFLOW) + 1u);
                    } else if (fresh) {
                        uint32_t rp = sa0;
                        GW32(rp - 4u, e);
                        uint32_t tex = GR32(G_B97D24);
                        GW32(rp - 8u, GR32(G_BA5060));
                        if (tex)
                            GW32(rp - 12u, GR32(G_B97B1C + 4u * tex) | GR32(G_BA2058) << 16);
                        else
                            GW32(rp - 12u, GR32(G_BA2058) << 16 | 0xFFFFu);
                        GW32(rp - 16u, GR32(s94 + 0x1Cu));
                        GW32(rp - 20u, s80);
                        GW32(rp - 24u, s58 + 1u);
                        rt_stf64(gm + rp - 32u, (double)(int32_t)s14);
                        rt_stf64(gm + rp - 40u, (double)(int32_t)s78);
                        rt_stf64(gm + rp - 48u, (double)(int32_t)s5c);
                        sa0 = rp - 48u + 0x60u;
                        s2c++;
                    }
                }
            }
            if (!push && s20 != 0) {
                push = 1;
                push_node = s94;
            }
            if (push) {
                sp = s30 + 8u;
                s30 = sp;
                GW32(sp + 4u, s28 << 7 | s10);
                GW32(sp, push_node);
            }
            sp = s30;
        }
    } while (sp >= S + 0x240u);

    /* ---- cleanup ---- */
    for (uint32_t k = 0; k < 5; k++) {
        uint32_t head = GR32(terrain + 0x10u + 4u * (uint32_t)(int32_t)(int8_t)GR8(G_LAYER_INDEX + k));
        if (head)
            memmove(gm + head, gm + G_LAYER_SAVE + 0x40u * k, 0x40);
    }
    GW32(G_REQUEST_COUNT, s2c);
    GW32(G_PATCH_COUNT, s74);
    uint32_t hi = s64 & 0xFFFF0000u, lo = s64 & 0xFFFFu;
    s14 = hi;
    s64 = lo;
    for (uint32_t e = G_CACHE; e < G_CACHE + CACHE_COUNT * CACHE_SIZE; e += CACHE_SIZE) {
        uint32_t ef = GR32(e + 0x10u);
        if ((ef & 0xFFFF0000u) != hi || (ef & 0xFFFFu) == lo) {
            if ((ef & 0xFFFFu) + 1u >= lo)
                continue;
        }
        /* Not used this frame: evict. */
        uint32_t patch = GR32(e + 0x18u);
        if (patch) {
            uint32_t type = GR32(e + 0x14u);
            if (type > 2u) {
                r = (Regs){ patch, e + 0x18u, type, terrain, ebp, lo, 0 };
                r.edi = (ef & 0xFFFF0000u) != hi ? ef & 0xFFFF0000u : ef & 0xFFFFu;
                fatal(c, native_fsw(&fsw), S, MSG_BAD_POOL, RET_FATAL2, &r);
            }
            pool_remove(type == 0 ? G_POOL0 : type == 1 ? G_POOL1 : G_POOL2, patch);
            GW32(e + 0x18u, 0);
        }
        GW32(e + 0x10u, 0xFFFFFFFFu);
    }
    s24 = CACHE_COUNT;
    s2c = G_CACHE + CACHE_COUNT * CACHE_SIZE + 0x18u;

    /* The frame as the guest leaves it. */
    const uint32_t vals[] = { s0c, s10, s14, s18, s1c, s20, s24, s28, s2c, s30, s34, s38, s3c, s40, s44, s48,
                              s4c, s50, s54, s58, s5c, s60, s64, s68, s6c, s70, s74, s78, s7c, s80, s84, s88,
                              s8c, s90, s94, s98, s9c, sa0, cur[0], cur[1], cur[2], cur[3], cur[4] };
    for (uint32_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
        GW32(S + 0x0Cu + 4u * i, vals[i]);
    GW32(S + 0xBCu, sbc);
    GW32(S + 0xC0u, sc0);
    GW32(S + 0xC4u, sc4);
    GW32(S + 0xC8u, sc8);
    GW32(S + 0xCCu, scc);
    GW32(S + 0xD0u, sd0);
    GW32(S + 0xD4u, sd4);

    c->fsw = native_fsw(&fsw);   /* the guest code stores its status word on return */
    c->r[1] = s2c;
    c->r[2] = s14;
    c->r[3] = ebx0;
    c->r[5] = ebp0;
    c->r[6] = esi0;
    c->r[7] = edi0;
    (void)ecx;
    (void)eax;
    *ret = ((uint64_t)(esp + 4u) << 32) | CACHE_COUNT;
    return 1;
}
