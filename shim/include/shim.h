/* Shared helpers for the Win32 / DirectX API shims. */
#ifndef IGI_SHIM_H
#define IGI_SHIM_H

#include "recomp_gen.h"
#include "runtime.h"

#define SHIM(dll, name) void imp_##dll##_##name(Ctx *c)

/* Windows view of the game: installed to C:\IGI, retail CD in D:. */
#define WIN_INSTALL_DIR "C:\\IGI"
#define WIN_CD_DRIVE 'D'

/* Translate a Windows path used by the game into a host path. */
void win_path_to_host(const char *win, char *out, size_t out_size);

/* Handle table shared by files, find handles, mutexes and events. */
enum { H_FREE, H_FILE, H_FIND, H_MUTEX, H_STD, H_MODULE, H_HEAP, H_MISC };

typedef struct HandleObj {
    int type;
    int fd;
    void *ptr;
} HandleObj;

uint32_t handle_new(int type);
HandleObj *handle_get(uint32_t h, int type);
void handle_close(uint32_t h);

void win32_set_last_error(uint32_t e);

/* Host function exported to GetProcAddress under dll!name. */
typedef struct ProcExport {
    const char *dll;
    const char *name;
    GuestFn fn;
} ProcExport;

uint32_t win32_get_proc(const char *dll, const char *name);

/* Win32 time helpers. */
uint32_t win32_tick_count(void);
uint64_t win32_perf_counter(void);

extern char g_cd_dir[1024];

#endif
