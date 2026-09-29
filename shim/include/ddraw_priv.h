/* Shared state between the DirectDraw, Direct3D and GL renderer shims. */
#ifndef IGI_DDRAW_PRIV_H
#define IGI_DDRAW_PRIV_H

#include "com.h"

#define DD_OK 0u
#define DDERR_GENERIC E_FAIL
#define DDERR_INVALIDPARAMS E_INVALIDARG
#define DDERR_UNSUPPORTED E_NOTIMPL
#define DDERR_NOTFOUND 0x887600FFu
#define DDERR_SURFACELOST 0x887601C2u
#define DDERR_NOTLOCKED 0x887602E6u
#define DDERR_WASSTILLDRAWING 0x8876021Cu
#define DDENUMRET_OK 1u

/* DDSURFACEDESC flags */
#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_ZBUFFERBITDEPTH 0x40u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_CKSRCBLT 0x10000u
#define DDSD_MIPMAPCOUNT 0x20000u
#define DDSD_REFRESHRATE 0x40000u

/* DDSCAPS */
#define DDSCAPS_ALPHA 0x2u
#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_TEXTURE 0x1000u
#define DDSCAPS_3DDEVICE 0x2000u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_VISIBLE 0x8000u
#define DDSCAPS_ZBUFFER 0x20000u
#define DDSCAPS_MIPMAP 0x400000u
#define DDSCAPS_LOCALVIDMEM 0x10000000u

/* DDPIXELFORMAT flags */
#define DDPF_ALPHAPIXELS 0x1u
#define DDPF_FOURCC 0x4u
#define DDPF_PALETTEINDEXED8 0x20u
#define DDPF_RGB 0x40u
#define DDPF_ZBUFFER 0x400u

typedef struct PixFmt {
    uint32_t flags;
    uint32_t bits;
    uint32_t r, g, b, a;   /* masks; for z buffers `r` holds the z mask */
} PixFmt;

typedef struct Surface Surface;

struct Surface {
    int refs;
    uint32_t objs[8];      /* guest COM objects per interface version, index = version */
    uint32_t w, h, pitch;
    PixFmt pf;
    uint32_t caps, caps2;
    uint32_t mem;          /* guest pixel memory (0 = not allocated yet) */
    int has_ck;
    uint32_t ck_lo, ck_hi;
    Surface *attached;     /* back buffer of a flip chain / next mip level */
    Surface *zbuf;         /* attached depth buffer */
    Surface *parent;       /* owner of an implicitly created attachment */
    int is_primary, is_back, is_z, is_texture;
    int locked;
    int lock_readback;     /* lock of a render target: pixels came from GL */
    int lock_readonly;     /* current lock has DDLOCK_READONLY */
    uint32_t gl_tex;       /* texture name, 0 = none */
    int dirty;             /* guest pixels changed since the last upload */
    int mip_levels;
    int client_mem;        /* mem belongs to the game (DDSD_LPSURFACE): never freed */
    /* Video fast path: the decoder hands its RGBA frame straight to the renderer
     * instead of converting it into the surface's pixel format. */
    struct VideoFrameSource *video;   /* set while the surface shows a decoded movie */
    uint32_t priv[8];      /* renderer scratch */
};

/* Display state. */
extern int g_dd_width, g_dd_height, g_dd_bpp;
extern Surface *g_primary;     /* front buffer */
extern Surface *g_backbuffer;  /* 3D render target */

uint32_t surface_obj(Surface *s, int version);
Surface *surface_from_obj(uint32_t obj);
void surface_alloc_mem(Surface *s);
void pixfmt_write(uint32_t addr, const PixFmt *pf);
void pixfmt_read(uint32_t addr, PixFmt *pf);
PixFmt pixfmt_rgb(int bits);
/* Surface whose pixel memory is the game-supplied buffer at `mem`, if any. */
Surface *surface_by_client_mem(uint32_t mem);
/* True if `mem` is (or was) the pixel memory of a game-supplied surface. */
int ddraw_is_client_mem(uint32_t mem);

/* Decoded movie frame shared by the AVI shim and the renderer. */
typedef struct VideoFrameSource {
    int width, height;
    uint32_t mem;                       /* game buffer the frame was decoded "into" */
    uint32_t serial;                    /* bumps on every decoded frame */
    const uint32_t *(*rgba)(struct VideoFrameSource *);   /* converts lazily */
} VideoFrameSource;
/* The movie frame destined for the surface memory `mem`, if any. */
VideoFrameSource *video_source_for(uint32_t mem);

/* Convert one pixel of `pf` at `p` to RGBA8 (R in the low byte). */
uint32_t pixel_to_rgba(const PixFmt *pf, const uint8_t *p);
uint32_t rgba_to_pixel(const PixFmt *pf, uint32_t rgba);

/* ---- renderer (render_gl.c) -------------------------------------------- */

typedef struct Rect {
    int32_t l, t, r, b;
} Rect;

void render_init(int w, int h);
/* Submit pending batched geometry (before anything that changes surfaces or targets). */
void render_flush(void);
void render_begin_frame(void);
void render_present(void);
void render_fill(const Rect *r, uint32_t rgba);
void render_blit(Surface *src, const Rect *sr, const Rect *dr, int use_ck);
/* Read the render target into the surface's guest memory. */
void render_readback(Surface *s);
/* Draw the surface's guest memory over the whole render target. */
void render_writeback(Surface *s);
void render_texture_release(Surface *s);
/* Make sure the texture object matches the guest pixels. */
uint32_t render_texture_get(Surface *s);

#endif
