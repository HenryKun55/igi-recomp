/* Inline fast path of the render queue's rq_begin/rq_end (render_queue.c). */
#ifndef IGI_RQ_FAST_H
#define IGI_RQ_FAST_H

#include <stddef.h>
#include <stdint.h>

extern int rq_on;
void *rq_begin(void (*fn)(const void *args), size_t size);
void rq_end(void);
#define RQ_FAST_MAX_ARGS 4096u   /* RQ_MAX_ARGS */

/* The game thread's command stream (render_queue.c), for the inline fast path of
 * rq_begin below: commands are records {header, arguments} rounded up to 16
 * bytes in chained chunks; a NULL fn continues at the next chunk. */
typedef struct RqChunk {
    struct RqChunk *next;
    size_t cap, used;
    _Alignas(16) uint8_t mem[];
} RqChunk;
typedef struct RqCmdHdr {
    _Alignas(16) void (*fn)(const void *args);
    uint32_t size;               /* whole record, header included */
} RqCmdHdr;
extern RqChunk *rq_rec_chunk;
extern uint64_t rq_rec_cmds;

/* rq_begin / rq_end for the commands recorded with every draw: inline while the
 * current chunk has room (as rq_cmd: room for a continuation header stays). */
static inline void *rq_begin_fast(void (*fn)(const void *args), size_t size)
{
    if (rq_on && size <= RQ_FAST_MAX_ARGS) {
        RqChunk *c = rq_rec_chunk;
        const uint32_t rec = (uint32_t)((sizeof(RqCmdHdr) + size + 15u) & ~(size_t)15u);
        if (c->cap - c->used >= rec + sizeof(RqCmdHdr)) {
            RqCmdHdr *h = (RqCmdHdr *)(void *)(c->mem + c->used);
            h->fn = fn;
            h->size = rec;
            c->used += rec;
            rq_rec_cmds++;
            return h + 1;
        }
    }
    return rq_begin(fn, size);
}

static inline void rq_end_fast(void)
{
    if (!rq_on)
        rq_end();
}

#endif
