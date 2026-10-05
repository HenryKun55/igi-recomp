/*
 * Direct entries of the IDirect3DDevice7 methods (shim/src/d3d7.c) for the
 * hand-decompiled game functions (runtime/src/native_*.c): the same effects
 * as the guest's COM call (vtable of the device object), without the guest
 * call, the thunk and the stack arguments. The guest's call would also leave
 * its arguments and return address below its esp; a native function writes
 * those words itself where the guest's frame is reproduced.
 *
 * Every method here returns D3D_OK (0) to the guest, as the COM methods do.
 * Pointer arguments are guest addresses.
 */
#ifndef IGI_D3D_DIRECT_H
#define IGI_D3D_DIRECT_H

#include <stdint.h>

/* Guest address of the device's vtable (0 before the first device exists): a
 * native function uses the direct entries only for a device object whose
 * vtable this is. */
extern uint32_t g_d3dd_vtbl;

/* What the methods return (D3D_OK). */
#define D3DD_OK 0u

void d3dd_set_render_state(uint32_t state, uint32_t value);
void d3dd_set_tss(uint32_t stage, uint32_t type, uint32_t value);
void d3dd_set_texture(uint32_t stage, uint32_t tex);
void d3dd_set_transform(uint32_t type, uint32_t matrix);
void d3dd_set_light(uint32_t index, uint32_t light);
void d3dd_light_enable(uint32_t index, uint32_t enable);
void d3dd_set_material(uint32_t material);
void d3dd_draw_indexed_vb(uint32_t prim, uint32_t vb, uint32_t start, uint32_t nv, uint32_t idx, uint32_t ni);

#endif
