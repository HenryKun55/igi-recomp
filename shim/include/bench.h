/* Automated in-game benchmark (shim/src/bench.c). */
#ifndef IGI_BENCH_H
#define IGI_BENCH_H

#include <stdint.h>

/* IGI_BENCH=<seconds> (desktop) or ux0:data/igi/bench.txt (Vita). */
int bench_enabled(void);
/* Once per presented frame: drives the game and records the frame's timings (us). */
void bench_frame(uint64_t now, uint64_t frame_us, uint64_t draw_us, uint64_t swap_us, uint32_t draws,
                 uint32_t gpu_draws, uint32_t verts);

#endif
