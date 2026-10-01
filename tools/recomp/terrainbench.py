"""Terrain renderer benchmark and differential test on synthetic and captured frames.

armbench.py runs every function from random inputs, which never gets far into
the terrain renderer (f_00512C30, the hottest guest function in gameplay). This
builds a frame for it instead: a camera anywhere above a grid of terrain patches
(base and morph vertices, triangle strips, texture groups, detail pass), the
batch pool and fake Direct3D objects whose COM methods are host stubs that log
every call into guest memory, then runs it in Unicorn ARM and counts the ARM
instructions executed per function. It can also replay frames captured from the
running game (IGI_TERRAIN_DUMP, runtime/src/capture.c).

  terrainbench.py build IGI.exe GEN_DIR OUT.elf
  terrainbench.py run IGI.exe A.elf [B.elf] [--patches N] [--seed S] [--frame F]
                  [--native 0|1] [--profile GEN_DIR [--func f_XXXXXXXX]]
  terrainbench.py diff IGI.exe A.elf [--seed S --count N | --frame F ...]

run: with two ELFs (e.g. before and after a generator change) also checks that
the final guest memory (outside the dead stack) and context are identical.
diff: runs one ELF with g_igi_native = 0 (recompiled code) and 1 (the native
version in runtime/src/native_terrain.c) and compares them the same way,
including the logged Direct3D calls.

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
 * (thunk address: THUNK_BASE + vtable slot / 4 * 0x80 + 4 * k), return 0 (S_OK).
 * Every call is appended to the log at guest address g_com_log (when set): count,
 * then per call the vtable slot, k and the k arguments. */
uint32_t g_com_log;
static void com_stub(Ctx *c)
{
    uint32_t off = g_current_thunk - THUNK_BASE;
    uint32_t k = (off & 0x7Fu) / 4u;
    if (g_com_log) {
        uint32_t n = RD32(g_com_log), p = g_com_log + 4u + 4u * n;
        WR32(p, (off >> 7) * 4u);
        WR32(p + 4u, k);
        for (uint32_t i = 0; i < k; i++)
            WR32(p + 8u + 4u * i, RD32(c->r[4] + 4u + 4u * i));
        WR32(g_com_log, n + 2u + k);
    }
    c->r[4] += 4u + 4u * k;
    c->r[0] = 0;
}
GuestFn thunk_lookup(uint32_t a) { (void)a; return com_stub; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; for (;;) __asm__ volatile("udf #1"); }
void rt_probe(Ctx *c, uint32_t f) { (void)c; (void)f; }
void rt_log(const char *fmt, ...) { (void)fmt; }
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
COM_LOG_SIZE = 0x40000


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


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def scene(npatch=12, seed=1):
    """A terrain frame: camera at a random place and orientation above a grid of
    16-unit patches around it (7x7 base vertices plus morph vertices, strips of
    full-triangle entries, 0-2 extra texture groups, optional detail pass). Patches
    under and behind the camera cross the near plane, others the screen edges or
    the patch range."""
    rnd = random.Random(seed)
    m = Mem()
    al = Alloc(SCENE)
    # Fake COM objects sharing one vtable.
    vt = al(0x100)
    for s in range(0, 0x100, 4):
        m.u32(vt + s, THUNK_BASE + s // 4 * 0x80 + SLOTS.get(s, 0xC))
    dev = al(16)
    m.u32(dev, vt)
    vb = al(16)
    m.u32(vb, vt)
    m.u32(0x5CA114, dev)
    m.u32(0xA7F890, vb)
    vbuf = al(0x80000)
    m.u32(0xA7F89C, vbuf)
    m.u32(0xA7E078, 0)
    m.u32(0xA7E070, rnd.randrange(0, 100))
    # Writable dummies written through at the start.
    for g in (0xBCADE8, 0xBCAD7C, 0xBCADAC, 0xBCADC4):
        m.u32(g, al(64))
    m.u32(0xC28B44, 640)
    m.u32(0xC28B48, 480)
    # Camera: position (doubles), rotation rows (right, down, forward).
    cam = (rnd.uniform(-40.0, 40.0), rnd.uniform(1.5, 12.0), rnd.uniform(-40.0, 40.0))
    for i, v in enumerate(cam):
        m.f64(0xBCAB08 + 8 * i, v)
    yaw = rnd.uniform(-math.pi, math.pi)
    pitch = rnd.uniform(-0.6, 0.3)
    fw = (math.cos(pitch) * math.sin(yaw), math.sin(pitch), math.cos(pitch) * math.cos(yaw))
    right = (math.cos(yaw), 0.0, -math.sin(yaw))
    up = cross(fw, right)
    rows = [right, tuple(-x for x in up), fw]
    for r, row in enumerate(rows):
        for k, v in enumerate(row):
            m.f32(0xBCAAE0 + 12 * r + 4 * k, v)
    m.f32(0xBCAAE0 + 36, 0.0)
    m.f32(0xC311A4, 320.0)
    m.f32(0xC311A0, 320.0)
    m.f32(0xC311B0, 320.0)
    m.f32(0xC311B4, 240.0)
    # Clip rectangle: the screen, sometimes a smaller viewport.
    inset = rnd.choice([0.0, 0.0, 16.0])
    for a, v in ((0xBCABD0, inset), (0xBCABD4, inset), (0xBCABD8, 640.0 - inset), (0xBCABDC, 480.0 - inset)):
        m.f32(a, v)
    m.f32(0xBCADC0, rnd.choice([0.5, 1.0, 2.0]))     # near plane
    m.f32(0xBCADCC, 1771.0 + rnd.uniform(30.0, 80.0))   # patch range (radius term 1024 * 1.73)
    m.f32(0xBCADA0, 1.0)
    m.f32(0xBCADC8, 2.0)
    m.f32(0xBCAD8C, 1000.0)
    m.f32(0xBCADB4, 1.0)
    m.f32(0xBCAD70, 0.5)                             # rhw scale
    m.f32(0xBCABEC, 2.0)                             # 1 / rhw scale
    m.u32(0xBCAD78, 1)
    m.f32(0xA7DA18, rnd.uniform(0.002, 0.03))        # fog density
    m.f32(0xA7DA30, 0.5)
    m.f32(0xA7DA34, 0.6)
    m.f32(0xA7DA38, 0.7)
    m.u32(0xC31088, rnd.choice([0, 0, 1]))           # lod bias
    m.f32(0xC28FC0, rnd.uniform(1.5, 3.0))           # lod scale
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
    for i in range(4):
        mt = al(0x40)
        m.u32(mt + 0x20, 0x1000 + i)
        mats.append(mt)
    # Patches on the 16-unit grid cells nearest to the camera, in random order.
    n = 7
    size = 16.0
    step = size / (n - 1)
    cx, cz = int(math.floor(cam[0] / size)), int(math.floor(cam[2] / size))
    cells = [(cx + i, cz + j) for i in range(-3, 4) for j in range(-3, 4)]
    cells.sort(key=lambda c: (c[0] - cx) ** 2 + (c[1] - cz) ** 2)
    cells = cells[:npatch]
    rnd.shuffle(cells)
    plist = []
    for (gx, gz) in cells:
        x0, z0 = gx * size, gz * size
        nbase = n * n
        base = []
        for j in range(n):
            for i in range(n):
                base.append((x0 + i * step, rnd.uniform(-1.5, 1.5), z0 + j * step))
        # Morph vertices: edge midpoints moving to a neighbouring base vertex.
        nextra = rnd.randrange(0, 16)
        extra = []
        for e in range(nextra):
            j, i = rnd.randrange(n - 1), rnd.randrange(n - 1)
            a = j * n + i
            b = a + rnd.choice([1, n])
            pa, pb = base[a], base[b]
            extra.append(((pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2 + rnd.uniform(-0.3, 0.3),
                          (pa[2] + pb[2]) / 2, rnd.choice([a, b])))
        ntotal = nbase + nextra
        # Strips: one per grid row, entries (previous two, new); single triangles with morph vertices.
        words = []
        for r in range(n - 1):
            seq = []
            for i in range(n):
                seq += [r * n + i, (r + 1) * n + i]
            for k in range(2, len(seq)):
                words.append((seq[k - 2] << 16) | (seq[k - 1] << 8) | seq[k])
            words[-1] |= 0x10000000
        for e in range(nextra):
            v = nbase + e
            p = extra[e][3]
            q = p + 1 if (p % n) < n - 1 else p - 1
            words.append((p << 16) | (q << 8) | v | 0x10000000)
        ngroups = rnd.choice([0, 1, 2])
        detail = rnd.random() < 0.6
        rec = al(0x20 + 16 * ntotal + 16 + (max(1, ngroups) * 2 * (4 + 20 * ntotal)) + 4 + 20 * ntotal + 64)
        plist.append(rec)
        m.f64(rec, x0 + size / 2)
        m.f64(rec + 8, 0.0)
        m.f64(rec + 16, z0 + size / 2)
        m.u32(rec + 0x18, (nbase << 16) | ntotal)
        m.u32(rec + 0x1C, 20)
        e = rec + 0x20
        for (x, y, z) in base:
            m.f32(e, x)
            m.f32(e + 4, y)
            m.f32(e + 8, z)
            m.f32(e + 12, rnd.uniform(-0.5, 0.5))
            e += 16
        for (x, y, z, p) in extra:
            m.f32(e, x)
            m.f32(e + 4, y)
            m.f32(e + 8, z)
            m.u32(e + 12, p)
            e += 16
        il = al(4 * len(words))
        for k, w in enumerate(words):
            m.u32(il + 4 * k, w)
        m.u32(e, rnd.choice([0, 4, 0x13]))
        m.u32(e + 4, il)
        m.u32(e + 8, ngroups)
        m.u32(e + 12, len(words))
        e += 16

        def records(e, mat):
            m.u32(e, mat)
            e += 4
            for v in range(nbase):
                m.u32(e, 32 * v + 4 * rnd.randrange(8))
                m.f32(e + 4, rnd.uniform(0.0, 4.0))
                m.f32(e + 8, rnd.uniform(0.0, 4.0))
                e += 12
            for (_, _, _, p) in extra:
                m.u32(e, (p << 16) | (32 * p + 4 * rnd.randrange(8)))
                for k in range(4):
                    m.f32(e + 4 + 4 * k, rnd.uniform(-1.0, 4.0))
                e += 20
            return e
        for g in range(max(1, ngroups)):
            for pas in range(2):
                e = records(e, rnd.choice(mats))
        if detail:
            e = records(e, mats[3])
        else:
            m.u32(e, 0)
    m.u32(0xB16C9C, len(plist))
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


class Frame:
    """Guest state at the entry of f_00512C30."""
    def __init__(self, mem, esp, regs=None, fcw=0x27F, gsize=None, com_log=0x02F80000):
        self.com_log = com_log      # COM call log (compared like the rest of guest memory)
        self.mem = mem
        self.esp = esp
        self.regs = regs or [0x11111111, 0x22222222, 0x33333333, 0x44444444, esp, 0x55555555, 0x66666666,
                             0x77777777]
        self.fcw = fcw
        self.gsize = gsize or armbench.GSIZE


def load_frame(path):
    """A frame captured from the game with IGI_TERRAIN_DUMP (runtime/src/capture.c).
    The Direct3D objects are replaced by the stub ones and the vertex buffer by a
    fresh area above the captured memory."""
    data = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<2I', data, 0)
    assert magic == 0x44544749 and ver in (1, 2), path
    if ver == 1:
        esp, eax, ecx, fcw = struct.unpack_from('<4I', data, 8)
        regs = list(struct.unpack_from('<8I', data, 24))
        off = 56
    else:   # version 2 (IGI_NATIVE_DUMP format, see nativebench.load_capture)
        fn, esp, fcw = struct.unpack_from('<3I', data, 8)
        assert fn == 0x00512C30, '%s: not a terrain capture' % path
        regs = list(struct.unpack_from('<8I', data, 28))
        off = 124
    m = Mem()
    top = 0
    while True:
        a, n = struct.unpack_from('<II', data, off)
        off += 8
        if not n:
            break
        m.raw(a, data[off:off + n])
        off += n
        top = max(top, a + n)
    al = Alloc((top + 0xFFFF) & ~0xFFFF)
    vt = al(0x100)
    for s in range(0, 0x100, 4):
        m.u32(vt + s, THUNK_BASE + s // 4 * 0x80 + SLOTS.get(s, 0xC))
    dev = al(16)
    m.u32(dev, vt)
    vb = al(16)
    m.u32(vb, vt)
    m.u32(0x5CA114, dev)
    m.u32(0xA7F890, vb)
    m.u32(0xA7F89C, al(0x400000))
    log = al(COM_LOG_SIZE)
    gsize = (al.p + 0xFFFFF) & ~0xFFFFF
    return Frame(m, esp, regs, fcw, gsize, log)


def run(exe, path, npatch, seed, prof_gen=None, prof_func='f_00512C30', native=None, frame=None, setup=None):
    """Runs f_00512C30 on a synthetic scene (or a Frame, e.g. a captured one).
    native: value for g_igi_native (None leaves the ELF's default)."""
    if frame is None:
        frame = Frame(scene(npatch, seed), 0x02EFF000)
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
    uc.mem_map(ab.GMEM, frame.gsize)
    uc.mem_write(ab.GMEM + 0x400000, ab.guest_image(exe))
    uc.mem_write(syms['g_mem'], struct.pack('<I', ab.GMEM))
    m, esp = frame.mem, frame.esp
    for a, b in m.w.items():
        uc.mem_write(ab.GMEM + a, b)
    if native is not None:
        uc.mem_write(syms['g_igi_native'], struct.pack('<i', native))
    uc.mem_write(syms['g_com_log'], struct.pack('<I', frame.com_log))
    uc.mem_write(ab.GMEM + esp, struct.pack('<I', 0xFDFDFD00))
    regs = list(frame.regs)
    regs[4] = esp
    ctx = struct.pack('<8I3I4x8dIII', *regs, 0, 0, 0x00020000, *([0.0] * 8), 0, frame.fcw, 0)
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
    if setup:
        setup(uc, syms)          # extra hooks (diagnostics)
    status = 'ok'
    try:
        uc.emu_start(syms['f_00512C30'] | 1, ab.STOP, count=200_000_000)
    except UcError as e:
        status = 'fault %s pc=%08X' % (e, uc.reg_read(A.UC_ARM_REG_PC))
    if status == 'ok' and uc.reg_read(A.UC_ARM_REG_PC) & ~1 != ab.STOP:
        status = 'limit'
    mem = bytes(uc.mem_read(ab.GMEM, frame.gsize))
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


# Guest stack below the entry esp: dead after the return (locals, call frames).
STACK_SCRATCH = 0x40000
# Context fields a caller can observe: eax, ebx, esp, ebp, esi, edi, x87 top.
CTX_LIVE = [(0, 4), (12, 4), (16, 4), (20, 4), (24, 4), (28, 4), (112, 4)]


def compare(ra, rb, esp):
    """Differences between two runs: guest memory outside the dead stack area and
    the live context fields. Returns a list of descriptions (empty: identical)."""
    a, b = ra[3], rb[3]
    lo = esp - STACK_SCRATCH
    out = []
    if a[:lo] != b[:lo] or a[esp:] != b[esp:]:
        for base, end in ((0, lo), (esp, len(a))):
            for pg in range(base, end, 4096):
                if a[pg:pg + 4096] == b[pg:pg + 4096]:
                    continue
                for i in range(pg, min(pg + 4096, end), 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        out.append('mem %08X: %s / %s' % (i, a[i:i + 4].hex(), b[i:i + 4].hex()))
    for off, n in CTX_LIVE:
        if ra[4][off:off + n] != rb[4][off:off + n]:
            out.append('ctx +%d: %s / %s' % (off, ra[4][off:off + n].hex(), rb[4][off:off + n].hex()))
    return out


def diff_mode(args):
    """Recompiled (g_igi_native = 0) against native (1) on several scenes."""
    cases = [('frame %s' % os.path.basename(p), p) for p in args.frame] or \
        [('seed %3d' % seed, seed) for seed in range(args.seed, args.seed + args.count)]
    fails = 0
    tot = [0, 0]
    for label, case in cases:
        frame = load_frame(case) if isinstance(case, str) else Frame(scene(args.patches, case), 0x02EFF000)
        res = [run(args.exe, args.files[0], 0, 0, native=k, frame=frame) for k in (0, 1)]
        mem = res[0][3]
        verts = struct.unpack_from('<I', mem, 0xA7E078)[0]
        clips = struct.unpack_from('<I', mem, 0xA7E070)[0]
        d = compare(res[0], res[1], frame.esp)
        tot[0] += res[0][1]
        tot[1] += res[1][1]
        ok = not d and res[0][0] == 'ok' and res[1][0] == 'ok'
        fails += not ok
        print('%s: %s  vertices %5d clip counter %4d  ARM insns %10d -> %9d (%.1fx)%s' % (
            label, 'same' if ok else 'DIFFERENT', verts, clips, res[0][1], res[1][1],
            res[0][1] / max(1, res[1][1]), '' if res[0][0] == res[1][0] == 'ok' else
            ' status %s / %s' % (res[0][0], res[1][0])))
        for line in d[:12]:
            print('    ' + line)
        if len(d) > 12:
            print('    ... %d differences' % len(d))
    print('%d/%d scenes identical; ARM instructions %d -> %d (%.2fx)' % (
        len(cases) - fails, len(cases), tot[0], tot[1], tot[0] / max(1, tot[1])))
    sys.exit(1 if fails else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['build', 'run', 'diff'])
    ap.add_argument('exe')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--patches', type=int, default=12)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--count', type=int, default=20, help='diff: number of scenes (seeds from --seed)')
    ap.add_argument('--native', type=int, choices=[0, 1], help='run: g_igi_native value')
    ap.add_argument('--frame', action='append', default=[], help='diff: captured frame (IGI_TERRAIN_DUMP file)')
    ap.add_argument('--profile', metavar='GEN_DIR')
    ap.add_argument('--func', default='f_00512C30')
    args = ap.parse_args()
    if args.mode == 'build':
        build(args.exe, args.files[0], args.files[1])
        return
    if args.mode == 'diff':
        diff_mode(args)
        return
    frame = load_frame(args.frame[0]) if args.frame else None
    res = [run(args.exe, e, args.patches, args.seed, args.profile if k == 0 else None, args.func, args.native,
               frame) for k, e in enumerate(args.files)]
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
        d = compare(res[0], res[1], frame.esp if frame else 0x02EFF000)
        print('guest memory and context: identical' if not d else 'DIFFERENT: %d, first: %s' % (len(d), d[0]))
        sys.exit(1 if d else 0)


if __name__ == '__main__':
    main()
