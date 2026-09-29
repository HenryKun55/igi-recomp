/* Fixed-function OpenGL backend for the DirectDraw / Direct3D 7 shims. */
#include "platform.h"
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

static int rt_w = 640, rt_h = 480;       /* game resolution */
static int fb_w = 640, fb_h = 480;       /* drawable size in pixels */
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
    platform_get_drawable_size(&fb_w, &fb_h);
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
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void render_begin_frame(void)
{
    platform_get_drawable_size(&fb_w, &fb_h);
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
    maybe_screenshot();   /* before the trace state advances: saves the traced frame */
    if (g_trace_next_frame == 2) {
        g_trace_next_frame = 0;
        rt_log("---- frame trace end ----");
    } else if (g_trace_next_frame == 1) {
        g_trace_next_frame = 2;
        rt_log("---- frame trace begin ----");
    }
    platform_swap();
    platform_pump();
    render_begin_frame();
}

/* Game pixel rectangle -> GL window rectangle (y up). */
static void to_fb(int32_t x, int32_t y, int32_t w, int32_t h, GLint *ox, GLint *oy, GLsizei *ow, GLsizei *oh)
{
    int32_t x0 = (int32_t)((int64_t)x * fb_w / rt_w);
    int32_t x1 = (int32_t)((int64_t)(x + w) * fb_w / rt_w);
    int32_t y0 = (int32_t)((int64_t)y * fb_h / rt_h);
    int32_t y1 = (int32_t)((int64_t)(y + h) * fb_h / rt_h);
    *ox = x0;
    *oy = fb_h - y1;
    *ow = x1 - x0;
    *oh = y1 - y0;
}

/* Orthographic projection over the whole render target in D3D pixel coordinates. */
static void load_screen_ortho(void)
{
    glViewport(0, 0, fb_w, fb_h);
    glDepthRange(0.0, 1.0);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* D3D samples pixel centers at integer coordinates, GL at +0.5. */
    glOrtho(-0.5, rt_w - 0.5, rt_h - 0.5, -0.5, 0.0, -1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void render_clear(uint32_t nrects, const Rect *rects, uint32_t flags, uint32_t color, float z)
{
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
    glEnable(GL_SCISSOR_TEST);
    if (!nrects) {
        GLint x, y;
        GLsizei w, h;
        to_fb(g_rs.vp_x, g_rs.vp_y, g_rs.vp_w, g_rs.vp_h, &x, &y, &w, &h);
        glScissor(x, y, w, h);
        glClear(mask);
    }
    for (uint32_t i = 0; i < nrects; i++) {
        GLint x, y;
        GLsizei w, h;
        to_fb(rects[i].l, rects[i].t, rects[i].r - rects[i].l, rects[i].b - rects[i].t, &x, &y, &w, &h);
        glScissor(x, y, w, h);
        glClear(mask);
    }
    glDisable(GL_SCISSOR_TEST);
}

void render_fill(const Rect *r, uint32_t rgba)
{
    uint32_t argb = (rgba & 0xFF00FF00u) | ((rgba & 0xFFu) << 16) | ((rgba >> 16) & 0xFFu);
    render_clear(1, r, 1u, argb, 1.0f);
}

/* ---- textures ----------------------------------------------------------- */

/* Convert a surface's guest pixels to RGBA8; color-keyed texels get alpha 0. */
static uint32_t *surface_rgba(Surface *s)
{
    surface_alloc_mem(s);
    uint32_t *out = conv((size_t)s->w * s->h);
    uint32_t bpp = s->pf.bits / 8u;
    for (uint32_t y = 0; y < s->h; y++) {
        const uint8_t *row = g_mem + s->mem + y * s->pitch;
        for (uint32_t x = 0; x < s->w; x++) {
            const uint8_t *p = row + x * bpp;
            uint32_t v = pixel_to_rgba(&s->pf, p);
            if (s->has_ck) {
                uint32_t raw = 0;
                memcpy(&raw, p, bpp);
                if (raw >= s->ck_lo && raw <= s->ck_hi)
                    v &= 0x00FFFFFFu;
            }
            out[y * s->w + x] = v;
        }
    }
    return out;
}

void render_texture_release(Surface *s)
{
    if (s->gl_tex) {
        GLuint t = s->gl_tex;
        glDeleteTextures(1, &t);
        s->gl_tex = 0;
    }
}

uint32_t render_texture_get(Surface *s)
{
    if (!s)
        return white_tex;
    int levels_dirty = s->dirty;
    for (Surface *m = s->attached; m && m->parent; m = m->attached)
        levels_dirty |= m->dirty;
    if (s->gl_tex && !levels_dirty)
        return s->gl_tex;
    if (!s->gl_tex) {
        GLuint t;
        glGenTextures(1, &t);
        s->gl_tex = t;
    }
    glBindTexture(GL_TEXTURE_2D, s->gl_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    int level = 0;
    for (Surface *m = s; m; m = m->attached, level++) {
        uint32_t *px = surface_rgba(m);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)m->w, (GLsizei)m->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        m->dirty = 0;
        if (!m->attached || m->attached->parent != m)
            break;
    }
    s->priv[0] = (uint32_t)level;   /* highest defined mip level */
    return s->gl_tex;
}

/* ---- 2D ----------------------------------------------------------------- */

static void draw_rgba_quad(const uint32_t *px, int w, int h, const Rect *sr, const Rect *dr, int blend)
{
    glBindTexture(GL_TEXTURE_2D, blit_tex);
    if (w != blit_tex_w || h != blit_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        blit_tex_w = w;
        blit_tex_h = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPushAttrib(GL_ALL_ATTRIB_BITS);
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
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, rt_w, rt_h, 0, -1, 1);
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
    glPopAttrib();
}

void render_blit(Surface *src, const Rect *sr, const Rect *dr, int use_ck)
{
    int saved_ck = src->has_ck;
    if (!use_ck)
        src->has_ck = 0;
    uint32_t *px = surface_rgba(src);
    src->has_ck = saved_ck;
    draw_rgba_quad(px, (int)src->w, (int)src->h, sr, dr, use_ck && saved_ck);
}

void render_readback(Surface *s)
{
    surface_alloc_mem(s);
    uint32_t *px = malloc((size_t)fb_w * (size_t)fb_h * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, fb_w, fb_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    uint32_t bpp = s->pf.bits / 8u;
    for (uint32_t y = 0; y < s->h; y++) {
        uint32_t fy = (uint32_t)fb_h - 1u - (uint32_t)((uint64_t)y * (uint32_t)fb_h / s->h);
        for (uint32_t x = 0; x < s->w; x++) {
            uint32_t fx = (uint32_t)((uint64_t)x * (uint32_t)fb_w / s->w);
            uint32_t v = rgba_to_pixel(&s->pf, px[fy * (uint32_t)fb_w + fx]);
            memcpy(g_mem + s->mem + y * s->pitch + x * bpp, &v, bpp);
        }
    }
    free(px);
}

void render_writeback(Surface *s)
{
    int ck = s->has_ck;
    s->has_ck = 0;
    uint32_t *px = surface_rgba(s);
    s->has_ck = ck;
    Rect r = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    Rect d = { 0, 0, rt_w, rt_h };
    draw_rgba_quad(px, (int)s->w, (int)s->h, &r, &d, 0);
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
    GLenum src0 = alpha ? GL_SOURCE0_ALPHA : GL_SOURCE0_RGB, src1 = alpha ? GL_SOURCE1_ALPHA : GL_SOURCE1_RGB;
    GLenum src2 = alpha ? GL_SOURCE2_ALPHA : GL_SOURCE2_RGB;
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
    glTexEnvf(GL_TEXTURE_ENV, scale, sc);
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
            int mip = st->mipfilter > 1u && st->texture->priv[0] > 1u;
            GLint minf = st->minfilter >= 2u ? (mip ? (st->mipfilter == 3u ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : GL_LINEAR)
                                             : (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, st->magfilter >= 2u ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, (GLint)st->texture->priv[0] - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(st->addressu));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(st->addressv));
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

static void apply_states(int pretransformed, int has_normal)
{
    uint32_t *rs = g_rs.rs;
    if (rs[7]) {
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
    if (rs[22] == 1u || pretransformed) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(rs[22] == 3u ? GL_CW : GL_CCW);
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
            /* Vertex fog computed by the game: the factor is in the specular alpha. */
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 1.0f);
            glFogf(GL_FOG_END, 0.0f);
        } else {
            glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
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
        float amb[4];
        color4(rs[139], amb);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
        glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, g_rs.mat_diffuse);
        glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, g_rs.mat_ambient);
        glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, g_rs.mat_specular);
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, g_rs.mat_emissive);
        glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, g_rs.mat_power > 128.0f ? 128.0f : g_rs.mat_power);
        if (rs[141] && rs[145] == 1u) {
            glEnable(GL_COLOR_MATERIAL);
            glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        } else {
            glDisable(GL_COLOR_MATERIAL);
        }
        if (rs[143])
            glEnable(GL_NORMALIZE);
        else
            glDisable(GL_NORMALIZE);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(g_rs.view);
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
        }
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
    glViewport(x, y, w, h);
    glDepthRange(g_rs.vp_minz, g_rs.vp_maxz);
    /* D3D clip-space z is [0, w]; GL wants [-w, w]: z' = 2z - w. */
    float p[16];
    memcpy(p, g_rs.proj, sizeof(p));
    for (int r = 0; r < 4; r++)
        p[r * 4 + 2] = 2.0f * g_rs.proj[r * 4 + 2] - g_rs.proj[r * 4 + 3];
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(p);
    float wv[16];
    mat_mul(g_rs.world, g_rs.view, wv);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(wv);
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

static float *pos_buf;
static float *fog_buf;
static size_t pos_cap;

void render_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                 int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    if (!nv)
        return;
    if (g_trace_next_frame == 2) {
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
               "| s1 c%u a%u tex %ux%u tf %08X",
               prim, nv, ni, pretransformed, has_normal, has_color, has_spec, ntex, (double)x0, (double)y0, (double)x1,
               (double)y1, v[0].color, g_rs.rs[7], g_rs.rs[14], g_rs.rs[27], g_rs.rs[19], g_rs.rs[20], g_rs.rs[15],
               g_rs.rs[25], g_rs.rs[24], g_rs.rs[41], g_rs.rs[28], g_rs.rs[137], g_rs.rs[22], s0->colorop,
               s0->colorarg1, s0->colorarg2, s0->alphaop, s0->alphaarg1, s0->alphaarg2,
               s0->texture ? s0->texture->w : 0u, s0->texture ? s0->texture->h : 0u,
               s0->texture ? s0->texture->pf.bits : 0u, s0->texture ? s0->texture->has_ck : 0, s1->colorop,
               s1->alphaop, s1->texture ? s1->texture->w : 0u, s1->texture ? s1->texture->h : 0u, g_rs.rs[60]);
    }
    apply_states(pretransformed, has_normal);
    int stages = apply_stages(ntex);
    apply_transforms(pretransformed);

    if (nv > pos_cap) {
        pos_cap = nv;
        pos_buf = realloc(pos_buf, nv * 16u);
        fog_buf = realloc(fog_buf, nv * 4u);
    }
    for (uint32_t i = 0; i < nv; i++) {
        float *p = pos_buf + i * 4u;
        if (pretransformed) {
            float w = v[i].w != 0.0f ? 1.0f / v[i].w : 1.0f;
            p[0] = v[i].x * w;
            p[1] = v[i].y * w;
            p[2] = v[i].z * w;
            p[3] = w;
        } else {
            p[0] = v[i].x;
            p[1] = v[i].y;
            p[2] = v[i].z;
            p[3] = 1.0f;
        }
        fog_buf[i] = (float)(v[i].spec >> 24) / 255.0f;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(4, GL_FLOAT, 0, pos_buf);
    if (has_color || pretransformed) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(RVertex), &v[0].color);
    } else {
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4f(1, 1, 1, 1);
    }
    if (has_normal) {
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, sizeof(RVertex), &v[0].nx);
    } else {
        glDisableClientState(GL_NORMAL_ARRAY);
    }
    if (g_rs.rs[28] && (pretransformed || (!g_rs.rs[35] && !g_rs.rs[140])) && has_spec) {
        glEnableClientState(GL_FOG_COORDINATE_ARRAY);
        glFogCoordPointer(GL_FLOAT, 0, fog_buf);
    } else {
        glDisableClientState(GL_FOG_COORDINATE_ARRAY);
        glFogCoordf(0.0f);
    }
    for (int s = 0; s < stages; s++) {
        glClientActiveTexture(GL_TEXTURE0 + (GLenum)s);
        uint32_t ci = g_rs.stage[s].texcoordindex & 0xFFFFu;
        if ((int)ci < ntex) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, sizeof(RVertex), &v[0].uv[ci < MAX_STAGES ? ci : 0]);
        } else {
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
            glMultiTexCoord2f(GL_TEXTURE0 + (GLenum)s, 0, 0);
        }
    }
    glClientActiveTexture(GL_TEXTURE0);
    if (idx)
        glDrawElements(gl_prim(prim), (GLsizei)ni, GL_UNSIGNED_SHORT, idx);
    else
        glDrawArrays(gl_prim(prim), 0, (GLsizei)nv);
}
