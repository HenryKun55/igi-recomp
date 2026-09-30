"""ARM (PS Vita) differential tester for the x86 -> C code generator.

The generated C is compiled with the Vita toolchain and flags (arm-vita-eabi-gcc,
Cortex-A9, Thumb-2, hard-float VFPv3/NEON) and executed inside Unicorn ARM. The
same inputs are run through:

  x86    the original IGI.exe code in Unicorn x86 (the reference),
  arm    the generated C built for the Vita, in Unicorn ARM (Cortex-A9 model),
  host   the generated C built natively on this machine (clang, like the macOS port).

Anything where `arm` differs from `host` is a Vita-only behavior difference (the
same C source gives different results); `arm` != `x86` with `arm` == `host` is a
general recompiler inaccuracy that the macOS build shares.

Modes:
  insn  every instruction form used by IGI.exe on its own (see difftest.py),
        all flags live, random register/memory/x87 states.
  func  whole recovered functions, all linked together, on random object graphs:
        registers and stack arguments point into a buffer of random floats,
        integers and pointers. Runs that fault, escape to imports/unknown code or
        exceed the instruction budget in the x86 reference are skipped.

Usage:
  armdifftest.py insn IGI.exe WORK_DIR [--samples N] [--runs N] [--fpscr HEX]
  armdifftest.py func IGI.exe WORK_DIR [--runs N] [--only A,B,...] [--x87-only]
                 [--jobs N] [--fpscr HEX] [--limit N]
  armdifftest.py func IGI.exe WORK_DIR --trace ADDR [--trace-run R]

--trace runs one function on the inputs of one run (--trace-run) and reports
  1. the first guest call whose entry state differs between the normal ARM build
     and x86 (non-intrusive: Unicorn block hooks on function entries), and
  2. from a whole-program build with a state probe before every guest
     instruction: the first instruction after which ARM and host (exact) or ARM
     and x86 (float tolerance) disagree, with its inputs.

--fpscr sets the ARM FPSCR for every call (default 0: IEEE, round to nearest, no
flush-to-zero, which is what Vita3K gives new threads; 0x03000000 is "RunFast":
flush-to-zero + default NaN).

Needs: unicorn + capstone (pip), ~/vitasdk (arm-vita-eabi-gcc), a host C compiler.
"""
import argparse
import functools
import math
import multiprocessing
import os
import random
import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_ARCH_X86, UC_MODE_32, UC_MODE_THUMB
from unicorn import UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn import arm_const as A
from unicorn import x86_const as U

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import difftest  # noqa: E402
from gen import FuncGen, Generator  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VITASDK = os.environ.get('VITASDK', os.path.expanduser('~/vitasdk'))
# The recompiled code is under test: no hooks to hand-decompiled native versions.
os.environ['IGI_NATIVE'] = '0'
VITA_CC = os.path.join(VITASDK, 'bin', 'arm-vita-eabi-gcc')
VITA_NM = os.path.join(VITASDK, 'bin', 'arm-vita-eabi-nm')
# Same as the Vita build of igi_recomp (CMakeLists.txt + vita.toolchain.cmake, Release):
# "-O3 ... -O2" so the generated code ends up at -O2; the runtime is built at -O3.
VITA_FLAGS = ['-std=gnu11', '-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-mthumb', '-fno-short-enums',
              '-ffunction-sections', '-fdata-sections', '-fno-pic', '-fno-pie', '-D__vita__',
              '-Wall', '-Wextra', '-Werror', '-fwrapv', '-fno-strict-aliasing', '-fsigned-char']
HOST_FLAGS = ['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fwrapv', '-fno-strict-aliasing', '-fsigned-char']

# Guest memory window shared by every engine: guest addresses [0, GSIZE).
GSIZE = 0x03000000
IMAGE_BASE = 0x00400000
CODE_END = difftest.CODE_END
TIB = 0x00020000
GDT = 0x00030000
BUF = 0x02000000            # random object graph (func mode)
BUF_SIZE = 0x00040000
ESP0 = 0x02EFF000           # initial guest esp (func mode)
STACK_WORDS = 32            # return address + arguments written at ESP0
RET_MAGIC = 0x00031000      # guest return address (holds hlt; the x86 run stops there)
THUNK_BASE = 0xFE000000     # IAT slots point here (unmapped: imports end the x86 run)

# ARM host layout inside Unicorn.
ARM_GMEM = 0x10000000
ARM_CTX = 0x0E100000
ARM_STOP = 0x0E200000
ARM_STACK_TOP = 0x0E000000

CTX_FMT = '<8I3I4x8dIII'
CTX_SIZE = struct.calcsize(CTX_FMT)

STUB_C = r'''
/* Minimal runtime for running generated guest code under the differential tester. */
#include "recomp_gen.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef __arm__
volatile uint32_t g_armdt_trap;
static void __attribute__((noreturn)) trap(uint32_t code)
{
    g_armdt_trap = code;
    for (;;)
        __asm__ volatile("udf #0");
}
int *__errno(void) { static int e; return &e; }
/* Plain loops: keep GCC from turning them back into memset/memcpy calls. */
#pragma GCC optimize("no-tree-loop-distribute-patterns")
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp < sp)
        while (n--) *dp++ = *sp++;
    else
        while (n--) dp[n] = sp[n];
    return d;
}
void *memcpy(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *memset(void *d, int v, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)v; return d; }
#else
#include <unistd.h>
static void __attribute__((noreturn)) trap(uint32_t code)
{
    printf("T %u\n", code);
    fflush(stdout);
    _exit(0);
}
#endif

GuestFn thunk_lookup(uint32_t a) { (void)a; return NULL; }
void rt_probe(Ctx *c, uint32_t f) { (void)c; (void)f; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; trap(1); }
void rt_missing_import(Ctx *c, const char *n) { (void)c; (void)n; trap(2); }
#ifdef ARMDT_NO_TABLES
const GuestFunc g_guest_funcs[1];
const uint32_t g_guest_func_count = 0;
const GuestImport g_guest_imports[1];
const uint32_t g_guest_import_count = 0;
#endif
'''

NATIVE_FUNC_MAIN = r'''
/* Host (native) runner for func mode: one forked child per test. */
#include "recomp_gen.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define GSIZE 0x03000000u

static uint8_t *pristine;

static void on_signal(int s)
{
    char msg[] = "T 9\n";
    msg[2] = (char)('0' + (s % 10));
    if (write(1, msg, 4) < 0)
        _exit(1);
    _exit(0);
}

static GuestFn find(uint32_t addr)
{
    for (uint32_t i = 0; i < g_guest_func_count; i++)
        if (g_guest_funcs[i].addr == addr)
            return g_guest_funcs[i].fn;
    return NULL;
}

static void run_child(uint32_t addr, const uint8_t *buf, uint32_t buf_size, char *p)
{
    Ctx c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 8; i++)
        c.r[i] = (uint32_t)strtoul(p, &p, 16);
    c.fcw = (uint32_t)strtoul(p, &p, 16);
    c.fs_base = (uint32_t)strtoul(p, &p, 16);
    uint32_t sp = c.r[4];
    int nstack = (int)strtol(p, &p, 10);
    memcpy(g_mem + BUF_ADDR, buf, buf_size);
    memcpy(pristine + BUF_ADDR, buf, buf_size);
    for (int i = 0; i < nstack; i++) {
        uint32_t v = (uint32_t)strtoul(p, &p, 16);
        WR32(sp + 4u * (uint32_t)i, v);
        memcpy(pristine + sp + 4u * (uint32_t)i, &v, 4);
    }
    signal(SIGSEGV, on_signal);
    signal(SIGBUS, on_signal);
    signal(SIGILL, on_signal);
    signal(SIGFPE, on_signal);
    signal(SIGALRM, on_signal);
    alarm(3);
    GuestFn f = find(addr);
    if (!f) {
        printf("T 3\n");
        return;
    }
    f(&c);
    alarm(0);
    printf("R");
    for (int i = 0; i < 8; i++)
        printf(" %08X", c.r[i]);
    printf(" %X %04X", c.ftop, c.fsw & 0x4700u);
    for (int i = 0; i < 8; i++) {
        uint64_t b;
        memcpy(&b, &c.st[i], 8);
        printf(" %016llX", (unsigned long long)b);
    }
    printf("\n");
    /* Changed guest memory, in runs. */
    uint32_t n = 0;
    for (uint32_t a = 0; a < GSIZE; a += 4096u) {
        if (!memcmp(g_mem + a, pristine + a, 4096u))
            continue;
        for (uint32_t b = a; b < a + 4096u; b++) {
            if (g_mem[b] == pristine[b])
                continue;
            uint32_t e = b;
            while (e < a + 4096u && g_mem[e] != pristine[e])
                e++;
            if (n < 0x01000000u) {
                printf("M %X ", b);
                for (uint32_t k = b; k < e; k++)
                    printf("%02X", g_mem[k]);
                printf("\n");
            }
            n += e - b;
            b = e;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    pristine = mmap(NULL, GSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    size_t isz = fread(g_mem + 0x400000u, 1, GSIZE - 0x400000u, f);
    fclose(f);
    memcpy(pristine + 0x400000u, g_mem + 0x400000u, isz);
    /* Thread information block for fs: accesses. */
    WR32(TIB_ADDR, 0xFFFFFFFFu);
    memcpy(pristine + TIB_ADDR, g_mem + TIB_ADDR, 4);
    uint32_t buf_size = (uint32_t)strtoul(argv[3], NULL, 0);
    f = fopen(argv[2], "rb");
    if (!f)
        return 2;
    fseek(f, 0, SEEK_END);
    long bsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *bufs = malloc((size_t)bsz);
    if (fread(bufs, 1, (size_t)bsz, f) != (size_t)bsz)
        return 2;
    fclose(f);
    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        char *p = line;
        uint32_t addr = (uint32_t)strtoul(p, &p, 16);
        uint32_t run = (uint32_t)strtoul(p, &p, 10);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            run_child(addr, bufs + (size_t)run * buf_size, buf_size, p);
            fflush(stdout);
            _exit(0);
        }
        int st;
        waitpid(pid, &st, 0);
        printf("E\n");
        fflush(stdout);
    }
    return 0;
}
'''


TRACE_H = r"""
/* Per-instruction guest state trace used by armdifftest.py --trace. */
#ifndef ARMDT_TRACE_H
#define ARMDT_TRACE_H
#include "recomp.h"
void armdt_trace(Ctx *c, uint32_t a);
#define TRACE(a) do { SPILL; armdt_trace(c, (a)); } while (0)
#endif
"""

TRACE_C = r"""
#include "armdt_trace.h"

typedef struct TraceRec {
    uint32_t addr, r[8], ftop, fsw, pad;
    double st[8];
} TraceRec;

#define TRACE_MAX 200000u
TraceRec g_armdt_trace[TRACE_MAX];
uint32_t g_armdt_trace_n;

void armdt_trace(Ctx *c, uint32_t a)
{
    if (g_armdt_trace_n >= TRACE_MAX)
        return;
    TraceRec *t = &g_armdt_trace[g_armdt_trace_n++];
    t->addr = a;
    memcpy(t->r, c->r, sizeof(t->r));
    t->ftop = c->ftop;
    t->fsw = c->fsw;
    t->pad = 0;
    memcpy(t->st, c->st, sizeof(t->st));
}
"""

TRACE_HOST_MAIN = r"""
/* Host runner for one traced call (see TRACE_C). */
#include "recomp_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

typedef struct TraceRec {
    uint32_t addr, r[8], ftop, fsw, pad;
    double st[8];
} TraceRec;
extern TraceRec g_armdt_trace[];
extern uint32_t g_armdt_trace_n;

static int load(const char *path, uint32_t addr, long skip, long n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, skip, SEEK_SET);
    size_t got = fread(g_mem + addr, 1, n > 0 ? (size_t)n : (size_t)(0x03000000u - addr), f);
    fclose(f);
    return got > 0;
}

int main(int argc, char **argv)
{
    /* image bufs run buf_size entry, then (stdin) regs, fcw, fs_base, nstack, stack words */
    if (argc < 6)
        return 2;
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    long run = strtol(argv[3], NULL, 0), bsz = strtol(argv[4], NULL, 0);
    if (!load(argv[1], 0x400000u, 0, 0) || !load(argv[2], BUF_ADDR, run * bsz, bsz))
        return 2;
    WR32(TIB_ADDR, 0xFFFFFFFFu);
    Ctx c;
    memset(&c, 0, sizeof(c));
    for (int i = 0; i < 8; i++)
        if (scanf("%x", &c.r[i]) != 1)
            return 2;
    int n;
    if (scanf("%x %x %d", &c.fcw, &c.fs_base, &n) != 3)
        return 2;
    for (int i = 0; i < n; i++) {
        uint32_t v;
        if (scanf("%x", &v) != 1)
            return 2;
        WR32(c.r[4] + 4u * (uint32_t)i, v);
    }
    uint32_t entry = (uint32_t)strtoul(argv[5], NULL, 16);
    for (uint32_t i = 0; i < g_guest_func_count; i++)
        if (g_guest_funcs[i].addr == entry)
            g_guest_funcs[i].fn(&c);
    fwrite(g_armdt_trace, sizeof(TraceRec), g_armdt_trace_n, stdout);
    return 0;
}
"""
TRACE_REC = '<12I8d'
TRACE_REC_SIZE = struct.calcsize(TRACE_REC)


# ------------------------------------------------------------------ building

def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0 or r.stderr.strip():
        raise RuntimeError('command failed (or warned):\n%s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


def compile_many(jobs, cc, flags, srcs, objdir, incs):
    """Compile sources in parallel, skipping objects newer than their source."""
    os.makedirs(objdir, exist_ok=True)
    todo, objs = [], []
    for s in srcs:
        o = os.path.join(objdir, os.path.basename(s) + '.o')
        objs.append(o)
        if not os.path.exists(o) or os.path.getmtime(o) < os.path.getmtime(s):
            todo.append([cc] + flags + ['-I' + i for i in incs] + ['-c', s, '-o', o])
    if todo:
        with multiprocessing.Pool(jobs) as pool:
            pool.map(sh, todo)
    return objs


def write_if_changed(path, text):
    if os.path.exists(path) and open(path).read() == text:
        return
    with open(path, 'w') as f:
        f.write(text)


def vita_link(objs, out, entry):
    libdir = os.path.join(VITASDK, 'arm-vita-eabi', 'lib')
    libgcc = sh([VITA_CC] + VITA_FLAGS[:5] + ['-print-libgcc-file-name']).strip()
    sh([VITA_CC] + VITA_FLAGS[:5] + ['-nostdlib', '-nostartfiles', '-static', '-Wl,--gc-sections',
                                     '-Wl,--no-enum-size-warning', '-Wl,-e,' + entry, '-Wl,-Ttext=0x100000',
                                     '-o', out] + objs +
       [os.path.join(libdir, 'libm.a'), os.path.join(libdir, 'libc.a'), libgcc])


def guest_image(an, fix_iat=True):
    """Guest bytes [IMAGE_BASE, CODE_END) as the runtime loads them."""
    image = bytearray(CODE_END - IMAGE_BASE)
    for sec in an.pe.sections:
        raw = an.pe.data[sec.raw_off:sec.raw_off + sec.raw_size]
        off = sec.va - IMAGE_BASE
        image[off:off + len(raw)] = raw[:max(0, len(image) - off)]
    if fix_iat:
        for i, slot in enumerate(sorted(an.pe.imports)):
            struct.pack_into('<I', image, slot - IMAGE_BASE, THUNK_BASE + 4 * i)
    return bytes(image)


# ------------------------------------------------------------------ engines

@functools.lru_cache(maxsize=4)
def load_elf(path):
    data = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from('<8I', data,
                                                                                      phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_vaddr, data[p_off:p_off + p_filesz], p_memsz, p_flags))
    syms = {}
    for line in subprocess.check_output([VITA_NM, path], text=True).splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    return segs, syms


class ArmEngine:
    """Generated C compiled for the Vita, executed by Unicorn's Cortex-A9 model."""

    def __init__(self, elf, fpscr=0):
        segs, self.syms = load_elf(elf)
        self.segs = segs
        self.dirty = False
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A9)
        self.rw = []
        for va, blob, memsz, flags in segs:
            lo = va & ~0xFFF
            hi = (va + memsz + 0xFFF) & ~0xFFF
            uc.mem_map(lo, hi - lo)
            uc.mem_write(va, blob)
            if flags & 2:
                self.rw.append((va, bytes(blob) + bytes(memsz - len(blob))))
        uc.mem_map(ARM_STACK_TOP - 0x100000, 0x100000)
        uc.mem_map(ARM_CTX, 0x1000)
        uc.mem_map(ARM_STOP, 0x1000)
        uc.mem_map(ARM_GMEM, GSIZE)
        uc.mem_write(self.syms['g_mem'], struct.pack('<I', ARM_GMEM))
        for va, blob in self.rw:
            if va <= self.syms['g_mem'] < va + len(blob):
                o = self.syms['g_mem'] - va
                self.rw[self.rw.index((va, blob))] = (va, blob[:o] + struct.pack('<I', ARM_GMEM) + blob[o + 4:])
        # No memory hooks here: with a UC_HOOK_MEM_WRITE installed, Unicorn 2.1 ARM
        # executes some generated functions wrongly (e.g. f_00401B20 reads a wild
        # address). Written guest memory is found by diffing instead (dirty_ranges).
        self.fpscr = fpscr

    def dirty_ranges(self, expect, chunk=0x10000):
        """Byte ranges [a, b) of guest memory that differ from `expect`."""
        mem = self.uc.mem_read(ARM_GMEM, GSIZE)
        out = []
        for c in range(0, GSIZE, chunk):
            if mem[c:c + chunk] == expect[c:c + chunk]:
                continue
            m, e = mem[c:c + chunk], expect[c:c + chunk]
            i = 0
            while i < chunk:
                if m[i] != e[i]:
                    j = i
                    while j < chunk and m[j] != e[j]:
                        j += 1
                    out.append([c + i, c + j])
                    i = j
                else:
                    i += 1
        return out

    def gwrite(self, a, data):
        self.uc.mem_write(ARM_GMEM + a, bytes(data))

    def gread(self, a, n):
        return bytes(self.uc.mem_read(ARM_GMEM + a, n))

    def call(self, sym, ctx, r1=0, count=50_000_000):
        """Run sym(Ctx *c[, r1]) and return (status, ctx bytes)."""
        uc = self.uc
        if self.dirty:
            # The previous call failed and may have scribbled over code or data.
            for va, blob, _, _ in self.segs:
                uc.mem_write(va, blob)
            self.dirty = False
        for va, blob in self.rw:
            uc.mem_write(va, blob)
        uc.mem_write(ARM_CTX, ctx)
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        uc.reg_write(A.UC_ARM_REG_FPSCR, self.fpscr)
        uc.reg_write(A.UC_ARM_REG_SP, ARM_STACK_TOP - 0x100)
        uc.reg_write(A.UC_ARM_REG_R0, ARM_CTX)
        uc.reg_write(A.UC_ARM_REG_R1, r1)
        uc.reg_write(A.UC_ARM_REG_LR, ARM_STOP | 1)
        try:
            uc.emu_start(self.syms[sym] | 1, ARM_STOP, timeout=20_000_000, count=count)
        except UcError as e:
            self.dirty = True
            trap = struct.unpack('<I', uc.mem_read(self.syms['g_armdt_trap'], 4))[0]
            if trap:
                uc.mem_write(self.syms['g_armdt_trap'], b'\0' * 4)
                return 'trap%d' % trap, None
            return 'fault %s pc=%08X' % (e, uc.reg_read(A.UC_ARM_REG_PC)), None
        if uc.reg_read(A.UC_ARM_REG_PC) & ~1 != ARM_STOP:
            self.dirty = True
            return 'limit', None
        return 'ok', bytes(uc.mem_read(ARM_CTX, CTX_SIZE))


def make_ctx(regs, fcw, flags=0, ftop=0, st=None, fs_base=0):
    st = st or [0.0] * 8
    return struct.pack(CTX_FMT, *regs, 0, flags, fs_base, *st, ftop, fcw, 0)


def parse_ctx(b):
    v = struct.unpack(CTX_FMT, b)
    return {'r': list(v[0:8]), 'eflags': v[9], 'st': list(v[11:19]), 'ftop': v[19], 'fcw': v[20], 'fsw': v[21]}


def f80_to_double(t):
    return difftest.f80_to_double(t)


class X86Engine:
    """The original code in Unicorn x86 with the same guest memory window."""

    def __init__(self, image, record=True):
        uc = self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(0, GSIZE)
        uc.mem_write(IMAGE_BASE, image)
        uc.mem_write(TIB, struct.pack('<I', 0xFFFFFFFF))
        # Flat 32-bit code/data segments plus an fs segment whose base is the TIB (fs:[0]
        # SEH chain). All selectors must be loaded: writing only fs leaves Unicorn with
        # a broken code segment (near returns land at the wrong address).
        def desc(base, limit, access, flags):
            return struct.pack('<HHBBBB', limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access,
                               ((limit >> 16) & 0xF) | (flags << 4), (base >> 24) & 0xFF)
        gdt = (bytes(8) + desc(0, 0xFFFFF, 0x9A, 0xC) + desc(0, 0xFFFFF, 0x92, 0xC) +
               desc(TIB, 0xFFFFF, 0x92, 0xC))
        uc.mem_write(GDT, gdt)
        uc.mem_write(RET_MAGIC, b'\xf4')
        uc.reg_write(U.UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
        uc.reg_write(U.UC_X86_REG_CS, 0x08)
        for r in (U.UC_X86_REG_DS, U.UC_X86_REG_ES, U.UC_X86_REG_SS):
            uc.reg_write(r, 0x10)
        uc.reg_write(U.UC_X86_REG_FS, 0x18)
        self.writes = []
        if record:
            uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        # Every run starts from this CPU state (a faulting run may leave odd segment/FPU state).
        self.cpu0 = uc.context_save()

    def _on_write(self, uc, access, addr, size, value, user):
        self.writes.append((addr, size))

    def run(self, entry, regs, fcw, count=2_000_000):
        uc = self.uc
        uc.context_restore(self.cpu0)
        pro = b'\xdb\xe3' + b'\xd9\x2d' + struct.pack('<I', GDT + 0x100)
        uc.mem_write(GDT + 0x100, struct.pack('<H', fcw))
        uc.mem_write(GDT + 0x200, pro)
        uc.emu_start(GDT + 0x200, GDT + 0x200 + len(pro))
        for i, r in enumerate(difftest.REGS):
            uc.reg_write(r, regs[i])
        uc.reg_write(U.UC_X86_REG_EFLAGS, 0x202)
        self.writes = []
        try:
            uc.emu_start(entry, RET_MAGIC, count=count)
        except UcError as e:
            pc = uc.reg_read(U.UC_X86_REG_EIP)
            if pc >= THUNK_BASE and pc < THUNK_BASE + 0x10000:
                return 'import', None
            return 'fault %s eip=%08X' % (e, pc), None
        pc = uc.reg_read(U.UC_X86_REG_EIP)
        if pc != RET_MAGIC:
            return 'limit', None
        out = [uc.reg_read(r) for r in difftest.REGS]
        fsw = uc.reg_read(U.UC_X86_REG_FPSW)
        top = (fsw >> 11) & 7
        phys = [f80_to_double(uc.reg_read(U.UC_X86_REG_FP0 + i)) for i in range(8)]
        return 'ok', {'r': out, 'fsw': fsw, 'ftop': top, 'st': [phys[(top + i) & 7] for i in range(8)]}


# ------------------------------------------------------------------ comparison helpers

def dbits(v):
    return struct.unpack('<Q', struct.pack('<d', v))[0]


def same_double(a, b, exact):
    if a != a and b != b:
        return True
    if exact:
        return dbits(a) == dbits(b)
    return a == b or abs(a - b) <= 1e-9 * max(1.0, abs(b))


def fmt_double(v):
    return '%r(%016X)' % (v, dbits(v))


def describe_bytes(a, x, y):
    """Human-readable difference of two byte strings at guest address a."""
    out = []
    for off in range(0, len(x), 4):
        w0, w1 = x[off:off + 4], y[off:off + 4]
        if w0 == w1:
            continue
        s = '%08X: %s vs %s' % (a + off, w0.hex(), w1.hex())
        if len(w0) == 4 and (a + off) % 4 == 0:
            f0, f1 = struct.unpack('<f', w0)[0], struct.unpack('<f', w1)[0]
            i0, i1 = struct.unpack('<i', w0)[0], struct.unpack('<i', w1)[0]
            s += ' (f32 %.9g vs %.9g, i32 %d vs %d)' % (f0, f1, i0, i1)
        out.append(s)
    return out


def float_close(x, y):
    """Byte strings equal up to float32/float64 rounding noise."""
    if len(x) % 4:
        return False
    for off in range(0, len(x), 4):
        w0, w1 = x[off:off + 4], y[off:off + 4]
        if w0 == w1:
            continue
        f0, f1 = struct.unpack('<f', w0)[0], struct.unpack('<f', w1)[0]
        if f0 != f0 and f1 != f1:
            continue
        if not (math.isfinite(f0) and math.isfinite(f1)):
            return False
        if abs(f0 - f1) > 2e-6 * max(abs(f0), abs(f1), 1e-30):
            return False
    return True


# ================================================================== insn mode

ARM_INSN_MAIN = r'''
typedef uint64_t (*TestFn)(Ctx *);
static const TestFn tests[] = { %TABLE% };
void armdt_run_test(Ctx *c, int idx);
void armdt_run_test(Ctx *c, int idx) { tests[idx](c); }
'''


def insn_mode(args):
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    g = Generator(args.exe)
    tests = difftest.collect_forms(g.an, g.funcs, args.samples)
    parts, keep = [], []
    for ins in tests:
        try:
            # x87 forms run both with the stack in locals and in the context copy.
            for mode in difftest.test_modes(g, ins):
                parts.append(difftest.emit_test(g, ins, len(keep), mode))
                keep.append(ins)
        except Exception as e:  # noqa: BLE001 - unsupported form in isolation
            print('skip', ins.text, e)
    print('instruction samples:', len(keep))
    image = guest_image(g.an, fix_iat=False)
    with open(os.path.join(work, 'image.bin'), 'wb') as f:
        f.write(image)
    gen_dir = os.path.join(work, 'gen')
    os.makedirs(gen_dir, exist_ok=True)
    g.write_header(gen_dir)
    rt_inc = os.path.join(ROOT, 'runtime', 'include')
    # The single-instruction test bodies are not warning-clean on their own (unused
    # labels/variables); only the generated functions themselves must be.
    tests_src = ('#include "recomp_gen.h"\n#pragma GCC diagnostic ignored "-Wunused-label"\n'
                 '#pragma GCC diagnostic ignored "-Wunused-variable"\n'
                 '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n' + ''.join(parts) +
                 ARM_INSN_MAIN.replace('%TABLE%', ', '.join('t_%d' % i for i in range(len(keep)))))
    write_if_changed(os.path.join(work, 'insn_tests.c'), tests_src)
    write_if_changed(os.path.join(work, 'stub.c'), STUB_C)
    # ARM build.
    objs = compile_many(args.jobs, VITA_CC, VITA_FLAGS + ['-O2', '-DARMDT_NO_TABLES'],
                        [os.path.join(work, 'insn_tests.c'), os.path.join(work, 'stub.c')],
                        os.path.join(work, 'arm_insn_obj'), [gen_dir, rt_inc])
    objs += compile_many(1, VITA_CC, VITA_FLAGS + ['-O3'], [os.path.join(ROOT, 'runtime/src/cpu.c')],
                         os.path.join(work, 'arm_insn_obj'), [gen_dir, rt_inc])
    elf = os.path.join(work, 'insn_arm.elf')
    vita_link(objs, elf, 'armdt_run_test')
    # Host build (difftest's harness, same test bodies).
    src = difftest.HARNESS.replace('%TESTS%', ''.join(parts)).replace(
        '%TABLE%', ', '.join('t_%d' % i for i in range(len(keep)))).replace('%IMGSIZE%', str(len(image)))
    write_if_changed(os.path.join(work, 'harness.c'), src)
    host = os.path.join(work, 'harness')
    sh(['cc', '-O2', '-w', '-std=gnu11', '-fwrapv', '-fno-strict-aliasing', '-fsigned-char', '-o', host,
        os.path.join(work, 'harness.c'), '-I' + gen_dir, '-I' + rt_inc, os.path.join(ROOT, 'runtime/src/cpu.c'),
        '-lm'])

    arm = ArmEngine(elf, args.fpscr)
    arm.gwrite(IMAGE_BASE, image)
    rnd = random.Random(1234)
    lines, cases = [], []
    for idx, ins in enumerate(keep):
        for _ in range(args.runs):
            regs, flags, fcw, vals = difftest.random_state(ins, rnd)
            if rnd.random() < 0.15:
                # Denormal / tiny / huge operands probe flush-to-zero and overflow paths.
                vals = [rnd.choice([1e-310, -3e-320, 1e-39, 2.5e-40, 1e300, -1e300, 3.4e38, 0.0, -0.0]) if
                        rnd.random() < 0.5 else v for v in vals]
            addrs = difftest.mem_addrs(ins, regs) + [regs[4]]
            if ins.mnem.startswith('rep') or ins.mnem.split()[-1] in ('movsb', 'movsd', 'stosb', 'stosd'):
                addrs += [regs[6], regs[7]]
            mem = []
            for a in addrs[:6]:
                if not (difftest.SCRATCH <= a < difftest.SCRATCH + difftest.SCRATCH_SIZE - 16 or
                        0x536000 <= a < CODE_END - 16):
                    continue
                data = bytes(rnd.getrandbits(8) for _ in range(16))
                if rnd.random() < 0.3:
                    # Plausible float32/float64 values in memory operands.
                    fv = rnd.choice([rnd.uniform(-1e4, 1e4), rnd.uniform(-2, 2), 1e-40, -2.9e-39, 0.5, -1.5,
                                     2147483648.0, -2147483649.0, 4294967295.0, 65535.5, 1e20])
                    data = struct.pack('<ff', fv, fv) + struct.pack('<d', fv)
                mem.append((a, data))
            try:
                res = difftest.run_unicorn(image, ins, regs, flags, fcw, vals, mem)
            except UcError:
                continue
            lines.append('%d %s %X %X 0 %s %d %s' % (
                idx, ' '.join('%X' % r for r in regs), flags, fcw, ' '.join('%016X' % dbits(v) for v in vals),
                len(mem), ' '.join('%X %s' % (a, d.hex()) for a, d in mem)))
            cases.append((idx, ins, regs, flags, fcw, vals, mem, res))
    p = subprocess.run([host], input='\n'.join(lines) + '\n', capture_output=True, text=True, cwd=work)
    host_out = p.stdout.strip().split('\n')
    print('tests: %d' % len(cases))

    fails_x86, fails_host = {}, {}
    for (idx, ins, regs, flags, fcw, vals, mem, res), hline in zip(cases, host_out):
        # ARM run. Like difftest's host harness, clear what earlier tests left around
        # esi/edi (string operations read past the initialised bytes).
        for k in (6, 7):
            if 0x02000100 <= regs[k] < 0x02E00000:
                arm.gwrite(regs[k] - 0x100, bytes(0x300))
        touched = set()
        for a, d in mem:
            arm.gwrite(a, d)
            touched.add(a)
        status, cb = arm.call('armdt_run_test', make_ctx(regs, fcw, flags, 0, vals), idx)
        arm_mem = [arm.gread(a, 16) for a, _ in mem]
        # Restore memory windows (the image part matters for later tests).
        for a, _ in mem:
            if IMAGE_BASE <= a < CODE_END:
                arm.gwrite(a, image[a - IMAGE_BASE:a - IMAGE_BASE + 16])
        text = ins.text
        inp = 'regs %s fl %X fcw %X st %s mem %s' % (' '.join('%08X' % r for r in regs), flags, fcw,
                                                   ' '.join(repr(v) for v in vals),
                                                   ' '.join('%X:%s' % (a, d.hex()) for a, d in mem))
        if status != 'ok':
            fails_x86.setdefault(text, []).append('arm %s | %s' % (status, inp))
            continue
        c = parse_ctx(cb)
        c_st = [c['st'][(c['ftop'] + i) & 7] for i in range(8)]
        # 1) ARM vs host: must be bit-identical.
        hp = []
        f = hline.split()
        if hline.startswith('R') and len(f) >= 20:
            h_regs = [int(x, 16) for x in f[1:9]]
            h_fl, h_top, h_fsw = int(f[9], 16), int(f[10], 16), int(f[11], 16)
            h_st = [struct.unpack('<d', struct.pack('<Q', int(x, 16)))[0] for x in f[12:20]]
            h_mem = [bytes.fromhex(x[1:]) for x in f[20:]]
            for i in range(8):
                if h_regs[i] != c['r'][i]:
                    hp.append('%s arm %08X host %08X' % (difftest.NAMES[i], c['r'][i], h_regs[i]))
            if h_fl != c['eflags']:
                hp.append('eflags arm %X host %X' % (c['eflags'], h_fl))
            if h_top != c['ftop'] or h_fsw != c['fsw'] & 0x4700:
                hp.append('ftop/fsw arm %X/%04X host %X/%04X' % (c['ftop'], c['fsw'] & 0x4700, h_top, h_fsw))
            for i in range(8):
                if not same_double(c_st[i], h_st[i], True):
                    hp.append('st%d arm %s host %s' % (i, fmt_double(c_st[i]), fmt_double(h_st[i])))
            for m, (a, _) in enumerate(mem):
                if m < len(h_mem) and h_mem[m] != arm_mem[m]:
                    hp.append('mem ' + '; '.join(describe_bytes(a, arm_mem[m], h_mem[m])))
        else:
            hp.append('host: ' + hline)
        if hp:
            fails_host.setdefault(text, []).append('; '.join(hp) + '  | ' + inp)
        # 2) ARM vs x86 (difftest's tolerance and flag rules).
        u_regs, u_ef, u_fsw, u_top, u_st, u_mem = res
        problems = []
        for i in range(8):
            if c['r'][i] != u_regs[i]:
                problems.append('%s %08X vs %08X' % (difftest.NAMES[i], c['r'][i], u_regs[i]))
        base = ins.mnem.split()[-1]
        for name, bit in difftest.FLAG_BITS.items():
            if base in ('mul', 'imul') and name in ('zf', 'sf', 'pf'):
                continue
            if base in ('div', 'idiv', 'bsf', 'bsr') and name != 'zf':
                continue
            if base in ('shl', 'shr', 'sar', 'rol', 'ror', 'shld', 'shrd') and name == 'of':
                continue
            if ((c['eflags'] >> bit) & 1) != ((u_ef >> bit) & 1):
                problems.append('%s %d vs %d' % (name, (c['eflags'] >> bit) & 1, (u_ef >> bit) & 1))
        if c['ftop'] != u_top:
            problems.append('ftop %d vs %d' % (c['ftop'], u_top))
        cc_mask = 0x4500 if base.startswith('f') and base not in ('fprem', 'fprem1', 'fsqrt', 'fyl2x') else 0
        if (c['fsw'] & cc_mask) != (u_fsw & cc_mask):
            problems.append('fsw %04X vs %04X' % (c['fsw'] & cc_mask, u_fsw & cc_mask))
        for i in range(8):
            if not same_double(c_st[i], u_st[i], False):
                problems.append('st%d %r vs %r' % (i, c_st[i], u_st[i]))
        for m, (a, _) in enumerate(mem):
            if arm_mem[m] != u_mem[m]:
                problems.append('mem ' + '; '.join(describe_bytes(a, arm_mem[m], u_mem[m])))
        if problems:
            # Tagged per case: does the host build agree with the ARM build here?
            fails_x86.setdefault(text, []).append(('arm!=host ' if hp else 'arm==host ') + '; '.join(problems) +
                                                  '  | ' + inp)

    print('\n== ARM (Vita GCC) vs host build: %d instruction forms differ' % len(fails_host))
    for text, probs in sorted(fails_host.items()):
        print('VITA-ONLY %-45s (%d)  e.g. %s' % (text, len(probs), probs[0]))
    print('\n== ARM (Vita GCC) vs x86 reference: %d instruction forms differ' % len(fails_x86))
    for text, probs in sorted(fails_x86.items()):
        vita = [p for p in probs if p.startswith('arm!=host')]
        print('FAIL %-45s (%d, %d of them Vita-only)  e.g. %s' % (text, len(probs), len(vita), (vita or probs)[0]))


# ================================================================== func mode

F_INTEREST = ('sar', 'shr', 'shl', 'idiv', 'div', 'imul', 'movsx', 'cdq', 'sbb', 'adc', 'neg')


def func_priority(g, fn):
    """Higher for functions heavy in x87 math and sign/shift/division sensitive integer ops."""
    x87 = sum(1 for i in fn.insns.values() if bytes(i.raw.bytes)[0] in range(0xD8, 0xE0))
    other = sum(1 for i in fn.insns.values() if i.mnem.split()[-1] in F_INTEREST)
    return x87 * 4 + other, x87


def gen_buffers(nruns, seed=99):
    """Random object graphs: mixtures of floats, small ints, zeros and pointers into the buffer."""
    out = []
    for r in range(nruns):
        rnd = random.Random(seed * 1000 + r)
        words = []
        for _ in range(BUF_SIZE // 4):
            k = rnd.random()
            if k < 0.30:
                w = struct.unpack('<I', struct.pack('<f', rnd.uniform(-200.0, 200.0)))[0]
            elif k < 0.45:
                w = struct.unpack('<I', struct.pack('<f', rnd.uniform(-1.0, 1.0)))[0]
            elif k < 0.60:
                w = BUF + rnd.randrange(0, BUF_SIZE * 3 // 4) & ~3
            elif k < 0.75:
                w = rnd.randrange(0, 16)
            elif k < 0.85:
                w = 0
            elif k < 0.90:
                w = struct.unpack('<I', struct.pack('<f', rnd.uniform(-1e5, 1e5)))[0]
            elif k < 0.91:
                w = struct.unpack('<I', struct.pack('<f', rnd.choice([1e-40, -1e-41, 1e-38, 3e-39])))[0]
            elif k < 0.95:
                w = rnd.randrange(0, 0x100)
            else:
                w = rnd.getrandbits(32)
            words.append(w)
        out.append(struct.pack('<%dI' % len(words), *words))
    return out


def func_inputs(entry, r):
    rnd = random.Random('%08X/%d' % (entry, r))

    def val():
        k = rnd.random()
        if k < 0.6:
            return BUF + rnd.randrange(0, BUF_SIZE * 3 // 4) & ~3
        if k < 0.75:
            return struct.unpack('<I', struct.pack('<f', rnd.uniform(-100, 100)))[0]
        if k < 0.9:
            return rnd.randrange(0, 16)
        return rnd.getrandbits(32)
    regs = [val() for _ in range(8)]
    regs[4] = ESP0
    stack = [RET_MAGIC] + [val() for _ in range(STACK_WORDS - 1)]
    return regs, stack


def merge(intervals):
    out = []
    for a, n in sorted(intervals):
        if out and a <= out[-1][1]:
            out[-1][1] = max(out[-1][1], a + n)
        else:
            out.append([a, a + n])
    return out


class FuncWorker:
    def __init__(self, cfg):
        self.cfg = cfg
        image = open(cfg['image'], 'rb').read()
        self.image = image
        self.bufs = gen_buffers(cfg['runs'])
        self.x86 = X86Engine(image)
        self.arm = ArmEngine(cfg['elf'], cfg['fpscr'])
        self.arm.gwrite(IMAGE_BASE, image)
        self.arm.gwrite(TIB, struct.pack('<I', 0xFFFFFFFF))
        # Initial guest memory of the current run: image + TIB + per-run buffer and stack.
        self.expect = bytearray(GSIZE)
        self.expect[IMAGE_BASE:CODE_END] = image
        self.expect[TIB:TIB + 4] = b'\xff' * 4
        self.host = subprocess.Popen([cfg['host'], cfg['image'], cfg['bufs'], str(BUF_SIZE)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def pristine(self, a, n, r, stack):
        """Initial guest bytes of the current run (self.expect is kept up to date)."""
        return bytes(self.expect[a:a + n])

    def restore(self, eng_write, ivs, r, stack):
        for a, b in ivs:
            eng_write(a, self.pristine(a, b - a, r, stack))

    def host_run(self, entry, r, regs, stack):
        self.host.stdin.write('%X %d %s %X %X %d %s\n' % (
            entry, r, ' '.join('%X' % x for x in regs), 0x27F, TIB, len(stack), ' '.join('%X' % x for x in stack)))
        self.host.stdin.flush()
        lines = []
        while True:
            ln = self.host.stdout.readline()
            if not ln or ln.strip() == 'E':
                break
            lines.append(ln.rstrip('\n'))
        if not lines or not lines[0].startswith('R'):
            return (lines[0] if lines else 'no output'), None, None
        f = lines[0].split()
        st = {'r': [int(x, 16) for x in f[1:9]], 'ftop': int(f[9], 16), 'fsw': int(f[10], 16),
              'st': [struct.unpack('<d', struct.pack('<Q', int(x, 16)))[0] for x in f[11:19]]}
        mem = {}
        for ln in lines[1:]:
            if ln.startswith('M '):
                _, a, hx = ln.split()
                mem[int(a, 16)] = bytes.fromhex(hx)
        return 'ok', st, mem

    def test(self, entry):
        cfg = self.cfg
        res = {'entry': entry, 'ran': 0, 'skipped': {}, 'vita': [], 'x86': [], 'x86_fp': 0}
        for r in range(cfg['runs']):
            regs, stack = func_inputs(entry, r)
            stack_bytes = struct.pack('<%dI' % len(stack), *stack)
            for w in (self.x86.uc.mem_write, self.arm.gwrite):
                w(BUF, self.bufs[r])
                w(ESP0, stack_bytes)
            self.expect[BUF:BUF + BUF_SIZE] = self.bufs[r]
            self.expect[ESP0:ESP0 + len(stack_bytes)] = stack_bytes
            xs, xst = self.x86.run(entry, regs, 0x27F)
            xw = merge(self.x86.writes)
            aw = []
            try:
                if xs != 'ok':
                    # Not comparable. The C code is not run either: with wild guest
                    # addresses, g_mem + addr wraps around the 32-bit ARM address
                    # space and would corrupt the tester itself.
                    res['skipped'][xs.split()[0]] = res['skipped'].get(xs.split()[0], 0) + 1
                    continue
                as_, acb = self.arm.call('w_%08X' % entry, make_ctx(regs, 0x27F, fs_base=TIB))
                aw = self.arm.dirty_ranges(self.expect)
                if as_ == 'limit':
                    # Out of emulation budget (e.g. a big rep stos through the byte-loop
                    # memset of the stub runtime): not a behavior difference.
                    res['skipped']['arm-limit'] = res['skipped'].get('arm-limit', 0) + 1
                    continue
                ivs = merge([(a, b - a) for a, b in xw + aw if a < GSIZE])
                x_mem = {a: bytes(self.x86.uc.mem_read(a, b - a)) for a, b in ivs}
                a_mem = {a: self.arm.gread(a, b - a) for a, b in ivs}
                hs, hst, hmem = self.host_run(entry, r, regs, stack)
                res['ran'] += 1
                inp = 'run %d regs %s stack %s' % (r, ' '.join('%08X' % x for x in regs),
                                                   ' '.join('%08X' % x for x in stack[1:9]))
                if as_ != 'ok':
                    res['x86'].append('arm %s | %s' % (as_, inp))
                    if hs == 'ok':
                        res['vita'].append('arm %s but host ok | %s' % (as_, inp))
                    continue
                c = parse_ctx(acb)
                # -- ARM vs host (exact).
                vp = []
                if hs != 'ok':
                    vp.append('host %s but arm ok' % hs)
                else:
                    for i in range(8):
                        if c['r'][i] != hst['r'][i]:
                            vp.append('%s arm %08X host %08X' % (difftest.NAMES[i], c['r'][i], hst['r'][i]))
                    if c['ftop'] != hst['ftop'] or (c['fsw'] & 0x4700) != hst['fsw']:
                        vp.append('ftop/fsw arm %X/%04X host %X/%04X' % (c['ftop'], c['fsw'] & 0x4700,
                                                                         hst['ftop'], hst['fsw']))
                    live = (-c['ftop']) & 7
                    for i in range(live):
                        p = (c['ftop'] + i) & 7
                        if not same_double(c['st'][p], hst['st'][p], True):
                            vp.append('st%d arm %s host %s' % (i, fmt_double(c['st'][p]), fmt_double(hst['st'][p])))
                    # Host dirty bytes vs ARM final bytes.
                    hiv = merge([(a, len(d)) for a, d in hmem.items()])
                    allv = merge([(a, b - a) for a, b in ivs + hiv])
                    for a, b in allv:
                        am = self.arm.gread(a, b - a)
                        hm = bytearray(self.pristine(a, b - a, r, stack))
                        for ha, hd in hmem.items():
                            lo, hi = max(a, ha), min(b, ha + len(hd))
                            if lo < hi:
                                hm[lo - a:hi - a] = hd[lo - ha:hi - ha]
                        if am != bytes(hm):
                            vp += ['mem ' + x for x in describe_bytes(a, am, bytes(hm))[:4]]
                if vp:
                    res['vita'].append('; '.join(vp[:8]) + ' | ' + inp)
                # -- ARM vs x86.
                xp, fp_only = [], True
                for i in range(8):
                    if c['r'][i] != xst['r'][i]:
                        xp.append('%s arm %08X x86 %08X' % (difftest.NAMES[i], c['r'][i], xst['r'][i]))
                        fp_only = False
                if c['ftop'] != xst['ftop']:
                    xp.append('ftop arm %d x86 %d' % (c['ftop'], xst['ftop']))
                    fp_only = False
                else:
                    live = (-c['ftop']) & 7
                    for i in range(live):
                        v = c['st'][(c['ftop'] + i) & 7]
                        if not same_double(v, xst['st'][i], False):
                            xp.append('st%d arm %r x86 %r' % (i, v, xst['st'][i]))
                            if not (math.isfinite(v) and abs(v - xst['st'][i]) <= 1e-6 * max(1.0, abs(v))):
                                fp_only = False
                for a, b in ivs:
                    if x_mem[a] != a_mem[a]:
                        if not float_close(a_mem[a] + bytes(-len(a_mem[a]) % 4), x_mem[a] + bytes(-len(x_mem[a]) % 4)):
                            fp_only = False
                        xp += ['mem ' + x for x in describe_bytes(a, a_mem[a], x_mem[a])[:4]]
                if xp:
                    if fp_only:
                        res['x86_fp'] += 1
                    else:
                        res['x86'].append('; '.join(xp[:8]) + ' | ' + inp)
            finally:
                self.restore(self.x86.uc.mem_write, [(a, b) for a, b in xw if a < GSIZE], r, stack)
                self.restore(self.arm.gwrite, [(a, b) for a, b in aw if a < GSIZE], r, stack)
        return res


_worker = None


def _init_worker(cfg):
    global _worker
    try:
        _worker = FuncWorker(cfg)
    except Exception as e:  # noqa: BLE001 - a failing initializer would make the pool respawn forever
        _worker = 'worker init failed: %r' % e


def _test(entry):
    if isinstance(_worker, str):
        raise RuntimeError(_worker)
    try:
        return _worker.test(entry)
    except Exception as e:  # noqa: BLE001 - keep the pool alive, report the crash
        return {'entry': entry, 'ran': 0, 'skipped': {'error %s' % e: 1}, 'vita': [], 'x86': [], 'x86_fp': 0}


def parse_trace(blob):
    out = []
    for off in range(0, len(blob) - TRACE_REC_SIZE + 1, TRACE_REC_SIZE):
        v = struct.unpack_from(TRACE_REC, blob, off)
        out.append({'addr': v[0], 'r': list(v[1:9]), 'ftop': v[9], 'fsw': v[10] & 0x4700,
                    'st': [v[12 + ((v[9] + i) & 7)] for i in range(8)]})
    return out


def trace_diff(a, b, exact):
    """Differences between two trace records (live x87 registers only)."""
    d = []
    for i in range(8):
        if a['r'][i] != b['r'][i]:
            d.append('%s %08X/%08X' % (difftest.NAMES[i], a['r'][i], b['r'][i]))
    if a['ftop'] != b['ftop']:
        d.append('ftop %d/%d' % (a['ftop'], b['ftop']))
    elif exact and a['fsw'] != b['fsw']:
        d.append('fsw %04X/%04X' % (a['fsw'], b['fsw']))
    live = (-a['ftop']) & 7
    for i in range(live):
        if not same_double(a['st'][i], b['st'][i], exact):
            d.append('st%d %s/%s' % (i, fmt_double(a['st'][i]), fmt_double(b['st'][i])))
    return d


def build_trace_program(g, work):
    """Every generated function with TRACE(addr) before each emitted instruction.
    Returns (gen dir, set of traced guest addresses)."""
    tdir = os.path.join(work, 'trace_gen')
    os.makedirs(tdir, exist_ok=True)
    entries = sorted(g.funcs)
    traced = set()
    files = []
    for n in range(0, len(entries), Generator.FUNCS_PER_FILE):
        out = ['/* Generated by armdifftest.py (traced copy of gen.py output). */\n#include "recomp_gen.h"\n'
               '#include "armdt_trace.h"\n\n']
        for e in entries[n:n + Generator.FUNCS_PER_FILE]:
            body = []
            fg = FuncGen(g, g.funcs[e])
            for line in fg.emit():
                body.append(line)
                m = line.strip()
                if m.startswith('/* ') and m[3:11].isalnum() and m[11:13] == ': ':
                    body.append('    TRACE(0x%sU);' % m[3:11])
                    traced.add(int(m[3:11], 16))
            out.append(g.wrap(e, body, fg))
        path = os.path.join(tdir, 'tfuncs_%03d.c' % (n // Generator.FUNCS_PER_FILE))
        write_if_changed(path, ''.join(out))
        files.append(path)
    g.write_table(tdir, entries)
    write_if_changed(os.path.join(tdir, 'armdt_trace.h'), TRACE_H)
    write_if_changed(os.path.join(tdir, 'armdt_trace.c'), TRACE_C)
    files += [os.path.join(tdir, x) for x in ('dispatch_table.c', 'import_stubs.c', 'armdt_trace.c')]
    return tdir, files, traced


def call_trace(args, g, elf, image, entry, regs, stack, buf):
    """Guest state at every guest function entry, from the normal (uninstrumented)
    ARM build and from x86. Instrumentation changes the generated code, so this
    catches divergences that the per-instruction trace would hide."""
    stack_bytes = struct.pack('<%dI' % len(stack), *stack)
    arm = ArmEngine(elf, args.fpscr)
    arm.gwrite(IMAGE_BASE, image)
    arm.gwrite(TIB, struct.pack('<I', 0xFFFFFFFF))
    arm.gwrite(BUF, buf)
    arm.gwrite(ESP0, stack_bytes)
    by_host = {}
    for name, a in arm.syms.items():
        if name.startswith('f_') and len(name) == 10:
            by_host[a & ~1] = int(name[2:], 16)
    t_arm = []

    def arm_block(uc, addr, size, user):
        f = by_host.get(addr)
        if f is not None and len(t_arm) < 100000:
            ctx = parse_ctx(bytes(uc.mem_read(uc.reg_read(A.UC_ARM_REG_R0), CTX_SIZE)))
            t_arm.append((f, ctx['r'], ctx['ftop'], [ctx['st'][(ctx['ftop'] + i) & 7] for i in range(8)]))
    arm.uc.hook_add(UC_HOOK_BLOCK, arm_block)
    status, _ = arm.call('w_%08X' % entry, make_ctx(regs, 0x27F, fs_base=TIB))
    x86 = X86Engine(image, record=False)
    x86.uc.mem_write(BUF, buf)
    x86.uc.mem_write(ESP0, stack_bytes)
    t_x86 = []
    funcs = set(g.funcs)

    def x86_block(uc, addr, size, user):
        if addr in funcs and len(t_x86) < 100000:
            fsw = uc.reg_read(U.UC_X86_REG_FPSW)
            top = (fsw >> 11) & 7
            phys = [f80_to_double(uc.reg_read(U.UC_X86_REG_FP0 + i)) for i in range(8)]
            t_x86.append((addr, [uc.reg_read(x) for x in difftest.REGS], top, [phys[(top + i) & 7] for i in range(8)]))
    x86.uc.hook_add(UC_HOOK_BLOCK, x86_block)
    xs, _ = x86.run(entry, regs, 0x27F)
    print('call trace (uninstrumented build): arm %d calls (%s), x86 %d calls (%s)' % (len(t_arm), status, len(t_x86), xs))
    for i in range(min(len(t_arm), len(t_x86))):
        fa, ra, ta, sa = t_arm[i]
        fx, rx, tx, sx = t_x86[i]
        d = []
        if fa != fx:
            d.append('function %08X/%08X' % (fa, fx))
        d += ['%s %08X/%08X' % (difftest.NAMES[k], ra[k], rx[k]) for k in range(8) if ra[k] != rx[k]]
        if ta != tx:
            d.append('ftop %d/%d' % (ta, tx))
        else:
            d += ['st%d %r/%r' % (k, sa[k], sx[k]) for k in range((-ta) & 7) if not same_double(sa[k], sx[k], False)]
        if d:
            print('   first divergent call #%d (arm/x86): %s' % (i, '; '.join(d)))
            if i:
                print('   previous call #%d: %08X' % (i - 1, t_arm[i - 1][0]))
            return
    print('   no divergence in %d common calls' % min(len(t_arm), len(t_x86)))


def trace_mode(args, g, work, image_path, bufs_path):
    """Run one function/run with a whole-program per-instruction trace in all three
    engines and report the first guest instruction after which the states diverge."""
    entry = int(args.trace, 16)
    r = args.trace_run
    regs, stack = func_inputs(entry, r)
    call_trace(args, g, os.path.join(work, 'func_arm.elf'), open(image_path, 'rb').read(), entry, regs, stack,
               gen_buffers(r + 1)[r])
    print('building traced program ...', flush=True)
    tdir, files, traced = build_trace_program(g, work)
    rt_inc = os.path.join(ROOT, 'runtime', 'include')
    stub = os.path.join(work, 'stub.c')
    cpu_c = os.path.join(ROOT, 'runtime/src/cpu.c')
    hle_c = os.path.join(ROOT, 'runtime/src/crt_hle.c')
    tobjs = compile_many(args.jobs, VITA_CC, VITA_FLAGS + ['-O2'], files + [stub],
                         os.path.join(work, 'arm_trace_obj'), [tdir, rt_inc])
    tobjs += compile_many(1, VITA_CC, VITA_FLAGS + ['-O3'], [cpu_c, hle_c], os.path.join(work, 'arm_trace_obj'), [tdir, rt_inc])
    elf = os.path.join(work, 'trace_arm.elf')
    vita_link(tobjs, elf, 'rt_dispatch')
    main_c = os.path.join(work, 'trace_main.c')
    write_if_changed(main_c, TRACE_HOST_MAIN)
    hflags = HOST_FLAGS + ['-O2', '-DBUF_ADDR=0x%Xu' % BUF, '-DTIB_ADDR=0x%Xu' % TIB]
    hobjs = compile_many(args.jobs, 'cc', hflags, files + [stub, main_c, cpu_c, hle_c], os.path.join(work, 'host_trace_obj'),
                         [tdir, rt_inc])
    host = os.path.join(work, 'trace_host')
    sh(['cc', '-o', host] + hobjs + ['-lm'])

    r = args.trace_run
    regs, stack = func_inputs(entry, r)
    buf = gen_buffers(r + 1)[r]
    image = open(image_path, 'rb').read()
    stack_bytes = struct.pack('<%dI' % len(stack), *stack)
    # Host.
    p = subprocess.run([host, image_path, bufs_path, str(r), str(BUF_SIZE), '%X' % entry], capture_output=True,
                       input=('%s %X %X %d %s\n' % (' '.join('%X' % x for x in regs), 0x27F, TIB, len(stack),
                                                    ' '.join('%X' % x for x in stack))).encode())
    t_host = parse_trace(p.stdout)
    # ARM.
    arm = ArmEngine(elf, args.fpscr)
    arm.gwrite(IMAGE_BASE, image)
    arm.gwrite(TIB, struct.pack('<I', 0xFFFFFFFF))
    arm.gwrite(BUF, buf)
    arm.gwrite(ESP0, stack_bytes)
    status, _ = arm.call('w_%08X' % entry, make_ctx(regs, 0x27F, fs_base=TIB))
    n = struct.unpack('<I', arm.uc.mem_read(arm.syms['g_armdt_trace_n'], 4))[0]
    t_arm = parse_trace(bytes(arm.uc.mem_read(arm.syms['g_armdt_trace'], n * TRACE_REC_SIZE)))
    # x86: state before every guest instruction the C code traces (fused jcc/setcc excluded).
    x86 = X86Engine(image, record=False)
    x86.uc.mem_write(BUF, buf)
    x86.uc.mem_write(ESP0, stack_bytes)
    t_x86 = []
    last_rep = [False]

    def hook(uc, addr, size, user):
        if addr in traced and len(t_x86) < 200000:
            if t_x86 and t_x86[-1]['addr'] == addr and last_rep[0]:
                return      # rep-prefixed string instruction: one step per iteration on x86
            last_rep[0] = uc.mem_read(addr, 1)[0] in (0xF2, 0xF3)
            fsw = uc.reg_read(U.UC_X86_REG_FPSW)
            top = (fsw >> 11) & 7
            phys = [f80_to_double(uc.reg_read(U.UC_X86_REG_FP0 + i)) for i in range(8)]
            t_x86.append({'addr': addr, 'r': [uc.reg_read(x) for x in difftest.REGS], 'ftop': top,
                          'fsw': fsw & 0x4700, 'st': [phys[(top + i) & 7] for i in range(8)]})
    x86.uc.hook_add(UC_HOOK_CODE, hook)
    xs, _ = x86.run(entry, regs, 0x27F)
    print('trace %08X run %d: host %d steps, arm %d steps (%s), x86 %d steps (%s)' % (
        entry, r, len(t_host), len(t_arm), status, len(t_x86), xs))

    insns = {}
    for f in g.funcs.values():
        insns.update(f.insns)

    def report(name, ta, tb, exact):
        for i in range(min(len(ta), len(tb))):
            if ta[i]['addr'] != tb[i]['addr'] or trace_diff(ta[i], tb[i], exact):
                prev = insns.get(ta[i - 1]['addr']) if i else None
                print('%s: first divergence at step %d (before %08X / %08X)' % (name, i, ta[i]['addr'], tb[i]['addr']))
                if prev is not None:
                    print('   caused by %08X: %s' % (prev.addr, prev.text))
                    print('   inputs  : %s' % '; '.join(
                        ['%s %08X' % (difftest.NAMES[k], ta[i - 1]['r'][k]) for k in range(8)] +
                        ['st%d %s' % (k, fmt_double(ta[i - 1]['st'][k])) for k in range((-ta[i - 1]['ftop']) & 7)]))
                print('   diff    : %s' % '; '.join(trace_diff(ta[i], tb[i], exact)))
                return
        n = min(len(ta), len(tb))
        print('%s: no divergence in %d common steps%s' % (name, n, '' if len(ta) == len(tb) else
                                                          ' (then only one side continues; last common step %08X: %s)'
                                                          % (ta[n - 1]['addr'], insns[ta[n - 1]['addr']].text)))
    report('arm vs host', t_arm, t_host, True)
    if t_x86:
        # x87 C0-C3 after arithmetic are undefined on x86; compare registers/stack only.
        report('arm vs x86 ', t_arm, t_x86, False)


def func_mode(args):
    work = os.path.abspath(args.work)
    os.makedirs(work, exist_ok=True)
    g = Generator(args.exe)
    gen_dir = os.path.join(work, 'gen')
    stamp = os.path.join(gen_dir, 'files.txt')
    if not os.path.exists(stamp) or os.path.getmtime(stamp) < max(
            os.path.getmtime(os.path.join(ROOT, 'tools/recomp', p)) for p in ('gen.py', 'analyze.py')):
        g.run(gen_dir)
    gen_files = [os.path.join(gen_dir, x) for x in open(stamp).read().split()]
    image = guest_image(g.an)
    image_path = os.path.join(work, 'func_image.bin')
    with open(image_path, 'wb') as f:
        f.write(image)
    rt_inc = os.path.join(ROOT, 'runtime', 'include')
    write_if_changed(os.path.join(work, 'stub.c'), STUB_C)
    write_if_changed(os.path.join(work, 'native_main.c'), NATIVE_FUNC_MAIN)
    cpu_c = os.path.join(ROOT, 'runtime/src/cpu.c')
    hle_c = os.path.join(ROOT, 'runtime/src/crt_hle.c')
    # ARM build of every generated function.
    print('building ARM objects ...', flush=True)
    objs = compile_many(args.jobs, VITA_CC, VITA_FLAGS + ['-O2'], gen_files + [os.path.join(work, 'stub.c')],
                        os.path.join(work, 'arm_obj'), [gen_dir, rt_inc])
    objs += compile_many(1, VITA_CC, VITA_FLAGS + ['-O3'], [cpu_c, hle_c], os.path.join(work, 'arm_obj'), [gen_dir, rt_inc])
    elf = os.path.join(work, 'func_arm.elf')
    vita_link(objs, elf, 'rt_dispatch')
    # Host build.
    print('building host objects ...', flush=True)
    hflags = HOST_FLAGS + ['-O2', '-DBUF_ADDR=0x%Xu' % BUF, '-DTIB_ADDR=0x%Xu' % TIB]
    hobjs = compile_many(args.jobs, 'cc', hflags, gen_files + [os.path.join(work, 'stub.c'),
                                                             os.path.join(work, 'native_main.c'), cpu_c, hle_c],
                         os.path.join(work, 'host_obj'), [gen_dir, rt_inc])
    host = os.path.join(work, 'func_host')
    sh(['cc', '-o', host] + hobjs + ['-lm'])
    runs = args.runs
    bufs_path = os.path.join(work, 'func_bufs_%d.bin' % runs)
    if not os.path.exists(bufs_path):
        with open(bufs_path, 'wb') as f:
            for b in gen_buffers(runs):
                f.write(b)
    if args.trace:
        trace_mode(args, g, work, image_path, bufs_path)
        return
    # Function selection and order.
    if args.only:
        entries = [int(x, 16) for x in args.only.split(',')]
    else:
        scored = []
        for e, fn in g.funcs.items():
            pr, x87 = func_priority(g, fn)
            if args.x87_only and not x87:
                continue
            scored.append((-pr, e))
        entries = [e for _, e in sorted(scored)]
    if args.limit:
        entries = entries[:args.limit]
    print('functions: %d, runs each: %d' % (len(entries), runs), flush=True)
    cfg = {'image': image_path, 'elf': elf, 'host': host, 'bufs': bufs_path, 'runs': runs, 'fpscr': args.fpscr}
    results = []
    with multiprocessing.Pool(args.jobs, initializer=_init_worker, initargs=(cfg,)) as pool:
        for i, res in enumerate(pool.imap_unordered(_test, entries, chunksize=1)):
            results.append(res)
            if res['vita']:
                print('VITA-ONLY %08X (%d/%d runs): %s' % (res['entry'], len(res['vita']), res['ran'], res['vita'][0]),
                      flush=True)
            if (i + 1) % 200 == 0:
                print('  ... %d/%d functions' % (i + 1, len(entries)), flush=True)
    ran = sum(1 for r in results if r['ran'])
    runs_total = sum(r['ran'] for r in results)
    skipped = {}
    for r in results:
        for k, v in r['skipped'].items():
            skipped[k] = skipped.get(k, 0) + v
    vita = [r for r in results if r['vita']]
    x86 = [r for r in results if r['x86']]
    fp = sum(1 for r in results if r['x86_fp'] and not r['x86'])
    print('\nfunctions with at least one completed run: %d / %d (%d runs; skipped: %s)' % (
        ran, len(results), runs_total, ', '.join('%s %d' % kv for kv in sorted(skipped.items()))))
    print('== ARM (Vita GCC) vs host build: %d functions differ' % len(vita))
    for r in sorted(vita, key=lambda r: r['entry']):
        print('VITA-ONLY %08X (%d/%d runs)  e.g. %s' % (r['entry'], len(r['vita']), r['ran'], r['vita'][0]))
    print('== ARM vs x86 reference: %d functions differ beyond float rounding '
          '(%d more only by float32/64 rounding noise)' % (len(x86), fp))
    for r in sorted(x86, key=lambda r: r['entry']):
        tag = 'VITA-ONLY' if r['vita'] else 'ALSO-HOST'
        print('FAIL %s %08X (%d/%d runs)  e.g. %s' % (tag, r['entry'], len(r['x86']), r['ran'], r['x86'][0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('mode', choices=['insn', 'func'])
    ap.add_argument('exe')
    ap.add_argument('work')
    ap.add_argument('--samples', type=int, default=3, help='insn: samples per instruction form')
    ap.add_argument('--runs', type=int, default=None, help='random states per sample/function')
    ap.add_argument('--fpscr', type=lambda s: int(s, 0), default=0)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument('--only', help='func: comma-separated guest function addresses (hex)')
    ap.add_argument('--x87-only', action='store_true', help='func: only functions using the x87 FPU')
    ap.add_argument('--limit', type=int, default=0, help='func: test only the N highest-priority functions')
    ap.add_argument('--trace', help='func: trace one function (hex) and locate the first diverging instruction')
    ap.add_argument('--trace-run', type=int, default=0, help='func: which random run to trace')
    args = ap.parse_args()
    if args.runs is None:
        args.runs = 20 if args.mode == 'insn' else 8
    insn_mode(args) if args.mode == 'insn' else func_mode(args)


if __name__ == '__main__':
    main()
