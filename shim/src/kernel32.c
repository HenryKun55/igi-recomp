/* kernel32.dll: process, memory, file, time and locale APIs used by IGI.exe and its CRT. */
#include "shim.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_FILE_EXISTS 80u
#define ERROR_ALREADY_EXISTS 183u
#define INVALID_HANDLE_VALUE 0xFFFFFFFFu

#define FAKE_MODULE_BASE 0x7F000000u

static uint32_t last_error;

void win32_set_last_error(uint32_t e) { last_error = e; }

/* ---- handles ------------------------------------------------------------ */

#define MAX_HANDLES 1024u
#define HANDLE_BASE 0x1000u
static HandleObj handles[MAX_HANDLES];

uint32_t handle_new(int type)
{
    for (uint32_t i = 1; i < MAX_HANDLES; i++) {
        if (handles[i].type == H_FREE) {
            memset(&handles[i], 0, sizeof(handles[i]));
            handles[i].type = type;
            handles[i].fd = -1;
            return HANDLE_BASE + i * 4u;
        }
    }
    rt_fatal(NULL, "out of Win32 handles");
}

HandleObj *handle_get(uint32_t h, int type)
{
    if (h < HANDLE_BASE)
        return NULL;
    uint32_t i = (h - HANDLE_BASE) / 4u;
    if (i >= MAX_HANDLES || handles[i].type == H_FREE)
        return NULL;
    if (type != H_FREE && handles[i].type != type)
        return NULL;
    return &handles[i];
}

void handle_close(uint32_t h)
{
    HandleObj *o = handle_get(h, H_FREE);
    if (!o)
        return;
    if (o->type == H_FILE && o->fd >= 0)
        close(o->fd);
    if (o->type == H_FILE) {
        free(o->ptr);
        o->ptr = NULL;
    }
    if (o->type == H_FIND && o->ptr) {
        free(o->ptr);
    }
    o->type = H_FREE;
}

/* ---- paths -------------------------------------------------------------- */

char g_cd_dir[1024];

void win_path_to_host(const char *win, char *out, size_t out_size)
{
    char tmp[1024];
    size_t n = 0;
    for (const char *p = win; *p && n + 1 < sizeof(tmp); p++)
        tmp[n++] = *p == '\\' ? '/' : *p;
    tmp[n] = 0;
    const char *root = g_game_dir;
    const char *rest = tmp;
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') {
        if (toupper((unsigned char)tmp[0]) == WIN_CD_DRIVE) {
            root = g_cd_dir;
            rest = tmp + 2;
        } else {
            rest = tmp + 2;
            /* C:/IGI/... is the install directory. */
            const char *inst = "/IGI";
            size_t il = strlen(inst);
            if (strncasecmp(rest, inst, il) == 0 && (rest[il] == '/' || rest[il] == 0))
                rest += il;
        }
    }
    while (*rest == '/')
        rest++;
    if (rest[0] == '.' && rest[1] == '/')
        rest += 2;
    snprintf(out, out_size, "%s/%s", root, rest);
}

/* ---- process / module --------------------------------------------------- */

SHIM(kernel32, GetVersion) { stdcall_ret(c, 0x0A280105u, 0); }   /* Windows XP 5.1.2600 */

SHIM(kernel32, GetVersionExA)
{
    uint32_t p = ARG(0);
    WR32(p + 4u, 5);
    WR32(p + 8u, 1);
    WR32(p + 12u, 2600);
    WR32(p + 16u, 2);         /* VER_PLATFORM_WIN32_NT */
    WR8(p + 20u, 0);
    stdcall_ret(c, 1, 1);
}

static uint32_t cmdline;

SHIM(kernel32, GetCommandLineA) { stdcall_ret(c, cmdline, 0); }

SHIM(kernel32, GetStartupInfoA)
{
    uint32_t p = ARG(0);
    memset(gptr(p), 0, 68);
    WR32(p, 68);
    stdcall_ret(c, 0, 1);
}

static const char *const known_modules[] = {
    "kernel32.dll", "user32.dll", "gdi32.dll", "ddraw.dll", "dinput.dll", "dsound.dll",
    "winmm.dll", "ole32.dll", "avifil32.dll", "msvfw32.dll",
};

static uint32_t module_handle(const char *name)
{
    char lower[64];
    size_t n = 0;
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            base = p + 1;
    for (; base[n] && n + 1 < sizeof(lower); n++)
        lower[n] = (char)tolower((unsigned char)base[n]);
    lower[n] = 0;
    if (!strchr(lower, '.') && n + 5 < sizeof(lower))
        strcat(lower, ".dll");
    for (uint32_t i = 0; i < sizeof(known_modules) / sizeof(known_modules[0]); i++)
        if (strcmp(lower, known_modules[i]) == 0)
            return FAKE_MODULE_BASE + (i + 1u) * 0x10000u;
    return 0;
}

SHIM(kernel32, GetModuleHandleA)
{
    uint32_t name = ARG(0);
    uint32_t h = name ? module_handle(gstr(name)) : GUEST_IMAGE_BASE;
    stdcall_ret(c, h, 1);
}

SHIM(kernel32, GetModuleFileNameA)
{
    uint32_t buf = ARG(1), size = ARG(2);
    const char *path = WIN_INSTALL_DIR "\\IGI.exe";
    uint32_t n = (uint32_t)strlen(path);
    if (n >= size)
        n = size ? size - 1u : 0u;
    memcpy(gptr(buf), path, n);
    if (size)
        WR8(buf + n, 0);
    stdcall_ret(c, n, 3);
}

SHIM(kernel32, LoadLibraryA)
{
    const char *name = gstr(ARG(0));
    uint32_t h = module_handle(name);
    rt_log("LoadLibraryA(%s) -> %08X", name, h);
    if (!h)
        win32_set_last_error(126);
    stdcall_ret(c, h, 1);
}

SHIM(kernel32, FreeLibrary) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, GetProcAddress)
{
    uint32_t h = ARG(0), name = ARG(1);
    uint32_t r = 0;
    if (h > FAKE_MODULE_BASE && name >= 0x10000u) {
        uint32_t i = (h - FAKE_MODULE_BASE) / 0x10000u - 1u;
        if (i < sizeof(known_modules) / sizeof(known_modules[0]))
            r = win32_get_proc(known_modules[i], gstr(name));
    }
    rt_log("GetProcAddress(%08X, %s) -> %08X", h, name >= 0x10000u ? gstr(name) : "#ordinal", r);
    stdcall_ret(c, r, 2);
}

uint32_t ddraw_get_proc(const char *name);

uint32_t win32_get_proc(const char *dll, const char *name)
{
    if (strcasecmp(dll, "ddraw.dll") == 0) {
        uint32_t r = ddraw_get_proc(name);
        if (r)
            return r;
    }
    for (uint32_t i = 0; i < g_guest_import_count; i++) {
        const GuestImport *imp = &g_guest_imports[i];
        if (strcasecmp(imp->dll, dll) == 0 && strcmp(imp->name, name) == 0)
            return thunk_register(imp->fn, imp->name);
    }
    return 0;
}

SHIM(kernel32, GetCurrentProcess) { stdcall_ret(c, 0xFFFFFFFFu, 0); }

void platform_shutdown(void);

SHIM(kernel32, ExitProcess)
{
    rt_log("ExitProcess(%u)", ARG(0));
    platform_shutdown();
    exit((int)ARG(0));
}

SHIM(kernel32, TerminateProcess)
{
    rt_log("TerminateProcess(%u)", ARG(1));
    platform_shutdown();
    exit((int)ARG(1));
}

SHIM(kernel32, UnhandledExceptionFilter) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, RaiseException)
{
    rt_fatal(c, "RaiseException(%08X, flags %08X)", ARG(0), ARG(1));
}

SHIM(kernel32, RtlUnwind)
{
    rt_fatal(c, "RtlUnwind(frame %08X, target %08X)", ARG(0), ARG(1));
}

SHIM(kernel32, DebugBreak) { rt_fatal(c, "DebugBreak"); }

SHIM(kernel32, OutputDebugStringA)
{
    rt_log("[debug] %s", gstr(ARG(0)));
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetLastError) { stdcall_ret(c, last_error, 0); }

SHIM(kernel32, CreateMutexA)
{
    win32_set_last_error(0);
    stdcall_ret(c, handle_new(H_MUTEX), 3);
}

SHIM(kernel32, ReleaseMutex) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, CloseHandle)
{
    handle_close(ARG(0));
    stdcall_ret(c, 1, 1);
}

/* ---- environment -------------------------------------------------------- */

static uint32_t env_block, env_block_w;

SHIM(kernel32, GetEnvironmentStrings) { stdcall_ret(c, env_block, 0); }
SHIM(kernel32, GetEnvironmentStringsW) { stdcall_ret(c, env_block_w, 0); }
SHIM(kernel32, FreeEnvironmentStringsA) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, FreeEnvironmentStringsW) { stdcall_ret(c, 1, 1); }
SHIM(kernel32, GetEnvironmentVariableA) { win32_set_last_error(203); stdcall_ret(c, 0, 3); }
SHIM(kernel32, SetHandleCount) { stdcall_ret(c, ARG(0), 1); }

static uint32_t std_handles[3];

SHIM(kernel32, GetStdHandle)
{
    uint32_t which = ARG(0);
    uint32_t r = INVALID_HANDLE_VALUE;
    if (which >= 0xFFFFFFF4u && which <= 0xFFFFFFF6u)
        r = std_handles[0xFFFFFFF6u - which];
    stdcall_ret(c, r, 1);
}

SHIM(kernel32, SetStdHandle) { stdcall_ret(c, 1, 2); }

SHIM(kernel32, GetFileType)
{
    HandleObj *o = handle_get(ARG(0), H_FREE);
    uint32_t t = 0;
    if (o && o->type == H_STD)
        t = 2;   /* FILE_TYPE_CHAR */
    else if (o && o->type == H_FILE)
        t = 1;   /* FILE_TYPE_DISK */
    stdcall_ret(c, t, 1);
}

/* ---- memory ------------------------------------------------------------- */

SHIM(kernel32, HeapCreate) { stdcall_ret(c, handle_new(H_HEAP), 3); }
SHIM(kernel32, HeapDestroy) { handle_close(ARG(0)); stdcall_ret(c, 1, 1); }

SHIM(kernel32, HeapAlloc)
{
    uint32_t flags = ARG(1), size = ARG(2);
    uint32_t p = heap_alloc(size ? size : 1u);
    if (p && (flags & 0x8u))
        memset(gptr(p), 0, size);
    if (!p)
        rt_log("HeapAlloc(%u) failed", size);
    stdcall_ret(c, p, 3);
}

SHIM(kernel32, HeapFree)
{
    heap_free(ARG(2));
    stdcall_ret(c, 1, 3);
}

SHIM(kernel32, HeapReAlloc)
{
    uint32_t flags = ARG(1), ptr = ARG(2), size = ARG(3);
    uint32_t old = heap_size(ptr);
    uint32_t p;
    if (flags & 0x10u)            /* HEAP_REALLOC_IN_PLACE_ONLY */
        p = size <= old ? ptr : 0u;
    else
        p = heap_realloc(ptr, size);
    if (p && (flags & 0x8u) && size > old)
        memset(gptr(p + old), 0, size - old);
    stdcall_ret(c, p, 4);
}

/* VirtualAlloc reservations: 64 KiB aligned blocks carved from the guest heap. */
typedef struct Region {
    uint32_t base, size, raw;
} Region;

static Region regions[256];

SHIM(kernel32, VirtualAlloc)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    uint32_t r = 0;
    if (addr) {
        /* Commit inside an existing reservation: the memory is already there. */
        for (uint32_t i = 0; i < 256u; i++)
            if (regions[i].base && addr >= regions[i].base && addr + size <= regions[i].base + regions[i].size)
                r = addr & ~0xFFFu;
        if (!r)
            rt_log("VirtualAlloc(%08X, %u, %X): address outside any reservation", addr, size, type);
    } else {
        uint32_t sz = (size + 0xFFFFu) & ~0xFFFFu;
        uint32_t raw = heap_alloc(sz + 0x10000u);
        if (raw) {
            r = (raw + 0xFFFFu) & ~0xFFFFu;
            memset(gptr(r), 0, sz);
            for (uint32_t i = 0; i < 256u; i++) {
                if (!regions[i].base) {
                    regions[i].base = r;
                    regions[i].size = sz;
                    regions[i].raw = raw;
                    break;
                }
            }
        }
    }
    stdcall_ret(c, r, 4);
}

SHIM(kernel32, VirtualFree)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    for (uint32_t i = 0; i < 256u; i++) {
        Region *g = &regions[i];
        if (!g->base || addr < g->base || addr >= g->base + g->size)
            continue;
        if (type & 0x8000u) {        /* MEM_RELEASE */
            heap_free(g->raw);
            g->base = 0;
        } else if (size) {           /* MEM_DECOMMIT: recommitted pages must read as zero */
            memset(gptr(addr), 0, size);
        }
        break;
    }
    stdcall_ret(c, 1, 3);
}

/* ---- files -------------------------------------------------------------- */

SHIM(kernel32, CreateFileA)
{
    const char *win = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    int flags = 0;
    int rw = (access & 0x40000000u) != 0;       /* GENERIC_WRITE */
    int rd = (access & 0x80000000u) != 0 || !rw;
    flags = rw ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp) {
    case 1: flags |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: flags |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                              /* OPEN_EXISTING */
    case 4: flags |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: flags |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    default: break;
    }
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        win32_set_last_error(errno == EEXIST ? ERROR_FILE_EXISTS :
                             errno == EACCES ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
        rt_log("CreateFileA(%s) -> not found", win);
        stdcall_ret(c, INVALID_HANDLE_VALUE, 7);
        return;
    }
    if (rw)
        rt_log("CreateFileA(%s, write, disp %u) -> %s", win, disp, path);
    uint32_t h = handle_new(H_FILE);
    handle_get(h, H_FILE)->fd = fd;
    win32_set_last_error(0);
    stdcall_ret(c, h, 7);
}

/* ---- buffered reads ----------------------------------------------------
 * The game streams sound in small ReadFile calls; each one was a system call
 * (slow on the Vita). Reads go through a per-handle read-ahead buffer. While
 * the buffer is valid the descriptor sits at base + len and the file position
 * the game sees is base + pos.
 */
#define READ_AHEAD (64u * 1024u)

typedef struct ReadBuf {
    off_t base;
    uint32_t len, pos;
    uint8_t data[READ_AHEAD];
} ReadBuf;

/* Puts the descriptor back at the game's position and drops the buffer. */
static void rb_drop(HandleObj *o)
{
    ReadBuf *b = o->ptr;
    if (b && b->len) {
        lseek(o->fd, b->base + (off_t)b->pos, SEEK_SET);
        b->len = b->pos = 0;
    }
}

static ssize_t rb_read(HandleObj *o, uint8_t *dst, uint32_t n)
{
    ReadBuf *b = o->ptr;
    if (!b) {
        b = calloc(1, sizeof(*b));
        o->ptr = b;
    }
    ssize_t total = 0;
    while (n) {
        if (b->pos < b->len) {
            uint32_t k = b->len - b->pos < n ? b->len - b->pos : n;
            memcpy(dst, b->data + b->pos, k);
            b->pos += k;
            dst += k;
            n -= k;
            total += k;
            continue;
        }
        if (n >= READ_AHEAD) {   /* big reads go straight through */
            rb_drop(o);
            ssize_t got = read(o->fd, dst, n);
            return got < 0 ? (total ? total : got) : total + got;
        }
        off_t at = b->len ? b->base + (off_t)b->len : lseek(o->fd, 0, SEEK_CUR);
        ssize_t got = read(o->fd, b->data, READ_AHEAD);
        if (got <= 0) {
            b->len = b->pos = 0;
            break;
        }
        b->base = at;
        b->len = (uint32_t)got;
        b->pos = 0;
    }
    return total;
}

SHIM(kernel32, ReadFile)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), out = ARG(3);
    HandleObj *o = handle_get(h, H_FILE);
    if (!o) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0, 5);
        return;
    }
    uint64_t t0 = win32_perf_counter();
    ssize_t got = rb_read(o, gptr(buf), n);
    if (got < 0)
        got = 0;
    /* I/O statistics (ReadFile shows up in gameplay profiles). */
    static uint64_t io_us, io_bytes, io_last;
    static uint32_t io_calls;
    uint64_t t1 = win32_perf_counter();
    io_us += t1 - t0;
    io_bytes += (uint64_t)got;
    io_calls++;
    if (t1 - io_last >= 5000000u) {
        rt_log("io: %u ReadFile calls, %llu KB, %.1f ms in the last %.1f s", io_calls,
               (unsigned long long)(io_bytes / 1024u), (double)io_us / 1000.0, (double)(t1 - io_last) / 1e6);
        io_last = t1;
        io_us = io_bytes = 0;
        io_calls = 0;
    }
    if (out)
        WR32(out, (uint32_t)got);
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, WriteFile)
{
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), out = ARG(3);
    HandleObj *o = handle_get(h, H_FREE);
    ssize_t put = 0;
    if (o && o->type == H_STD) {
        put = (ssize_t)fwrite(gptr(buf), 1, n, stderr);
    } else if (o && o->type == H_FILE) {
        rb_drop(o);
        put = write(o->fd, gptr(buf), n);
    } else {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0, 5);
        return;
    }
    if (out)
        WR32(out, put < 0 ? 0u : (uint32_t)put);
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, SetFilePointer)
{
    uint32_t h = ARG(0), dist = ARG(1), high = ARG(2), method = ARG(3);
    HandleObj *o = handle_get(h, H_FILE);
    if (!o) {
        win32_set_last_error(ERROR_INVALID_HANDLE);
        stdcall_ret(c, 0xFFFFFFFFu, 4);
        return;
    }
    int64_t off = (int32_t)dist;
    if (high)
        off = (int64_t)(((uint64_t)RD32(high) << 32) | dist);
    int whence = method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET;
    ReadBuf *b = o->ptr;
    off_t r;
    if (b && b->len && whence != SEEK_END) {
        off_t target = whence == SEEK_CUR ? b->base + (off_t)b->pos + (off_t)off : (off_t)off;
        if (target >= b->base && target <= b->base + (off_t)b->len) {
            b->pos = (uint32_t)(target - b->base);   /* inside the buffer: no system call */
            r = target;
        } else {
            b->len = b->pos = 0;
            r = lseek(o->fd, target, SEEK_SET);
        }
    } else {
        rb_drop(o);
        r = lseek(o->fd, (off_t)off, whence);
    }
    if (high)
        WR32(high, (uint32_t)((uint64_t)r >> 32));
    stdcall_ret(c, (uint32_t)r, 4);
}

SHIM(kernel32, SetEndOfFile)
{
    HandleObj *o = handle_get(ARG(0), H_FILE);
    int ok = 0;
    if (o) {
        rb_drop(o);
        off_t pos = lseek(o->fd, 0, SEEK_CUR);
        ok = ftruncate(o->fd, pos) == 0;
    }
    stdcall_ret(c, (uint32_t)ok, 1);
}

SHIM(kernel32, FlushFileBuffers) { stdcall_ret(c, 1, 1); }

SHIM(kernel32, DeleteFileA)
{
    char path[1100];
    win_path_to_host(gstr(ARG(0)), path, sizeof(path));
    stdcall_ret(c, unlink(path) == 0, 1);
}

static void unix_to_filetime(time_t t, uint32_t dst)
{
    uint64_t ft = ((uint64_t)t + 11644473600ull) * 10000000ull;
    WR64(dst, ft);
}

SHIM(kernel32, GetFileTime)
{
    HandleObj *o = handle_get(ARG(0), H_FILE);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0) {
        stdcall_ret(c, 0, 4);
        return;
    }
    for (int i = 1; i <= 3; i++)
        if (ARG(i))
            unix_to_filetime(st.st_mtime, ARG(i));
    stdcall_ret(c, 1, 4);
}

SHIM(kernel32, CompareFileTime)
{
    uint64_t a = RD64(ARG(0)), b = RD64(ARG(1));
    stdcall_ret(c, a < b ? 0xFFFFFFFFu : a > b ? 1u : 0u, 2);
}

SHIM(kernel32, GetDriveTypeA)
{
    uint32_t root = ARG(0);
    uint32_t t = 1;   /* DRIVE_NO_ROOT_DIR */
    if (root) {
        int d = toupper((unsigned char)*gstr(root));
        if (d == 'C')
            t = 3;    /* DRIVE_FIXED */
        else if (d == WIN_CD_DRIVE)
            t = 5;    /* DRIVE_CDROM */
    } else {
        t = 3;
    }
    stdcall_ret(c, t, 1);
}

/* FindFirstFile / FindNextFile over a directory with a case-insensitive wildcard. */
typedef struct FindState {
    DIR *dir;
    char dir_path[1100];
    char pattern[260];
} FindState;

static int wild_match(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (wild_match(p + 1, s))
                    return 1;
            return 0;
        }
        if (!*s)
            return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
            return 0;
    }
    if (*s == 0)
        return 1;
    /* "*.*" style patterns also match names without a dot. */
    return 0;
}

static int find_next(FindState *f, uint32_t data)
{
    struct dirent *e;
    while ((e = readdir(f->dir)) != NULL) {
        const char *pat = f->pattern;
        int match = wild_match(pat, e->d_name);
        if (!match && strcmp(pat, "*.*") == 0)
            match = 1;
        if (!match)
            continue;
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", f->dir_path, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0)
            continue;
        memset(gptr(data), 0, 320);
        WR32(data, S_ISDIR(st.st_mode) ? 0x10u : 0x20u);
        unix_to_filetime(st.st_mtime, data + 4u);
        unix_to_filetime(st.st_mtime, data + 12u);
        unix_to_filetime(st.st_mtime, data + 20u);
        WR32(data + 28u, (uint32_t)((uint64_t)st.st_size >> 32));
        WR32(data + 32u, (uint32_t)st.st_size);
        snprintf(gbuf(data + 44u), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

SHIM(kernel32, FindFirstFileA)
{
    const char *win = gstr(ARG(0));
    uint32_t data = ARG(1);
    char path[1100];
    win_path_to_host(win, path, sizeof(path));
    char *slash = strrchr(path, '/');
    FindState *f = calloc(1, sizeof(*f));
    if (slash) {
        *slash = 0;
        snprintf(f->dir_path, sizeof(f->dir_path), "%s", path);
        snprintf(f->pattern, sizeof(f->pattern), "%s", slash + 1);
    }
    f->dir = opendir(f->dir_path);
    if (!f->dir || !find_next(f, data)) {
        if (f->dir)
            closedir(f->dir);
        free(f);
        win32_set_last_error(ERROR_FILE_NOT_FOUND);
        stdcall_ret(c, INVALID_HANDLE_VALUE, 2);
        return;
    }
    uint32_t h = handle_new(H_FIND);
    handle_get(h, H_FIND)->ptr = f;
    stdcall_ret(c, h, 2);
}

SHIM(kernel32, FindNextFileA)
{
    HandleObj *o = handle_get(ARG(0), H_FIND);
    int ok = o && find_next(o->ptr, ARG(1));
    if (!ok)
        win32_set_last_error(ERROR_NO_MORE_FILES);
    stdcall_ret(c, (uint32_t)ok, 2);
}

SHIM(kernel32, FindClose)
{
    HandleObj *o = handle_get(ARG(0), H_FIND);
    if (o) {
        FindState *f = o->ptr;
        closedir(f->dir);
    }
    handle_close(ARG(0));
    stdcall_ret(c, 1, 1);
}

/* ---- time --------------------------------------------------------------- */

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
static uint64_t t0_us;

static uint64_t now_us(void) { return sceKernelGetProcessTimeWide(); }
#else
static uint64_t t0_us;

/* IGI_FAKE_CLOCK=N (debug): the clock advances N microseconds per query instead
 * of following real time, so a run (e.g. the benchmark with IGI_BENCH_SHOT) plays
 * the same frames every time and screenshots of two builds can be compared. */
static int fake_clock = -1;
static uint64_t fake_us, fake_step;

static uint64_t now_us(void)
{
    if (fake_clock < 0) {
        const char *e = getenv("IGI_FAKE_CLOCK");
        fake_step = e ? strtoull(e, NULL, 10) : 0u;
        fake_clock = fake_step != 0;
        fake_us = 1000000u;
    }
    if (fake_clock)
        return fake_us += fake_step;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}
#endif

/* Microseconds -> milliseconds without a 64-bit division (a library call on the
 * Vita's CPU; the game polls the tick count thousands of times per frame): only
 * the 32-bit delta since the previous call is divided (by a constant). */
static uint32_t us_to_ms(uint64_t us)
{
    static uint64_t base_us;
    static uint32_t base_ms;
    uint64_t d = us - base_us;
    if (us < base_us || d >= 0x7FFFFFFFull) {   /* rare: first call or a long gap */
        base_ms = (uint32_t)(us / 1000ull);
        base_us = (uint64_t)base_ms * 1000u;
        return base_ms;
    }
    uint32_t dms = (uint32_t)d / 1000u;
    base_us += (uint64_t)dms * 1000u;
    base_ms += dms;
    return base_ms;
}

uint32_t win32_tick_count(void)
{
    return us_to_ms(now_us() - t0_us);
}

uint64_t win32_perf_counter(void)
{
    return now_us() - t0_us;
}

/* Milliseconds since start, for log timestamps (valid before kernel32_init too). */
uint32_t win32_perf_counter_ms(void)
{
    if (!t0_us)
        t0_us = now_us();
    return (uint32_t)((now_us() - t0_us) / 1000ull);
}

/* Time the game spent waiting for its next fixed step (its 30 fps cap), for
 * benchmarks and the overlay; accumulated by the scheduler hook in
 * runtime/src/igi_fixes.c. */
uint64_t g_idle_us;

SHIM(kernel32, GetTickCount)
{
    stdcall_ret(c, us_to_ms(win32_perf_counter()) + 60000u, 0);
}

SHIM(kernel32, QueryPerformanceCounter)
{
    WR64(ARG(0), win32_perf_counter());
    stdcall_ret(c, 1, 1);
}

SHIM(kernel32, Sleep)
{
    usleep(ARG(0) * 1000u);
    stdcall_ret(c, 0, 1);
}

SHIM(kernel32, GetLocalTime)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t p = ARG(0);
    WR16(p + 0u, (uint32_t)tm.tm_year + 1900u);
    WR16(p + 2u, (uint32_t)tm.tm_mon + 1u);
    WR16(p + 4u, (uint32_t)tm.tm_wday);
    WR16(p + 6u, (uint32_t)tm.tm_mday);
    WR16(p + 8u, (uint32_t)tm.tm_hour);
    WR16(p + 10u, (uint32_t)tm.tm_min);
    WR16(p + 12u, (uint32_t)tm.tm_sec);
    WR16(p + 14u, 0);
    stdcall_ret(c, 0, 1);
}

/* ---- code pages / locale (Latin-1 only) --------------------------------- */

SHIM(kernel32, GetACP) { stdcall_ret(c, 1252, 0); }
SHIM(kernel32, GetOEMCP) { stdcall_ret(c, 437, 0); }

SHIM(kernel32, GetCPInfo)
{
    uint32_t p = ARG(1);
    memset(gptr(p), 0, 20);
    WR32(p, 1);
    WR8(p + 4u, '?');
    stdcall_ret(c, 1, 2);
}

SHIM(kernel32, MultiByteToWideChar)
{
    uint32_t src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR16(dst + (uint32_t)i * 2u, RD8(src + (uint32_t)i));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, WideCharToMultiByte)
{
    uint32_t src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++) {
            uint32_t ch = RD16(src + (uint32_t)i * 2u);
            WR8(dst + (uint32_t)i, ch < 256u ? ch : '?');
        }
    }
    stdcall_ret(c, (uint32_t)n, 8);
}

static uint32_t ctype1(uint32_t ch)
{
    uint32_t t = 0;
    if (ch >= 256u)
        return 0x100u;   /* C1_ALPHA */
    if (isupper((int)ch)) t |= 0x001u;
    if (islower((int)ch)) t |= 0x002u;
    if (isdigit((int)ch)) t |= 0x004u;
    if (isspace((int)ch)) t |= 0x008u;
    if (ispunct((int)ch)) t |= 0x010u;
    if (iscntrl((int)ch)) t |= 0x020u;
    if (ch == ' ' || ch == '\t') t |= 0x040u;
    if (isxdigit((int)ch)) t |= 0x080u;
    if (isalpha((int)ch)) t |= 0x100u;
    return t;
}

SHIM(kernel32, GetStringTypeA)
{
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    for (int32_t i = 0; i < n; i++)
        WR16(out + (uint32_t)i * 2u, ctype1(RD8(src + (uint32_t)i)));
    stdcall_ret(c, 1, 5);
}

SHIM(kernel32, GetStringTypeW)
{
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++)
        WR16(out + (uint32_t)i * 2u, ctype1(RD16(src + (uint32_t)i * 2u)));
    stdcall_ret(c, 1, 4);
}

static uint32_t map_char(uint32_t flags, uint32_t ch)
{
    if (ch < 256u && (flags & 0x100u))
        return (uint32_t)tolower((int)ch);
    if (ch < 256u && (flags & 0x200u))
        return (uint32_t)toupper((int)ch);
    return ch;
}

SHIM(kernel32, LCMapStringA)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0)
        n = (int32_t)strlen(gstr(src)) + 1;
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR8(dst + (uint32_t)i, map_char(flags, RD8(src + (uint32_t)i)));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, LCMapStringW)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4), dst_n = ARG(5);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) {
        n = 0;
        while (RD16(src + (uint32_t)n * 2u))
            n++;
        n++;
    }
    if (dst_n) {
        if ((uint32_t)n > dst_n)
            n = (int32_t)dst_n;
        for (int32_t i = 0; i < n; i++)
            WR16(dst + (uint32_t)i * 2u, map_char(flags, RD16(src + (uint32_t)i * 2u)));
    }
    stdcall_ret(c, (uint32_t)n, 6);
}

SHIM(kernel32, GetLocaleInfoA) { stdcall_ret(c, 0, 4); }

/* ---- init --------------------------------------------------------------- */

void kernel32_init(void)
{
    if (!t0_us)
        t0_us = now_us();
    cmdline = scratch_strdup(WIN_INSTALL_DIR "\\IGI.exe");
    env_block = scratch_alloc(4, 4);
    env_block_w = scratch_alloc(4, 4);
    for (int i = 0; i < 3; i++)
        std_handles[i] = handle_new(H_STD);
    if (!g_cd_dir[0])
        if (snprintf(g_cd_dir, sizeof(g_cd_dir), "%.1000s/..", g_game_dir) >= (int)sizeof(g_cd_dir))
            g_cd_dir[0] = 0;
}
