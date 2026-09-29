"""x86 -> C static recompiler for IGI.exe.

Every recovered function becomes `void f_XXXXXXXX(Ctx *c)`. Guest registers live
in C locals and are spilled to the context around calls; guest memory is a flat
array (`g_mem`) indexed by guest address. EFLAGS are computed only where a later
instruction reads them (backward liveness), and cmp/test + jcc/setcc pairs are
fused into direct C comparisons. x87 instructions are decoded from their raw
opcode bytes and run against the FPU stack kept in the context.

Usage: gen.py IGI.exe OUT_DIR
"""
import os
import re
import sys

from capstone import x86 as X

from analyze import Analyzer, JCC

REGS32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
SUB16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
SUB8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
SUB8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}

CF, ZF, SF, OF, PF = 1, 2, 4, 8, 16
ALL = CF | ZF | SF | OF | PF
FLAG_NAMES = [(CF, 'cf'), (ZF, 'zf'), (SF, 'sf'), (OF, 'of'), (PF, 'pf')]

MASK = {1: '0xFFu', 2: '0xFFFFu', 4: '0xFFFFFFFFu'}
SBIT = {1: 7, 2: 15, 4: 31}
STYPE = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}
RD = {1: 'RD8', 2: 'RD16', 4: 'RD32'}
WR = {1: 'WR8', 2: 'WR16', 4: 'WR32'}

COND = {
    'o': ('of', OF), 'no': ('!of', OF), 'b': ('cf', CF), 'ae': ('!cf', CF),
    'e': ('zf', ZF), 'ne': ('!zf', ZF), 'be': ('(cf | zf)', CF | ZF), 'a': ('!(cf | zf)', CF | ZF),
    's': ('sf', SF), 'ns': ('!sf', SF), 'p': ('pf', PF), 'np': ('!pf', PF),
    'l': ('sf != of', SF | OF), 'ge': ('sf == of', SF | OF),
    'le': ('(zf | (sf != of))', ZF | SF | OF), 'g': ('!(zf | (sf != of))', ZF | SF | OF),
}

STRING_OPS = {'movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
              'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'}

# Guest functions whose entry calls rt_probe (see runtime/src/igi_fixes.c).
GAME_HOOKS = [
    0x00418970,   # UI window render (cutscene window state)
    0x00497700,   # 2D render queue flush (start)
    0x00497860,   # 2D render queue: per-node state setup
]

NORETURN_IMPORTS = {'ExitProcess', 'TerminateProcess', 'RaiseException'}


class Unsupported(Exception):
    pass


def h(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


def cond_of(mnem):
    """Condition code suffix for jcc/setcc mnemonics."""
    for p in ('set', 'j'):
        if mnem.startswith(p):
            cc = mnem[len(p):]
            if cc in COND:
                return cc
    return None


def flag_effects(ins, target_is_shift_count_reg=False):
    """(use, def) flag masks. Variable-count shifts define nothing for liveness:
    when the count is zero the flags pass through unchanged."""
    m = ins.mnem
    if m.startswith('rep') and ' ' not in m:
        pass
    base = m.split()[-1]
    cc = cond_of(base)
    if cc and (base.startswith('j') or base.startswith('set')):
        return COND[cc][1], 0
    if base in ('add', 'sub', 'cmp', 'neg', 'and', 'or', 'xor', 'test', 'mul', 'imul', 'div', 'idiv',
                'bsf', 'bsr', 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw', 'scasd', 'popfd', 'shld', 'shrd'):
        if base in ('shld', 'shrd') and ins.ops[2].type != X.X86_OP_IMM:
            return 0, 0
        return 0, ALL
    if base in ('adc', 'sbb'):
        return CF, ALL
    if base in ('inc', 'dec'):
        return 0, ZF | SF | OF | PF
    if base in ('shl', 'sal', 'shr', 'sar'):
        if ins.ops[1].type == X.X86_OP_IMM:
            return (0, ALL) if ins.ops[1].imm & 31 else (0, 0)
        return 0, 0
    if base in ('rol', 'ror'):
        if ins.ops[1].type == X.X86_OP_IMM:
            return (0, CF | OF) if ins.ops[1].imm & 31 else (0, 0)
        return 0, 0
    if base in ('rcl', 'rcr'):
        return CF, 0          # conservative: CF read, flags may pass through
    if base == 'sahf':
        return 0, CF | ZF | SF | PF
    if base in ('lahf', 'pushfd'):
        return (CF | ZF | SF | PF) if base == 'lahf' else ALL, 0
    if base == 'cmc':
        return CF, CF
    if base in ('stc', 'clc'):
        return 0, CF
    if base in ('loope', 'loopne'):
        return ZF, 0
    if base == 'call':
        return 0, ALL
    return 0, 0


class FuncGen:
    def __init__(self, g, fn):
        self.g = g
        self.an = g.an
        self.fn = fn
        self.lines = []
        self.labels_used = set()
        self.tmp = 0
        self.order = sorted(fn.insns)
        self.succ = {}
        self.live_out = {}
        self.fused = set()
        self.fused_pairs = {}
        self.cur_idx = 0

    # ----------------------------------------------------------- operands
    def rname(self, reg):
        return self.an.md.reg_name(reg)

    def reg_read(self, name):
        if name in REGS32:
            return name
        if name in SUB16:
            return '(%s & 0xFFFFu)' % SUB16[name]
        if name in SUB8L:
            return '(%s & 0xFFu)' % SUB8L[name]
        if name in SUB8H:
            return '((%s >> 8) & 0xFFu)' % SUB8H[name]
        raise Unsupported('register %s' % name)

    def reg_write(self, name, expr):
        if name in REGS32:
            return '%s = %s;' % (name, expr)
        if name in SUB16:
            r = SUB16[name]
            return '%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % (r, r, expr)
        if name in SUB8L:
            r = SUB8L[name]
            return '%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFFu);' % (r, r, expr)
        if name in SUB8H:
            r = SUB8H[name]
            return '%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);' % (r, r, expr)
        raise Unsupported('register %s' % name)

    def addr(self, op):
        m = op.mem
        parts = []
        if m.segment and self.rname(m.segment) == 'fs':
            parts.append('c->fs_base')
        elif m.segment and self.rname(m.segment) not in ('ds', 'ss', 'es', 'cs'):
            raise Unsupported('segment %s' % self.rname(m.segment))
        if m.base:
            parts.append(self.reg_read(self.rname(m.base)))
        if m.index:
            idx = self.reg_read(self.rname(m.index))
            parts.append(idx if m.scale == 1 else '%s * %du' % (idx, m.scale))
        if m.disp or not parts:
            parts.append(h(m.disp))
        if len(parts) == 1:
            return parts[0]
        return '(uint32_t)(%s)' % ' + '.join(parts)

    def rd(self, op, size=None, addr_var=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(self.rname(op.reg))
        if op.type == X.X86_OP_IMM:
            return h(op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return '%s(%s)' % (RD[size], addr_var or self.addr(op))
        raise Unsupported('operand')

    def wr(self, op, expr, size=None, addr_var=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(self.rname(op.reg), expr)
        if op.type == X.X86_OP_MEM:
            return '%s(%s, %s);' % (WR[size], addr_var or self.addr(op), expr)
        raise Unsupported('write operand')

    # ------------------------------------------------------------- flags
    def compute_liveness(self):
        fn = self.fn
        an = self.an
        for a in self.order:
            ins = fn.insns[a]
            m = ins.mnem
            nxt = a + ins.size
            s = []
            base = m.split()[-1]
            if base in ('ret', 'retf', 'iretd', 'int3', 'hlt'):
                pass
            elif base == 'jmp':
                t = an.direct_target(ins)
                if a in fn.jump_tables:
                    s = [x for x in fn.jump_tables[a] if x in fn.insns]
                elif t is not None and t in fn.insns and a not in fn.tail_calls:
                    s = [t]
            elif base in JCC:
                t = an.direct_target(ins)
                s = [x for x in (t, nxt) if x in fn.insns]
            elif base == 'call' and self.is_noreturn_call(ins):
                pass
            else:
                if nxt in fn.insns:
                    s = [nxt]
            self.succ[a] = s
        live_in = {a: 0 for a in self.order}
        changed = True
        while changed:
            changed = False
            for a in reversed(self.order):
                out = 0
                for s in self.succ[a]:
                    out |= live_in[s]
                use, d = flag_effects(self.fn.insns[a])
                inn = use | (out & ~d)
                self.live_out[a] = out
                if inn != live_in[a]:
                    live_in[a] = inn
                    changed = True

    def set_flags(self, a, kind, size, r, d=None, s=None, cin=None, mask_flags=None):
        """Emit assignments for live flags. r/d/s are C expressions (already masked)."""
        live = self.live_out[a] if mask_flags is None else mask_flags
        out = []
        sb = SBIT[size]
        for bit, name in FLAG_NAMES:
            if not live & bit:
                continue
            if bit == ZF:
                e = '%s == 0' % r
            elif bit == SF:
                e = '(%s >> %d) & 1u' % (r, sb)
            elif bit == PF:
                e = 'rt_parity(%s)' % r
            elif bit == CF:
                e = {
                    'add': '%s < %s' % (r, d),
                    'adc': '%s ? %s <= %s : %s < %s' % (cin, r, d, r, d),
                    'sub': '%s < %s' % (d, s),
                    'sbb': '%s ? %s <= %s : %s < %s' % (cin, d, s, d, s),
                    'logic': '0',
                    'neg': '%s != 0' % d,
                }.get(kind)
                if e is None:
                    continue
            elif bit == OF:
                e = {
                    'add': '((~(%s ^ %s) & (%s ^ %s)) >> %d) & 1u' % (d, s, d, r, sb),
                    'adc': '((~(%s ^ %s) & (%s ^ %s)) >> %d) & 1u' % (d, s, d, r, sb),
                    'sub': '(((%s ^ %s) & (%s ^ %s)) >> %d) & 1u' % (d, s, d, r, sb),
                    'sbb': '(((%s ^ %s) & (%s ^ %s)) >> %d) & 1u' % (d, s, d, r, sb),
                    'logic': '0',
                    'neg': '%s == %s' % (d, h(1 << sb)),
                    'inc': '%s == %s' % (r, h(1 << sb)),
                    'dec': '%s == %s' % (r, h((1 << sb) - 1)),
                }.get(kind)
                if e is None:
                    continue
            out.append('%s = %s;' % (name, e))
        return out

    # ------------------------------------------------------------ control
    def goto(self, target):
        if target in self.fn.insns:
            self.labels_used.add(target)
            return 'goto L_%08X;' % target
        return self.tail_call(target)

    def tail_call(self, target):
        g = self.g
        if target in g.func_set:
            return 'SPILL; f_%08X(c); return;' % target
        return 'SPILL; rt_dispatch(c, %s); return;' % h(target)

    def is_noreturn_call(self, ins):
        imp = self.an.import_of(ins)
        return imp is not None and imp[1] in NORETURN_IMPORTS

    # --------------------------------------------------------------- emit
    def emit(self):
        self.compute_liveness()
        fn = self.fn
        body = []
        for idx, a in enumerate(self.order):
            ins = fn.insns[a]
            if a in self.fused:
                continue
            try:
                code = self.emit_insn(ins, idx)
            except Unsupported as e:
                code = ['SPILL; rt_unimplemented(c, %s, "%s"); return;' % (h(a), str(e).replace('"', "'"))]
            nxt = a + ins.size
            falls = self.falls_through(ins)
            last = ins
            if a in self.fused_pairs:
                last = fn.insns[self.fused_pairs[a]]
                nxt = last.addr + last.size
                falls = self.falls_through(last)
            following = self.order[idx + 1] if idx + 1 < len(self.order) else None
            if following is not None and following in self.fused:
                following = self.order[idx + 2] if idx + 2 < len(self.order) else None
            if falls and following != nxt:
                code.append(self.goto(nxt))
            body.append((a, ins.text, code))
        out = []
        if self.order and self.order[0] != fn.entry:
            # Blocks below the entry address (backward jumps) are emitted first.
            self.labels_used.add(fn.entry)
            out.append('    goto L_%08X;' % fn.entry)
        for a, text, code in body:
            if a in self.labels_used:
                out.append('L_%08X:;' % a)
            out.append('    /* %08X: %s */' % (a, text))
            for line in code:
                out.append('    ' + line)
        return out

    def falls_through(self, ins):
        m = ins.mnem.split()[-1]
        if m in ('ret', 'retf', 'iretd', 'jmp', 'int3', 'hlt'):
            return False
        if m == 'call' and self.is_noreturn_call(ins):
            return False
        return True

    def emit_insn(self, ins, idx):
        m = ins.mnem
        a = ins.addr
        ops = ins.ops
        nxt = a + ins.size
        raw = bytes(ins.raw.bytes)
        if raw[0] in (0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF) or m == 'wait':
            return self.emit_x87(ins, raw)
        self.cur_idx = idx
        base = m.split()[-1]
        if base in STRING_OPS and (not ops or all(o.type == X.X86_OP_MEM for o in ops)
                                   or base[:4] in ('stos', 'lods', 'scas')):
            return self.emit_string(ins)
        h_ = getattr(self, 'i_' + m, None)
        if h_ is None:
            cc = cond_of(m)
            if cc and m.startswith('j'):
                return self.i_jcc(ins, cc)
            if cc and m.startswith('set'):
                return self.i_setcc(ins, cc)
            raise Unsupported(m)
        return h_(ins)

    # ------------------------------------------------------- integer ALU
    def binop(self, ins, kind, cexpr, write=True, cin=False):
        a = ins.addr
        d_op, s_op = ins.ops[0], ins.ops[1]
        size = d_op.size
        out = ['{']
        av = None
        if d_op.type == X.X86_OP_MEM:
            out.append('uint32_t ea = %s;' % self.addr(d_op))
            av = 'ea'
        out.append('uint32_t d = %s;' % self.rd(d_op, size, av))
        out.append('uint32_t s = %s;' % self.rd(s_op, size))
        if cin:
            out.append('uint32_t ci = (uint32_t)cf;')
        r = cexpr.replace('D', 'd').replace('S', 's').replace('CI', 'ci')
        if size != 4:
            r = '(%s) & %s' % (r, MASK[size])
        out.append('uint32_t r = %s;' % r)
        out += self.set_flags(a, kind, size, 'r', 'd', 's', 'ci' if cin else None)
        if write:
            out.append(self.wr(d_op, 'r', size, av))
        out.append('}')
        return [' '.join(out)]

    def i_add(self, ins):
        if ins.ops[0].type == X.X86_OP_REG and self.rname(ins.ops[0].reg) == 'esp' and not self.live_out[ins.addr]:
            return ['esp += %s;' % self.rd(ins.ops[1], 4)]
        return self.binop(ins, 'add', 'D + S')

    def i_sub(self, ins):
        if ins.ops[0].type == X.X86_OP_REG and self.rname(ins.ops[0].reg) == 'esp' and not self.live_out[ins.addr]:
            return ['esp -= %s;' % self.rd(ins.ops[1], 4)]
        return self.binop(ins, 'sub', 'D - S')

    def i_adc(self, ins):
        return self.binop(ins, 'adc', 'D + S + CI', cin=True)

    def i_sbb(self, ins):
        return self.binop(ins, 'sbb', 'D - S - CI', cin=True)

    def i_and(self, ins):
        return self.binop(ins, 'logic', 'D & S')

    def i_or(self, ins):
        return self.binop(ins, 'logic', 'D | S')

    def i_xor(self, ins):
        o = ins.ops
        if o[0].type == X.X86_OP_REG and o[1].type == X.X86_OP_REG and o[0].reg == o[1].reg:
            out = [self.wr(o[0], '0')]
            live = self.live_out[ins.addr]
            for bit, name in FLAG_NAMES:
                if live & bit:
                    out.append('%s = %d;' % (name, 1 if bit in (ZF, PF) else 0))
            return out
        return self.binop(ins, 'logic', 'D ^ S')

    def fuse_next(self, ins, idx):
        """If the next instruction is a jcc/setcc that only this compare feeds, return it."""
        if idx + 1 >= len(self.order):
            return None
        n = self.fn.insns[self.order[idx + 1]]
        if n.addr != ins.addr + ins.size or n.addr in self.fn.block_starts:
            return None
        cc = cond_of(n.mnem)
        if not cc or cc in ('o', 'no', 'p', 'np'):
            return None
        if not (n.mnem.startswith('j') or n.mnem.startswith('set')) or n.mnem in ('jecxz',):
            return None
        return n

    def cmp_like(self, ins, is_test):
        idx = self.cur_idx
        d_op, s_op = ins.ops
        size = d_op.size
        st = STYPE[size]
        n = self.fuse_next(ins, idx)
        out = ['{']
        out.append('uint32_t d = %s;' % self.rd(d_op, size))
        out.append('uint32_t s = %s;' % self.rd(s_op, size))
        if is_test:
            out.append('uint32_t r = d & s;')
            kind = 'logic'
        else:
            out.append('uint32_t r = %s;' % ('d - s' if size == 4 else '(d - s) & %s' % MASK[size]))
            kind = 'sub'
        if n is None:
            out += self.set_flags(ins.addr, kind, size, 'r', 'd', 's')
            out.append('(void)r;')
            out.append('}')
            return [' '.join(out)]
        # Fused compare + branch/set.
        self.fused.add(n.addr)
        self.fused_pairs[ins.addr] = n.addr
        cc = cond_of(n.mnem)
        if is_test:
            e = {
                'e': 'r == 0', 'ne': 'r != 0', 's': '(%s)r < 0' % st, 'ns': '(%s)r >= 0' % st,
                'l': '(%s)r < 0' % st, 'ge': '(%s)r >= 0' % st, 'le': '(%s)r <= 0' % st, 'g': '(%s)r > 0' % st,
                'b': '0', 'ae': '1', 'be': 'r == 0', 'a': 'r != 0',
            }[cc]
        else:
            e = {
                'e': 'd == s', 'ne': 'd != s', 'b': 'd < s', 'ae': 'd >= s', 'be': 'd <= s', 'a': 'd > s',
                'l': '(%s)d < (%s)s' % (st, st), 'ge': '(%s)d >= (%s)s' % (st, st),
                'le': '(%s)d <= (%s)s' % (st, st), 'g': '(%s)d > (%s)s' % (st, st),
                's': '(%s)r < 0' % st, 'ns': '(%s)r >= 0' % st,
            }[cc]
        # Flags still needed after the consumer.
        out += self.set_flags(ins.addr, kind, size, 'r', 'd', 's', mask_flags=self.live_out[n.addr])
        out.append('(void)r;')
        if n.mnem.startswith('set'):
            out.append(self.wr(n.ops[0], '(uint32_t)(%s)' % e, 1))
            out.append('}')
            return [' '.join(out)]
        t = self.an.direct_target(n)
        out.append('if (%s) { %s }' % (e, self.goto(t)))
        out.append('}')
        return [' '.join(out)]

    def i_cmp(self, ins):
        return self.cmp_like(ins, False)

    def i_test(self, ins):
        return self.cmp_like(ins, True)

    def i_inc(self, ins):
        return self.incdec(ins, '+', 'inc')

    def i_dec(self, ins):
        return self.incdec(ins, '-', 'dec')

    def incdec(self, ins, opch, kind):
        o = ins.ops[0]
        size = o.size
        out = ['{']
        av = None
        if o.type == X.X86_OP_MEM:
            out.append('uint32_t ea = %s;' % self.addr(o))
            av = 'ea'
        r = '%s %s 1u' % (self.rd(o, size, av), opch)
        if size != 4:
            r = '(%s) & %s' % (r, MASK[size])
        out.append('uint32_t r = %s;' % r)
        out += self.set_flags(ins.addr, kind, size, 'r')
        out.append(self.wr(o, 'r', size, av))
        out.append('}')
        return [' '.join(out)]

    def i_neg(self, ins):
        o = ins.ops[0]
        size = o.size
        out = ['{']
        av = None
        if o.type == X.X86_OP_MEM:
            out.append('uint32_t ea = %s;' % self.addr(o))
            av = 'ea'
        out.append('uint32_t d = %s;' % self.rd(o, size, av))
        out.append('uint32_t r = %s;' % ('0u - d' if size == 4 else '(0u - d) & %s' % MASK[size]))
        out += self.set_flags(ins.addr, 'neg', size, 'r', 'd', 'd')
        out.append(self.wr(o, 'r', size, av))
        out.append('}')
        return [' '.join(out)]

    def i_not(self, ins):
        o = ins.ops[0]
        size = o.size
        if o.type == X.X86_OP_MEM:
            return ['{ uint32_t ea = %s; %s }' % (self.addr(o), self.wr(o, '~%s' % self.rd(o, size, 'ea'), size, 'ea'))]
        return [self.wr(o, '~%s' % self.rd(o, size), size)]

    def shift(self, ins, kind):
        d_op, c_op = ins.ops[0], ins.ops[1]
        size = d_op.size
        bits = 8 * size
        sb = SBIT[size]
        live = self.live_out[ins.addr]
        out = ['{']
        av = None
        if d_op.type == X.X86_OP_MEM:
            out.append('uint32_t ea = %s;' % self.addr(d_op))
            av = 'ea'
        const = c_op.type == X.X86_OP_IMM
        if const:
            n = c_op.imm & 31
            if n == 0:
                return []
            out.append('const uint32_t n = %du;' % n)
        else:
            out.append('uint32_t n = %s & 31u;' % self.rd(c_op, 1))
        out.append('uint32_t d = %s;' % self.rd(d_op, size, av))
        fl = []
        if kind == 'shl':
            r = 'd << n'
            fl_cf = '(n <= %d) ? (d >> (%d - n)) & 1u : 0u' % (bits, bits)
            fl_of = '((r >> %d) & 1u) ^ (uint32_t)cf' % sb
        elif kind == 'shr':
            r = 'd >> n'
            fl_cf = '(d >> (n - 1)) & 1u'
            fl_of = '(d >> %d) & 1u' % sb
        elif kind == 'sar':
            r = '(uint32_t)((int32_t)(%s)d >> n)' % STYPE[size]
            fl_cf = '(uint32_t)(((int32_t)(%s)d >> (n - 1)) & 1)' % STYPE[size]
            fl_of = '0u'
        elif kind == 'rol':
            r = '(d << (n %% %d)) | (d >> ((%d - n %% %d) %% %d))' % (bits, bits, bits, bits)
            fl_cf = 'r & 1u'
            fl_of = '((r >> %d) ^ r) & 1u' % sb
        elif kind == 'ror':
            r = '(d >> (n %% %d)) | (d << ((%d - n %% %d) %% %d))' % (bits, bits, bits, bits)
            fl_cf = '(r >> %d) & 1u' % sb
            fl_of = '((r >> %d) ^ (r >> %d)) & 1u' % (sb, sb - 1)
        if size != 4:
            r = '(%s) & %s' % (r, MASK[size])
        out.append('uint32_t r = %s;' % r)
        if kind in ('rol', 'ror') and size != 4:
            pass
        if live:
            if live & CF:
                fl.append('cf = (int)(%s);' % fl_cf)
            if live & OF:
                fl.append('of = (int)(%s);' % fl_of)
            if kind not in ('rol', 'ror'):
                if live & ZF:
                    fl.append('zf = r == 0;')
                if live & SF:
                    fl.append('sf = (r >> %d) & 1u;' % sb)
                if live & PF:
                    fl.append('pf = rt_parity(r);')
            # OF for shl uses the new CF: order CF before OF (already).
            if kind == 'shl' and live & OF and not live & CF:
                fl.insert(0, 'cf = (int)(%s);' % fl_cf)
            if const:
                out += fl
            else:
                out.append('if (n) { %s }' % ' '.join(fl))
        out.append(self.wr(d_op, 'r', size, av))
        out.append('}')
        return [' '.join(out)]

    def i_shl(self, ins):
        return self.shift(ins, 'shl')

    i_sal = i_shl

    def i_shr(self, ins):
        return self.shift(ins, 'shr')

    def i_sar(self, ins):
        return self.shift(ins, 'sar')

    def i_rol(self, ins):
        return self.shift(ins, 'rol')

    def i_ror(self, ins):
        return self.shift(ins, 'ror')

    def rc(self, ins, left):
        d_op, c_op = ins.ops
        size = d_op.size
        av = None
        out = ['{']
        if d_op.type == X.X86_OP_MEM:
            out.append('uint32_t ea = %s;' % self.addr(d_op))
            av = 'ea'
        out.append('uint32_t n = %s & 31u;' % self.rd(c_op, 1))
        out.append('uint32_t d = %s;' % self.rd(d_op, size, av))
        fn = 'rt_rcl' if left else 'rt_rcr'
        out.append('uint32_t r = %s(d, n, %d, &cf, &of);' % (fn, 8 * size))
        out.append(self.wr(d_op, 'r', size, av))
        out.append('}')
        return [' '.join(out)]

    def i_rcl(self, ins):
        return self.rc(ins, True)

    def i_rcr(self, ins):
        return self.rc(ins, False)

    def i_shld(self, ins):
        return self.dshift(ins, True)

    def i_shrd(self, ins):
        return self.dshift(ins, False)

    def dshift(self, ins, left):
        d_op, s_op, c_op = ins.ops
        if d_op.size != 4:
            raise Unsupported('16-bit shld/shrd')
        out = ['{']
        out.append('uint32_t n = %s & 31u;' % self.rd(c_op, 1))
        out.append('uint32_t d = %s;' % self.rd(d_op))
        out.append('uint32_t s = %s;' % self.rd(s_op))
        if left:
            out.append('uint32_t r = n ? (d << n) | (s >> (32u - n)) : d;')
            cfe = '(d >> (32u - n)) & 1u'
        else:
            out.append('uint32_t r = n ? (d >> n) | (s << (32u - n)) : d;')
            cfe = '(d >> (n - 1u)) & 1u'
        live = self.live_out[ins.addr]
        fl = []
        if live & CF:
            fl.append('cf = (int)(%s);' % cfe)
        if live & ZF:
            fl.append('zf = r == 0;')
        if live & SF:
            fl.append('sf = r >> 31;')
        if live & OF:
            fl.append('of = (int)(((r ^ d) >> 31) & 1u);')
        if live & PF:
            fl.append('pf = rt_parity(r);')
        if fl:
            out.append('if (n) { %s }' % ' '.join(fl))
        out.append(self.wr(d_op, 'r'))
        out.append('}')
        return [' '.join(out)]

    def i_mul(self, ins):
        o = ins.ops[0]
        size = o.size
        live = self.live_out[ins.addr]
        if size == 4:
            out = ['{ uint64_t p = (uint64_t)eax * (uint64_t)%s;' % self.rd(o),
                   'eax = (uint32_t)p; edx = (uint32_t)(p >> 32);']
            ov = 'edx != 0'
        elif size == 1:
            out = ['{ uint32_t p = (eax & 0xFFu) * %s;' % self.rd(o), self.reg_write('ax', 'p')]
            ov = '(p >> 8) != 0'
        else:
            raise Unsupported('mul16')
        out += self.mul_flags(live, ov, 'eax')
        out.append('}')
        return [' '.join(out)]

    def mul_flags(self, live, ov, res):
        fl = []
        if live & CF:
            fl.append('cf = %s;' % ov)
        if live & OF:
            fl.append('of = %s;' % ov)
        if live & ZF:
            fl.append('zf = %s == 0;' % res)
        if live & SF:
            fl.append('sf = %s >> 31;' % res)
        if live & PF:
            fl.append('pf = rt_parity(%s);' % res)
        return fl

    def i_imul(self, ins):
        o = ins.ops
        live = self.live_out[ins.addr]
        if len(o) == 1:
            size = o[0].size
            if size == 4:
                out = ['{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t)%s;' % self.rd(o[0]),
                       'eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32);']
                out += self.mul_flags(live, 'p != (int64_t)(int32_t)p', 'eax')
            elif size == 1:
                out = ['{ int32_t p = (int32_t)(int8_t)(eax & 0xFFu) * (int32_t)(int8_t)%s;' % self.rd(o[0]),
                       self.reg_write('ax', '(uint32_t)p')]
                out += self.mul_flags(live, 'p != (int32_t)(int8_t)p', 'eax')
            else:
                raise Unsupported('imul16')
            out.append('}')
            return [' '.join(out)]
        dst = o[0]
        if dst.size != 4:
            raise Unsupported('imul16')
        a_ = self.rd(o[1]) if len(o) == 2 else self.rd(o[1])
        b_ = self.rd(o[0]) if len(o) == 2 else self.rd(o[2], 4)
        out = ['{ int64_t p = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s;' % (a_, b_),
               'uint32_t r = (uint32_t)p;']
        out += self.mul_flags(live, 'p != (int64_t)(int32_t)p', 'r')
        out.append(self.wr(dst, 'r'))
        out.append('}')
        return [' '.join(out)]

    def i_div(self, ins):
        o = ins.ops[0]
        if o.size == 4:
            return ['{ uint32_t v = %s; uint64_t n = ((uint64_t)edx << 32) | eax; '
                    'if (!v) { SPILL; rt_divide_error(c, %s); return; } '
                    'eax = (uint32_t)(n / v); edx = (uint32_t)(n %% v); }' % (self.rd(o), h(ins.addr))]
        if o.size == 1:
            return ['{ uint32_t v = %s; uint32_t n = eax & 0xFFFFu; '
                    'if (!v) { SPILL; rt_divide_error(c, %s); return; } '
                    'eax = (eax & 0xFFFF0000u) | ((n %% v) << 8) | ((n / v) & 0xFFu); }' % (self.rd(o), h(ins.addr))]
        raise Unsupported('div16')

    def i_idiv(self, ins):
        o = ins.ops[0]
        if o.size != 4:
            raise Unsupported('idiv8/16')
        return ['{ int32_t v = (int32_t)%s; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); '
                'if (!v) { SPILL; rt_divide_error(c, %s); return; } '
                'eax = (uint32_t)(int32_t)(n / v); edx = (uint32_t)(int32_t)(n %% v); }' % (self.rd(o), h(ins.addr))]

    def i_bsf(self, ins):
        return self.bitscan(ins, '(uint32_t)__builtin_ctz(s)')

    def i_bsr(self, ins):
        return self.bitscan(ins, '31u - (uint32_t)__builtin_clz(s)')

    def bitscan(self, ins, expr):
        d, s = ins.ops
        live = self.live_out[ins.addr]
        zf = ' zf = 1;' if live & ZF else ''
        zf0 = ' zf = 0;' if live & ZF else ''
        return ['{ uint32_t s = %s; if (s) { %s%s } else {%s } }' % (self.rd(s), self.wr(d, expr), zf0, zf)]

    # ------------------------------------------------------------ moves
    def i_mov(self, ins):
        d, s = ins.ops
        if d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
            return []
        return [self.wr(d, self.rd(s, d.size))]

    def i_movzx(self, ins):
        d, s = ins.ops
        return [self.wr(d, self.rd(s))]

    def i_movsx(self, ins):
        d, s = ins.ops
        return [self.wr(d, '(uint32_t)(int32_t)(%s)%s' % (STYPE[s.size], self.rd(s)))]

    def i_lea(self, ins):
        d, s = ins.ops
        m = s.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(self.rname(m.base)))
        if m.index:
            idx = self.reg_read(self.rname(m.index))
            parts.append(idx if m.scale == 1 else '%s * %du' % (idx, m.scale))
        if m.disp or not parts:
            parts.append(h(m.disp))
        return [self.wr(d, '(uint32_t)(%s)' % ' + '.join(parts))]

    def i_xchg(self, ins):
        a_, b_ = ins.ops
        if a_.type == X.X86_OP_MEM:
            a_, b_ = b_, a_
        if b_.type == X.X86_OP_MEM:
            return ['{ uint32_t ea = %s; uint32_t t = %s; %s %s }' % (
                self.addr(b_), self.rd(a_), self.wr(a_, self.rd(b_, None, 'ea')), self.wr(b_, 't', None, 'ea'))]
        return ['{ uint32_t t = %s; %s %s }' % (self.rd(a_), self.wr(a_, self.rd(b_)), self.wr(b_, 't'))]

    def i_cdq(self, ins):
        return ['edx = (uint32_t)((int32_t)eax >> 31);']

    def i_cwde(self, ins):
        return ['eax = (uint32_t)(int32_t)(int16_t)(eax & 0xFFFFu);']

    def i_xlatb(self, ins):
        return [self.reg_write('al', 'RD8((uint32_t)(ebx + (eax & 0xFFu)))')]

    def i_nop(self, ins):
        return []

    def i_cld(self, ins):
        return ['c->df = 0;']

    def i_std(self, ins):
        return ['c->df = 1;']

    def i_clc(self, ins):
        return ['cf = 0;'] if self.live_out[ins.addr] & CF else []

    def i_stc(self, ins):
        return ['cf = 1;'] if self.live_out[ins.addr] & CF else []

    def i_cmc(self, ins):
        return ['cf = !cf;'] if self.live_out[ins.addr] & CF else []

    def i_lahf(self, ins):
        return [self.reg_write('ah', '(uint32_t)((sf << 7) | (zf << 6) | (pf << 2) | 2 | cf)')]

    def i_sahf(self, ins):
        live = self.live_out[ins.addr]
        out = ['{ uint32_t f = (eax >> 8) & 0xFFu;']
        if live & SF:
            out.append('sf = (f >> 7) & 1u;')
        if live & ZF:
            out.append('zf = (f >> 6) & 1u;')
        if live & PF:
            out.append('pf = (f >> 2) & 1u;')
        if live & CF:
            out.append('cf = f & 1u;')
        out.append('(void)f; }')
        return [' '.join(out)]

    def i_pushfd(self, ins):
        return ['esp -= 4u; WR32(esp, (uint32_t)(cf | (pf << 2) | (zf << 6) | (sf << 7) | (of << 11)) | (c->df << 10) | 0x202u | c->eflags_extra);']

    def i_popfd(self, ins):
        return ['{ uint32_t f = RD32(esp); esp += 4u; cf = f & 1u; pf = (f >> 2) & 1u; zf = (f >> 6) & 1u; '
                'sf = (f >> 7) & 1u; of = (f >> 11) & 1u; c->df = (f >> 10) & 1u; c->eflags_extra = f & 0x00240000u; }']

    def i_setcc(self, ins, cc):
        return [self.wr(ins.ops[0], '(uint32_t)(%s)' % COND[cc][0], 1)]

    # ------------------------------------------------------------ stack
    def i_push(self, ins):
        o = ins.ops[0]
        if o.type == X.X86_OP_REG and self.rname(o.reg) not in REGS32:
            raise Unsupported('push %s' % self.rname(o.reg))
        if o.type == X.X86_OP_IMM:
            return ['esp -= 4u; WR32(esp, %s);' % h(o.imm)]
        return ['{ uint32_t v = %s; esp -= 4u; WR32(esp, v); }' % self.rd(o, 4)]

    def i_pop(self, ins):
        o = ins.ops[0]
        if o.type == X.X86_OP_REG:
            if self.rname(o.reg) not in REGS32:
                raise Unsupported('pop %s' % self.rname(o.reg))
            if self.rname(o.reg) == 'esp':
                return ['esp = RD32(esp);']
            return ['%s = RD32(esp); esp += 4u;' % self.rname(o.reg)]
        return ['{ uint32_t v = RD32(esp); esp += 4u; %s }' % self.wr(o, 'v', 4)]

    def i_leave(self, ins):
        return ['esp = ebp; ebp = RD32(esp); esp += 4u;']

    # ---------------------------------------------------------- control
    def i_jmp(self, ins):
        fn = self.fn
        a = ins.addr
        t = self.an.direct_target(ins)
        if t is not None:
            if a in fn.tail_calls or t not in fn.insns:
                return [self.tail_call(t)]
            return [self.goto(t)]
        imp = self.an.import_of(ins)
        if imp:
            return ['SPILL; %s(c); return;' % self.g.import_fn(imp)]
        o = ins.ops[0]
        if a in fn.jump_tables:
            cases = ' '.join('case %s: %s' % (h(x), self.goto(x)) for x in fn.jump_tables[a])
            return ['{ uint32_t t = %s; switch (t) { %s default: SPILL; rt_dispatch(c, t); return; } }' % (
                self.rd(o, 4), cases)]
        return ['{ uint32_t t = %s; SPILL; rt_dispatch(c, t); return; }' % self.rd(o, 4)]

    def i_jcc(self, ins, cc):
        t = self.an.direct_target(ins)
        return ['if (%s) { %s }' % (COND[cc][0], self.goto(t))]

    def i_jecxz(self, ins):
        return ['if (ecx == 0) { %s }' % self.goto(self.an.direct_target(ins))]

    def i_loop(self, ins):
        return ['ecx -= 1u; if (ecx != 0) { %s }' % self.goto(self.an.direct_target(ins))]

    def i_loope(self, ins):
        return ['ecx -= 1u; if (ecx != 0 && zf) { %s }' % self.goto(self.an.direct_target(ins))]

    def i_loopne(self, ins):
        return ['ecx -= 1u; if (ecx != 0 && !zf) { %s }' % self.goto(self.an.direct_target(ins))]

    def i_call(self, ins):
        ret = h(ins.addr + ins.size)
        t = self.an.direct_target(ins)
        if t is not None:
            if t in self.g.func_set:
                return ['esp -= 4u; WR32(esp, %s); SPILL; f_%08X(c); RELOAD;' % (ret, t)]
            return ['esp -= 4u; WR32(esp, %s); SPILL; rt_dispatch(c, %s); RELOAD;' % (ret, h(t))]
        imp = self.an.import_of(ins)
        if imp:
            call = 'esp -= 4u; WR32(esp, %s); SPILL; %s(c);' % (ret, self.g.import_fn(imp))
            if imp[1] in NORETURN_IMPORTS:
                return [call + ' rt_noreturn(c, %s); return;' % h(ins.addr)]
            return [call + ' RELOAD;']
        return ['{ uint32_t t = %s; esp -= 4u; WR32(esp, %s); SPILL; rt_dispatch(c, t); RELOAD; }' % (
            self.rd(ins.ops[0], 4), ret)]

    def i_ret(self, ins):
        n = 4 + (ins.ops[0].imm if ins.ops else 0)
        return ['esp += %du; SPILL; return;' % n]

    def i_int3(self, ins):
        return ['SPILL; rt_unimplemented(c, %s, "int3"); return;' % h(ins.addr)]

    # --------------------------------------------------------- strings
    def emit_string(self, ins):
        m = ins.mnem
        rep = None
        if m.startswith('rep'):
            rep, m = m.split()
        size = {'b': 1, 'w': 2, 'd': 4}[m[-1]]
        kind = m[:-1]
        step = '(c->df ? (uint32_t)-%d : %du)' % (size, size)
        live = self.live_out[ins.addr]
        if kind == 'movs':
            if rep:
                return ['SPILL; rt_rep_movs(c, %d); RELOAD;' % size]
            return ['%s(edi, %s(esi)); esi += %s; edi += %s;' % (WR[size], RD[size], step, step)]
        if kind == 'stos':
            val = {1: '(eax & 0xFFu)', 2: '(eax & 0xFFFFu)', 4: 'eax'}[size]
            if rep:
                return ['SPILL; rt_rep_stos(c, %d); RELOAD;' % size]
            return ['%s(edi, %s); edi += %s;' % (WR[size], val, step)]
        if kind == 'lods':
            val = {1: 'al', 2: 'ax', 4: 'eax'}[size]
            if rep:
                raise Unsupported('rep lods')
            return ['%s esi += %s;' % (self.reg_write(val, '%s(esi)' % RD[size]), step)]
        if kind in ('scas', 'cmps'):
            if kind == 'scas':
                d = {1: '(eax & 0xFFu)', 2: '(eax & 0xFFFFu)', 4: 'eax'}[size]
                s = '%s(edi)' % RD[size]
                adv = 'edi += %s;' % step
            else:
                d = '%s(esi)' % RD[size]
                s = '%s(edi)' % RD[size]
                adv = 'esi += %s; edi += %s;' % (step, step)
            r = 'd - s' if size == 4 else '(d - s) & %s' % MASK[size]
            fl = ' '.join(self.set_flags(ins.addr, 'sub', size, 'r', 'd', 's', mask_flags=live))
            if not rep:
                return ['{ uint32_t d = %s; uint32_t s = %s; uint32_t r = %s; %s (void)r; %s }' % (d, s, r, fl, adv)]
            stop = 'r == 0' if rep == 'repne' else 'r != 0'
            return ['while (ecx != 0) { uint32_t d = %s; uint32_t s = %s; uint32_t r = %s; ecx -= 1u; %s %s '
                    'if (%s) break; }' % (d, s, r, adv, fl + ' (void)r;', stop)]
        raise Unsupported(ins.mnem)

    # ------------------------------------------------------------- x87
    def emit_x87(self, ins, raw):
        if ins.mnem == 'wait':
            return []
        i = 0
        while raw[i] in (0x66, 0x67, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x9B):
            i += 1
        op = raw[i]
        modrm = raw[i + 1]
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        mem = modrm < 0xC0
        a = ins.addr
        if mem:
            mop = next(o for o in ins.ops if o.type == X.X86_OP_MEM)
            ea = self.addr(mop)
        arith = ['+', '*', None, None, '-', 'R-', '/', 'R/']

        def arith_expr(k, x, y):
            o = arith[k]
            if o.startswith('R'):
                return '%s %s %s' % (y, o[1], x)
            return '%s %s %s' % (x, o, y)

        if op in (0xD8, 0xDC, 0xDA, 0xDE) and mem:
            load = {0xD8: 'RDF32(%s)', 0xDC: 'RDF64(%s)', 0xDA: '(double)(int32_t)RD32(%s)',
                    0xDE: '(double)(int16_t)RD16(%s)'}[op] % ea
            if reg in (2, 3):
                pop = ' fpop(c);' if reg == 3 else ''
                return ['fcompare(c, ST(0), %s);%s' % (load, pop)]
            return ['{ double m = %s; ST(0) = %s; }' % (load, arith_expr(reg, 'ST(0)', 'm'))]
        if op == 0xD8 and not mem:
            if reg in (2, 3):
                return ['fcompare(c, ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 3 else '')]
            return ['ST(0) = %s;' % arith_expr(reg, 'ST(0)', 'ST(%d)' % rm)]
        if op in (0xDC, 0xDE) and not mem:
            if op == 0xDE and modrm == 0xD9:
                return ['fcompare(c, ST(0), ST(1)); fpop(c); fpop(c);']
            if reg in (2, 3):
                return ['fcompare(c, ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 3 else '')]
            # DC/DE register forms: ST(i) = ST(i) op ST(0), with sub/div directions swapped
            # relative to D8: /4 is FSUBR (st0 - sti), /5 FSUB, /6 FDIVR, /7 FDIV.
            swap = {4: 5, 5: 4, 6: 7, 7: 6}.get(reg, reg)
            code = 'ST(%d) = %s;' % (rm, arith_expr(swap, 'ST(%d)' % rm, 'ST(0)'))
            if op == 0xDE:
                code += ' fpop(c);'
            return [code]
        if op == 0xD9:
            if mem:
                if reg == 0:
                    return ['fpush(c, RDF32(%s));' % ea]
                if reg == 2:
                    return ['WRF32(%s, ST(0));' % ea]
                if reg == 3:
                    return ['WRF32(%s, ST(0)); fpop(c);' % ea]
                if reg == 4:
                    return ['rt_fldenv(c, %s);' % ea]
                if reg == 5:
                    return ['c->fcw = RD16(%s);' % ea]
                if reg == 6:
                    return ['rt_fnstenv(c, %s);' % ea]
                if reg == 7:
                    return ['WR16(%s, c->fcw);' % ea]
                raise Unsupported('D9 /%d' % reg)
            if reg == 0:
                return ['{ double v = ST(%d); fpush(c, v); }' % rm]
            if reg == 1:
                return ['{ double v = ST(0); ST(0) = ST(%d); ST(%d) = v; }' % (rm, rm)]
            simple = {
                0xD0: '',
                0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));',
                0xE4: 'fcompare(c, ST(0), 0.0);', 0xE5: 'rt_fxam(c);',
                0xE8: 'fpush(c, 1.0);', 0xE9: 'fpush(c, 3.321928094887362);',
                0xEA: 'fpush(c, 1.4426950408889634);', 0xEB: 'fpush(c, 3.141592653589793);',
                0xEC: 'fpush(c, 0.30102999566398120);', 0xED: 'fpush(c, 0.6931471805599453);',
                0xEE: 'fpush(c, 0.0);',
                0xF0: 'ST(0) = exp2(ST(0)) - 1.0;',
                0xF1: 'ST(1) = ST(1) * log2(ST(0)); fpop(c);',
                0xF2: 'ST(0) = tan(ST(0)); fpush(c, 1.0); c->fsw &= ~0x400u;',
                0xF3: 'ST(1) = atan2(ST(1), ST(0)); fpop(c);',
                0xF5: 'ST(0) = remainder(ST(0), ST(1)); c->fsw &= ~0x400u;',
                0xF8: 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400u;',
                0xFA: 'ST(0) = sqrt(ST(0));',
                0xFB: '{ double v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); c->fsw &= ~0x400u; }',
                0xFC: 'ST(0) = rt_fround(c, ST(0));',
                0xFD: 'ST(0) = ldexp(ST(0), (int)trunc(ST(1)));',
                0xFE: 'ST(0) = sin(ST(0)); c->fsw &= ~0x400u;',
                0xFF: 'ST(0) = cos(ST(0)); c->fsw &= ~0x400u;',
            }
            if modrm in simple:
                return [simple[modrm]] if simple[modrm] else []
            raise Unsupported('D9 %02X' % modrm)
        if op == 0xDD:
            if mem:
                if reg == 0:
                    return ['fpush(c, RDF64(%s));' % ea]
                if reg == 2:
                    return ['WRF64(%s, ST(0));' % ea]
                if reg == 3:
                    return ['WRF64(%s, ST(0)); fpop(c);' % ea]
                if reg == 4:
                    return ['rt_frstor(c, %s);' % ea]
                if reg == 6:
                    return ['rt_fnsave(c, %s);' % ea]
                if reg == 7:
                    return ['WR16(%s, rt_fstsw(c));' % ea]
                raise Unsupported('DD /%d' % reg)
            if reg == 0:
                return []                               # ffree
            if reg == 2:
                return ['ST(%d) = ST(0);' % rm]
            if reg == 3:
                return ['ST(%d) = ST(0); fpop(c);' % rm]
            if reg in (4, 5):
                return ['fcompare(c, ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 5 else '')]
            raise Unsupported('DD %02X' % modrm)
        if op == 0xDB:
            if mem:
                if reg == 0:
                    return ['fpush(c, (double)(int32_t)RD32(%s));' % ea]
                if reg in (2, 3):
                    return ['WR32(%s, (uint32_t)rt_fist32(c, ST(0)));%s' % (ea, ' fpop(c);' if reg == 3 else '')]
                if reg == 5:
                    return ['fpush(c, rt_ld80(%s));' % ea]
                if reg == 7:
                    return ['rt_st80(%s, ST(0)); fpop(c);' % ea]
                raise Unsupported('DB /%d' % reg)
            if modrm == 0xE2:
                return ['c->fsw &= 0x7F00u;']
            if modrm == 0xE3:
                return ['rt_fninit(c);']
            raise Unsupported('DB %02X' % modrm)
        if op == 0xDF:
            if mem:
                if reg == 0:
                    return ['fpush(c, (double)(int16_t)RD16(%s));' % ea]
                if reg in (2, 3):
                    return ['WR16(%s, (uint16_t)rt_fist16(c, ST(0)));%s' % (ea, ' fpop(c);' if reg == 3 else '')]
                if reg == 5:
                    return ['fpush(c, (double)(int64_t)RD64(%s));' % ea]
                if reg == 7:
                    return ['WR64(%s, (uint64_t)rt_fist64(c, ST(0))); fpop(c);' % ea]
                raise Unsupported('DF /%d' % reg)
            if modrm == 0xE0:
                return [self.reg_write('ax', 'rt_fstsw(c)')]
            raise Unsupported('DF %02X' % modrm)
        if op == 0xDA and modrm == 0xE9:
            return ['fcompare(c, ST(0), ST(1)); fpop(c); fpop(c);']
        raise Unsupported('x87 %02X %02X' % (op, modrm))


class Generator:
    FUNCS_PER_FILE = 150

    def __init__(self, exe):
        self.an = Analyzer(exe)
        self.funcs, self.entries = self.an.run()
        self.funcs = {e: f for e, f in self.funcs.items() if not f.bad}
        self.func_set = set(self.funcs)
        # Entry hooks: game-specific fixes (runtime/src/igi_fixes.c) plus diagnostic probes.
        self.probes = set(GAME_HOOKS) | {int(x, 16) for x in os.environ.get('IGI_PROBES', '').replace(',', ' ').split()}
        self.imports_used = {}

    def import_fn(self, imp):
        dll, name = imp
        cname = 'imp_%s_%s' % (dll.split('.')[0], re.sub(r'[^A-Za-z0-9_]', '_', name))
        self.imports_used[cname] = imp
        return cname

    def run(self, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        entries = sorted(self.funcs)
        unsupported = {}
        chunks = [entries[i:i + self.FUNCS_PER_FILE] for i in range(0, len(entries), self.FUNCS_PER_FILE)]
        files = []
        for n, chunk in enumerate(chunks):
            path = os.path.join(out_dir, 'funcs_%03d.c' % n)
            files.append(os.path.basename(path))
            with open(path, 'w') as f:
                f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n#include "recomp_gen.h"\n\n')
                for e in chunk:
                    fg = FuncGen(self, self.funcs[e])
                    body = fg.emit()
                    for line in body:
                        if 'rt_unimplemented' in line:
                            k = line.split('"')[1]
                            unsupported[k] = unsupported.get(k, 0) + 1
                    f.write(self.wrap(e, body))
        self.write_header(out_dir)
        self.write_table(out_dir, entries)
        with open(os.path.join(out_dir, 'files.txt'), 'w') as f:
            f.write('\n'.join(files + ['dispatch_table.c', 'import_stubs.c']) + '\n')
        print('functions: %d, files: %d' % (len(entries), len(files)))
        for k, v in sorted(unsupported.items(), key=lambda x: -x[1]):
            print('  unsupported %-30s %d' % (k, v))

    def wrap(self, entry, body):
        if entry in self.probes:
            # Diagnostic probe requested at configure time (IGI_PROBES).
            body = ['    rt_probe(c, 0x%08Xu);' % entry] + body
        text = '\n'.join(body)
        used_flags = [n for _, n in FLAG_NAMES if re.search(r'\b%s\b' % n, text)]
        out = ['void f_%08X(Ctx *c)' % entry, '{',
               '    uint32_t eax = c->r[0], ecx = c->r[1], edx = c->r[2], ebx = c->r[3];',
               '    uint32_t esp = c->r[4], ebp = c->r[5], esi = c->r[6], edi = c->r[7];']
        if used_flags:
            out.append('    int %s;' % ', '.join('%s = 0' % n for n in used_flags))
        out.append(text)
        out.append('}\n\n')
        return '\n'.join(out)

    def write_header(self, out_dir):
        with open(os.path.join(out_dir, 'recomp_gen.h'), 'w') as f:
            f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n')
            f.write('#ifndef RECOMP_GEN_H\n#define RECOMP_GEN_H\n#include "recomp.h"\n\n')
            f.write('#define SPILL do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; '
                    'c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)\n')
            f.write('#define RELOAD do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; '
                    'esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)\n\n')
            for e in sorted(self.funcs):
                f.write('void f_%08X(Ctx *c);\n' % e)
            f.write('\n')
            for cname in sorted(self.imports_used):
                f.write('void %s(Ctx *c);\n' % cname)
            f.write('\n#endif\n')

    def write_table(self, out_dir, entries):
        an = self.an
        with open(os.path.join(out_dir, 'dispatch_table.c'), 'w') as f:
            f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n#include "recomp_gen.h"\n\n')
            f.write('const GuestFunc g_guest_funcs[] = {\n')
            for e in entries:
                f.write('    { 0x%08Xu, f_%08X },\n' % (e, e))
            f.write('};\nconst uint32_t g_guest_func_count = %d;\n\n' % len(entries))
            # Every IAT slot, so the loader can point it at a host thunk.
            f.write('const GuestImport g_guest_imports[] = {\n')
            for slot, imp in sorted(an.pe.imports.items()):
                f.write('    { 0x%08Xu, "%s", "%s", %s },\n' % (slot, imp[0], imp[1], self.import_fn(imp)))
            f.write('};\nconst uint32_t g_guest_import_count = %d;\n' % len(an.pe.imports))
        # write_table adds imports not referenced by code; refresh the header.
        self.write_header(out_dir)
        # Weak fallbacks: any import without a host implementation fails loudly by name.
        with open(os.path.join(out_dir, 'import_stubs.c'), 'w') as f:
            f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n#include "recomp_gen.h"\n#include "runtime.h"\n\n')
            for cname, (dll, name) in sorted(self.imports_used.items()):
                f.write('__attribute__((weak)) void %s(Ctx *c) { rt_missing_import(c, "%s!%s"); }\n' % (cname, dll, name))


if __name__ == '__main__':
    Generator(sys.argv[1]).run(sys.argv[2])
