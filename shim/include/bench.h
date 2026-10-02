/* Automated in-game benchmark (shim/src/bench.c). */
#ifndef IGI_BENCH_H
#define IGI_BENCH_H

#include <stdint.h>

/* IGI_BENCH=<seconds> (desktop) or ux0:data/igi/bench.txt (Vita). */
int bench_enabled(void);
/* Index of the measured frame being presented when the benchmark wants a screenshot
 * of it (second number in bench.txt, IGI_BENCH_SHOT on desktop: every N frames), else -1. */
int bench_shot_due(void);
/* Once per presented frame: drives the game and records the frame's timings (us). */
void bench_frame(uint64_t now, uint64_t frame_us, uint64_t draw_us, uint64_t swap_us, uint32_t draws,
                 uint32_t gpu_draws, uint32_t verts);

/* Per-frame event counts for the benchmark's frame breakdown; always counted
 * (plain increments), read and cleared once per measured frame. */
typedef struct BenchCounters {
    uint32_t file_opens, file_reads, read_bytes, heap_allocs;
    uint64_t read_us;
} BenchCounters;
extern BenchCounters g_bench_ctr;

/* Scheduler pass classification (runtime/src/igi_fixes.c): the previous pass of
 * the game's scheduler took us microseconds and ran an update step, a render
 * step, or both. */
enum { BENCH_STEP_UPDATE = 1, BENCH_STEP_RENDER = 2 };
void bench_step(uint32_t kind, uint64_t us);

#endif
