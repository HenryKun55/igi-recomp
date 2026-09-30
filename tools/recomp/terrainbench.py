"""Terrain renderer benchmark on a synthetic but realistic frame.

armbench.py runs every function from random inputs, which never gets far into
the terrain renderer (f_00512C30, the hottest guest function in gameplay). This
builds a frame for it instead: a camera, 9x9-vertex terrain patches with
triangle strips and material passes, the batch pool and fake Direct3D objects
(their COM methods are host stubs), then runs it in Unicorn ARM and counts the
ARM instructions executed per function. Given two ELFs (e.g. before and after a
generator change) it also checks that the final guest memory and context are
identical.

  terrainbench.py build IGI.exe GEN_DIR OUT.elf
  terrainbench.py run IGI.exe A.elf [B.elf] [--patches N] [--seed S]
                  [--profile GEN_DIR [--func f_XXXXXXXX]]

--profile writes A.elf.prof: executed ARM instructions per guest instruction of
one function (default f_00512C30), from the debug line info. The ELF is built
from the runtime headers next to this script (armbench.py).
"""
import argparse
import bisect
import os
import math
import random
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armbench  # noqa: E402

STUB = r'''
#include "recomp.h"
#include "runtime.h"
/* COM methods of the fake Direct3D objects: stdcall, pop 4 * k bytes of arguments
 * (k from the thunk address), return 0 (S_OK). */
static void com_stub(Ctx *c)
{
    uint32_t k = (g_current_thunk - THUNK_BASE) / 4u;
    c->r[4] += 4u + 4u * k;
    c->r[0] = 0;
}
GuestFn thunk_lookup(uint32_t a) { (void)a; return com_stub; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; for (;;) __asm__ volatile("udf #1"); }
void rt_probe(Ctx *c, uint32_t f) { (void)c; (void)f; }
void rt_missing_import(Ctx *c, const char *n) { (void)c; (void)n; __asm__ volatile("udf #2"); }
int *__errno(void) { static int e; return &e; }
/* On the Vita memcpy/memmove/memset are SceLibc calls; these word-at-a-time
 * stand-ins approximate their cost (armbench's byte loops would overstate it). */
#pragma GCC optimize("no-tree-loop-distribute-patterns")
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if (dp <= sp || dp >= sp + n) {
        for (; n >= 4; n -= 4, dp += 4, sp += 4)
            *(u32u *)dp = *(const u32u *)sp;
        while (n--) *dp++ = *sp++;
    } else {
        while (n & 3u) { n--; dp[n] = sp[n]; }
        while (n) { n -= 4; *(u32u *)(dp + n) = *(const u32u *)(sp + n); }
    }
    return d;
}
void *memcpy(void *d, const void *s, size_t n) { return memmove(d, s, n); }
void *memset(void *d, int v, size_t n)
{
    unsigned char *p = d;
    uint32_t w = (uint8_t)v * 0x01010101u;
    for (; n >= 4; n -= 4, p += 4)
        *(u32u *)p = w;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
'''

THUNK_BASE = 0xFE000000
# vtable slot -> bytes of arguments (this included)
SLOTS = {0x0C: 0x10, 0x10: 4, 0x2C: 0xC, 0x34: 8, 0x50: 0xC, 0x80: 0x20, 0x8C: 0xC, 0x94: 0x10}

SCENE = 0x02100000


class Mem:
    def __init__(self):
        self.w = {}

    def u32(self, a, v):
        self.w[a] = struct.pack('<I', v & 0xFFFFFFFF)

    def f32(self, a, v):
        self.w[a] = struct.pack('<f', v)

    def f64(self, a, v):
        self.w[a] = struct.pack('<d', v)

    def raw(self, a, b):
        self.w[a] = bytes(b)


class Alloc:
    def __init__(self, base):
        self.p = base

    def __call__(self, n, align=16):
        self.p = (self.p + align - 1) & ~(align - 1)
        a = self.p
        self.p += n
        return a


def scene(npatch=4, seed=1):
    rnd = random.Random(seed)
    m = Mem()
    al = Alloc(SCENE)
    # Fake COM objects sharing one vtable.
    vt = al(0x100)
    for s in range(0, 0x100, 4):
        m.u32(vt + s, THUNK_BASE + SLOTS.get(s, 0xC))
    dev = al(16)
    m.u32(dev, vt)
    vb = al(16)
    m.u32(vb, vt)
    m.u32(0x5CA114, dev)
    m.u32(0xA7F890, vb)
    vbuf = al(0x40000)
    m.u32(0xA7F89C, vbuf)
    m.u32(0xA7E078, 0)
    # Writable dummies written through at the start.
    for g in (0xBCADE8, 0xBCAD7C, 0xBCADAC, 0xBCADC4):
        m.u32(g, al(64))
    # Camera: position (doubles), rotation rows, projection, clipping.
    cam = (0.0, 30.0, 0.0)
    for i, v in enumerate(cam):
        m.f64(0xBCAB08 + 8 * i, v)
    ang = 0.3                          # yaw
    ca, sa = math.cos(ang), math.sin(ang)
    rows = [(ca, 0.0, -sa), (0.0, -1.0, 0.0), (sa, 0.0, ca)]
    for r, row in enumerate(rows):
        for k, v in enumerate(row):
            m.f32(0xBCAAE0 + 12 * r + 4 * k, v)
    m.f32(0xBCAAE0 + 36, 0.0)
    m.f32(0xC311A4, 320.0)
    m.f32(0xC311A0, 320.0)
    m.f32(0xC311B0, 320.0)
    m.f32(0xC311B4, 240.0)
    for a, v in ((0xBCABD0, 0.0), (0xBCABD4, 0.0), (0xBCABD8, 640.0), (0xBCABDC, 480.0)):
        m.f32(a, v)
    m.f32(0xBCADC0, 1.0)           # near plane
    m.f32(0xBCADCC, 1.0e9)         # patch distance limit
    m.f32(0xBCADA0, 1.0)
    m.f32(0xBCADC8, 2.0)
    m.f32(0xBCAD8C, 1000.0)
    m.f32(0xBCADB4, 1.0)
    m.f32(0xBCAD70, 0.001)
    m.f32(0xBCABEC, 1.0)
    m.f32(0xA7DA18, 0.004)         # fog density
    m.f32(0xA7DA30, 0.5)
    m.f32(0xA7DA34, 0.6)
    m.f32(0xA7DA38, 0.7)
    m.u32(0xC31088, 40)
    m.f32(0xC28FC0, 0.02)
    m.u32(0xC28FBC, 0)
    # Batch pool used by 514680 and the flush loop.
    kmax = 64
    stride = 0x2F00
    pool = al(stride * kmax)
    idx = al(4 * kmax)
    for i in range(kmax):
        m.u32(idx + 4 * i, i)
    m.u32(0xA7F878, pool)
    m.u32(0xA7F87C, idx)
    m.u32(0xA7F880, 0)
    m.u32(0xA7F884, kmax)
    m.u32(0xA7F888, stride)
    m.u32(0xA7F88C, 0x2EF4)
    for b in range(0x1FD):          # exec-style lists: head, tail (0), tailpred
        s = 0xA7E080 + 12 * b
        m.u32(s, s + 4)
        m.u32(s + 4, 0)
        m.u32(s + 8, s)
    mats = []
    for i in range(3):
        mt = al(0x40)
        m.u32(mt + 0x20, 0x1000 + i)
        mats.append(mt)
    # Patches: 9x9 vertex grids ahead of the camera.
    n = 9
    nv = n * n
    plist = []
    for p in range(npatch):
        px = (p % 2) * 64.0 - 32.0
        pz = 16.0 + (p // 2) * 64.0
        rec = al(0x18 + 16 + nv * 16 + 16 + 3 * (4 + nv * 12) + 4 + 4 + nv * 12 + 16)
        plist.append(rec)
        m.f64(rec, px + 32.0)
        m.f64(rec + 8, 0.0)
        m.f64(rec + 16, pz + 32.0)
        e = rec + 0x18
        m.u32(e, (nv << 16) | nv)
        m.u32(e + 4, 18)
        e += 8
        for j in range(n):
            for i in range(n):
                m.f32(e, px + i * 8.0)
                m.f32(e + 4, rnd.uniform(-4.0, 4.0))
                m.f32(e + 8, pz + j * 8.0)
                m.f32(e + 12, rnd.uniform(-1.0, 1.0))
                e += 16
        # Triangle strips, one per grid row.
        strips = []
        for r in range(n - 1):
            seq = []
            for i in range(n):
                seq += [r * n + i, (r + 1) * n + i]
            words = [(seq[0] << 16) | (seq[1] << 8) | seq[2]] + seq[3:]
            words[-1] |= 0x10000000
            strips += words
        il = al(4 * len(strips))
        for k, w in enumerate(strips):
            m.u32(il + 4 * k, w)
        m.u32(e, 0)
        m.u32(e + 4, il)
        m.u32(e + 8, 0)
        m.u32(e + 12, len(strips))
        e += 16
        for pas in range(3):
            m.u32(e, mats[pas])
            e += 4
            for v in range(nv):
                m.u32(e, 32 * v)
                m.f32(e + 4, rnd.uniform(0.0, 1.0))
                m.f32(e + 8, rnd.uniform(0.0, 1.0))
                e += 12
        m.u32(e, mats[0])           # detail pass
        e += 4
        for v in range(nv):
            m.u32(e, 32 * v)
            m.f32(e + 4, rnd.uniform(0.0, 4.0))
            m.f32(e + 8, rnd.uniform(0.0, 4.0))
            e += 12
    pl = al(4 * len(plist))
    for k, a in enumerate(plist):
        m.u32(pl + 4 * k, a)
    m.u32(0xB16C9C, len(plist))
    # The list pointer is an immediate (0xB12C60): copy it there.
    for k, a in enumerate(plist):
        m.u32(0xB12C60 + 4 * k, a)
    return m


def build(exe, gen_dir, out):
    import multiprocessing
    multiprocessing.set_start_method('fork')   # workers inherit the settings below
    armbench.STUB = STUB
    armbench.CFLAGS = armbench.CFLAGS + ['-g', '-fwrapv', '-fno-strict-aliasing', '-fsigned-char']
    lst = out + '.list'
    with open(lst, 'w') as f:
        f.write('00512C30\n')
    armbench.build(exe, gen_dir, out, lst)


def funcs_of(path):
    syms = []
    out = subprocess.check_output([armbench.TOOL + 'nm', '-S', '--size-sort', path], text=True)
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in 'tT':
            syms.append((int(f[0], 16), int(f[1], 16), f[3]))
    syms.sort()
    return syms


def run(exe, path, npatch, seed, prof_gen=None, prof_func='f_00512C30'):
    ab = armbench
    from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_BLOCK
    from unicorn import arm_const as A
    segs, syms = ab.load_elf(path)
    addrs, prefix = ab.classify(path)
    fsyms = funcs_of(path)
    starts = [s[0] for s in fsyms]
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for va, blob, memsz in segs:
        lo = va & ~0xFFF
        uc.mem_map(lo, ((va + memsz + 0xFFF) & ~0xFFF) - lo)
        uc.mem_write(va, blob)
    uc.mem_map(ab.HSTACK - 0x100000, 0x100000)
    uc.mem_map(ab.CTX, 0x1000)
    uc.mem_map(ab.STOP, 0x1000)
    uc.mem_map(ab.GMEM, ab.GSIZE)
    uc.mem_write(ab.GMEM + 0x400000, ab.guest_image(exe))
    uc.mem_write(syms['g_mem'], struct.pack('<I', ab.GMEM))
    m = scene(npatch, seed)
    for a, b in m.w.items():
        uc.mem_write(ab.GMEM + a, b)
    esp = 0x02EFF000
    uc.mem_write(ab.GMEM + esp, struct.pack('<I', 0xFDFDFD00))
    regs = [0, 0, 0, 0, esp, 0x02EFFF00, 0, 0]
    ctx = struct.pack('<8I3I4x8dIII', *regs, 0, 0, 0x00020000, *([0.0] * 8), 0, 0x27F, 0)
    uc.mem_write(ab.CTX, ctx)
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(A.UC_ARM_REG_SP, ab.HSTACK - 0x100)
    uc.reg_write(A.UC_ARM_REG_R0, ab.CTX)
    uc.reg_write(A.UC_ARM_REG_R1, esp)
    uc.reg_write(A.UC_ARM_REG_R2, 0)
    uc.reg_write(A.UC_ARM_REG_R3, 0)
    uc.reg_write(A.UC_ARM_REG_LR, ab.STOP | 1)
    per = {}
    tot = [0]
    blocks = {}

    def hook(uc_, addr, size, user):
        i0 = bisect.bisect_left(addrs, addr)
        i1 = bisect.bisect_left(addrs, addr + size)
        n = i1 - i0
        tot[0] += n
        k = bisect.bisect_right(starts, addr) - 1
        name = fsyms[k][2] if k >= 0 and addr < fsyms[k][0] + fsyms[k][1] + 4 else '?'
        per[name] = per.get(name, 0) + n
        if name == prof_func:
            blocks[(addr, size)] = blocks.get((addr, size), 0) + 1

    uc.hook_add(UC_HOOK_BLOCK, hook)
    status = 'ok'
    try:
        uc.emu_start(syms['f_00512C30'] | 1, ab.STOP, count=200_000_000)
    except UcError as e:
        status = 'fault %s pc=%08X' % (e, uc.reg_read(A.UC_ARM_REG_PC))
    if status == 'ok' and uc.reg_read(A.UC_ARM_REG_PC) & ~1 != ab.STOP:
        status = 'limit'
    mem = bytes(uc.mem_read(ab.GMEM, ab.GSIZE))
    out = bytearray(uc.mem_read(ab.CTX, len(ctx)))
    struct.pack_into('<I', out, 0, uc.reg_read(A.UC_ARM_REG_R0))
    struct.pack_into('<I', out, 16, uc.reg_read(A.UC_ARM_REG_R1))
    if prof_gen:
        profile(path, prof_gen, addrs, blocks)
    return status, tot[0], per, mem, bytes(out)


def profile(path, gen, addrs, blocks):
    cnt = {}
    for (a, s), n in blocks.items():
        i0 = bisect.bisect_left(addrs, a)
        i1 = bisect.bisect_left(addrs, a + s)
        for x in addrs[i0:i1]:
            cnt[x] = cnt.get(x, 0) + n
    xs = sorted(cnt)
    out = subprocess.check_output([armbench.TOOL + 'addr2line', '-i', '-a', '-e', path] + ['%x' % x for x in xs],
                                  text=True).split('\n')
    owners = {}                         # C file -> line -> guest instruction address

    def owner(f, ln):
        if f not in owners:
            o = {}
            cur = None
            fp = os.path.join(gen, f)
            if os.path.exists(fp):
                for i, l in enumerate(open(fp), 1):
                    m = re.match(r'\s+/\* ([0-9A-F]{8}): ', l)
                    if m:
                        cur = int(m.group(1), 16)
                    if l.startswith('uint64_t '):
                        cur = None
                    o[i] = cur
            owners[f] = o
        return owners[f].get(ln)
    res = {}
    k = -1
    frames = []

    def flush():
        if k < 0:
            return
        x86 = None
        for fr in frames:
            m = re.match(r'(.*):(\d+)', fr)
            if m and os.path.basename(m.group(1)).startswith('funcs_'):
                x86 = owner(os.path.basename(m.group(1)), int(m.group(2)))
        res[x86] = res.get(x86, 0) + cnt[xs[k]]
    for l in out:
        if l.startswith('0x'):
            flush()
            k += 1
            frames = []
        elif l.strip():
            frames.append(l.split(' ')[0])
    flush()
    with open(path + '.prof', 'w') as f:
        for a in sorted(res, key=lambda a: -res[a]):
            f.write('%s %d\n' % ('%08X' % a if a else '?', res[a]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['build', 'run'])
    ap.add_argument('exe')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--patches', type=int, default=4)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--profile', metavar='GEN_DIR')
    ap.add_argument('--func', default='f_00512C30')
    args = ap.parse_args()
    if args.mode == 'build':
        build(args.exe, args.files[0], args.files[1])
        return
    res = [run(args.exe, e, args.patches, args.seed, args.profile if k == 0 else None, args.func)
           for k, e in enumerate(args.files)]
    names = set()
    for r in res:
        names |= set(r[2])
    print('status: %s' % ', '.join(r[0] for r in res))
    print('%-28s' % 'total' + ''.join('%12d' % r[1] for r in res))
    for nm in sorted(names, key=lambda x: -res[0][2].get(x, 0)):
        if all(r[2].get(nm, 0) < 1000 for r in res):
            continue
        print('%-28s' % nm + ''.join('%12d' % r[2].get(nm, 0) for r in res))
    if len(res) == 2:
        a, b = res[0][3], res[1][3]
        if a == b and res[0][4] == res[1][4]:
            print('guest memory and context: identical')
        else:
            diff = [i for i in range(0, len(a), 4) if a[i:i + 4] != b[i:i + 4]]
            print('DIFFERENT: %d words, first at %s; context %s' % (
                len(diff), ', '.join('%08X' % d for d in diff[:8]), 'same' if res[0][4] == res[1][4] else 'differs'))
        sys.exit(0 if a == b and res[0][4] == res[1][4] else 1)


if __name__ == '__main__':
    main()
