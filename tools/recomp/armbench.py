"""Executed-instruction benchmark of the generated code for the Vita's CPU.

Builds a subset of the generated functions with the vitasdk compiler (ARMv7,
Thumb-2, VFP/NEON, as the real build) into a bare ELF, runs every function in
Unicorn from identical synthetic inputs (argument pointers into buffers of random
floats) and counts the ARM instructions executed, split into loads, stores, VFP,
branches and the rest. Given two ELFs, it also cross-checks the final guest
state (registers, x87 top, condition codes and pushed values, argument memory
and stack) so a code generator change can be validated on real functions.

Indirect calls between generated functions work (the dispatch table is linked);
functions that fault on the synthetic inputs (wild pointers, calls into host
code) or run longer than the instruction limit are skipped.

Usage:
  armbench.py build IGI.exe GEN_DIR OUT.elf [LIST]   # GEN_DIR: output of gen.py
  armbench.py run IGI.exe A.elf [B.elf]

LIST is a file of hex function addresses (default: every function with at least
15 instructions). Needs unicorn and capstone (pip) and VITASDK (default ~/vitasdk).
"""
import bisect
import functools
import glob
import hashlib
import math
import multiprocessing
import os
import random
import struct
import subprocess
import sys

from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_BLOCK
from unicorn import arm_const as A

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SDK = os.environ.get('VITASDK', os.path.expanduser('~/vitasdk'))
TOOL = os.path.join(SDK, 'bin', 'arm-vita-eabi-')
CFLAGS = ['-O2', '-mcpu=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard', '-mthumb', '-fno-short-enums',
          '-ffunction-sections', '-fdata-sections', '-D__vita__']

GMEM = 0x10000000           # guest memory in the emulated address space
GSIZE = 0x03000000
CTX = 0x00900000
STOP = 0x00A00000           # return address of the emulated call
HSTACK = 0x00800000
LIMIT = 3000000             # instructions per function

STUB = r'''
#include "recomp.h"
#include "runtime.h"
GuestFn thunk_lookup(uint32_t a) { (void)a; return NULL; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; for (;;) __asm__ volatile("udf #1"); }
void rt_probe(Ctx *c, uint32_t f) { (void)c; (void)f; }
void rt_missing_import(Ctx *c, const char *n) { (void)c; (void)n; __asm__ volatile("udf #2"); }
int *__errno(void) { static int e; return &e; }
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
'''


def guest_image(exe):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from pe import PE
    pe = PE(exe)
    end = max(s.va + s.vsize for s in pe.sections)
    image = bytearray(end - 0x400000)
    for s in pe.sections:
        raw = pe.data[s.raw_off:s.raw_off + s.raw_size]
        image[s.va - 0x400000:s.va - 0x400000 + len(raw)] = raw
    return bytes(image)


def default_list(exe):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gen import Generator
    g = Generator(exe)
    return ['%08X' % e for e, f in sorted(g.funcs.items()) if len(f.insns) >= 15]


def compile_one(args):
    src, obj, incs = args
    # The stub's memmove/memset must not be turned back into calls to themselves.
    extra = ['-fno-builtin', '-fno-tree-loop-distribute-patterns'] if src.endswith('stub.c') else []
    subprocess.check_call([TOOL + 'gcc'] + CFLAGS + extra + incs + ['-c', src, '-o', obj])


def build(exe, gen_dir, out, list_file=None):
    funcs = open(list_file).read().split() if list_file else default_list(exe)
    work = out + '.d'
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, 'stub.c'), 'w') as f:
        f.write(STUB)
    incs = ['-I' + os.path.join(ROOT, 'runtime/include'), '-I' + gen_dir]
    srcs = sorted(glob.glob(os.path.join(gen_dir, 'funcs_*.c'))) + [
        os.path.join(gen_dir, 'import_stubs.c'), os.path.join(gen_dir, 'dispatch_table.c'),
        os.path.join(ROOT, 'runtime/src/cpu.c'), os.path.join(work, 'stub.c')]
    objs = [os.path.join(work, os.path.basename(s)[:-2] + '.o') for s in srcs]
    with multiprocessing.Pool() as pool:
        pool.map(compile_one, [(s, o, incs) for s, o in zip(srcs, objs)])
    libgcc = subprocess.check_output([TOOL + 'gcc'] + CFLAGS + ['-print-libgcc-file-name'], text=True).strip()
    lib = os.path.join(SDK, 'arm-vita-eabi', 'lib')
    roots = ['-Wl,--undefined=f_' + f for f in funcs] + ['-Wl,--undefined=rt_dispatch', '-Wl,--undefined=rt_call_indirect']
    subprocess.check_call([TOOL + 'gcc'] + CFLAGS + [
        '-nostdlib', '-nostartfiles', '-static', '-Wl,--gc-sections', '-Wl,--no-enum-size-warning',
        '-Wl,-e,f_' + funcs[0], '-Wl,-Ttext=0x100000'] + roots + ['-o', out] + objs +
        [os.path.join(lib, 'libm.a'), os.path.join(lib, 'libc.a'), libgcc])
    with open(out + '.funcs', 'w') as f:
        f.write(' '.join(funcs) + '\n')
    print('built %s with %d root functions' % (out, len(funcs)))


@functools.lru_cache(maxsize=4)
def load_elf(path):
    data = open(path, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, p_memsz, _, _ = struct.unpack_from('<8I', data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((p_vaddr, data[p_off:p_off + p_filesz], p_memsz))
    syms = {}
    for line in subprocess.check_output([TOOL + 'nm', path], text=True).splitlines():
        f = line.split()
        if len(f) == 3:
            syms[f[2]] = int(f[0], 16)
    return segs, syms


@functools.lru_cache(maxsize=4)
def classify(path):
    """Sorted instruction addresses and per-class prefix counts, from the disassembly."""
    cls = {}
    for line in subprocess.check_output([TOOL + 'objdump', '-d', path], text=True).splitlines():
        f = line.split('\t')
        if len(f) < 3 or not f[0].strip().endswith(':'):
            continue
        try:
            a = int(f[0].strip()[:-1], 16)
        except ValueError:
            continue
        m = f[2].split()[0] if f[2].split() else ''
        if m.startswith(('vldr', 'vldm', 'vld1', 'ldr', 'ldm', 'pop', 'vpop')):
            c = 'ld'
        elif m.startswith(('vstr', 'vstm', 'vst1', 'str', 'stm', 'push', 'vpush')):
            c = 'st'
        elif m.startswith('v'):
            c = 'vfp'
        elif m.startswith(('b', 'cb', 'tb')):
            c = 'br'
        else:
            c = 'alu'
        cls[a] = c
    addrs = sorted(cls)
    prefix = {n: [0] for n in ('ld', 'st', 'vfp', 'br', 'alu')}
    for a in addrs:
        for n, p in prefix.items():
            p.append(p[-1] + (cls[a] == n))
    return addrs, prefix


def run_one(path, image, fname, seed=1):
    """(status, instruction counts, final state) of one function."""
    segs, syms = load_elf(path)
    addrs, prefix = classify(path)
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for va, blob, memsz in segs:
        lo = va & ~0xFFF
        uc.mem_map(lo, ((va + memsz + 0xFFF) & ~0xFFF) - lo)
        uc.mem_write(va, blob)
    uc.mem_map(HSTACK - 0x100000, 0x100000)
    uc.mem_map(CTX, 0x1000)
    uc.mem_map(STOP, 0x1000)
    uc.mem_map(GMEM, GSIZE)
    uc.mem_write(GMEM + 0x400000, image)
    uc.mem_write(syms['g_mem'], struct.pack('<I', GMEM))
    rnd = random.Random(seed)
    bufs = [0x02000000 + i * 0x10000 for i in range(8)]
    for b in bufs:
        vals = [rnd.uniform(-10.0, 10.0) for _ in range(0x4000)]
        uc.mem_write(GMEM + b, struct.pack('<%df' % len(vals), *vals))
    esp = 0x02EFF000
    stack = [0xFDFDFD00] + bufs
    uc.mem_write(GMEM + esp, struct.pack('<%dI' % len(stack), *stack))
    regs = [bufs[0], bufs[1], bufs[2], bufs[3], esp, 0x02EFFF00, bufs[4], bufs[5]]
    st = [rnd.uniform(-10.0, 10.0) for _ in range(8)]
    ctx = struct.pack('<8I3I4x8dIII', *regs, 0, 0, 0x00020000, *st, 0, 0x27F, 0)
    uc.mem_write(CTX, ctx)
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))   # VFP on
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(A.UC_ARM_REG_SP, HSTACK - 0x100)
    # uint64_t f(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax)
    uc.reg_write(A.UC_ARM_REG_R0, CTX)
    uc.reg_write(A.UC_ARM_REG_R1, regs[4])
    uc.reg_write(A.UC_ARM_REG_R2, regs[1])
    uc.reg_write(A.UC_ARM_REG_R3, regs[0])
    uc.reg_write(A.UC_ARM_REG_LR, STOP | 1)
    counts = {'total': 0}

    def hook(uc_, addr, size, user):
        i0 = bisect.bisect_left(addrs, addr)
        i1 = bisect.bisect_left(addrs, addr + size)
        counts['total'] += i1 - i0
        for n, p in prefix.items():
            counts[n] = counts.get(n, 0) + p[i1] - p[i0]

    uc.hook_add(UC_HOOK_BLOCK, hook)
    try:
        uc.emu_start(syms['f_' + fname] | 1, STOP, count=LIMIT)
    except UcError as e:
        return 'fault %s' % e, counts, None
    if uc.reg_read(A.UC_ARM_REG_PC) & ~1 != STOP:
        return 'limit', counts, None
    out = bytearray(uc.mem_read(CTX, len(ctx)))
    struct.pack_into('<I', out, 0, uc.reg_read(A.UC_ARM_REG_R0))     # eax
    struct.pack_into('<I', out, 16, uc.reg_read(A.UC_ARM_REG_R1))    # esp
    r = struct.unpack_from('<8I', out, 0)
    ftop, _, fsw = struct.unpack_from('<III', out, 112)
    stv = struct.unpack_from('<8d', out, 48)
    # Entry ftop is 0: only values the function left pushed are live afterwards.
    live = (-ftop) & 7
    live = live if live < 4 else 0
    h = hashlib.sha1()
    for b in bufs + [esp - 0x1000]:
        h.update(bytes(uc.mem_read(GMEM + b, 0x10000 if b != esp - 0x1000 else 0x1100)))
    return 'ok', counts, (r, ftop, fsw & 0x4700, [stv[(ftop + i) & 7] for i in range(live)], h.hexdigest())


def worker(args):
    paths, exe_image, funcs = args
    res = {}
    for f in funcs:
        res[f] = []
        for p in paths:
            try:
                res[f].append(run_one(p, exe_image, f))
            except Exception as e:  # noqa: BLE001 - keep the pool alive
                res[f].append(('error %s' % e, {'total': 0}, None))
    return res


def same(a, b):
    ra, ta, fa, sa, ha = a
    rb, tb, fb, sb, hb = b
    return (ra, ta, fa, ha) == (rb, tb, fb, hb) and len(sa) == len(sb) and all(
        x == y or (math.isnan(x) and math.isnan(y)) for x, y in zip(sa, sb))


def run(exe, paths):
    funcs = open(paths[0] + '.funcs').read().split()
    image = guest_image(exe)
    n = max(1, os.cpu_count() or 1)
    with multiprocessing.Pool(n) as pool:
        parts = pool.map(worker, [(paths, image, funcs[i::n]) for i in range(n)])
    res = {}
    for p in parts:
        res.update(p)
    totals = [{} for _ in paths]
    logs, ok, mism = [], 0, 0
    for f in funcs:
        rs = res[f]
        if any(r[0] != 'ok' for r in rs):
            continue
        if len(rs) == 2 and not same(rs[0][2], rs[1][2]):
            mism += 1
            print('MISMATCH %s\n  a %s\n  b %s' % (f, rs[0][2], rs[1][2]))
            continue
        ok += 1
        for t, r in zip(totals, rs):
            for k, v in r[1].items():
                t[k] = t.get(k, 0) + v
        if len(rs) == 2:
            logs.append(math.log(rs[0][1]['total'] / max(1, rs[1][1]['total'])))
    print('functions run: %d%s' % (ok, ', state mismatches: %d' % mism if len(paths) == 2 else ''))
    if logs:
        print('geometric mean of per-function instruction ratios a/b: %.3f' % math.exp(sum(logs) / len(logs)))
    for k in ('total', 'ld', 'st', 'vfp', 'br', 'alu'):
        print('  %-6s' % k + ''.join('%12d' % t.get(k, 0) for t in totals))
    return mism == 0


def main():
    if len(sys.argv) >= 5 and sys.argv[1] == 'build':
        build(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else None)
    elif len(sys.argv) >= 4 and sys.argv[1] == 'run':
        sys.exit(0 if run(sys.argv[2], sys.argv[3:5]) else 1)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
