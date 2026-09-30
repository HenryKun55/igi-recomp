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
#include "recomp.h"
#include "runtime.h"

#if defined(__clang__)
/* The guest rounds after every x87 operation: no fused multiply-add. */
#pragma STDC FP_CONTRACT OFF
#endif

int g_igi_native = 1;
/* Debug capture of the guest state at entry (runtime/src/capture.c, desktop). */
void (*g_native_capture)(Ctx *c, uint32_t esp);
/* Debug self-check that runs both implementations on the live frame (capture.c). */
int (*g_native_check)(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret);
int g_native_checking;

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
    double e = (double)vz * f->fog_depth * f->fog_density * LOG2E;
    double n = rt_fround(c, e);
    double fog = 1.0 / rt_fscale((exp2(e - n) - 1.0) + 1.0, n);
    if (BELOW(fog, 0.0))
        fog = 0.0;
    o->specular = fist(c, fog * 255.0) << 24;
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
        if (off + 4u > sizeof(w->blend))
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
        uint32_t g1 = grey3(fist(c, a));
        uint32_t g2 = grey3(fist(c, a * omt));
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

int rt_native_terrain_render(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax, uint64_t *ret)
{
    (void)ecx;
    (void)eax;
    if (g_native_capture)
        g_native_capture(c, esp);
    if (g_native_check && !g_native_checking)
        return g_native_check(c, esp, ecx, eax, ret);
    if (!g_igi_native || !terrain_supported())
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

    static PatchWork work;             /* one terrain pass at a time (game thread) */
    memset(work.blend, 0, sizeof(work.blend));
    for (int32_t i = 0; i < npatch; i++) {
        uint32_t patch = RD32(G_PATCH_LIST + 4u * (uint32_t)i);
        if (patch)
            draw_patch(&f, &work, patch);
    }

    flush_batches(&f);
    *ret = ((uint64_t)(esp + 4u) << 32) | RD32(G_BATCH_COUNT);
    return 1;
}
