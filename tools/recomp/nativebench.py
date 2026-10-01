"""Differential test and benchmark of the hand-decompiled game functions.

Every function in the NATIVE table of gen.py has a native version
(runtime/src/native_*.c) that must behave exactly like the recompiled code.
This runs the function's entry (f_XXXXXXXX, which holds the native hook) in
Unicorn ARM, built with the vitasdk compiler like the Vita build, from one guest
state twice: with g_igi_native = 0 (recompiled code) and 1 (native), and
compares
  - all of guest memory, including the stack below the entry esp (the guest's
    dead locals: guest code reads uninitialised locals left by earlier calls);
  - eax, ecx, edx, esp, ebx, ebp, esi, edi, the x87 top, condition codes and
    the x87 registers that were live at the entry;
  - every guest call made at any depth, in order: entries of generated functions
    (target, all registers, x87 top and condition codes, return address and
    arguments) and host thunk calls (target, esp, eax, ecx, edx, return address
    and arguments).
The states are synthetic (per-function generators below, with random and
special values) or captured from the running game (IGI_NATIVE_DUMP,
runtime/src/capture.c). Host thunks are stubs that record the call, pop the
arguments (sizes from the thunk names in the capture) and return 0; a vertex
buffer Lock returns a fresh buffer. It also counts the ARM instructions
executed, per host function, and with --profile per guest instruction of the
recompiled function.

  nativebench.py build IGI.exe GEN_DIR OUT.elf
  nativebench.py diff IGI.exe OUT.elf FN [--synth N] [--seed S] [--capture FILE ...]
  nativebench.py run IGI.exe OUT.elf FN (--capture FILE | --seed S) [--native 0|1] [--profile GEN_DIR]

FN is the hex entry address (e.g. 49B700). Needs unicorn and capstone (pip) and
VITASDK (default ~/vitasdk).
"""
import argparse
import bisect
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import armbench  # noqa: E402
import terrainbench  # noqa: E402

STUB = r'''
#include "recomp.h"
#include "runtime.h"
/* Host thunk stub. g_thunk_tab (guest address) has one word per thunk index:
 * argument bytes popped (low 16 bits; 0xFFFF: unknown, stop) and a kind (high
 * 16 bits): 1 Lock (stores g_lock_buf through the third argument), 2 Lock that
 * fails, 3 Lock that returns a null pointer. The harness records every call. */
uint32_t g_thunk_tab, g_lock_buf;
static void thunk_stub(Ctx *c)
{
    uint32_t i = (g_current_thunk - THUNK_BASE) / THUNK_STRIDE;
    uint32_t e = RD32(g_thunk_tab + 4u * i);
    uint32_t bytes = e & 0xFFFFu, kind = e >> 16;
    if (bytes == 0xFFFFu)
        __asm__ volatile("udf #3");
    c->r[0] = 0;
    if (kind == 1u || kind == 3u)
        WR32(RD32(c->r[4] + 12u), kind == 1u ? g_lock_buf : 0u);
    if (kind == 2u)
        c->r[0] = 0x80004005u;
    c->r[4] += 4u + bytes;
}
GuestFn thunk_lookup(uint32_t a) { (void)a; return thunk_stub; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; (void)fmt; for (;;) __asm__ volatile("udf #1"); }
void rt_probe(Ctx *c, uint32_t f) { (void)c; (void)f; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_missing_import(Ctx *c, const char *n) { (void)c; (void)n; __asm__ volatile("udf #2"); }
int *__errno(void) { static int e; return &e; }
''' + terrainbench.STUB[terrainbench.STUB.index('/* On the Vita memcpy'):]

# Argument bytes (this included) and kind of the host thunks the tested code calls.
THUNKS = {
    'IDirect3DVertexBuffer7::Lock': (16, 1),
    'IDirect3DVertexBuffer7::Unlock': (4, 0),
    'IDirect3DDevice7::SetTexture': (12, 0),
    'IDirect3DDevice7::SetRenderState': (12, 0),
    'IDirect3DDevice7::DrawPrimitive': (24, 0),
    'IDirect3DDevice7::DrawIndexedPrimitive': (32, 0),
    'IDirect3DDevice7::DrawPrimitiveVB': (24, 0),
    'IDirect3DDevice7::DrawIndexedPrimitiveVB': (32, 0),
}
KIND = {'lock': 1, 'lockfail': 2, 'locknull': 3}
# Stack arguments of the guest functions called (recorded and compared with the
# return address; default 4). The stack above them is the caller's dead frame.
CALL_ARGS = {}

GSTACK = 0x02EFF000          # entry esp of synthetic states
LIMIT = 40_000_000           # ARM instructions per run (a whole frame's terrain walk takes about 1M)
EXTRA = 0x800000             # mapped above the guest memory: thunk table, lock buffer
CTX_REGS = [(0, 'eax'), (4, 'ecx'), (8, 'edx'), (12, 'ebx'), (16, 'esp'), (20, 'ebp'), (24, 'esi'), (28, 'edi')]
# The whole guest stack is compared, including the "dead" part below the entry
# esp: guest functions read uninitialised locals (garbage left by earlier calls),
# so a native function leaves its frame as the guest code does.


class State:
    """Guest state at the entry of the tested function."""
    def __init__(self, mem, esp, regs, thunks, fcw=0x27F, ftop=0, fsw=0, st=None, gsize=None, image=True):
        self.mem = mem              # terrainbench.Mem (address -> bytes)
        self.esp = esp
        self.regs = regs
        self.thunks = thunks        # thunk address -> name (or name#kind)
        self.fcw, self.ftop, self.fsw = fcw, ftop, fsw
        self.st = st or [0.0] * 8
        self.gsize = gsize or armbench.GSIZE
        self.image = image          # write the exe image first (synthetic states)


def load_capture(path):
    data = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<2I', data, 0)
    assert magic == 0x44544749 and ver == 2, path
    fn, esp, fcw, ftop, fsw = struct.unpack_from('<5I', data, 8)
    regs = list(struct.unpack_from('<8I', data, 28))
    st = list(struct.unpack_from('<8d', data, 60))
    m = terrainbench.Mem()
    off = 124
    top = 0
    while True:
        a, n = struct.unpack_from('<II', data, off)
        off += 8
        if not n:
            break
        m.raw(a, data[off:off + n])
        off += n
        top = max(top, a + n)
    thunks = {}
    while off < len(data):
        t, = struct.unpack_from('<I', data, off)
        off += 4
        if not t:
            break
        n, = struct.unpack_from('<I', data, off)
        thunks[t] = data[off + 4:off + 4 + n].decode()
        off += 4 + n
    gsize = (top + 0xFFFFF) & ~0xFFFFF
    return fn, State(m, esp, regs, thunks, fcw, ftop, fsw, st, gsize, image=False)


# ---- synthetic states ------------------------------------------------------

def special(rnd, lo, hi):
    """A float in [lo, hi), sometimes a special value."""
    r = rnd.random()
    if r < 0.03:
        return rnd.choice([0.0, -0.0, 1e-40, -3e-39, float('inf'), float('-inf'), float('nan'), 3.0e38, -1e-30])
    return rnd.uniform(lo, hi)


def fake_com(m, al, methods):
    """COM object whose vtable slots (offset -> thunk address) are given."""
    vt = al(0x100)
    for off, t in methods.items():
        m.u32(vt + off, t)
    obj = al(16)
    m.u32(obj, vt)
    return obj


def synth_skin(seed, bases):
    """0x49B700: a mesh with up to 80 vertices and 24 extra influences on a random
    skeleton; append or discard lock, lock failures, rigid or weighted."""
    rnd = random.Random(seed)
    m = terrainbench.Mem()
    al = terrainbench.Alloc(0x02100000)
    lock = rnd.choice(['lock'] * 8 + ['lockfail', 'locknull'])
    thunks = {0xFE000100: 'IDirect3DVertexBuffer7::Lock#' + lock, 0xFE000104: 'IDirect3DVertexBuffer7::Unlock'}
    vb = fake_com(m, al, {0x0C: 0xFE000100, 0x10: 0xFE000104})
    nverts = rnd.choice([0, 1, 3, rnd.randrange(1, 80)])
    nextra = rnd.choice([0, rnd.randrange(0, 24)])
    cap = rnd.choice([4000, 100, nverts + rnd.randrange(0, 3)])
    m.u32(0xBCAD90, vb)
    m.u32(0xBCAD94, cap)
    m.u32(0xBCAD98, rnd.randrange(0, max(1, cap)))
    nb = rnd.randrange(1, 12)
    bones = al(nb * 0x84)
    for b in range(nb):
        for k in range(0x84 // 4):
            m.f32(bones + 4 * k + 0x84 * b, special(rnd, -2.0, 2.0) if k >= 6 else rnd.uniform(-9.0, 9.0))
    skel = al(16)
    m.u32(skel + 8, bones)
    verts = al(0x28 * (nverts + nextra))
    for i in range(nverts + nextra):
        v = verts + 0x28 * i
        for k in range(8):
            m.f32(v + 4 * k, special(rnd, -50.0, 50.0) if k < 3 else special(rnd, -1.0, 1.0))
        m.f32(v + 0x20, special(rnd, 0.0, 1.0))
        tgt = rnd.randrange(0, max(1, nverts)) if i >= nverts else rnd.randrange(0, 0x10000)
        m.u32(v + 0x24, (rnd.randrange(nb) << 16) | tgt)
    mesh = al(0x40)
    m.u32(mesh + 0x0C, nextra)
    m.u32(mesh + 0x10, nverts)
    m.u32(mesh + 0x18, verts)
    obj = al(0x40)
    m.raw(obj + 0x14, b'mesh\0')
    rigid = rnd.choice([0, 0, 1, 0x100])
    for k, a in enumerate((0xFDFDFD00, mesh, obj, skel, rigid)):
        m.u32(GSTACK + 4 * k, a)
    regs = [rnd.getrandbits(32) for _ in range(8)]
    regs[4] = GSTACK
    return State(m, GSTACK, regs, thunks)


class Peek:
    """Reads the guest memory of a captured state (pages plus later writes)."""
    def __init__(self, m):
        self.m = m

    def raw(self, a, n):
        out = b''
        while n:
            k = min(n, 4096 - (a & 0xFFF))
            pg = self.m.w.get(a & ~0xFFF)
            out += pg[a & 0xFFF:(a & 0xFFF) + k] if pg is not None and len(pg) == 4096 else bytes(k)
            a += k
            n -= k
        return out

    def u32(self, a):
        return struct.unpack('<I', self.raw(a, 4))[0]

    def u16(self, a):
        return struct.unpack('<H', self.raw(a, 2))[0]

    def f32(self, a):
        return struct.unpack('<f', self.raw(a, 4))[0]

    def f64(self, a):
        return struct.unpack('<d', self.raw(a, 8))[0]


def base_state(rnd, bases, fn):
    if not bases:
        sys.exit('synthetic states of %08X start from captured ones: pass --base FILE (or glob)' % fn)
    cfn, st = load_capture(rnd.choice(bases))
    assert cfn == fn
    return st


def jitter(rnd, v, amount):
    r = rnd.random()
    if r < 0.02:
        return rnd.choice([0.0, -0.0, float('nan'), float('inf'), -1e-40, 1e30])
    return v + rnd.uniform(-amount, amount) * max(1.0, abs(v))


def synth_mesh(seed, bases):
    """0x4A1570: a captured call with the camera, the object's transform and scale,
    the projection, clip rectangle, near plane and far limit, the vertices and the
    stale locals the guest reads randomised (the mesh type too, sometimes)."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x004A1570)
    m, pk = st.mem, Peek(st.mem)
    obj, ctx = pk.u32(st.esp + 4), pk.u32(st.esp + 8)
    small = rnd.choice([0.0, 0.001, 0.01, 0.05])
    for i in range(9):
        m.f32(0xBCAAE0 + 4 * i, jitter(rnd, pk.f32(0xBCAAE0 + 4 * i), small))
        m.f32(obj + 0x70 + 4 * i, jitter(rnd, pk.f32(obj + 0x70 + 4 * i), small))
    for i in range(3):
        m.f64(0xBCAB08 + 8 * i, pk.f64(0xBCAB08 + 8 * i) + rnd.uniform(-2.0, 2.0) * small * 20)
        m.f64(obj + 0x20 + 8 * i, pk.f64(obj + 0x20 + 8 * i) + rnd.uniform(-2.0, 2.0) * small * 20)
    sc = rnd.choice(['keep', 'one', 'rand', 'tie', 'nan'])
    for i in range(3):
        v = {'keep': pk.f32(obj + 0x98 + 4 * i), 'one': 1.0, 'rand': rnd.uniform(0.3, 3.0), 'tie': 1.5,
             'nan': rnd.choice([1.0, float('nan')])}[sc]
        m.f32(obj + 0x98 + 4 * i, v)
    for a in (0xBCAB28, 0xBCAB2C, 0xBCAB30, 0xBCAB34):
        m.f32(a, jitter(rnd, pk.f32(a), small))
    inset = rnd.choice([0.0, 0.0, 40.0, 200.0])
    for a, sgn in ((0xBCABD0, 1), (0xBCABD4, 1), (0xBCABD8, -1), (0xBCABDC, -1)):
        m.f32(a, pk.f32(a) + sgn * inset)
    m.f32(0xBCADC0, rnd.choice([pk.f32(0xBCADC0), 0.1, 1.0, 20.0, 200.0]))
    m.f32(ctx + 8, rnd.choice([pk.f32(ctx + 8), 1e9, rnd.uniform(1.0, 3000.0)]))
    S = st.esp - 0xB0
    for k in range(3):
        m.u32(S + 0x40 + 4 * k, rnd.getrandbits(32))
    model = pk.u32(obj + 0x6C)
    mesh = pk.u32(pk.u32(pk.u32(model) + 0xC) + 0xC) if model and pk.u32(model) else 0
    if mesh:
        typ = pk.u32(mesh + 0x20)
        geom = pk.u32(mesh + 0x24)
        if typ in (0, 3):
            n, src, stride = (pk.u32(geom + 0xC), pk.u32(geom + 0x10), 0x20) if typ == 0 else \
                (pk.u32(geom + 0x10), pk.u32(geom + 0x14), 0x28)
            amount = rnd.choice([0.0, 0.01, 0.2])
            for i in range(min(n, 2000)):
                for k in range(3):
                    a = src + stride * i + 4 * k
                    m.f32(a, jitter(rnd, pk.f32(a), amount))
        if rnd.random() < 0.08:
            m.u32(mesh + 0x20, 5)
    if rnd.random() < 0.03:
        m.u32(obj + 0xEC, 0)
    return st


def synth_tree(seed, bases):
    """0x4D14E0: a captured frame with the camera moved and turned (a little or
    anywhere), the field of view, frame counter (stale caches: new patch requests
    and evictions), level-of-detail distance, height limit and the stale locals
    randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x004D14E0)
    m, pk = st.mem, Peek(st.mem)
    move = rnd.choice([0.0, 1.0, 50.0, 2000.0])
    for i in range(3):
        m.f64(0xBCAB08 + 8 * i, pk.f64(0xBCAB08 + 8 * i) + rnd.uniform(-move, move))
    if rnd.random() < 0.5:
        yaw, pitch, roll = (rnd.uniform(-math.pi, math.pi) for _ in range(3))
        cy, sy, cp, sp, cr, sr = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch), math.cos(roll), \
            math.sin(roll)
        rows = [(cy * cr + sy * sp * sr, cp * sr, -sy * cr + cy * sp * sr),
                (-cy * sr + sy * sp * cr, cp * cr, sy * sr + cy * sp * cr),
                (sy * cp, -sp, cy * cp)]
        for r_, row in enumerate(rows):
            for k, v in enumerate(row):
                m.f32(0xBCAAE0 + 12 * r_ + 4 * k, v)
    for a in (0xBCAB20, 0xBCAB24):
        m.f32(a, pk.f32(a) * rnd.choice([1.0, 0.5, 1.3, 2.0]))
    fr = pk.u32(0xC10380)
    m.u32(0xC10380, rnd.choice([fr, fr, fr + 1, fr + 7, rnd.getrandbits(16)]))
    lod = pk.u32(0xB16C6C)
    m.u32(0xB16C6C, rnd.choice([lod, lod // 2, lod + lod // 2, rnd.randrange(0, 64)]))
    settings = pk.u32(0xA44344)
    if settings and rnd.random() < 0.3:
        lim = pk.u32(settings + 0xDFC)
        if lim:
            m.f64(lim, rnd.uniform(-5e8, 5e8))
    S = ((st.esp - 4) & ~7) - 0x640
    if rnd.random() < 0.5:
        for off in range(0x0C, 0x100, 4):
            m.u32(S + off, rnd.getrandbits(32))
    return st


def synth_adpcm(seed, bases):
    """0x496280: random input bytes, frame counts (none, one, odd, long), channel
    counts (the guest's 1 and 2, sometimes 0 or negative), predictors, step
    indices (sometimes out of range), nibble flag, stale frame; sometimes the
    output overlapping the input, the state or the stack."""
    rnd = random.Random(seed)
    m = terrainbench.Mem()
    al = terrainbench.Alloc(0x02100000)
    ch = rnd.choice([1, 2, 2, 1, 2, rnd.choice([0, -1])])
    count = rnd.choice([0, 1, 2, 3, rnd.randrange(1, 64), rnd.randrange(64, 3000), -rnd.randrange(1, 5)])
    nin = max(0, count) * max(1, abs(ch)) // 2 + 4
    src = al(nin + 16)
    m.raw(src, bytes(rnd.getrandbits(8) for _ in range(nin)))
    nout = 2 * max(0, count) * max(1, abs(ch)) + 16
    out = al(nout + 16)
    st = al(16)
    m.raw(st, struct.pack('<H', ch & 0xFFFF))
    for k in range(2):
        m.raw(st + 2 + 2 * k, struct.pack('<H', rnd.choice([0, 0x7FFF, 0x8000, rnd.getrandbits(16)])))
        idx = rnd.choice([0, 88, rnd.randrange(0, 89), rnd.randrange(0, 89)] +
                         ([rnd.choice([89, 0xFFFF, 200])] if rnd.random() < 0.1 else []))
        m.raw(st + 6 + 2 * k, struct.pack('<H', idx))
    m.raw(st + 10, bytes([rnd.choice([0, 1, 0x80])]))
    r = rnd.random()
    if r < 0.03:
        out = src + rnd.randrange(0, 8)
    elif r < 0.05:
        out = st + rnd.choice([8, 10])
    elif r < 0.06:
        out = GSTACK - 0x2A - 2 * max(0, count) * max(1, abs(ch))   # the last sample on the saved edi
    for off in range(-0x40, 0, 4):
        m.u32(GSTACK + off, rnd.getrandbits(32))
    for k, a in enumerate((0xFDFDFD00, src, out, count & 0xFFFFFFFF, st)):
        m.u32(GSTACK + 4 * k, a)
    regs = [rnd.getrandbits(32) for _ in range(8)]
    regs[4] = GSTACK
    return State(m, GSTACK, regs, {})


def synth_deform(seed, bases):
    """0x49CC80: a captured call with the rigid and reversed flags, the extent,
    segment count, rotations and segment data, the vertices (some on segment
    boundaries or outside the extent), the rounding mode, the vertex buffer fill
    (append or discard), Lock failures and the stale locals randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x0049CC80)
    m, pk = st.mem, Peek(st.mem)
    mesh, obj = pk.u32(st.esp + 4), pk.u32(st.esp + 8)
    if rnd.random() < 0.3:
        m.raw(obj + 0x5B4, bytes([rnd.choice([0, 1, 0x80])]))
    if rnd.random() < 0.3:
        m.raw(obj + 0x5B5, bytes([rnd.choice([0, 1])]))
    x0, x1 = pk.f32(obj + 0xFC), pk.f32(obj + 0x100)
    if rnd.random() < 0.4:
        # Extents that keep the segments near the object's: wild segments read
        # unrelated memory, where two different NaNs can meet in one operation,
        # and the NaN that results depends on the compiler's operand order.
        x0, x1 = rnd.choice([(x0, x1), (x1, x0), (x0 + (x1 - x0) * 0.25, x1), (x0, x0 + (x1 - x0) * 0.7),
                             (x0, x0)])
        m.f32(obj + 0xFC, x0)
        m.f32(obj + 0x100, x1)
    nseg = pk.u32(obj + 0x59C)
    if rnd.random() < 0.3:
        nseg = rnd.choice([0, 1, 2, nseg, nseg + 1, 63, 64, 65, 100])
        m.u32(obj + 0x59C, nseg)
    small = rnd.choice([0.0, 0.001, 0.1])
    for i in range(9):
        m.f32(obj + 0x70 + 4 * i, jitter(rnd, pk.f32(obj + 0x70 + 4 * i), small))
    for k in range(min(nseg + 2, 27)):   # the object's other fields follow at 0x59C
        for j in range(10):
            a = obj + 0x200 + 40 * k + 4 * j
            m.f32(a, jitter(rnd, pk.f32(a), small))
        for j in range(3):
            a = obj + 0x104 + 12 * k + 4 * j
            m.f32(a, jitter(rnd, pk.f32(a), small))
    n, src = pk.u32(mesh + 0xC), pk.u32(mesh + 0x10)
    if rnd.random() < 0.15:
        n = rnd.choice([0, 1, 2, n // 2])
        m.u32(mesh + 0xC, n)
    span = (x1 - x0) if x1 != x0 else 1.0
    for i in range(min(n, 4000)):
        a = src + 0x20 * i
        r = rnd.random()
        if r < 0.1 and nseg:
            m.f32(a, x0 + span * rnd.randrange(0, nseg + 1) / nseg)
        elif r < 0.15:
            m.f32(a, rnd.uniform(x0 - span, x1 + span))
        else:
            m.f32(a, jitter(rnd, pk.f32(a), small))
        for k in range(1, 6):
            m.f32(a + 4 * k, jitter(rnd, pk.f32(a + 4 * k), small))
    if rnd.random() < 0.3:
        st.fcw = (st.fcw & ~0xC00) | (rnd.randrange(4) << 10)
    cap, used = pk.u32(0xBCAD94), pk.u32(0xBCAD98)
    if rnd.random() < 0.3:
        m.u32(0xBCAD98, rnd.choice([0, cap - n, cap - n + 1, cap, used]) & 0xFFFFFFFF)
    if rnd.random() < 0.1:
        for t, name in list(st.thunks.items()):
            if name == 'IDirect3DVertexBuffer7::Lock':
                st.thunks[t] = name + '#' + rnd.choice(['lockfail', 'locknull'])
    if rnd.random() < 0.5:
        for off in range(-0x54, 0x0C, 4):
            if off not in (0, 4, 8):
                m.u32(st.esp + off, rnd.getrandbits(32))
    return st


def synth_sphere(seed, bases):
    """0x49FEB0: a captured call with the sphere moved anywhere around the view
    volume (often onto a plane, the near or the far distance exactly), the
    radius (zero, negative, special values), the planes, near and far, with or
    without the inside flag, and the stale frame randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x0049FEB0)
    m, pk = st.mem, Peek(st.mem)
    t = pk.u32(st.esp + 4)
    s = pk.u32(t)
    near, far = pk.f32(0xBCADDC), pk.f32(0xBCADCC)
    if rnd.random() < 0.3:
        near, far = rnd.choice([(near, far), (1.0, 1000.0), (0.0, 0.0), (far, near)])
        m.f32(0xBCADDC, near)
        m.f32(0xBCADCC, far)
    r = rnd.choice([pk.f32(t + 4), 0.0, -5.0, rnd.uniform(0.0, 5000.0), rnd.uniform(0.0, 50.0)])
    if rnd.random() < 0.03:
        r = rnd.choice([float('nan'), float('inf'), -0.0])
    m.f32(t + 4, r)
    z = rnd.choice([pk.f32(s + 8), rnd.uniform(-2 * far, 2 * far), near - r, far + r, -r, r + 409.6,
                    rnd.uniform(-100.0, 100.0)])
    for a in (0xB46CE4, 0xB817A4, 0xB46D1C, 0xB46D20, 0xB816FC, 0xB46D18, 0xB8179C, 0xB46CE8):
        if rnd.random() < 0.2:
            m.f32(a, jitter(rnd, pk.f32(a), 0.1))
    # x and y on a side plane sometimes: x = k * z + r / scale.
    k, sc = pk.f32(0xB46CE4), pk.f32(0xB817A4)
    x = rnd.choice([pk.f32(s), rnd.uniform(-abs(z) - 100, abs(z) + 100),
                    k * z + (r / sc if sc else 0.0), k * z - (r / sc if sc else 0.0)])
    y = rnd.choice([pk.f32(s + 4), rnd.uniform(-abs(z) - 100, abs(z) + 100)])
    if rnd.random() < 0.03:
        x = float('nan')
    m.f32(s, x)
    m.f32(s + 4, y)
    m.f32(s + 8, z)
    if rnd.random() < 0.3:
        m.u32(t + 8, 0)
    elif not pk.u32(t + 8):
        m.u32(t + 8, st.esp + 0x40)
    if rnd.random() < 0.5:
        for off in range(-0x30, 0, 4):
            m.u32(st.esp + off, rnd.getrandbits(32))
    return st


def synth_bend(seed, bases):
    """0x49BE10: a captured call with the camera, the object's transform, sphere
    offset and radius, the rigid and reversed flags, extent, segments and
    vertices (some on segment boundaries), the projection, clip rectangle, near
    plane and far limit, the rounding mode and the stale locals randomised (the
    mesh type and the model too, sometimes)."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x0049BE10)
    m, pk = st.mem, Peek(st.mem)
    obj, ctx = pk.u32(st.esp + 4), pk.u32(st.esp + 8)
    small = rnd.choice([0.0, 0.001, 0.01, 0.05])
    for i in range(9):
        m.f32(0xBCAAE0 + 4 * i, jitter(rnd, pk.f32(0xBCAAE0 + 4 * i), small))
        m.f32(obj + 0x70 + 4 * i, jitter(rnd, pk.f32(obj + 0x70 + 4 * i), small))
    for i in range(3):
        m.f64(0xBCAB08 + 8 * i, pk.f64(0xBCAB08 + 8 * i) + rnd.uniform(-2.0, 2.0) * small * 20)
        m.f64(obj + 0x20 + 8 * i, pk.f64(obj + 0x20 + 8 * i) + rnd.uniform(-2.0, 2.0) * small * 20)
    sph = pk.u32(obj + 0x5A0)
    if sph:
        for k in range(4):
            m.f32(sph + 4 * k, jitter(rnd, pk.f32(sph + 4 * k), small))
        if rnd.random() < 0.2:
            m.u32(obj + 0x5A0, 0)
    m.f32(obj + 0x44, rnd.choice([pk.f32(obj + 0x44), 0.0, rnd.uniform(0.0, 50.0)]))
    for a in (0xBCAB28, 0xBCAB2C, 0xBCAB30, 0xBCAB34):
        m.f32(a, jitter(rnd, pk.f32(a), small))
    inset = rnd.choice([0.0, 0.0, 40.0, 200.0])
    for a, sgn in ((0xBCABD0, 1), (0xBCABD4, 1), (0xBCABD8, -1), (0xBCABDC, -1)):
        m.f32(a, pk.f32(a) + sgn * inset)
    m.f32(0xBCADC0, rnd.choice([pk.f32(0xBCADC0), 0.1, 1.0, 20.0, 200.0, 2000.0]))
    m.f32(ctx + 8, rnd.choice([pk.f32(ctx + 8), 1e9, rnd.uniform(1.0, 3000.0)]))
    if rnd.random() < 0.3:
        m.raw(obj + 0x5B4, bytes([rnd.choice([0, 1, 0x80])]))
    if rnd.random() < 0.3:
        m.raw(obj + 0x5B5, bytes([rnd.choice([0, 1])]))
    x0, x1 = pk.f32(obj + 0xFC), pk.f32(obj + 0x100)
    if rnd.random() < 0.4:
        x0, x1 = rnd.choice([(x0, x1), (x1, x0), (x0 + (x1 - x0) * 0.25, x1), (x0, x0 + (x1 - x0) * 0.7),
                             (x0, x0)])
        m.f32(obj + 0xFC, x0)
        m.f32(obj + 0x100, x1)
    nseg = pk.u32(obj + 0x59C)
    if rnd.random() < 0.3:
        nseg = rnd.choice([0, 1, 2, nseg, nseg + 1, 63, 64, 65, 100])
        m.u32(obj + 0x59C, nseg)
    for k in range(min(nseg + 2, 27)):
        for j in range(10):
            a = obj + 0x200 + 40 * k + 4 * j
            if a < obj + 0x59C:   # the segment count and the sphere pointer follow
                m.f32(a, jitter(rnd, pk.f32(a), small))
        for j in range(3):
            a = obj + 0x104 + 12 * k + 4 * j
            m.f32(a, jitter(rnd, pk.f32(a), small))
    model = pk.u32(obj + 0x6C)
    mesh = pk.u32(pk.u32(pk.u32(model) + 0xC) + 0xC) if model and pk.u32(model) else 0
    if mesh:
        geom = pk.u32(mesh + 0x24)
        n, src = pk.u32(geom + 0xC), pk.u32(geom + 0x10)
        span = (x1 - x0) if x1 != x0 else 1.0
        amount = rnd.choice([0.0, 0.01, 0.2])
        for i in range(min(n, 4000)):
            a = src + 0x20 * i
            r = rnd.random()
            if r < 0.1 and nseg:
                m.f32(a, x0 + span * rnd.randrange(0, nseg + 1) / nseg)
            elif r < 0.15:
                m.f32(a, rnd.uniform(x0 - span, x1 + span))
            else:
                m.f32(a, jitter(rnd, pk.f32(a), amount))
            for k in (1, 2):
                m.f32(a + 4 * k, jitter(rnd, pk.f32(a + 4 * k), amount))
        if rnd.random() < 0.06:
            m.u32(mesh + 0x20, 3)
    if rnd.random() < 0.03:
        m.u32(obj + 0x6C, 0)
    if rnd.random() < 0.3:
        st.fcw = (st.fcw & ~0xC00) | (rnd.randrange(4) << 10)
    S = st.esp - 0x84
    if rnd.random() < 0.5:
        for off in range(0x10, 0x84, 4):
            m.u32(S + off, rnd.getrandbits(32))
    return st


def synth_patch(seed, bases):
    """0x510010: a captured frame's requests with their flags (mirroring), level,
    light, height and material sources randomised, sometimes fewer of them, the
    pools nearly full (blocks moved to larger pools, requests dropped), the
    rounding mode and the stale frame randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x00510010)
    m, pk = st.mem, Peek(st.mem)
    n = pk.u32(0xB16CA4)
    if n > 1 and rnd.random() < 0.3:
        n = rnd.randrange(1, n + 1)
        m.u32(0xB16CA4, n)
    for i in range(n):
        req = 0xB16CE0 + 0x30 * i
        if rnd.random() < 0.5:
            m.u32(req + 0x1C, (pk.u32(req + 0x1C) & ~7) | rnd.randrange(8))
        if rnd.random() < 0.2:
            m.u32(req + 0x18, rnd.choice([12, 13, 14, 15, 16, pk.u32(req + 0x18)]))
        r24 = pk.u32(req + 0x24)
        if rnd.random() < 0.2:
            r24 = (r24 & 0xFFFF0000) | 0xFFFF
        if rnd.random() < 0.2:
            r24 &= 0xFF00FFFF
        m.u32(req + 0x24, r24)
        if rnd.random() < 0.15:
            r28 = pk.u32(req + 0x28)
            m.u32(req + 0x28, rnd.choice([0, r28 & 0xFF, r28 & 0xFFFF00FF, 0xFFFFFFFF]))
    for pool in (0xB12B60, 0xB12C40, 0xB16CC0):
        if rnd.random() < 0.25:
            cap = pk.u32(pool + 0xC)
            m.u32(pool + 8, max(0, cap - rnd.choice([2, 3, 4, 5])))
    if rnd.random() < 0.3:
        st.fcw = (st.fcw & ~0xC00) | (rnd.randrange(4) << 10)
    if rnd.random() < 0.5:
        S = (((st.esp - 4) & ~7) - 0xC20) - 0x10
        for off in range(0x10, 0x330, 4):
            if off not in range(0x148, 0x1AC):   # the material sources may be kept
                m.u32(S + off, rnd.getrandbits(32))
    return st


def synth_raycast(seed, bases):
    """0x4C1840: a captured query with its segments moved (often through the
    object's centre, sometimes degenerate), the earlier hits (none, near, far,
    on either side), the object's scale and rotation, the class filter and
    the surface flag randomised, and the stale frame randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x004C1840)
    m, pk = st.mem, Peek(st.mem)
    obj, q = pk.u32(st.esp + 4), pk.u32(st.esp + 8)
    n = pk.u32(q + 8)
    c = [pk.f64(obj + 0x20 + 8 * k) for k in range(3)]
    p0a, p1a, res = pk.u32(q), pk.u32(q + 4), pk.u32(q + 0x20)
    # Triangles of the collision mesh, to aim segments at.
    model = pk.u32(obj + 0x6C)
    tris = []
    if model and pk.u32(obj + 0xA4) != 0xFFFFFFFF:
        mesh = pk.u32(pk.u32(pk.u32(model + 4 * pk.u32(obj + 0xA4)) + 0xC) + 0xC)
        coll = pk.u32(mesh + 0x2C) + (pk.u32(q + 0x14) << 5) if mesh else 0
        if coll and pk.u32(coll):
            ntri, tarr, verts = pk.u32(coll), pk.u32(coll + 0x10), pk.u32(coll + 0x14)
            for _ in range(min(ntri, 8)):
                t = tarr + 8 * rnd.randrange(max(1, min(ntri, 4096)))
                vs = [verts + 16 * pk.u16(t + 2 * k) for k in range(3)]
                tris.append([sum(pk.f32(v + 4 * k) for v in vs) / 3.0 for k in range(3)])
    rot = [pk.f32(obj + 0x70 + 4 * k) for k in range(9)]
    sx = pk.f32(obj + 0x98)
    for i in range(min(n, 64)):
        a, b = p0a + 0x18 * i, p1a + 0x18 * i
        r = rnd.random()
        if tris and r < 0.6:
            lc = rnd.choice(tris)
            w = [c[j] + sx * sum(rot[3 * j + k] * lc[k] for k in range(3)) for j in range(3)]
            d = [rnd.uniform(-1, 1) for _ in range(3)]
            ln = rnd.choice([0.5, 5.0, 50.0])
            far = rnd.choice([1.0, 1.0, 0.5, -0.3])
            for k in range(3):
                m.f64(a + 8 * k, w[k] + d[k] * ln)
                m.f64(b + 8 * k, w[k] - d[k] * ln * far)
        elif r < 0.75:
            d = [rnd.uniform(-1, 1) for _ in range(3)]
            ln = rnd.choice([1.0, 10.0, 100.0, 1000.0])
            off = [rnd.uniform(-1, 1) * rnd.choice([0.0, 0.1, 1.0, 20.0]) for _ in range(3)]
            for k in range(3):
                m.f64(a + 8 * k, c[k] + off[k] + d[k] * ln)
                m.f64(b + 8 * k, c[k] + off[k] - d[k] * ln * rnd.choice([1.0, 0.3, -0.2]))
        elif r < 0.8:
            for k in range(3):
                m.f64(b + 8 * k, pk.f64(a + 8 * k))
        else:
            for k in range(3):
                m.f64(a + 8 * k, pk.f64(a + 8 * k) + rnd.uniform(-5, 5))
                m.f64(b + 8 * k, pk.f64(b + 8 * k) + rnd.uniform(-5, 5))
        rr = res + 0x70 * i
        if rnd.random() < 0.4:
            m.u32(rr + 0x58, rnd.choice([0, 1]))
            m.u32(rr + 0x4C, rnd.choice([0, 0, 7]))
            m.f64(rr + 0x60, rnd.choice([0.0, 1.0, 50.0, 1e9]))
            t = rnd.uniform(0.0, 1.0)
            for k in range(3):
                m.f64(rr + 8 + 8 * k, pk.f64(a + 8 * k) * (1 - t) + pk.f64(b + 8 * k) * t)
    sc = rnd.choice(['keep', 'one', 'rand', 'uni'])
    u = rnd.uniform(0.5, 2.0)
    for k in range(3):
        v = {'keep': pk.f32(obj + 0x98 + 4 * k), 'one': 1.0, 'rand': rnd.uniform(0.5, 2.0), 'uni': u}[sc]
        m.f32(obj + 0x98 + 4 * k, v)
    small = rnd.choice([0.0, 0.0, 0.01, 0.2])
    for i in range(9):
        m.f32(obj + 0x70 + 4 * i, jitter(rnd, pk.f32(obj + 0x70 + 4 * i), small))
    if rnd.random() < 0.3:
        m.u32(q + 0x18, rnd.choice([0, 1]))
    if rnd.random() < 0.2:
        m.raw(q + 0xC, bytes([0xFF]))
    if rnd.random() < 0.5:
        S = (((st.esp - 4) & ~7) - 0x2F4) - 12
        for off in range(0x10, 0x2F4, 4):
            m.u32(S + off, rnd.getrandbits(32))
    return st


def synth_poly2d(seed, bases):
    """0x50D4C0: a captured polygon with its vertices (positions, depth in front,
    on and behind the eye, colours, texture coordinates), the vertex count, the
    colour flags, the depth range and depth switch, the texture's scale, the
    rounding mode and the stale frame randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x0050D4C0)
    m, pk = st.mem, Peek(st.mem)
    p = pk.u32(st.esp + 4)
    fl = pk.u32(p + 4)
    if rnd.random() < 0.6:
        fl = (fl & ~0x10880024) | rnd.choice([0, 4, 0x20, 0x24, 0x10000000, 0x800000, 0x80000 | 0x20, 0x4])
        m.u32(p + 4, fl)
    n = pk.u32(p + 12)
    src = pk.u32(p + 16)
    if rnd.random() < 0.3:
        n = rnd.choice([0, 1, n, max(1, n - 1)])
        m.u32(p + 12, n)
    for i in range(min(n, 64)):
        v = src + 0x24 * i
        for k in range(9):
            a = v + 4 * k
            if k == 2:
                m.f32(a, rnd.choice([pk.f32(a), 0.0, -0.0, -1.0, rnd.uniform(0.0, 2.0), rnd.uniform(0.0, 5000.0),
                                     special(rnd, -10.0, 10.0)]))
            elif k >= 5:
                m.f32(a, rnd.choice([pk.f32(a), 0.0, 1.0, 0.5, rnd.uniform(0.0, 1.0), rnd.uniform(-0.5, 1.5),
                                     1.0 / 255 * rnd.randrange(256) + rnd.choice([0, 0.5 / 255]), special(rnd, -1, 2)]))
            else:
                m.f32(a, jitter(rnd, pk.f32(a), rnd.choice([0.0, 0.01, 1.0])))
    if rnd.random() < 0.5:
        m.u32(0x6E5BB0, rnd.choice([0, 1]))
    for a in (0xBCADA0, 0xBCADC8, 0xBCABEC, 0xBCAB6C, 0xBCADB4, 0xBCAD8C):
        if rnd.random() < 0.3:
            m.f32(a, rnd.choice([jitter(rnd, pk.f32(a), 0.5), rnd.uniform(0.0, 10.0), rnd.uniform(1.0, 3000.0),
                                 0.0, 1.0]))
    tex = pk.u32(0xA84A50)
    if tex and rnd.random() < 0.3:
        for k in (0x10, 0x14):
            m.f32(tex + k, rnd.choice([pk.f32(tex + k), 256.0, 64.0, rnd.uniform(1.0, 512.0)]))
    if rnd.random() < 0.3:
        st.fcw = (st.fcw & ~0xC00) | (rnd.randrange(4) << 10)
    if rnd.random() < 0.5:
        for off in range(-0x40, 0, 4):
            m.u32(st.esp + off, rnd.getrandbits(32))
    return st


def synth_bounds(seed, bases):
    """0x4DD7F0: a captured face group with the camera moved and turned (often
    behind the group's plane, sometimes onto it), the object's rotation and
    position, the group's vertices (some behind the near plane), the material
    switch, the given rectangle, the projection, clip rectangle, rounding mode
    and the stale frame randomised."""
    rnd = random.Random(seed)
    st = base_state(rnd, bases, 0x004DD7F0)
    m, pk = st.mem, Peek(st.mem)
    E = st.esp
    obj, a1, a2 = pk.u32(E + 4), pk.u32(E + 8), pk.u32(E + 12)
    model = pk.u32(pk.u32(obj + 0x6C) + 4 * a1)
    mesh = pk.u32(pk.u32(model + 0xC) + 0xC)
    g = pk.u32(mesh + 0x98) + 20 * a2
    small = rnd.choice([0.0, 0.001, 0.05, 0.5])
    move = rnd.choice([0.0, 1.0, 20.0, 500.0])
    for k in range(3):
        m.f64(0xBCAB08 + 8 * k, pk.f64(0xBCAB08 + 8 * k) + rnd.uniform(-move, move))
        if rnd.random() < 0.1:
            m.f64(0xBCAB08 + 8 * k, pk.f64(obj + 0x20 + 8 * k))
    for i in range(9):
        m.f32(0xBCAAE0 + 4 * i, jitter(rnd, pk.f32(0xBCAAE0 + 4 * i), small))
        m.f32(obj + 0x70 + 4 * i, jitter(rnd, pk.f32(obj + 0x70 + 4 * i), small))
    verts = pk.u32(mesh + 0x94)
    first, nv = pk.u32(g), pk.u32(g + 4)
    amount = rnd.choice([0.0, 0.01, 0.3])
    for i in range(first, first + min(nv, 64)):
        for k in range(3):
            a = verts + 12 * i + 4 * k
            m.f32(a, jitter(rnd, pk.f32(a), amount))
    if rnd.random() < 0.1:
        mat = pk.u32(g + 0x10) + pk.u32(0xA5E354) + 0x14C
        m.raw(mat, bytes([rnd.choice([0, 1, 0x80])]))
    rmin, rmax = pk.u32(E + 16), pk.u32(E + 20)
    if rnd.random() < 0.5:
        x0, y0 = rnd.uniform(-100, 900), rnd.uniform(-100, 700)
        for a, v in ((rmin, x0), (rmin + 4, y0), (rmax, x0 + rnd.uniform(0, 400)), (rmax + 4, y0 + rnd.uniform(0, 300))):
            m.f32(a, v)
    for a in (0xBCAB28, 0xBCAB2C, 0xBCAB30, 0xBCAB34):
        m.f32(a, jitter(rnd, pk.f32(a), small))
    inset = rnd.choice([0.0, 0.0, 40.0, 300.0])
    for a, sgn in ((0xBCABD0, 1), (0xBCABD4, 1), (0xBCABD8, -1), (0xBCABDC, -1)):
        m.f32(a, pk.f32(a) + sgn * inset)
    if rnd.random() < 0.3:
        st.fcw = (st.fcw & ~0xC00) | (rnd.randrange(4) << 10)
    if rnd.random() < 0.5:
        for off in range(-0xB80, 0, 4):
            m.u32(E + off, rnd.getrandbits(32))
    return st


SYNTH = {0x004DD7F0: synth_bounds, 0x0050D4C0: synth_poly2d, 0x0049B700: synth_skin, 0x004A1570: synth_mesh, 0x004D14E0: synth_tree, 0x00496280: synth_adpcm,
         0x0049CC80: synth_deform, 0x0049FEB0: synth_sphere, 0x0049BE10: synth_bend,
         0x00510010: synth_patch, 0x004C1840: synth_raycast}


# ---- running -----------------------------------------------------------------

def build(exe, gen_dir, out):
    import multiprocessing
    multiprocessing.set_start_method('fork')   # workers inherit the settings below
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gen import NATIVE
    armbench.STUB = STUB
    # Generated functions are not inlined into each other, so that every guest call
    # of either version shows up as the entry of its function.
    armbench.CFLAGS = armbench.CFLAGS + ['-g', '-fwrapv', '-fno-strict-aliasing', '-fsigned-char',
                                         '-fno-inline-functions', '-fno-inline-small-functions',
                                         '-fno-inline-functions-called-once', '-fno-ipa-cp', '-fno-ipa-sra',
                                         '-fno-ipa-icf']
    lst = out + '.list'
    with open(lst, 'w') as f:
        f.write('\n'.join('%08X' % a for a in sorted(NATIVE)) + '\n')
    armbench.build(exe, gen_dir, out, lst)


def hot_lines(path, addrs, blocks, top=40):
    """Executed ARM instructions per source line (host code, e.g. a native function)."""
    import subprocess
    cnt = {}
    for (a, sz), n in blocks.items():
        i0 = bisect.bisect_left(addrs, a)
        i1 = bisect.bisect_left(addrs, a + sz)
        for x in addrs[i0:i1]:
            cnt[x] = cnt.get(x, 0) + n
    xs = sorted(cnt)
    out = subprocess.check_output([armbench.TOOL + 'addr2line', '-e', path] + ['%x' % x for x in xs],
                                  text=True).split('\n')
    per = {}
    for x, line in zip(xs, out):
        key = os.path.basename(line.split(' ')[0])
        per[key] = per.get(key, 0) + cnt[x]
    tot = sum(per.values())
    for k in sorted(per, key=lambda k: -per[k])[:top]:
        print('  %-40s %9d %5.1f%%' % (k, per[k], 100.0 * per[k] / max(1, tot)))


def run(exe, path, fn, state, native, prof_gen=None, trace=True, hot=None):
    """(status, ARM instructions, per host function, guest memory, context, trace)."""
    ab = armbench
    from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_BLOCK
    from unicorn import arm_const as A
    segs, syms = ab.load_elf(path)
    addrs, _ = ab.classify(path)
    fsyms = terrainbench.funcs_of(path)
    starts = [s[0] for s in fsyms]
    entries = {}
    for a, _, name in fsyms:
        if name.startswith('f_') and len(name) == 10:
            entries[a & ~1] = int(name[2:], 16)
    stub_addr = syms['thunk_stub'] & ~1 if 'thunk_stub' in syms else None
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for va, blob, memsz in segs:
        lo = va & ~0xFFF
        uc.mem_map(lo, ((va + memsz + 0xFFF) & ~0xFFF) - lo)
        uc.mem_write(va, blob)
    uc.mem_map(ab.HSTACK - 0x100000, 0x100000)
    uc.mem_map(ab.CTX, 0x1000)
    uc.mem_map(ab.STOP, 0x1000)
    gsize = state.gsize
    uc.mem_map(ab.GMEM, gsize + EXTRA)
    if state.image:
        uc.mem_write(ab.GMEM + 0x400000, ab.guest_image(exe))
    for a, b in state.mem.w.items():
        uc.mem_write(ab.GMEM + a, b)
    # Thunk table and lock buffer above the guest memory.
    tab = gsize
    words = [0xFFFF] * 8192
    for t, name in state.thunks.items():
        base, _, kind = name.partition('#')
        if base in THUNKS:
            nbytes, k = THUNKS[base]
            words[(t - 0xFE000000) // 4] = nbytes | ((KIND[kind] if kind else k) << 16)
    uc.mem_write(ab.GMEM + tab, struct.pack('<8192I', *words))
    uc.mem_write(syms['g_thunk_tab'], struct.pack('<I', tab))
    uc.mem_write(syms['g_lock_buf'], struct.pack('<I', gsize + 0x10000))
    uc.mem_write(syms['g_mem'], struct.pack('<I', ab.GMEM))
    uc.mem_write(syms['g_igi_native'], struct.pack('<i', native))
    regs = list(state.regs)
    regs[4] = state.esp
    ctx = struct.pack('<8I3I4x8dIII', *regs, 0, 0, 0x00020000, *state.st, state.ftop, state.fcw, state.fsw)
    uc.mem_write(ab.CTX, ctx)
    uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(A.UC_ARM_REG_SP, ab.HSTACK - 0x100)
    uc.reg_write(A.UC_ARM_REG_R0, ab.CTX)
    uc.reg_write(A.UC_ARM_REG_R1, state.esp)
    uc.reg_write(A.UC_ARM_REG_R2, regs[1])
    uc.reg_write(A.UC_ARM_REG_R3, regs[0])
    uc.reg_write(A.UC_ARM_REG_LR, ab.STOP | 1)
    per = {}
    tot = [0]
    blocks = {}
    calls = []
    prof_func = 'f_%08X' % fn
    thunk_names = state.thunks

    def record(target, esp, eax, ecx):
        c = bytes(uc.mem_read(ab.CTX, 124))
        edx, = struct.unpack_from('<I', c, 8)
        ftop, = struct.unpack_from('<I', c, 112)
        if isinstance(target, str):
            # Host thunks see only their arguments.
            nargs = THUNKS.get(target.partition('#')[0], (0, 0))[0] // 4
            more = ()
        else:
            # Guest code may also push the other registers or the status word
            # (read through fnstsw) into its frame.
            nargs = CALL_ARGS.get(target, 4)
            more = struct.unpack_from('<4I', c, 12)[0:1] + struct.unpack_from('<3I', c, 20) + \
                (struct.unpack_from('<I', c, 120)[0] & 0x4700,)
        n = 4 * (nargs + 1)
        words = struct.unpack('<%dI' % (nargs + 1), bytes(uc.mem_read(ab.GMEM + esp, n))) \
            if esp + n <= gsize + EXTRA else ()
        calls.append((target, esp, eax, ecx, edx, ftop, words, more))

    def hook(uc_, addr, size, user):
        i0 = bisect.bisect_left(addrs, addr)
        i1 = bisect.bisect_left(addrs, addr + size)
        n = i1 - i0
        tot[0] += n
        k = bisect.bisect_right(starts, addr) - 1
        name = fsyms[k][2] if k >= 0 and addr < fsyms[k][0] + fsyms[k][1] + 4 else '?'
        per[name] = per.get(name, 0) + n
        if name == (hot or prof_func):
            blocks[(addr, size)] = blocks.get((addr, size), 0) + 1
        if not trace:
            return
        g = entries.get(addr)
        if g is not None:
            record(g, uc_.reg_read(A.UC_ARM_REG_R1), uc_.reg_read(A.UC_ARM_REG_R3), uc_.reg_read(A.UC_ARM_REG_R2))
        elif addr == stub_addr:
            c = bytes(uc_.mem_read(ab.CTX, 20))
            eax, ecx, _, _, esp = struct.unpack('<5I', c)
            t, = struct.unpack('<I', bytes(uc_.mem_read(syms['g_current_thunk'], 4)))
            record(thunk_names.get(t, '%08X' % t), esp, eax, ecx)

    uc.hook_add(UC_HOOK_BLOCK, hook)
    status = 'ok'
    try:
        uc.emu_start(syms[prof_func] | 1, ab.STOP, count=LIMIT)
    except UcError as e:
        pc = uc.reg_read(A.UC_ARM_REG_PC)
        status = 'fault %s pc=%08X' % (e, pc)
        if stub_addr is not None and stub_addr <= pc < stub_addr + 0x80:
            t, = struct.unpack('<I', bytes(uc.mem_read(syms['g_current_thunk'], 4)))
            status += ' in thunk %s' % thunk_names.get(t, '%08X' % t)
    if status == 'ok' and uc.reg_read(A.UC_ARM_REG_PC) & ~1 != ab.STOP:
        status = 'limit'
    mem = bytes(uc.mem_read(ab.GMEM, gsize + EXTRA))
    out = bytearray(uc.mem_read(ab.CTX, len(ctx)))
    struct.pack_into('<I', out, 0, uc.reg_read(A.UC_ARM_REG_R0))
    struct.pack_into('<I', out, 16, uc.reg_read(A.UC_ARM_REG_R1))
    if hot:
        hot_lines(path, addrs, blocks)
    elif prof_gen:
        terrainbench.profile(path, prof_gen, addrs, blocks)
    return status, tot[0], per, mem, bytes(out), calls


def compare(ra, rb, state):
    """Differences between two runs (empty: identical)."""
    a, b = ra[3], rb[3]
    out = []
    if a != b:
        for base, end in ((0, len(a)),):
            for pg in range(base, end, 4096):
                if a[pg:pg + 4096] == b[pg:pg + 4096]:
                    continue
                for i in range(pg, min(pg + 4096, end), 4):
                    if a[i:i + 4] != b[i:i + 4]:
                        out.append('mem %08X: %s / %s' % (i, a[i:i + 4].hex(), b[i:i + 4].hex()))
    ca, cb = ra[4], rb[4]
    for off, name in CTX_REGS:
        if ca[off:off + 4] != cb[off:off + 4]:
            out.append('%s: %08X / %08X' % (name, struct.unpack_from('<I', ca, off)[0], struct.unpack_from('<I', cb, off)[0]))
    fa = struct.unpack_from('<III', ca, 112)
    fb = struct.unpack_from('<III', cb, 112)
    if fa[0] != fb[0]:
        out.append('x87 top: %d / %d' % (fa[0], fb[0]))
    if (fa[2] ^ fb[2]) & 0x4700:
        out.append('x87 condition codes: %04X / %04X' % (fa[2] & 0x4700, fb[2] & 0x4700))
    for k in range(state.ftop, 8 if state.ftop else 0):
        if ca[48 + 8 * k:56 + 8 * k] != cb[48 + 8 * k:56 + 8 * k]:
            out.append('st phys %d: %s / %s' % (k, ca[48 + 8 * k:56 + 8 * k].hex(), cb[48 + 8 * k:56 + 8 * k].hex()))
    ta, tb = ra[5], rb[5]
    for i, (x, y) in enumerate(zip(ta, tb)):
        if x != y:
            out.append('call %d: %s / %s' % (i, fmt_call(x), fmt_call(y)))
            break
    if len(ta) != len(tb):
        out.append('calls: %d / %d' % (len(ta), len(tb)))
    return out


def fmt_call(c):
    t, esp, eax, ecx, edx, ftop, words, more = c
    return '%s esp %08X eax %08X ecx %08X edx %08X top %d [%s]%s' % (
        t if isinstance(t, str) else 'f_%08X' % t, esp, eax, ecx, edx, ftop, ' '.join('%08X' % w for w in words),
        ' ebx %08X ebp %08X esi %08X edi %08X sw %04X' % more if more else '')


def cases(args, fn):
    import glob
    paths = [q for p in args.capture for q in (sorted(glob.glob(p)) or [p])]
    for p in paths:
        cfn, state = load_capture(p)
        if cfn != fn:
            sys.exit('%s is a capture of %08X, not %08X' % (p, cfn, fn))
        yield 'capture %s' % os.path.basename(p), state
    if args.synth or not args.capture:
        gen = SYNTH.get(fn)
        if not gen:
            sys.exit('no synthetic states for %08X' % fn)
        bases = [q for p in args.base for q in (sorted(glob.glob(p)) or [p])]
        for seed in range(args.seed, args.seed + (args.synth or 1)):
            yield 'seed %4d' % seed, gen(seed, bases)


def diff_mode(args, fn):
    fails = 0
    tot = [0, 0]
    ncases = 0
    for label, state in cases(args, fn):
        res = [run(args.exe, args.elf, fn, state, k) for k in (0, 1)]
        d = compare(res[0], res[1], state)
        ok = not d and res[0][0] == res[1][0] == 'ok'
        ncases += 1
        fails += not ok
        tot[0] += res[0][1]
        tot[1] += res[1][1]
        print('%s: %s  calls %4d  ARM insns %10d -> %9d (%.2fx)%s' % (
            label, 'same' if ok else 'DIFFERENT', len(res[0][5]), res[0][1], res[1][1],
            res[0][1] / max(1, res[1][1]), '' if res[0][0] == res[1][0] == 'ok' else
            ' status %s / %s' % (res[0][0], res[1][0])))
        for line in d[:12]:
            print('    ' + line)
        if len(d) > 12:
            print('    ... %d differences' % len(d))
    print('%d/%d states identical; ARM instructions %d -> %d (%.2fx)' % (
        ncases - fails, ncases, tot[0], tot[1], tot[0] / max(1, tot[1])))
    return fails == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('mode', choices=['build', 'run', 'diff'])
    ap.add_argument('exe')
    ap.add_argument('elf', help='build: GEN_DIR OUT.elf; run/diff: the ELF')
    ap.add_argument('rest', nargs='*')
    ap.add_argument('--synth', type=int, default=0, help='number of synthetic states')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--capture', action='append', default=[], help='captured state (file or glob pattern)')
    ap.add_argument('--base', action='append', default=[],
                    help='captured states the synthetic ones of some functions start from')
    ap.add_argument('--native', type=int, choices=[0, 1], default=1)
    ap.add_argument('--profile', metavar='GEN_DIR')
    ap.add_argument('--trace', type=int, default=0, help='run: print the first N guest calls')
    ap.add_argument('--hot', metavar='FUNC', help='run: executed instructions per source line of host function FUNC')
    args = ap.parse_args()
    if args.mode == 'build':
        build(args.exe, args.elf, args.rest[0])
        return
    fn = int(args.rest[0], 16)
    if args.mode == 'diff':
        sys.exit(0 if diff_mode(args, fn) else 1)
    label, state = next(cases(args, fn))
    r = run(args.exe, args.elf, fn, state, args.native, args.profile, hot=args.hot)
    print('%s: status %s, ARM instructions %d, guest calls %d' % (label, r[0], r[1], len(r[5])))
    for nm in sorted(r[2], key=lambda x: -r[2][x])[:25]:
        print('  %-32s %10d' % (nm, r[2][nm]))
    if args.trace:
        for c in r[5][:args.trace]:
            print('  call ' + fmt_call(c))


if __name__ == '__main__':
    main()
