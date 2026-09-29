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

enum {
    PRIM_POINTS = 1, PRIM_LINES, PRIM_LINE_STRIP, PRIM_TRIANGLES, PRIM_TRIANGLE_STRIP, PRIM_TRIANGLE_FAN
};

/* Draw vertices; `pretransformed` means x/y are screen pixels, z depth, w = rhw. */
void render_draw(int prim, const RVertex *v, uint32_t nv, const uint16_t *idx, uint32_t ni,
                 int pretransformed, int has_normal, int has_color, int has_spec, int ntex);
void render_clear(uint32_t nrects, const Rect *rects, uint32_t flags, uint32_t color, float z);

#endif
