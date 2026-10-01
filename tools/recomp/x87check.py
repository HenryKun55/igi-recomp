"""Equivalence check of the x87 helpers in runtime/include/recomp.h.

The inline fast paths of the x87 helpers (fist, f2xm1) must give bit-identical
results to the reference: the previous versions of the fast paths (kept below,
REF) with the same out-of-line code (copied from runtime/src/cpu.c), and the C
library's exp2. Both are compiled from one C file for

  host  this machine's compiler (the macOS build), and
  arm   the vitasdk compiler with the Vita build's flags, linked with newlib's
        libm (the library the Vita build uses), run in Unicorn ARM;

and compared on random and special operands in every rounding mode: random bit
patterns, doubles of every magnitude, integers and halves plus or minus a few
ulps, the conversion bounds (2^15, 2^31, 2^52, 2^63) and their neighbours,
zeros, denormals, infinities, NaNs, float values, and exp2 operands in [-1, 1],
[-600, 600] and around 2^-54. The host also checks every float value (2^32
operands, the game's fist operands are mostly floats) with --floats.

  x87check.py [--samples N] [--seed S] [--floats]

Needs unicorn (pip) and VITASDK (default ~/vitasdk), and cc.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

SRC = r'''
#include "recomp.h"

/* ---- out-of-line code (runtime/src/cpu.c) ---- */
@SLOW@

/* ---- reference: the previous inline fast paths ---- */
#define REF_SMALL(v) ((v) > -2147483647.0 && (v) < 2147483647.0)
static uint64_t ref_round_bits(double v)
{
    double t = v + RT_ROUND_MAGIC;
    uint64_t b;
    memcpy(&b, &t, 8);
    return b;
}
static int64_t ref_fist64(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = ref_round_bits(v);
        if ((uint32_t)(b >> 52) == 0x433u)
            return (int64_t)(b - RT_ROUND_MAGIC_BITS);
    } else if (RT_FIST_CHOP(c) && REF_SMALL(v)) {
        return (int32_t)v;
    }
    return rt_fist64_slow(c, v);
}
static int32_t ref_fist32(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = ref_round_bits(v);
        uint32_t lo = (uint32_t)b, hi = (uint32_t)(b >> 32) - 0x43380000u;
        if (hi == (uint32_t)((int32_t)lo >> 31))
            return (int32_t)lo;
    } else if (RT_FIST_CHOP(c) && REF_SMALL(v)) {
        return (int32_t)v;
    }
    return rt_fist32_slow(c, v);
}
static int16_t ref_fist16(Ctx *c, double v)
{
    if (RT_FIST_NEAREST(c)) {
        uint64_t b = ref_round_bits(v);
        uint32_t lo = (uint32_t)b, hi = (uint32_t)(b >> 32) - 0x43380000u;
        if (hi == (uint32_t)((int32_t)lo >> 31) && lo + 0x8000u < 0x10000u)
            return (int16_t)lo;
    } else if (RT_FIST_CHOP(c) && v > -32769.0 && v < 32768.0) {
        return (int16_t)(int32_t)v;
    }
    return rt_fist16_slow(c, v);
}

/* ---- operands ---- */
static uint64_t rs;
static uint64_t rnd(void)
{
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}
static double d_of(uint64_t b) { double v; memcpy(&v, &b, 8); return v; }
static uint64_t b_of(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }
static double ulps(double v, int k)
{
    uint64_t b = b_of(v);
    if (v == 0.0 || (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
        return v;
    return d_of(b + (uint64_t)(int64_t)k);
}
static double uni(double lo, double hi)
{
    return lo + (hi - lo) * (double)(rnd() >> 11) * (1.0 / 9007199254740992.0);
}
static const double bounds[] = {
    0.5, 1.0, 1.5, 2.5, 32767.0, 32767.5, 32768.0, 32768.5, 32769.0, 2147483646.5, 2147483647.0,
    2147483647.5, 2147483648.0, 2147483648.5, 2147483649.0, 4294967296.0, 4503599627370496.0,
    2251799813685248.0, 6755399441055744.0, 9007199254740992.0, 9223372036854775808.0, 1e300,
    0.0078125, 0.99999999999999989, 5.551115123125783e-17, 2.7755575615628914e-17, 512.0, 1022.0, 1024.0,
};
static double operand(void)
{
    uint64_t r = rnd();
    double s = (r & 1) ? -1.0 : 1.0;
    switch ((r >> 1) % 12u) {
    case 0: return d_of(rnd());
    case 1: return s * uni(0.0, 1.0) * d_of((uint64_t)(1023 + (int)((r >> 8) % 70u)) << 52);
    case 2: return s * (double)(rnd() >> (rnd() % 64u)) + (double)((int)((r >> 8) % 5u) - 2) * 0.25;
    case 3: return ulps(s * bounds[(r >> 8) % (sizeof(bounds) / sizeof(bounds[0]))], (int)((r >> 16) % 9u) - 4);
    case 4: {
        static const uint64_t sp[] = { 0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
                                       0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF0000000000001ull,
                                       0x7FF4000000000000ull, 1, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
                                       0x7FEFFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull };
        return d_of(sp[(r >> 8) % (sizeof(sp) / sizeof(sp[0]))]);
    }
    case 5: { uint32_t f = (uint32_t)(rnd() >> 32); float v; memcpy(&v, &f, 4); return (double)v; }
    case 6: return uni(-1.0, 1.0);
    case 7: return uni(-600.0, 600.0);
    case 8: return s * d_of((uint64_t)(0x3C0 + (r >> 8) % 24u) << 52 | (rnd() >> 12));
    case 9: return s * ((double)(int64_t)((rnd() >> 33) % 70000u) + uni(-1.0, 1.0) * 1e-9);
    case 10: return s * (double)((rnd() >> 40) % 0x10000u) * 0.5;
    default: return uni(-5000.0, 5000.0) * 0.001;
    }
}

/* ---- comparison ---- */
uint32_t g_fail, g_kind, g_mode;
uint64_t g_bits, g_got, g_want;

static void fail(uint32_t kind, uint32_t mode, double v, uint64_t got, uint64_t want)
{
    if (!g_fail++) {
        g_kind = kind;
        g_mode = mode;
        g_bits = b_of(v);
        g_got = got;
        g_want = want;
    }
}

static __attribute__((noinline)) void check(Ctx *c, double v)
{
    for (uint32_t m = 0; m < 4u; m++) {
        c->fcw = 0x027Fu | m << 10;
        int64_t a = rt_fist64(c, v), b = ref_fist64(c, v);
        if (a != b) fail(64, m, v, (uint64_t)a, (uint64_t)b);
        int32_t a32 = rt_fist32(c, v), b32 = ref_fist32(c, v);
        if (a32 != b32) fail(32, m, v, (uint32_t)a32, (uint32_t)b32);
        int16_t a16 = rt_fist16(c, v), b16 = ref_fist16(c, v);
        if (a16 != b16) fail(16, m, v, (uint16_t)a16, (uint16_t)b16);
    }
    uint64_t e = b_of(rt_exp2(v)), w = b_of(exp2(v));
    if (e != w) fail(2, 0, v, e, w);
}

uint32_t x87check(uint32_t seed, uint32_t n)
{
    Ctx c = { .fcw = 0x027Fu };
    rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed << 17) ^ seed;
    for (uint32_t i = 0; i < n; i++)
        check(&c, operand());
    return g_fail;
}

/* Every float operand from lo (bits) up to hi (exclusive), round to nearest and truncate. */
uint32_t x87check_floats(uint32_t lo, uint32_t hi)
{
    Ctx c = { .fcw = 0x027Fu };
    for (uint64_t f = lo; f < hi; f++) {
        float x;
        uint32_t fb = (uint32_t)f;
        memcpy(&x, &fb, 4);
        double v = x;
        for (uint32_t m = 0; m < 4u; m += 3u) {
            c.fcw = 0x027Fu | m << 10;
            if (rt_fist64(&c, v) != ref_fist64(&c, v)) fail(64, m, v, 0, 0);
            if (rt_fist32(&c, v) != ref_fist32(&c, v)) fail(32, m, v, 0, 0);
            if (rt_fist16(&c, v) != ref_fist16(&c, v)) fail(16, m, v, 0, 0);
        }
        if (x >= -1.0f && x <= 1.0f && b_of(rt_exp2(v)) != b_of(exp2(v)))
            fail(2, 0, v, 0, 0);
    }
    return g_fail;
}

#ifdef X87CHECK_MAIN
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    uint32_t n;
    if (argc > 3) {
        n = x87check_floats((uint32_t)strtoul(argv[2], 0, 0), (uint32_t)strtoull(argv[3], 0, 0));
    } else {
        n = x87check((uint32_t)atoi(argv[1]), (uint32_t)strtoul(argv[2], 0, 0));
    }
    printf("%u %u %u %016llx %016llx %016llx\n", n, g_kind, g_mode, (unsigned long long)g_bits,
           (unsigned long long)g_got, (unsigned long long)g_want);
    return 0;
}
#else
/* Bare-metal ARM build: no C runtime. */
int *__errno(void) { static int e; return &e; }
/* newlib's memset and memcpy call these (system library on the Vita). */
void *sceClibMemset(void *d, int v, unsigned n)
{
    volatile unsigned char *p = d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
void *sceClibMemcpy(void *d, const void *s, unsigned n)
{
    volatile unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
#endif
'''


def source(work):
    cpu = open(os.path.join(ROOT, 'runtime/src/cpu.c')).read()
    a = cpu.index('#define FIST_SMALL')
    b = cpu.index('/* ---- integer')
    path = os.path.join(work, 'x87check.c')
    with open(path, 'w') as f:
        f.write(SRC.replace('@SLOW@', cpu[a:b]))
    return path


def report(tag, out):
    n, kind, mode, bits, got, want = out.split()
    if n == '0':
        return True
    print('%s: %s DIFFERENT, first: %s mode %s operand %s got %s want %s' % (tag, n, {
        '64': 'fist m64', '32': 'fist m32', '16': 'fist m16', '2': 'exp2'}[kind], mode, bits, got, want))
    return False


def host(work, args):
    src = source(work)
    exe = os.path.join(work, 'x87check_host')
    subprocess.check_call(['cc', '-O2', '-ffp-contract=off', '-DX87CHECK_MAIN', '-I' + os.path.join(ROOT, 'runtime/include'),
                           src, '-o', exe, '-lm'])
    ok = report('host', subprocess.check_output([exe, str(args.seed), str(args.samples)], text=True))
    print('host: %d operands x 4 rounding modes %s' % (args.samples, 'identical' if ok else ''))
    if args.floats:
        # Every float, in parallel slices.
        procs = []
        step = 1 << 28
        for lo in range(0, 1 << 32, step):
            procs.append(subprocess.Popen([exe, 'f', str(lo), str(lo + step)], stdout=subprocess.PIPE, text=True))
        fl = all(report('host floats', p.communicate()[0]) for p in procs)
        print('host: all 2^32 float operands (nearest, truncate; exp2 in [-1, 1]) %s' % ('identical' if fl else ''))
        ok = ok and fl
    return ok


def arm(work, args):
    import armbench
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB
    from unicorn import arm_const as A
    src = source(work)
    elf = os.path.join(work, 'x87check_arm.elf')
    libgcc = subprocess.check_output([armbench.TOOL + 'gcc'] + armbench.CFLAGS + ['-print-libgcc-file-name'],
                                     text=True).strip()
    lib = os.path.join(armbench.SDK, 'arm-vita-eabi', 'lib')
    subprocess.check_call([armbench.TOOL + 'gcc'] + armbench.CFLAGS + [
        '-Wall', '-Wextra', '-I' + os.path.join(ROOT, 'runtime/include'), '-nostdlib', '-nostartfiles', '-static',
        '-Wl,--gc-sections', '-Wl,--no-enum-size-warning', '-Wl,-e,x87check', '-Wl,-Ttext=0x100000',
        '-o', elf, src, os.path.join(lib, 'libm.a'), os.path.join(lib, 'libc.a'), libgcc])
    segs, syms = armbench.load_elf(elf)
    total = 0
    ok = True
    per = 200000
    for k in range(0, args.samples, per):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        for va, blob, memsz in segs:
            lo = va & ~0xFFF
            uc.mem_map(lo, ((va + memsz + 0xFFF) & ~0xFFF) - lo)
            uc.mem_write(va, blob)
        uc.mem_map(armbench.HSTACK - 0x100000, 0x100000)
        uc.mem_map(armbench.STOP, 0x1000)
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        uc.reg_write(A.UC_ARM_REG_SP, armbench.HSTACK - 0x100)
        n = min(per, args.samples - k)
        uc.reg_write(A.UC_ARM_REG_R0, args.seed * 7919 + k)
        uc.reg_write(A.UC_ARM_REG_R1, n)
        uc.reg_write(A.UC_ARM_REG_LR, armbench.STOP | 1)
        uc.emu_start(syms['x87check'] | 1, armbench.STOP)
        rd = lambda s, f: struct.unpack(f, bytes(uc.mem_read(syms[s], struct.calcsize(f))))[0]
        out = '%d %d %d %016x %016x %016x' % (rd('g_fail', '<I'), rd('g_kind', '<I'), rd('g_mode', '<I'),
                                             rd('g_bits', '<Q'), rd('g_got', '<Q'), rd('g_want', '<Q'))
        ok = report('arm', out) and ok
        total += n
    print('arm: %d operands x 4 rounding modes %s' % (total, 'identical' if ok else ''))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--samples', type=int, default=2000000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--floats', action='store_true', help='host: also every float operand')
    ap.add_argument('--only', choices=['host', 'arm'])
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as work:
        ok = True
        if args.only != 'arm':
            ok = host(work, args) and ok
        if args.only != 'host':
            ok = arm(work, args) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
