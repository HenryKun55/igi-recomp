/* Render thread: records the renderer's GL calls and replays them on another
 * core (see render_queue.h).
 *
 * Commands go into per-frame slots (two: the frame being recorded and the one
 * being executed). A slot owns chained chunks of command records (function +
 * arguments) and two chained data arenas (copied payloads, streamed vertices).
 * The game thread publishes the number of commands recorded so far; the render
 * thread executes up to that number, so recording never takes a lock. A frame's
 * slot is reused only after the render thread has executed it.
 */
#include "platform.h"
#include "runtime.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#elif defined(__vita__)
#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#else
#include <GL/gl.h>
#include <pthread.h>
#include <semaphore.h>
#endif

#define RQ_NO_REDIRECT
#include "render_queue.h"

int rq_on;

/* ---- semaphores and the thread ------------------------------------------------ */

#if defined(__vita__)
typedef SceUID Sema;
static void sema_init(Sema *s, const char *name) { *s = sceKernelCreateSema(name, 0, 0, 0x7FFFFFFF, NULL); }
static void sema_signal(Sema *s) { sceKernelSignalSema(*s, 1); }
static void sema_wait(Sema *s) { sceKernelWaitSema(*s, 1, NULL); }
#elif defined(__APPLE__)
typedef dispatch_semaphore_t Sema;
static void sema_init(Sema *s, const char *name) { (void)name; *s = dispatch_semaphore_create(0); }
static void sema_signal(Sema *s) { dispatch_semaphore_signal(*s); }
static void sema_wait(Sema *s) { dispatch_semaphore_wait(*s, DISPATCH_TIME_FOREVER); }
#else
typedef sem_t Sema;
static void sema_init(Sema *s, const char *name) { (void)name; sem_init(s, 0, 0); }
static void sema_signal(Sema *s) { sem_post(s); }
static void sema_wait(Sema *s) { while (sem_wait(s) != 0) {} }
#endif

/* The game thread's wait: on macOS the main thread also runs the main queue,
 * which the GL thread's buffer swap may need (window updates). */
static void sema_wait_game(Sema *s)
{
#if defined(__APPLE__)
    while (dispatch_semaphore_wait(*s, dispatch_time(DISPATCH_TIME_NOW, 2000000)) != 0)
        platform_service_main();
#else
    sema_wait(s);
#endif
}

/* ---- memory ------------------------------------------------------------------ */

typedef struct Chunk {
    struct Chunk *next;
    size_t cap, used;
    _Alignas(16) uint8_t mem[];
} Chunk;

static Chunk *chunk_new(size_t cap)
{
    /* 16-byte aligned (malloc is not on every libc); chunks are never freed. */
    uint8_t *raw = malloc(sizeof(Chunk) + cap + 16u);
    if (!raw)
        rt_fatal(NULL, "render queue: out of memory (%zu bytes)", cap);
    Chunk *c = (Chunk *)(void *)(raw + ((16u - ((uintptr_t)raw & 15u)) & 15u));
    c->next = NULL;
    c->cap = cap;
    c->used = 0;
    return c;
}

/* A chain of chunks used as one bump allocator; reset keeps the chunks. */
typedef struct Arena {
    Chunk *first, *cur;
    size_t chunk_size;
} Arena;

static void arena_reset(Arena *a)
{
    a->cur = a->first;
    a->cur->used = 0;
}

/* Makes room for n bytes at a->cur (moving on or chaining a new chunk). */
static uint8_t *arena_room(Arena *a, size_t n)
{
    Chunk *c = a->cur;
    if (c->cap - c->used >= n)
        return c->mem + c->used;
    if (c->next && c->next->cap >= n) {
        c = c->next;
    } else {
        Chunk *nc = chunk_new(n > a->chunk_size ? n : a->chunk_size);
        nc->next = c->next;
        c->next = nc;
        c = nc;
    }
    c->used = 0;
    a->cur = c;
    return c->mem;
}

#define CMD_CHUNK (256u << 10)
#define DATA_CHUNK (2u << 20)
#define VTX_CHUNK (4u << 20)
#define NSLOTS 2u

typedef void (*CmdFn)(const void *args);
typedef struct CmdHdr {
    _Alignas(16) CmdFn fn;   /* NULL: continue at the next chunk */
    uint32_t size;           /* whole record, header included */
} CmdHdr;

typedef struct Slot {
    Chunk *cmd_first;
    Arena data, vtx;
} Slot;

static Slot slots[NSLOTS];

/* Game thread recording state. */
static Chunk *rec_chunk;
static uint64_t rec_cmds;      /* commands recorded */
static uint32_t rec_frame;     /* frame being recorded */
static size_t vtx_reserved;

/* Shared counters. */
static _Atomic uint64_t pub_cmds;      /* commands the render thread may execute */
static _Atomic uint64_t done_cmds;     /* executed (updated when the render thread catches up) */
static _Atomic uint32_t done_frames;   /* frames fully executed */
static _Atomic int render_idle;        /* the render thread is (about to be) waiting for work */
static _Atomic int game_waiting;
static _Atomic uint64_t busy_us;
static Sema render_sema, game_sema;
static int started;

/* Markers compared by the render thread (never called). */
static void cmd_end_frame(const void *a) { (void)a; }
static void cmd_exit(const void *a) { (void)a; }

static void *rq_cmd(CmdFn fn, uint32_t args)
{
    uint32_t size = (uint32_t)((sizeof(CmdHdr) + args + 15u) & ~(size_t)15u);
    Chunk *c = rec_chunk;
    if (c->cap - c->used < size + sizeof(CmdHdr)) {
        CmdHdr *j = (CmdHdr *)(void *)(c->mem + c->used);
        j->fn = NULL;
        j->size = 0;
        rec_cmds++;
        if (!c->next)
            c->next = chunk_new(CMD_CHUNK);
        c = c->next;
        c->used = 0;
        rec_chunk = c;
    }
    CmdHdr *h = (CmdHdr *)(void *)(c->mem + c->used);
    h->fn = fn;
    h->size = size;
    c->used += size;
    rec_cmds++;
    return h + 1;
}

void *rq_data(size_t n)
{
    Arena *a = &slots[rec_frame % NSLOTS].data;
    n = (n + 15u) & ~(size_t)15u;
    uint8_t *p = arena_room(a, n);
    a->cur->used += n;
    return p;
}

void *rq_vtx_reserve(size_t n)
{
    Arena *a = &slots[rec_frame % NSLOTS].vtx;
    vtx_reserved = n;
    return arena_room(a, (n + 15u) & ~(size_t)15u);
}

void rq_vtx_commit(size_t used)
{
    Arena *a = &slots[rec_frame % NSLOTS].vtx;
    if (used > vtx_reserved)
        used = vtx_reserved;
    a->cur->used += (used + 15u) & ~(size_t)15u;
    vtx_reserved = 0;
}

static void publish(void)
{
    atomic_store(&pub_cmds, rec_cmds);
    if (atomic_exchange(&render_idle, 0))
        sema_signal(&render_sema);
}

/* Game thread: waits until *ctr >= target. */
static void wait_for(_Atomic uint64_t *ctr, uint64_t target)
{
    if (atomic_load(ctr) >= target)
        return;
    atomic_store(&game_waiting, 1);
    while (atomic_load(ctr) < target)
        sema_wait_game(&game_sema);
    atomic_store(&game_waiting, 0);
}

static void wait_frames(uint32_t target)
{
    if (atomic_load(&done_frames) >= target)
        return;
    atomic_store(&game_waiting, 1);
    while (atomic_load(&done_frames) < target)
        sema_wait_game(&game_sema);
    atomic_store(&game_waiting, 0);
}

static void notify_game(void)
{
    if (atomic_load(&game_waiting))
        sema_signal(&game_sema);
}

void rq_sync(void)
{
    if (!rq_on)
        return;
    publish();
    wait_for(&done_cmds, rec_cmds);
}

/* ---- render thread ------------------------------------------------------------- */

extern uint64_t win32_perf_counter(void);

/* The render thread's own clock: win32_perf_counter is the game's (IGI_FAKE_CLOCK
 * advances it per call, so calls from here would change the game's timing). */
static uint64_t thread_clock_us(void)
{
#if defined(__vita__)
    return sceKernelGetProcessTimeWide();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
#endif
}
static _Atomic int gl_ready;

static void render_loop(void)
{
    platform_gl_init();
    atomic_store(&gl_ready, 1);
    notify_game();
    uint32_t frame = 0;
    Chunk *c = slots[0].cmd_first;
    size_t off = 0;
    uint64_t done = 0;
    for (;;) {
        uint64_t pub = atomic_load(&pub_cmds);
        if (done == pub) {
            atomic_store(&done_cmds, done);
            notify_game();
            atomic_store(&render_idle, 1);
            if (atomic_load(&pub_cmds) == done)
                sema_wait(&render_sema);
            else
                atomic_store(&render_idle, 0);
            continue;
        }
        uint64_t t0 = thread_clock_us();
        while (done < pub) {
            if (!c) {
                c = slots[frame % NSLOTS].cmd_first;
                off = 0;
            }
            const CmdHdr *h = (const CmdHdr *)(const void *)(c->mem + off);
            done++;
            if (!h->fn) {
                c = c->next;
                off = 0;
                continue;
            }
            off += h->size;
            if (h->fn == cmd_end_frame) {
                frame++;
                c = NULL;
                atomic_fetch_add(&busy_us, thread_clock_us() - t0);
                t0 = thread_clock_us();
                atomic_store(&done_cmds, done);
                atomic_store(&done_frames, frame);
                notify_game();
                continue;
            }
            if (h->fn == cmd_exit) {
                atomic_store(&done_cmds, done);
                notify_game();
                return;
            }
            h->fn(h + 1);
        }
        atomic_fetch_add(&busy_us, thread_clock_us() - t0);
    }
}

#if defined(__vita__)
static SceUID render_tid;
static int render_entry(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    render_loop();
    return sceKernelExitThread(0);
}
#else
static pthread_t render_tid;
static void *render_entry(void *arg)
{
    (void)arg;
    render_loop();
    return NULL;
}
#endif

/* 2 (debug): the game waits for every frame to be drawn before going on (same
 * command streams with single-thread timing, for comparing images). */
static int lockstep;

static int want_thread(void)
{
    int on = 1;
    const char *e = getenv("IGI_RENDER_THREAD");
    if (e) {
        on = e[0] != '0';
        lockstep = e[0] == '2';
    }
#if defined(__vita__)
    FILE *f = fopen("ux0:data/igi/igi.cfg", "r");
    if (f) {
        char buf[128];
        while (fgets(buf, sizeof(buf), f))
            if (strncmp(buf, "render_thread=", 14) == 0)
                on = buf[14] != '0';
        fclose(f);
    }
#endif
    return on;
}

void rq_start(void)
{
    if (started)
        return;
    started = 1;
    if (!want_thread()) {
        rt_log("render thread: off");
        platform_gl_init();
        return;
    }
    for (unsigned i = 0; i < NSLOTS; i++) {
        slots[i].cmd_first = chunk_new(CMD_CHUNK);
        slots[i].data.first = slots[i].data.cur = chunk_new(DATA_CHUNK);
        slots[i].data.chunk_size = DATA_CHUNK;
        slots[i].vtx.first = slots[i].vtx.cur = chunk_new(VTX_CHUNK);
        slots[i].vtx.chunk_size = VTX_CHUNK;
    }
    rec_chunk = slots[0].cmd_first;
    sema_init(&render_sema, "igi_rq_render");
    sema_init(&game_sema, "igi_rq_game");
    platform_gl_release();
#if defined(__vita__)
    /* The game on core 0, GL on core 1 (the profiler samples from core 2). */
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);
    render_tid = sceKernelCreateThread("igi_render", render_entry, 0x10000100, 1u << 20, 0,
                                       SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (render_tid < 0 || sceKernelStartThread(render_tid, 0, NULL) < 0)
        rt_fatal(NULL, "render thread: cannot start (%08X)", (unsigned)render_tid);
#else
    if (pthread_create(&render_tid, NULL, render_entry, NULL) != 0)
        rt_fatal(NULL, "render thread: cannot start");
#endif
    atomic_store(&game_waiting, 1);
    while (!atomic_load(&gl_ready))
        sema_wait_game(&game_sema);
    atomic_store(&game_waiting, 0);
    rq_on = 1;
    rt_log("render thread: on");
}

static void cmd_swap(const void *a)
{
    (void)a;
    platform_swap();
}

uint64_t rq_swap_end_frame(void)
{
    uint64_t t0 = win32_perf_counter();
    if (!rq_on) {
        platform_swap();
        return win32_perf_counter() - t0;
    }
    rq_cmd(cmd_swap, 0);
    rq_cmd(cmd_end_frame, 0);
    publish();
    /* At most one frame in flight: the slot recorded next held the frame before this one. */
    wait_frames(lockstep ? rec_frame + 1u : rec_frame);
    rec_frame++;
    Slot *s = &slots[rec_frame % NSLOTS];
    rec_chunk = s->cmd_first;
    rec_chunk->used = 0;
    arena_reset(&s->data);
    arena_reset(&s->vtx);
    return win32_perf_counter() - t0;
}

void rq_shutdown(void)
{
    if (!rq_on)
        return;
    rq_cmd(cmd_exit, 0);
    publish();
    wait_for(&done_cmds, rec_cmds);
#if defined(__vita__)
    sceKernelWaitThreadEnd(render_tid, NULL, NULL);
#else
    pthread_join(render_tid, NULL);
#endif
    rq_on = 0;
    rt_log("render thread: stopped");
}

void render_shutdown(void)
{
    rq_shutdown();
}

uint64_t rq_take_busy_us(void)
{
    return atomic_exchange(&busy_us, 0);
}

/* ---- name mapping ---------------------------------------------------------------
 * Names handed to the game thread are the recorder's; the render thread creates
 * the GL object on first bind. Freed names are reused only after their delete
 * was recorded, and the stream keeps the order. */
typedef struct Names {
    GLuint next;
    GLuint *free_list;
    uint32_t nfree, free_cap;
    GLuint *map;       /* render thread: recorder name -> GL name */
    uint32_t map_cap;
} Names;
static Names tex_names, buf_names;

static GLuint name_alloc(Names *n)
{
    if (n->nfree)
        return n->free_list[--n->nfree];
    return ++n->next;
}

static void name_free(Names *n, GLuint v)
{
    if (n->nfree == n->free_cap) {
        n->free_cap = n->free_cap ? n->free_cap * 2u : 256u;
        n->free_list = realloc(n->free_list, n->free_cap * sizeof(GLuint));
    }
    n->free_list[n->nfree++] = v;
}

static GLuint *name_slot(Names *n, GLuint v)
{
    if (v >= n->map_cap) {
        uint32_t cap = n->map_cap ? n->map_cap : 1024u;
        while (cap <= v)
            cap *= 2u;
        n->map = realloc(n->map, cap * sizeof(GLuint));
        memset(n->map + n->map_cap, 0, (cap - n->map_cap) * sizeof(GLuint));
        n->map_cap = cap;
    }
    return &n->map[v];
}

GLuint rq_tex_name(GLuint name)
{
    if (!rq_on || !name)
        return name;
    GLuint *s = name_slot(&tex_names, name);
    if (!*s)
        glGenTextures(1, s);
    return *s;
}

GLuint rq_buf_name(GLuint name)
{
    if (!rq_on || !name)
        return name;
    GLuint *s = name_slot(&buf_names, name);
    if (!*s)
        glGenBuffers(1, s);
    return *s;
}

/* ---- generic commands ---------------------------------------------------------- */

static _Alignas(16) uint8_t direct_args[RQ_MAX_ARGS];
static CmdFn direct_fn;

void rq_exec(void (*fn)(const void *args), const void *args, size_t size)
{
    if (!rq_on) {
        fn(args);
        return;
    }
    if (size)
        memcpy(rq_cmd(fn, (uint32_t)size), args, size);
    else
        rq_cmd(fn, 0);
}

void *rq_begin(void (*fn)(const void *args), size_t size)
{
    if (size > RQ_MAX_ARGS)
        rt_fatal(NULL, "render queue: command arguments too large (%zu)", size);
    if (!rq_on) {
        direct_fn = fn;
        return direct_args;
    }
    return rq_cmd(fn, (uint32_t)size);
}

void rq_end(void)
{
    if (!rq_on)
        direct_fn(direct_args);
}

/* ---- wrappers ------------------------------------------------------------------- */

#define Q0(name)                                                                      \
    static void x_##name(const void *p) { (void)p; name(); }                          \
    void q_##name(void)                                                               \
    {                                                                                 \
        if (!rq_on) {                                                                 \
            name();                                                                   \
            return;                                                                   \
        }                                                                             \
        rq_cmd(x_##name, 0);                                                          \
    }
#define Q1(name, T1)                                                                  \
    typedef struct { T1 a1; } a_##name;                                               \
    static void x_##name(const void *p) { const a_##name *a = p; name(a->a1); }       \
    void q_##name(T1 a1)                                                              \
    {                                                                                 \
        if (!rq_on) {                                                                 \
            name(a1);                                                                 \
            return;                                                                   \
        }                                                                             \
        a_##name *a = rq_cmd(x_##name, sizeof(*a));                                   \
        a->a1 = a1;                                                                   \
    }
#define Q2(name, T1, T2)                                                              \
    typedef struct { T1 a1; T2 a2; } a_##name;                                        \
    static void x_##name(const void *p) { const a_##name *a = p; name(a->a1, a->a2); } \
    void q_##name(T1 a1, T2 a2)                                                       \
    {                                                                                 \
        if (!rq_on) {                                                                 \
            name(a1, a2);                                                             \
            return;                                                                   \
        }                                                                             \
        a_##name *a = rq_cmd(x_##name, sizeof(*a));                                   \
        a->a1 = a1;                                                                   \
        a->a2 = a2;                                                                   \
    }
#define Q3(name, T1, T2, T3)                                                          \
    typedef struct { T1 a1; T2 a2; T3 a3; } a_##name;                                 \
    static void x_##name(const void *p)                                               \
    {                                                                                 \
        const a_##name *a = p;                                                        \
        name(a->a1, a->a2, a->a3);                                                    \
    }                                                                                 \
    void q_##name(T1 a1, T2 a2, T3 a3)                                                \
    {                                                                                 \
        if (!rq_on) {                                                                 \
            name(a1, a2, a3);                                                         \
            return;                                                                   \
        }                                                                             \
        a_##name *a = rq_cmd(x_##name, sizeof(*a));                                   \
        a->a1 = a1;                                                                   \
        a->a2 = a2;                                                                   \
        a->a3 = a3;                                                                   \
    }
#define Q4(name, T1, T2, T3, T4)                                                      \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; } a_##name;                          \
    static void x_##name(const void *p)                                               \
    {                                                                                 \
        const a_##name *a = p;                                                        \
        name(a->a1, a->a2, a->a3, a->a4);                                             \
    }                                                                                 \
    void q_##name(T1 a1, T2 a2, T3 a3, T4 a4)                                         \
    {                                                                                 \
        if (!rq_on) {                                                                 \
            name(a1, a2, a3, a4);                                                     \
            return;                                                                   \
        }                                                                             \
        a_##name *a = rq_cmd(x_##name, sizeof(*a));                                   \
        a->a1 = a1;                                                                   \
        a->a2 = a2;                                                                   \
        a->a3 = a3;                                                                   \
        a->a4 = a4;                                                                   \
    }
/* Array parameters copied by value (n floats). */
#define QV1(name, n)                                                                  \
    typedef struct { GLfloat v[n]; } a_##name;                                        \
    static void x_##name(const void *p)                                               \
    {                                                                                 \
        a_##name a = *(const a_##name *)p;                                            \
        name(a.v);                                                                    \
    }                                                                                 \
    void q_##name(const GLfloat *v)                                                   \
    {                                                                                 \
        a_##name a;                                                                   \
        memcpy(a.v, v, sizeof(a.v));                                                  \
        if (!rq_on) {                                                                 \
            name(a.v);                                                                \
            return;                                                                   \
        }                                                                             \
        *(a_##name *)rq_cmd(x_##name, sizeof(a)) = a;                                 \
    }
#define QV2(name, T1, n)                                                              \
    typedef struct { T1 a1; GLfloat v[n]; } a_##name;                                 \
    static void x_##name(const void *p)                                               \
    {                                                                                 \
        a_##name a = *(const a_##name *)p;                                            \
        name(a.a1, a.v);                                                              \
    }                                                                                 \
    void q_##name(T1 a1, const GLfloat *v)                                            \
    {                                                                                 \
        a_##name a;                                                                   \
        a.a1 = a1;                                                                    \
        memcpy(a.v, v, sizeof(a.v));                                                  \
        if (!rq_on) {                                                                 \
            name(a.a1, a.v);                                                          \
            return;                                                                   \
        }                                                                             \
        *(a_##name *)rq_cmd(x_##name, sizeof(a)) = a;                                 \
    }
#define QV3(name, T1, T2)                                                             \
    typedef struct { T1 a1; T2 a2; GLfloat v[4]; } a_##name;                          \
    static void x_##name(const void *p)                                               \
    {                                                                                 \
        a_##name a = *(const a_##name *)p;                                            \
        name(a.a1, a.a2, a.v);                                                        \
    }                                                                                 \
    void q_##name(T1 a1, T2 a2, const GLfloat *v)                                     \
    {                                                                                 \
        a_##name a;                                                                   \
        a.a1 = a1;                                                                    \
        a.a2 = a2;                                                                    \
        memcpy(a.v, v, (a2 == 0x1204 /* GL_SPOT_DIRECTION */ ? 3u : 4u) * sizeof(GLfloat)); \
        if (!rq_on) {                                                                 \
            name(a.a1, a.a2, a.v);                                                    \
            return;                                                                   \
        }                                                                             \
        *(a_##name *)rq_cmd(x_##name, sizeof(a)) = a;                                 \
    }

Q1(glEnable, GLenum)
Q1(glDisable, GLenum)
Q1(glEnableClientState, GLenum)
Q1(glDisableClientState, GLenum)
Q2(glBlendFunc, GLenum, GLenum)
Q1(glDepthFunc, GLenum)
Q1(glDepthMask, GLboolean)
Q2(glAlphaFunc, GLenum, GLfloat)
Q1(glCullFace, GLenum)
Q1(glFrontFace, GLenum)
Q1(glShadeModel, GLenum)
Q2(glPolygonMode, GLenum, GLenum)
Q1(glActiveTexture, GLenum)
Q1(glClientActiveTexture, GLenum)
Q3(glTexEnvi, GLenum, GLenum, GLint)
Q3(glTexEnvf, GLenum, GLenum, GLfloat)
QV3(glTexEnvfv, GLenum, GLenum)
Q2(glFogi, GLenum, GLint)
Q2(glFogf, GLenum, GLfloat)
QV2(glFogfv, GLenum, 4)
Q4(glColor4f, GLfloat, GLfloat, GLfloat, GLfloat)
Q4(glViewport, GLint, GLint, GLsizei, GLsizei)
Q2(glDepthRange, rq_gldouble, rq_gldouble)
Q1(glMatrixMode, GLenum)
QV1(glLoadMatrixf, 16)
Q4(glClearColor, GLfloat, GLfloat, GLfloat, GLfloat)
Q1(glClearDepth, rq_gldouble)
Q4(glColorMask, GLboolean, GLboolean, GLboolean, GLboolean)
Q1(glClear, GLbitfield)
Q4(glScissor, GLint, GLint, GLsizei, GLsizei)
Q1(glBegin, GLenum)
Q0(glEnd)
Q2(glVertex2f, GLfloat, GLfloat)
Q2(glTexCoord2f, GLfloat, GLfloat)
Q2(glPixelStorei, GLenum, GLint)
Q3(glTexParameteri, GLenum, GLenum, GLint)
QV3(glLightfv, GLenum, GLenum)
QV2(glLightModelfv, GLenum, 4)
QV3(glMaterialfv, GLenum, GLenum)
Q3(glMaterialf, GLenum, GLenum, GLfloat)
Q2(glColorMaterial, GLenum, GLenum)
Q4(glVertexPointer, GLint, GLenum, GLsizei, const void *)
Q4(glColorPointer, GLint, GLenum, GLsizei, const void *)
Q3(glNormalPointer, GLenum, GLsizei, const void *)
Q4(glTexCoordPointer, GLint, GLenum, GLsizei, const void *)
Q3(glDrawArrays, GLenum, GLint, GLsizei)
#if defined(__vita__)
Q1(glUseProgram, GLuint)
Q2(vglBufferData, GLenum, const void *)
#else
Q3(glLightf, GLenum, GLenum, GLfloat)
Q3(glFogCoordPointer, GLenum, GLsizei, const void *)
Q1(glFogCoordf, GLfloat)
Q3(glMultiTexCoord2f, GLenum, GLfloat, GLfloat)
#endif

/* Indices: copied unless an element buffer is bound (then `indices` is an offset). */
static GLuint rec_ibo;

typedef struct {
    GLenum mode;
    GLuint start, end;
    GLsizei count;
    GLenum type;
    const void *indices;
} a_draw_range;

static void x_draw_range(const void *p)
{
    const a_draw_range *a = p;
    glDrawRangeElements(a->mode, a->start, a->end, a->count, a->type, a->indices);
}

void q_glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices)
{
    if (!rq_on) {
        glDrawRangeElements(mode, start, end, count, type, indices);
        return;
    }
    if (!rec_ibo && indices) {
        size_t bytes = (size_t)count * (type == GL_UNSIGNED_SHORT ? 2u : type == GL_UNSIGNED_BYTE ? 1u : 4u);
        void *copy = rq_data(bytes);
        memcpy(copy, indices, bytes);
        indices = copy;
    }
    a_draw_range *a = rq_cmd(x_draw_range, sizeof(*a));
    a->mode = mode;
    a->start = start;
    a->end = end;
    a->count = count;
    a->type = type;
    a->indices = indices;
}

/* Names */
typedef struct {
    GLenum target;
    GLuint name;
} a_bind;

static void x_bind_texture(const void *p)
{
    const a_bind *a = p;
    glBindTexture(a->target, rq_tex_name(a->name));
}

static void x_bind_buffer(const void *p)
{
    const a_bind *a = p;
    glBindBuffer(a->target, rq_buf_name(a->name));
}

static void x_delete_texture(const void *p)
{
    const a_bind *a = p;
    GLuint *s = name_slot(&tex_names, a->name);
    if (*s)
        glDeleteTextures(1, s);
    *s = 0;
}

static void x_delete_buffer(const void *p)
{
    const a_bind *a = p;
    GLuint *s = name_slot(&buf_names, a->name);
    if (*s)
        glDeleteBuffers(1, s);
    *s = 0;
}

void q_glGenTextures(GLsizei n, GLuint *names)
{
    if (!rq_on) {
        glGenTextures(n, names);
        return;
    }
    for (GLsizei i = 0; i < n; i++)
        names[i] = name_alloc(&tex_names);
}

void q_glGenBuffers(GLsizei n, GLuint *names)
{
    if (!rq_on) {
        glGenBuffers(n, names);
        return;
    }
    for (GLsizei i = 0; i < n; i++)
        names[i] = name_alloc(&buf_names);
}

void q_glDeleteTextures(GLsizei n, const GLuint *names)
{
    if (!rq_on) {
        glDeleteTextures(n, names);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        if (!names[i])
            continue;
        a_bind *a = rq_cmd(x_delete_texture, sizeof(*a));
        a->target = 0;
        a->name = names[i];
        name_free(&tex_names, names[i]);
    }
}

void q_glDeleteBuffers(GLsizei n, const GLuint *names)
{
    if (!rq_on) {
        glDeleteBuffers(n, names);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        if (!names[i])
            continue;
        if (names[i] == rec_ibo)
            rec_ibo = 0;
        a_bind *a = rq_cmd(x_delete_buffer, sizeof(*a));
        a->target = 0;
        a->name = names[i];
        name_free(&buf_names, names[i]);
    }
}

void q_glBindTexture(GLenum target, GLuint name)
{
    if (!rq_on) {
        glBindTexture(target, name);
        return;
    }
    a_bind *a = rq_cmd(x_bind_texture, sizeof(*a));
    a->target = target;
    a->name = name;
}

void q_glBindBuffer(GLenum target, GLuint name)
{
    if (!rq_on) {
        glBindBuffer(target, name);
        return;
    }
    if (target == GL_ELEMENT_ARRAY_BUFFER)
        rec_ibo = name;
    a_bind *a = rq_cmd(x_bind_buffer, sizeof(*a));
    a->target = target;
    a->name = name;
}

/* Data */
typedef struct {
    GLenum target;
    size_t size;
    const void *data;
    GLenum usage;
} a_buffer_data;

static void x_buffer_data(const void *p)
{
    const a_buffer_data *a = p;
    glBufferData(a->target, (GLsizei)a->size, a->data, a->usage);
}

void q_glBufferData(GLenum target, size_t size, const void *data, GLenum usage)
{
    if (!rq_on) {
        glBufferData(target, (GLsizei)size, data, usage);
        return;
    }
    void *copy = NULL;
    if (data) {
        copy = rq_data(size);
        memcpy(copy, data, size);
    }
    a_buffer_data *a = rq_cmd(x_buffer_data, sizeof(*a));
    a->target = target;
    a->size = size;
    a->data = copy;
    a->usage = usage;
}

typedef struct {
    GLenum target;
    GLint level, ifmt, x, y;
    GLsizei w, h;
    GLint border;
    GLenum fmt, type;
    const void *pixels;
} a_tex;

static void x_tex_image(const void *p)
{
    const a_tex *a = p;
    glTexImage2D(a->target, a->level, a->ifmt, a->w, a->h, a->border, a->fmt, a->type, a->pixels);
}

static void x_tex_sub_image(const void *p)
{
    const a_tex *a = p;
    glTexSubImage2D(a->target, a->level, a->x, a->y, a->w, a->h, a->fmt, a->type, a->pixels);
}

/* Bytes of a w x h RGBA8 (or BGRA8) image with the renderer's alignment of 4. */
static size_t image_bytes(GLsizei w, GLsizei h)
{
    return (size_t)w * (size_t)h * 4u;
}

static void tex_record(CmdFn fn, GLenum target, GLint level, GLint ifmt, GLint x, GLint y, GLsizei w, GLsizei h,
                       GLint border, GLenum fmt, GLenum type, const void *pixels)
{
    a_tex *a = rq_cmd(fn, sizeof(*a));
    a->target = target;
    a->level = level;
    a->ifmt = ifmt;
    a->x = x;
    a->y = y;
    a->w = w;
    a->h = h;
    a->border = border;
    a->fmt = fmt;
    a->type = type;
    a->pixels = pixels;
}

static const void *copy_pixels(GLenum type, GLsizei w, GLsizei h, const void *pixels)
{
    if (!pixels)
        return NULL;
    if (type != GL_UNSIGNED_BYTE)
        rt_fatal(NULL, "render queue: unsupported pixel type %04X", type);
    size_t n = image_bytes(w, h);
    void *copy = rq_data(n);
    memcpy(copy, pixels, n);
    return copy;
}

void q_glTexImage2D(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                    GLenum type, const void *pixels)
{
    if (!rq_on) {
        glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
        return;
    }
    tex_record(x_tex_image, target, level, ifmt, 0, 0, w, h, border, fmt, type, copy_pixels(type, w, h, pixels));
}

void q_glTexImage2D_owned(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                          GLenum type, const void *pixels)
{
    if (!rq_on) {
        glTexImage2D(target, level, ifmt, w, h, border, fmt, type, pixels);
        return;
    }
    tex_record(x_tex_image, target, level, ifmt, 0, 0, w, h, border, fmt, type, pixels);
}

void q_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type,
                       const void *pixels)
{
    if (!rq_on) {
        glTexSubImage2D(target, level, x, y, w, h, fmt, type, pixels);
        return;
    }
    tex_record(x_tex_sub_image, target, level, 0, x, y, w, h, 0, fmt, type, copy_pixels(type, w, h, pixels));
}

/* Synchronous calls */
typedef struct {
    GLint x, y;
    GLsizei w, h;
    GLenum fmt, type;
    void *out;
} a_read;

static void x_read_pixels(const void *p)
{
    const a_read *a = p;
    glReadPixels(a->x, a->y, a->w, a->h, a->fmt, a->type, a->out);
}

void q_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, void *out)
{
    if (!rq_on) {
        glReadPixels(x, y, w, h, fmt, type, out);
        return;
    }
    a_read *a = rq_cmd(x_read_pixels, sizeof(*a));
    a->x = x;
    a->y = y;
    a->w = w;
    a->h = h;
    a->fmt = fmt;
    a->type = type;
    a->out = out;
    rq_sync();
}

#if defined(__vita__)
typedef struct {
    uint32_t size;
    void **out;
} a_alloc;

static void x_force_alloc(const void *p)
{
    const a_alloc *a = p;
    *a->out = vglForceAlloc(a->size);
}

void *q_vglForceAlloc(uint32_t size)
{
    if (!rq_on)
        return vglForceAlloc(size);
    void *r = NULL;
    a_alloc *a = rq_cmd(x_force_alloc, sizeof(*a));
    a->size = size;
    a->out = &r;
    rq_sync();
    return r;
}
#endif
