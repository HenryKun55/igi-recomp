/* Fixed-function OpenGL backend for the DirectDraw / Direct3D 7 shims. */
#include "platform.h"
#include "igi_fixes.h"
#include "render.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(__vita__)
#include <vitaGL.h>
#else
#include <GL/gl.h>
#endif

RenderState g_rs;
static uint32_t perf_gpu_draws;   /* draws actually submitted to GL this frame */
static void perf_frame_begin_swap(void);
static void perf_swap(void);

extern uint64_t win32_perf_counter(void);   /* microseconds */

/* Per-frame render counters, averaged over one second and shown by the overlay
 * (see perf_overlay_draw for the on-screen labels). */
enum {
    PC_STATE_US,    /* apply_states + apply_stages + apply_transforms, texture uploads excluded */
    PC_TEX_US,      /* texture conversion + glTexImage2D */
    PC_PACK_US,     /* copying vertices into the per-attribute arrays */
    PC_GL_US,       /* inside glDrawElements/glDrawArrays (vitaGL: shaders, uniforms, vertex copies) */
    PC_2D_US,       /* 2D surface blits to the render target (outside D) */
    PC_TEX_N,       /* textures uploaded */
    PC_TEX_SKIP,    /* dirty textures whose pixels turned out unchanged (upload skipped) */
    PC_TEX_BYTES,   /* RGBA bytes uploaded */
    PC_VERTS,       /* vertices sent to GL */
    PC_INDICES,     /* indices sent to GL (0 for glDrawArrays) */
    PC_2D_N,        /* 2D blits */
    PC_2D_BYTES,    /* RGBA bytes converted for 2D blits */
    PC_READBACK_N,  /* render target read backs (glReadPixels) */
    PC_COUNT
};
static uint64_t pc_frame[PC_COUNT];

/* ---- GL state cache ------------------------------------------------------
 * D3D state is re-applied before every draw. Forwarding redundant calls is
 * cheap on desktop GL but on vitaGL most of them invalidate the generated
 * fixed-function shaders, so only real changes reach GL.
 */
#define CACHE_SLOTS 128
typedef struct CacheSlot {
    uint32_t key;
    int valid;
    float v[4];
} CacheSlot;
static CacheSlot cache[CACHE_SLOTS];
static GLenum cur_unit = GL_TEXTURE0, cur_client_unit = GL_TEXTURE0;

/* Returns 1 if the value changed (and records it). */
static int cache_set(uint32_t key, const float *v, int n)
{
    uint32_t h = (key * 2654435761u) >> 25;   /* 7 bits */
    for (uint32_t i = 0; i < CACHE_SLOTS; i++) {
        CacheSlot *s = &cache[(h + i) % CACHE_SLOTS];
        if (s->valid && s->key == key) {
            if (!memcmp(s->v, v, (size_t)n * sizeof(float)))
                return 0;
            memcpy(s->v, v, (size_t)n * sizeof(float));
            return 1;
        }
        if (!s->valid) {
            s->valid = 1;
            s->key = key;
            memset(s->v, 0, sizeof(s->v));
            memcpy(s->v, v, (size_t)n * sizeof(float));
            return 1;
        }
    }
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

static void c_enable(GLenum cap, int on)
{
    float v = (float)on;
    if (!cache_set(0x10000000u ^ unit_key(cap), &v, 1))
        return;
    if (on)
        (glEnable)(cap);
    else
        (glDisable)(cap);
}

static void c_client(GLenum cap, int on)
{
    float v = (float)on;
    if (!cache_set(0x20000000u ^ unit_key(cap), &v, 1))
        return;
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
        /* vitaGL re-uploads the alpha reference only on a shader change;
         * glUseProgram(0) marks all fragment uniforms dirty. */
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

static void c_texenvi(GLenum target, GLenum pname, GLint value)
{
    float v = (float)value;
    if (cache_set(0x40000000u ^ ((uint32_t)(cur_unit - GL_TEXTURE0) << 20) ^ pname, &v, 1))
        (glTexEnvi)(target, pname, value);
}

static void c_texenvfv(GLenum target, GLenum pname, const GLfloat *value)
{
    if (cache_set(0x40000000u ^ ((uint32_t)(cur_unit - GL_TEXTURE0) << 20) ^ pname, value, 4)) {
        GLfloat v[4];   /* vitaGL takes a non-const pointer */
        memcpy(v, value, sizeof(v));
        (glTexEnvfv)(target, pname, v);
#if defined(__vita__)
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
    int vp_valid, dr_valid, proj_valid, mv_valid;
    GLint vp[4];
    float dr[2];
    float proj[16], mv[16];
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

/* Loads `m` into the projection or modelview matrix; leaves that matrix mode current. */
static void c_load_matrix(GLenum mode, const float *m)
{
    int proj = mode == GL_PROJECTION;
    float *slot = proj ? mcache.proj : mcache.mv;
    int *valid = proj ? &mcache.proj_valid : &mcache.mv_valid;
    glMatrixMode(mode);
    if (*valid && !memcmp(slot, m, sizeof(mcache.proj)))
        return;
    memcpy(slot, m, sizeof(mcache.proj));
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


static int rt_w = 640, rt_h = 480;       /* game resolution */
static int fb_w = 640, fb_h = 480;       /* drawable size in pixels */
static int vp_x, vp_y, vp_w = 640, vp_h = 480;   /* game image inside the drawable (aspect kept) */

/* Output framing: 1 = stretch to fill the display, 0 = keep 4:3 with black bars.
 * IGI_ASPECT=fill|keep on desktop, "aspect=fill|keep" in ux0:data/igi/igi.cfg on the Vita. */
static int aspect_fill = -1;

/* Culling override for diagnosis: 0 normal, 1 off, 2 flipped (cull=off|flip). */
static int cull_override;
/* Live diagnostic mode (Vita: L+R+SELECT cycles it), shown on the overlay. */
enum { DIAG_NONE, DIAG_NO_BATCH, DIAG_NO_CULL, DIAG_FLIP_ZREMAP, DIAG_NO_SCREEN, DIAG_NO_3D, DIAG_DEPTH_LEQUAL,
       DIAG_COUNT };
static const char *const diag_names[DIAG_COUNT] = { "NORMAL", "NO BATCH", "NO CULL", "FLIP ZREMAP",
                                                     "NO 2D VERTS", "NO 3D VERTS", "DEPTH LEQUAL" };
static int diag_mode;

void render_diag_cycle(void)
{
    diag_mode = (diag_mode + 1) % DIAG_COUNT;
    rt_log("diagnostic mode %d: %s", diag_mode, diag_names[diag_mode]);
}
/* 1: remap D3D clip z [0,w] to GL [-w,w] (desktop GL). zremap=0 in igi.cfg disables it.
 * GXM clips z to [0,w] like D3D, so on the Vita the remap would clip away the
 * near half of the depth range: clip z stays in D3D form there and the depth
 * range is widened instead (see d3d_depth_range). */
#if defined(__vita__)
static int z_remap = 0;
#else
static int z_remap = 1;
#endif

static void load_aspect_setting(void)
{
    const char *v = getenv("IGI_ASPECT");
    const char *cv = getenv("IGI_CULL");
#if defined(__vita__)
    aspect_fill = 1;
    static char line[128], cline[128];
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f)) {
            if (strncmp(buf, "aspect=", 7) == 0) {
                memcpy(line, buf, sizeof(line));
                v = line + 7;
            } else if (strncmp(buf, "zremap=", 7) == 0) {
                z_remap = buf[7] != '0';
                rt_log("zremap=%d", z_remap);
            } else if (strncmp(buf, "cull=", 5) == 0) {
                memcpy(cline, buf, sizeof(cline));
                cv = cline + 5;
            }
        }
        fclose(f);
    }
#else
    aspect_fill = 0;
#endif
    if (v)
        aspect_fill = strncmp(v, "fill", 4) == 0;
    if (cv)
        cull_override = strncmp(cv, "off", 3) == 0 ? 1 : strncmp(cv, "flip", 4) == 0 ? 2 : 0;
    if (cull_override)
        rt_log("culling override: %s", cull_override == 1 ? "off" : "flipped");
}

static void update_letterbox(void)
{
    if (aspect_fill < 0)
        load_aspect_setting();
    platform_get_drawable_size(&fb_w, &fb_h);
    if (aspect_fill) {
        vp_w = fb_w;
        vp_h = fb_h;
    } else if ((int64_t)fb_w * rt_h > (int64_t)fb_h * rt_w) {
        vp_h = fb_h;
        vp_w = (int)((int64_t)fb_h * rt_w / rt_h);
    } else {
        vp_w = fb_w;
        vp_h = (int)((int64_t)fb_w * rt_h / rt_w);
    }
    vp_x = (fb_w - vp_w) / 2;
    vp_y = (fb_h - vp_h) / 2;
}

/* Paint the area outside the game image black (glClear alone is not reliable
 * outside the viewport on vitaGL). */
static void draw_bars(void)
{
    if (vp_w == fb_w && vp_h == fb_h)
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
    c_viewport(0, 0, fb_w, fb_h);
    float m[16];
    ortho_matrix(0, fb_w, 0, fb_h, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
    c_load_matrix(GL_MODELVIEW, identity_matrix);
    c_color(0, 0, 0, 1);
    float x0 = (float)vp_x, x1 = (float)(vp_x + vp_w), y0 = (float)vp_y, y1 = (float)(vp_y + vp_h);
    float w = (float)fb_w, h = (float)fb_h;
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

static GLuint white_tex;
static GLuint blit_tex;
static int blit_tex_w, blit_tex_h;
static uint32_t *conv_buf;
static size_t conv_cap;

static uint32_t *conv(size_t n)
{
    if (n > conv_cap) {
        conv_cap = n;
        conv_buf = realloc(conv_buf, n * 4u);
    }
    return conv_buf;
}

void render_init(int w, int h)
{
    rt_w = w;
    rt_h = h;
    update_letterbox();
    if (!white_tex) {
        uint32_t white = 0xFFFFFFFFu;
        glGenTextures(1, &white_tex);
        glBindTexture(GL_TEXTURE_2D, white_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &blit_tex);
    }
    /* D3D defaults. */
    memset(&g_rs, 0, sizeof(g_rs));
    g_rs.rs[7] = 1;          /* ZENABLE */
    g_rs.rs[14] = 1;         /* ZWRITEENABLE */
    g_rs.rs[19] = 2;         /* SRCBLEND ONE */
    g_rs.rs[20] = 1;         /* DESTBLEND ZERO */
    g_rs.rs[22] = 3;         /* CULLMODE CCW */
    g_rs.rs[23] = 4;         /* ZFUNC LESSEQUAL */
    g_rs.rs[25] = 8;         /* ALPHAFUNC ALWAYS */
    g_rs.rs[137] = 1;        /* LIGHTING */
    g_rs.rs[141] = 1;        /* COLORVERTEX */
    g_rs.rs[145] = 1;        /* DIFFUSEMATERIALSOURCE = COLOR1 */
    g_rs.rs[146] = 2;
    g_rs.rs[60] = 0xFFFFFFFFu;
    for (int i = 0; i < 16; i += 5) {
        g_rs.world[i] = g_rs.view[i] = g_rs.proj[i] = 1.0f;
    }
    for (int s = 0; s < MAX_STAGES; s++) {
        StageState *st = &g_rs.stage[s];
        st->colorop = s == 0 ? 4u : 1u;      /* MODULATE / DISABLE */
        st->colorarg1 = 2;                   /* TEXTURE */
        st->colorarg2 = 1;                   /* CURRENT */
        st->alphaop = s == 0 ? 2u : 1u;      /* SELECTARG1 / DISABLE */
        st->alphaarg1 = 2;
        st->alphaarg2 = 1;
        st->texcoordindex = (uint32_t)s;
        st->addressu = st->addressv = 1;
        st->magfilter = 1;
        st->minfilter = 1;
        st->mipfilter = 1;
    }
    g_rs.vp_w = w;
    g_rs.vp_h = h;
    g_rs.vp_maxz = 1.0f;
    g_rs.mat_diffuse[0] = g_rs.mat_diffuse[1] = g_rs.mat_diffuse[2] = g_rs.mat_diffuse[3] = 1.0f;
    c_viewport(0, 0, fb_w, fb_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void render_begin_frame(void)
{
    update_letterbox();
}

/* IGI_SHOT=N: save every Nth presented frame as shot_<frame>.bmp (debugging aid). */
static void maybe_screenshot(void)
{
    static int every = -1;
    static unsigned frame;
    if (every < 0) {
        const char *e = getenv("IGI_SHOT");
        every = e ? atoi(e) : 0;
    }
    frame++;
    int forced = g_trace_next_frame == 2;
    if (!forced && (every <= 0 || frame % (unsigned)every))
        return;
    uint8_t *px = malloc((size_t)fb_w * (size_t)fb_h * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, fb_w, fb_h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    char name[64];
    snprintf(name, sizeof(name), "shot_%05u.bmp", frame);
    FILE *f = fopen(name, "wb");
    if (f) {
        uint32_t size = 54u + (uint32_t)fb_w * (uint32_t)fb_h * 4u;
        uint8_t h[54] = { 'B', 'M' };
        memcpy(h + 2, &size, 4);
        h[10] = 54;
        h[14] = 40;
        int32_t w = fb_w, hh = fb_h;
        memcpy(h + 18, &w, 4);
        memcpy(h + 22, &hh, 4);
        h[26] = 1;
        h[28] = 32;
        fwrite(h, 1, 54, f);
        fwrite(px, 4, (size_t)fb_w * (size_t)fb_h, f);
        fclose(f);
    }
    free(px);
}

void render_present(void)
{
    render_flush();
    /* IGI_TRACE_EVERY=N: trace (log every draw + save the image of) every Nth frame. */
    static int trace_every = -1;
    static unsigned present_count;
    if (trace_every < 0) {
        const char *e = getenv("IGI_TRACE_EVERY");
        trace_every = e ? atoi(e) : 0;
    }
    if (trace_every > 0 && ++present_count % (unsigned)trace_every == 0 && !g_trace_next_frame)
        g_trace_next_frame = 1;
    platform_scripted_input();   /* cutscenes present frames without pumping messages */
    igi_end_frame();
    draw_bars();
    maybe_screenshot();   /* before the trace state advances: saves the traced frame */
    if (g_trace_next_frame == 2) {
        g_trace_next_frame = 0;
        rt_log("---- frame trace end ----");
    } else if (g_trace_next_frame == 1) {
        g_trace_next_frame = 2;
        rt_log("---- frame trace begin ----");
    }
    perf_frame_begin_swap();
    perf_swap();
    platform_pump();
    render_begin_frame();
}

/* Game pixel rectangle -> GL window rectangle (y up). */
static void to_fb(int32_t x, int32_t y, int32_t w, int32_t h, GLint *ox, GLint *oy, GLsizei *ow, GLsizei *oh)
{
    int32_t x0 = (int32_t)((int64_t)x * vp_w / rt_w);
    int32_t x1 = (int32_t)((int64_t)(x + w) * vp_w / rt_w);
    int32_t y0 = (int32_t)((int64_t)y * vp_h / rt_h);
    int32_t y1 = (int32_t)((int64_t)(y + h) * vp_h / rt_h);
    *ox = vp_x + x0;
    *oy = vp_y + vp_h - y1;
    *ow = x1 - x0;
    *oh = y1 - y0;
}

/* Orthographic projection over the whole render target in D3D pixel coordinates. */
static int use_z_remap(void)
{
    return z_remap != (diag_mode == DIAG_FLIP_ZREMAP);
}

/* Depth range for clip z in D3D form ([0,w]): GL maps NDC [-1,1] to [n,f], so
 * NDC [0,1] lands on [minz,maxz] with n = 2 minz - maxz. */
static void d3d_depth_range(float minz, float maxz)
{
    c_depthrange(2.0f * minz - maxz, maxz);
}

static void load_screen_ortho(void)
{
    c_viewport(vp_x, vp_y, vp_w, vp_h);
    /* D3D samples pixel centers at integer coordinates, GL at +0.5. */
    float m[16];
    ortho_matrix(-0.5, rt_w - 0.5, rt_h - 0.5, -0.5, 0.0, -1.0, m);
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

void render_clear(uint32_t nrects, const Rect *rects, uint32_t flags, uint32_t color, float z)
{
    render_flush();
    GLbitfield mask = 0;
    if (flags & 1u) {
        mask |= GL_COLOR_BUFFER_BIT;
        glClearColor(((color >> 16) & 0xFFu) / 255.0f, ((color >> 8) & 0xFFu) / 255.0f, (color & 0xFFu) / 255.0f,
                     (color >> 24) / 255.0f);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (flags & 2u) {
        mask |= GL_DEPTH_BUFFER_BIT;
        glClearDepth(z);
        glDepthMask(GL_TRUE);
    }
    if (!mask)
        return;
    /* vitaGL emulates the scissor by drawing a full-screen quad that obeys the
     * current depth state, so scissoring must never happen with depth writes on
     * unless depth is being cleared anyway, and is skipped for full clears. */
    int restore_mask = !(flags & 2u);
    uint32_t n = nrects ? nrects : 1u;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l, t, rr, b;
        if (nrects) {
            l = rects[i].l; t = rects[i].t; rr = rects[i].r; b = rects[i].b;
        } else {
            l = g_rs.vp_x; t = g_rs.vp_y; rr = g_rs.vp_x + g_rs.vp_w; b = g_rs.vp_y + g_rs.vp_h;
        }
        int full = l <= 0 && t <= 0 && rr >= rt_w && b >= rt_h;
        if (full && vp_w == fb_w && vp_h == fb_h) {
            glDisable(GL_SCISSOR_TEST);
            glClear(mask);
            continue;
        }
        GLint x, y;
        GLsizei w, h;
        to_fb(l, t, rr - l, b - t, &x, &y, &w, &h);
        if (restore_mask)
            glDepthMask(GL_FALSE);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x, y, w, h);
        glClear(mask);
        glDisable(GL_SCISSOR_TEST);
    }
    /* Depth mask is re-applied from the render state on the next draw. */
}

void render_fill(const Rect *r, uint32_t rgba)
{
    uint32_t argb = (rgba & 0xFF00FF00u) | ((rgba & 0xFFu) << 16) | ((rgba >> 16) & 0xFFu);
    render_clear(1, r, 1u, argb, 1.0f);
}

/* ---- textures ----------------------------------------------------------- */

/* 16-bit formats convert through a 65536-entry table built once per format:
 * no per-pixel math (the Vita's CPU has no hardware divide). */
typedef struct Lut16 {
    PixFmt pf;
    uint32_t *rgba;
} Lut16;

static const uint32_t *lut16(const PixFmt *pf)
{
    static Lut16 luts[6];
    for (int i = 0; i < 6; i++)
        if (luts[i].rgba && !memcmp(&luts[i].pf, pf, sizeof(*pf)))
            return luts[i].rgba;
    int slot = 0;
    while (slot < 5 && luts[slot].rgba)
        slot++;
    free(luts[slot].rgba);
    luts[slot].pf = *pf;
    luts[slot].rgba = malloc(65536u * 4u);
    for (uint32_t v = 0; v < 65536u; v++) {
        uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
        luts[slot].rgba[v] = pixel_to_rgba(pf, b);
    }
    return luts[slot].rgba;
}

/* Convert a w x h rectangle of a surface's guest pixels to tightly packed RGBA8;
 * color-keyed texels get alpha 0. The rectangle must lie inside the surface. */
static uint32_t *surface_rgba_rect(Surface *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
    surface_alloc_mem(s);
    uint32_t *out = conv((size_t)w * h);
    uint32_t bpp = s->pf.bits / 8u;
    const uint32_t *lut = bpp == 2u ? lut16(&s->pf) : NULL;
    int plain32 = bpp == 4u && s->pf.r == 0x00FF0000u && s->pf.g == 0x0000FF00u && s->pf.b == 0x000000FFu;
    uint32_t alpha32 = (s->pf.flags & DDPF_ALPHAPIXELS) ? 0u : 0xFF000000u;
    /* Locals: stores through `out` could otherwise alias the surface fields. */
    const int has_ck = s->has_ck;
    const uint32_t ck_lo = s->ck_lo, ck_hi = s->ck_hi;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = g_mem + s->mem + (y0 + y) * s->pitch + x0 * bpp;
        uint32_t *o = out + (size_t)y * w;
        if (lut) {
            if (has_ck) {
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t raw = (uint32_t)row[x * 2u] | ((uint32_t)row[x * 2u + 1u] << 8);
                    uint32_t v = lut[raw];
                    if (raw >= ck_lo && raw <= ck_hi)
                        v &= 0x00FFFFFFu;
                    o[x] = v;
                }
            } else {
                for (uint32_t x = 0; x < w; x++)
                    o[x] = lut[(uint32_t)row[x * 2u] | ((uint32_t)row[x * 2u + 1u] << 8)];
            }
        } else if (plain32) {
            for (uint32_t x = 0; x < w; x++) {
                uint32_t raw;
                memcpy(&raw, row + x * 4u, 4);
                uint32_t v = ((raw >> 16) & 0xFFu) | (raw & 0xFF00FF00u) | ((raw & 0xFFu) << 16) | alpha32;
                if (has_ck && (raw & 0x00FFFFFFu) >= ck_lo && (raw & 0x00FFFFFFu) <= ck_hi)
                    v &= 0x00FFFFFFu;
                o[x] = v;
            }
        } else {
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = row + x * bpp;
                uint32_t v = pixel_to_rgba(&s->pf, p);
                if (has_ck) {
                    uint32_t raw = 0;
                    memcpy(&raw, p, bpp);
                    if (raw >= ck_lo && raw <= ck_hi)
                        v &= 0x00FFFFFFu;
                }
                o[x] = v;
            }
        }
    }
    return out;
}

static uint32_t *surface_rgba(Surface *s)
{
    return surface_rgba_rect(s, 0, 0, s->w, s->h);
}

/* Renderer scratch slots in Surface.priv (priv[0] = highest defined mip level). */
enum {
    TEXP_SAMPLER = 1,   /* signature of the sampler state last set on gl_tex (0 = unknown) */
    TEXP_HASH_LO = 2,   /* content hash of the pixels last uploaded from this level */
    TEXP_HASH_HI = 3,
    TEXP_HASH_OK = 4,   /* 1 if TEXP_HASH_* is valid */
};

/* vitaGL ignores the pixels of glTexImage2D levels > 0 and derives the whole
 * mip chain from level 0 on the GPU, so only level 0 is converted there. */
#if defined(__vita__)
#define UPLOAD_MIP_PIXELS 0
#else
#define UPLOAD_MIP_PIXELS 1
#endif

static Surface *next_mip(const Surface *m)
{
    return m->attached && m->attached->parent == m ? m->attached : NULL;
}

/* 64-bit hash of a surface's pixel memory, format and color key. Two independent
 * multiply chains: a single changed word always changes the result. */
static uint64_t surface_hash(Surface *s)
{
    surface_alloc_mem(s);
    const uint8_t *p = g_mem + s->mem;
    size_t n = (size_t)s->pitch * s->h;
    uint32_t a = 0x811C9DC5u ^ s->w ^ (s->pf.bits << 16) ^ s->pf.r ^ s->pf.flags;
    uint32_t b = 0x2545F491u ^ s->h ^ s->pf.g ^ (s->pf.b << 3) ^ s->pf.a;
    a = (a ^ (uint32_t)s->has_ck) * 0x9E3779B1u;
    b = (b ^ (s->has_ck ? s->ck_lo : 0u)) * 0x85EBCA77u;
    a = (a ^ (s->has_ck ? s->ck_hi : 0u)) * 0x9E3779B1u;
    size_t i = 0;
    for (; i + 8u <= n; i += 8u) {
        uint32_t w0, w1;
        memcpy(&w0, p + i, 4);
        memcpy(&w1, p + i + 4u, 4);
        a = (a ^ w0) * 0x9E3779B1u;
        b = (b ^ w1) * 0x85EBCA77u;
    }
    for (; i < n; i++)
        a = (a ^ p[i]) * 0x9E3779B1u;
    a ^= a >> 16;
    b ^= b >> 13;
    return ((uint64_t)(a ^ (b * 0xC2B2AE3Du)) << 32) | (uint64_t)(b ^ (a * 0x27D4EB2Fu));
}

static int hash_matches(const Surface *m, uint64_t h)
{
    return m->priv[TEXP_HASH_OK] && m->priv[TEXP_HASH_LO] == (uint32_t)h && m->priv[TEXP_HASH_HI] == (uint32_t)(h >> 32);
}

static void hash_store(Surface *m, uint64_t h)
{
    m->priv[TEXP_HASH_LO] = (uint32_t)h;
    m->priv[TEXP_HASH_HI] = (uint32_t)(h >> 32);
    m->priv[TEXP_HASH_OK] = 1;
}

void render_texture_release(Surface *s)
{
    if (s->gl_tex) {
        GLuint t = s->gl_tex;
        glDeleteTextures(1, &t);
        s->gl_tex = 0;
    }
    s->priv[TEXP_SAMPLER] = 0;
    for (Surface *m = s; m; m = next_mip(m))
        m->priv[TEXP_HASH_OK] = 0;
}

#define MAX_MIPS 16

uint32_t render_texture_get(Surface *s)
{
    if (!s)
        return white_tex;
    int levels_dirty = 0;
    for (Surface *m = s; m; m = next_mip(m))
        levels_dirty |= m->dirty;
    if (s->gl_tex && !levels_dirty)
        return s->gl_tex;
    uint64_t t0 = win32_perf_counter();

    /* Lock/Unlock marks a texture dirty even when the game only read it or wrote
     * the same pixels: compare content hashes before converting and uploading. */
    Surface *lv[MAX_MIPS];
    uint64_t hv[MAX_MIPS];
    int n = 0, changed = !s->gl_tex;
    for (Surface *m = s; m && n < MAX_MIPS; m = next_mip(m), n++) {
        lv[n] = m;
        hv[n] = 0;
        if (n == 0 || UPLOAD_MIP_PIXELS) {
            hv[n] = surface_hash(m);
            if (!hash_matches(m, hv[n]))
                changed = 1;
        }
    }
#if defined(__vita__)
    /* vitaGL rebuilds mip levels with a GPU transfer that came out empty/zero-alpha
     * (distant walls vanished under the color-key alpha test): level 0 only. */
    for (int i = 1; i < n; i++)
        lv[i]->dirty = 0;
    n = 1;
#endif
    if (s->priv[0] != (uint32_t)(n - 1))
        changed = 1;
    if (!changed) {
        for (int i = 0; i < n; i++)
            lv[i]->dirty = 0;
        pc_frame[PC_TEX_SKIP]++;
        pc_frame[PC_TEX_US] += win32_perf_counter() - t0;
        return s->gl_tex;
    }

    if (!s->gl_tex) {
        GLuint t;
        glGenTextures(1, &t);
        s->gl_tex = t;
    }
    glBindTexture(GL_TEXTURE_2D, s->gl_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (int level = 0; level < n; level++) {
        Surface *m = lv[level];
        if (level == 0 || UPLOAD_MIP_PIXELS) {
            uint32_t *px = surface_rgba(m);
            glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)m->w, (GLsizei)m->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            hash_store(m, hv[level]);
            pc_frame[PC_TEX_BYTES] += (uint64_t)m->w * m->h * 4u;
        }
        m->dirty = 0;
    }
#if !UPLOAD_MIP_PIXELS
    /* One call builds the whole chain (vitaGL regenerates every level below the
     * requested one, so level-by-level calls would redo the work n times). */
    if (n > 1)
        glTexImage2D(GL_TEXTURE_2D, n - 1, GL_RGBA, (GLsizei)lv[n - 1]->w, (GLsizei)lv[n - 1]->h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);
#endif
    s->priv[0] = (uint32_t)(n - 1);   /* highest defined mip level */
    s->priv[TEXP_SAMPLER] = 0;        /* re-send the sampler state after an upload */
    pc_frame[PC_TEX_N]++;
    pc_frame[PC_TEX_US] += win32_perf_counter() - t0;
    return s->gl_tex;
}

/* ---- 2D ----------------------------------------------------------------- */

static void draw_rgba_quad(const uint32_t *px, int w, int h, const Rect *sr, const Rect *dr, int blend)
{
    glBindTexture(GL_TEXTURE_2D, blit_tex);
#if defined(__vita__)
    /* vitaGL's glTexSubImage2D writes in place while earlier draws of this frame are
     * still queued on the GPU; a full glTexImage2D gets fresh storage. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    blit_tex_w = w;
    blit_tex_h = h;
#else
    if (w != blit_tex_w || h != blit_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        blit_tex_w = w;
        blit_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
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
    if (blend) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    glDisable(GL_BLEND);
    load_screen_ortho();
    float m[16];
    ortho_matrix(0, rt_w, rt_h, 0, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
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

void render_blit(Surface *src, const Rect *sr, const Rect *dr, int use_ck)
{
    render_flush();
    uint64_t t0 = win32_perf_counter();
    pc_frame[PC_2D_N]++;
    VideoFrameSource *vs = src->client_mem ? video_source_for(src->mem) : NULL;
    if (vs) {
        /* Movie frame: convert it once, upload at native size, let the GPU scale. */
        const uint32_t *px = vs->rgba(vs);
        Rect vr = { (int32_t)((int64_t)sr->l * vs->width / (int32_t)src->w),
                    (int32_t)((int64_t)sr->t * vs->height / (int32_t)src->h),
                    (int32_t)((int64_t)sr->r * vs->width / (int32_t)src->w),
                    (int32_t)((int64_t)sr->b * vs->height / (int32_t)src->h) };
        draw_rgba_quad(px, vs->width, vs->height, &vr, dr, 0);
        pc_frame[PC_2D_US] += win32_perf_counter() - t0;
        return;
    }
    int saved_ck = src->has_ck;
    if (!use_ck)
        src->has_ck = 0;
    if (sr->l >= 0 && sr->t >= 0 && sr->r > sr->l && sr->b > sr->t && sr->r <= (int32_t)src->w &&
        sr->b <= (int32_t)src->h) {
        /* Convert and upload only the source rectangle (HUD pieces of large surfaces). */
        uint32_t w = (uint32_t)(sr->r - sr->l), h = (uint32_t)(sr->b - sr->t);
        uint32_t *px = surface_rgba_rect(src, (uint32_t)sr->l, (uint32_t)sr->t, w, h);
        src->has_ck = saved_ck;
        Rect local = { 0, 0, (int32_t)w, (int32_t)h };
        draw_rgba_quad(px, (int)w, (int)h, &local, dr, use_ck && saved_ck);
        pc_frame[PC_2D_BYTES] += (uint64_t)w * h * 4u;
    } else {
        uint32_t *px = surface_rgba(src);
        src->has_ck = saved_ck;
        draw_rgba_quad(px, (int)src->w, (int)src->h, sr, dr, use_ck && saved_ck);
        pc_frame[PC_2D_BYTES] += (uint64_t)src->w * src->h * 4u;
    }
    pc_frame[PC_2D_US] += win32_perf_counter() - t0;
}

/* rgba_to_pixel() through per-channel tables: no per-pixel divisions (the
 * Vita's CPU has no hardware divide). Same results. */
typedef struct PixelPacker {
    uint32_t r[256], g[256], b[256], a[256];
} PixelPacker;

static void packer_init(PixelPacker *pk, const PixFmt *pf)
{
    for (uint32_t c = 0; c < 256u; c++) {
        pk->r[c] = rgba_to_pixel(pf, c) & pf->r;
        pk->g[c] = rgba_to_pixel(pf, c << 8) & pf->g;
        pk->b[c] = rgba_to_pixel(pf, c << 16) & pf->b;
        pk->a[c] = (pf->flags & DDPF_ALPHAPIXELS) ? rgba_to_pixel(pf, c << 24) & pf->a : 0u;
    }
}

void render_readback(Surface *s)
{
    render_flush();
    uint64_t t0 = win32_perf_counter();
    pc_frame[PC_READBACK_N]++;
    surface_alloc_mem(s);
    uint32_t *px = malloc((size_t)vp_w * (size_t)vp_h * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(vp_x, vp_y, vp_w, vp_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t bpp = s->pf.bits / 8u;
    static PixelPacker pk;
    static PixFmt pk_fmt;
    static int pk_valid;
    if (!pk_valid || memcmp(&pk_fmt, &s->pf, sizeof(pk_fmt)) != 0) {
        packer_init(&pk, &s->pf);
        pk_fmt = s->pf;
        pk_valid = 1;
    }
    uint32_t *fx = malloc((size_t)s->w * sizeof(uint32_t));
    for (uint32_t x = 0; x < s->w; x++)
        fx[x] = (uint32_t)((uint64_t)x * (uint32_t)vp_w / s->w);
    for (uint32_t y = 0; y < s->h; y++) {
        uint32_t fy = (uint32_t)vp_h - 1u - (uint32_t)((uint64_t)y * (uint32_t)vp_h / s->h);
        const uint32_t *src = px + (size_t)fy * (uint32_t)vp_w;
        uint8_t *dst = g_mem + s->mem + y * s->pitch;
        for (uint32_t x = 0; x < s->w; x++) {
            uint32_t c = src[fx[x]];
            uint32_t v = pk.r[c & 0xFFu] | pk.g[(c >> 8) & 0xFFu] | pk.b[(c >> 16) & 0xFFu] | pk.a[c >> 24];
            memcpy(dst + x * bpp, &v, bpp);
        }
    }
    free(fx);
    free(px);
    pc_frame[PC_2D_US] += win32_perf_counter() - t0;
}

void render_writeback(Surface *s)
{
    render_flush();
    uint64_t t0 = win32_perf_counter();
    pc_frame[PC_2D_N]++;
    int ck = s->has_ck;
    s->has_ck = 0;
    uint32_t *px = surface_rgba(s);
    s->has_ck = ck;
    Rect r = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    Rect d = { 0, 0, rt_w, rt_h };
    draw_rgba_quad(px, (int)s->w, (int)s->h, &r, &d, 0);
    pc_frame[PC_2D_BYTES] += (uint64_t)s->w * s->h * 4u;
    pc_frame[PC_2D_US] += win32_perf_counter() - t0;
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
    const GLfloat scv[4] = { sc, sc, sc, sc };
    glTexEnvfv(GL_TEXTURE_ENV, scale, scv);
}

static GLint wrap_mode(uint32_t a)
{
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: case 4: return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

/* Returns the number of active texture stages. */
static int apply_stages(int ntex)
{
    int active = 0;
    float tf[4];
    color4(g_rs.rs[60], tf);
    for (int i = 0; i < MAX_STAGES; i++) {
        StageState *st = &g_rs.stage[i];
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)i);
        /* A stage without a texture passes the previous result through, as in D3D. */
        if (st->colorop == 1u || (i > 0 && active < i) || !st->texture) {
            glDisable(GL_TEXTURE_2D);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            continue;
        }
        active++;
        glEnable(GL_TEXTURE_2D);
        GLuint tex = render_texture_get(st->texture);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (st->texture) {
            Surface *t = st->texture;
            int mip = st->mipfilter > 1u && t->priv[0] > 1u;
            GLint minf = st->minfilter >= 2u ? (mip ? (st->mipfilter == 3u ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : GL_LINEAR)
                                             : (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
            GLint magf = st->magfilter >= 2u ? GL_LINEAR : GL_NEAREST;
            GLint ws = wrap_mode(st->addressu), wt = wrap_mode(st->addressv);
            /* Sampler state lives in the texture object: only changes are sent. */
            uint32_t sig = 1u | (uint32_t)(st->minfilter >= 2u) << 1 | (uint32_t)mip << 2 |
                           (uint32_t)(st->mipfilter == 3u) << 3 | (uint32_t)(magf == GL_LINEAR) << 4 |
                           (uint32_t)(ws == GL_REPEAT ? 0 : ws == GL_MIRRORED_REPEAT ? 1 : 2) << 5 |
                           (uint32_t)(wt == GL_REPEAT ? 0 : wt == GL_MIRRORED_REPEAT ? 1 : 2) << 7 |
                           (t->priv[0] & 0xFFu) << 9;
            if (t->priv[TEXP_SAMPLER] != sig) {
                t->priv[TEXP_SAMPLER] = sig;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
#if !defined(__vita__)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)t->priv[0] - 1);
#endif
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, ws);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wt);
            }
        }
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, tf);
        setup_combiner(0, st->colorop, st->colorarg1, st->colorarg2);
        setup_combiner(1, st->alphaop == 1u ? 3u : st->alphaop, st->alphaarg1,
                       st->alphaop == 1u ? 1u : st->alphaarg2);
        (void)ntex;
    }
    glActiveTexture(GL_TEXTURE0);
    glClientActiveTexture(GL_TEXTURE0);
    return active;
}

/* Everything the lighting setup below sends to GL. vitaGL re-uploads its
 * uniforms (and transforms light positions) on every glLight/glMaterial call,
 * so the whole block is skipped while the inputs are unchanged. */
typedef struct LightSnapshot {
    uint32_t ambient, color_material, normalize;
    float mat[17];
    float view[16];
    Light lights[MAX_LIGHTS];
} LightSnapshot;
static LightSnapshot light_snap;
static int light_snap_valid;

static void apply_lights(void)
{
    const uint32_t *rs = g_rs.rs;
    static LightSnapshot cur;
    memset(&cur, 0, sizeof(cur));   /* also clears padding for memcmp */
    cur.ambient = rs[139];
    cur.color_material = rs[141] && rs[145] == 1u;
    cur.normalize = rs[143] != 0;
    memcpy(cur.mat, g_rs.mat_diffuse, 16);
    memcpy(cur.mat + 4, g_rs.mat_ambient, 16);
    memcpy(cur.mat + 8, g_rs.mat_specular, 16);
    memcpy(cur.mat + 12, g_rs.mat_emissive, 16);
    cur.mat[16] = g_rs.mat_power;
    memcpy(cur.view, g_rs.view, sizeof(cur.view));
    for (int i = 0; i < MAX_LIGHTS; i++)
        if (g_rs.lights[i].enabled)
            cur.lights[i] = g_rs.lights[i];
    if (light_snap_valid && !memcmp(&cur, &light_snap, sizeof(cur)))
        return;
    light_snap = cur;
    light_snap_valid = 1;

    float amb[4];
    color4(rs[139], amb);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, g_rs.mat_diffuse);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, g_rs.mat_ambient);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, g_rs.mat_specular);
    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, g_rs.mat_emissive);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, g_rs.mat_power > 128.0f ? 128.0f : g_rs.mat_power);
    if (cur.color_material) {
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    } else {
        glDisable(GL_COLOR_MATERIAL);
    }
    if (cur.normalize)
        glEnable(GL_NORMALIZE);
    else
        glDisable(GL_NORMALIZE);
    /* Light positions are transformed by the modelview matrix current at glLight time. */
    c_load_matrix(GL_MODELVIEW, g_rs.view);
    for (int i = 0; i < MAX_LIGHTS; i++) {
        Light *l = &g_rs.lights[i];
        GLenum gl = GL_LIGHT0 + (GLenum)i;
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
#if !defined(__vita__)
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

static void apply_states(int pretransformed, int has_normal)
{
    uint32_t *rs = g_rs.rs;
    if (diag_mode == DIAG_DEPTH_LEQUAL && !pretransformed) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else if (rs[7]) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(gl_cmp(rs[23]));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    if (rs[27]) {
        glEnable(GL_BLEND);
        uint32_t sb = rs[19], db = rs[20];
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
    if (rs[15]) {
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(gl_cmp(rs[25]), (rs[24] & 0xFFu) / 255.0f);
    } else if (rs[41]) {
        /* Color-keyed texels were uploaded with alpha 0. */
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GREATER, 0.0f);
    } else {
        glDisable(GL_ALPHA_TEST);
    }
    if (rs[22] == 1u || pretransformed || cull_override == 1 || diag_mode == DIAG_NO_CULL) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace((rs[22] == 3u) != (cull_override == 2) ? GL_CW : GL_CCW);
    }
    glShadeModel(rs[9] == 1u ? GL_FLAT : GL_SMOOTH);
    glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 2u ? GL_LINE : rs[8] == 1u ? GL_POINT : GL_FILL);

    if (rs[28]) {
        glEnable(GL_FOG);
        float fc[4];
        color4(rs[34], fc);
        glFogfv(GL_FOG_COLOR, fc);
        uint32_t mode = rs[35] ? rs[35] : rs[140];
        float start, end, dens;
        memcpy(&start, &rs[36], 4);
        memcpy(&end, &rs[37], 4);
        memcpy(&dens, &rs[38], 4);
        if (pretransformed || !mode) {
#if defined(__vita__)
            /* No fog coordinates in vitaGL: game-computed vertex fog is not drawn yet. */
            glDisable(GL_FOG);
#else
            /* Vertex fog computed by the game: the factor is in the specular alpha. */
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 1.0f);
            glFogf(GL_FOG_END, 0.0f);
#endif
        } else {
#if !defined(__vita__)
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

    if (!pretransformed && rs[137] && has_normal) {
        glEnable(GL_LIGHTING);
        apply_lights();
    } else {
        glDisable(GL_LIGHTING);
    }
}

static void mat_mul(const float *a, const float *b, float *out)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}

static void apply_transforms(int pretransformed)
{
    if (pretransformed) {
        load_screen_ortho();
        return;
    }
    GLint x, y;
    GLsizei w, h;
    to_fb(g_rs.vp_x, g_rs.vp_y, g_rs.vp_w, g_rs.vp_h, &x, &y, &w, &h);
    c_viewport(x, y, w, h);
    /* D3D clip-space z is [0, w]; GL wants [-w, w]: z' = 2z - w. */
    float p[16];
    memcpy(p, g_rs.proj, sizeof(p));
    int remap = use_z_remap();
    if (remap)
        c_depthrange(g_rs.vp_minz, g_rs.vp_maxz);
    else
        d3d_depth_range(g_rs.vp_minz, g_rs.vp_maxz);
    if (remap)
        for (int r = 0; r < 4; r++)
            p[r * 4 + 2] = 2.0f * g_rs.proj[r * 4 + 2] - g_rs.proj[r * 4 + 3];
    c_load_matrix(GL_PROJECTION, p);
    float wv[16];
    mat_mul(g_rs.world, g_rs.view, wv);
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

/* ---- vertex packing -------------------------------------------------------
 * Vertices are copied straight into tight per-attribute arrays holding only
 * what the draw uses (vitaGL copies count * stride bytes of every enabled
 * client array on each draw). Batched draws are packed when they are queued,
 * so every vertex is read once and never copied as a whole RVertex.
 */
typedef struct Layout {
    int pretransformed, has_normal, has_color, has_spec, ntex;   /* draw key */
    int pos_size;               /* 4: pre-transformed (x/w, y/w, z/w, 1/w); 3: object space */
    int use_color;
    int use_fog;                /* desktop only: game vertex fog taken from the specular alpha */
    int stages;                 /* texture stages apply_stages() will enable */
    int uv_src[MAX_STAGES];     /* RVertex uv set feeding each stage, -1 = none */
} Layout;

static float *pos_buf;
static float *fog_buf;
static uint32_t *col_buf;
static float *nrm_buf;
static float *uv_buf[MAX_STAGES];
static size_t pos_cap;

static void ensure_capacity(size_t n)
{
    if (n <= pos_cap)
        return;
    pos_cap = n + 1024u;
    pos_buf = realloc(pos_buf, pos_cap * 16u);
    fog_buf = realloc(fog_buf, pos_cap * 4u);
    col_buf = realloc(col_buf, pos_cap * 4u);
    nrm_buf = realloc(nrm_buf, pos_cap * 12u);
    for (int s = 0; s < MAX_STAGES; s++)
        uv_buf[s] = realloc(uv_buf[s], pos_cap * 8u);
}

/* Number of stages apply_stages() enables for the current state (same rule). */
static int active_stages(void)
{
    int active = 0;
    for (int i = 0; i < MAX_STAGES; i++) {
        const StageState *st = &g_rs.stage[i];
        if (st->colorop == 1u || (i > 0 && active < i) || !st->texture)
            continue;
        active++;
    }
    return active;
}

static void layout_init(Layout *L, int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    memset(L, 0, sizeof(*L));
    L->pretransformed = pretransformed;
    L->has_normal = has_normal;
    L->has_color = has_color;
    L->has_spec = has_spec;
    L->ntex = ntex;
    L->pos_size = pretransformed ? 4 : 3;
    L->use_color = has_color || pretransformed;
#if !defined(__vita__)
    L->use_fog = g_rs.rs[28] && (pretransformed || (!g_rs.rs[35] && !g_rs.rs[140])) && has_spec;
#endif
    L->stages = active_stages();
    for (int s = 0; s < MAX_STAGES; s++) {
        L->uv_src[s] = -1;
        uint32_t ci = g_rs.stage[s].texcoordindex & 0xFFFFu;
        if (s < L->stages && (int)ci < ntex)
            L->uv_src[s] = ci < MAX_STAGES ? (int)ci : 0;
    }
}

/* Writes nv vertices at array position `base`. */
static void pack_vertices(const Layout *L, const RVertex *v, uint32_t nv, uint32_t base)
{
    uint64_t t0 = win32_perf_counter();
    ensure_capacity((size_t)base + nv);
    if (L->pretransformed) {
        float *p = pos_buf + (size_t)base * 4u;
        for (uint32_t i = 0; i < nv; i++, p += 4) {
            float w = v[i].w != 0.0f ? 1.0f / v[i].w : 1.0f;
            p[0] = v[i].x * w;
            p[1] = v[i].y * w;
            p[2] = v[i].z * w;
            p[3] = w;
        }
    } else {
        float *p = pos_buf + (size_t)base * 3u;
        for (uint32_t i = 0; i < nv; i++, p += 3) {
            p[0] = v[i].x;
            p[1] = v[i].y;
            p[2] = v[i].z;
        }
    }
    if (L->use_color) {
        uint32_t *c = col_buf + base;
        for (uint32_t i = 0; i < nv; i++)
            c[i] = v[i].color;
    }
    if (L->has_normal) {
        float *n = nrm_buf + (size_t)base * 3u;
        for (uint32_t i = 0; i < nv; i++, n += 3) {
            n[0] = v[i].nx;
            n[1] = v[i].ny;
            n[2] = v[i].nz;
        }
    }
    if (L->use_fog) {
        float *f = fog_buf + base;
        for (uint32_t i = 0; i < nv; i++)
            f[i] = (float)(v[i].spec >> 24) * (1.0f / 255.0f);
    }
    for (int s = 0; s < L->stages; s++) {
        int k = L->uv_src[s];
        if (k < 0)
            continue;
        float *uv = uv_buf[s] + (size_t)base * 2u;
        for (uint32_t i = 0; i < nv; i++, uv += 2) {
            uv[0] = v[i].uv[k][0];
            uv[1] = v[i].uv[k][1];
        }
    }
    pc_frame[PC_PACK_US] += win32_perf_counter() - t0;
}

/* Client array pointers: re-sent only when they change (GL reads them at draw time). */
enum { PTR_VERTEX, PTR_COLOR, PTR_NORMAL, PTR_FOG, PTR_TEX0, PTR_COUNT = PTR_TEX0 + MAX_STAGES };
static const void *ptr_cache[PTR_COUNT];
static int ptr_size_cache[PTR_COUNT];

static int ptr_changed(int slot, const void *p, int size)
{
    if (ptr_cache[slot] == p && ptr_size_cache[slot] == size)
        return 0;
    ptr_cache[slot] = p;
    ptr_size_cache[slot] = size;
    return 1;
}

/* Sets up the GL state for arrays packed with layout L and draws them. */
static void submit(int prim, const Layout *L, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    perf_gpu_draws++;
    uint64_t t0 = win32_perf_counter();
    uint64_t tex_before = pc_frame[PC_TEX_US];
    apply_states(L->pretransformed, L->has_normal);
    int stages = apply_stages(L->ntex);
    apply_transforms(L->pretransformed);

    glEnableClientState(GL_VERTEX_ARRAY);
    if (ptr_changed(PTR_VERTEX, pos_buf, L->pos_size))
        glVertexPointer(L->pos_size, GL_FLOAT, 0, pos_buf);
    if (L->use_color) {
        glEnableClientState(GL_COLOR_ARRAY);
        if (ptr_changed(PTR_COLOR, col_buf, 4))
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, col_buf);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
    }
    if (L->has_normal) {
        glEnableClientState(GL_NORMAL_ARRAY);
        if (ptr_changed(PTR_NORMAL, nrm_buf, 3))
            glNormalPointer(GL_FLOAT, 0, nrm_buf);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
#if !defined(__vita__)
    if (L->use_fog) {
        glEnableClientState(GL_FOG_COORDINATE_ARRAY);
        if (ptr_changed(PTR_FOG, fog_buf, 1))
            glFogCoordPointer(GL_FLOAT, 0, fog_buf);
    } else {
        glDisableClientState(GL_FOG_COORDINATE_ARRAY);
        glFogCoordf(1.0f);
    }
#endif
    for (int s = 0; s < stages; s++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        if (s < L->stages && L->uv_src[s] >= 0) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            if (ptr_changed(PTR_TEX0 + s, uv_buf[s], 2))
                glTexCoordPointer(2, GL_FLOAT, 0, uv_buf[s]);
        } else {
#if defined(__vita__)
            /* vitaGL only counts a texture unit whose texcoord array is enabled. */
            memset(uv_buf[s], 0, nv * 8u);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, 0, uv_buf[s]);
#else
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glMultiTexCoord2f(GL_TEXTURE0 + (GLenum)s, 0, 0);
#endif
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
    uint64_t t1 = win32_perf_counter();
    pc_frame[PC_STATE_US] += (t1 - t0) - (pc_frame[PC_TEX_US] - tex_before);

    if (idx)
        glDrawElements(gl_prim(prim), (GLsizei)ni, GL_UNSIGNED_SHORT, idx);
    else
        glDrawArrays(gl_prim(prim), 0, (GLsizei)nv);
    if (L->use_color)
        color_cache_valid = 0;   /* GL leaves the current color undefined after a color array */
    pc_frame[PC_GL_US] += win32_perf_counter() - t1;
    pc_frame[PC_VERTS] += nv;
    pc_frame[PC_INDICES] += idx ? ni : 0u;
}

static void trace_draw(int prim, const RVertex *v, uint32_t nv, uint32_t ni, int pretransformed, int has_normal,
                       int has_color, int has_spec, int ntex)
{
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (uint32_t i = 0; i < nv; i++) {
        x0 = v[i].x < x0 ? v[i].x : x0;
        y0 = v[i].y < y0 ? v[i].y : y0;
        x1 = v[i].x > x1 ? v[i].x : x1;
        y1 = v[i].y > y1 ? v[i].y : y1;
    }
    const StageState *s0 = &g_rs.stage[0], *s1 = &g_rs.stage[1];
    rt_log("draw prim %d nv %u ni %u tl %d n %d c %d s %d t %d box %.1f,%.1f-%.1f,%.1f col0 %08X | z %u/%u "
           "blend %u %u/%u atest %u %u/%u ck %u fog %u light %u cull %u | s0 c%u(%X,%X) a%u(%X,%X) tex %ux%u/%ubpp ck%d "
           "| s1 c%u a%u tex %ux%u tf %08X | vp %d,%d %dx%d",
           prim, nv, ni, pretransformed, has_normal, has_color, has_spec, ntex, (double)x0, (double)y0, (double)x1,
           (double)y1, v[0].color, g_rs.rs[7], g_rs.rs[14], g_rs.rs[27], g_rs.rs[19], g_rs.rs[20], g_rs.rs[15],
           g_rs.rs[25], g_rs.rs[24], g_rs.rs[41], g_rs.rs[28], g_rs.rs[137], g_rs.rs[22], s0->colorop,
           s0->colorarg1, s0->colorarg2, s0->alphaop, s0->alphaarg1, s0->alphaarg2,
           s0->texture ? s0->texture->w : 0u, s0->texture ? s0->texture->h : 0u,
           s0->texture ? s0->texture->pf.bits : 0u, s0->texture ? s0->texture->has_ck : 0, s1->colorop,
           s1->alphaop, s1->texture ? s1->texture->w : 0u, s1->texture ? s1->texture->h : 0u, g_rs.rs[60],
           g_rs.vp_x, g_rs.vp_y, g_rs.vp_w, g_rs.vp_h);
    if (pretransformed && nv <= 8) {
        for (uint32_t i = 0; i < nv; i++)
            rt_log("    v%u: %.1f %.1f z %.4f rhw %.4f col %08X spec %08X uv %.3f %.3f", i, (double)v[i].x,
                   (double)v[i].y, (double)v[i].z, (double)v[i].w, v[i].color, v[i].spec, (double)v[i].uv[0][0],
                   (double)v[i].uv[0][1]);
    }
}

/* Unbatched draw of the game's vertices as given. */
static void draw_now(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                     int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    if (!nv)
        return;
    if (g_trace_next_frame == 2)
        trace_draw(prim, v, nv, ni, pretransformed, has_normal, has_color, has_spec, ntex);
    Layout L;
    layout_init(&L, pretransformed, has_normal, has_color, has_spec, ntex);
    pack_vertices(&L, v, nv, 0);
    submit(prim, &L, nv, idx, ni);
}


/* A (near) zero rhw makes w effectively infinite. Hardware of the era interpolates
 * colors perspective-correctly (attribute * rhw), so such primitives come out fully
 * transparent. IGI's cutscenes submit an overlay with rhw left as garbage/zero. */
static int rhw_invisible(int prim, const RVertex *v, uint32_t nv)
{
    float min_rhw = 1e30f, max_rhw = 0.0f;
    for (uint32_t i = 0; i < nv; i++) {
        min_rhw = v[i].w < min_rhw ? v[i].w : min_rhw;
        max_rhw = v[i].w > max_rhw ? v[i].w : max_rhw;
    }
    if (max_rhw >= 1e-6f)
        return 0;
    static int reported;
    if (reported < 8) {
        reported++;
        rt_log("dropping pre-transformed prim %d (%u verts): rhw %g..%g, color %08X", prim, nv, (double)min_rhw,
               (double)max_rhw, v[0].color);
    }
    return 1;
}

/* ---- draw batching --------------------------------------------------------
 * Consecutive triangle draws with identical render state are merged into one
 * indexed triangle list: on the Vita every draw costs a round of vitaGL/GXM
 * state setup. IGI_BATCH=0 disables it.
 */
#define BATCH_MAX_V 16384u
#define BATCH_MAX_I 49152u

static uint16_t batch_i[BATCH_MAX_I];
static uint32_t batch_nv, batch_ni;
static RenderState batch_rs;
static Layout batch_layout;
static int batch_active;
static int batch_enabled = -1;

/* ---- on-screen performance overlay ----------------------------------------
 * Per-frame averages over one second: total frame time, time spent submitting
 * draws to GL (D: render_draw plus batch flushes triggered elsewhere), time
 * blocked in the buffer swap, and the rest (game code), plus the counters in
 * pc_frame. Enabled by default on the Vita; IGI_OVERLAY=1 on desktop.
 */
static struct {
    uint64_t draw_us, swap_us, frame_start;
    uint32_t draws, frames;
    uint64_t acc_frame, acc_draw, acc_swap, window_start;
    uint32_t acc_draws, acc_gpu_draws;
    uint32_t gpu_draws_per_frame;
    float fps, ms_frame, ms_draw, ms_swap, ms_game;
    uint32_t draws_per_frame;
    uint64_t acc_pc[PC_COUNT];
    float avg_pc[PC_COUNT];   /* per-frame averages of pc_frame */
    int enabled;
} perf = { .enabled = -1 };
static int in_render_draw;

static void flush_batch(void)
{
    batch_active = 0;
    if (!batch_ni)
        return;
    static RenderState saved;
    saved = g_rs;
    g_rs = batch_rs;
    submit(PRIM_TRIANGLES, &batch_layout, batch_nv, batch_i, batch_ni);
    g_rs = saved;
    batch_nv = batch_ni = 0;
}

void render_flush(void)
{
    if (!batch_active)
        return;
    if (in_render_draw) {
        flush_batch();
        return;
    }
    /* Flushes from EndScene, Lock, Blt, Present...: still draw submission time. */
    uint64_t t0 = win32_perf_counter();
    flush_batch();
    perf.draw_us += win32_perf_counter() - t0;
}

static void batch_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                       int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    if (!nv)
        return;
    if (pretransformed && rhw_invisible(prim, v, nv))
        return;
    if ((diag_mode == DIAG_NO_SCREEN && pretransformed) || (diag_mode == DIAG_NO_3D && !pretransformed))
        return;
    if (batch_enabled < 0) {
        const char *e = getenv("IGI_BATCH");
        batch_enabled = !(e && e[0] == '0');
    }
    uint32_t n = idx ? ni : nv;
    int batchable = batch_enabled && diag_mode != DIAG_NO_BATCH &&
                    (prim == PRIM_TRIANGLES || prim == PRIM_TRIANGLE_STRIP || prim == PRIM_TRIANGLE_FAN);
    uint32_t tris = prim == PRIM_TRIANGLES ? n / 3u : (n >= 3u ? n - 2u : 0u);
    if (!batchable || nv > BATCH_MAX_V || tris * 3u > BATCH_MAX_I || g_trace_next_frame == 2) {
        render_flush();
        draw_now(prim, v, nv, idx, ni, pretransformed, has_normal, has_color, has_spec, ntex);
        return;
    }
    if (!tris)
        return;
    const Layout *bl = &batch_layout;
    if (batch_active &&
        (batch_nv + nv > BATCH_MAX_V || batch_ni + tris * 3u > BATCH_MAX_I || bl->pretransformed != pretransformed ||
         bl->has_normal != has_normal || bl->has_color != has_color || bl->has_spec != has_spec || bl->ntex != ntex ||
         memcmp(&g_rs, &batch_rs, sizeof(g_rs)) != 0))
        render_flush();
    if (!batch_active) {
        batch_active = 1;
        batch_rs = g_rs;
        layout_init(&batch_layout, pretransformed, has_normal, has_color, has_spec, ntex);
        batch_nv = batch_ni = 0;
    }
    /* Same state as the batch start, so the layout (active stages, uv sets) holds. */
    uint32_t base = batch_nv;
    pack_vertices(&batch_layout, v, nv, base);
    batch_nv += nv;
    uint16_t *out = &batch_i[batch_ni];
    for (uint32_t t = 0; t < tris; t++) {
        uint32_t a, b, c;
        if (prim == PRIM_TRIANGLES) {
            a = t * 3u;
            b = a + 1u;
            c = a + 2u;
        } else if (prim == PRIM_TRIANGLE_STRIP) {
            /* Alternate winding so every triangle keeps the strip's orientation. */
            a = (t & 1u) ? t + 1u : t;
            b = (t & 1u) ? t : t + 1u;
            c = t + 2u;
        } else {
            a = 0u;
            b = t + 1u;
            c = t + 2u;
        }
        if (idx) {
            a = idx[a];
            b = idx[b];
            c = idx[c];
        }
        *out++ = (uint16_t)(base + a);
        *out++ = (uint16_t)(base + b);
        *out++ = (uint16_t)(base + c);
    }
    batch_ni += tris * 3u;
}

void render_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                 int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    uint64_t t0 = win32_perf_counter();
    in_render_draw = 1;
    batch_draw(prim, v, nv, idx, ni, pretransformed, has_normal, has_color, has_spec, ntex);
    in_render_draw = 0;
    perf.draw_us += win32_perf_counter() - t0;
    perf.draws++;
}

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
                    glVertex2f(x0, y0);
                    glVertex2f(x0 + px, y0);
                    glVertex2f(x0 + px, y0 + px);
                    glVertex2f(x0, y0 + px);
                }
    }
}

/* Overlay lines (per-frame averages over the last second):
 *   F fps | T frame ms | G game ms | D draw submission ms | S swap ms
 *   N game draws  B GPU draws (after batching)
 *   V vertices  I indices sent to GL
 *   DS state setup ms  DT texture upload ms       (parts of D; DT also counts PreLoad)
 *   DP vertex packing ms  DG inside glDraw* ms  DO rest of D (batching, rhw scan)
 *   U textures uploaded  K unchanged dirty textures skipped  KB uploaded
 *   2D blits  ms  KB converted (inside G)  R render target read backs
 */
#define OVERLAY_LINES 12

static void perf_overlay_draw(void)
{
    const float *a = perf.avg_pc;
    float ds = a[PC_STATE_US] / 1000.0f, dt = a[PC_TEX_US] / 1000.0f, dp = a[PC_PACK_US] / 1000.0f;
    float dg = a[PC_GL_US] / 1000.0f;
    float dother = perf.ms_draw - ds - dt - dp - dg;
    char lines[OVERLAY_LINES][40];
    snprintf(lines[0], sizeof(lines[0]), "F %.1f", (double)perf.fps);
    snprintf(lines[1], sizeof(lines[1]), "T %.1f", (double)perf.ms_frame);
    snprintf(lines[2], sizeof(lines[2]), "G %.1f", (double)perf.ms_game);
    snprintf(lines[3], sizeof(lines[3]), "D %.1f", (double)perf.ms_draw);
    snprintf(lines[4], sizeof(lines[4]), "S %.1f", (double)perf.ms_swap);
    snprintf(lines[5], sizeof(lines[5]), "N %u B %u", perf.draws_per_frame, perf.gpu_draws_per_frame);
    snprintf(lines[6], sizeof(lines[6]), "V %u I %u", (unsigned)a[PC_VERTS], (unsigned)a[PC_INDICES]);
    snprintf(lines[7], sizeof(lines[7]), "DS %.1f DT %.1f", (double)ds, (double)dt);
    snprintf(lines[8], sizeof(lines[8]), "DP %.1f DG %.1f DO %.1f", (double)dp, (double)dg,
             (double)(dother > 0.0f ? dother : 0.0f));
    snprintf(lines[9], sizeof(lines[9]), "U %.1f K %.1f KB %u", (double)a[PC_TEX_N], (double)a[PC_TEX_SKIP],
             (unsigned)(a[PC_TEX_BYTES] / 1024.0f));
    snprintf(lines[10], sizeof(lines[10]), "2D %.1f %.1f KB %u R %.1f", (double)a[PC_2D_N],
             (double)(a[PC_2D_US] / 1000.0f), (unsigned)(a[PC_2D_BYTES] / 1024.0f), (double)a[PC_READBACK_N]);
    snprintf(lines[11], sizeof(lines[11]), "M %d %s", diag_mode, diag_names[diag_mode]);
    size_t width = 0;
    for (int i = 0; i < OVERLAY_LINES; i++) {
        size_t l = strlen(lines[i]);
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
    c_viewport(0, 0, fb_w, fb_h);
    float m[16];
    ortho_matrix(0, fb_w, fb_h, 0, -1, 1, m);
    c_load_matrix(GL_PROJECTION, m);
    c_load_matrix(GL_MODELVIEW, identity_matrix);
    float px = (float)fb_h / 181.0f;   /* ~3 px per font pixel at 544 lines */
    float bw = px * (float)(width * 4u + 2u), bh = px * (float)(OVERLAY_LINES * 6 + 1);
    glColor4f(0, 0, 0, 1);
    glBegin(GL_QUADS);
    glVertex2f(0, 0);
    glVertex2f(bw, 0);
    glVertex2f(bw, bh);
    glVertex2f(0, bh);
    glEnd();
    glColor4f(1, 1, 0, 1);
    glBegin(GL_QUADS);
    for (int i = 0; i < OVERLAY_LINES; i++)
        overlay_text(px, px * (1.0f + 6.0f * (float)i), px, lines[i]);
    glEnd();
    glColor4f(1, 1, 1, 1);
}

static void perf_frame_begin_swap(void)
{
    if (perf.enabled < 0) {
        const char *e = getenv("IGI_OVERLAY");
#if defined(__vita__)
        perf.enabled = !(e && e[0] == '0');
#else
        perf.enabled = e && e[0] == '1';
#endif
    }
    uint64_t now = win32_perf_counter();
    if (!perf.frame_start) {
        perf.frame_start = now;
        perf.window_start = now;
    }
    perf.acc_frame += now - perf.frame_start;
    perf.acc_draw += perf.draw_us;
    perf.acc_draws += perf.draws;
    perf.acc_gpu_draws += perf_gpu_draws;
    for (int i = 0; i < PC_COUNT; i++) {
        perf.acc_pc[i] += pc_frame[i];
        pc_frame[i] = 0;
    }
    perf_gpu_draws = 0;
    perf.frames++;
    perf.draw_us = 0;
    perf.draws = 0;
    perf.frame_start = now;
    if (now - perf.window_start >= 1000000u && perf.frames) {
        float n = (float)perf.frames;
        perf.fps = n * 1e6f / (float)(now - perf.window_start);
        perf.ms_frame = (float)perf.acc_frame / n / 1000.0f;
        perf.ms_draw = (float)perf.acc_draw / n / 1000.0f;
        perf.ms_swap = (float)perf.acc_swap / n / 1000.0f;
        perf.ms_game = perf.ms_frame - perf.ms_draw - perf.ms_swap;
        perf.draws_per_frame = perf.acc_draws / perf.frames;
        perf.gpu_draws_per_frame = perf.acc_gpu_draws / perf.frames;
        for (int i = 0; i < PC_COUNT; i++) {
            perf.avg_pc[i] = (float)perf.acc_pc[i] / n;
            perf.acc_pc[i] = 0;
        }
        const float *a = perf.avg_pc;
        rt_log("perf: %.1f fps, frame %.1f ms = game %.1f + draw %.1f + swap %.1f, %u draws -> %u gpu draws",
               (double)perf.fps, (double)perf.ms_frame, (double)perf.ms_game, (double)perf.ms_draw,
               (double)perf.ms_swap, perf.draws_per_frame, perf.gpu_draws_per_frame);
        rt_log("perf: draw = state %.2f + tex %.2f + pack %.2f + gl %.2f ms (+ rest); %.0f verts %.0f idx; "
               "tex %.1f uploads %.1f skipped %.0f KB; 2d %.1f blits %.2f ms %.0f KB; %.1f readbacks",
               (double)(a[PC_STATE_US] / 1000.0f), (double)(a[PC_TEX_US] / 1000.0f),
               (double)(a[PC_PACK_US] / 1000.0f), (double)(a[PC_GL_US] / 1000.0f), (double)a[PC_VERTS],
               (double)a[PC_INDICES], (double)a[PC_TEX_N], (double)a[PC_TEX_SKIP],
               (double)(a[PC_TEX_BYTES] / 1024.0f), (double)a[PC_2D_N], (double)(a[PC_2D_US] / 1000.0f),
               (double)(a[PC_2D_BYTES] / 1024.0f), (double)a[PC_READBACK_N]);
        perf.acc_frame = perf.acc_draw = perf.acc_swap = 0;
        perf.acc_draws = perf.acc_gpu_draws = 0;
        perf.frames = 0;
        perf.window_start = now;
    }
    if (perf.enabled)
        perf_overlay_draw();
}

static void perf_swap(void)
{
    uint64_t t0 = win32_perf_counter();
    platform_swap();
    perf.acc_swap += win32_perf_counter() - t0;
}
