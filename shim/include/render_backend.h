/* The renderer's GL backend (render_backend.c) and the commands render_gl.c
 * sends it.
 *
 * render_gl.c is the front end: it keeps the Direct3D state, batches and packs
 * vertices and uploads textures. Everything that sets GL state through the
 * renderer's state caches is a backend command, carrying a copy of the state it
 * needs, so it can run on the render thread (render_queue.h: rq_exec) while the
 * game goes on changing g_rs. Without the render thread the commands run at once.
 *
 * Include after the GL headers. */
#ifndef IGI_RENDER_BACKEND_H
#define IGI_RENDER_BACKEND_H

#include "render.h"

/* Interleaved vertex layout of a draw or batch. */
typedef struct Layout {
    int pretransformed, has_normal, has_color, has_spec, ntex;   /* draw key */
    int pos_size;               /* 4: pre-transformed (x/w, y/w, z/w, 1/w); 3: object space */
    int use_color;
    int use_fog;                /* game vertex fog taken from the specular alpha (Vita: right after uv0) */
    int stages;                 /* texture stages the backend enables */
    int uv_src[MAX_STAGES];     /* RVertex uv set feeding each stage, -1 = none */
    uint32_t stride;            /* bytes per interleaved vertex */
    uint32_t off_col, off_nrm, off_fog, off_uv[MAX_STAGES];
} Layout;

/* Where a draw's attributes live: client arrays (vbo 0) or offsets into a buffer
 * object; ibo: indices at offset 0 of that buffer object (idx ignored). On the
 * Vita, vbo_data/ibo_data (when set) are first given to the buffer objects with
 * vglBufferData (the GPU arena). */
typedef struct DrawArrays {
    GLuint vbo, ibo;
    int stride;
    const void *pos, *col, *nrm, *fog, *uv[MAX_STAGES];
    const void *vbo_data, *ibo_data;
} DrawArrays;

/* Front-end settings the backend works with (sent when they change). */
typedef struct FrameParams {
    int rt_w, rt_h;             /* game resolution */
    int fb_w, fb_h;             /* drawable */
    int vp_x, vp_y, vp_w, vp_h; /* game image inside the drawable */
    int diag;                   /* diagnostic mode (DIAG_*) */
    int cull_override;          /* 0 normal, 1 off, 2 flipped */
    int z_remap;                /* remap D3D clip z to GL's (before the diagnostic flip) */
    int timing;                 /* measure state/draw time (single thread only) */
} FrameParams;

enum { DIAG_NONE, DIAG_NO_BATCH, DIAG_NO_CULL, DIAG_FLIP_ZREMAP, DIAG_NO_SCREEN, DIAG_NO_3D, DIAG_DEPTH_LEQUAL,
       DIAG_NO_COMPACT, DIAG_USEPROGRAM, DIAG_ZERO_UV, DIAG_LEVEL0, DIAG_NO_NEARCLIP, DIAG_NO_VB_CACHE, DIAG_VBO,
       DIAG_COUNT };

/* A texture stage as the draw uses it. */
typedef struct DrawStage {
    GLuint tex;                 /* 0: stage disabled */
    uint32_t colorop, colorarg1, colorarg2, alphaop, alphaarg1, alphaarg2;
    int sampler;                /* send the sampler state below (it changed for this texture) */
    GLint minf, magf, wrap_s, wrap_t, max_level;
} DrawStage;

/* One draw: the render state it needs and its arrays. */
typedef struct DrawCmd {
    int prim;
    uint32_t nv, ni;
    const uint16_t *idx;        /* client indices (A.ibo == 0), NULL for glDrawArrays */
    Layout L;
    DrawArrays A;
    /* D3D render states (by number) */
    uint32_t zenable, fillmode, shademode, zwrite, alphatest, srcblend, destblend, cullmode, zfunc, alpharef,
        alphafunc, blend, fog, fogcolor, fogtablemode, fogstart, fogend, fogdensity, colorkey, lighting,
        fogvertexmode, tfactor;
    DrawStage stage[MAX_STAGES];
    /* transforms (not for pre-transformed draws) */
    int32_t vp_x, vp_y, vp_w, vp_h;
    float vp_minz, vp_maxz;
    float world[16], view[16], proj[16];
} DrawCmd;

/* Fixed-function lighting inputs (sent when g_light_serial changes). */
typedef struct LightSnapshot {
    uint32_t ambient, color_material, normalize;
    float mat[17];              /* diffuse, ambient, specular, emissive, power */
    float view[16];
    Light lights[MAX_LIGHTS];   /* disabled lights all zero */
} LightSnapshot;

#define CLEAR_MAX_RECTS 16
typedef struct ClearCmd {
    GLbitfield mask;
    float color[4], z;
    int restore_mask;           /* depth writes off while scissoring (not clearing depth) */
    uint32_t n;
    struct {
        int full;               /* whole drawable: no scissor */
        GLint x, y;
        GLsizei w, h;
    } r[CLEAR_MAX_RECTS];
} ClearCmd;

/* RGBA8 pixels drawn as a textured quad over the render target (2D blits). */
typedef struct QuadCmd {
    const uint32_t *px;         /* valid until the command has run */
    GLuint tex;
    int w, h, blend;
    Rect sr, dr;
} QuadCmd;

#define OVERLAY_LINES 12
typedef struct OverlayCmd {
    char lines[OVERLAY_LINES][40];
} OverlayCmd;

void bk_params(const void *frame_params);
void bk_init(const void *unused);       /* viewport and clear after a mode set */
void bk_bars(const void *unused);       /* black bars around the game image */
void bk_clear(const void *clear_cmd);
void bk_quad(const void *quad_cmd);
void bk_lights(const void *light_snapshot);
void bk_draw(const void *draw_cmd);
/* Sends a texture stage's sampler state on its own (normally part of bk_draw). */
void bk_sampler(const void *draw_stage);
void bk_overlay(const void *overlay_cmd);
/* Single-thread mode with timing on: state setup and draw-call microseconds since the last call. */
void bk_take_timing(uint64_t *state_us, uint64_t *gl_us);

#endif
