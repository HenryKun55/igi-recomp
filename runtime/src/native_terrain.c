/* Hand-decompiled terrain renderer of IGI.exe.
 *
 *   0x00512C30  terrain_render   draws every visible terrain patch of the frame
 *   0x00514760  clip_triangle    clips one triangle against the near plane and
 *                                the screen rectangle, recursively
 *
 * gen.py (NATIVE table) routes the entry of 0x512C30 here when the guest bytes of
 * both functions match. The native version replaces the whole call: the
 * recompiled 0x514760 is only ever called from 0x512C30 and from itself.
 *
 * Two versions: the bit-exact one (terrain_exact=1 in igi.cfg, IGI_TERRAIN_EXACT=1,
 * and always under IGI_NATIVE_CHECK), described here, and the default fast one
 * in single precision (see "fast version" below; terrainbench.py fast compares
 * the two).
 *
 * Arithmetic follows the guest's x87 code as the recompiler models it (every x87
 * register a double, rounded to float wherever the game stores a float): the
 * same operations in the same order, so the results are bit-identical to the
 * recompiled code. tools/recomp/terrainbench.py checks that on synthetic frames
 * and on frames captured from the running game (IGI_TERRAIN_DUMP).
 *
 * Differences from the guest code, reachable only with corrupt patch data (the
 * guest reads or writes unrelated stack memory in those cases):
 *   - a frame with a patch of more than 64 vertices or 256 strip entries is left
 *     to the recompiled code (terrain_supported);
 *   - morph parents and colour offsets outside the patch's own tables read zero.
 * Unreferenced stack contents (the colour table rows of morph vertices, which the
 * guest never writes) read as zero too; they are counted and logged once. */
#include "native.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

/* ---- guest globals ------------------------------------------------------ */

/* Camera (written by the renderer setup before the terrain pass). */
#define G_CAM_POS        0x00BCAB08u   /* double[3] world position */
#define G_CAM_ROT        0x00BCAAE0u   /* float[10]: 3x3 world-to-view rows, then one unused float */
#define G_CLIP_LEFT      0x00BCABD0u   /* float: screen clip rectangle */
#define G_CLIP_TOP       0x00BCABD4u
#define G_CLIP_RIGHT     0x00BCABD8u
#define G_CLIP_BOTTOM    0x00BCABDCu
#define G_FOG_DEPTH      0x00BCABECu   /* float: view depth factor of the fog and 1/w of the clipper */
#define G_RHW_SCALE      0x00BCAD70u   /* float: rhw = scale / depth */
#define G_ZFAR           0x00BCAD8Cu   /* float */
#define G_ZNEAR_W        0x00BCADB4u   /* float */
#define G_PROJ_A         0x00BCADA0u   /* float: projection parameters for the depth mapping */
#define G_PROJ_B         0x00BCADC8u
#define G_NEAR           0x00BCADC0u   /* float: near plane distance */
#define G_PATCH_RANGE    0x00BCADCCu   /* float: patches further away are skipped */
#define G_ZENABLE        0x00BCAD78u   /* D3DRENDERSTATE_ZENABLE value */
#define G_DEPTH_CLEAR    0x00BCADE8u   /* four objects whose +4 field is reset every frame */
#define G_DEPTH_CLEAR2   0x00BCAD7Cu
#define G_DEPTH_CLEAR3   0x00BCADACu
#define G_DEPTH_CLEAR4   0x00BCADC4u
#define G_PROJ_SY        0x00C311A0u   /* float: screen y scale */
#define G_PROJ_SX        0x00C311A4u   /* float: screen x scale */
#define G_PROJ_CX        0x00C311B0u   /* float: screen centre */
#define G_PROJ_CY        0x00C311B4u
/* Fog. */
#define G_FOG_DENSITY    0x00A7DA18u   /* float */
#define G_FOG_R          0x00A7DA30u   /* float[3] fog colour, 0..1 */
#define G_FOG_COLOR      0x00A7E068u   /* packed fog colour (output) */
/* Depth mapping computed here: z = rhw_z * z_scale + z_offset. */
#define G_Z_SCALE        0x00A7F894u
#define G_Z_OFFSET       0x00A7E074u
/* Level of detail. */
#define G_LOD_BIAS       0x00C31088u   /* int */
#define G_LOD_SCALE      0x00C28FC0u   /* float */
#define G_STAT_PATCHES   0x00C28FBCu   /* patches submitted (statistics) */
#define G_STAT_2         0x00C28FB8u
/* Patch list. */
#define G_PATCH_COUNT    0x00B16C9Cu
#define G_PATCH_LIST     0x00B12C60u   /* u32[G_PATCH_COUNT] patch record addresses */
/* Direct3D. */
#define G_D3D_DEVICE     0x005CA114u   /* IDirect3DDevice7 * */
#define G_VIEWPORT       0x00A7F860u   /* D3DVIEWPORT7 */
#define G_VIEWPORT_SRC   0x00C28B40u   /* screen size source (+4 width, +8 height) */
#define G_VBUF           0x00A7F890u   /* IDirect3DVertexBuffer7 * */
#define G_VBUF_PTR       0x00A7F89Cu   /* locked vertex buffer write pointer */
#define G_VBUF_COUNT     0x00A7E078u   /* vertices written this frame */
#define G_STRIP_FLIP     0x00A7E06Cu   /* winding of the current patch's strips */
#define G_CLIP_DEPTH     0x00A7E070u   /* clipper call counter (see clip_triangle) */
/* Batch pool (0x514680 finds or creates the batch of a material and kind). */
#define G_BATCH_POOL     0x00A7F878u
#define G_BATCH_INDEX    0x00A7F87Cu   /* u32[] pool slot of each live batch */
#define G_BATCH_COUNT    0x00A7F880u
#define G_BATCH_STRIDE   0x00A7F888u
#define G_BATCH_POS_OFF  0x00A7F88Cu   /* offset of a batch's own position in the index */

/* Batch (0x2F00 bytes in the pool). */
#define BATCH_INDICES    0x0008u       /* u16[] triangle list */
#define BATCH_TRIS       0x2EE8u       /* triangles in BATCH_INDICES */
#define BATCH_MATERIAL   0x2EECu       /* material; its +0x20 is the texture */
#define BATCH_KIND       0x2EF0u       /* 0 base pass, 1 blend passes, 2 detail */

/* Guest functions. */
#define FN_GET_BATCH     0x00514680u   /* batch(material, kind) */
#define FN_KIND_STATES   0x005144E0u   /* render states for batches of a kind */
#define FN_FREE_BATCH    0x004AF960u

/* COM vtable offsets. */
#define D3DDEV_SET_TRANSFORM   0x2Cu
#define D3DDEV_SET_VIEWPORT    0x34u
#define D3DDEV_SET_RENDERSTATE 0x50u
#define D3DDEV_DRAW_INDEXED_VB 0x80u
#define D3DDEV_SET_TEXTURE     0x8Cu
#define D3DDEV_SET_TSS         0x94u
#define D3DVB_LOCK             0x0Cu
#define D3DVB_UNLOCK           0x10u

/* Limits of the guest's stack arrays. */
#define MAX_VERTS 64
#define MAX_STRIP 256

/* Outcode bits of a vertex. */
#define OUT_LEFT   0x01u
#define OUT_TOP    0x02u
#define OUT_RIGHT  0x04u
#define OUT_BOTTOM 0x08u
#define OUT_NEAR   0x10u
/* Triangle flag (strip entry): culled. Other bits: outcodes to clip against. */
#define TRI_CULLED 0x20u

#define LOG2E 1.4426950408889634   /* fldl2e as the recompiler loads it */

/* x87 comparisons as the game tests them: "below" is C0, which is also set for
 * unordered operands, "above" needs C0 = C3 = 0. */
#define BELOW(a, b) (!((a) >= (b)))
#define ABOVE(a, b) ((a) > (b))

/* a + (b - a) * t, computed as the guest does: ((b - a) * t) + a. */
#define LERP(b, a, t) ((float)(((double)(b) - (double)(a)) * (t) + (double)(a)))

/* ---- data structures ---------------------------------------------------- */

/* D3DTLVERTEX: the vertex format of the terrain vertex buffer. */
typedef struct TLVertex {
    float sx, sy, sz, rhw;
    uint32_t color, specular;       /* specular alpha: fog factor */
    float tu, tv;
} TLVertex;

/* Clipper vertex (0x40 bytes in the guest, passed by value). Its +0x24 points to
 * the view-space position, which only the near-plane step of one clipper call
 * uses: here that is a local of clip_near. */
typedef struct ClipVert {
    TLVertex v;         /* +0x00; sz holds 1/depth while clipping */
    uint32_t code;      /* +0x20 outcode */
    float attr[6];      /* +0x28 r, g, b, tu, tv, fog, multiplied by 1/depth */
} ClipVert;

/* Where the clipper appends its output (the guest passes a pointer to this). */
typedef struct ClipOut {
    uint32_t vb;        /* vertex buffer at the start of the pass */
    uint32_t nverts;    /* vertices of the pass written so far */
    uint32_t idx;       /* first index of the pass in the batch */
    uint32_t ntris;     /* triangles of the pass written so far */
    uint32_t vbase;     /* vertex buffer index of the pass's first vertex */
} ClipOut;

/* Per-frame constants (guest globals that do not change during the call). */
typedef struct Frame {
    Ctx *c;
    uint32_t sp;                    /* guest stack pointer for calls */
    double cam[3];
    float ncam[3];                  /* -(float)cam */
    float rot[9];
    float lod_bias, lod_scale;
    float near_z, patch_range;
    float left, top, right, bottom;
    float proj_sx, proj_sy, proj_cx, proj_cy;
    float rhw_scale, z_scale, z_offset;
    float fog_depth, fog_density;
    float fog_k;                    /* fast version: fog exponent (log2) per unit of depth */
    int chop;                       /* fast version: fist truncates (the game's mode while drawing) */
} Frame;

/* Per-patch working set (the guest keeps these in its stack frame). */
typedef struct PatchWork {
    TLVertex vtx[256];
    float morph[256];               /* morph factor of each base vertex */
    uint8_t code[256];              /* outcode of each vertex */
    uint8_t unused[256];            /* 1: not referenced by any drawn triangle */
    uint8_t tri[MAX_STRIP];         /* per strip entry: 0 draw, TRI_CULLED, or outcodes to clip */
    uint32_t blend[MAX_VERTS][8];   /* per base vertex: colours of the blend passes */
    ClipVert clip[256];             /* clipper input of each vertex in the current pass */
    uint32_t clip_pass[256];        /* pass number of clip[i] */
    uint32_t pass;                  /* pass number (counts all passes) */
} PatchWork;

static uint32_t stale_reads;        /* reads of data the guest leaves undefined */

/* ---- helpers ------------------------------------------------------------ */

static inline float rdf(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline float gf(uint32_t a) { return rdf(g_mem + a); }
static inline uint32_t fist(Ctx *c, double v) { return (uint32_t)rt_fist64(c, v); }
/* fist of a value known to be in [0, 2^31) (no NaN): the range check is left out
 * when truncating, the game's mode while it draws. */
static inline uint32_t fist_u31(Ctx *c, double v)
{
    return (c->fcw & 0x0C00u) == 0x0C00u ? (uint32_t)(int32_t)v : fist(c, v);
}

static void note_stale(void)
{
    if (stale_reads++ == 0)
        rt_log("native terrain: patch data references undefined stack data (reads zero)");
}

/* Calls guest code (function or COM method) with stack arguments; returns eax. */
static uint32_t guest_call_args(Ctx *c, uint32_t sp, uint32_t fn, uint32_t n, const uint32_t *args)
{
    uint32_t p = sp - 4u * (n + 1u);
    WR32(p, 0x00512C30u);           /* return address: the caller's entry, for diagnostics */
    for (uint32_t i = 0; i < n; i++)
        WR32(p + 4u + 4u * i, args[i]);
    return (uint32_t)rt_call_indirect(c, fn, p, 0, 0);
}

static uint32_t com_call(const Frame *f, uint32_t obj, uint32_t slot, uint32_t n, const uint32_t *args)
{
    uint32_t a[8];
    a[0] = obj;
    for (uint32_t i = 0; i < n; i++)
        a[1 + i] = args[i];
    return guest_call_args(f->c, f->sp, RD32(RD32(obj) + slot), n + 1u, a);
}

static void set_render_state(const Frame *f, uint32_t state, uint32_t value)
{
    uint32_t a[2] = { state, value };
    com_call(f, RD32(G_D3D_DEVICE), D3DDEV_SET_RENDERSTATE, 2, a);
}

static void set_tss(const Frame *f, uint32_t stage, uint32_t type, uint32_t value)
{
    uint32_t a[3] = { stage, type, value };
    com_call(f, RD32(G_D3D_DEVICE), D3DDEV_SET_TSS, 3, a);
}

static uint32_t get_batch(const Frame *f, uint32_t material, uint32_t kind)
{
    uint32_t a[2] = { material, kind };
    return guest_call_args(f->c, f->sp, FN_GET_BATCH, 2, a);
}

static void copy_vertex(uint32_t dst, const TLVertex *v)
{
    rt_move_small(g_mem + dst, (const uint8_t *)v, sizeof(*v));
}

/* ---- clipper (0x514760) ------------------------------------------------- */

static uint32_t screen_code(const Frame *f, double x, double y)
{
    return (uint32_t)BELOW(x, f->left) | (uint32_t)BELOW(y, f->top) << 1 |
           (uint32_t)ABOVE(x, f->right) << 2 | (uint32_t)ABOVE(y, f->bottom) << 3;
}

/* Interpolation of the screen position, depth and attributes of b towards a. */
static void lerp_screen(ClipVert *b, const ClipVert *a, double t)
{
    b->v.sx = LERP(b->v.sx, a->v.sx, t);
    b->v.sy = LERP(b->v.sy, a->v.sy, t);
    b->v.sz = LERP(b->v.sz, a->v.sz, t);
    for (int k = 0; k < 6; k++)
        b->attr[k] = LERP(b->attr[k], a->attr[k], t);
}

/* Moves b (view-space position bc, attributes) towards a onto the near plane. */
static void near_lerp(const Frame *f, ClipVert *b, float *bc, const ClipVert *a, const float *ac, int nattr,
                      double *t_out)
{
    double t = ((double)f->near_z - ac[2]) / ((double)bc[2] - ac[2]);
    for (int k = 0; k < 3; k++)
        bc[k] = LERP(bc[k], ac[k], t);
    for (int k = 0; k < nattr; k++)
        b->attr[k] = LERP(b->attr[k], a->attr[k], t);
    *t_out = t;
}

/* Screen position of a vertex just moved onto the near plane. The guest has two
 * copies of this with different multiplication orders; `alt` selects the first. */
static void near_project(const Frame *f, ClipVert *v, const float *cam, int alt)
{
    double r = 1.0 / (double)cam[2];
    v->v.sz = (float)r;
    if (alt) {
        v->v.sx = (float)(r * f->proj_sx * cam[0] + f->proj_cx);
        v->v.sy = (float)((double)f->proj_sy * cam[1] * v->v.sz + f->proj_cy);
    } else {
        v->v.sx = (float)(r * cam[0] * f->proj_sx + f->proj_cx);
        v->v.sy = (float)((double)cam[1] * v->v.sz * f->proj_sy + f->proj_cy);
    }
    v->code = screen_code(f, v->v.sx, v->v.sy);
}

/* Writes the finished triangle: vertices to the vertex buffer, indices to the batch. */
static void clip_emit(const Frame *f, ClipVert *v, ClipOut *out)
{
    Ctx *c = f->c;
    uint32_t ip = out->idx + out->ntris * 6u;
    out->ntris++;
    for (int k = 0; k < 3; k++, ip += 2u) {
        ClipVert *p = &v[k];
        double w = p->v.sz;
        float r = (float)(1.0 / w);
        p->v.rhw = (float)(w * f->rhw_scale);
        p->v.sz = (float)(w * f->z_scale + f->z_offset);
        WR16(ip, (out->nverts + out->vbase) & 0xFFFFu);
        TLVertex o = p->v;
        o.tu = (float)((double)r * p->attr[3]);
        o.tv = (float)((double)r * p->attr[4]);
        o.specular = fist(c, (double)r * p->attr[5]) << 24;
        uint32_t cr = fist(c, (double)r * p->attr[0]);
        uint32_t cg = fist(c, (double)r * p->attr[1]);
        uint32_t cb = fist(c, (double)r * p->attr[2]);
        o.color = (((cr << 8) | cg) << 8) | cb;
        copy_vertex(out->vb + out->nverts * 32u, &o);
        out->nverts++;
    }
}

static void clip_triangle(const Frame *f, ClipVert *v, ClipOut *out);

/* Index of the first vertex whose outcode has (want != 0) or lacks (want == 0) bit. */
static int first_with(const ClipVert *v, uint32_t bit, int want)
{
    if (((v[0].code & bit) != 0) == want)
        return 0;
    if (((v[1].code & bit) != 0) == want)
        return 1;
    return 2;
}

static int count_out(const ClipVert *v, uint32_t bit)
{
    return (int)((v[0].code & bit) != 0) + (int)((v[1].code & bit) != 0) + (int)((v[2].code & bit) != 0);
}

/* The second triangle of a quad left by clipping one vertex: (in2, tmp, o). */
static void clip_second(const Frame *f, const ClipVert *in2, const ClipVert *tmp, const ClipVert *o, ClipOut *out)
{
    ClipVert sub[3];
    sub[0] = *in2;
    sub[1] = *tmp;
    sub[2] = *o;
    clip_triangle(f, sub, out);
}

/* Near plane: back to view space (the screen position of a vertex in front of the
 * plane is divided by its 1/depth), attributes undivided, clip, and divide the
 * attributes again. */
static void clip_near(const Frame *f, ClipVert *v, ClipOut *out, uint32_t *any, uint32_t *all)
{
    float cam[3][3];
    for (int k = 0; k < 3; k++) {
        ClipVert *p = &v[k];
        if (!(p->code & OUT_NEAR)) {
            double w = p->v.sz;
            cam[k][2] = (float)(1.0 / w);
            cam[k][0] = (float)(((double)p->v.sx - f->proj_cx) / ((double)f->proj_sx * w));
            cam[k][1] = (float)(((double)p->v.sy - f->proj_cy) / ((double)f->proj_sy * w));
        } else {
            cam[k][0] = p->v.sx;
            cam[k][1] = p->v.sy;
            cam[k][2] = p->v.sz;
        }
        p->attr[0] = (float)((p->v.color >> 16) & 0xFFu);
        p->attr[1] = (float)((p->v.color >> 8) & 0xFFu);
        p->attr[2] = (float)(p->v.color & 0xFFu);
        p->attr[3] = p->v.tu;
        p->attr[4] = p->v.tv;
        p->attr[5] = (float)(p->v.specular >> 24);
    }
    double t;
    if (count_out(v, OUT_NEAR) == 2) {
        int k = first_with(v, OUT_NEAR, 0), k1 = (k + 1) % 3, k2 = (k + 2) % 3;
        near_lerp(f, &v[k1], cam[k1], &v[k], cam[k], 6, &t);
        near_project(f, &v[k1], cam[k1], 1);
        near_lerp(f, &v[k2], cam[k2], &v[k], cam[k], 6, &t);
        near_project(f, &v[k2], cam[k2], 0);
        *any = v[0].code | v[1].code | v[2].code;
        *all = v[0].code & v[1].code & v[2].code;
        for (int j = 0; j < 3; j++)
            for (int m = 0; m < 6; m++)
                v[j].attr[m] = (float)((double)v[j].attr[m] * v[j].v.sz);
        return;
    }
    /* One vertex behind: the quad in front becomes two triangles. */
    int k = first_with(v, OUT_NEAR, 1), k1 = (k + 1) % 3, k2 = (k + 2) % 3;
    ClipVert *o = &v[k], *in2 = &v[k2];
    ClipVert sub[3];
    ClipVert *tmp = &sub[1];
    float tcam[3] = { cam[k][0], cam[k][1], cam[k][2] };
    *tmp = *o;
    near_lerp(f, o, cam[k], &v[k1], cam[k1], 6, &t);
    near_project(f, o, cam[k], 0);
    near_lerp(f, tmp, tcam, in2, cam[k2], 5, &t);
    double fog = ((double)tmp->attr[5] - in2->attr[5]) * t + in2->attr[5];   /* kept unrounded */
    double r = 1.0 / (double)tcam[2];
    tmp->v.sz = (float)r;
    double x = r * tcam[0] * f->proj_sx + f->proj_cx;
    tmp->v.sx = (float)x;
    double y = (double)tmp->v.sz * tcam[1] * f->proj_sy + f->proj_cy;
    tmp->v.sy = (float)y;
    tmp->code = (uint32_t)BELOW(x, f->left) | (uint32_t)BELOW(y, f->top) << 1 |
                (uint32_t)ABOVE(x, f->right) << 2 | (uint32_t)ABOVE((double)tmp->v.sy, f->bottom) << 3;
    *any = v[0].code | v[1].code | v[2].code;
    *all = v[0].code & v[1].code & v[2].code;
    for (int j = 0; j < 3; j++)
        for (int m = 0; m < 6; m++)
            v[j].attr[m] = (float)((double)v[j].v.sz * v[j].attr[m]);
    for (int m = 0; m < 5; m++)
        tmp->attr[m] = (float)((double)tmp->v.sz * tmp->attr[m]);
    tmp->attr[5] = (float)((double)tmp->v.sz * fog);
    sub[0] = *in2;
    sub[2] = *o;
    clip_triangle(f, sub, out);
}

/* Clips the triangle v[0..2] (the guest passes the three vertices by value; v is
 * the callee's copy and is modified). */
static void clip_triangle(const Frame *f, ClipVert *v, ClipOut *out)
{
    WR32(G_CLIP_DEPTH, RD32(G_CLIP_DEPTH) + 1u);
    uint32_t any = v[0].code | v[1].code | v[2].code;
    uint32_t all = v[0].code & v[1].code & v[2].code;
    if (all)
        goto rejected;

    if (any & OUT_NEAR) {
        clip_near(f, v, out, &any, &all);
        if (all)
            goto rejected;
    }

    if (any & OUT_LEFT) {
        if (count_out(v, OUT_LEFT) == 2) {
            int k = first_with(v, OUT_LEFT, 0);
            ClipVert *in = &v[k];
            for (int n = 1; n <= 2; n++) {
                ClipVert *o = &v[(k + n) % 3];
                float t = (float)(((double)f->left - in->v.sx) / ((double)o->v.sx - in->v.sx));
                lerp_screen(o, in, t);
                o->code = screen_code(f, o->v.sx, o->v.sy) & ~OUT_LEFT;
            }
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
        } else {
            int k = first_with(v, OUT_LEFT, 1);
            ClipVert *o = &v[k], *in1 = &v[(k + 1) % 3], *in2 = &v[(k + 2) % 3];
            ClipVert tmp = *o;
            float t = (float)(((double)f->left - in1->v.sx) / ((double)o->v.sx - in1->v.sx));
            lerp_screen(o, in1, t);
            o->code = screen_code(f, o->v.sx, o->v.sy) & ~OUT_LEFT;
            t = (float)(((double)f->left - in2->v.sx) / ((double)tmp.v.sx - in2->v.sx));
            double x = ((double)tmp.v.sx - in2->v.sx) * t + in2->v.sx;
            double y = ((double)tmp.v.sy - in2->v.sy) * t + in2->v.sy;
            tmp.v.sx = (float)x;
            tmp.v.sy = (float)y;
            tmp.v.sz = LERP(tmp.v.sz, in2->v.sz, t);
            for (int j = 0; j < 6; j++)
                tmp.attr[j] = LERP(tmp.attr[j], in2->attr[j], t);
            tmp.code = (uint32_t)BELOW(y, f->top) << 1 | (uint32_t)ABOVE(x, f->right) << 2 |
                       (uint32_t)ABOVE((double)tmp.v.sy, f->bottom) << 3;
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
            clip_second(f, in2, &tmp, o, out);
        }
        if (all)
            goto rejected;
    }

    if (any & OUT_TOP) {
        if (count_out(v, OUT_TOP) == 2) {
            int k = first_with(v, OUT_TOP, 0);
            ClipVert *in = &v[k];
            for (int n = 1; n <= 2; n++) {
                ClipVert *o = &v[(k + n) % 3];
                double t = ((double)f->top - in->v.sy) / ((double)o->v.sy - in->v.sy);
                lerp_screen(o, in, t);
                o->code = (uint32_t)ABOVE(o->v.sx, f->right) << 2 | (uint32_t)ABOVE(o->v.sy, f->bottom) << 3;
            }
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
        } else {
            int k = first_with(v, OUT_TOP, 1);
            ClipVert *o = &v[k], *in1 = &v[(k + 1) % 3], *in2 = &v[(k + 2) % 3];
            ClipVert tmp = *o;
            double t = ((double)f->top - in1->v.sy) / ((double)o->v.sy - in1->v.sy);
            lerp_screen(o, in1, t);
            o->code = (uint32_t)ABOVE(o->v.sx, f->right) << 2 | (uint32_t)ABOVE(o->v.sy, f->bottom) << 3;
            float t2 = (float)(((double)f->top - in2->v.sy) / ((double)tmp.v.sy - in2->v.sy));
            tmp.v.sx = LERP(tmp.v.sx, in2->v.sx, t2);
            double y = ((double)tmp.v.sy - in2->v.sy) * t2 + in2->v.sy;
            tmp.v.sy = (float)y;
            tmp.v.sz = LERP(tmp.v.sz, in2->v.sz, t2);
            for (int j = 0; j < 6; j++)
                tmp.attr[j] = LERP(tmp.attr[j], in2->attr[j], t2);
            tmp.code = (uint32_t)ABOVE(tmp.v.sx, f->right) << 2 | (uint32_t)ABOVE(y, f->bottom) << 3;
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
            clip_second(f, in2, &tmp, o, out);
        }
        if (all)
            goto rejected;
    }

    if (any & OUT_RIGHT) {
        if (count_out(v, OUT_RIGHT) == 2) {
            int k = first_with(v, OUT_RIGHT, 0);
            ClipVert *in = &v[k];
            for (int n = 1; n <= 2; n++) {
                ClipVert *o = &v[(k + n) % 3];
                float t = (float)(((double)f->right - in->v.sx) / ((double)o->v.sx - in->v.sx));
                lerp_screen(o, in, t);
                o->code = (uint32_t)ABOVE(o->v.sy, f->bottom) << 3;
            }
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
        } else {
            int k = first_with(v, OUT_RIGHT, 1);
            ClipVert *o = &v[k], *in1 = &v[(k + 1) % 3], *in2 = &v[(k + 2) % 3];
            ClipVert tmp = *o;
            float t = (float)(((double)f->right - in1->v.sx) / ((double)o->v.sx - in1->v.sx));
            lerp_screen(o, in1, t);
            o->code = (uint32_t)ABOVE(o->v.sy, f->bottom) << 3;
            t = (float)(((double)f->right - in2->v.sx) / ((double)tmp.v.sx - in2->v.sx));
            tmp.v.sx = LERP(tmp.v.sx, in2->v.sx, t);
            double y = ((double)tmp.v.sy - in2->v.sy) * t + in2->v.sy;
            tmp.v.sy = (float)y;
            tmp.v.sz = LERP(tmp.v.sz, in2->v.sz, t);
            for (int j = 0; j < 6; j++)
                tmp.attr[j] = LERP(tmp.attr[j], in2->attr[j], t);
            tmp.code = (uint32_t)ABOVE(y, f->bottom) << 3;
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
            clip_second(f, in2, &tmp, o, out);
        }
        if (all)
            goto rejected;
    }

    if (any & OUT_BOTTOM) {
        if (count_out(v, OUT_BOTTOM) == 2) {
            int k = first_with(v, OUT_BOTTOM, 0);
            ClipVert *in = &v[k];
            for (int n = 1; n <= 2; n++) {
                ClipVert *o = &v[(k + n) % 3];
                double t = ((double)f->bottom - in->v.sy) / ((double)o->v.sy - in->v.sy);
                lerp_screen(o, in, t);
                o->code = 0;
            }
            all = v[0].code & v[1].code & v[2].code;
        } else {
            int k = first_with(v, OUT_BOTTOM, 1);
            ClipVert *o = &v[k], *in1 = &v[(k + 1) % 3], *in2 = &v[(k + 2) % 3];
            ClipVert tmp = *o;
            double t = ((double)f->bottom - in1->v.sy) / ((double)o->v.sy - in1->v.sy);
            lerp_screen(o, in1, t);
            o->code = 0;
            float t2 = (float)(((double)f->bottom - in2->v.sy) / ((double)tmp.v.sy - in2->v.sy));
            lerp_screen(&tmp, in2, t2);
            tmp.code = 0;
            all = v[0].code & v[1].code & v[2].code;
            clip_second(f, in2, &tmp, o, out);
        }
        if (all)
            goto rejected;
    }

    /* The guest does not decrement the call counter on this path. */
    clip_emit(f, v, out);
    return;

rejected:
    WR32(G_CLIP_DEPTH, RD32(G_CLIP_DEPTH) - 1u);
}

/* ---- terrain patches ---------------------------------------------------- */

/* Clipper input for vertex i: attributes divided by depth unless it is behind
 * the near plane (the clipper derives those itself). */
static void make_clip_vert(const Frame *f, const PatchWork *w, uint32_t i, ClipVert *cv)
{
    cv->v = w->vtx[i];
    cv->code = w->code[i];
    if (cv->code & OUT_NEAR)
        return;
    const TLVertex *p = &w->vtx[i];
    double s = (double)f->fog_depth * p->rhw;
    cv->v.sz = (float)s;
    double sf = cv->v.sz;
    cv->attr[0] = (float)((double)((p->color >> 16) & 0xFFu) * s);
    cv->attr[1] = (float)((double)((p->color >> 8) & 0xFFu) * sf);
    cv->attr[2] = (float)((double)(p->color & 0xFFu) * sf);
    cv->attr[3] = (float)((double)p->tu * sf);
    cv->attr[4] = (float)((double)p->tv * sf);
    cv->attr[5] = (float)((double)(p->specular >> 24) * sf);
}

/* Fog byte of a vertex: fist(255 / 2^e) with the guest's x87 exp idiom,
 * fog = 1 / fscale(f2xm1(e - round(e)) + 1, round(e)), clamped at zero.
 *
 * Only the integer reaches the game, so the common case avoids the library
 * exp2 and the division: 2^-e from a polynomial (relative error below 1e-11 for
 * e in [0, 1000)) gives 255 * fog within 1e-8 of the guest's value, whose own
 * error against the exact result is a few ulp. The estimate goes to 20-bit
 * fixed point with one truncating conversion (plus one half for round to
 * nearest); when it is more than 4 / 2^20 away from the next rounding boundary,
 * its integer part is the guest's result (the guest's value is positive, so
 * zero has no lower boundary when truncating). Otherwise, in other rounding
 * modes, or outside that range (-0, NaN included: checked on the bits, without
 * floating-point compares), the guest's computation runs as is. */
static uint32_t fog_byte(Ctx *c, double e)
{
    uint64_t eb;
    memcpy(&eb, &e, 8);
    uint32_t mode = c->fcw & 0x0C00u;
    if ((uint32_t)(eb >> 32) < 0x408F4000u && (mode == 0x0C00u || mode == 0u)) {   /* +0 <= e < 1000 */
        int32_t n = (int32_t)(e + 0.5);
        double r = (double)n - e;               /* 2^-e = 2^r * 2^-n, |r| <= 0.5 */
        /* 2^r = sum (r ln 2)^k / k!, k <= 9 */
        double p = 1.0 + r * (6.9314718055994531e-01 + r * (2.4022650695910071e-01 +
                   r * (5.5504108664821580e-02 + r * (9.6181291076284772e-03 + r * (1.3333558146428443e-03 +
                   r * (1.5403530393381609e-04 + r * (1.5252733804059841e-05 + r * (1.3215486790144307e-06 +
                   r * 1.0178086009239699e-07))))))));
        uint64_t b = (uint64_t)(1023 - n) << 52;
        double s;
        memcpy(&s, &b, 8);
        int32_t q = (int32_t)(p * s * (255.0 * 1048576.0));   /* 255 * fog * 2^20, below 2^28 */
        if (mode == 0u)
            q += 0x80000;
        uint32_t frac = (uint32_t)q & 0xFFFFFu;
        if ((frac >= 4u || q < 0x100000) && frac < 0xFFFFBu)
            return (uint32_t)(q >> 20);
    }
    double n = rt_fround(c, e);
    double fog = 1.0 / rt_fscale((exp2(e - n) - 1.0) + 1.0, n);
    if (BELOW(fog, 0.0))
        fog = 0.0;
    return fist(c, fog * 255.0);
}

/* Screen position, depth values, fog and outcode of a view-space vertex. */
static uint8_t project_vertex(const Frame *f, TLVertex *o, float vx, float vy, double depth, int *behind)
{
    Ctx *c = f->c;
    float vz = (float)depth;
    if (BELOW(depth, f->near_z)) {
        o->sx = vx;
        o->sy = vy;
        o->sz = vz;
        o->rhw = 0.0f;
        o->specular = 0xFF000000u;
        *behind = 1;
        return OUT_NEAR;
    }
    double inv = 1.0 / (double)vz;
    float invf = (float)inv;
    double sx = inv * vx * f->proj_sx + f->proj_cx;
    double sy = (double)invf * vy * f->proj_sy + f->proj_cy;
    o->sx = (float)sx;
    o->sy = (float)sy;
    o->rhw = (float)((double)invf * f->rhw_scale);
    o->sz = (float)((double)invf * f->z_scale + f->z_offset);
    /* fog = 1 / 2^(depth * k * log2(e)), the x87 exp idiom */
    o->specular = fog_byte(c, (double)vz * f->fog_depth * f->fog_density * LOG2E) << 24;
    return (uint8_t)((uint32_t)BELOW(sx, f->left) | (uint32_t)BELOW(sy, f->top) << 1 |
                     (uint32_t)ABOVE(sx, f->right) << 2 | (uint32_t)ABOVE((double)o->sy, f->bottom) << 3);
}

static uint32_t grey3(uint32_t g)
{
    return g | ((g << 8 | g) << 8);
}

static float morph_of(const PatchWork *w, int32_t parent, int32_t nbase)
{
    if (parent < 0 || parent >= nbase) {
        note_stale();
        return parent >= 0 && parent < 256 ? w->morph[parent] : 0.0f;
    }
    return w->morph[parent];
}

static uint32_t blend_of(const PatchWork *w, uint32_t off, int32_t nbase)
{
    uint32_t v = 0;
    if (off + 4u > (uint32_t)nbase * 32u) {
        note_stale();
        if (off + 4u > MAX_VERTS * 32u)
            return 0;
    }
    memcpy(&v, (const uint8_t *)w->blend + off, 4);
    return v;
}

/* Draws the strip entries of one pass into a batch; flagged triangles go through the clipper. */
/* Clipper input of vertex i, made once per pass. */
static const ClipVert *clip_vert(const Frame *f, PatchWork *w, uint32_t i)
{
    if (w->clip_pass[i] != w->pass) {
        make_clip_vert(f, w, i, &w->clip[i]);
        w->clip_pass[i] = w->pass;
    }
    return &w->clip[i];
}

/* The guest builds the clipper's vertices of the detail pass at esp+0x280, inside
 * its triangle flag array (esp+0x240, 256 entries): each clipped triangle
 * overwrites the flags of strip entries 64..255 with the vertices' bytes. */
static void overlay_detail_flags(PatchWork *w, const ClipVert *cv)
{
    for (int k = 0; k < 3; k++) {
        uint8_t *dst = w->tri + 64 + 64 * k;
        memcpy(dst, &cv[k].v, 32);
        dst[32] = (uint8_t)cv[k].code;
        if (!(cv[k].code & OUT_NEAR))
            memcpy(dst + 40, cv[k].attr, 24);
    }
}

static void emit_strips(const Frame *f, PatchWork *w, const uint8_t *strip, int32_t nwords,
                        uint32_t batch, ClipOut *out, int detail)
{
    uint32_t pack = out->vbase * 0x10001u;
    uint32_t ip = out->idx;
    if (++w->pass == 0) {               /* wrapped: forget every cached vertex */
        memset(w->clip_pass, 0, sizeof(w->clip_pass));
        w->pass = 1;
    }
    for (int32_t j = 0; j < nwords; j++) {
        uint32_t fl = w->tri[j];
        uint32_t word = rd32(strip + 4 * j);
        if (fl == 0) {
            WR32(ip, (word & 0x00FF00FFu) + pack);
            WR16(ip + 4u, (((word >> 8) & 0xFFu) + pack) & 0xFFFFu);
            ip += 6u;
        } else if (!(fl & TRI_CULLED)) {
            ClipVert cv[3];
            cv[0] = *clip_vert(f, w, word & 0xFFu);
            cv[1] = *clip_vert(f, w, (word >> 16) & 0xFFu);
            cv[2] = *clip_vert(f, w, (word >> 8) & 0xFFu);
            if (detail)
                overlay_detail_flags(w, cv);
            out->ntris = (ip - out->idx) / 6u;
            PROF_SECTION(0x00514760u);
            clip_triangle(f, cv, out);
            PROF_SECTION_END;
            ip = out->idx + out->ntris * 6u;
        }
    }
    WR32(batch + BATCH_TRIS, RD32(batch + BATCH_TRIS) + (ip - out->idx) / 6u);
    WR32(G_VBUF_PTR, RD32(G_VBUF_PTR) + out->nverts * 32u);
    WR32(G_VBUF_COUNT, RD32(G_VBUF_COUNT) + out->nverts);
}

static void draw_patch(const Frame *f, PatchWork *w, uint32_t patch)
{
    Ctx *c = f->c;
    const uint8_t *pp = g_mem + patch;
    double center[3];
    memcpy(center, pp, sizeof(center));
    uint32_t hdr = rd32(pp + 0x18);
    int32_t nbase = (int32_t)hdr >> 16;
    int32_t ntotal = (int32_t)(hdr & 0xFFFFu);
    uint32_t lod = rd32(pp + 0x1C);
    float lod_k = (float)(0.375 / (double)(int32_t)(1u << ((25u - lod) & 31u)));
    int32_t radius = 0x40000000 >> (lod & 31u);

    double dx = center[0] - f->cam[0], dy = center[1] - f->cam[1], dz = center[2] - f->cam[2];
    double depth = (dz * f->rot[8] + dy * f->rot[7]) + dx * f->rot[6];
    if ((double)radius * (double)1.73f + depth >= f->patch_range)
        return;

    /* Base vertices: {x, y, z, dz}; z morphs by dz as the patch fades to the
     * coarser level (morph factor from the view-space distance). */
    const uint8_t *vp = pp + 0x20;
    const uint8_t *base = vp;
    uint32_t passes = 0;
    int behind = 0;
    for (int32_t i = 0; i < nbase; i++, vp += 16) {
        float x = rdf(vp), y = rdf(vp + 4), z = rdf(vp + 8), dzm = rdf(vp + 12);
        double xr = (double)x + f->ncam[0];
        double ax = xr < 0.0 ? -xr : xr;
        double yr = (double)y + f->ncam[1];
        float yrf = (float)yr;
        double ay = yr < 0.0 ? -yr : yr;
        double dist = ay > ax ? (double)(float)ay : ax;
        double zr = (double)z + f->ncam[2];
        double az = zr < 0.0 ? -zr : zr;
        if (az > dist)
            dist = (double)(float)az;
        double t = (dist * lod_k - f->lod_bias) * f->lod_scale;
        if (BELOW(t, 0.0))
            t = 0.0;
        else if (ABOVE(t, 1.0))
            t = 1.0;
        w->morph[i] = (float)t;
        if (ABOVE(t, 0.01))
            passes |= 2;
        if (BELOW(t, 0.99))
            passes |= 1;
        double omt = 1.0 - t;
        z = (float)(omt * dzm + z);
        double a = omt * 255.0;
        uint32_t g1 = grey3(fist_u31(c, a));           /* t, omt in [0, 1] */
        uint32_t g2 = grey3(fist_u31(c, a * omt));
        uint32_t inv1 = 0xFFFFFFu - g1;

        double zc = (double)z + f->ncam[2];
        float vx = (float)((zc * f->rot[2] + (double)yrf * f->rot[1]) + xr * f->rot[0]);
        float vy = (float)((zc * f->rot[5] + (double)yrf * f->rot[4]) + xr * f->rot[3]);
        double vd = (zc * f->rot[8] + (double)yrf * f->rot[7]) + xr * f->rot[6];
        w->code[i] = project_vertex(f, &w->vtx[i], vx, vy, vd, &behind);

        uint32_t *bl = w->blend[i];
        bl[0] = 0;
        bl[1] = 0;
        bl[2] = g1 - g2;
        bl[3] = (inv1 - g1) + g2;
        bl[4] = g2;
        bl[5] = g1 - g2;
        bl[6] = g1;
        bl[7] = inv1;
        w->unused[i] = 1;
    }
    /* Morph vertices: {x, y, z, parent}; they move towards their parent base vertex. */
    for (int32_t i = nbase; i < ntotal; i++, vp += 16) {
        float x = rdf(vp), y = rdf(vp + 4), z = rdf(vp + 8);
        int32_t parent = (int32_t)rd32(vp + 12);
        double t = morph_of(w, parent, nbase);
        const uint8_t *pv = base + 16 * (uint32_t)parent;
        double xm = ((double)rdf(pv) - x) * t + x;
        float ym = (float)(((double)rdf(pv + 4) - y) * t + y);
        float zm = (float)(((double)rdf(pv + 8) - z) * t + z);
        double xr = xm + f->ncam[0];
        double yr = (double)ym + f->ncam[1];
        double zr = (double)zm + f->ncam[2];
        float vx = (float)((zr * f->rot[2] + yr * f->rot[1]) + xr * f->rot[0]);
        float vy = (float)((zr * f->rot[5] + yr * f->rot[4]) + xr * f->rot[3]);
        double vd = (zr * f->rot[8] + yr * f->rot[7]) + xr * f->rot[6];
        w->code[i] = project_vertex(f, &w->vtx[i], vx, vy, vd, &behind);
        w->unused[i] = 1;
    }

    /* Strip header: {flags, strip entries, extra groups, entry count}. */
    uint32_t sflags = rd32(vp);
    WR32(G_STRIP_FLIP, (sflags >> 2) & 1u);
    const uint8_t *strip = g_mem + rd32(vp + 4);
    int32_t ngroups = (int32_t)rd32(vp + 8);
    int32_t nwords = (int32_t)rd32(vp + 12);
    const uint8_t *rec = vp + 16;

    if (!behind) {
        /* Cull: triangles fully outside one screen edge or back-facing. Each strip
         * entry holds a triangle (bytes 2, 1, 0); after the first entry of a strip
         * only byte 0 is new and the winding alternates. */
        float wind0 = (sflags >> 2) & 1u ? -1.0f : 1.0f, wind = wind0;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        uint32_t c0 = 0, c1 = 0, c2 = 0, i0 = 0, i1 = 0, i2 = 0;
        uint32_t allculled = 0xFFFFFFFFu;
        int32_t k = 0;
        for (int32_t j = 0; j < nwords; j++) {
            uint32_t word = rd32(strip + 4 * j);
            x0 = x1;
            y0 = y1;
            x1 = x2;
            y1 = y2;
            c0 = c1;
            c1 = c2;
            i0 = i1;
            i1 = i2;
            if (k == 0) {
                i0 = (word >> 16) & 0xFFu;
                c0 = w->code[i0];
                x0 = w->vtx[i0].sx;
                y0 = w->vtx[i0].sy;
                i1 = (word >> 8) & 0xFFu;
                c1 = w->code[i1];
                x1 = w->vtx[i1].sx;
                y1 = w->vtx[i1].sy;
            }
            i2 = word & 0xFFu;
            c2 = w->code[i2];
            x2 = w->vtx[i2].sx;
            y2 = w->vtx[i2].sy;
            uint32_t culled = 1;
            uint32_t fl = c0 | c1 | c2 | TRI_CULLED;
            if (!(c0 & c1 & c2)) {
                double area = ((double)x1 - x2) * ((double)y0 - y2) - ((double)x0 - x2) * ((double)y1 - y2);
                if (BELOW(area * wind, (double)-0.001f)) {
                    w->unused[i2] = 0;
                    w->unused[i1] = 0;
                    w->unused[i0] = 0;
                    fl ^= TRI_CULLED;
                    culled = 0;
                }
            }
            w->tri[j] = (uint8_t)fl;
            wind = (float)((double)wind * -1.0);
            allculled &= culled;
            k++;
            if (word & 0x10000000u) {
                k = 0;
                wind = wind0;
            }
        }
        if (allculled)
            return;
    } else {
        /* A vertex is behind the near plane: every triangle goes to the clipper. */
        if (nwords > 0)
            memset(w->tri, OUT_NEAR, (size_t)nwords);
        if (ntotal > 0)
            memset(w->unused, 0, (size_t)ntotal);
    }

    /* Texture passes: per group, pass 1 (base, needs a factor below 0.99) and
     * pass 2 (blend, needs one above 0.01), each {material, per-vertex records}.
     * Base vertex records {colour offset, u, v}, morph vertex records
     * {parent << 16 | colour offset, u, v, du, dv}. */
    int first_batch = 1;
    int32_t g = 0;
    do {
        for (uint32_t pass = 1; pass < 3; pass++) {
            if (!(passes & pass)) {
                rec += 4 + 20 * ntotal - 8 * nbase;
                continue;
            }
            uint32_t material = rd32(rec);
            rec += 4;
            ClipOut out;
            out.vbase = RD32(G_VBUF_COUNT);
            out.vb = RD32(G_VBUF_PTR);
            int32_t i = 0;
            for (; i < nbase; i++, rec += 12) {
                if (w->unused[i])
                    continue;
                TLVertex *v = &w->vtx[i];
                v->color = blend_of(w, rd32(rec) & 0xFFFFu, nbase);
                memcpy(&v->tu, rec + 4, 4);
                memcpy(&v->tv, rec + 8, 4);
                copy_vertex(out.vb + 32u * (uint32_t)i, v);
            }
            for (; i < ntotal; i++, rec += 20) {
                if (w->unused[i])
                    continue;
                uint32_t word = rd32(rec);
                double t = morph_of(w, (int32_t)word >> 16, nbase);
                TLVertex *v = &w->vtx[i];
                v->color = blend_of(w, word & 0xFFFFu, nbase);
                v->tu = (float)((double)rdf(rec + 12) * t + rdf(rec + 4));
                v->tv = (float)((double)rdf(rec + 16) * t + rdf(rec + 8));
                copy_vertex(out.vb + 32u * (uint32_t)i, v);
            }
            uint32_t batch = get_batch(f, material, first_batch ? 0u : 1u);
            first_batch = 0;
            out.vb = RD32(G_VBUF_PTR);
            out.nverts = (uint32_t)ntotal;
            out.idx = batch + BATCH_INDICES + RD32(batch + BATCH_TRIS) * 6u;
            out.ntris = 0;
            emit_strips(f, w, strip, nwords, batch, &out, 0);
        }
    } while (++g < ngroups);

    /* Detail texture pass: {material, per-vertex records as above}, white. */
    uint32_t detail = rd32(rec);
    rec += 4;
    if (!detail)
        return;
    ClipOut out;
    out.vbase = RD32(G_VBUF_COUNT);
    out.vb = RD32(G_VBUF_PTR);
    int32_t i = 0;
    for (; i < nbase; i++, rec += 12) {
        if (w->unused[i])
            continue;
        TLVertex *v = &w->vtx[i];
        memcpy(&v->tu, rec + 4, 4);
        memcpy(&v->tv, rec + 8, 4);
        v->color = 0xFFFFFFu;
        copy_vertex(out.vb + 32u * (uint32_t)i, v);
    }
    for (; i < ntotal; i++, rec += 20) {
        if (w->unused[i])
            continue;
        double t = morph_of(w, (int32_t)rd32(rec) >> 16, nbase);
        TLVertex *v = &w->vtx[i];
        v->color = 0xFFFFFFu;
        v->tu = (float)((double)rdf(rec + 12) * t + rdf(rec + 4));
        v->tv = (float)((double)rdf(rec + 16) * t + rdf(rec + 8));
        copy_vertex(out.vb + 32u * (uint32_t)i, v);
    }
    uint32_t batch = get_batch(f, detail, 2);
    out.vb = RD32(G_VBUF_PTR);
    out.nverts = (uint32_t)ntotal;
    out.idx = batch + BATCH_INDICES + RD32(batch + BATCH_TRIS) * 6u;
    out.ntris = 0;
    emit_strips(f, w, strip, nwords, batch, &out, 1);
}

/* The guest's stack arrays hold 64 vertices and 256 strip entries; frames with
 * larger patches run the recompiled code (which then overwrites its own locals). */
static int terrain_supported(void)
{
    int32_t n = (int32_t)RD32(G_PATCH_COUNT);
    for (int32_t i = 0; i < n; i++) {
        uint32_t p = RD32(G_PATCH_LIST + 4u * (uint32_t)i);
        if (!p)
            continue;
        uint32_t hdr = RD32(p + 0x18u);
        int32_t nbase = (int32_t)hdr >> 16, ntotal = (int32_t)(hdr & 0xFFFFu);
        if (nbase < 1 || nbase > ntotal || ntotal > MAX_VERTS)
            return 0;
        int32_t nwords = (int32_t)RD32(p + 0x20u + 16u * (uint32_t)ntotal + 12u);
        if (nwords < 1 || nwords > MAX_STRIP)
            return 0;
    }
    return 1;
}

/* ---- 0x512C30 ----------------------------------------------------------- */

static void setup_frame(Frame *f)
{
    Ctx *c = f->c;
    /* Depth range: z = rhw_z * z_scale + z_offset. */
    double q = 1.0 / (1.0 - (double)gf(G_PROJ_A) / gf(G_PROJ_B));
    double range = (double)gf(G_ZFAR) - gf(G_ZNEAR_W);
    WRF32(G_Z_SCALE, -(range * gf(G_RHW_SCALE) * gf(G_PROJ_A) * q));
    WRF32(G_Z_OFFSET, range * q + gf(G_ZNEAR_W));
    WR32(RD32(G_DEPTH_CLEAR) + 4u, 0xFFFFFFFFu);
    WR32(RD32(G_DEPTH_CLEAR2) + 4u, 0xFFFFFFFFu);
    WR32(RD32(G_DEPTH_CLEAR3) + 4u, 0xFFFFFFFFu);
    WR32(RD32(G_DEPTH_CLEAR4) + 4u, 0xFFFFFFFFu);
    /* D3DVIEWPORT7 {0, 0, width, height, MinZ, MaxZ} */
    WR32(G_VIEWPORT + 0x00u, 0);
    WR32(G_VIEWPORT + 0x04u, 0);
    WR32(G_VIEWPORT + 0x08u, RD32(G_VIEWPORT_SRC + 4u));
    WR32(G_VIEWPORT + 0x0Cu, RD32(G_VIEWPORT_SRC + 8u));
    WR32(G_VIEWPORT + 0x10u, RD32(G_ZNEAR_W));
    WR32(G_VIEWPORT + 0x14u, RD32(G_ZFAR));
    /* Packed fog colour (computed with negated red and green, as the game does). */
    uint32_t r = fist(c, (double)gf(G_FOG_R) * -255.0);
    uint32_t g = fist(c, (double)gf(G_FOG_R + 4u) * -255.0);
    uint32_t b = fist(c, (double)gf(G_FOG_R + 8u) * 255.0);
    WR32(G_FOG_COLOR, b - (((r << 8) + g) << 8));
}

static void setup_device(const Frame *f)
{
    /* World transform: identity, in the guest frame where the game keeps it. */
    uint32_t m = f->sp + 0x1A8u;
    memset(g_mem + m, 0, 64);
    for (uint32_t i = 0; i < 4; i++)
        WR32(m + 20u * i, 0x3F800000u);
    uint32_t dev = RD32(G_D3D_DEVICE);
    uint32_t a[2] = { 1, m };
    com_call(f, dev, D3DDEV_SET_TRANSFORM, 2, a);
    uint32_t vp = G_VIEWPORT;
    com_call(f, dev, D3DDEV_SET_VIEWPORT, 1, &vp);
    static const uint32_t rs[][2] = {
        { 0x88, 1 }, { 0x26, 0 }, { 0x23, 0 }, { 0x8C, 3 }, { 0x30, 1 }, { 0x24, 0x461C4000u },
        { 0x25, 0x46EA6000u }, { 0x2F, 0 }, { 0x1B, 0 }, { 0x16, 1 }, { 0x89, 0 }, { 0x8D, 1 },
        { 0x1A, 1 }, { 0x0F, 0 }, { 0x07, 0 }, { 0x0E, 1 }, { 0x1D, 0 },
    };
    for (uint32_t i = 0; i < sizeof(rs) / sizeof(rs[0]); i++) {
        uint32_t value = rs[i][1];
        if (rs[i][0] == 0x26)
            value = RD32(G_FOG_DENSITY);   /* D3DRENDERSTATE_FOGDENSITY */
        else if (rs[i][0] == 0x07)
            value = RD32(G_ZENABLE);
        set_render_state(f, rs[i][0], value);
    }
    static const uint32_t tss[][3] = {
        { 0, 0x11, 2 }, { 0, 0x10, 2 }, { 0, 0x0C, 3 }, { 0, 0x12, 2 }, { 0, 0x0B, 0 },
        { 0, 0x04, 1 }, { 0, 0x01, 4 }, { 0, 0x02, 2 }, { 0, 0x03, 0 }, { 1, 0x01, 1 },
    };
    for (uint32_t i = 0; i < sizeof(tss) / sizeof(tss[0]); i++)
        set_tss(f, tss[i][0], tss[i][1], tss[i][2]);
}

/* Draws the batches (base, blend, detail) and returns them to the pool. */
static void flush_batches(const Frame *f)
{
    uint32_t vb = RD32(G_VBUF);
    com_call(f, vb, D3DVB_UNLOCK, 0, NULL);
    for (uint32_t kind = 0; kind < 3; kind++) {
        guest_call_args(f->c, f->sp, FN_KIND_STATES, 1, &kind);
        for (uint32_t n = RD32(G_BATCH_COUNT); n != 0;) {
            uint32_t b = RD32(RD32(G_BATCH_INDEX) + 4u * (n - 1u));
            n--;
            b = b * RD32(G_BATCH_STRIDE) + RD32(G_BATCH_POOL);
            if (RD32(b + BATCH_TRIS) == 0 || RD32(b + BATCH_KIND) != kind)
                continue;
            uint32_t dev = RD32(G_D3D_DEVICE);
            uint32_t tex[2] = { 0, RD32(RD32(b + BATCH_MATERIAL) + 0x20u) };
            com_call(f, dev, D3DDEV_SET_TEXTURE, 2, tex);
            uint32_t draw[7] = { 4 /* D3DPT_TRIANGLELIST */, RD32(G_VBUF), 0, RD32(G_VBUF_COUNT),
                                 b + BATCH_INDICES, RD32(b + BATCH_TRIS) * 3u, 0 };
            com_call(f, RD32(G_D3D_DEVICE), D3DDEV_DRAW_INDEXED_VB, 7, draw);
            WR32(b + BATCH_TRIS, 0);
        }
    }
    /* Free every batch: remove the first one from the index, moving the last into its place. */
    while (RD32(G_BATCH_COUNT) != 0) {
        uint32_t stride = RD32(G_BATCH_STRIDE), index = RD32(G_BATCH_INDEX);
        uint32_t pool = RD32(G_BATCH_POOL), pos_off = RD32(G_BATCH_POS_OFF);
        uint32_t b = stride * RD32(index) + pool;
        uint32_t last = RD32(G_BATCH_COUNT) - 1u;
        WR32(G_BATCH_COUNT, last);
        uint32_t pos = RD32(pos_off + b);
        if (pos != last) {
            uint32_t lb = RD32(index + 4u * last) * stride + pool;
            uint32_t lpos = RD32(pos_off + lb);
            uint32_t moved = RD32(index + 4u * pos);
            WR32(index + 4u * pos, RD32(index + 4u * lpos));
            WR32(RD32(G_BATCH_INDEX) + 4u * lpos, moved);
            WR32(RD32(G_BATCH_POS_OFF) + lb, pos);
        }
        guest_call_args(f->c, f->sp, FN_FREE_BATCH, 1, &b);
    }
}

/* ---- fast version ------------------------------------------------------- *
 *
 * The default unless terrain_exact=1 (igi.cfg) / IGI_TERRAIN_EXACT=1. The same
 * frame in single precision: the same patches, batches, Direct3D calls and
 * guest-visible bookkeeping in the same order, vertex values within float
 * rounding of the exact version (fog and colour bytes within one step), culling
 * and clipping decisions equal except for triangles at a boundary.
 *
 * Restructured for speed:
 *   - vertices are transformed and projected four at a time (NEON) into
 *     structure-of-arrays tables, with a refined reciprocal estimate and a
 *     polynomial 2^x for the fog;
 *   - a triangle that needs clipping is clipped once per patch instead of once
 *     per texture pass: the clipper carries, instead of the attributes, the
 *     weights of the triangle's three source vertices (every clipping step is
 *     linear in the attributes), and each pass only evaluates the colour and
 *     texture coordinates of the output vertices from them;
 *   - the index stream of a pass (strip triangles and clipped ones) is built once
 *     per patch and written with the pass's vertex base added, eight at a time.
 *
 * The guest's detail pass reads triangle flags it has overwritten itself (see
 * overlay_detail_flags). That is reproduced with the fast version's values, so
 * the strip entries it affects can differ from the exact version (their flags
 * are bytes of floats). Corrupt patch data (morph parents outside the patch) is
 * read as in the exact version except that a parent outside the guest's 256
 * entry table does not move the vertex. */

#define FAST_MAX_CLIPV 12288         /* clipper output vertices per patch (4096 triangles) */

/* A clipper output vertex: final screen values and the weights of the source
 * vertices' colour and texture coordinates. */
typedef struct FClipV {
    float p[4];                     /* sx, sy, sz, rhw */
    float u[3];
    uint32_t spec;
    uint8_t s[4];                   /* source vertices (strip entry bytes 0, 2, 1) */
} FClipV;

/* The second half of a vertex buffer entry. */
typedef struct VTail {
    uint32_t col, spec;
    float tu, tv;
} VTail;

typedef struct FastWork {
    float pos[256][4];              /* vertex buffer layout: sx, sy, sz, rhw */
    float x[256], y[256], z[256];   /* camera-relative world position, then view space */
    float q[256];                   /* the clipper's 1/depth (fog depth * rhw) */
    uint32_t spec[256];
    uint32_t code[256];
    float morph[256];
    uint8_t unused[256];
    uint32_t blend[MAX_VERTS + 4][8];   /* rows from MAX_VERTS: scratch of the last vector */
    VTail tl[256];                  /* colour and texture coordinates of the current pass */
    uint8_t tri[MAX_STRIP];
    uint8_t cdone[MAX_STRIP];       /* the clipper output of the strip entry is in cv */
    uint16_t cstart[MAX_STRIP], ccount[MAX_STRIP];
    uint16_t rel[3 * MAX_STRIP + FAST_MAX_CLIPV];   /* index stream of a pass, from its vertex base */
    uint32_t nrel, ncv, ncv_main, ntri_main;
    FClipV cv[FAST_MAX_CLIPV];
    /* Batches of this call by material and kind (see fast_get_batch). */
    uint32_t bc_material[64], bc_kind[64], bc_batch[64];
} FastWork;

static uint32_t clip_overflow;

/* get_batch through a cache: 0x514680 finds the batch of a material and kind
 * in its hash chains or creates it; a batch, once there, stays until
 * flush_batches. A found batch has no other effect (the guest's lookup only
 * writes its dead stack), so a repeated lookup in the same call is answered
 * here. The cache is cleared at the start of every call. */
static uint32_t fast_get_batch(const Frame *f, FastWork *w, uint32_t material, uint32_t kind)
{
    uint32_t h = ((material >> 4) ^ (material >> 10) ^ (kind * 23u)) & 63u;
    if (w->bc_batch[h] && w->bc_material[h] == material && w->bc_kind[h] == kind)
        return w->bc_batch[h];
    uint32_t b = get_batch(f, material, kind);
    w->bc_material[h] = material;
    w->bc_kind[h] = kind;
    w->bc_batch[h] = b;
    return b;
}

/* fist of a non-negative value in the frame's rounding mode (nearest or truncate). */
static inline int32_t ffist(const Frame *f, float v)
{
    return (int32_t)(f->chop ? v : v + 0.5f);
}

static inline uint32_t fbyte(const Frame *f, float v)
{
    int32_t k = ffist(f, v);
    return k < 0 ? 0u : k > 255 ? 255u : (uint32_t)k;
}

static float fast_morph_of(const FastWork *w, int32_t parent, int32_t nbase)
{
    if (parent < 0 || parent >= nbase) {
        note_stale();
        return parent >= 0 && parent < 256 ? w->morph[parent] : 0.0f;
    }
    return w->morph[parent];
}

static uint32_t fast_blend_of(const FastWork *w, uint32_t off, int32_t nbase)
{
    uint32_t v = 0;
    if (off + 4u > (uint32_t)nbase * 32u) {
        note_stale();
        if (off + 4u > MAX_VERTS * 32u)
            return 0;
    }
    memcpy(&v, (const uint8_t *)w->blend + off, 4);
    return v;
}

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
/* Sign bit of each lane (1: negative, -0 included). */
static inline uint32x4_t fsign(float32x4_t v)
{
    return vshrq_n_u32(vreinterpretq_u32_f32(v), 31);
}

/* All ones where a >= b (a - b not negative). */
static inline uint32x4_t fge_mask(float32x4_t a, float32x4_t b)
{
    return vreinterpretq_u32_s32(vmvnq_s32(vshrq_n_s32(vreinterpretq_s32_f32(vsubq_f32(a, b)), 31)));
}
#else
/* Blend colours of a base vertex from its grey levels (as draw_patch). */
static inline void fast_blend_row(uint32_t *bl, uint32_t g1, uint32_t g2)
{
    g1 = grey3(g1);
    g2 = grey3(g2);
    uint32_t inv1 = 0xFFFFFFu - g1;
    bl[0] = 0;
    bl[1] = 0;
    bl[2] = g1 - g2;
    bl[3] = (inv1 - g1) + g2;
    bl[4] = g2;
    bl[5] = g1 - g2;
    bl[6] = g1;
    bl[7] = inv1;
}
#endif

/* Base vertices: morph factor, morphed height and the blend colours; the
 * camera-relative position into x, y, z. Returns the texture passes the patch
 * needs (bit 0: a factor below 0.99, bit 1: one above 0.01). */
static __attribute__((noinline)) uint32_t fast_base_vertices(const Frame *f, FastWork *w, const uint8_t *vp, int32_t nbase, float lod_k)
{
    int32_t i = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    const float32x4_t one = vdupq_n_f32(1.0f), zero = vdupq_n_f32(0.0f), k255 = vdupq_n_f32(255.0f);
    const float32x4_t half = vdupq_n_f32(f->chop ? 0.0f : 0.5f);
    const float32x4_t c0 = vdupq_n_f32(f->ncam[0]), c1 = vdupq_n_f32(f->ncam[1]), c2 = vdupq_n_f32(f->ncam[2]);
    const float32x4_t lk = vdupq_n_f32(lod_k), bias = vdupq_n_f32(f->lod_bias), scale = vdupq_n_f32(f->lod_scale);
    const float32x4_t k001 = vdupq_n_f32(0.01f), k099 = vdupq_n_f32(0.99f);
    const uint32x4_t lane = { 0, 1, 2, 3 }, grey = vdupq_n_u32(0x010101u), white = vdupq_n_u32(0xFFFFFFu);
    const uint32x2_t z2 = vdup_n_u32(0);
    uint32x4_t pass1 = vdupq_n_u32(0), pass2 = pass1;
    /* Up to three records past the base vertices are read (morph vertices or the
     * strip header); those lanes are not used. */
    for (; i < nbase; i += 4) {
        float32x4x4_t r = vld4q_f32((const float *)(const void *)(vp + 16 * i));
        float32x4_t xr = vaddq_f32(r.val[0], c0);
        float32x4_t yr = vaddq_f32(r.val[1], c1);
        float32x4_t zr = vaddq_f32(r.val[2], c2);
        float32x4_t d = vmaxq_f32(vmaxq_f32(vabsq_f32(xr), vabsq_f32(yr)), vabsq_f32(zr));
        float32x4_t t = vmulq_f32(vsubq_f32(vmulq_f32(d, lk), bias), scale);
        t = vminq_f32(vmaxq_f32(t, zero), one);
        vst1q_f32(w->morph + i, t);
        float32x4_t omt = vsubq_f32(one, t);
        float32x4_t zc = vaddq_f32(vmlaq_f32(r.val[2], omt, r.val[3]), c2);
        float32x4_t a = vmulq_f32(omt, k255);
        vst1q_f32(w->x + i, xr);
        vst1q_f32(w->y + i, yr);
        vst1q_f32(w->z + i, zc);
        /* Passes, from the lanes of base vertices only. */
        uint32x4_t valid = vcltq_u32(vaddq_u32(lane, vdupq_n_u32((uint32_t)i)), vdupq_n_u32((uint32_t)nbase));
        pass1 = vorrq_u32(pass1, vandq_u32(valid, fsign(vsubq_f32(t, k099))));
        pass2 = vorrq_u32(pass2, vandq_u32(valid, fsign(vsubq_f32(k001, t))));
        /* Blend rows (grey levels times 0x010101), transposed to one row per vertex. */
        uint32x4_t g1 = vmulq_u32(vcvtq_u32_f32(vaddq_f32(a, half)), grey);
        uint32x4_t g2 = vmulq_u32(vcvtq_u32_f32(vaddq_f32(vmulq_f32(a, omt), half)), grey);
        uint32x4_t inv1 = vsubq_u32(white, g1), d12 = vsubq_u32(g1, g2);
        uint32x4x2_t lo = vtrnq_u32(d12, vaddq_u32(vsubq_u32(inv1, g1), g2));
        uint32x4x2_t h0 = vtrnq_u32(g2, d12), h1 = vtrnq_u32(g1, inv1);
        uint32_t *bl = w->blend[i];
        vst1q_u32(bl, vcombine_u32(z2, vget_low_u32(lo.val[0])));
        vst1q_u32(bl + 4, vcombine_u32(vget_low_u32(h0.val[0]), vget_low_u32(h1.val[0])));
        vst1q_u32(bl + 8, vcombine_u32(z2, vget_low_u32(lo.val[1])));
        vst1q_u32(bl + 12, vcombine_u32(vget_low_u32(h0.val[1]), vget_low_u32(h1.val[1])));
        vst1q_u32(bl + 16, vcombine_u32(z2, vget_high_u32(lo.val[0])));
        vst1q_u32(bl + 20, vcombine_u32(vget_high_u32(h0.val[0]), vget_high_u32(h1.val[0])));
        vst1q_u32(bl + 24, vcombine_u32(z2, vget_high_u32(lo.val[1])));
        vst1q_u32(bl + 28, vcombine_u32(vget_high_u32(h0.val[1]), vget_high_u32(h1.val[1])));
    }
    uint32x2_t p1 = vorr_u32(vget_low_u32(pass1), vget_high_u32(pass1));
    uint32x2_t p2 = vorr_u32(vget_low_u32(pass2), vget_high_u32(pass2));
    return (vget_lane_u32(p1, 0) | vget_lane_u32(p1, 1)) | (vget_lane_u32(p2, 0) | vget_lane_u32(p2, 1)) << 1;
#else
    uint32_t passes = 0;
    for (; i < nbase; i++) {
        const uint8_t *p = vp + 16 * i;
        float xr = rdf(p) + f->ncam[0], yr = rdf(p + 4) + f->ncam[1], zr = rdf(p + 8) + f->ncam[2];
        float d = fabsf(xr) > fabsf(yr) ? fabsf(xr) : fabsf(yr);
        if (fabsf(zr) > d)
            d = fabsf(zr);
        float t = (d * lod_k - f->lod_bias) * f->lod_scale;
        t = !(t >= 0.0f) ? 0.0f : t > 1.0f ? 1.0f : t;
        w->morph[i] = t;
        if (t > 0.01f)
            passes |= 2;
        if (t < 0.99f)
            passes |= 1;
        float omt = 1.0f - t, a = omt * 255.0f;
        fast_blend_row(w->blend[i], (uint32_t)ffist(f, a), (uint32_t)ffist(f, a * omt));
        w->x[i] = xr;
        w->y[i] = yr;
        w->z[i] = (rdf(p + 8) + omt * rdf(p + 12)) + f->ncam[2];
    }
    return passes;
#endif
}


/* View-space transform, projection, fog and outcodes of vertices [0, n), the
 * view-space position back into x, y, z. Up to three entries past n are
 * computed too (unused). */
static __attribute__((noinline)) void fast_project(const Frame *f, FastWork *w, int32_t n)
{
    int32_t i = 0;
    const float *rt = f->rot;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* Four loops with few constants each (the NEON registers hold them all).
     * Comparisons use the sign of a difference: GCC turns NEON float compare
     * intrinsics into scalar VFP compares unless math may be unsafe. */
    const float32x4_t KA = { rt[0], rt[1], rt[2], rt[3] }, KB = { rt[4], rt[5], rt[6], rt[7] };
    const float32x2_t KC = { rt[8], 0.0f };
    for (i = 0; i < n; i += 4) {
        float32x4_t X = vld1q_f32(w->x + i), Y = vld1q_f32(w->y + i), Z = vld1q_f32(w->z + i);
        float32x4_t vx = vmulq_lane_f32(Z, vget_high_f32(KA), 0);
        vx = vmlaq_lane_f32(vx, Y, vget_low_f32(KA), 1);
        vx = vmlaq_lane_f32(vx, X, vget_low_f32(KA), 0);
        float32x4_t vy = vmulq_lane_f32(Z, vget_low_f32(KB), 1);
        vy = vmlaq_lane_f32(vy, Y, vget_low_f32(KB), 0);
        vy = vmlaq_lane_f32(vy, X, vget_high_f32(KA), 1);
        float32x4_t vz = vmulq_lane_f32(Z, KC, 0);
        vz = vmlaq_lane_f32(vz, Y, vget_high_f32(KB), 1);
        vz = vmlaq_lane_f32(vz, X, vget_high_f32(KB), 0);
        vst1q_f32(w->x + i, vx);
        vst1q_f32(w->y + i, vy);
        vst1q_f32(w->z + i, vz);
    }
    /* Screen position and depth values; behind the near plane the view-space
     * position and rhw 0. */
    const float32x4_t KP = { f->proj_sx, f->proj_sy, f->rhw_scale, f->z_scale };
    const float32x2_t KF = { f->fog_depth, f->fog_k };
    const float32x4_t vnear = vdupq_n_f32(f->near_z), zero = vdupq_n_f32(0.0f);
    const float32x4_t pcx = vdupq_n_f32(f->proj_cx), pcy = vdupq_n_f32(f->proj_cy), zo = vdupq_n_f32(f->z_offset);
    for (i = 0; i < n; i += 4) {
        float32x4_t vx = vld1q_f32(w->x + i), vy = vld1q_f32(w->y + i), vz = vld1q_f32(w->z + i);
        uint32x4_t front = fge_mask(vz, vnear);
        float32x4_t inv = vrecpeq_f32(vz);
        inv = vmulq_f32(vrecpsq_f32(vz, inv), inv);
        inv = vmulq_f32(vrecpsq_f32(vz, inv), inv);
        float32x4x4_t o;
        o.val[0] = vbslq_f32(front, vmlaq_lane_f32(pcx, vmulq_f32(inv, vx), vget_low_f32(KP), 0), vx);
        o.val[1] = vbslq_f32(front, vmlaq_lane_f32(pcy, vmulq_f32(inv, vy), vget_low_f32(KP), 1), vy);
        o.val[2] = vbslq_f32(front, vmlaq_lane_f32(zo, inv, vget_high_f32(KP), 1), vz);
        float32x4_t rhw = vmulq_lane_f32(inv, vget_high_f32(KP), 0);
        o.val[3] = vbslq_f32(front, rhw, zero);
        vst4q_f32(&w->pos[i][0], o);
        vst1q_f32(w->q + i, vmulq_lane_f32(rhw, KF, 0));
    }
    /* Outcodes. */
    const float32x4_t left = vdupq_n_f32(f->left), top = vdupq_n_f32(f->top);
    const float32x4_t right = vdupq_n_f32(f->right), bottom = vdupq_n_f32(f->bottom);
    const uint32x4_t cnear = vdupq_n_u32(OUT_NEAR);
    for (i = 0; i < n; i += 4) {
        float32x4x4_t o = vld4q_f32(&w->pos[i][0]);
        uint32x4_t front = fge_mask(vld1q_f32(w->z + i), vnear);
        uint32x4_t code = fsign(vsubq_f32(o.val[0], left));
        code = vsliq_n_u32(code, fsign(vsubq_f32(o.val[1], top)), 1);
        code = vsliq_n_u32(code, fsign(vsubq_f32(right, o.val[0])), 2);
        code = vsliq_n_u32(code, fsign(vsubq_f32(bottom, o.val[1])), 3);
        vst1q_u32(w->code + i, vbslq_u32(front, code, cnear));
    }
    /* Fog: 2^-e, e = depth * k, as 2^-round(e) from the exponent bits times 2^r,
     * r = round(e) - e in [-0.5, 0.5], from its series to r^6 (relative error
     * below 2e-7). */
    const float32x4_t KS1 = { 6.9314718e-01f, 2.4022651e-01f, 5.5504109e-02f, 9.6181291e-03f };
    const float32x2_t KS2 = { 1.3333558e-03f, 1.5403530e-04f };
    const float32x4_t one = vdupq_n_f32(1.0f), half = vdupq_n_f32(0.5f), emax = vdupq_n_f32(120.0f);
    const float32x4_t fhalf = vdupq_n_f32(f->chop ? 0.0f : 0.5f), k255 = vdupq_n_f32(255.0f);
    const uint32x4_t u127 = vdupq_n_u32(127), u255 = vdupq_n_u32(255), opaque = vdupq_n_u32(0xFF000000u);
    for (i = 0; i < n; i += 4) {
        float32x4_t vz = vld1q_f32(w->z + i);
        uint32x4_t front = fge_mask(vz, vnear);
        float32x4_t e = vminq_f32(vmaxq_f32(vmulq_lane_f32(vz, KF, 1), zero), emax);
        uint32x4_t en = vcvtq_u32_f32(vaddq_f32(e, half));
        float32x4_t r = vsubq_f32(vcvtq_f32_u32(en), e);
        float32x4_t r2 = vmulq_f32(r, r), r3 = vmulq_f32(r2, r), r4 = vmulq_f32(r2, r2);
        float32x4_t p = vmlaq_lane_f32(one, r, vget_low_f32(KS1), 0);
        p = vmlaq_lane_f32(p, r2, vget_low_f32(KS1), 1);
        p = vmlaq_lane_f32(p, r3, vget_high_f32(KS1), 0);
        p = vmlaq_lane_f32(p, r4, vget_high_f32(KS1), 1);
        p = vmlaq_lane_f32(p, vmulq_f32(r4, r), KS2, 0);
        p = vmlaq_lane_f32(p, vmulq_f32(r3, r3), KS2, 1);
        float32x4_t s2 = vreinterpretq_f32_u32(vshlq_n_u32(vsubq_u32(u127, en), 23));
        uint32x4_t fog = vminq_u32(vcvtq_u32_f32(vmlaq_f32(fhalf, vmulq_f32(p, s2), k255)), u255);
        vst1q_u32(w->spec + i, vbslq_u32(front, vshlq_n_u32(fog, 24), opaque));
    }
    (void)i;
#else
    for (; i < n; i++) {
        float X = w->x[i], Y = w->y[i], Z = w->z[i];
        float vx = (Z * rt[2] + Y * rt[1]) + X * rt[0];
        float vy = (Z * rt[5] + Y * rt[4]) + X * rt[3];
        float vz = (Z * rt[8] + Y * rt[7]) + X * rt[6];
        w->x[i] = vx;
        w->y[i] = vy;
        w->z[i] = vz;
        float *o = w->pos[i];
        if (!(vz >= f->near_z)) {
            o[0] = vx;
            o[1] = vy;
            o[2] = vz;
            o[3] = 0.0f;
            w->spec[i] = 0xFF000000u;
            w->code[i] = OUT_NEAR;
            continue;
        }
        float inv = 1.0f / vz;
        o[0] = inv * vx * f->proj_sx + f->proj_cx;
        o[1] = inv * vy * f->proj_sy + f->proj_cy;
        o[2] = inv * f->z_scale + f->z_offset;
        o[3] = inv * f->rhw_scale;
        w->q[i] = o[3] * f->fog_depth;
        float e = vz * f->fog_k;
        e = !(e >= 0.0f) ? 0.0f : e > 120.0f ? 120.0f : e;
        w->spec[i] = fbyte(f, exp2f(-e) * 255.0f) << 24;
        w->code[i] = (uint32_t)!(o[0] >= f->left) | (uint32_t)!(o[1] >= f->top) << 1 |
                     (uint32_t)(o[0] > f->right) << 2 | (uint32_t)(o[1] > f->bottom) << 3;
    }
#endif
}

/* ---- fast clipper ---- */

/* Clipper vertex: sx, sy, 1/depth and the weights of the source vertices times
 * 1/depth (attributes over depth are linear on the screen). */
typedef struct FCV {
    float a[6];
    uint32_t code;
} FCV;

typedef struct FClip {
    const Frame *f;
    FastWork *w;
    float P[3][3];                  /* view-space positions of the source vertices */
    float fog[3];
    uint8_t s[3];
    uint32_t ntri;
} FClip;

static inline uint32_t fcode(const Frame *f, float x, float y)
{
    return (uint32_t)!(x >= f->left) | (uint32_t)!(y >= f->top) << 1 | (uint32_t)(x > f->right) << 2 |
           (uint32_t)(y > f->bottom) << 3;
}

static inline int ffirst(const FCV *v, uint32_t bit, int want)
{
    if (((v[0].code & bit) != 0) == want)
        return 0;
    if (((v[1].code & bit) != 0) == want)
        return 1;
    return 2;
}

static inline int fcount(const FCV *v, uint32_t bit)
{
    return (int)((v[0].code & bit) != 0) + (int)((v[1].code & bit) != 0) + (int)((v[2].code & bit) != 0);
}

/* o moves towards in: o = in + (o - in) * t. */
static inline void flerp(FCV *o, const FCV *in, float t)
{
    for (int m = 0; m < 6; m++)
        o->a[m] = in->a[m] + (o->a[m] - in->a[m]) * t;
}

static void fclip_emit(FClip *cl, const FCV *v)
{
    FastWork *w = cl->w;
    const Frame *f = cl->f;
    if (w->ncv + 3u > FAST_MAX_CLIPV) {
        if (clip_overflow++ == 0)
            rt_log("native terrain (fast): clipper output of a patch over %u vertices, dropped", FAST_MAX_CLIPV);
        return;
    }
    for (int k = 0; k < 3; k++) {
        FClipV *o = &w->cv[w->ncv++];
        float q = v[k].a[2], r = 1.0f / q;
        o->p[0] = v[k].a[0];
        o->p[1] = v[k].a[1];
        o->p[2] = q * f->z_scale + f->z_offset;
        o->p[3] = q * f->rhw_scale;
        o->u[0] = v[k].a[3] * r;
        o->u[1] = v[k].a[4] * r;
        o->u[2] = v[k].a[5] * r;
        o->spec = fbyte(f, o->u[0] * cl->fog[0] + o->u[1] * cl->fog[1] + o->u[2] * cl->fog[2]) << 24;
        o->s[0] = cl->s[0];
        o->s[1] = cl->s[1];
        o->s[2] = cl->s[2];
        o->s[3] = 0;
    }
    cl->ntri++;
}

/* Vertex on the near plane at view-space position P with weights R (not over depth). */
static void fnear_vertex(const FClip *cl, FCV *o, int a, int b)
{
    const Frame *f = cl->f;
    const float *pa = cl->P[a], *pb = cl->P[b];
    float t = (f->near_z - pa[2]) / (pb[2] - pa[2]);
    float P[3], R[3] = { 0.0f, 0.0f, 0.0f };
    for (int m = 0; m < 3; m++)
        P[m] = pa[m] + (pb[m] - pa[m]) * t;
    R[a] = 1.0f - t;
    R[b] = t;
    float r = 1.0f / P[2];
    o->a[0] = r * P[0] * f->proj_sx + f->proj_cx;
    o->a[1] = r * P[1] * f->proj_sy + f->proj_cy;
    o->a[2] = r;
    for (int m = 0; m < 3; m++)
        o->a[3 + m] = R[m] * r;
    o->code = fcode(f, o->a[0], o->a[1]);
}

static void fclip_tri(FClip *cl, FCV *v);

/* Near plane, as clip_near: only for the top-level triangle (slot k holds source
 * vertex k); the triangles it makes have no vertex behind the plane. */
static void fclip_near(FClip *cl, FCV *v)
{
    if (fcount(v, OUT_NEAR) == 2) {
        int k = ffirst(v, OUT_NEAR, 0);
        fnear_vertex(cl, &v[(k + 1) % 3], k, (k + 1) % 3);
        fnear_vertex(cl, &v[(k + 2) % 3], k, (k + 2) % 3);
        return;
    }
    /* One vertex behind: the quad in front becomes two triangles. */
    int k = ffirst(v, OUT_NEAR, 1), k1 = (k + 1) % 3, k2 = (k + 2) % 3;
    FCV sub[3];
    fnear_vertex(cl, &v[k], k1, k);
    fnear_vertex(cl, &sub[1], k2, k);
    sub[0] = v[k2];
    sub[2] = v[k];
    fclip_tri(cl, sub);
}

/* Screen edges in the exact version's order (left, top, right, bottom); after
 * an edge, new vertices get the outcodes of the later edges only. */
static void fclip_tri(FClip *cl, FCV *v)
{
    const Frame *f = cl->f;
    uint32_t any = v[0].code | v[1].code | v[2].code;
    uint32_t all = v[0].code & v[1].code & v[2].code;
    if (all)
        return;
    if (any & OUT_NEAR) {
        fclip_near(cl, v);
        any = v[0].code | v[1].code | v[2].code;
        all = v[0].code & v[1].code & v[2].code;
        if (all)
            return;
    }
    const float edge[4] = { f->left, f->top, f->right, f->bottom };
    for (int p = 0; p < 4; p++) {
        uint32_t bit = 1u << p, keep = (0xFu << (p + 1)) & 0xFu;
        int ax = p & 1;
        if (!(any & bit))
            continue;
        if (fcount(v, bit) == 2) {
            int k = ffirst(v, bit, 0);
            const FCV *in = &v[k];
            for (int n = 1; n <= 2; n++) {
                FCV *o = &v[(k + n) % 3];
                flerp(o, in, (edge[p] - in->a[ax]) / (o->a[ax] - in->a[ax]));
                o->code = fcode(f, o->a[0], o->a[1]) & keep;
            }
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
        } else {
            int k = ffirst(v, bit, 1);
            FCV *o = &v[k], *in1 = &v[(k + 1) % 3], *in2 = &v[(k + 2) % 3];
            FCV sub[3];
            sub[1] = *o;
            flerp(o, in1, (edge[p] - in1->a[ax]) / (o->a[ax] - in1->a[ax]));
            o->code = fcode(f, o->a[0], o->a[1]) & keep;
            flerp(&sub[1], in2, (edge[p] - in2->a[ax]) / (sub[1].a[ax] - in2->a[ax]));
            sub[1].code = fcode(f, sub[1].a[0], sub[1].a[1]) & keep;
            any = v[0].code | v[1].code | v[2].code;
            all = v[0].code & v[1].code & v[2].code;
            sub[0] = *in2;
            sub[2] = *o;
            fclip_tri(cl, sub);
        }
        if (all)
            return;
    }
    fclip_emit(cl, v);
}

/* Clips the triangle of a strip entry (vertices: bytes 0, 2, 1), appending its
 * output to w->cv; returns the number of triangles. */
static uint32_t fclip_entry(const Frame *f, FastWork *w, uint32_t word)
{
    FClip cl;
    FCV v[3];
    cl.f = f;
    cl.w = w;
    cl.ntri = 0;
    cl.s[0] = (uint8_t)word;
    cl.s[1] = (uint8_t)(word >> 16);
    cl.s[2] = (uint8_t)(word >> 8);
    for (int k = 0; k < 3; k++) {
        uint32_t i = cl.s[k];
        cl.P[k][0] = w->x[i];
        cl.P[k][1] = w->y[i];
        cl.P[k][2] = w->z[i];
        cl.fog[k] = (float)(w->spec[i] >> 24);
        v[k].code = w->code[i];
        float q = w->q[i];
        if (v[k].code & OUT_NEAR)
            q = 0.0f;               /* replaced by the near plane step */
        v[k].a[0] = w->pos[i][0];
        v[k].a[1] = w->pos[i][1];
        v[k].a[2] = q;
        v[k].a[3] = k == 0 ? q : 0.0f;
        v[k].a[4] = k == 1 ? q : 0.0f;
        v[k].a[5] = k == 2 ? q : 0.0f;
    }
    fclip_tri(&cl, v);
    return cl.ntri;
}

/* Index stream and clipper output of the patch's passes (flags in w->tri).
 * Returns the first strip entry that goes to the clipper (-1: none). */
static __attribute__((noinline)) int32_t fast_prepare(const Frame *f, FastWork *w, const uint8_t *strip, int32_t nwords, int32_t ntotal)
{
    int32_t first = -1;
    uint32_t nrel = 0, ntri = 0;
    w->ncv = 0;
    for (int32_t j = 0; j < nwords; j++) {
        uint32_t fl = w->tri[j];
        uint32_t word = rd32(strip + 4 * j);
        w->cdone[j] = 0;
        if (fl == 0) {
            w->rel[nrel] = (uint16_t)(word & 0xFFu);
            w->rel[nrel + 1] = (uint16_t)((word >> 16) & 0xFFu);
            w->rel[nrel + 2] = (uint16_t)((word >> 8) & 0xFFu);
            nrel += 3;
        } else if (!(fl & TRI_CULLED)) {
            if (first < 0)
                first = j;
            uint32_t start = w->ncv;
            uint32_t n = fclip_entry(f, w, word);
            w->cdone[j] = 1;
            w->cstart[j] = (uint16_t)start;
            w->ccount[j] = (uint16_t)n;
            for (uint32_t k = start; k < w->ncv; k++)
                w->rel[nrel++] = (uint16_t)((uint32_t)ntotal + k);
            ntri += n;
        }
    }
    w->nrel = nrel;
    w->ncv_main = w->ncv;
    w->ntri_main = ntri;
    return first;
}

static inline void copy32(uint8_t *d, const float *pos, const VTail *t)
{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    vst1q_u8(d, vreinterpretq_u8_f32(vld1q_f32(pos)));
    vst1q_u8(d + 16, vreinterpretq_u8_u32(vld1q_u32((const uint32_t *)(const void *)t)));
#else
    memcpy(d, pos, 16);
    memcpy(d + 16, t, 16);
#endif
}

/* Vertices of a pass: colour and texture coordinates from the records, written
 * to the vertex buffer at vb for every vertex a drawn triangle uses. */
static const uint8_t *fast_pass_vertices(FastWork *w, const uint8_t *rec, int32_t nbase, int32_t ntotal,
                                         uint32_t vb, int detail)
{
    uint8_t *dst = g_mem + vb;
    int32_t i = 0;
    for (; i < nbase; i++, rec += 12) {
        if (w->unused[i])
            continue;
        VTail *t = &w->tl[i];
        t->col = detail ? 0xFFFFFFu : fast_blend_of(w, rd32(rec) & 0xFFFFu, nbase);
        t->spec = w->spec[i];
        memcpy(&t->tu, rec + 4, 8);
        copy32(dst + 32u * (uint32_t)i, w->pos[i], t);
    }
    for (; i < ntotal; i++, rec += 20) {
        if (w->unused[i])
            continue;
        uint32_t word = rd32(rec);
        float m = fast_morph_of(w, (int32_t)word >> 16, nbase);
        VTail *t = &w->tl[i];
        t->col = detail ? 0xFFFFFFu : fast_blend_of(w, word & 0xFFFFu, nbase);
        t->spec = w->spec[i];
        t->tu = rdf(rec + 12) * m + rdf(rec + 4);
        t->tv = rdf(rec + 16) * m + rdf(rec + 8);
        copy32(dst + 32u * (uint32_t)i, w->pos[i], t);
    }
    return rec;
}

/* A clipper output vertex with the current pass's colour and texture coordinates. */
static inline void fast_clip_vertex(const Frame *f, const FastWork *w, uint8_t *d, const FClipV *o)
{
    const VTail *t0 = &w->tl[o->s[0]], *t1 = &w->tl[o->s[1]], *t2 = &w->tl[o->s[2]];
    float u0 = o->u[0], u1 = o->u[1], u2 = o->u[2];
    VTail t;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    /* Lanes: red, green, blue, tu. */
    const int32x4_t sh = { -16, -8, 0, 0 };
    const uint32x4_t mask = { 0xFFu, 0xFFu, 0xFFu, 0u };
    float32x4_t a0 = vcvtq_f32_u32(vandq_u32(vshlq_u32(vdupq_n_u32(t0->col), sh), mask));
    float32x4_t a1 = vcvtq_f32_u32(vandq_u32(vshlq_u32(vdupq_n_u32(t1->col), sh), mask));
    float32x4_t a2 = vcvtq_f32_u32(vandq_u32(vshlq_u32(vdupq_n_u32(t2->col), sh), mask));
    a0 = vsetq_lane_f32(t0->tu, a0, 3);
    a1 = vsetq_lane_f32(t1->tu, a1, 3);
    a2 = vsetq_lane_f32(t2->tu, a2, 3);
    float32x4_t s = vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(a0, u0), a1, u1), a2, u2);
    uint32x4_t c = vminq_u32(vcvtq_u32_f32(vaddq_f32(s, vdupq_n_f32(f->chop ? 0.0f : 0.5f))), vdupq_n_u32(255));
    const int32x4_t up = { 16, 8, 0, 0 };
    c = vshlq_u32(vandq_u32(c, mask), up);    /* lane 3 (tu) cleared */
    uint32x2_t c2 = vadd_u32(vget_low_u32(c), vget_high_u32(c));
    t.col = vget_lane_u32(c2, 0) | vget_lane_u32(c2, 1);
    t.tu = vgetq_lane_f32(s, 3);
#else
    uint32_t col = 0;
    for (int k = 16; k >= 0; k -= 8)
        col |= fbyte(f, u0 * (float)((t0->col >> k) & 0xFFu) + u1 * (float)((t1->col >> k) & 0xFFu) +
                            u2 * (float)((t2->col >> k) & 0xFFu)) << k;
    t.col = col;
    t.tu = u0 * t0->tu + u1 * t1->tu + u2 * t2->tu;
#endif
    t.spec = o->spec;
    t.tv = u0 * t0->tv + u1 * t1->tv + u2 * t2->tv;
    copy32(d, o->p, &t);
}

/* Bookkeeping at the end of a pass (as emit_strips). */
static void fast_pass_end(uint32_t batch, uint32_t ntris, uint32_t nclip, uint32_t nverts)
{
    WR32(batch + BATCH_TRIS, RD32(batch + BATCH_TRIS) + ntris);
    WR32(G_CLIP_DEPTH, RD32(G_CLIP_DEPTH) + nclip);   /* one per clipper output triangle */
    WR32(G_VBUF_PTR, RD32(G_VBUF_PTR) + nverts * 32u);
    WR32(G_VBUF_COUNT, RD32(G_VBUF_COUNT) + nverts);
}

/* Draws a pass from the prepared stream: clipper output vertices after the
 * patch's own, then the indices. */
static void fast_emit(const Frame *f, FastWork *w, int32_t ntotal, uint32_t batch, uint32_t vbase)
{
    uint8_t *d = g_mem + RD32(G_VBUF_PTR) + 32u * (uint32_t)ntotal;
    for (uint32_t n = 0; n < w->ncv_main; n++, d += 32)
        fast_clip_vertex(f, w, d, &w->cv[n]);
    uint8_t *ip = g_mem + batch + BATCH_INDICES + RD32(batch + BATCH_TRIS) * 6u;
    const uint16_t *rel = w->rel;
    uint32_t i = 0, n = w->nrel;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t vb = vdupq_n_u16((uint16_t)vbase);
    for (; i + 8u <= n; i += 8u)
        vst1q_u8(ip + 2u * i, vreinterpretq_u8_u16(vaddq_u16(vld1q_u16(rel + i), vb)));
#endif
    for (; i < n; i++) {
        uint16_t x = (uint16_t)(rel[i] + vbase);
        memcpy(ip + 2u * i, &x, 2);
    }
    fast_pass_end(batch, n / 3u, w->ntri_main, (uint32_t)ntotal + w->ncv_main);
}

/* The guest's flag overlay of a detail pass triangle (see overlay_detail_flags):
 * the clipper input vertices s[0..2] as the guest builds them. */
static void fast_overlay(FastWork *w, const uint8_t *s)
{
    for (int k = 0; k < 3; k++) {
        uint32_t i = s[k], code = w->code[i];
        uint8_t *dst = w->tri + 64 + 64 * k;
        float q = w->q[i];
        uint32_t v[8];
        memcpy(&v[0], w->pos[i], 16);
        if (!(code & OUT_NEAR))
            memcpy(&v[2], &q, 4);
        v[4] = w->tl[i].col;
        v[5] = w->spec[i];
        memcpy(&v[6], &w->tl[i].tu, 4);
        memcpy(&v[7], &w->tl[i].tv, 4);
        memcpy(dst, v, 32);
        dst[32] = (uint8_t)code;
        if (!(code & OUT_NEAR)) {
            float a[6];
            a[0] = (float)((w->tl[i].col >> 16) & 0xFFu) * q;
            a[1] = (float)((w->tl[i].col >> 8) & 0xFFu) * q;
            a[2] = (float)(w->tl[i].col & 0xFFu) * q;
            a[3] = w->tl[i].tu * q;
            a[4] = w->tl[i].tv * q;
            a[5] = (float)(w->spec[i] >> 24) * q;
            memcpy(dst + 40, a, 24);
        }
    }
}

/* Detail pass of a patch whose flag overlay changes later strip entries: entry
 * by entry as emit_strips, clipping entries not clipped before on demand. */
static void fast_emit_detail(const Frame *f, FastWork *w, const uint8_t *strip, int32_t nwords, int32_t ntotal,
                             uint32_t batch, uint32_t vbase)
{
    uint32_t vb = RD32(G_VBUF_PTR);
    uint8_t *ip = g_mem + batch + BATCH_INDICES + RD32(batch + BATCH_TRIS) * 6u;
    uint32_t m = 0, ntris = 0, nclip = 0;
    for (int32_t j = 0; j < nwords; j++) {
        uint32_t fl = w->tri[j];
        uint32_t word = rd32(strip + 4 * j);
        if (fl == 0) {
            uint16_t x[3] = { (uint16_t)((word & 0xFFu) + vbase), (uint16_t)(((word >> 16) & 0xFFu) + vbase),
                              (uint16_t)(((word >> 8) & 0xFFu) + vbase) };
            memcpy(ip, x, 6);
            ip += 6;
            ntris++;
        } else if (!(fl & TRI_CULLED)) {
            uint8_t s[3] = { (uint8_t)word, (uint8_t)(word >> 16), (uint8_t)(word >> 8) };
            fast_overlay(w, s);
            if (!w->cdone[j]) {
                w->cstart[j] = (uint16_t)w->ncv;
                w->ccount[j] = (uint16_t)fclip_entry(f, w, word);
                w->cdone[j] = 1;
            }
            const FClipV *o = &w->cv[w->cstart[j]];
            for (uint32_t k = 0; k < 3u * w->ccount[j]; k++, m++) {
                fast_clip_vertex(f, w, g_mem + vb + 32u * ((uint32_t)ntotal + m), &o[k]);
                uint16_t x = (uint16_t)((uint32_t)ntotal + m + vbase);
                memcpy(ip, &x, 2);
                ip += 2;
            }
            ntris += w->ccount[j];
            nclip += w->ccount[j];
        }
    }
    fast_pass_end(batch, ntris, nclip, (uint32_t)ntotal + m);
}

/* Culls the strip entries as the exact version (flags into w->tri, used
 * vertices); returns 1 when every triangle is culled. */
static __attribute__((noinline)) uint32_t fast_cull(FastWork *w, const uint8_t *strip, int32_t nwords, uint32_t sflags)
{
    float wind0 = (sflags >> 2) & 1u ? -1.0f : 1.0f, wind = wind0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    uint32_t c0 = 0, c1 = 0, c2 = 0, i0 = 0, i1 = 0, i2 = 0;
    uint32_t allculled = 1;
    int32_t k = 0;
    for (int32_t j = 0; j < nwords; j++) {
        uint32_t word = rd32(strip + 4 * j);
        x0 = x1;
        y0 = y1;
        x1 = x2;
        y1 = y2;
        c0 = c1;
        c1 = c2;
        i0 = i1;
        i1 = i2;
        if (k == 0) {
            i0 = (word >> 16) & 0xFFu;
            c0 = w->code[i0];
            x0 = w->pos[i0][0];
            y0 = w->pos[i0][1];
            i1 = (word >> 8) & 0xFFu;
            c1 = w->code[i1];
            x1 = w->pos[i1][0];
            y1 = w->pos[i1][1];
        }
        i2 = word & 0xFFu;
        c2 = w->code[i2];
        x2 = w->pos[i2][0];
        y2 = w->pos[i2][1];
        uint32_t fl = c0 | c1 | c2 | TRI_CULLED;
        if (!(c0 & c1 & c2)) {
            float area = (x1 - x2) * (y0 - y2) - (x0 - x2) * (y1 - y2);
            if (!(area * wind >= -0.001f)) {
                w->unused[i2] = 0;
                w->unused[i1] = 0;
                w->unused[i0] = 0;
                fl ^= TRI_CULLED;
                allculled = 0;
            }
        }
        w->tri[j] = (uint8_t)fl;
        wind = -wind;
        k++;
        if (word & 0x10000000u) {
            k = 0;
            wind = wind0;
        }
    }
    return allculled;
}

static void fast_patch(const Frame *f, FastWork *w, uint32_t patch)
{
    const uint8_t *pp = g_mem + patch;
    double center[3];
    memcpy(center, pp, sizeof(center));
    uint32_t hdr = rd32(pp + 0x18);
    int32_t nbase = (int32_t)hdr >> 16;
    int32_t ntotal = (int32_t)(hdr & 0xFFFFu);
    uint32_t lod = rd32(pp + 0x1C);
    float lod_k = (float)(0.375 / (double)(int32_t)(1u << ((25u - lod) & 31u)));
    int32_t radius = 0x40000000 >> (lod & 31u);

    /* The patch range test is the exact version's (once per patch). */
    double dx = center[0] - f->cam[0], dy = center[1] - f->cam[1], dz = center[2] - f->cam[2];
    double depth = (dz * f->rot[8] + dy * f->rot[7]) + dx * f->rot[6];
    if ((double)radius * (double)1.73f + depth >= f->patch_range)
        return;

    const uint8_t *base = pp + 0x20;
    uint32_t passes = fast_base_vertices(f, w, base, nbase, lod_k);
    /* Morph vertices: {x, y, z, parent}; they move towards their parent base vertex. */
    const uint8_t *vp = base + 16 * nbase;
    for (int32_t i = nbase; i < ntotal; i++, vp += 16) {
        float x = rdf(vp), y = rdf(vp + 4), z = rdf(vp + 8);
        int32_t parent = (int32_t)rd32(vp + 12);
        if (parent >= 0 && parent < 256) {
            float t = fast_morph_of(w, parent, nbase);
            const uint8_t *pv = base + 16 * parent;
            x += (rdf(pv) - x) * t;
            y += (rdf(pv + 4) - y) * t;
            z += (rdf(pv + 8) - z) * t;
        } else {
            note_stale();
        }
        w->x[i] = x + f->ncam[0];
        w->y[i] = y + f->ncam[1];
        w->z[i] = z + f->ncam[2];
    }
    fast_project(f, w, ntotal);
    int behind = 0;
    for (int32_t i = 0; i < ntotal; i++) {
        behind |= (int)w->code[i];
        w->unused[i] = 1;
    }
    behind &= OUT_NEAR;

    uint32_t sflags = rd32(vp);
    WR32(G_STRIP_FLIP, (sflags >> 2) & 1u);
    const uint8_t *strip = g_mem + rd32(vp + 4);
    int32_t ngroups = (int32_t)rd32(vp + 8);
    int32_t nwords = (int32_t)rd32(vp + 12);
    const uint8_t *rec = vp + 16;

    if (!behind) {
        if (fast_cull(w, strip, nwords, sflags))
            return;
    } else {
        if (nwords > 0)
            memset(w->tri, OUT_NEAR, (size_t)nwords);
        if (ntotal > 0)
            memset(w->unused, 0, (size_t)ntotal);
    }

    int32_t first_clip = fast_prepare(f, w, strip, nwords, ntotal);

    int first_batch = 1;
    int32_t g = 0;
    do {
        for (uint32_t pass = 1; pass < 3; pass++) {
            if (!(passes & pass)) {
                rec += 4 + 20 * ntotal - 8 * nbase;
                continue;
            }
            uint32_t material = rd32(rec);
            uint32_t vbase = RD32(G_VBUF_COUNT);
            rec = fast_pass_vertices(w, rec + 4, nbase, ntotal, RD32(G_VBUF_PTR), 0);
            uint32_t batch = fast_get_batch(f, w, material, first_batch ? 0u : 1u);
            first_batch = 0;
            fast_emit(f, w, ntotal, batch, vbase);
        }
    } while (++g < ngroups);

    uint32_t detail = rd32(rec);
    if (!detail)
        return;
    uint32_t vbase = RD32(G_VBUF_COUNT);
    fast_pass_vertices(w, rec + 4, nbase, ntotal, RD32(G_VBUF_PTR), 1);
    uint32_t batch = fast_get_batch(f, w, detail, 2);
    /* Clipped triangles overwrite the flags of entries 64 and up (the guest's
     * overlay): entry by entry when a later one is affected. */
    if (first_clip >= 0 && (first_clip + 1 > 64 ? first_clip + 1 : 64) < nwords)
        fast_emit_detail(f, w, strip, nwords, ntotal, batch, vbase);
    else
        fast_emit(f, w, ntotal, batch, vbase);
}

int rt_native_terrain_render(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    NATIVE_ENTRY(0x00512C30u);
    if (!terrain_supported())
        return 0;
    Frame f;
    f.c = c;
    f.sp = esp - 0x1500u;              /* bottom of the guest's frame (0x14F0 locals, 4 registers) */
    setup_frame(&f);
    setup_device(&f);
    WR32(G_VBUF_COUNT, 0);
    uint32_t lock[3] = { 0x2020u, G_VBUF_PTR, 0 };
    uint32_t hr = com_call(&f, RD32(G_VBUF), D3DVB_LOCK, 3, lock);
    if (hr != 0) {
        *ret = ((uint64_t)(esp + 4u) << 32) | hr;
        return 1;
    }

    int32_t npatch = (int32_t)RD32(G_PATCH_COUNT);
    WR32(G_STAT_PATCHES, RD32(G_STAT_PATCHES) + (uint32_t)npatch);
    WR32(G_STAT_2, 0);
    memcpy(f.cam, g_mem + G_CAM_POS, sizeof(f.cam));
    for (int k = 0; k < 9; k++)
        f.rot[k] = gf(G_CAM_ROT + 4u * (uint32_t)k);
    f.lod_bias = (float)(int32_t)RD32(G_LOD_BIAS);
    f.lod_scale = gf(G_LOD_SCALE);
    f.near_z = gf(G_NEAR);
    f.patch_range = gf(G_PATCH_RANGE);
    f.left = gf(G_CLIP_LEFT);
    f.top = gf(G_CLIP_TOP);
    f.right = gf(G_CLIP_RIGHT);
    f.bottom = gf(G_CLIP_BOTTOM);
    f.proj_sx = gf(G_PROJ_SX);
    f.proj_sy = gf(G_PROJ_SY);
    f.proj_cx = gf(G_PROJ_CX);
    f.proj_cy = gf(G_PROJ_CY);
    f.rhw_scale = gf(G_RHW_SCALE);
    f.z_scale = gf(G_Z_SCALE);
    f.z_offset = gf(G_Z_OFFSET);
    f.fog_depth = gf(G_FOG_DEPTH);
    f.fog_density = gf(G_FOG_DENSITY);
    for (int k = 0; k < 3; k++)
        f.ncam[k] = -(float)f.cam[k];

    f.fog_k = (float)((double)f.fog_depth * f.fog_density * LOG2E);
    f.chop = (c->fcw & 0x0C00u) == 0x0C00u;

    /* One terrain pass at a time (game thread). */
    if (g_terrain_exact) {
        static PatchWork work;
        memset(work.blend, 0, sizeof(work.blend));
        for (int32_t i = 0; i < npatch; i++) {
            uint32_t patch = RD32(G_PATCH_LIST + 4u * (uint32_t)i);
            if (patch)
                draw_patch(&f, &work, patch);
        }
    } else {
        static FastWork fwork;
        memset(fwork.blend, 0, sizeof(fwork.blend));
        memset(fwork.bc_batch, 0, sizeof(fwork.bc_batch));
        for (int32_t i = 0; i < npatch; i++) {
            uint32_t patch = RD32(G_PATCH_LIST + 4u * (uint32_t)i);
            if (patch)
                fast_patch(&f, &fwork, patch);
        }
    }

    flush_batches(&f);
    *ret = ((uint64_t)(esp + 4u) << 32) | RD32(G_BATCH_COUNT);
    return 1;
}
