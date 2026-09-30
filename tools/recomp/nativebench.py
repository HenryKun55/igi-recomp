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
    and host thunk calls, with their esp, eax, ecx, edx, x87 top and the top six
    stack words (return address and arguments).
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
}
KIND = {'lock': 1, 'lockfail': 2, 'locknull': 3}
# Stack arguments of the guest functions called (recorded and compared with the
# return address; default 4). The stack above them is the caller's dead frame.
CALL_ARGS = {}

GSTACK = 0x02EFF000          # entry esp of synthetic states
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


def synth_skin(seed):
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


SYNTH = {0x0049B700: synth_skin}


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


def run(exe, path, fn, state, native, prof_gen=None, trace=True):
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
            nargs = THUNKS.get(target.partition('#')[0], (0, 0))[0] // 4
        else:
            nargs = CALL_ARGS.get(target, 4)
        n = 4 * (nargs + 1)
        words = struct.unpack('<%dI' % (nargs + 1), bytes(uc.mem_read(ab.GMEM + esp, n))) \
            if esp + n <= gsize + EXTRA else ()
        calls.append((target, esp, eax, ecx, edx, ftop, words))

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
        uc.emu_start(syms[prof_func] | 1, ab.STOP, count=400_000_000)
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
    if prof_gen:
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
    t, esp, eax, ecx, edx, ftop, words = c
    return '%s esp %08X eax %08X ecx %08X edx %08X top %d [%s]' % (
        t if isinstance(t, str) else 'f_%08X' % t, esp, eax, ecx, edx, ftop, ' '.join('%08X' % w for w in words))


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
        for seed in range(args.seed, args.seed + (args.synth or 1)):
            yield 'seed %4d' % seed, gen(seed)


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
    ap.add_argument('--native', type=int, choices=[0, 1], default=1)
    ap.add_argument('--profile', metavar='GEN_DIR')
    ap.add_argument('--trace', type=int, default=0, help='run: print the first N guest calls')
    args = ap.parse_args()
    if args.mode == 'build':
        build(args.exe, args.elf, args.rest[0])
        return
    fn = int(args.rest[0], 16)
    if args.mode == 'diff':
        sys.exit(0 if diff_mode(args, fn) else 1)
    label, state = next(cases(args, fn))
    r = run(args.exe, args.elf, fn, state, args.native, args.profile)
    print('%s: status %s, ARM instructions %d, guest calls %d' % (label, r[0], r[1], len(r[5])))
    for nm in sorted(r[2], key=lambda x: -r[2][x])[:25]:
        print('  %-32s %10d' % (nm, r[2][nm]))
    if args.trace:
        for c in r[5][:args.trace]:
            print('  call ' + fmt_call(c))


if __name__ == '__main__':
    main()
