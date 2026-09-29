/* Host-side runtime: guest memory layout, heap, thunks, guest calls, logging. */
#ifndef IGI_RUNTIME_H
#define IGI_RUNTIME_H

#include "recomp.h"

#include <stdarg.h>
#include <stddef.h>

/* Guest address space layout. */
#define GUEST_NULL_GUARD   0x00010000u   /* [0, guard) is never mapped */
#define GUEST_SCRATCH_BASE 0x00010000u   /* host-owned static structures (TIB, strings) */
#define GUEST_SCRATCH_END  0x00400000u
#define GUEST_IMAGE_BASE   0x00400000u
#define GUEST_STACK_BASE   0x01000000u
#define GUEST_STACK_SIZE   0x00800000u
#define GUEST_HEAP_BASE    0x02000000u

/* Host thunks: fake guest code addresses that dispatch to host functions. */
#define THUNK_BASE   0xFE000000u
#define THUNK_STRIDE 4u
#define THUNK_MAX    8192u

/* Return address pushed when the host calls into guest code. */
#define GUEST_RETURN_MAGIC 0xFDFDFD00u

extern uint32_t g_guest_mem_size;

void mem_init(void);
uint32_t mem_heap_limit(void);

/* Scratch allocator for host-owned guest structures that live forever. */
uint32_t scratch_alloc(uint32_t size, uint32_t align);
uint32_t scratch_strdup(const char *s);

/* General guest heap (backs HeapAlloc / VirtualAlloc / COM objects). */
uint32_t heap_alloc(uint32_t size);
uint32_t heap_realloc(uint32_t ptr, uint32_t size);
void heap_free(uint32_t ptr);
uint32_t heap_size(uint32_t ptr);

uint32_t thunk_register(GuestFn fn, const char *name);
uint32_t thunk_register_unique(GuestFn fn, const char *name);
extern uint32_t g_current_thunk;   /* address of the thunk being executed */
GuestFn thunk_lookup(uint32_t addr);
const char *thunk_name(uint32_t addr);

/* Call guest code from the host (stdcall or cdecl; stack is restored either way). */
uint32_t guest_call(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);

/* Host pointer for a guest address. */
static inline void *gptr(uint32_t a) { return a ? (void *)(g_mem + a) : NULL; }
static inline char *gstr(uint32_t a) { return a ? (char *)(g_mem + a) : NULL; }
/* Guest buffer the host writes into (never NULL, unlike gstr). */
static inline char *gbuf(uint32_t a) { return (char *)(g_mem + a); }

/* Stack arguments of a stdcall import: ARG(0) is the first parameter. */
#define ARG(n) RD32(c->r[4] + 4u + 4u * (uint32_t)(n))

static inline void stdcall_ret(Ctx *c, uint32_t value, int nargs)
{
    c->r[0] = value;
    c->r[4] += 4u + 4u * (uint32_t)nargs;
}

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(Ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
void rt_missing_import(Ctx *c, const char *name);

/* Game data directory (contains IGI.exe and the game folders). */
extern char g_game_dir[1024];

#endif
