/* GL backend of the renderer: sets GL state through caches and draws, from the
 * commands render_gl.c sends (render_backend.h). It runs on the render thread
 * (or inline without it) and only ever uses its own copies of the front end's
 * state, so it never reads g_rs. */
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(__vita__)
#include <vitaGL.h>
#else
#include <GL/gl.h>
#endif
/* Direct GL calls: this code is what the render thread runs. */
#define RQ_NO_REDIRECT
#include "render_queue.h"
#include "render_backend.h"

extern uint64_t win32_perf_counter(void);   /* microseconds (the game's clock: single thread only) */

static FrameParams B;

void bk_params(const void *p)
{
    B = *(const FrameParams *)p;
}

/* State setup and draw-call time (single thread with timing on). */
static uint64_t t_state_us, t_gl_us;

static uint64_t bk_now(void)
{
    return B.timing ? win32_perf_counter() : 0u;
}

void bk_take_timing(uint64_t *state_us, uint64_t *gl_us)
{
    *state_us = t_state_us;
    *gl_us = t_gl_us;
    t_state_us = t_gl_us = 0;
}

/* ---- GL state cache ------------------------------------------------------
 * D3D state is re-applied before every draw. Forwarding redundant calls is
 * cheap on desktop GL but on vitaGL most of them invalidate the generated
 * fixed-function shaders, so only real changes reach GL.
 */
#define CACHE_SLOTS 128
typedef struct CacheSlot {
    uint32_t key;
    int valid;
    uint32_t w[4];   /* the float values' bits */
} CacheSlot;
static CacheSlot cache[CACHE_SLOTS];
static GLenum cur_unit = GL_TEXTURE0, cur_client_unit = GL_TEXTURE0;

/* Changes of the state apply_states sets (capabilities other than texturing,
 * blend/depth/alpha/cull/shading/polygon settings, fog): apply_states is skipped
 * while its inputs and this count are what they were after its last run. */
static uint32_t rs_changes;

static inline int rs_key(uint32_t key)
{
    uint32_t fam = key >> 28;
    return (fam == 1u && (key & 0xFFFFu) != GL_TEXTURE_2D) || fam == 3u || fam == 5u;
}

/* Returns 1 if the value changed (and records it). Called dozens of times per
 * draw with a constant n: inlined, and the values compare as words. */
static inline __attribute__((always_inline)) int cache_set(uint32_t key, const float *v, int n)
{
    uint32_t w[4];
    memcpy(w, v, (size_t)n * sizeof(float));
    uint32_t h = (key * 2654435761u) >> 25;   /* 7 bits */
    for (uint32_t i = 0; i < CACHE_SLOTS; i++) {
        CacheSlot *s = &cache[(h + i) % CACHE_SLOTS];
        if (s->valid && s->key == key) {
            int same = 1;
            for (int k = 0; k < n; k++)
                same &= s->w[k] == w[k];
            if (same)
                return 0;
            for (int k = 0; k < n; k++)
                s->w[k] = w[k];
            rs_changes += (uint32_t)rs_key(key);
            return 1;
        }
        if (!s->valid) {
            s->valid = 1;
            s->key = key;
            for (int k = 0; k < 4; k++)
                s->w[k] = k < n ? w[k] : 0u;
            rs_changes += (uint32_t)rs_key(key);
            return 1;
        }
    }
    rs_changes++;
    return 1;   /* table full: never skip */
}

static uint32_t unit_key(GLenum cap)
{
    /* Per-unit capabilities carry the unit in the high bits. */
    if (cap == GL_TEXTURE_2D)
        return cap | ((uint32_t)(cur_unit - GL_TEXTURE0) << 24);
    if (cap == GL_TEXTURE_COORD_ARRAY)
        return cap | ((uint32_t)(cur_client_unit - GL_TEXTURE0) << 24);
    return cap;
}

/* The capabilities and client arrays set for every draw have their own slots
 * (0 unknown, 1 off, 2 on): no hashing on the way. Others use the cache. */
static uint8_t enable_slots[11], client_slots[6];

static int enable_slot(GLenum cap)
{
    switch (cap) {
    case GL_DEPTH_TEST: return 0;
    case GL_BLEND: return 1;
    case GL_ALPHA_TEST: return 2;
    case GL_CULL_FACE: return 3;
    case GL_FOG: return 4;
    case GL_LIGHTING: return 5;
    case GL_COLOR_MATERIAL: return 6;
    case GL_NORMALIZE: return 7;
    case GL_SCISSOR_TEST: return 8;
    case GL_TEXTURE_2D: return cur_unit == GL_TEXTURE0 ? 9 : cur_unit == GL_TEXTURE1 ? 10 : -1;
    default: return -1;
    }
}

static int client_slot(GLenum cap)
{
    switch (cap) {
    case GL_VERTEX_ARRAY: return 0;
    case GL_NORMAL_ARRAY: return 1;
    case GL_COLOR_ARRAY: return 2;
#if defined(GL_FOG_COORDINATE_ARRAY)
    case GL_FOG_COORDINATE_ARRAY: return 3;
#endif
    case GL_TEXTURE_COORD_ARRAY:
        return cur_client_unit == GL_TEXTURE0 ? 4 : cur_client_unit == GL_TEXTURE1 ? 5 : -1;
    default: return -1;
    }
}

static void c_enable(GLenum cap, int on)
{
    const int slot = enable_slot(cap);
    if (slot >= 0) {
        const uint8_t want = on ? 2u : 1u;
        if (enable_slots[slot] == want)
            return;
        enable_slots[slot] = want;
        rs_changes += cap != GL_TEXTURE_2D;
    } else {
        float v = (float)on;
        if (!cache_set(0x10000000u ^ unit_key(cap), &v, 1))
            return;
    }
    if (on)
        (glEnable)(cap);
    else
        (glDisable)(cap);
}

static void c_client(GLenum cap, int on)
{
    const int slot = client_slot(cap);
    if (slot >= 0) {
        const uint8_t want = on ? 2u : 1u;
        if (client_slots[slot] == want)
            return;
        client_slots[slot] = want;
    } else {
        float v = (float)on;
        if (!cache_set(0x20000000u ^ unit_key(cap), &v, 1))
            return;
    }
    if (on)
        (glEnableClientState)(cap);
    else
        (glDisableClientState)(cap);
}

static void c_blend(GLenum s, GLenum d)
{
    float v[2] = { (float)s, (float)d };
    if (cache_set(0x30000001u, v, 2))
        (glBlendFunc)(s, d);
}

static void c_depthfunc(GLenum f)
{
    float v = (float)f;
    if (cache_set(0x30000002u, &v, 1))
        (glDepthFunc)(f);
}

static void c_depthmask(GLboolean m)
{
    float v = (float)m;
    if (cache_set(0x30000003u, &v, 1))
        (glDepthMask)(m);
}

static void c_alphafunc(GLenum f, GLfloat ref)
{
    float v[2] = { (float)f, ref };
    if (cache_set(0x30000004u, v, 2)) {
        (glAlphaFunc)(f, ref);
#if defined(__vita__)
        /* Audit experiment (diagnostic mode 8): refresh fragment uniforms through
         * glUseProgram(0). Off by default: together with modes 9 and 10 it turned
         * the terrain and sky white. */
        if (B.diag == DIAG_USEPROGRAM)
            glUseProgram(0);
#endif
    }
}

static void c_cullface(GLenum m)
{
    float v = (float)m;
    if (cache_set(0x30000005u, &v, 1))
        (glCullFace)(m);
}

static void c_frontface(GLenum m)
{
    float v = (float)m;
    if (cache_set(0x30000006u, &v, 1))
        (glFrontFace)(m);
}

static void c_shademodel(GLenum m)
{
    float v = (float)m;
    if (cache_set(0x30000007u, &v, 1))
        (glShadeModel)(m);
}

static void c_polygonmode(GLenum face, GLenum m)
{
    float v = (float)m;
    (void)face;
    if (cache_set(0x30000008u, &v, 1))
        (glPolygonMode)(GL_FRONT_AND_BACK, m);
}

static void c_activetexture(GLenum u)
{
    if (u != cur_unit) {
        cur_unit = u;
        (glActiveTexture)(u);
    }
}

static void c_clientactivetexture(GLenum u)
{
    if (u != cur_client_unit) {
        cur_client_unit = u;
        (glClientActiveTexture)(u);
    }
}

/* Combiner setup last sent to each texture unit by apply_stages (see there). */
static struct {
    int valid;
    uint32_t colorop, colorarg1, colorarg2, alphaop, alphaarg1, alphaarg2, factor;
} stage_env[MAX_STAGES];
static int in_stage_env;   /* apply_stages is sending a combiner setup */

static void stage_env_touched(void)
{
    uint32_t u = (uint32_t)(cur_unit - GL_TEXTURE0);
    if (!in_stage_env && u < MAX_STAGES)
        stage_env[u].valid = 0;
}

static void c_texenvi(GLenum target, GLenum pname, GLint value)
{
    stage_env_touched();
    float v = (float)value;
    if (cache_set(0x40000000u ^ ((uint32_t)(cur_unit - GL_TEXTURE0) << 20) ^ pname, &v, 1))
        (glTexEnvi)(target, pname, value);
}

static void c_texenvfv(GLenum target, GLenum pname, const GLfloat *value)
{
    stage_env_touched();
    if (cache_set(0x40000000u ^ ((uint32_t)(cur_unit - GL_TEXTURE0) << 20) ^ pname, value, 4)) {
        GLfloat v[4];   /* vitaGL takes a non-const pointer */
        memcpy(v, value, sizeof(v));
        (glTexEnvfv)(target, pname, v);
#if defined(__vita__)
        if (B.diag == DIAG_USEPROGRAM)
            glUseProgram(0);   /* refresh fragment uniforms (combiner scale/color) */
#endif
    }
}

static void c_fogi(GLenum pname, GLint value)
{
    float v = (float)value;
    if (cache_set(0x50000000u ^ pname, &v, 1))
        (glFogi)(pname, value);
}

static void c_fogf(GLenum pname, GLfloat value)
{
    if (cache_set(0x50000000u ^ pname, &value, 1))
        (glFogf)(pname, value);
}

static void c_fogfv(GLenum pname, const GLfloat *value)
{
    if (cache_set(0x50000000u ^ pname, value, 4))
        (glFogfv)(pname, value);
}

#define glEnable(c) c_enable(c, 1)
#define glDisable(c) c_enable(c, 0)
#define glEnableClientState(c) c_client(c, 1)
#define glDisableClientState(c) c_client(c, 0)
#define glBlendFunc c_blend
#define glDepthFunc c_depthfunc
#define glDepthMask c_depthmask
#define glAlphaFunc c_alphafunc
#define glCullFace c_cullface
#define glFrontFace c_frontface
#define glShadeModel c_shademodel
#define glPolygonMode c_polygonmode
#define glActiveTexture c_activetexture
#define glClientActiveTexture c_clientactivetexture
#define glTexEnvi c_texenvi
#define glTexEnvfv c_texenvfv
#define glFogi c_fogi
#define glFogf c_fogf
#define glFogfv c_fogfv

/* Viewport, depth range and matrices: vitaGL recomputes the MVP (and the normal
 * matrix when lighting) and re-uploads the vertex uniforms after every load, so
 * identical reloads between draws are skipped. All matrix loads must go
 * through c_load_matrix (no glLoadIdentity/glOrtho/glMultMatrix elsewhere). */
static struct {
    int vp_valid, dr_valid, proj_valid, mv_valid, mode_valid;
    GLint vp[4];
    float dr[2];
    float proj[16], mv[16];
    GLenum mode;
} mcache;

static void c_viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (mcache.vp_valid && mcache.vp[0] == x && mcache.vp[1] == y && mcache.vp[2] == w && mcache.vp[3] == h)
        return;
    mcache.vp_valid = 1;
    mcache.vp[0] = x;
    mcache.vp[1] = y;
    mcache.vp[2] = w;
    mcache.vp[3] = h;
    (glViewport)(x, y, w, h);
}

static void c_depthrange(float n, float f)
{
    if (mcache.dr_valid && mcache.dr[0] == n && mcache.dr[1] == f)
        return;
    mcache.dr_valid = 1;
    mcache.dr[0] = n;
    mcache.dr[1] = f;
    (glDepthRange)(n, f);
}

/* 4x4 matrices copied and compared inline: on the Vita memcpy and memset are
 * calls into SceLibKernel, several per draw otherwise. */
typedef struct Mat4 {
    float m[16];
} Mat4;

static inline void mat_copy(float *d, const float *s)
{
    *(Mat4 *)d = *(const Mat4 *)s;
}

static inline int mat_equal(const float *a, const float *b)
{
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    uint32_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= x[i] ^ y[i];
    return diff == 0;
}

static void mat_mul(const float *a, const float *b, float *out)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    mat_copy(out, r);
}

/* Loads `m` into the projection or modelview matrix; leaves that matrix mode current. */
static void c_load_matrix(GLenum mode, const float *m)
{
    int proj = mode == GL_PROJECTION;
    float *slot = proj ? mcache.proj : mcache.mv;
    int *valid = proj ? &mcache.proj_valid : &mcache.mv_valid;
    if (!mcache.mode_valid || mcache.mode != mode) {
        mcache.mode_valid = 1;
        mcache.mode = mode;
        glMatrixMode(mode);
    }
    if (*valid && mat_equal(slot, m))
        return;
    mat_copy(slot, m);
    *valid = 1;
    glLoadMatrixf(m);
}

/* Column-major glOrtho matrix (computed in double like the GL call). */
static void ortho_matrix(double l, double r, double b, double t, double n, double f, float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = (float)(2.0 / (r - l));
    m[5] = (float)(2.0 / (t - b));
    m[10] = (float)(-2.0 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1.0f;
}

static const float identity_matrix[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

/* Current color (vitaGL re-uploads fragment uniforms on every change). GL leaves
 * the current color undefined after a draw with a color array, so such draws
 * reset the cache (color_cache_valid = 0). */
static float color_cache[4];
static int color_cache_valid;

static void c_color(float r, float g, float b, float a)
{
    if (color_cache_valid && color_cache[0] == r && color_cache[1] == g && color_cache[2] == b && color_cache[3] == a)
        return;
    color_cache_valid = 1;
    color_cache[0] = r;
    color_cache[1] = g;
    color_cache[2] = b;
    color_cache[3] = a;
    (glColor4f)(r, g, b, a);
}
#define glColor4f c_color

/* Paint the area outside the game image black (glClear alone is not reliable
 * outside the viewport on vitaGL). */
void bk_bars(const void *unused)
{
    (void)unused;
    if (B.vp_w == B.fb_w && B.vp_h == B.fb_h)
        return;
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    c_viewport(0, 0, B.fb_w, B.fb_h);
    float m[16];
    ortho_matrix(0, B.fb_w, 0, B.fb_h, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
    c_load_matrix(GL_MODELVIEW, identity_matrix);
    c_color(0, 0, 0, 1);
    float x0 = (float)B.vp_x, x1 = (float)(B.vp_x + B.vp_w), y0 = (float)B.vp_y, y1 = (float)(B.vp_y + B.vp_h);
    float w = (float)B.fb_w, h = (float)B.fb_h;
    const float quads[4][4] = { { 0, 0, x0, h }, { x1, 0, w, h }, { 0, 0, w, y0 }, { 0, y1, w, h } };
    glBegin(GL_QUADS);
    for (int i = 0; i < 4; i++) {
        glVertex2f(quads[i][0], quads[i][1]);
        glVertex2f(quads[i][2], quads[i][1]);
        glVertex2f(quads[i][2], quads[i][3]);
        glVertex2f(quads[i][0], quads[i][3]);
    }
    glEnd();
}

void bk_init(const void *unused)
{
    (void)unused;
    c_viewport(0, 0, B.fb_w, B.fb_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* Game pixel rectangle -> GL window rectangle (y up). Every 3D draw asks for the
 * same rectangle: the last result is kept (the 64-bit divisions are library
 * calls on the Vita). */
static void to_fb(int32_t x, int32_t y, int32_t w, int32_t h, GLint *ox, GLint *oy, GLsizei *ow, GLsizei *oh)
{
    static int32_t key[10], out[4];
    static int valid;
    const int32_t k[10] = { x, y, w, h, B.vp_x, B.vp_y, B.vp_w, B.vp_h, B.rt_w, B.rt_h };
    int same = valid;
    for (int i = 0; i < 10; i++)
        same &= k[i] == key[i];
    if (same) {
        *ox = out[0];
        *oy = out[1];
        *ow = out[2];
        *oh = out[3];
        return;
    }
    int32_t x0 = (int32_t)((int64_t)x * B.vp_w / B.rt_w);
    int32_t x1 = (int32_t)((int64_t)(x + w) * B.vp_w / B.rt_w);
    int32_t y0 = (int32_t)((int64_t)y * B.vp_h / B.rt_h);
    int32_t y1 = (int32_t)((int64_t)(y + h) * B.vp_h / B.rt_h);
    *ox = B.vp_x + x0;
    *oy = B.vp_y + B.vp_h - y1;
    *ow = x1 - x0;
    *oh = y1 - y0;
    for (int i = 0; i < 10; i++)
        key[i] = k[i];
    out[0] = *ox;
    out[1] = *oy;
    out[2] = *ow;
    out[3] = *oh;
    valid = 1;
}

static int use_z_remap(void)
{
    return B.z_remap != (B.diag == DIAG_FLIP_ZREMAP);
}

/* Depth range for clip z in D3D form ([0,w]): GL maps NDC [-1,1] to [n,f], so
 * NDC [0,1] lands on [minz,maxz] with n = 2 minz - maxz. */
static void d3d_depth_range(float minz, float maxz)
{
    c_depthrange(2.0f * minz - maxz, maxz);
}

/* Orthographic projection over the whole render target in D3D pixel coordinates.
 * D3D samples pixel centers at integer coordinates, GL at +0.5, so the image is
 * moved by half a pixel. When the game image is scaled up, though, every window
 * pixel samples the texture, not just the game pixel's center: a point-sampled
 * texture mapped one texel per pixel (font glyphs) then shows half a texel of the
 * neighbouring glyph along the edges. Such draws (texel_snap) skip the offset, so
 * each game pixel covers exactly one texel; they end up half a game pixel lower
 * and to the right. */
static void load_screen_ortho(int texel_snap)
{
    c_viewport(B.vp_x, B.vp_y, B.vp_w, B.vp_h);
    double off = texel_snap && (B.vp_w != B.rt_w || B.vp_h != B.rt_h) ? 0.0 : 0.5;
    float m[16];
    ortho_matrix(-off, B.rt_w - off, B.rt_h - off, -off, 0.0, -1.0, m);
    if (use_z_remap()) {
        c_depthrange(0.0f, 1.0f);
    } else {
        m[10] = 1.0f;   /* clip z = screen z */
        m[14] = 0.0f;
        d3d_depth_range(0.0f, 1.0f);
    }
    c_load_matrix(GL_PROJECTION, m);
    c_load_matrix(GL_MODELVIEW, identity_matrix);
}

void bk_clear(const void *p)
{
    const ClearCmd *c = p;
    if (c->mask & GL_COLOR_BUFFER_BIT) {
        glClearColor(c->color[0], c->color[1], c->color[2], c->color[3]);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (c->mask & GL_DEPTH_BUFFER_BIT) {
        glClearDepth(c->z);
        glDepthMask(GL_TRUE);
    }
    /* vitaGL emulates the scissor by drawing a full-screen quad that obeys the
     * current depth state, so scissoring must never happen with depth writes on
     * unless depth is being cleared anyway, and is skipped for full clears. */
    for (uint32_t i = 0; i < c->n; i++) {
        if (c->r[i].full) {
            glDisable(GL_SCISSOR_TEST);
            glClear(c->mask);
            continue;
        }
        if (c->restore_mask)
            glDepthMask(GL_FALSE);
        glEnable(GL_SCISSOR_TEST);
        glScissor(c->r[i].x, c->r[i].y, c->r[i].w, c->r[i].h);
        glClear(c->mask);
        glDisable(GL_SCISSOR_TEST);
    }
    /* Depth mask is re-applied from the render state on the next draw. */
}

/* ---- 2D ----------------------------------------------------------------- */

static int blit_tex_w, blit_tex_h;

void bk_quad(const void *p)
{
    const QuadCmd *q = p;
    const int w = q->w, h = q->h;
    glBindTexture(GL_TEXTURE_2D, rq_tex_name(q->tex));
#if defined(__vita__)
    /* vitaGL's glTexSubImage2D writes in place while earlier draws of this frame are
     * still queued on the GPU; a full glTexImage2D gets fresh storage. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, q->px);
    blit_tex_w = w;
    blit_tex_h = h;
#else
    if (w != blit_tex_w || h != blit_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, q->px);
        blit_tex_w = w;
        blit_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, q->px);
    }
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glDisable(GL_SCISSOR_TEST);
    if (q->blend) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    glDisable(GL_BLEND);
    load_screen_ortho(0);
    float m[16];
    ortho_matrix(0, B.rt_w, B.rt_h, 0, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
    const Rect *sr = &q->sr, *dr = &q->dr;
    float u0 = (float)sr->l / (float)w, u1 = (float)sr->r / (float)w;
    float v0 = (float)sr->t / (float)h, v1 = (float)sr->b / (float)h;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0);
    glVertex2f((float)dr->l, (float)dr->t);
    glTexCoord2f(u1, v0);
    glVertex2f((float)dr->r, (float)dr->t);
    glTexCoord2f(u1, v1);
    glVertex2f((float)dr->r, (float)dr->b);
    glTexCoord2f(u0, v1);
    glVertex2f((float)dr->l, (float)dr->b);
    glEnd();
}

/* ---- 3D state ----------------------------------------------------------- */

static GLenum gl_blend(uint32_t b)
{
    switch (b) {
    case 1: return GL_ZERO;
    case 2: return GL_ONE;
    case 3: return GL_SRC_COLOR;
    case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA;
    case 6: return GL_ONE_MINUS_SRC_ALPHA;
    case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR;
    case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

static GLenum gl_cmp(uint32_t f)
{
    switch (f) {
    case 1: return GL_NEVER;
    case 2: return GL_LESS;
    case 3: return GL_EQUAL;
    case 4: return GL_LEQUAL;
    case 5: return GL_GREATER;
    case 6: return GL_NOTEQUAL;
    case 7: return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

static void color4(uint32_t argb, float *out)
{
    out[0] = ((argb >> 16) & 0xFFu) / 255.0f;
    out[1] = ((argb >> 8) & 0xFFu) / 255.0f;
    out[2] = (argb & 0xFFu) / 255.0f;
    out[3] = (argb >> 24) / 255.0f;
}

/* D3DTA_* -> GL combiner source/operand. */
static void ta_to_gl(uint32_t ta, int alpha, GLenum *src, GLenum *op)
{
    switch (ta & 0xFu) {
    case 0: *src = GL_PRIMARY_COLOR; break;
    case 1: *src = GL_PREVIOUS; break;
    case 2: *src = GL_TEXTURE; break;
    case 3: *src = GL_CONSTANT; break;
    default: *src = GL_PRIMARY_COLOR; break;   /* SPECULAR: not supported as a combiner input */
    }
    int comp = (ta & 0x10u) != 0;
    int rep = (ta & 0x20u) != 0 || alpha;
    if (rep)
        *op = comp ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    else
        *op = comp ? GL_ONE_MINUS_SRC_COLOR : GL_SRC_COLOR;
}

static void setup_combiner(int alpha, uint32_t op, uint32_t a1, uint32_t a2)
{
    GLenum combine = alpha ? GL_COMBINE_ALPHA : GL_COMBINE_RGB;
    GLenum src0 = alpha ? GL_SRC0_ALPHA : GL_SRC0_RGB, src1 = alpha ? GL_SRC1_ALPHA : GL_SRC1_RGB;
    GLenum src2 = alpha ? GL_SRC2_ALPHA : GL_SRC2_RGB;
    GLenum op0 = alpha ? GL_OPERAND0_ALPHA : GL_OPERAND0_RGB, op1 = alpha ? GL_OPERAND1_ALPHA : GL_OPERAND1_RGB;
    GLenum op2 = alpha ? GL_OPERAND2_ALPHA : GL_OPERAND2_RGB;
    GLenum scale = alpha ? GL_ALPHA_SCALE : GL_RGB_SCALE;
    GLenum s, o;
    GLenum mode = GL_MODULATE;
    float sc = 1.0f;
    uint32_t first = a1, second = a2;
    switch (op) {
    case 2: mode = GL_REPLACE; break;
    case 3: mode = GL_REPLACE; first = a2; break;
    case 4: mode = GL_MODULATE; break;
    case 5: mode = GL_MODULATE; sc = 2.0f; break;
    case 6: mode = GL_MODULATE; sc = 4.0f; break;
    case 7: mode = GL_ADD; break;
    case 8: mode = GL_ADD_SIGNED; break;
    case 9: mode = GL_ADD_SIGNED; sc = 2.0f; break;
    case 10: mode = GL_SUBTRACT; break;
    case 12: case 13: case 14: case 16:
        mode = GL_INTERPOLATE;
        break;
    default: mode = GL_MODULATE; break;
    }
    glTexEnvi(GL_TEXTURE_ENV, combine, (GLint)mode);
    ta_to_gl(first, alpha, &s, &o);
    glTexEnvi(GL_TEXTURE_ENV, src0, (GLint)s);
    glTexEnvi(GL_TEXTURE_ENV, op0, (GLint)o);
    ta_to_gl(second, alpha, &s, &o);
    glTexEnvi(GL_TEXTURE_ENV, src1, (GLint)s);
    glTexEnvi(GL_TEXTURE_ENV, op1, (GLint)o);
    if (mode == GL_INTERPOLATE) {
        GLenum is = op == 12 ? GL_PRIMARY_COLOR : op == 13 ? GL_TEXTURE : op == 14 ? GL_CONSTANT : GL_PREVIOUS;
        glTexEnvi(GL_TEXTURE_ENV, src2, (GLint)is);
        glTexEnvi(GL_TEXTURE_ENV, op2, GL_SRC_ALPHA);
    }
    /* glTexEnvfv: vitaGL only marks the scale uniform dirty through the vector form. */
    if (B.diag == DIAG_USEPROGRAM) {
        const GLfloat scv[4] = { sc, sc, sc, sc };
        glTexEnvfv(GL_TEXTURE_ENV, scale, scv);
    } else {
        glTexEnvf(GL_TEXTURE_ENV, scale, sc);
    }
}

/* Returns the number of active texture stages. */
static int apply_stages(const DrawCmd *d)
{
    int active = 0;
    float tf[4];
    color4(d->tfactor, tf);
    for (int i = 0; i < MAX_STAGES; i++) {
        const DrawStage *st = &d->stage[i];
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)i);
        /* A stage without a texture passes the previous result through, as in D3D
         * (the front end leaves tex 0 for every stage that is off). */
        if (!st->tex) {
            glDisable(GL_TEXTURE_2D);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            continue;
        }
        active++;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, rq_tex_name(st->tex));
        /* Sampler state lives in the texture object: only changes are sent. */
        if (st->sampler) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->magf);
#if !defined(__vita__)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, st->max_level);
#endif
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st->wrap_s);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st->wrap_t);
        }
        /* The combiner setup is a dozen texenv calls: skipped while this unit's
         * inputs are the ones it was last sent with. */
        if (!stage_env[i].valid || stage_env[i].colorop != st->colorop || stage_env[i].colorarg1 != st->colorarg1 ||
            stage_env[i].colorarg2 != st->colorarg2 || stage_env[i].alphaop != st->alphaop ||
            stage_env[i].alphaarg1 != st->alphaarg1 || stage_env[i].alphaarg2 != st->alphaarg2 ||
            stage_env[i].factor != d->tfactor) {
            in_stage_env = 1;
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
            setup_combiner(0, st->colorop, st->colorarg1, st->colorarg2);
            setup_combiner(1, st->alphaop == 1u ? 3u : st->alphaop, st->alphaarg1,
                           st->alphaop == 1u ? 1u : st->alphaarg2);
            in_stage_env = 0;
            stage_env[i].valid = 1;
            stage_env[i].colorop = st->colorop;
            stage_env[i].colorarg1 = st->colorarg1;
            stage_env[i].colorarg2 = st->colorarg2;
            stage_env[i].alphaop = st->alphaop;
            stage_env[i].alphaarg1 = st->alphaarg1;
            stage_env[i].alphaarg2 = st->alphaarg2;
            stage_env[i].factor = d->tfactor;
        }
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    return active;
}

void bk_sampler(const void *draw_stage)
{
    const DrawStage *st = draw_stage;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, rq_tex_name(st->tex));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, st->minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->magf);
#if !defined(__vita__)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, st->max_level);
#endif
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, st->wrap_s);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, st->wrap_t);
}

/* Lighting inputs: the latest from the front end, and what was last sent to GL.
 * vitaGL re-uploads its uniforms (and transforms light positions) on every
 * glLight/glMaterial call, so the whole setup is skipped while they match. */
static LightSnapshot light_in, light_snap;
static int light_in_new, light_snap_valid;

void bk_lights(const void *p)
{
    light_in = *(const LightSnapshot *)p;
    light_in_new = 1;
}

static void apply_lights(void)
{
    if (!light_in_new)
        return;
    light_in_new = 0;
    const LightSnapshot *cur = &light_in;
    if (light_snap_valid && !memcmp(cur, &light_snap, sizeof(*cur)))
        return;
    light_snap = *cur;
    light_snap_valid = 1;

    float amb[4];
    color4(cur->ambient, amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, cur->mat);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, cur->mat + 4);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, cur->mat + 8);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, cur->mat + 12);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, cur->mat[16] > 128.0f ? 128.0f : cur->mat[16]);
    if (cur->color_material)
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    if (cur->normalize)
        glEnable(GL_NORMALIZE);
    else
        glDisable(GL_NORMALIZE);
    /* Light positions are transformed by the modelview matrix current at glLight time. */
    c_load_matrix(GL_MODELVIEW, cur->view);
#if defined(__vita__)
    /* Lighting with no light still gives emissive + ambient * material in D3D and GL;
     * vitaGL then skips lighting altogether (the vertex color shows instead, often
     * white). A black directional light keeps its lighting path on. */
    int any = 0;
    for (int i = 0; i < MAX_LIGHTS; i++)
        any |= cur->lights[i].enabled;
    static const Light black_light = { .type = 3u, .dir = { 0.0f, 0.0f, 1.0f }, .enabled = 1 };
#endif
    for (int i = 0; i < MAX_LIGHTS; i++) {
        const Light *l = &cur->lights[i];
        GLenum gl = GL_LIGHT0 + (GLenum)i;
#if defined(__vita__)
        if (!any && i == 0)
            l = &black_light;
#endif
        if (!l->enabled) {
            glDisable(gl);
            continue;
        }
        glEnable(gl);
        glLightfv(gl, GL_DIFFUSE, l->diffuse);
        glLightfv(gl, GL_SPECULAR, l->specular);
        glLightfv(gl, GL_AMBIENT, l->ambient);
        float p[4];
        if (l->type == 3u) {
            p[0] = -l->dir[0];
            p[1] = -l->dir[1];
            p[2] = -l->dir[2];
            p[3] = 0.0f;
        } else {
            p[0] = l->pos[0];
            p[1] = l->pos[1];
            p[2] = l->pos[2];
            p[3] = 1.0f;
        }
        glLightfv(gl, GL_POSITION, p);
#if defined(__vita__)
        /* vitaGL takes attenuation only through glLightfv; it has no spot lights. */
        /* Directional lights (and degenerate all-zero factors) get no attenuation;
         * the slot may still hold a previous point light's factors. */
        int unatt = l->type == 3u || (l->att0 == 0.0f && l->att1 == 0.0f && l->att2 == 0.0f);
        GLfloat a0[4] = { unatt ? 1.0f : l->att0, 0, 0, 0 };
        GLfloat a1[4] = { unatt ? 0.0f : l->att1, 0, 0, 0 };
        GLfloat a2[4] = { unatt ? 0.0f : l->att2, 0, 0, 0 };
        glLightfv(gl, GL_CONSTANT_ATTENUATION, a0);
        glLightfv(gl, GL_LINEAR_ATTENUATION, a1);
        glLightfv(gl, GL_QUADRATIC_ATTENUATION, a2);
#else
        glLightf(gl, GL_CONSTANT_ATTENUATION, l->att0);
        glLightf(gl, GL_LINEAR_ATTENUATION, l->att1);
        glLightf(gl, GL_QUADRATIC_ATTENUATION, l->att2);
        if (l->type == 2u) {
            glLightfv(gl, GL_SPOT_DIRECTION, l->dir);
            glLightf(gl, GL_SPOT_CUTOFF, l->phi * 0.5f * 57.29578f);
            glLightf(gl, GL_SPOT_EXPONENT, l->falloff);
        } else {
            glLightf(gl, GL_SPOT_CUTOFF, 180.0f);
        }
#endif
    }
}

static void apply_states_now(const DrawCmd *d);

/* Consecutive draws mostly differ in texture or transforms only: the render
 * states are applied again only when an input differs from the last run or a
 * cached setting changed since (rs_changes). */
static void apply_states(const DrawCmd *d)
{
    /* Two key buffers used in turn: the new key is written where the one before
     * the last was (a copy of the key would be a SceLibc memcpy call). */
    enum { N = 26 };
    static uint32_t keys[2][N], changes_after;
    static int last, valid;
    uint32_t *k = keys[last ^ 1];
    const uint32_t *o = keys[last];
    k[0] = d->zenable;
    k[1] = d->fillmode;
    k[2] = d->shademode;
    k[3] = d->zwrite;
    k[4] = d->alphatest;
    k[5] = d->srcblend;
    k[6] = d->destblend;
    k[7] = d->cullmode;
    k[8] = d->zfunc;
    k[9] = d->alpharef;
    k[10] = d->alphafunc;
    k[11] = d->blend;
    k[12] = d->fog;
    k[13] = d->fogcolor;
    k[14] = d->fogtablemode;
    k[15] = d->fogstart;
    k[16] = d->fogend;
    k[17] = d->fogdensity;
    k[18] = d->colorkey;
    k[19] = d->lighting;
    k[20] = d->fogvertexmode;
    k[21] = (uint32_t)d->L.pretransformed;
    k[22] = (uint32_t)d->L.has_normal << 1 | (uint32_t)d->L.has_color;
    k[23] = (uint32_t)d->L.use_fog;
    k[24] = (uint32_t)B.diag;
    k[25] = (uint32_t)B.cull_override;
    const int lit = !d->L.pretransformed && d->lighting && d->L.has_normal;
    uint32_t diff = !valid || changes_after != rs_changes || (lit && light_in_new);
    for (int i = 0; i < N; i++)
        diff |= k[i] ^ o[i];
    if (!diff)
        return;
    apply_states_now(d);
    last ^= 1;
    changes_after = rs_changes;
    valid = 1;
}

static void apply_states_now(const DrawCmd *d)
{
    const int pretransformed = d->L.pretransformed;
    if (B.diag == DIAG_DEPTH_LEQUAL && !pretransformed) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else if (d->zenable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(d->zfunc));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(d->zwrite ? GL_TRUE : GL_FALSE);
    if (d->blend) {
        glEnable(GL_BLEND);
        uint32_t sb = d->srcblend, db = d->destblend;
        if (sb == 12u) {          /* BOTHSRCALPHA */
            sb = 5;
            db = 6;
        } else if (sb == 13u) {   /* BOTHINVSRCALPHA */
            sb = 6;
            db = 5;
        }
        glBlendFunc(gl_blend(sb), gl_blend(db));
    } else {
        glDisable(GL_BLEND);
    }
    if (d->alphatest) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(gl_cmp(d->alphafunc), (d->alpharef & 0xFFu) / 255.0f);
    } else if (d->colorkey) {
        /* Color-keyed texels were uploaded with alpha 0. */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (d->cullmode == 1u || pretransformed || B.cull_override == 1 || B.diag == DIAG_NO_CULL) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace((d->cullmode == 3u) != (B.cull_override == 2) ? GL_CW : GL_CCW);
    }
    glShadeModel(d->shademode == 1u ? GL_FLAT : GL_SMOOTH);
    glPolygonMode(GL_FRONT_AND_BACK, d->fillmode == 2u ? GL_LINE : d->fillmode == 1u ? GL_POINT : GL_FILL);

    if (d->fog) {
        glEnable(GL_FOG);
        float fc[4];
        color4(d->fogcolor, fc);
        glFogfv(GL_FOG_COLOR, fc);
        uint32_t mode = d->fogtablemode ? d->fogtablemode : d->fogvertexmode;
        float start, end, dens;
        memcpy(&start, &d->fogstart, 4);
        memcpy(&end, &d->fogend, 4);
        memcpy(&dens, &d->fogdensity, 4);
        if (pretransformed || !mode) {
#if defined(__vita__) && !defined(GL_FOG_COORDINATE_SOURCE)
            /* The SDK's vitaGL has no fog coordinates: game-computed vertex fog is not drawn. */
            glDisable(GL_FOG);
#else
            /* Vertex fog computed by the game: the factor is in the specular alpha
             * (no factor in the vertices: no fog, like a factor of 1). */
            if (d->L.use_fog) {
                glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
#if !defined(__vita__)
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glFogf(GL_FOG_START, 1.0f);
                glFogf(GL_FOG_END, 0.0f);
#endif
                /* The private vitaGL uses the coordinate as the fog factor whatever
                 * the mode and range (tools/vitagl), which then stay as they are. */
            } else {
#if defined(__vita__)
                glDisable(GL_FOG);
#else
                glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
                glFogi(GL_FOG_MODE, GL_LINEAR);
                glFogf(GL_FOG_START, 1.0f);
                glFogf(GL_FOG_END, 0.0f);
#endif
            }
#endif
        } else {
#if defined(GL_FOG_COORDINATE_SOURCE)
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
#endif
            glFogi(GL_FOG_MODE, mode == 1u ? GL_EXP : mode == 2u ? GL_EXP2 : GL_LINEAR);
            glFogf(GL_FOG_START, start);
            glFogf(GL_FOG_END, end);
            glFogf(GL_FOG_DENSITY, dens);
        }
    } else {
        glDisable(GL_FOG);
    }

    if (!pretransformed && d->lighting && d->L.has_normal) {
        glEnable(GL_LIGHTING);
        apply_lights();
        /* D3D takes the material color from the vertex only when the vertex has
         * one; otherwise the material's own colors apply. (vitaGL's color
         * material without a color array came out black.) */
        static int color_material = -1;
        int want = light_snap.color_material && d->L.has_color;
        if (want != color_material) {
            color_material = want;
            if (want)
                glEnable(GL_COLOR_MATERIAL);
            else
                glDisable(GL_COLOR_MATERIAL);
        }
    } else {
        glDisable(GL_LIGHTING);
    }
}

static void apply_transforms(const DrawCmd *d)
{
    if (d->L.pretransformed) {
        load_screen_ortho(d->stage[0].tex && d->stage[0].magf == GL_NEAREST);
        return;
    }
    GLint x, y;
    GLsizei w, h;
    to_fb(d->vp_x, d->vp_y, d->vp_w, d->vp_h, &x, &y, &w, &h);
    c_viewport(x, y, w, h);
    /* D3D clip-space z is [0, w]; GL wants [-w, w]: z' = 2z - w. */
    float p[16];
    mat_copy(p, d->proj);
    int remap = use_z_remap();
    if (remap)
        c_depthrange(d->vp_minz, d->vp_maxz);
    else
        d3d_depth_range(d->vp_minz, d->vp_maxz);
    if (remap)
        for (int r = 0; r < 4; r++)
            p[r * 4 + 2] = 2.0f * d->proj[r * 4 + 2] - d->proj[r * 4 + 3];
    c_load_matrix(GL_PROJECTION, p);
    /* Consecutive draws often differ in texture only: the product is kept with its factors. */
    static float last_w[16], last_v[16], wv[16];
    static int wv_valid;
    if (!wv_valid || !mat_equal(last_w, d->world) || !mat_equal(last_v, d->view)) {
        mat_copy(last_w, d->world);
        mat_copy(last_v, d->view);
        mat_mul(d->world, d->view, wv);
        wv_valid = 1;
    }
    c_load_matrix(GL_MODELVIEW, wv);
}

static GLenum gl_prim(int prim)
{
    switch (prim) {
    case PRIM_POINTS: return GL_POINTS;
    case PRIM_LINES: return GL_LINES;
    case PRIM_LINE_STRIP: return GL_LINE_STRIP;
    case PRIM_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case PRIM_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
    default: return GL_TRIANGLES;
    }
}

/* Client array pointers: re-sent only when they change (GL reads them at draw
 * time). The key includes the buffer object the pointer is an offset into. */
enum { PTR_VERTEX, PTR_COLOR, PTR_NORMAL, PTR_FOG, PTR_TEX0, PTR_COUNT = PTR_TEX0 + MAX_STAGES };
static struct {
    const void *p;
    GLuint vbo;
    int size, stride;
} ptr_cache[PTR_COUNT];

/* Forgets every cached pointer (after pointer calls made elsewhere). */
static void ptr_cache_reset(void)
{
    for (int i = 0; i < PTR_COUNT; i++)
        ptr_cache[i].size = -1;
}

static int ptr_changed(int slot, GLuint vbo, const void *p, int size, int stride)
{
    if (ptr_cache[slot].p == p && ptr_cache[slot].vbo == vbo && ptr_cache[slot].size == size &&
        ptr_cache[slot].stride == stride)
        return 0;
    ptr_cache[slot].p = p;
    ptr_cache[slot].vbo = vbo;
    ptr_cache[slot].size = size;
    ptr_cache[slot].stride = stride;
    return 1;
}

void bk_draw(const void *cmd)
{
    const DrawCmd *d = cmd;
    const Layout *L = &d->L;
    const DrawArrays *A = &d->A;
    const uint32_t nv = d->nv, ni = d->ni;
    uint64_t t0 = bk_now();
#if defined(__vita__)
    /* The arena buffer objects point at this draw's data (no copy in vitaGL). */
    if (A->vbo_data) {
        glBindBuffer(GL_ARRAY_BUFFER, rq_buf_name(A->vbo));
        vglBufferData(GL_ARRAY_BUFFER, A->vbo_data);
    }
    if (A->ibo_data) {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rq_buf_name(A->ibo));
        vglBufferData(GL_ELEMENT_ARRAY_BUFFER, A->ibo_data);
    }
#endif
    apply_states(d);
    int stages = apply_stages(d);
    apply_transforms(d);

    /* Pointers are offsets into A->vbo while it is bound (vitaGL and GL record
     * the binding with each pointer). */
    const GLuint vbo = A->vbo;
    const int st = A->stride;
    if (vbo)
        glBindBuffer(GL_ARRAY_BUFFER, rq_buf_name(vbo));
    glEnableClientState(GL_VERTEX_ARRAY);
    if (ptr_changed(PTR_VERTEX, vbo, A->pos, L->pos_size, st))
        glVertexPointer(L->pos_size, GL_FLOAT, st, A->pos);
    if (L->use_color) {
        glEnableClientState(GL_COLOR_ARRAY);
        if (ptr_changed(PTR_COLOR, vbo, A->col, 4, st))
            glColorPointer(4, GL_UNSIGNED_BYTE, st, A->col);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
    }
    if (L->has_normal) {
        glEnableClientState(GL_NORMAL_ARRAY);
        if (ptr_changed(PTR_NORMAL, vbo, A->nrm, 3, st))
            glNormalPointer(GL_FLOAT, st, A->nrm);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
#if !defined(__vita__)
    if (L->use_fog) {
        glEnableClientState(GL_FOG_COORDINATE_ARRAY);
        if (ptr_changed(PTR_FOG, vbo, A->fog, 1, st))
            glFogCoordPointer(GL_FLOAT, st, A->fog);
    } else {
        glDisableClientState(GL_FOG_COORDINATE_ARRAY);
        glFogCoordf(1.0f);
    }
#endif
    for (int s = 0; s < stages; s++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        if (s < L->stages && L->uv_src[s] >= 0) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
#if defined(__vita__)
            /* The fog coordinate rides along as uv0's third component (layout_init). */
            const int uv_size = s == 0 && L->use_fog ? 3 : 2;
#else
            const int uv_size = 2;
#endif
            if (ptr_changed(PTR_TEX0 + s, vbo, A->uv[s], uv_size, st))
                glTexCoordPointer(uv_size, GL_FLOAT, st, A->uv[s]);
        } else {
#if defined(__vita__)
            /* Audit experiment (diagnostic mode 9): zero texcoord array on units
             * without texture coordinates. Off by default. */
            if (B.diag != DIAG_ZERO_UV) {
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                ptr_cache[PTR_TEX0 + s].size = -1;
            } else {
                static float *zero_uv;
                static uint32_t zero_uv_cap;
                if (nv > zero_uv_cap) {
                    zero_uv_cap = nv + 1024u;
                    free(zero_uv);
                    zero_uv = calloc(zero_uv_cap, 8u);
                }
                if (vbo)
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, 0, zero_uv);
                ptr_cache[PTR_TEX0 + s].size = -1;
                if (vbo)
                    glBindBuffer(GL_ARRAY_BUFFER, rq_buf_name(vbo));
            }
#else
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glMultiTexCoord2f(GL_TEXTURE0 + (GLenum)s, 0, 0);
#endif
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
    if (vbo)
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    uint64_t t1 = bk_now();
    t_state_us += t1 - t0;

    if (!rq_on)
        PROF_HOST(PROF_H_GLDRAW);
    /* The index range is known: glDrawRangeElements spares vitaGL a scan of the
     * indices for the highest one before copying the vertex arrays. */
    if (A->ibo) {
        if (!A->ibo_data)
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rq_buf_name(A->ibo));
        glDrawRangeElements(gl_prim(d->prim), 0, nv - 1u, (GLsizei)ni, GL_UNSIGNED_SHORT, (const void *)0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else if (d->idx) {
        glDrawRangeElements(gl_prim(d->prim), 0, nv - 1u, (GLsizei)ni, GL_UNSIGNED_SHORT, d->idx);
    } else {
        glDrawArrays(gl_prim(d->prim), 0, (GLsizei)nv);
    }
    if (L->use_color)
        color_cache_valid = 0;   /* GL leaves the current color undefined after a color array */
    t_gl_us += bk_now() - t1;
}

/* ---- on-screen performance overlay ---------------------------------------- */

/* 3x5 pixel glyphs, one bit per pixel (4 = left column), rows top to bottom. */
static const uint16_t *glyph(char ch)
{
    static const uint16_t digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
        { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    };
    static const uint16_t letters[26][5] = {
        { 2, 5, 7, 5, 5 }, { 6, 5, 6, 5, 6 }, { 3, 4, 4, 4, 3 }, { 6, 5, 5, 5, 6 }, { 7, 4, 6, 4, 7 },   /* A-E */
        { 7, 4, 6, 4, 4 }, { 7, 4, 5, 5, 7 }, { 5, 5, 7, 5, 5 }, { 7, 2, 2, 2, 7 }, { 1, 1, 1, 5, 2 },   /* F-J */
        { 5, 5, 6, 5, 5 }, { 4, 4, 4, 4, 7 }, { 5, 7, 7, 5, 5 }, { 5, 7, 7, 7, 5 }, { 2, 5, 5, 5, 2 },   /* K-O */
        { 6, 5, 6, 4, 4 }, { 2, 5, 5, 6, 3 }, { 6, 5, 6, 5, 5 }, { 7, 4, 7, 1, 7 }, { 7, 2, 2, 2, 2 },   /* P-T */
        { 5, 5, 5, 5, 7 }, { 5, 5, 5, 5, 2 }, { 5, 5, 7, 7, 5 }, { 5, 5, 2, 5, 5 }, { 5, 5, 2, 2, 2 },   /* U-Y */
        { 7, 1, 2, 4, 7 },                                                                             /* Z */
    };
    static const uint16_t dot[5] = { 0, 0, 0, 0, 2 };
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'];
    if (ch >= 'A' && ch <= 'Z')
        return letters[ch - 'A'];
    if (ch == '.')
        return dot;
    return NULL;
}

/* Overlay quads as a vertex array (immediate mode is a call per vertex in vitaGL),
 * rebuilt only when the text changes. */
#define OVERLAY_MAX_VERTS 32768
static float ov_verts[OVERLAY_MAX_VERTS * 2];
static int ov_count;

static void ov_quad(float x0, float y0, float x1, float y1)
{
    if (ov_count + 4 > OVERLAY_MAX_VERTS)
        return;
    float *v = &ov_verts[ov_count * 2];
    v[0] = x0; v[1] = y0; v[2] = x1; v[3] = y0; v[4] = x1; v[5] = y1; v[6] = x0; v[7] = y1;
    ov_count += 4;
}

static void overlay_text(float x, float y, float px, const char *s)
{
    for (; *s; s++, x += 4.0f * px) {
        const uint16_t *g = glyph(*s);
        if (!g)
            continue;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if (g[row] & (4u >> col)) {
                    float x0 = x + (float)col * px, y0 = y + (float)row * px;
                    ov_quad(x0, y0, x0 + px, y0 + px);
                }
    }
}

void bk_overlay(const void *p)
{
    const OverlayCmd *o = p;
    size_t width = 0;
    for (int i = 0; i < OVERLAY_LINES; i++) {
        size_t l = strlen(o->lines[i]);
        width = l > width ? l : width;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_FOG);
    glActiveTexture(GL_TEXTURE1);
    glDisable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_TEXTURE_2D);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    c_viewport(0, 0, B.fb_w, B.fb_h);
    float m[16];
    ortho_matrix(0, B.fb_w, B.fb_h, 0, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
    c_load_matrix(GL_MODELVIEW, identity_matrix);
    float px = (float)B.fb_h / 181.0f;   /* ~3 px per font pixel at 544 lines */
    float bw = px * (float)(width * 4u + 2u), bh = px * (float)(OVERLAY_LINES * 6 + 1);
    static char prev[OVERLAY_LINES][40];
    static float prev_px;
    if (memcmp(prev, o->lines, sizeof(prev)) || prev_px != px) {
        memcpy(prev, o->lines, sizeof(prev));
        prev_px = px;
        ov_count = 0;
        ov_quad(0, 0, bw, bh);   /* background: first quad */
        for (int i = 0; i < OVERLAY_LINES; i++)
            overlay_text(px, px * (1.0f + 6.0f * (float)i), px, o->lines[i]);
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, ov_verts);
    ptr_cache_reset();
    glColor4f(0, 0, 0, 1);
    glDrawArrays(GL_QUADS, 0, 4);
    glColor4f(1, 1, 0, 1);
    if (ov_count > 4)
        glDrawArrays(GL_QUADS, 4, ov_count - 4);
    glColor4f(1, 1, 1, 1);
}
