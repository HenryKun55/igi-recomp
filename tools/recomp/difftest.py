"""Differential tester for the x86 -> C code generator.

Every (non control-flow) instruction form used by IGI.exe is taken from the
binary, recompiled on its own with all flags forced live, and executed both in
the generated C and in Unicorn (a real x86 CPU model) from identical random
states. Registers, flags, the x87 stack/status and touched memory are compared.

Usage: difftest.py IGI.exe WORK_DIR [samples-per-form] [runs-per-sample]
Needs: unicorn + capstone (pip), a host C compiler.
"""
import os
import random
import struct
import subprocess
import sys

from capstone import x86 as X
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32
from unicorn import x86_const as U

sys.path.insert(0, os.path.dirname(__file__))
from analyze import Analyzer, Function, JCC  # noqa: E402
from gen import FuncGen, Generator, ALL, FLAG_NAMES, REGS32  # noqa: E402

SCRATCH = 0x02000000
SCRATCH_SIZE = 0x01000000
STACK_TOP = 0x02F00000
CODE_END = 0x00C40000

# Double/extended precision with every rounding mode; precision control is tested separately.
FCWS = [0x027F, 0x037F, 0x067F, 0x0A7F, 0x0E7F, 0x0F7F]

SKIP = JCC | {'jmp', 'call', 'ret', 'int3', 'int', 'hlt', 'leave', 'enter', 'wait', 'lcall', 'ljmp',
              'fnsave', 'frstor', 'fnstenv', 'fldenv', 'in', 'out', 'insb', 'insd', 'popfd', 'pushfd',
              'lahf', 'sahf', 'xlatb', 'rcr', 'rcl'}
REGS = [U.UC_X86_REG_EAX, U.UC_X86_REG_ECX, U.UC_X86_REG_EDX, U.UC_X86_REG_EBX,
        U.UC_X86_REG_ESP, U.UC_X86_REG_EBP, U.UC_X86_REG_ESI, U.UC_X86_REG_EDI]
NAMES = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
FLAG_BITS = {'cf': 0, 'pf': 2, 'zf': 6, 'sf': 7, 'of': 11}


def collect_forms(an, funcs, per_form):
    forms = {}
    for fn in funcs.values():
        for ins in fn.insns.values():
            base = ins.mnem.split()[-1]
            if base in SKIP or ins.mnem.startswith('rep') and base not in ('movsd', 'movsb', 'stosd', 'stosb'):
                continue
            if any(o.type == X.X86_OP_REG and an.md.reg_name(o.reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs')
                   for o in ins.ops):
                continue
            if any(p == 0x64 for p in ins.prefix):
                continue
            key = (ins.mnem, tuple((o.type, o.size) for o in ins.ops), bytes(ins.raw.bytes)[:1])
            lst = forms.setdefault(key, [])
            if len(lst) < per_form:
                lst.append(ins)
    return [i for lst in forms.values() for i in lst]


def test_modes(g, ins):
    """Code-generation modes to test an instruction in. x87 instructions run both with
    the stack in locals (static depth, as in most functions) and in the context copy."""
    info = g.x87_info(ins)
    if info is None:
        return ['none']
    return ['dynamic'] if info[1] else ['static', 'dynamic']


def emit_test(g, ins, idx, mode):
    """C function running one instruction with every flag live, then saving flags."""
    fn = Function(ins.addr)
    fn.insns = {ins.addr: ins}
    fg = FuncGen(g, fn)
    fg.compute_liveness()
    for a in fg.live_out:
        fg.live_out[a] = ALL
    fg.order = [ins.addr]
    code = fg.emit_insn(ins, 0)
    lines, depth, _ = fg.lower(ins.addr, code, mode, 'L', 0, set(REGS32))
    body = '\n    '.join(lines)
    exit_code = fg.int_spill(set(REGS32)) + fg.fpu_flush(mode, depth)
    flags = ' | '.join('((uint32_t)%s << %d)' % (n, FLAG_BITS[n]) for _, n in FLAG_NAMES)
    labels = ''.join('L_%08X:; ' % t for t in fg.labels_used)
    fpu = ''
    if mode == 'static':
        fpu = ('    const uint32_t fp_top0 = c->ftop; uint32_t fp_sw = c->fsw; (void)fp_top0; (void)fp_sw;\n'
               '    x87_t %s; %s\n') % (', '.join('fr%d = 0.0' % k for k in range(1, 9)),
                                        ' '.join('(void)fr%d;' % k for k in range(1, 9)))
    elif mode == 'dynamic':
        fpu = '    uint32_t fp_top = c->ftop; uint32_t fp_sw = c->fsw; (void)fp_top; (void)fp_sw;\n'
    return ('static uint64_t t_%d(Ctx *c)\n{\n'
            '    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3];\n'
            '    uint32_t esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];\n'
            '    uint8_t *const gm = g_mem; (void)gm;\n'
            '    int cf = (int)(c->eflags_extra & 1u), pf = (int)((c->eflags_extra >> 2) & 1u), '
            'zf = (int)((c->eflags_extra >> 6) & 1u), sf = (int)((c->eflags_extra >> 7) & 1u), '
            'of = (int)((c->eflags_extra >> 11) & 1u);\n%s'
            '    %s\n    goto done;\n    %s\ndone:\n    %s\n    c->eflags_extra = %s;\n    return 0;\n}\n\n') % (
        idx, fpu, body, labels or '', exit_code, flags)


HARNESS = r'''
#include "recomp_gen.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "runtime.h"
GuestFn thunk_lookup(uint32_t a) { (void)a; return NULL; }
void rt_fatal(Ctx *c, const char *fmt, ...) { (void)c; printf("FATAL %s\n", fmt); exit(3); }
const GuestFunc g_guest_funcs[1];
const uint32_t g_guest_func_count = 0;
const GuestFunc g_guest_func_hash[1];
const uint32_t g_guest_func_hash_mask = 0;
const GuestImport g_guest_imports[1];
const uint32_t g_guest_import_count = 0;
%TESTS%
typedef uint64_t (*TestFn)(Ctx *);
static const TestFn tests[] = { %TABLE% };
int main(void)
{
    g_mem = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    FILE *img = fopen("image.bin", "rb");
    fread(g_mem + 0x400000, 1, %IMGSIZE%, img);
    fclose(img);
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        /* idx r0..r7 flags fcw top st0..st7(hex double) nmem [addr hex16]... */
        Ctx c;
        memset(&c, 0, sizeof(c));
        char *p = line;
        int idx = (int)strtol(p, &p, 10);
        for (int i = 0; i < 8; i++) c.r[i] = (uint32_t)strtoul(p, &p, 16);
        c.eflags_extra = (uint32_t)strtoul(p, &p, 16);
        c.fcw = (uint32_t)strtoul(p, &p, 16);
        c.ftop = (uint32_t)strtoul(p, &p, 16);
        for (int i = 0; i < 8; i++) {
            uint64_t bits = strtoull(p, &p, 16);
            memcpy(&c.st[(c.ftop + (uint32_t)i) & 7u], &bits, 8);
        }
        /* String operations read past the initialised bytes: clear what earlier
         * tests left around esi/edi (Unicorn starts from zeroed memory). */
        for (int k = 6; k < 8; k++)
            if (c.r[k] >= 0x02000100u && c.r[k] < 0x02E00000u)
                memset(g_mem + c.r[k] - 0x100u, 0, 0x300);
        int nmem = (int)strtol(p, &p, 10);
        uint32_t addrs[8];
        for (int m = 0; m < nmem; m++) {
            addrs[m] = (uint32_t)strtoul(p, &p, 16);
            for (int b = 0; b < 16; b++) {
                while (*p == ' ') p++;
                char h[3] = { p[0], p[1], 0 };
                g_mem[addrs[m] + (uint32_t)b] = (uint8_t)strtoul(h, NULL, 16);
                p += 2;
            }
        }
        tests[idx](&c);
        printf("R");
        for (int i = 0; i < 8; i++) printf(" %08X", c.r[i]);
        printf(" %08X %X %04X", c.eflags_extra, c.ftop, c.fsw & 0x4700u);
        for (int i = 0; i < 8; i++) { uint64_t b; memcpy(&b, &c.st[(c.ftop + (uint32_t)i) & 7u], 8); printf(" %016llX", (unsigned long long)b); }
        for (int m = 0; m < nmem; m++) { printf(" M"); for (int b = 0; b < 16; b++) printf("%02X", g_mem[addrs[m] + (uint32_t)b]); }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
'''


def f80_to_double(t):
    """unicorn returns (mantissa, exponent) for FP registers."""
    mant, exp = t
    sign = -1.0 if exp & 0x8000 else 1.0
    e = exp & 0x7FFF
    if e == 0 and mant == 0:
        return 0.0 * sign
    if e == 0x7FFF:
        return float('nan') if (mant << 1) & 0xFFFFFFFFFFFFFFFF else sign * float('inf')
    import math
    try:
        return sign * math.ldexp(mant, e - 16383 - 63)
    except OverflowError:
        return sign * float('inf')


def mem_addrs(ins, regs):
    out = []
    for o in ins.ops:
        if o.type == X.X86_OP_MEM:
            m = o.mem
            a = m.disp
            if m.base:
                a += regs[NAMES.index(ins.raw.reg_name(m.base)[-3:] if len(ins.raw.reg_name(m.base)) == 3 else 'eax')]
            if m.index:
                a += regs[NAMES.index(ins.raw.reg_name(m.index))] * m.scale
            out.append(a & 0xFFFFFFFF)
    return out


def random_state(ins, rnd):
    regs = [rnd.choice([rnd.getrandbits(32), rnd.randrange(0, 256), 0xFFFFFFFF - rnd.randrange(0, 16),
                        0x80000000, 0x7FFFFFFF, 0]) for _ in range(8)]
    # Registers used as addresses point into scratch memory.
    for o in ins.ops:
        if o.type == X.X86_OP_MEM:
            for r in (o.mem.base, o.mem.index):
                if r:
                    n = ins.raw.reg_name(r)
                    if n in NAMES:
                        i = NAMES.index(n)
                        regs[i] = SCRATCH + 0x100000 + rnd.randrange(0, 0x1000) * 4 if r == o.mem.base else rnd.randrange(0, 64)
    regs[4] = STACK_TOP - rnd.randrange(0, 64) * 4
    if ins.mnem.startswith('rep') or ins.mnem in ('movsb', 'movsd', 'stosb', 'stosd', 'cmpsb', 'cmpsd', 'scasb'):
        regs[1] = rnd.choice([rnd.randrange(0, 8), rnd.randrange(8, 40)])
        regs[6] = SCRATCH + 0x200000 + rnd.randrange(0, 256)
        regs[7] = SCRATCH + 0x300000 + rnd.randrange(0, 256)
        if rnd.random() < 0.3:
            regs[7] = regs[6] + rnd.randrange(-8, 9)   # overlapping source and destination
    flags = sum(rnd.getrandbits(1) << b for b in FLAG_BITS.values())
    vals = [rnd.choice([rnd.uniform(-1e6, 1e6), rnd.uniform(-2, 2), float(rnd.randrange(-100, 100)), 0.5, -0.5, 1e-7,
                        123456789.75]) for _ in range(8)]
    fcw = rnd.choice(FCWS)
    return regs, flags, fcw, vals


def run_unicorn(image, ins, regs, flags, fcw, vals, mem):
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(0x400000, CODE_END - 0x400000)
    uc.mem_write(0x400000, image)
    uc.mem_map(SCRATCH, SCRATCH_SIZE)
    for a, data in mem:
        uc.mem_write(a, data)
    # Prologue: load the FPU stack (deepest first) and the control word, then the instruction.
    pro = bytearray()
    vaddr = SCRATCH + 0x10
    for i, v in enumerate(reversed(vals)):
        uc.mem_write(vaddr + i * 8, struct.pack('<d', v))
    uc.mem_write(SCRATCH + 8, struct.pack('<H', fcw))
    pro += b'\xdb\xe3'                      # fninit
    pro += b'\xd9\x2d' + struct.pack('<I', SCRATCH + 8)   # fldcw [scratch+8]
    for i in range(8):
        pro += b'\xdd\x05' + struct.pack('<I', vaddr + i * 8)   # fld qword [..]
    stub = SCRATCH + 0x800
    uc.mem_write(stub, bytes(pro))
    for i, r in enumerate(REGS):
        uc.reg_write(r, regs[i])
    uc.reg_write(U.UC_X86_REG_EFLAGS, flags | 0x2)
    uc.emu_start(stub, stub + len(pro))
    for i, r in enumerate(REGS):
        uc.reg_write(r, regs[i])
    uc.reg_write(U.UC_X86_REG_EFLAGS, flags | 0x2)
    if ins.mnem.startswith('rep'):
        uc.emu_start(ins.addr, ins.addr + ins.size)
    else:
        uc.emu_start(ins.addr, ins.addr + ins.size, count=1)
    out_regs = [uc.reg_read(r) for r in REGS]
    ef = uc.reg_read(U.UC_X86_REG_EFLAGS)
    fsw = uc.reg_read(U.UC_X86_REG_FPSW)
    top = (fsw >> 11) & 7
    # Unicorn exposes the physical registers; ST(i) = FP[(top + i) & 7].
    phys = [f80_to_double(uc.reg_read(U.UC_X86_REG_FP0 + i)) for i in range(8)]
    st = [phys[(top + i) & 7] for i in range(8)]
    mems = [bytes(uc.mem_read(a, 16)) for a, _ in mem]
    return out_regs, ef, fsw, top, st, mems


def main():
    exe, work = sys.argv[1], sys.argv[2]
    per_form = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    runs = int(sys.argv[4]) if len(sys.argv) > 4 else 20
    os.makedirs(work, exist_ok=True)
    g = Generator(exe)
    an = g.an
    tests = collect_forms(an, g.funcs, per_form)
    print('instruction samples:', len(tests))
    parts, keep = [], []
    for ins in tests:
        try:
            for mode in test_modes(g, ins):
                parts.append(emit_test(g, ins, len(keep), mode))
                keep.append((ins, mode))
        except Exception as e:  # unsupported form in isolation
            print('skip', ins.text, e)
    image = bytearray(CODE_END - 0x400000)
    for sec in an.pe.sections:
        raw = an.pe.data[sec.raw_off:sec.raw_off + sec.raw_size]
        off = sec.va - 0x400000
        image[off:off + len(raw)] = raw[:max(0, len(image) - off)]
    image = bytes(image)
    with open(os.path.join(work, 'image.bin'), 'wb') as f:
        f.write(image)
    src = HARNESS.replace('%TESTS%', ''.join(parts)).replace(
        '%TABLE%', ', '.join('t_%d' % i for i in range(len(keep)))).replace('%IMGSIZE%', str(len(image)))
    with open(os.path.join(work, 'harness.c'), 'w') as f:
        f.write(src)
    gen_dir = os.path.join(work, 'gen')
    g.write_header(gen_dir) if os.path.isdir(gen_dir) else (os.makedirs(gen_dir), g.write_header(gen_dir))
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    subprocess.check_call(['cc', '-O1', '-w', '-o', os.path.join(work, 'harness'), os.path.join(work, 'harness.c'),
                           '-I' + gen_dir, '-I' + os.path.join(root, 'runtime/include'),
                           os.path.join(root, 'runtime/src/cpu.c'), '-lm'])
    rnd = random.Random(1234)
    lines, expect = [], []
    for idx, (ins, mode) in enumerate(keep):
        for _ in range(runs):
            regs, flags, fcw, vals = random_state(ins, rnd)
            addrs = mem_addrs(ins, regs) + [regs[4]]
            if ins.mnem.startswith('rep') or ins.mnem.split()[-1] in ('movsb', 'movsd', 'stosb', 'stosd'):
                addrs += [regs[6], regs[7], regs[7] + 16, regs[7] + 32]
            mem = []
            for a in addrs[:8]:
                if not (SCRATCH <= a < SCRATCH + SCRATCH_SIZE - 16 or 0x536000 <= a < CODE_END - 16):
                    continue
                mem.append((a, bytes(rnd.getrandbits(8) for _ in range(16))))
            try:
                res = run_unicorn(image, ins, regs, flags, fcw, vals, mem)
            except UcError:
                continue
            line = '%d %s %X %X 0 %s %d %s' % (
                idx, ' '.join('%X' % r for r in regs), flags, fcw,
                ' '.join(struct.pack('<d', v).hex()[::-1] and '%016X' % struct.unpack('<Q', struct.pack('<d', v))[0]
                         for v in vals),
                len(mem), ' '.join('%X %s' % (a, d.hex()) for a, d in mem))
            lines.append(line)
            expect.append((ins, mode, regs, flags, fcw, vals, mem, res))
    p = subprocess.run([os.path.join(work, 'harness')], input='\n'.join(lines) + '\n', capture_output=True,
                       text=True, cwd=work)
    outs = p.stdout.strip().split('\n')
    fails = {}
    for (ins, mode, regs, flags, fcw, vals, mem, res), out in zip(expect, outs):
        tname = ins.text if mode == 'none' else '%s [%s]' % (ins.text, mode)
        u_regs, u_ef, u_fsw, u_top, u_st, u_mem = res
        if not out.startswith('R'):
            fails.setdefault(tname, []).append('harness: ' + out)
            continue
        f = out.split()
        c_regs = [int(x, 16) for x in f[1:9]]
        c_flags = int(f[9], 16)
        c_top = int(f[10], 16)
        c_fsw = int(f[11], 16)
        c_st = [struct.unpack('<d', struct.pack('<Q', int(x, 16)))[0] for x in f[12:20]]
        c_mem = [bytes.fromhex(x[1:]) for x in f[20:]]
        problems = []
        for i in range(8):
            if c_regs[i] != u_regs[i]:
                problems.append('%s %08X vs %08X' % (NAMES[i], c_regs[i], u_regs[i]))
        base = ins.mnem.split()[-1]
        undefined = {'shl': 0, 'shr': 0, 'sar': 0, 'rol': 0, 'ror': 0, 'shld': 0, 'shrd': 0}   # OF: count 1 only
        for name, bit in FLAG_BITS.items():
            if base in ('mul', 'imul') and name in ('zf', 'sf', 'pf'):
                continue
            if base in ('div', 'idiv', 'bsf', 'bsr') and name != 'zf':
                continue
            if base in undefined and name == 'of':
                continue
            if ((c_flags >> bit) & 1) != ((u_ef >> bit) & 1):
                problems.append('%s %d vs %d' % (name, (c_flags >> bit) & 1, (u_ef >> bit) & 1))
        if c_top != u_top:
            problems.append('ftop %d vs %d' % (c_top, u_top))
        cc_mask = 0x4500 if base.startswith('f') and base not in ('fprem', 'fprem1', 'fsqrt', 'fyl2x') else 0
        if (c_fsw & cc_mask) != (u_fsw & cc_mask):
            problems.append('fsw %04X vs %04X' % (c_fsw & cc_mask, u_fsw & cc_mask))
        for i in range(8):
            a, b = c_st[i], u_st[i]
            if a != a and b != b:
                continue
            if abs(a - b) > 1e-9 * max(1.0, abs(b)):
                problems.append('st%d %r vs %r' % (i, a, b))
        for m, (a, _) in enumerate(mem):
            if m < len(c_mem) and c_mem[m] != u_mem[m]:
                problems.append('mem %08X %s vs %s' % (a, c_mem[m].hex(), u_mem[m].hex()))
        if problems:
            fails.setdefault(tname, []).append('; '.join(problems) + '  | in regs ' + ' '.join(
                '%08X' % r for r in regs) + ' fl %X fcw %X' % (flags, fcw))
    print('tests run: %d, failing instructions: %d' % (len(outs), len(fails)))
    for text, probs in sorted(fails.items()):
        print('FAIL %-45s (%d)  e.g. %s' % (text, len(probs), probs[0]))


if __name__ == '__main__':
    main()
