/* Direct3D 7 fixed-function state as consumed by the GL renderer. */
#ifndef IGI_RENDER_H
#define IGI_RENDER_H

#include "ddraw_priv.h"

#define MAX_STAGES 2
#define MAX_LIGHTS 8

typedef struct RVertex {
    float x, y, z, w;          /* object space (w = 1) or pre-transformed screen space with rhw */
    float nx, ny, nz;
    uint32_t color;            /* RGBA8, R in the low byte */
    uint32_t spec;
    float uv[MAX_STAGES][2];
} RVertex;

typedef struct StageState {
    uint32_t colorop, colorarg1, colorarg2;
    uint32_t alphaop, alphaarg1, alphaarg2;
    uint32_t texcoordindex;
    uint32_t addressu, addressv;
    uint32_t magfilter, minfilter, mipfilter;
    Surface *texture;
} StageState;

typedef struct Light {
    uint32_t type;
    float diffuse[4], specular[4], ambient[4];
    float pos[3], dir[3];
    float range, falloff, att0, att1, att2, theta, phi;
    int enabled;
} Light;

typedef struct RenderState {
    uint32_t rs[256];
    StageState stage[MAX_STAGES];
    float world[16], view[16], proj[16];
    int32_t vp_x, vp_y, vp_w, vp_h;
    float vp_minz, vp_maxz;
    float mat_diffuse[4], mat_ambient[4], mat_specular[4], mat_emissive[4], mat_power;
    Light lights[MAX_LIGHTS];
} RenderState;

extern RenderState g_rs;
/* Bumped whenever g_rs actually changes (draw batching compares it instead of the whole state). */
extern uint32_t g_rs_serial;
/* Call before changing g_rs: draws the pending batch with the old state and bumps g_rs_serial. */
void render_state_changing(void);
/* Bumped with g_rs_serial when an input of the fixed-function lighting changes
 * (lights, material, view matrix, ambient/color material/normalize states). */
extern uint32_t g_light_serial;
/* Presented frames so far. */
extern uint32_t g_frame_serial;

enum {
    PRIM_POINTS = 1, PRIM_LINES, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN
};

/* Draw vertices; `pretransformed` means x/y are screen pixels, z depth, w = rhw. */
void render_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                 int pretransformed, int has_normal, int has_color, int has_spec, int ntex);
/* Diagnostic mode: decode whole index ranges instead of only the used vertices. */
int render_diag_no_compact(void);
/* Diagnostic mode: decode vertex buffers on every draw instead of caching them. */
int render_diag_no_vb_cache(void);
/* Object-space bounds of the next render_draw's vertices (NULL: unknown); lets
 * the near-plane test skip the per-vertex check. Cleared by render_draw. */
void render_set_bounds(const float *lo, const float *hi);

/* Untransformed vertices still in the game's own layout (guest memory). */
typedef struct GuestVerts {
    const uint8_t *base;
    uint32_t stride;
    const uint32_t *src;        /* vertex i is base + src[i] * stride; NULL: consecutive */
    uint32_t nrm_off, col_off, spec_off, uv_off[MAX_STAGES], uv_bytes[MAX_STAGES];
    int pretransformed;         /* x, y, z, rhw screen-space positions */
    int has_normal, has_color, has_spec, ntex;
} GuestVerts;
/* Packs straight from guest memory into the draw batch (no RVertex step).
 * Returns 0 when the draw needs the general path (near-plane clipping, big
 * or non-triangle draws, diagnostics). */
int render_draw_guest(int prim, const GuestVerts *g, uint32_t nv, const uint16_t *idx, uint32_t ni);
/* Indexed triangle list from a pre-transformed vertex buffer drawn more than once
 * since its last writable Lock (lock_id: unique per Lock; start: first vertex,
 * g->base points at it): packs the nv vertices once for all such draws. Returns 0
 * when the draw needs the general path. */
int render_draw_dynvb(uint32_t lock_id, uint32_t start, const GuestVerts *g, uint32_t nv, int prim,
                      const uint16_t *idx, uint32_t ni);
/* Draws from a GPU copy of an unchanged vertex buffer (`all`: the whole buffer decoded,
 * `gen`: its version). Returns 0 when the draw must take the normal path. */
int render_draw_vb(void **slot, uint32_t gen, const RVertex *all, uint32_t count, uint32_t start, int prim,
                   uint32_t nv, const uint16_t *idx, uint32_t ni, int has_normal, int has_color, int has_spec,
                   int ntex);
void render_vb_release(void **slot);
void render_clear(uint32_t nrects, const Rect *rects, uint32_t flags, uint32_t color, float z);

#endif
