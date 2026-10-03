/* Renderer front end for the DirectDraw / Direct3D 7 shims: keeps the D3D state,
 * batches and packs vertices, uploads textures, and sends GL work to the backend
 * (render_backend.c, on the render thread when it runs). */
#include "platform.h"
#include "bench.h"
#include "tick_test.h"
#include "igi_fixes.h"
#include "render.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#elif defined(__vita__)
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <GL/gl.h>
#endif
/* Every gl* call below goes through the render thread's recorder. */
#include "render_queue.h"
#include "render_backend.h"

RenderState g_rs;
uint32_t g_rs_serial;
uint32_t g_rs_parts;
uint32_t g_light_serial;
uint32_t g_frame_serial;
static uint32_t perf_gpu_draws;   /* draws actually submitted to GL this frame */
static void perf_frame_begin_swap(void);
static void perf_swap(void);

extern uint64_t win32_perf_counter(void);   /* microseconds */

/* Timestamps for the per-section draw counters (DS/DT/DP/DG/DO on the overlay).
 * Each is a system call on the Vita and a draw takes several: off unless
 * IGI_FINE_TIMING=1 (desktop) or fine_timing=1 in igi.cfg (Vita). */
static int fine_timing = -1;

static uint64_t fine_now(void)
{
    if (fine_timing < 0) {
        const char *e = getenv("IGI_FINE_TIMING");
        fine_timing = e && e[0] == '1';
#if defined(__vita__)
        FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (strncmp(buf, "fine_timing=", 12) == 0)
                    fine_timing = buf[12] == '1';
            fclose(f);
        }
#endif
    }
    return fine_timing ? win32_perf_counter() : 0u;
}

/* Same for the draw front end: never on the render thread (the pack thread), whose
 * clock queries would change the game's (IGI_FAKE_CLOCK advances per query). */
static uint64_t fine_now_fe(void)
{
    return g_pack_thread ? 0u : fine_now();
}

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

static int diag_mode;

/* ---- pack thread (render.h) -------------------------------------------------
 * The draw front end below (batching, packing, near-plane clipping) runs on the
 * game thread, or with g_pack_thread on the render thread only. There it never
 * reads g_rs: the game thread sends a snapshot of the state (PkState) whenever
 * a draw needs a different one, and the front end reads it through the cur_*
 * accessors. */
int g_pack_thread;

/* What the vertex layout of a draw depends on besides its vertex format. */
typedef struct LayoutIn {
    int stages;                       /* active_stages() */
    uint32_t tci[MAX_STAGES];         /* texture coordinate index of each stage */
    uint32_t fog, fogtable, fogvertex;
} LayoutIn;

typedef struct PkState {
    uint32_t serial;                  /* g_rs_serial when taken */
    LayoutIn li;
    DrawCmd d;                        /* the render state part (from zenable on) */
} PkState;

/* Render thread (pack thread on). */
static PkState rs_cur;                /* state of the draws being executed */
static int rs_cur_used;               /* a draw went to GL with rs_cur */
static int rs_diag, rs_trace;         /* of the draw being executed */
static uint32_t rs_frame;             /* frames ended so far */
static uint64_t rs_pc[PC_COUNT];      /* counters of the frame being executed */
static uint32_t rs_gpu_draws;
/* Published at the end of each executed frame, collected by the game thread. */
static _Atomic uint32_t rs_pub_gpu_draws, rs_pub_verts, rs_pub_indices, rs_pub_pack_us;

static int batch_enabled = -1;      /* IGI_BATCH=0: no draw batching */

/* Game thread (pack thread on). */
static int pk_pending;                /* draws recorded since the last flush command */
static int pk_snap_valid;             /* the render thread has the state for g_rs_serial */
static uint32_t pk_snap_serial;

static int cur_diag(void)
{
    return g_pack_thread ? rs_diag : diag_mode;
}

static int cur_trace(void)
{
    return g_pack_thread ? rs_trace : g_trace_next_frame == 2;
}

static uint32_t cur_serial(void)
{
    return g_pack_thread ? rs_cur.serial : g_rs_serial;
}

/* Per-frame counters the front end adds to (the render thread has its own). */
#define PCR (g_pack_thread ? rs_pc : pc_frame)

/* The sampling profiler's section markers belong to the game thread: the front
 * end only sets them when it runs there. */
#if defined(IGI_PROFILE)
#define PROF_RS(id)                          \
    const uint32_t prof_hprev = rt_prof_cur; \
    if (!g_pack_thread)                      \
    rt_prof_cur = (id)
#define PROF_RS_END                  \
    do {                             \
        if (!g_pack_thread)          \
            rt_prof_cur = prof_hprev; \
    } while (0)
#else
#define PROF_RS(id) ((void)0)
#define PROF_RS_END ((void)0)
#endif

int render_diag_mode(void)
{
    return diag_mode;
}

void render_pk_draw_begin(int diag, int trace)
{
    rs_diag = diag;
    rs_trace = trace;
}

/* 4x4 matrix product (as the backend computes it), for the near-plane tests and traces. */
static void mat_mul(const float *a, const float *b, float *out)
{
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
                           a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}

static int rt_w = 640, rt_h = 480;       /* game resolution */
static int fb_w = 640, fb_h = 480;       /* drawable size in pixels */
static int vp_x, vp_y, vp_w = 640, vp_h = 480;   /* game image inside the drawable (aspect kept) */

/* Output framing: 1 = stretch to fill the display, 0 = keep 4:3 with black bars.
 * IGI_ASPECT=fill|keep on desktop, "aspect=fill|keep" in ux0:data/igi/igi.cfg on the Vita. */
static int aspect_fill = -1;

/* Culling override for diagnosis: 0 normal, 1 off, 2 flipped (cull=off|flip). */
static int cull_override;
/* Live diagnostic mode (Vita: L+R+SELECT cycles it), shown on the overlay; diag_mode
 * is declared above, the modes in render_backend.h. */
static const char *const diag_names[DIAG_COUNT] = { "NORMAL", "NO BATCH", "NO CULL", "FLIP ZREMAP",
                                                     "NO 2D VERTS", "NO 3D VERTS", "DEPTH LEQUAL", "NO COMPACT",
                                                     "USEPROGRAM", "ZERO UV", "LEVEL0",
                                                     "NO NEARCLIP", "NO VB CACHE", "VBO" };
int render_diag_no_vb_cache(void)
{
    return diag_mode == DIAG_NO_VB_CACHE;
}

int render_diag_no_compact(void)
{
    return cur_diag() == DIAG_NO_COMPACT;
}

int render_pk_no_compact(void)
{
    return diag_mode == DIAG_NO_COMPACT;
}

void render_diag_cycle(void)
{
    diag_mode = (diag_mode + 1) % DIAG_COUNT;
    g_tex_epoch++;
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

/* The backend's copy of the settings above (and fine timing), sent when they change. */
static FrameParams params_sent;
static int params_sent_valid;

static void send_params(void)
{
    FrameParams f = { rt_w, rt_h, fb_w, fb_h, vp_x, vp_y, vp_w, vp_h, diag_mode, cull_override, z_remap,
                      fine_timing > 0 && !rq_on };
    const int *a = (const int *)(const void *)&f, *b = (const int *)(const void *)&params_sent;
    int diff = !params_sent_valid;
    for (size_t i = 0; i < sizeof(f) / sizeof(int); i++)
        diff |= a[i] != b[i];
    if (!diff)
        return;
    params_sent = f;
    params_sent_valid = 1;
    rq_exec(bk_params, &f, sizeof(f));
}

/* Paint the area outside the game image black. */
static void draw_bars(void)
{
    if (vp_w == fb_w && vp_h == fb_h)
        return;
    send_params();
    rq_exec(bk_bars, NULL, 0);
}

static GLuint white_tex;
static GLuint blit_tex;
static uint32_t *conv_buf;
static size_t conv_cap;

/* Scratch for converted pixels; while recording, memory of the frame (uploaded
 * from there without another copy: q_glTexImage2D_owned). */
static uint32_t *conv(size_t n)
{
    if (rq_on)
        return rq_data(n * 4u);
    if (n > conv_cap) {
        conv_cap = n;
        conv_buf = realloc(conv_buf, n * 4u);
    }
    return conv_buf;
}

#if defined(__vita__) && defined(IGI_PROFILE)
/* Phase markers inside a private vitaGL built with PROF_HOOKS=1 (absent otherwise). */
extern volatile uint32_t *vgl_prof_slot __attribute__((weak));
extern uint32_t vgl_prof_base __attribute__((weak));
#endif

#if defined(__vita__) && defined(IGI_PROFILE)
/* vitaGL's phase markers while it runs on the render thread (not sampled): they
 * must not land in the game thread's rt_prof_cur. */
static volatile uint32_t render_thread_prof;
#endif

/* The pack thread is on with the render thread unless pack_thread=0 (igi.cfg) or
 * IGI_PACK_THREAD=0. */
static int want_pack_thread(void)
{
    int on = 1;
    const char *e = getenv("IGI_PACK_THREAD");
    if (e)
        on = e[0] != '0';
#if defined(__vita__)
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f))
            if (strncmp(buf, "pack_thread=", 12) == 0)
                on = buf[12] != '0';
        fclose(f);
    }
#endif
    return on;
}

#if defined(__vita__)
static int arena_ready(void);
#endif

void render_init(int w, int h)
{
    render_flush();
    rq_start();
    static int pack_decided;
    if (!pack_decided) {
        pack_decided = 1;
        g_pack_thread = rq_on && want_pack_thread();
        rt_log("pack thread: %s", g_pack_thread ? "on" : "off");
        const char *e = getenv("IGI_BATCH");
        batch_enabled = !(e && e[0] == '0');
#if defined(__vita__)
        /* The render thread only rotates the arena: its GPU memory and buffer
         * names come from here. */
        if (g_pack_thread)
            arena_ready();
#endif
    }
    pk_snap_valid = 0;
#if defined(__vita__) && defined(IGI_PROFILE)
    if (&vgl_prof_slot && &vgl_prof_base) {
        vgl_prof_base = PROF_H_VGL;
        vgl_prof_slot = rq_on ? &render_thread_prof : &rt_prof_cur;
    }
#endif
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
    g_rs_serial++;
    g_light_serial++;
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
    fine_now();   /* reads the timing setting */
    send_params();
    rq_exec(bk_init, NULL, 0);
}

void render_begin_frame(void)
{
    update_letterbox();
}

#if defined(__vita__)
static int shot_hold = -1;   /* benchmark frame to hold on screen after its swap */
#endif

/* IGI_SHOT=N: save every Nth presented frame as shot_<frame>.bmp (debugging aid); the
 * benchmark's screenshots go to bench_shot_<measured frame>.bmp. */
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
    int bench_shot = bench_shot_due();
    const char *tick_shot = tick_test_shot_due();
#if defined(__vita__)
    /* Vita3K does not write rendered frames back to guest memory (read backs come
     * out black): the frame is held on screen instead for an external window
     * capture (tools/bench_vita3k.sh with SHOT set). */
    if (bench_shot >= 0) {
        shot_hold = bench_shot;
        bench_shot = -1;
    }
#endif
    if (!forced && bench_shot < 0 && !tick_shot && (every <= 0 || frame % (unsigned)every))
        return;
    uint8_t *px = malloc((size_t)fb_w * (size_t)fb_h * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, fb_w, fb_h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    char name[640];
#if defined(__vita__)
    const char *dir = "ux0:data/igi/";
#else
    const char *dir = "";
#endif
    if (tick_shot)
        snprintf(name, sizeof(name), "%s", tick_shot);
    else if (bench_shot >= 0)
        snprintf(name, sizeof(name), "%sbench_shot_%05d.bmp", dir, bench_shot);
    else
        snprintf(name, sizeof(name), "%sshot_%05u.bmp", dir, frame);
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

static void pk_frame_cmd(const void *unused);

void render_present(void)
{
    render_flush();
    if (g_pack_thread)
        rq_exec(pk_frame_cmd, NULL, 0);
    g_frame_serial++;
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
#if defined(__vita__)
    if (shot_hold >= 0) {
        rq_sync();                       /* the render thread has swapped it */
        sceKernelDelayThread(1000000);   /* until the frame is on screen */
        /* A file of its own: Vita3K only writes igi.log out when it is closed. */
        FILE *f = fopen("ux0:data/igi/bench_shot_now.txt", "w");
        if (f) {
            fprintf(f, "%d\n", shot_hold);
            fclose(f);
        }
        sceKernelDelayThread(3000000);   /* while it is captured */
        shot_hold = -1;
    }
#endif
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

void render_clear(uint32_t nrects, const Rect *rects, uint32_t flags, uint32_t color, float z)
{
    render_flush();
    ClearCmd c;
    c.mask = 0;
    if (flags & 1u) {
        c.mask |= GL_COLOR_BUFFER_BIT;
        c.color[0] = ((color >> 16) & 0xFFu) / 255.0f;
        c.color[1] = ((color >> 8) & 0xFFu) / 255.0f;
        c.color[2] = (color & 0xFFu) / 255.0f;
        c.color[3] = (color >> 24) / 255.0f;
    }
    if (flags & 2u) {
        c.mask |= GL_DEPTH_BUFFER_BIT;
        c.z = z;
    }
    if (!c.mask)
        return;
    c.restore_mask = !(flags & 2u);
    uint32_t n = nrects ? nrects : 1u;
    if (n > CLEAR_MAX_RECTS)
        n = CLEAR_MAX_RECTS;
    c.n = n;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l, t, rr, b;
        if (nrects) {
            l = rects[i].l; t = rects[i].t; rr = rects[i].r; b = rects[i].b;
        } else {
            l = g_rs.vp_x; t = g_rs.vp_y; rr = g_rs.vp_x + g_rs.vp_w; b = g_rs.vp_y + g_rs.vp_h;
        }
        int full = l <= 0 && t <= 0 && rr >= rt_w && b >= rt_h;
        c.r[i].full = full && vp_w == fb_w && vp_h == fb_h;
        to_fb(l, t, rr - l, b - t, &c.r[i].x, &c.r[i].y, &c.r[i].w, &c.r[i].h);
    }
    send_params();
    rq_exec(bk_clear, &c, offsetof(ClearCmd, r) + n * sizeof(c.r[0]));
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

uint32_t g_tex_epoch = 1;

void render_texture_release(Surface *s)
{
    g_tex_epoch++;
    if (g_pack_thread && s->gl_tex) {
        /* Recorded draws still to be drawn may use it. */
        render_flush();
        pk_snap_valid = 0;
    }
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

static uint32_t texture_get(Surface *s);

uint32_t render_texture_get(Surface *s)
{
    PROF_SECTION(PROF_H_TEXTURE);
    uint32_t r = texture_get(s);
    PROF_SECTION_END;
    return r;
}

static uint32_t texture_get(Surface *s)
{
    if (!s)
        return white_tex;
    if (s->clean_epoch == g_tex_epoch)
        return s->gl_tex;
    int levels_dirty = 0;
    for (Surface *m = s; m; m = next_mip(m))
        levels_dirty |= m->dirty;
    if (s->gl_tex && !levels_dirty && !(diag_mode != DIAG_LEVEL0 && s->priv[0] == 0 && next_mip(s))) {
        s->clean_epoch = g_tex_epoch;
        return s->gl_tex;
    }
    uint64_t t0 = fine_now();

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
    /* Audit experiment (diagnostic mode 10): level 0 only. It did not fix the
     * see-through walls and is off by default. */
    if (diag_mode == DIAG_LEVEL0) {
        for (int i = 1; i < n; i++)
            lv[i]->dirty = 0;
        n = 1;
    }
#endif
    if (s->priv[0] != (uint32_t)(n - 1))
        changed = 1;
    if (!changed) {
        for (int i = 0; i < n; i++)
            lv[i]->dirty = 0;
        pc_frame[PC_TEX_SKIP]++;
        pc_frame[PC_TEX_US] += fine_now() - t0;
        return s->gl_tex;
    }

    if (g_pack_thread) {
        /* Recorded draws still to be drawn may use the old pixels, and the
         * state snapshot holds the sampler state this upload resets. */
        render_flush();
        pk_snap_valid = 0;
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
            q_glTexImage2D_owned(GL_TEXTURE_2D, level, GL_RGBA, (GLsizei)m->w, (GLsizei)m->h, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, px);
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
    pc_frame[PC_TEX_US] += fine_now() - t0;
    return s->gl_tex;
}

/* ---- 2D ----------------------------------------------------------------- */

/* RGBA pixels drawn over the render target. owned: px is conv() memory (see
 * there); else it is copied for the render thread. */
static void draw_rgba_quad(const uint32_t *px, int owned, int w, int h, const Rect *sr, const Rect *dr, int blend)
{
    if (rq_on && !owned) {
        uint32_t *copy = rq_data((size_t)w * (size_t)h * 4u);
        memcpy(copy, px, (size_t)w * (size_t)h * 4u);
        px = copy;
    }
    QuadCmd q = { px, blit_tex, w, h, blend, *sr, *dr };
    send_params();
    rq_exec(bk_quad, &q, sizeof(q));
}

void render_blit(Surface *src, const Rect *sr, const Rect *dr, int use_ck)
{
    render_flush();
    uint64_t t0 = fine_now();
    pc_frame[PC_2D_N]++;
    VideoFrameSource *vs = src->client_mem ? video_source_for(src->mem) : NULL;
    if (vs) {
        /* Movie frame: convert it once, upload at native size, let the GPU scale. */
        const uint32_t *px = vs->rgba(vs);
        Rect vr = { (int32_t)((int64_t)sr->l * vs->width / (int32_t)src->w),
                    (int32_t)((int64_t)sr->t * vs->height / (int32_t)src->h),
                    (int32_t)((int64_t)sr->r * vs->width / (int32_t)src->w),
                    (int32_t)((int64_t)sr->b * vs->height / (int32_t)src->h) };
        draw_rgba_quad(px, 0, vs->width, vs->height, &vr, dr, 0);
        pc_frame[PC_2D_US] += fine_now() - t0;
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
        draw_rgba_quad(px, 1, (int)w, (int)h, &local, dr, use_ck && saved_ck);
        pc_frame[PC_2D_BYTES] += (uint64_t)w * h * 4u;
    } else {
        uint32_t *px = surface_rgba(src);
        src->has_ck = saved_ck;
        draw_rgba_quad(px, 1, (int)src->w, (int)src->h, sr, dr, use_ck && saved_ck);
        pc_frame[PC_2D_BYTES] += (uint64_t)src->w * src->h * 4u;
    }
    pc_frame[PC_2D_US] += fine_now() - t0;
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
    uint64_t t0 = fine_now();
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
    pc_frame[PC_2D_US] += fine_now() - t0;
}

void render_writeback(Surface *s)
{
    render_flush();
    uint64_t t0 = fine_now();
    pc_frame[PC_2D_N]++;
    int ck = s->has_ck;
    s->has_ck = 0;
    uint32_t *px = surface_rgba(s);
    s->has_ck = ck;
    Rect r = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    Rect d = { 0, 0, rt_w, rt_h };
    draw_rgba_quad(px, 1, (int)s->w, (int)s->h, &r, &d, 0);
    pc_frame[PC_2D_BYTES] += (uint64_t)s->w * s->h * 4u;
    pc_frame[PC_2D_US] += fine_now() - t0;
}

/* ---- 3D state ----------------------------------------------------------- */

static GLint wrap_mode(uint32_t a)
{
    switch (a) {
    case 2: return GL_MIRRORED_REPEAT;
    case 3: case 4: return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

/* Lighting inputs last sent to the backend (g_light_serial then). */
static uint32_t light_sent_serial;
static int light_sent_valid;

static void send_lights(void)
{
    if (light_sent_valid && light_sent_serial == g_light_serial)
        return;
    light_sent_valid = 1;
    light_sent_serial = g_light_serial;
    const uint32_t *rs = g_rs.rs;
    LightSnapshot *cur = rq_begin(bk_lights, sizeof(*cur));
    rt_fill_small((uint8_t *)cur, 0, sizeof(*cur));   /* also clears padding for memcmp; no SceLibc memset */
    cur->ambient = rs[139];
    cur->color_material = rs[141] && rs[145] == 1u;
    cur->normalize = rs[143] != 0;
    memcpy(cur->mat, g_rs.mat_diffuse, 16);
    memcpy(cur->mat + 4, g_rs.mat_ambient, 16);
    /* D3D adds specular lighting only with SPECULARENABLE; GL always does. */
    if (rs[29])
        memcpy(cur->mat + 8, g_rs.mat_specular, 16);
    memcpy(cur->mat + 12, g_rs.mat_emissive, 16);
    cur->mat[16] = g_rs.mat_power;
    memcpy(cur->view, g_rs.view, sizeof(cur->view));
    for (int i = 0; i < MAX_LIGHTS; i++)
        if (g_rs.lights[i].enabled)
            rt_copy_small(&cur->lights[i], &g_rs.lights[i], sizeof(Light));
    if (g_trace_next_frame == 2) {
        rt_log("lights: ambient %08X colorvertex %u diffsrc %u ambsrc %u specsrc %u emissrc %u specular %u normalize %u",
               rs[139], rs[141], rs[145], rs[147], rs[146], rs[148], rs[29], rs[143]);
        const float *m = cur->mat;
        rt_log("    material: diff %.2f %.2f %.2f %.2f amb %.2f %.2f %.2f %.2f spec %.2f %.2f %.2f %.2f "
               "emis %.2f %.2f %.2f %.2f power %.1f",
               (double)m[0], (double)m[1], (double)m[2], (double)m[3], (double)m[4], (double)m[5], (double)m[6],
               (double)m[7], (double)m[8], (double)m[9], (double)m[10], (double)m[11], (double)m[12], (double)m[13],
               (double)m[14], (double)m[15], (double)m[16]);
        for (int i = 0; i < MAX_LIGHTS; i++) {
            const Light *l = &cur->lights[i];
            if (l->enabled)
                rt_log("    light %d: type %u diff %.2f %.2f %.2f amb %.2f %.2f %.2f spec %.2f %.2f %.2f pos %.1f %.1f "
                       "%.1f dir %.2f %.2f %.2f range %.1f att %.4f %.6f %.8f theta %.2f phi %.2f falloff %.2f",
                       i, l->type, (double)l->diffuse[0], (double)l->diffuse[1], (double)l->diffuse[2],
                       (double)l->ambient[0], (double)l->ambient[1], (double)l->ambient[2], (double)l->specular[0],
                       (double)l->specular[1], (double)l->specular[2], (double)l->pos[0], (double)l->pos[1],
                       (double)l->pos[2], (double)l->dir[0], (double)l->dir[1], (double)l->dir[2], (double)l->range,
                       (double)l->att0, (double)l->att1, (double)l->att2, (double)l->theta, (double)l->phi,
                       (double)l->falloff);
        }
    }
    rq_end();
}

/* The draw's texture stages: textures uploaded (and bound names), combiner
 * inputs, and the sampler state where it changed for the texture. */
static void stages_for_draw(DrawStage *out)
{
    int active = 0;
    for (int i = 0; i < MAX_STAGES; i++) {
        const StageState *st = &g_rs.stage[i];
        DrawStage *ds = &out[i];
        /* A stage without a texture passes the previous result through, as in D3D. */
        if (st->colorop == 1u || (i > 0 && active < i) || !st->texture) {
            ds->tex = 0;
            ds->sampler = 0;
            continue;
        }
        active++;
        Surface *t = st->texture;
        ds->tex = render_texture_get(t);
        ds->colorop = st->colorop;
        ds->colorarg1 = st->colorarg1;
        ds->colorarg2 = st->colorarg2;
        ds->alphaop = st->alphaop;
        ds->alphaarg1 = st->alphaarg1;
        ds->alphaarg2 = st->alphaarg2;
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
        ds->sampler = t->priv[TEXP_SAMPLER] != sig;
        ds->magf = magf;   /* also read by the screen projection (texel snap) */
        if (ds->sampler) {
            t->priv[TEXP_SAMPLER] = sig;
            ds->minf = minf;
            ds->wrap_s = ws;
            ds->wrap_t = wt;
            ds->max_level = (GLint)t->priv[0] - 1;
        }
    }
}

/* ---- vertex packing -------------------------------------------------------
 * Vertices are copied straight into one interleaved array holding only what
 * the draw uses. Batched draws are packed when they are queued, so every
 * vertex is read once and never copied as a whole RVertex.
 *
 * On the Vita the arrays (and the batch index lists) are written directly into
 * GPU-mapped memory and drawn from there through buffer objects: with client
 * arrays vitaGL copies every enabled attribute and the indices again on each
 * draw.
 */
/* Number of stages the backend enables for the current state (same rule as stages_for_draw). */
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

static void layout_in_current(LayoutIn *li)
{
    li->stages = active_stages();
    for (int s = 0; s < MAX_STAGES; s++)
        li->tci[s] = g_rs.stage[s].texcoordindex;
    li->fog = g_rs.rs[28];
    li->fogtable = g_rs.rs[35];
    li->fogvertex = g_rs.rs[140];
}

static void layout_init(Layout *L, int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    LayoutIn cur;
    const LayoutIn *li = &rs_cur.li;
    if (!g_pack_thread) {
        layout_in_current(&cur);
        li = &cur;
    }
    L->pretransformed = pretransformed;
    L->has_normal = has_normal;
    L->has_color = has_color;
    L->has_spec = has_spec;
    L->ntex = ntex;
    L->pos_size = pretransformed ? 4 : 3;
    L->use_color = has_color || pretransformed;
    L->use_fog = li->fog && (pretransformed || (!li->fogtable && !li->fogvertex)) && has_spec;
    L->stages = li->stages;
    for (int s = 0; s < MAX_STAGES; s++) {
        L->uv_src[s] = -1;
        uint32_t ci = li->tci[s] & 0xFFFFu;
        if (s < L->stages && (int)ci < ntex)
            L->uv_src[s] = ci < MAX_STAGES ? (int)ci : 0;
    }
#if defined(__vita__)
    /* vitaGL (tools/vitagl) reads the fog coordinate as a third texture coordinate
     * of the first unit: it follows uv0 there, and needs that unit. */
    L->use_fog &= L->uv_src[0] >= 0;
    const int fog_after_uv0 = 1;
#else
    const int fog_after_uv0 = 0;
#endif
    uint32_t o = (uint32_t)L->pos_size * 4u;
    L->off_col = o;
    o += L->use_color ? 4u : 0u;
    L->off_nrm = o;
    o += has_normal ? 12u : 0u;
    if (!fog_after_uv0) {
        L->off_fog = o;
        o += L->use_fog ? 4u : 0u;
    }
    for (int s = 0; s < MAX_STAGES; s++) {
        L->off_uv[s] = o;
        o += L->uv_src[s] >= 0 ? 8u : 0u;
        if (s == 0 && fog_after_uv0) {
            L->off_fog = o;
            o += L->use_fog ? 4u : 0u;
        }
    }
    L->stride = o;
}

/* ---- vertex/index storage of the draw or batch being built ------------------ */
#define BATCH_MAX_V 16384u
#define BATCH_MAX_I 49152u

static uint8_t *vtx;          /* interleaved vertices (layout stride) */
static uint32_t vtx_cap;      /* vertices that fit at vtx */
static uint16_t *idx_out;     /* batch index list */
static uint32_t idx_cap;      /* indices that fit at idx_out */
static int vtx_gpu;           /* vtx and idx_out are in the GPU arena */

static uint8_t *heap_vtx;
static uint16_t *heap_idx;
static size_t heap_vtx_bytes, heap_idx_count;

/* Client arrays must outlive the draw call: the front end records for the render
 * thread (not with the pack thread, which runs the front end there). */
static int streams_recorded(void)
{
    return rq_on && !g_pack_thread;
}

#if defined(__vita__)
/* GPU-mapped arena, one segment per frame in flight rotated with vitaGL's frame
 * counter like its own temporary pool, so a segment is rewritten only once the
 * GPU is done with it. A draw that does not fit falls back to client arrays.
 * With the render thread the game records up to one frame ahead of vitaGL:
 * one more segment, rotated with the frames presented here. */
#define ARENA_SEGS 4u
#define ARENA_VTX_BYTES (3u << 20)
#define ARENA_IDX_BYTES (512u << 10)
static uint8_t *arena_vtx[ARENA_SEGS];
static uint16_t *arena_idx[ARENA_SEGS];
static uint32_t arena_vpos, arena_ipos, arena_seg, arena_frame;
static int arena_state;       /* 0 untried, 1 ready, -1 unavailable */
static GLuint arena_vbo, arena_ibo;

static uint32_t arena_segs(void)
{
    return rq_on ? ARENA_SEGS : 3u;
}

static uint32_t arena_frame_number(void)
{
    if (g_pack_thread)
        return rs_frame;   /* the render thread writes the arena while it draws */
    return rq_on ? g_frame_serial : vglGetFrameNumber();
}

static int arena_ready(void)
{
    if (arena_state == 0) {
        arena_state = -1;
        for (uint32_t s = 0; s < arena_segs(); s++) {
            arena_vtx[s] = vglForceAlloc(ARENA_VTX_BYTES);
            arena_idx[s] = vglForceAlloc(ARENA_IDX_BYTES);
            if (!arena_vtx[s] || !arena_idx[s]) {
                rt_log("render: no GPU memory for the vertex arena; using client arrays");
                return 0;
            }
        }
        glGenBuffers(1, &arena_vbo);
        glGenBuffers(1, &arena_ibo);
        arena_frame = arena_frame_number();
        arena_seg = arena_frame % arena_segs();
        arena_state = 1;
    }
    if (arena_state < 0)
        return 0;
    uint32_t f = arena_frame_number();
    if (f != arena_frame) {
        arena_frame = f;
        arena_seg = f % arena_segs();
        arena_vpos = arena_ipos = 0;
    }
    return 1;
}
#endif

/* Opens storage for a draw or batch of this layout with room for at least
 * nv vertices and ni indices. */
static void stream_begin(const Layout *L, uint32_t nv, uint32_t ni)
{
#if defined(__vita__)
    if (arena_ready()) {
        uint32_t vpos = (arena_vpos + 15u) & ~15u;
        uint32_t vc = vpos < ARENA_VTX_BYTES ? (ARENA_VTX_BYTES - vpos) / L->stride : 0u;
        uint32_t ic = (ARENA_IDX_BYTES - arena_ipos) / 2u;
        if (vc >= nv && ic >= ni) {
            vtx = arena_vtx[arena_seg] + vpos;
            vtx_cap = vc < BATCH_MAX_V ? vc : BATCH_MAX_V;
            idx_out = arena_idx[arena_seg] + arena_ipos / 2u;
            idx_cap = ic < BATCH_MAX_I ? ic : BATCH_MAX_I;
            arena_vpos = vpos;
            vtx_gpu = 1;
            return;
        }
        static int reported;
        if (!reported) {
            reported = 1;
            rt_log("render: vertex arena full (frame %u); falling back to client arrays", arena_frame);
        }
    }
#endif
    uint32_t vn = nv > BATCH_MAX_V ? nv : BATCH_MAX_V;
    uint32_t in = ni > BATCH_MAX_I ? ni : BATCH_MAX_I;
    if (streams_recorded()) {
        /* Client arrays are read when the render thread draws: frame memory. */
        vtx = rq_vtx_reserve((size_t)vn * L->stride);
    } else if ((size_t)vn * L->stride > heap_vtx_bytes) {
        heap_vtx_bytes = (size_t)vn * L->stride;
        free(heap_vtx);
        heap_vtx = malloc(heap_vtx_bytes);
    }
    if (!streams_recorded())
        vtx = heap_vtx;
    if (in > heap_idx_count) {
        heap_idx_count = in;
        free(heap_idx);
        heap_idx = malloc(heap_idx_count * sizeof(uint16_t));
    }
    vtx_cap = vn;
    idx_out = heap_idx;
    idx_cap = in;
    vtx_gpu = 0;
}

/* The draw of the open storage went to GL: its memory stays in use this frame. */
static void stream_end(const Layout *L, uint32_t nv, uint32_t ni)
{
#if defined(__vita__)
    if (vtx_gpu) {
        arena_vpos += nv * L->stride;
        arena_ipos += (ni * 2u + 3u) & ~3u;
    }
#else
    (void)ni;
#endif
    if (!vtx_gpu && streams_recorded())
        rq_vtx_commit((size_t)nv * L->stride);
    vtx_gpu = 0;
}

/* Writes nv vertices at array position `base`. */
static void pack_vertices(const Layout *L, const RVertex *v, uint32_t nv, uint32_t base)
{
    PROF_RS(PROF_H_PACK);
    uint64_t t0 = g_pack_thread ? 0u : fine_now();
    const uint32_t st = L->stride;
    uint8_t *d0 = vtx + (size_t)base * st;
    uint8_t *d = d0;
    if (L->pretransformed) {
        for (uint32_t i = 0; i < nv; i++, d += st) {
            float w = v[i].w != 0.0f ? 1.0f / v[i].w : 1.0f;
            float *p = (float *)d;
            p[0] = v[i].x * w;
            p[1] = v[i].y * w;
            p[2] = v[i].z * w;
            p[3] = w;
        }
    } else {
        for (uint32_t i = 0; i < nv; i++, d += st) {
            float *p = (float *)d;
            p[0] = v[i].x;
            p[1] = v[i].y;
            p[2] = v[i].z;
        }
    }
    if (L->use_color) {
        d = d0 + L->off_col;
        for (uint32_t i = 0; i < nv; i++, d += st)
            *(uint32_t *)d = v[i].color;
    }
    if (L->has_normal) {
        d = d0 + L->off_nrm;
        for (uint32_t i = 0; i < nv; i++, d += st) {
            float *n = (float *)d;
            n[0] = v[i].nx;
            n[1] = v[i].ny;
            n[2] = v[i].nz;
        }
    }
    if (L->use_fog) {
        d = d0 + L->off_fog;
        for (uint32_t i = 0; i < nv; i++, d += st)
            *(float *)d = (float)(v[i].spec >> 24) * (1.0f / 255.0f);
    }
    for (int s = 0; s < MAX_STAGES; s++) {
        int k = L->uv_src[s];
        if (k < 0)
            continue;
        d = d0 + L->off_uv[s];
        for (uint32_t i = 0; i < nv; i++, d += st) {
            float *uv = (float *)d;
            uv[0] = v[i].uv[k][0];
            uv[1] = v[i].uv[k][1];
        }
    }
    if (!g_pack_thread)
        pc_frame[PC_PACK_US] += fine_now() - t0;
    PROF_RS_END;
}

static void submit_arrays(int prim, const Layout *L, uint32_t nv, const uint16_t *idx, uint32_t ni,
                          const DrawArrays *A);

/* Per-draw structures are cleared and copied inline (rt_fill_small, rt_copy_small):
 * GCC would call memset and memcpy, which on the Vita are SceLibc imports. */
_Static_assert(sizeof(DrawArrays) % 4u == 0 && sizeof(DrawArrays) <= 128u, "DrawArrays size");
_Static_assert(sizeof(Layout) % 4u == 0 && sizeof(Layout) <= 128u, "Layout size");
_Static_assert(sizeof(Light) % 4u == 0 && sizeof(Light) <= 128u, "Light size");

/* Draws the open storage (vtx) with indices idx (idx_out for batches). */
static void submit(int prim, const Layout *L, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    DrawArrays A;
    rt_fill_small((uint8_t *)&A, 0, sizeof(A));
    A.stride = (int)L->stride;
    uintptr_t b = (uintptr_t)vtx;
#if defined(__vita__)
    if (vtx_gpu) {
        /* The arena buffer objects point at this draw's data (no copy in vitaGL). */
        A.vbo = arena_vbo;
        A.vbo_data = vtx;
        b = 0;
        if (idx && idx == idx_out) {
            A.ibo = arena_ibo;
            A.ibo_data = idx_out;
        }
    }
#endif
    A.pos = (const void *)b;
    A.col = (const void *)(b + L->off_col);
    A.nrm = (const void *)(b + L->off_nrm);
    A.fog = (const void *)(b + L->off_fog);
    for (int s = 0; s < MAX_STAGES; s++)
        A.uv[s] = (const void *)(b + L->off_uv[s]);
    submit_arrays(prim, L, nv, idx, ni, &A);
    stream_end(L, nv, idx == idx_out ? ni : 0u);
}

/* The render state part of a draw command (transforms too unless pre-transformed). */
static void fill_state(DrawCmd *d, const DrawStage *stages, int with_transforms)
{
    const uint32_t *rs = g_rs.rs;
    d->zenable = rs[7];
    d->fillmode = rs[8];
    d->shademode = rs[9];
    d->zwrite = rs[14];
    d->alphatest = rs[15];
    d->srcblend = rs[19];
    d->destblend = rs[20];
    d->cullmode = rs[22];
    d->zfunc = rs[23];
    d->alpharef = rs[24];
    d->alphafunc = rs[25];
    d->blend = rs[27];
    d->fog = rs[28];
    d->fogcolor = rs[34];
    d->fogtablemode = rs[35];
    d->fogstart = rs[36];
    d->fogend = rs[37];
    d->fogdensity = rs[38];
    d->colorkey = rs[41];
    d->tfactor = rs[60];
    d->lighting = rs[137];
    d->fogvertexmode = rs[140];
    d->stage[0] = stages[0];
    d->stage[1] = stages[1];
    if (with_transforms) {
        d->vp_x = g_rs.vp_x;
        d->vp_y = g_rs.vp_y;
        d->vp_w = g_rs.vp_w;
        d->vp_h = g_rs.vp_h;
        d->vp_minz = g_rs.vp_minz;
        d->vp_maxz = g_rs.vp_maxz;
        memcpy(d->world, g_rs.world, sizeof(d->world));
        memcpy(d->view, g_rs.view, sizeof(d->view));
        memcpy(d->proj, g_rs.proj, sizeof(d->proj));
    }
}

/* Render thread: one draw with the snapshot's state, straight to the backend. */
static void submit_arrays_rs(int prim, const Layout *L, uint32_t nv, const uint16_t *idx, uint32_t ni,
                             const DrawArrays *A)
{
    rs_gpu_draws++;
    PROF_RS(PROF_H_STATE);
    DrawCmd *d = &rs_cur.d;
    d->prim = prim;
    d->nv = nv;
    d->ni = ni;
    d->idx = idx;   /* client indices are read during the call */
    rt_copy_small(&d->L, L, sizeof(*L));
    rt_copy_small(&d->A, A, sizeof(*A));
    bk_draw(d);
    /* Sampler state goes with the first draw after the texture's settings changed. */
    d->stage[0].sampler = d->stage[1].sampler = 0;
    rs_cur_used = 1;
    rs_pc[PC_VERTS] += nv;
    rs_pc[PC_INDICES] += idx || A->ibo ? ni : 0u;
    PROF_RS_END;
}

/* Sends one draw to the backend with the state it needs. */
static void submit_arrays(int prim, const Layout *L, uint32_t nv, const uint16_t *idx, uint32_t ni,
                          const DrawArrays *A)
{
    if (g_pack_thread) {
        submit_arrays_rs(prim, L, nv, idx, ni, A);
        return;
    }
    perf_gpu_draws++;
    PROF_SECTION(PROF_H_STATE);
    uint64_t t0 = fine_now();
    uint64_t tex_before = pc_frame[PC_TEX_US];
    /* Everything this records (texture uploads, lighting, settings) goes before the draw. */
    DrawStage stages[MAX_STAGES];
    stages_for_draw(stages);
    const uint32_t *rs = g_rs.rs;
    const int pretransformed = L->pretransformed;
    if (!pretransformed && rs[137] && L->has_normal)
        send_lights();
    send_params();
    if (idx && !A->ibo && rq_on) {
        /* Client indices are read when the render thread draws. */
        uint16_t *copy = rq_data((size_t)ni * sizeof(uint16_t));
        memcpy(copy, idx, (size_t)ni * sizeof(uint16_t));
        idx = copy;
    }
    DrawCmd *d = rq_begin(bk_draw, pretransformed ? offsetof(DrawCmd, vp_x) : sizeof(DrawCmd));
    d->prim = prim;
    d->nv = nv;
    d->ni = ni;
    d->idx = idx;
    rt_copy_small(&d->L, L, sizeof(*L));
    rt_copy_small(&d->A, A, sizeof(*A));
    fill_state(d, stages, !pretransformed);
    uint64_t t1 = fine_now();
    pc_frame[PC_STATE_US] += (t1 - t0) - (pc_frame[PC_TEX_US] - tex_before);
    rq_end();
    pc_frame[PC_GL_US] += fine_now() - t1;
    pc_frame[PC_VERTS] += nv;
    pc_frame[PC_INDICES] += idx || A->ibo ? ni : 0u;
    PROF_SECTION_END;
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
    if (g_pack_thread) {
        /* On the render thread: the state snapshot (texture names, not surfaces). */
        const DrawCmd *d = &rs_cur.d;
        rt_log("draw prim %d nv %u ni %u tl %d n %d c %d s %d t %d box %.1f,%.1f-%.1f,%.1f col0 %08X | z %u/%u "
               "blend %u %u/%u atest %u %u/%u ck %u fog %u light %u cull %u | s0 c%u(%X,%X) a%u(%X,%X) tex %u "
               "| s1 c%u a%u tex %u tf %08X | vp %d,%d %dx%d world %.1f %.1f %.1f",
               prim, nv, ni, pretransformed, has_normal, has_color, has_spec, ntex, (double)x0, (double)y0,
               (double)x1, (double)y1, v[0].color, d->zenable, d->zwrite, d->blend, d->srcblend, d->destblend,
               d->alphatest, d->alphafunc, d->alpharef, d->colorkey, d->fog, d->lighting, d->cullmode,
               d->stage[0].colorop, d->stage[0].colorarg1, d->stage[0].colorarg2, d->stage[0].alphaop,
               d->stage[0].alphaarg1, d->stage[0].alphaarg2, d->stage[0].tex, d->stage[1].colorop,
               d->stage[1].alphaop, d->stage[1].tex, d->tfactor, d->vp_x, d->vp_y, d->vp_w, d->vp_h,
               (double)d->world[12], (double)d->world[13], (double)d->world[14]);
        float fs, fe, fd;
        memcpy(&fs, &d->fogstart, 4);
        memcpy(&fe, &d->fogend, 4);
        memcpy(&fd, &d->fogdensity, 4);
        uint32_t smin = 0xFFu, smax = 0u;
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t a = v[i].spec >> 24;
            smin = a < smin ? a : smin;
            smax = a > smax ? a : smax;
        }
        rt_log("    fog: table %u vertex %u start %.1f end %.1f density %.5f color %08X spec alpha %u..%u",
               d->fogtablemode, d->fogvertexmode, (double)fs, (double)fe, (double)fd, d->fogcolor, smin, smax);
        return;
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
    /* Depth range of the draw: screen z / rhw for pre-transformed vertices, clip z/w and w otherwise. */
    float zmin = 1e30f, zmax = -1e30f, wmin = 1e30f, wmax = -1e30f;
    float m[16];
    mat_mul(g_rs.world, g_rs.view, m);
    mat_mul(m, g_rs.proj, m);
    for (uint32_t i = 0; i < nv; i++) {
        float z = v[i].z, w = v[i].w;
        if (!pretransformed) {
            float cz = v[i].x * m[2] + v[i].y * m[6] + v[i].z * m[10] + m[14];
            w = v[i].x * m[3] + v[i].y * m[7] + v[i].z * m[11] + m[15];
            z = w != 0.0f ? cz / w : 0.0f;
        }
        zmin = z < zmin ? z : zmin;
        zmax = z > zmax ? z : zmax;
        wmin = w < wmin ? w : wmin;
        wmax = w > wmax ? w : wmax;
    }
    rt_log("    depth: zfunc %u z %.6f..%.6f %s %.4f..%.4f range %.3f..%.3f fogcol %08X world %.1f %.1f %.1f tex %08X",
           g_rs.rs[23], (double)zmin, (double)zmax, pretransformed ? "rhw" : "w", (double)wmin, (double)wmax,
           (double)g_rs.vp_minz, (double)g_rs.vp_maxz, g_rs.rs[34], (double)g_rs.world[12], (double)g_rs.world[13],
           (double)g_rs.world[14], g_rs.stage[0].texture ? g_rs.stage[0].texture->mem : 0u);
    if (!pretransformed && nv <= 16) {
        for (uint32_t i = 0; i < nv; i++) {
            float cz = v[i].x * m[2] + v[i].y * m[6] + v[i].z * m[10] + m[14];
            float cw = v[i].x * m[3] + v[i].y * m[7] + v[i].z * m[11] + m[15];
            rt_log("    o%u: %.2f %.2f %.2f clip z %.4f w %.4f", i, (double)v[i].x, (double)v[i].y, (double)v[i].z,
                   (double)cz, (double)cw);
        }
    }
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
    if (cur_trace())
        trace_draw(prim, v, nv, ni, pretransformed, has_normal, has_color, has_spec, ntex);
    Layout L;
    layout_init(&L, pretransformed, has_normal, has_color, has_spec, ntex);
    stream_begin(&L, nv, idx ? ni : 0u);
    pack_vertices(&L, v, nv, 0);
    if (idx) {
        memcpy(idx_out, idx, ni * sizeof(uint16_t));
        idx = idx_out;
    }
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
static uint32_t batch_nv, batch_ni;
static uint32_t batch_serial;
static Layout batch_layout;
static int batch_active;

/* ---- on-screen performance overlay ----------------------------------------
 * Per-frame averages over one second: total frame time, time spent submitting
 * draws to GL (D: render_draw plus batch flushes triggered elsewhere), time
 * blocked in the buffer swap, and the rest (game code), plus the counters in
 * pc_frame. Enabled by default on the Vita; IGI_OVERLAY=1 on desktop.
 */
static struct {
    uint64_t draw_us, swap_us, frame_start;
    uint64_t last_swap_us;   /* the previous buffer swap (part of this frame's time) */
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
    /* State setters flush before changing g_rs (render_state_changing), so the
     * current state is still the batch's. */
    submit(PRIM_TRIANGLES, &batch_layout, batch_nv, idx_out, batch_ni);
    batch_nv = batch_ni = 0;
}

void render_state_changing(void)
{
    /* With the pack thread, recorded draws carry their state: the render thread
     * ends its batch when a draw comes with a new state snapshot. */
    if (!g_pack_thread)
        render_flush();
    g_rs_serial++;
}

/* Draws the batch of the front end (on the thread running it). */
static void flush_local(void)
{
    if (!batch_active)
        return;
    if (in_render_draw || g_pack_thread) {
        flush_batch();
        return;
    }
    /* Flushes from EndScene, Lock, Blt, Present...: still draw submission time. */
    uint64_t t0 = fine_now();
    flush_batch();
    perf.draw_us += fine_now() - t0;
}

static void pk_flush_cmd(const void *unused)
{
    (void)unused;
    flush_local();
}

void render_flush(void)
{
    if (!g_pack_thread) {
        flush_local();
        return;
    }
    /* Whatever is recorded next goes after the draws recorded so far. */
    if (pk_pending) {
        pk_pending = 0;
        rq_exec(pk_flush_cmd, NULL, 0);
    }
}

/* ---- near-plane clipping ---------------------------------------------------
 * Vita3K clamps negative depth to 0 instead of clipping at the near plane, so
 * triangles that pass between the camera and the near plane came out in front
 * of everything (room interiors through walls, floors over floors). Triangles
 * crossing the D3D near plane (clip z = 0) are clipped here on the CPU; draws
 * entirely in front of it pass through untouched.
 */
#if defined(__vita__)
/* Clip-space z of object-space vertices: z = x*c[0] + y*c[1] + z*c[2] + c[3], the
 * third column of world * view * proj. Every near-plane test of a draw needs it
 * and the transforms change between most draws: one shared copy per state
 * serial, and only the 80 products that column needs (the same arithmetic as
 * mat_mul's, so the same values). */
static const float *clip_z_column(void)
{
    static float c[4];
    static uint32_t serial;
    static int valid;
    if (!valid || serial != cur_serial()) {
        const float *w = g_rs.world, *v = g_rs.view, *p = g_rs.proj;
        if (g_pack_thread) {
            w = rs_cur.d.world;
            v = rs_cur.d.view;
            p = rs_cur.d.proj;
        }
        for (int i = 0; i < 4; i++) {
            float wv[4];
            for (int j = 0; j < 4; j++)
                wv[j] = w[i * 4 + 0] * v[0 * 4 + j] + w[i * 4 + 1] * v[1 * 4 + j] + w[i * 4 + 2] * v[2 * 4 + j] +
                        w[i * 4 + 3] * v[3 * 4 + j];
            c[i] = wv[0] * p[0 * 4 + 2] + wv[1] * p[1 * 4 + 2] + wv[2] * p[2 * 4 + 2] + wv[3] * p[3 * 4 + 2];
        }
        serial = cur_serial();
        valid = 1;
    }
    return c;
}

static RVertex nc_lerp(const RVertex *a, const RVertex *b, float t)
{
    RVertex r;
    const float *fa = &a->x, *fb = &b->x;
    float *fr = &r.x;
    for (int i = 0; i < 7; i++)   /* x y z w nx ny nz */
        fr[i] = fa[i] + (fb[i] - fa[i]) * t;
    for (int s = 0; s < MAX_STAGES; s++)
        for (int k = 0; k < 2; k++)
            r.uv[s][k] = a->uv[s][k] + (b->uv[s][k] - a->uv[s][k]) * t;
    uint32_t c = 0, sp = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a->color >> sh) & 0xFFu), cb = (float)((b->color >> sh) & 0xFFu);
        float sa = (float)((a->spec >> sh) & 0xFFu), sb = (float)((b->spec >> sh) & 0xFFu);
        c |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
        sp |= (uint32_t)(sa + (sb - sa) * t + 0.5f) << sh;
    }
    r.color = c;
    r.spec = sp;
    return r;
}

/* Returns 1 when some vertex is behind the near plane, with the clipped triangles
 * as an indexed list: out_idx[0 .. *nidx) index the draw's vertices (below nv) and
 * the new vertices out_v[0 .. *nnew) (from nv on). Triangles in front keep their
 * vertices, so only the edges crossing the plane make new ones. */
static int near_clip_impl(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni, RVertex **out_v,
                          uint32_t *nnew, const uint32_t **out_idx, uint32_t *nidx);

static int near_clip(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni, RVertex **out_v,
                     uint32_t *nnew, const uint32_t **out_idx, uint32_t *nidx)
{
    PROF_RS(PROF_H_NEARCLIP);
    int r = near_clip_impl(prim, v, nv, idx, ni, out_v, nnew, out_idx, nidx);
    PROF_RS_END;
    return r;
}

static int near_clip_impl(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni, RVertex **out_v,
                          uint32_t *nnew, const uint32_t **out_idx, uint32_t *nidx)
{
    static float *d;
    static uint8_t *fr;   /* 1: vertex in front of the plane (d >= 0) */
    static uint32_t d_cap;
    static RVertex *ob;
    static uint32_t *oi;
    static uint32_t ob_cap, oi_cap;
    if (nv > d_cap) {
        d_cap = nv + 1024u;
        d = realloc(d, d_cap * sizeof(float));
        fr = realloc(fr, d_cap);
    }
    const float *m = clip_z_column();
    int any = 0;
    for (uint32_t i = 0; i < nv; i++) {
        d[i] = v[i].x * m[0] + v[i].y * m[1] + v[i].z * m[2] + m[3];
        fr[i] = d[i] >= 0.0f;
        any |= d[i] < 0.0f;
    }
    if (!any)
        return 0;
    uint32_t n = idx ? ni : nv;
    uint32_t tris = prim == PRIM_TRIANGLES ? n / 3u : (n >= 3u ? n - 2u : 0u);
    if (tris * 2u > ob_cap) {   /* a clipped triangle makes at most 2 new vertices */
        ob_cap = tris * 2u + 1024u;
        ob = realloc(ob, ob_cap * sizeof(RVertex));
    }
    if (tris * 6u > oi_cap) {   /* and at most 2 triangles */
        oi_cap = tris * 6u + 1024u;
        oi = realloc(oi, oi_cap * sizeof(uint32_t));
    }
    uint32_t no = 0, nn = 0;
    for (uint32_t t = 0; t < tris; t++) {
        uint32_t k[3];
        if (prim == PRIM_TRIANGLES) {
            k[0] = t * 3u; k[1] = t * 3u + 1u; k[2] = t * 3u + 2u;
        } else if (prim == PRIM_TRIANGLE_STRIP) {
            k[0] = t; k[1] = (t & 1u) ? t + 2u : t + 1u; k[2] = (t & 1u) ? t + 1u : t + 2u;
        } else {
            k[0] = 0; k[1] = t + 1u; k[2] = t + 2u;
        }
        uint32_t vi[3];
        for (int j = 0; j < 3; j++) {
            vi[j] = idx ? idx[k[j]] : k[j];
            if (vi[j] >= nv)
                vi[j] = nv - 1u;
        }
        uint32_t f = fr[vi[0]] | (uint32_t)fr[vi[1]] << 1 | (uint32_t)fr[vi[2]] << 2;
        if (f == 7u) {          /* in front: as is */
            oi[no++] = vi[0];
            oi[no++] = vi[1];
            oi[no++] = vi[2];
            continue;
        }
        if (f == 0u)            /* behind */
            continue;
        uint32_t poly[4];
        int np = 0;
        for (int j = 0; j < 3; j++) {
            uint32_t a = vi[j], b = vi[(j + 1) % 3];
            if (fr[a])
                poly[np++] = a;
            if (fr[a] != fr[b]) {
                float da = d[a], db = d[b];
                ob[nn] = nc_lerp(&v[a], &v[b], da / (da - db));
                poly[np++] = nv + nn++;
            }
        }
        for (int j = 1; j + 1 < np; j++) {
            oi[no++] = poly[0];
            oi[no++] = poly[j];
            oi[no++] = poly[j + 1];
        }
    }
    *out_v = ob;
    *nnew = nn;
    *out_idx = oi;
    *nidx = no;
    return 1;
}
#endif

static void batch_draw_impl(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                            int pretransformed, int has_normal, int has_color, int has_spec, int ntex);

static float bounds_lo[3], bounds_hi[3];
static int bounds_valid;

void render_set_bounds(const float *lo, const float *hi)
{
    bounds_valid = lo && hi;
    if (bounds_valid) {
        memcpy(bounds_lo, lo, sizeof(bounds_lo));
        memcpy(bounds_hi, hi, sizeof(bounds_hi));
    }
}

#if defined(__vita__)
/* True when the whole bounding box is in front of the near plane. */
static int bounds_in_front(void)
{
    if (!bounds_valid)
        return 0;
    const float *m = clip_z_column();
    for (int c = 0; c < 8; c++) {
        float x = (c & 1) ? bounds_hi[0] : bounds_lo[0], y = (c & 2) ? bounds_hi[1] : bounds_lo[1];
        float z = (c & 4) ? bounds_hi[2] : bounds_lo[2];
        if (x * m[0] + y * m[1] + z * m[2] + m[3] < 0.0f)
            return 0;
    }
    return 1;
}
#endif

/* Appends `tris` triangles of a list/strip/fan (vertex numbers from `idx` if
 * set), offset by `base`, to the batch index list. */
static void append_indices(int prim, const uint16_t *idx, uint32_t tris, uint32_t base)
{
    uint16_t *out = &idx_out[batch_ni];
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

static void batch_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                       int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    PROF_RS(PROF_H_BATCH);
    batch_draw_impl(prim, v, nv, idx, ni, pretransformed, has_normal, has_color, has_spec, ntex);
    PROF_RS_END;
}

static void batch_draw_impl(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                            int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    if (!nv)
        return;
    if (pretransformed && rhw_invisible(prim, v, nv))
        return;
    if ((cur_diag() == DIAG_NO_SCREEN && pretransformed) || (cur_diag() == DIAG_NO_3D && !pretransformed))
        return;
    if (batch_enabled < 0) {
        const char *e = getenv("IGI_BATCH");
        batch_enabled = !(e && e[0] == '0');
    }
    /* Vertices made by near-plane clipping, drawn after the draw's own (nex of them). */
    const RVertex *ex = NULL;
    uint32_t nex = 0;
#if defined(__vita__)
    if (!pretransformed && cur_diag() != DIAG_NO_NEARCLIP && !bounds_in_front() &&
        (prim == PRIM_TRIANGLES || prim == PRIM_TRIANGLE_STRIP || prim == PRIM_TRIANGLE_FAN)) {
        RVertex *cv;
        const uint32_t *ci;
        uint32_t cn, cni;
        if (near_clip(prim, v, nv, idx, ni, &cv, &cn, &ci, &cni)) {
            if (!cni)
                return;
            if (batch_enabled && cur_diag() != DIAG_NO_BATCH && nv + cn <= BATCH_MAX_V && cni <= BATCH_MAX_I &&
                !cur_trace()) {
                /* Indexed: the draw's vertices plus the new ones. */
                static uint16_t *ci16;
                static uint32_t ci16_cap;
                if (cni > ci16_cap) {
                    ci16_cap = cni + 1024u;
                    ci16 = realloc(ci16, ci16_cap * sizeof(uint16_t));
                }
                for (uint32_t i = 0; i < cni; i++)
                    ci16[i] = (uint16_t)ci[i];
                ex = cv;
                nex = cn;
                idx = ci16;
                ni = cni;
            } else {
                /* A plain triangle list, as the clipper's output used to be. */
                static RVertex *flat;
                static uint32_t flat_cap;
                if (cni > flat_cap) {
                    flat_cap = cni + 1024u;
                    flat = realloc(flat, flat_cap * sizeof(RVertex));
                }
                for (uint32_t i = 0; i < cni; i++)
                    flat[i] = ci[i] < nv ? v[ci[i]] : cv[ci[i] - nv];
                v = flat;
                nv = cni;
                idx = NULL;
                ni = 0;
            }
            prim = PRIM_TRIANGLES;
        }
    }
#endif
    uint32_t n = idx ? ni : nv;
    int batchable = batch_enabled && cur_diag() != DIAG_NO_BATCH &&
                    (prim == PRIM_TRIANGLES || prim == PRIM_TRIANGLE_STRIP || prim == PRIM_TRIANGLE_FAN);
    uint32_t tris = prim == PRIM_TRIANGLES ? n / 3u : (n >= 3u ? n - 2u : 0u);
    if (!nex && (!batchable || nv > BATCH_MAX_V || tris * 3u > BATCH_MAX_I || cur_trace())) {
        flush_local();
        draw_now(prim, v, nv, idx, ni, pretransformed, has_normal, has_color, has_spec, ntex);
        return;
    }
    if (!tris)
        return;
    const Layout *bl = &batch_layout;
    if (batch_active &&
        (batch_nv + nv + nex > vtx_cap || batch_ni + tris * 3u > idx_cap || bl->pretransformed != pretransformed ||
         bl->has_normal != has_normal || bl->has_color != has_color || bl->has_spec != has_spec || bl->ntex != ntex ||
         cur_serial() != batch_serial))
        flush_local();
    if (!batch_active) {
        batch_active = 1;
        batch_serial = cur_serial();
        layout_init(&batch_layout, pretransformed, has_normal, has_color, has_spec, ntex);
        stream_begin(&batch_layout, nv + nex, tris * 3u);
        batch_nv = batch_ni = 0;
    }
    /* Same state as the batch start, so the layout (active stages, uv sets) holds. */
    uint32_t base = batch_nv;
    pack_vertices(&batch_layout, v, nv, base);
    if (nex)
        pack_vertices(&batch_layout, ex, nex, base + nv);
    batch_nv += nv + nex;
    append_indices(prim, idx, tris, base);
}

/* ---- static vertex buffers in GPU memory ----------------------------------
 * Vertex buffers the game leaves unchanged are uploaded once to a GL buffer
 * object with every attribute they have; their draws then only send indices
 * (no decode, packing, index scan or per-draw vertex copy in vitaGL). Draws
 * that could cross the near plane still take the CPU path (near_clip).
 */
typedef struct VBGL {
    GLuint buf;
    uint32_t gen, count;
    int has_normal, has_color, has_spec, ntex;
    size_t off_col, off_nrm, off_fog, off_uv[MAX_STAGES];
    float lo[3], hi[3];   /* object-space bounds */
} VBGL;

void render_vb_release(void **slot)
{
    VBGL *g = *slot;
    if (!g)
        return;
    render_flush();
    GLuint b = g->buf;
    glDeleteBuffers(1, &b);
    free(g);
    *slot = NULL;
}

static VBGL *vb_upload(void **slot, uint32_t gen, const RVertex *v, uint32_t count, int has_normal, int has_color,
                       int has_spec, int ntex)
{
    VBGL *g = *slot;
    if (g && g->gen == gen && g->count == count && g->has_normal == has_normal && g->has_color == has_color &&
        g->has_spec == has_spec && g->ntex == ntex)
        return g;
    if (!g) {
        g = calloc(1, sizeof(*g));
        GLuint b;
        glGenBuffers(1, &b);
        g->buf = b;
        *slot = g;
    }
    g->gen = gen;
    g->count = count;
    g->has_normal = has_normal;
    g->has_color = has_color;
    g->has_spec = has_spec;
    g->ntex = ntex;
    size_t n = count, o = n * 12u;
    g->off_col = o;
    o += has_color ? n * 4u : 0u;
    g->off_nrm = o;
    o += has_normal ? n * 12u : 0u;
    g->off_fog = o;
    o += has_spec ? n * 4u : 0u;
    for (int s = 0; s < MAX_STAGES; s++) {
        g->off_uv[s] = o;
        o += s < ntex ? n * 8u : 0u;
    }
    uint8_t *data = malloc(o);
    float *pos = (float *)data;
    for (int k = 0; k < 3; k++) {
        g->lo[k] = 1e30f;
        g->hi[k] = -1e30f;
    }
    for (uint32_t i = 0; i < count; i++) {
        const float *p = &v[i].x;
        for (int k = 0; k < 3; k++) {
            pos[i * 3u + (uint32_t)k] = p[k];
            g->lo[k] = p[k] < g->lo[k] ? p[k] : g->lo[k];
            g->hi[k] = p[k] > g->hi[k] ? p[k] : g->hi[k];
        }
        if (has_color)
            memcpy(data + g->off_col + i * 4u, &v[i].color, 4);
        if (has_normal)
            memcpy(data + g->off_nrm + i * 12u, &v[i].nx, 12);
        if (has_spec) {
            float f = (float)(v[i].spec >> 24) * (1.0f / 255.0f);
            memcpy(data + g->off_fog + i * 4u, &f, 4);
        }
        for (int s = 0; s < MAX_STAGES && s < ntex; s++)
            memcpy(data + g->off_uv[s] + i * 8u, v[i].uv[s], 8);
    }
    glBindBuffer(GL_ARRAY_BUFFER, g->buf);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)o, data, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    free(data);
    PCR[PC_TEX_BYTES] += o;
    return g;
}

#if defined(__vita__)
/* True when every corner of the bounds is in front of the near plane (clip z >= 0). */
static int vb_in_front(const VBGL *g)
{
    const float *m = clip_z_column();
    for (int c = 0; c < 8; c++) {
        float x = (c & 1) ? g->hi[0] : g->lo[0], y = (c & 2) ? g->hi[1] : g->lo[1], z = (c & 4) ? g->hi[2] : g->lo[2];
        if (x * m[0] + y * m[1] + z * m[2] + m[3] < 0.0f)
            return 0;
    }
    return 1;
}
#endif

int render_draw_vb(void **slot, uint32_t gen, const RVertex *all, uint32_t count, uint32_t start, int prim,
                   uint32_t nv, const uint16_t *idx, uint32_t ni, int has_normal, int has_color, int has_spec,
                   int ntex)
{
    /* Off by default: unbatched buffer-object draws raised the GPU draw count
     * and cost more in vitaGL than the packing they saved (diagnostic mode 13).
     * Not with the pack thread (the game thread would draw past its batches). */
    if (g_pack_thread || cur_diag() != DIAG_VBO || cur_diag() == DIAG_ZERO_UV || cur_diag() == DIAG_NO_3D ||
        cur_diag() == DIAG_NO_COMPACT || cur_trace() || !nv || start + nv > count)
        return 0;
    if (prim != PRIM_TRIANGLES && prim != PRIM_TRIANGLE_STRIP && prim != PRIM_TRIANGLE_FAN)
        return 0;
    if (in_render_draw)
        return 0;
    PROF_RS(PROF_H_TEXTURE);
    VBGL *g = vb_upload(slot, gen, all, count, has_normal, has_color, has_spec, ntex);
    PROF_RS_END;
#if defined(__vita__)
    if (cur_diag() != DIAG_NO_NEARCLIP && !vb_in_front(g))
        return 0;
#endif
    Layout L;
    layout_init(&L, 0, has_normal, has_color, has_spec, ntex);
    L.use_color = has_color;
#if defined(__vita__)
    if (L.use_fog)
        return 0;   /* the fog coordinate must follow uv0 (layout_init): not in these arrays */
#endif
    flush_local();
    uint64_t t0 = fine_now_fe();
    perf.draws++;
    DrawArrays A;
    rt_fill_small((uint8_t *)&A, 0, sizeof(A));
    A.vbo = g->buf;
    A.pos = (const void *)(uintptr_t)(start * 12u);
    A.col = (const void *)(g->off_col + start * 4u);
    A.nrm = (const void *)(g->off_nrm + start * 12u);
    A.fog = (const void *)(g->off_fog + start * 4u);
    for (int s = 0; s < MAX_STAGES; s++) {
        int k = L.uv_src[s] < 0 ? 0 : L.uv_src[s];
        A.uv[s] = (const void *)(g->off_uv[k] + start * 8u);
    }
    static uint16_t *ib;
    static uint32_t ib_cap;
    const uint16_t *ip = idx;
    if (idx && ((uintptr_t)idx & 1u)) {
        if (ni > ib_cap) {
            ib_cap = ni + 1024u;
            ib = realloc(ib, ib_cap * sizeof(uint16_t));
        }
        memcpy(ib, idx, ni * sizeof(uint16_t));
        ip = ib;
    }
    submit_arrays(prim, &L, nv, ip, ni, &A);
    perf.draw_us += fine_now_fe() - t0;
    return 1;
}

/* ---- guest-layout fast path ------------------------------------------------ */

static uint32_t d3dcolor_rgba(uint32_t argb)
{
    return (argb & 0xFF00FF00u) | ((argb & 0xFFu) << 16) | ((argb >> 16) & 0xFFu);
}

/* Same as pack_vertices, reading the game's vertex layout directly, into d0. */
static void pack_guest_to(uint8_t *d0, const Layout *L, const GuestVerts *g, uint32_t nv)
{
    PROF_RS(PROF_H_PACK);
    const uint8_t *vb = g->base;
    const uint32_t stride = g->stride;
    const uint32_t *src = g->src;
    const uint32_t st = L->stride;
    uint8_t *d = d0;
    /* The terrain's layout (pre-transformed, colour, desktop fog factor, one
     * texture coordinate set): one pass over the vertices. */
    const uint32_t fog = L->use_fog ? 1u : 0u;
    /* Fog factor before uv0 (desktop) or after it (Vita): word indices. */
    const uint32_t fi = L->off_fog / 4u, ui = L->off_uv[0] / 4u;
    int one_uv = L->uv_src[0] == 0 && g->uv_bytes[0] == 8u && (fog ? (fi == 5u && ui == 6u) || (ui == 5u && fi == 7u) : ui == 5u);
    for (int s = 1; s < MAX_STAGES; s++)
        one_uv &= L->uv_src[s] < 0;
    if (L->pretransformed && !src && L->use_color && g->has_color && !L->has_normal && L->off_col == 16u &&
        (!fog || g->has_spec) && st == 28u + 4u * fog && one_uv &&
        (((uintptr_t)vb | stride | g->uv_off[0]) & 3u) == 0) {
        const uint32_t col = g->col_off, spec = g->spec_off, uv = g->uv_off[0];
        float *p = (float *)d;
        for (uint32_t i = 0; i < nv; i++, p += 7u + fog, vb += stride) {
            uint32_t c;
            memcpy(&c, vb + col, 4);
            /* Word-aligned here: direct float loads (memcpy into an array went
             * through the stack, which cost more than the division). */
            const float *v = (const float *)(const void *)vb, *t = (const float *)(const void *)(vb + uv);
            float w = v[3] != 0.0f ? 1.0f / v[3] : 1.0f;
            p[0] = v[0] * w;
            p[1] = v[1] * w;
            p[2] = v[2] * w;
            p[3] = w;
            ((uint32_t *)p)[4] = d3dcolor_rgba(c);
            if (fog) {
                uint32_t s;
                memcpy(&s, vb + spec, 4);
                p[fi] = (float)(s >> 24) * (1.0f / 255.0f);
            }
            p[ui] = t[0];
            p[ui + 1u] = t[1];
        }
        PROF_RS_END;
        return;
    }
    if (L->pretransformed) {
        for (uint32_t i = 0; i < nv; i++, d += st) {
            float v[4];
            memcpy(v, vb + (size_t)(src ? src[i] : i) * stride, 16);
            float w = v[3] != 0.0f ? 1.0f / v[3] : 1.0f;
            float *p = (float *)d;
            p[0] = v[0] * w;
            p[1] = v[1] * w;
            p[2] = v[2] * w;
            p[3] = w;
        }
    } else {
        for (uint32_t i = 0; i < nv; i++, d += st)
            memcpy(d, vb + (size_t)(src ? src[i] : i) * stride, 12);
    }
    if (L->use_color) {
        const uint32_t off = g->col_off;
        d = d0 + L->off_col;
        if (!g->has_color) {   /* pre-transformed without diffuse: white */
            for (uint32_t i = 0; i < nv; i++, d += st)
                *(uint32_t *)d = 0xFFFFFFFFu;
        } else {
            for (uint32_t i = 0; i < nv; i++, d += st) {
                uint32_t v;
                memcpy(&v, vb + (size_t)(src ? src[i] : i) * stride + off, 4);
                *(uint32_t *)d = d3dcolor_rgba(v);
            }
        }
    }
    if (L->has_normal) {
        const uint32_t off = g->nrm_off;
        d = d0 + L->off_nrm;
        for (uint32_t i = 0; i < nv; i++, d += st)
            memcpy(d, vb + (size_t)(src ? src[i] : i) * stride + off, 12);
    }
    if (L->use_fog) {
        const uint32_t off = g->spec_off;
        d = d0 + L->off_fog;
        for (uint32_t i = 0; i < nv; i++, d += st) {
            uint32_t v;
            memcpy(&v, vb + (size_t)(src ? src[i] : i) * stride + off, 4);
            *(float *)d = (float)(v >> 24) * (1.0f / 255.0f);
        }
    }
    for (int s = 0; s < MAX_STAGES; s++) {
        int k = L->uv_src[s];
        if (k < 0)
            continue;
        const uint32_t off = g->uv_off[k];
        d = d0 + L->off_uv[s];
        if (g->uv_bytes[k] == 8u) {
            for (uint32_t i = 0; i < nv; i++, d += st)
                memcpy(d, vb + (size_t)(src ? src[i] : i) * stride + off, 8);
        } else {
            for (uint32_t i = 0; i < nv; i++, d += st) {
                memcpy(d, vb + (size_t)(src ? src[i] : i) * stride + off, 4);
                ((float *)d)[1] = 0.0f;
            }
        }
    }
    PROF_RS_END;
}

static void pack_guest(const Layout *L, const GuestVerts *g, uint32_t nv, uint32_t base)
{
    pack_guest_to(vtx + (size_t)base * L->stride, L, g, nv);
}

int render_draw_guest(int prim, const GuestVerts *g, uint32_t nv, const uint16_t *idx, uint32_t ni)
{
    if (!nv || cur_trace() || cur_diag() == DIAG_NO_3D || cur_diag() == DIAG_NO_BATCH ||
        cur_diag() == DIAG_ZERO_UV)
        return 0;
    if (prim != PRIM_TRIANGLES && prim != PRIM_TRIANGLE_STRIP && prim != PRIM_TRIANGLE_FAN)
        return 0;
    uint32_t n = idx ? ni : nv;
    uint32_t tris = prim == PRIM_TRIANGLES ? n / 3u : (n >= 3u ? n - 2u : 0u);
    if (nv > BATCH_MAX_V || tris * 3u > BATCH_MAX_I)
        return 0;
    if (batch_enabled < 0) {
        const char *e = getenv("IGI_BATCH");
        batch_enabled = !(e && e[0] == '0');
    }
    if (!batch_enabled)
        return 0;
    if (g->pretransformed) {
        if (cur_diag() == DIAG_NO_SCREEN)
            return 1;
        /* rhw_invisible: all rhw ~0 means the primitive is fully transparent. */
        float max_rhw = 0.0f;
        for (uint32_t i = 0; i < nv; i++) {
            float w;
            memcpy(&w, g->base + (size_t)(g->src ? g->src[i] : i) * g->stride + 12u, 4);
            max_rhw = w > max_rhw ? w : max_rhw;
        }
        if (max_rhw < 1e-6f)
            return 1;
    }
#if defined(__vita__)
    if (!g->pretransformed && cur_diag() != DIAG_NO_NEARCLIP) {
        /* Any vertex behind the near plane: the general path clips on the CPU. */
        PROF_RS(PROF_H_NEARCLIP);
        const float *m = clip_z_column();
        int behind = 0;
        for (uint32_t i = 0; i < nv; i++) {
            float x[3];
            memcpy(x, g->base + (size_t)(g->src ? g->src[i] : i) * g->stride, 12);
            behind |= x[0] * m[0] + x[1] * m[1] + x[2] * m[2] + m[3] < 0.0f;
        }
        PROF_RS_END;
        if (behind)
            return 0;
    }
#endif
    uint64_t t0 = fine_now_fe();
    PROF_RS(PROF_H_BATCH);
    if (tris) {
        const Layout *bl = &batch_layout;
        if (batch_active &&
            (batch_nv + nv > vtx_cap || batch_ni + tris * 3u > idx_cap ||
             bl->pretransformed != g->pretransformed ||
             bl->has_normal != g->has_normal || bl->has_color != g->has_color || bl->has_spec != g->has_spec ||
             bl->ntex != g->ntex || cur_serial() != batch_serial))
            flush_local();
        if (!batch_active) {
            batch_active = 1;
            batch_serial = cur_serial();
            layout_init(&batch_layout, g->pretransformed, g->has_normal, g->has_color, g->has_spec, g->ntex);
            stream_begin(&batch_layout, nv, tris * 3u);
            batch_nv = batch_ni = 0;
        }
        uint32_t base = batch_nv;
        pack_guest(&batch_layout, g, nv, base);
        batch_nv += nv;
        append_indices(prim, idx, tris, base);
    }
    PROF_RS_END;
    if (!g_pack_thread) {   /* the game thread counts recorded draws */
        perf.draw_us += fine_now() - t0;
        perf.draws++;
    }
    return 1;
}

/* ---- pre-transformed vertex buffers drawn several times per lock -----------
 * The terrain fills one pre-transformed vertex buffer per frame and draws it
 * with one indexed draw per texture batch, each referencing vertices spread
 * over the whole buffer. Rather than scanning, compacting and packing the
 * referenced vertices for every draw, the range is packed once per lock (and
 * layout) and every draw only copies its indices. */
static struct {
    uint32_t lock_id, start, nv, frame;
    Layout L;
    uint8_t *data;
    int valid;
} dyn;
#if !defined(__vita__)
static uint8_t *dyn_heap;
static size_t dyn_heap_bytes;
static uint16_t *dyn_idx_heap;
static uint32_t dyn_idx_cap;
#endif

static int layout_equal(const Layout *a, const Layout *b)
{
    const uint32_t *x = (const uint32_t *)(const void *)a, *y = (const uint32_t *)(const void *)b;
    uint32_t d = 0;
    for (size_t i = 0; i < sizeof(Layout) / 4u; i++)
        d |= x[i] ^ y[i];
    return d == 0;
}

/* Copies n indices and returns the largest. */
static uint32_t copy_indices_max(uint16_t *d, const uint16_t *s, uint32_t n)
{
    uint32_t i = 0, mx = 0;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    uint16x8_t m = vdupq_n_u16(0);
    for (; i + 8u <= n; i += 8u) {
        uint16x8_t v = vld1q_u16(s + i);
        vst1q_u16(d + i, v);
        m = vmaxq_u16(m, v);
    }
    uint16x4_t m4 = vmax_u16(vget_low_u16(m), vget_high_u16(m));
    m4 = vpmax_u16(m4, m4);
    m4 = vpmax_u16(m4, m4);
    mx = vget_lane_u16(m4, 0);
#endif
    for (; i < n; i++) {
        d[i] = s[i];
        mx = s[i] > mx ? s[i] : mx;
    }
    return mx;
}

/* Packs the range for this lock and layout unless that is already done; returns 0
 * when there is no room. Vertices no draw references may hold anything. */
static int dyn_prepare(uint32_t lock_id, uint32_t start, const GuestVerts *g, uint32_t nv, const Layout *L)
{
    uint32_t frame = g_pack_thread ? rs_frame : g_frame_serial;
#if defined(__vita__)
    if (!arena_ready())
        return 0;
    frame = arena_frame;
#endif
    if (dyn.valid && dyn.lock_id == lock_id && dyn.nv == nv && dyn.frame == frame && dyn.start == start &&
        layout_equal(&dyn.L, L))
        return 1;
    dyn.valid = 0;
#if defined(__vita__)
    uint32_t vpos = (arena_vpos + 15u) & ~15u;
    if (vpos > ARENA_VTX_BYTES || (ARENA_VTX_BYTES - vpos) / L->stride < nv)
        return 0;
    dyn.data = arena_vtx[arena_seg] + vpos;
    arena_vpos = vpos + nv * L->stride;
#else
    if (streams_recorded()) {
        dyn.data = rq_data((size_t)nv * L->stride);
    } else {
        if ((size_t)nv * L->stride > dyn_heap_bytes) {
            dyn_heap_bytes = (size_t)nv * L->stride;
            free(dyn_heap);
            dyn_heap = malloc(dyn_heap_bytes);
        }
        dyn.data = dyn_heap;
    }
#endif
    pack_guest_to(dyn.data, L, g, nv);
    PCR[PC_VERTS] += nv;
    dyn.lock_id = lock_id;
    dyn.start = start;
    dyn.nv = nv;
    dyn.frame = frame;
    rt_copy_small(&dyn.L, L, sizeof(*L));
    dyn.valid = 1;
    return 1;
}

int render_draw_dynvb(uint32_t lock_id, uint32_t start, const GuestVerts *g, uint32_t nv, int prim,
                      const uint16_t *idx, uint32_t ni)
{
    if (!nv || !ni || nv > 65536u || prim != PRIM_TRIANGLES || !g->pretransformed || g->src ||
        ((uintptr_t)idx & 1u) || cur_trace() || cur_diag() != DIAG_NONE)
        return 0;
    if (batch_enabled < 0) {
        const char *e = getenv("IGI_BATCH");
        batch_enabled = !(e && e[0] == '0');
    }
    if (!batch_enabled || ni < 3u)
        return 0;
    /* rhw_invisible drops a draw whose vertices all have rhw below 1e-6: one
     * vertex of the first triangle above it shows the draw is kept, otherwise
     * the general path decides. */
    int visible = 0;
    for (int k = 0; k < 3; k++) {
        float w;
        if (idx[k] >= nv)
            return 0;
        memcpy(&w, g->base + (size_t)idx[k] * g->stride + 12u, 4);
        visible |= w >= 1e-6f;
    }
    if (!visible)
        return 0;
    /* Whatever is batched comes first, and its storage must be closed before
     * the arena is used here. */
    flush_local();
    uint64_t t0 = fine_now_fe();
    PROF_RS(PROF_H_BATCH);
    Layout L;
    layout_init(&L, 1, g->has_normal, g->has_color, g->has_spec, g->ntex);
    if (!dyn_prepare(lock_id, start, g, nv, &L)) {
        PROF_RS_END;
        return 0;
    }
    uint16_t *ip;
#if defined(__vita__)
    uint32_t ibytes = (ni * 2u + 3u) & ~3u;
    if (ibytes > ARENA_IDX_BYTES - arena_ipos) {
        PROF_RS_END;
        return 0;
    }
    ip = arena_idx[arena_seg] + arena_ipos / 2u;
#else
    if (ni > dyn_idx_cap) {
        dyn_idx_cap = ni + 4096u;
        free(dyn_idx_heap);
        dyn_idx_heap = malloc(dyn_idx_cap * sizeof(uint16_t));
    }
    ip = dyn_idx_heap;
#endif
    uint32_t tris = ni / 3u;
    if (!tris || copy_indices_max(ip, idx, tris * 3u) >= nv) {
        PROF_RS_END;
        return 0;   /* the general path clamps out-of-range indices */
    }
    DrawArrays A;
    rt_fill_small((uint8_t *)&A, 0, sizeof(A));
    A.stride = (int)L.stride;
    uintptr_t b = (uintptr_t)dyn.data;
#if defined(__vita__)
    arena_ipos += ibytes;
    A.vbo = arena_vbo;
    A.ibo = arena_ibo;
    A.vbo_data = dyn.data;
    A.ibo_data = ip;
    b = 0;
#endif
    A.pos = (const void *)b;
    A.col = (const void *)(b + L.off_col);
    A.nrm = (const void *)(b + L.off_nrm);
    A.fog = (const void *)(b + L.off_fog);
    for (int s = 0; s < MAX_STAGES; s++)
        A.uv[s] = (const void *)(b + L.off_uv[s]);
    submit_arrays(PRIM_TRIANGLES, &L, nv, ip, tris * 3u, &A);
    PCR[PC_VERTS] -= nv;   /* counted once, when packed */
    PROF_RS_END;
    if (!g_pack_thread) {   /* the game thread counts recorded draws */
        perf.draw_us += fine_now() - t0;
        perf.draws++;
    }
    return 1;
}

void render_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                 int pretransformed, int has_normal, int has_color, int has_spec, int ntex)
{
    uint64_t t0 = fine_now_fe();
    in_render_draw = 1;
    batch_draw(prim, v, nv, idx, ni, pretransformed, has_normal, has_color, has_spec, ntex);
    in_render_draw = 0;
    bounds_valid = 0;
    if (!g_pack_thread) {   /* the game thread counts recorded draws */
        perf.draw_us += fine_now() - t0;
        perf.draws++;
    }
}

/* ---- pack thread: state snapshots and frame ends -------------------------- */

/* Render thread: the state for the draws recorded next. */
static void pk_state_cmd(const void *p)
{
    flush_local();
    /* A texture's new sampler settings go with the first draw of the snapshot
     * that found them changed: if none of its draws reached GL, send them now. */
    if (!rs_cur_used)
        for (int i = 0; i < MAX_STAGES; i++)
            if (rs_cur.d.stage[i].tex && rs_cur.d.stage[i].sampler)
                bk_sampler(&rs_cur.d.stage[i]);
    memcpy(&rs_cur, p, sizeof(rs_cur));
    rs_cur_used = 0;
}

/* Render thread: the frame's draws are done (the arena moves on to the next
 * segment); its counters go to the game thread. */
static void pk_frame_cmd(const void *unused)
{
    (void)unused;
    flush_local();
    rs_frame++;
    atomic_fetch_add(&rs_pub_gpu_draws, rs_gpu_draws);
    atomic_fetch_add(&rs_pub_verts, (uint32_t)rs_pc[PC_VERTS]);
    atomic_fetch_add(&rs_pub_indices, (uint32_t)rs_pc[PC_INDICES]);
    atomic_fetch_add(&rs_pub_pack_us, (uint32_t)rs_pc[PC_PACK_US]);
    rs_gpu_draws = 0;
    memset(rs_pc, 0, sizeof(rs_pc));
}

/* Game thread: the counters of the frames the render thread finished. */
static void pk_collect(void)
{
    perf_gpu_draws += atomic_exchange(&rs_pub_gpu_draws, 0);
    pc_frame[PC_VERTS] += atomic_exchange(&rs_pub_verts, 0);
    pc_frame[PC_INDICES] += atomic_exchange(&rs_pub_indices, 0);
    pc_frame[PC_PACK_US] += atomic_exchange(&rs_pub_pack_us, 0);
}

/* A texture of the current stages needs an upload (texture_get would convert it). */
static int stage_textures_dirty(void)
{
    int active = 0;
    for (int i = 0; i < MAX_STAGES; i++) {
        const StageState *st = &g_rs.stage[i];
        if (st->colorop == 1u || (i > 0 && active < i) || !st->texture)
            continue;
        active++;
        Surface *s = st->texture;
        if (s->clean_epoch == g_tex_epoch)
            continue;
        if (!s->gl_tex || (diag_mode != DIAG_LEVEL0 && s->priv[0] == 0 && next_mip(s)))
            return 1;
        for (const Surface *m = s; m; m = next_mip(m))
            if (m->dirty)
                return 1;
        s->clean_epoch = g_tex_epoch;
    }
    return 0;
}

/* Partial snapshots: only the parts of the state that changed (most draws change
 * the world matrix and little else). */
#define RS_WORDS 22u   /* DrawCmd zenable .. tfactor */
_Static_assert(offsetof(DrawCmd, tfactor) - offsetof(DrawCmd, zenable) == (RS_WORDS - 1u) * 4u,
               "DrawCmd render states must be contiguous");

typedef struct PkRs {
    uint32_t serial;
    uint32_t fog, fogtable, fogvertex;   /* LayoutIn */
    uint32_t v[RS_WORDS];
} PkRs;

typedef struct PkStages {
    uint32_t serial;
    int stages;
    uint32_t tci[MAX_STAGES];
    DrawStage st[MAX_STAGES];
} PkStages;

typedef struct PkXf {
    uint32_t serial;
    int32_t vp_x, vp_y, vp_w, vp_h;
    float vp_minz, vp_maxz;
    float world[16], view[16], proj[16];
} PkXf;

typedef struct PkWorld {
    uint32_t serial;
    float world[16];
} PkWorld;

typedef struct Mat4f {
    float m[16];
} Mat4f;

static void mat4_copy(float *d, const float *s)
{
    *(Mat4f *)(void *)d = *(const Mat4f *)(const void *)s;
}

/* Render thread: a new state serial (the batch so far is drawn with the old state). */
static void rs_new_serial(uint32_t serial)
{
    flush_local();
    rs_cur.serial = serial;
}

static void pk_serial_cmd(const void *p)
{
    rs_new_serial(*(const uint32_t *)p);
}

static void pk_rs_cmd(const void *p)
{
    const PkRs *r = p;
    rs_new_serial(r->serial);
    rs_cur.li.fog = r->fog;
    rs_cur.li.fogtable = r->fogtable;
    rs_cur.li.fogvertex = r->fogvertex;
    uint32_t *d = &rs_cur.d.zenable;
    for (uint32_t i = 0; i < RS_WORDS; i++)
        d[i] = r->v[i];
}

static void pk_stages_cmd(const void *p)
{
    const PkStages *s = p;
    rs_new_serial(s->serial);
    if (!rs_cur_used)
        for (int i = 0; i < MAX_STAGES; i++)
            if (rs_cur.d.stage[i].tex && rs_cur.d.stage[i].sampler)
                bk_sampler(&rs_cur.d.stage[i]);
    rs_cur_used = 0;
    rs_cur.li.stages = s->stages;
    for (int i = 0; i < MAX_STAGES; i++) {
        rs_cur.li.tci[i] = s->tci[i];
        rs_cur.d.stage[i] = s->st[i];
    }
}

static void pk_xf_cmd(const void *p)
{
    const PkXf *x = p;
    rs_new_serial(x->serial);
    DrawCmd *d = &rs_cur.d;
    d->vp_x = x->vp_x;
    d->vp_y = x->vp_y;
    d->vp_w = x->vp_w;
    d->vp_h = x->vp_h;
    d->vp_minz = x->vp_minz;
    d->vp_maxz = x->vp_maxz;
    mat4_copy(d->world, x->world);
    mat4_copy(d->view, x->view);
    mat4_copy(d->proj, x->proj);
}

static void pk_world_cmd(const void *p)
{
    const PkWorld *w = p;
    rs_new_serial(w->serial);
    mat4_copy(rs_cur.d.world, w->world);
}

/* Game thread: sends the parts of the state that changed since the last snapshot. */
static void pk_send_parts(uint32_t parts)
{
    const uint32_t serial = g_rs_serial;
    if (parts & RS_PART_STAGE) {
        PkStages *s = rq_begin(pk_stages_cmd, sizeof(PkStages));
        s->serial = serial;
        s->stages = active_stages();
        for (int i = 0; i < MAX_STAGES; i++)
            s->tci[i] = g_rs.stage[i].texcoordindex;
        stages_for_draw(s->st);   /* no uploads: stage_textures_dirty() said so */
        rq_end();
    }
    if (parts & RS_PART_RS) {
        const uint32_t *rs = g_rs.rs;
        PkRs *r = rq_begin(pk_rs_cmd, sizeof(PkRs));
        r->serial = serial;
        r->fog = rs[28];
        r->fogtable = rs[35];
        r->fogvertex = rs[140];
        /* Per draw when objects set their own fog range: constant indices, no loop. */
#define RS_COPY(i, n) r->v[i] = rs[n]
        RS_COPY(0, 7), RS_COPY(1, 8), RS_COPY(2, 9), RS_COPY(3, 14), RS_COPY(4, 15), RS_COPY(5, 19);
        RS_COPY(6, 20), RS_COPY(7, 22), RS_COPY(8, 23), RS_COPY(9, 24), RS_COPY(10, 25), RS_COPY(11, 27);
        RS_COPY(12, 28), RS_COPY(13, 34), RS_COPY(14, 35), RS_COPY(15, 36), RS_COPY(16, 37), RS_COPY(17, 38);
        RS_COPY(18, 41), RS_COPY(19, 137), RS_COPY(20, 140), RS_COPY(21, 60);
#undef RS_COPY
        _Static_assert(RS_WORDS == 22u, "RS_COPY list");
        rq_end();
    }
    if (parts & RS_PART_XF) {
        PkXf *x = rq_begin(pk_xf_cmd, sizeof(PkXf));
        x->serial = serial;
        x->vp_x = g_rs.vp_x;
        x->vp_y = g_rs.vp_y;
        x->vp_w = g_rs.vp_w;
        x->vp_h = g_rs.vp_h;
        x->vp_minz = g_rs.vp_minz;
        x->vp_maxz = g_rs.vp_maxz;
        mat4_copy(x->world, g_rs.world);
        mat4_copy(x->view, g_rs.view);
        mat4_copy(x->proj, g_rs.proj);
        rq_end();
    } else if (parts & RS_PART_WORLD) {
        PkWorld *w = rq_begin(pk_world_cmd, sizeof(PkWorld));
        w->serial = serial;
        mat4_copy(w->world, g_rs.world);
        rq_end();
    }
    if (!parts) {
        uint32_t *s = rq_begin(pk_serial_cmd, sizeof(uint32_t));
        *s = serial;
        rq_end();
    }
}

void render_pk_prepare(int pretransformed, int has_normal)
{
    if (!pk_snap_valid || stage_textures_dirty()) {
        /* Draws recorded with the previous state are drawn before the uploads. */
        render_flush();
        DrawStage stages[MAX_STAGES];
        stages_for_draw(stages);
        send_params();
        PkState *s = rq_begin(pk_state_cmd, sizeof(PkState));
        s->serial = g_rs_serial;
        layout_in_current(&s->li);
        fill_state(&s->d, stages, 1);
        rq_end();
        pk_snap_valid = 1;
        pk_snap_serial = g_rs_serial;
        g_rs_parts = 0;
    } else if (pk_snap_serial != g_rs_serial) {
        pk_send_parts(g_rs_parts);
        pk_snap_serial = g_rs_serial;
        g_rs_parts = 0;
    }
    if (!pretransformed && has_normal && g_rs.rs[137] && (!light_sent_valid || light_sent_serial != g_light_serial)) {
        render_flush();
        send_lights();
    }
}
void render_pk_recorded(void)
{
    pk_pending = 1;
    perf.draws++;
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

extern uint64_t g_idle_us;   /* kernel32: waiting for the game's 30 fps cap */
static float overlay_wait_ms;

/* Overlay line 1: T = time per frame (33.3 at the game's 30 fps), B = busy
 * (T minus the game's wait for its next 30 Hz step), W = that wait. */
static void perf_overlay_draw(void)
{
    const float *a = perf.avg_pc;
    float ds = a[PC_STATE_US] / 1000.0f, dt = a[PC_TEX_US] / 1000.0f, dp = a[PC_PACK_US] / 1000.0f;
    float dg = a[PC_GL_US] / 1000.0f;
    float dother = perf.ms_draw - ds - dt - dp - dg;
    OverlayCmd o;
    memset(&o, 0, sizeof(o));
    char (*lines)[40] = o.lines;
    snprintf(lines[0], sizeof(lines[0]), "F %.1f", (double)perf.fps);
    snprintf(lines[1], sizeof(lines[1]), "T %.1f B %.1f W %.1f", (double)perf.ms_frame,
             (double)(perf.ms_frame - overlay_wait_ms), (double)overlay_wait_ms);
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
    send_params();
    rq_exec(bk_overlay, &o, sizeof(o));
}

static void perf_frame_begin_swap(void)
{
    if (perf.enabled < 0) {
        const char *e = getenv("IGI_OVERLAY");
#if defined(__vita__)
        perf.enabled = !(e && e[0] == '0');
        FILE *f = fopen("ux0:data/igi/igi.cfg", "r");   /* overlay=0 hides it */
        if (f) {
            char buf[128];
            while (fgets(buf, sizeof(buf), f))
                if (strncmp(buf, "overlay=", 8) == 0)
                    perf.enabled = buf[8] != '0';
            fclose(f);
        }
#else
        perf.enabled = e && e[0] == '1';
#endif
    }
    uint64_t now = win32_perf_counter();
    if (!perf.frame_start) {
        perf.frame_start = now;
        perf.window_start = now;
    }
    if (g_pack_thread)
        pk_collect();
    if (fine_timing > 0 && !rq_on) {
        uint64_t st, gl;
        bk_take_timing(&st, &gl);
        pc_frame[PC_STATE_US] += st;
        pc_frame[PC_GL_US] += gl;
    }
    bench_frame(now, now - perf.frame_start, perf.draw_us, perf.last_swap_us, perf.draws, perf_gpu_draws,
                (uint32_t)pc_frame[PC_VERTS]);
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
        /* Time the game spent waiting for its own 30 fps step (kernel32 g_idle_us). */
        static uint64_t idle_at_window;
        overlay_wait_ms = (float)(g_idle_us - idle_at_window) / n / 1000.0f;
        idle_at_window = g_idle_us;
        perf.draws_per_frame = perf.acc_draws / perf.frames;
        perf.gpu_draws_per_frame = perf.acc_gpu_draws / perf.frames;
        for (int i = 0; i < PC_COUNT; i++) {
            perf.avg_pc[i] = (float)perf.acc_pc[i] / n;
            perf.acc_pc[i] = 0;
        }
        const float *a = perf.avg_pc;
        /* Once a second into igi.log, only while measuring: each log line is
         * flushed to the memory card, which can stall the game on the Vita. */
        if (perf.enabled || bench_enabled()) {
        rt_log("perf: %.1f fps, frame %.1f ms = game %.1f + draw %.1f + swap %.1f, %u draws -> %u gpu draws; "
               "render thread busy %.1f ms",
               (double)perf.fps, (double)perf.ms_frame, (double)perf.ms_game, (double)perf.ms_draw,
               (double)perf.ms_swap, perf.draws_per_frame, perf.gpu_draws_per_frame,
               (double)rq_take_busy_us() / n / 1000.0);
        rt_log("perf: draw = state %.2f + tex %.2f + pack %.2f + gl %.2f ms (+ rest); %.0f verts %.0f idx; "
               "tex %.1f uploads %.1f skipped %.0f KB; 2d %.1f blits %.2f ms %.0f KB; %.1f readbacks",
               (double)(a[PC_STATE_US] / 1000.0f), (double)(a[PC_TEX_US] / 1000.0f),
               (double)(a[PC_PACK_US] / 1000.0f), (double)(a[PC_GL_US] / 1000.0f), (double)a[PC_VERTS],
               (double)a[PC_INDICES], (double)a[PC_TEX_N], (double)a[PC_TEX_SKIP],
               (double)(a[PC_TEX_BYTES] / 1024.0f), (double)a[PC_2D_N], (double)(a[PC_2D_US] / 1000.0f),
               (double)(a[PC_2D_BYTES] / 1024.0f), (double)a[PC_READBACK_N]);
        }
        perf.acc_frame = perf.acc_draw = perf.acc_swap = 0;
        perf.acc_draws = perf.acc_gpu_draws = 0;
        perf.frames = 0;
        perf.window_start = now;
    }
    if (perf.enabled)
        perf_overlay_draw();
}

/* With the render thread: the time the game waited for the previous frame. */
static void perf_swap(void)
{
    perf.last_swap_us = rq_swap_end_frame();
    perf.acc_swap += perf.last_swap_us;
}
