"""x86 -> C static recompiler for IGI.exe.

Every recovered function becomes `void f_XXXXXXXX(Ctx *c)`. Guest registers live
in C locals and are spilled to the context around calls; guest memory is a flat
array (`g_mem`) indexed by guest address. EFLAGS are computed only where a later
instruction reads them (backward liveness), and cmp/test + jcc/setcc pairs are
fused into direct C comparisons. x87 instructions are decoded from their raw
opcode bytes and run against the FPU stack kept in the context.

Usage: gen.py IGI.exe OUT_DIR
"""
import hashlib
import os
import re
import sys

from capstone import x86 as X

from analyze import Analyzer, Function, JCC

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
RD = {1: 'GRD8', 2: 'GRD16', 4: 'GRD32'}
WR = {1: 'GWR8', 2: 'GWR16', 4: 'GWR32'}
PRD = {1: 'PRD8', 2: 'PRD16', 4: 'PRD32'}
PWR = {1: 'PWR8', 2: 'PWR16', 4: 'PWR32'}

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

# MSVC CRT setjmp/longjmp: setjmp call sites become host setjmp, and the
# longjmp's final indirect jump becomes a host longjmp (see rt_jmpbuf).
SETJMP_FUNC = 0x004A5000
LONGJMP_JUMP = 0x004A50F1

NORETURN_IMPORTS = {'ExitProcess', 'TerminateProcess', 'RaiseException'}

# Static C runtime routines with native versions (runtime/src/crt_hle.c): entry ->
# (host function, code size, SHA-1 of the code). The native version is used only
# when the bytes match; it may decline (return 0) and run the guest code.
HLE = {
    0x004A5980: ('rt_hle_stricmp', 140, 'b74eac9d136f17e68cc33c831146d506c0eb41ef'),
    0x004ACD10: ('rt_hle_memset', 88, '4fd4d112065490f5cdd92c83810b685d8c896ca8'),
}

# Game functions with hand-decompiled native versions (runtime/src/native_*.c): entry ->
# (host function, [(address, size, SHA-1)] of every piece of guest code the native
# version reproduces). Same convention as HLE: the hook is emitted only when all the
# bytes match, and the native version may decline (return 0, e.g. when disabled at
# run time with IGI_NATIVE=0) to run the recompiled code. IGI_NATIVE=0 at configure
# time leaves the hooks out entirely.
NATIVE = {
    # Terrain renderer and its triangle clipper (runtime/src/native_terrain.c).
    0x00512C30: ('rt_native_terrain_render', [
        (0x00512C30, 6317, '0babcc6a828c5ba5fa10360608a72abc7b88dc57'),
        (0x00514760, 7839, '40c1f468e13cd4a2dbf61721e78741da9e81aaa6'),
    ]),
    # Skinned mesh transform into the dynamic vertex buffer (runtime/src/native_skin.c).
    0x0049B700: ('rt_native_skin_mesh', [
        (0x0049B700, 1086, '8d5e649fcba66a5c0499ad80f5f5bfaf7589f6cb'),
    ]),
    # Object mesh renderer (runtime/src/native_mesh.c).
    0x004A1570: ('rt_native_mesh_draw', [
        (0x004A1570, 3674, 'd92fcea1520b922a0f5cf1b176415cce29a3707f'),
    ]),
    # Terrain octree walk building the patch list (runtime/src/native_quadtree.c).
    0x004D14E0: ('rt_native_terrain_collect', [
        (0x004D14E0, 4181, '539333ef86b2329a05cbc5d9c8c72010b388de9a'),
    ]),
    # Rigid or bent mesh transform into the dynamic vertex buffer (runtime/src/native_deform.c).
    0x0049CC80: ('rt_native_deform_mesh', [
        (0x0049CC80, 1070, 'e15b1430ffbb328d2109daa7afcc7b45c84f1029'),
    ]),
    # View-space bounding sphere test (runtime/src/native_sphere.c).
    0x0049FEB0: ('rt_native_sphere_cull', [
        (0x0049FEB0, 603, 'd95fabbb53bb599991d5df1ae4eb32aaa1e7468d'),
    ]),
    # IMA ADPCM decoder (runtime/src/native_adpcm.c).
    0x00496280: ('rt_native_adpcm_decode', [
        (0x00496280, 409, 'a4e5b69d2e3cf1c83797f92ee34bd5b9512949ae'),
    ]),
}

# Instruction emitters produce C with a few tokens that are expanded per code copy
# (FuncGen.lower): ST(i), fpush(c, v), fpop(c) and FTOP address the x87 stack;
# SPILL/RELOAD synchronise registers and x87 state with the context around calls,
# ISPILL/IRELOAD only the integer registers, XSPILL/XRELOAD only the x87 state.
REG_ALT = '|'.join(REGS32)
TOKEN_RE = re.compile(r'\bST\((\d)\)|\bfpush\(c, |\bfpop\(c\);|\bFTOP\b|\bgoto L_'
                      r'|\b(%s)\s*(?:[-+*/%%&|^]|<<|>>)?=(?!=)'
                      r'|\b[IXCR]?SPILL;|\b[IXC]?RELOAD;|\bRET[VC]\b' % REG_ALT)
# `fst st(0)`, `fxch st(0)`... lower to self-assignments, which compilers warn about.
SELF_ASSIGN_RE = re.compile(r'(?<![\w\]])(fr\d|c->st\[[^\]]*\]) = \1;')
REGWRITE_RE = re.compile(r'\b(%s)\s*(?:[-+*/%%&|^]|<<|>>)?=(?!=)|\b([CI]?)RELOAD;' % REG_ALT)


class _Top:
    """Unknown x87 stack effect (lattice top)."""
    def __repr__(self):
        return 'TOP'


TOP = _Top()

# Generated functions take eax, ecx and esp as arguments and return eax and esp
# (uint64_t f(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax)); every other
# register is exchanged through the context. Dispatch goes through wrappers with
# the plain GuestFn signature (see Generator.write_table).
ARG_REGS = frozenset(('eax', 'ecx', 'esp'))
# Indirect call of `t` through a per-site cache of its dispatch hash entry (sites are
# mostly monomorphic); misses go through rt_call_indirect_ic, which refills it.
INDIRECT_CACHE = 'static const GuestFunc *ic = g_guest_func_hash; const GuestFunc *ge = ic;'
INDIRECT_CALL = ('(ge->addr == t && ge->fast ? ge->fast(c, esp, ecx, eax) '
                 ': rt_call_indirect_ic(c, t, esp, ecx, eax, &ic))')
FUNC_PROTO = 'uint64_t f_%08X(Ctx *c, uint32_t esp, uint32_t ecx, uint32_t eax)'
RET_REGS = frozenset(('eax', 'esp'))


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
        self.fcom_tests = set()
        self.cur_idx = 0
        self.fdepth = {}
        self.fcalls = {}
        self.dirty = {}
        self.fregs_used = set()
        self.fmode = 'none'
        self.need_dyn = False
        self.uses_fsw = True
        self.fsync = {}

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

    def mptr(self, op):
        """Host pointer to a memory operand. A small displacement off a base register
        is applied to the host pointer rather than the 32-bit guest address, so the
        compiler can use a register+offset addressing mode and share base+register
        between accesses. This differs from 32-bit wrap-around only when base+index
        is not itself a valid guest address, which compiled code does not produce
        for a small field or stack offset."""
        m = op.mem
        disp = ((m.disp + 0x80000000) & 0xFFFFFFFF) - 0x80000000
        if not m.base or disp == 0 or not -0x10000 < disp < 0x10000:
            return 'gm + %s' % self.addr(op)
        parts = []
        if m.segment and self.rname(m.segment) == 'fs':
            parts.append('c->fs_base')
        elif m.segment and self.rname(m.segment) not in ('ds', 'ss', 'es', 'cs'):
            raise Unsupported('segment %s' % self.rname(m.segment))
        parts.append(self.reg_read(self.rname(m.base)))
        if m.index:
            idx = self.reg_read(self.rname(m.index))
            parts.append(idx if m.scale == 1 else '%s * %du' % (idx, m.scale))
        regs = parts[0] if len(parts) == 1 else '(uint32_t)(%s)' % ' + '.join(parts)
        return 'gm + %s %s %d' % (regs, '+' if disp > 0 else '-', abs(disp))

    def rd(self, op, size=None, addr_var=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_read(self.rname(op.reg))
        if op.type == X.X86_OP_IMM:
            return h(op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return '%s(%s)' % (PRD[size], addr_var or self.mptr(op))
        raise Unsupported('operand')

    def wr(self, op, expr, size=None, addr_var=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_write(self.rname(op.reg), expr)
        if op.type == X.X86_OP_MEM:
            return '%s(%s, %s);' % (PWR[size], addr_var or self.mptr(op), expr)
        raise Unsupported('write operand')

    # ------------------------------------------------------------- flags
    def compute_succ(self):
        """Intra-function successors of every instruction (tail calls and returns have none)."""
        if self.succ:
            return
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

    def compute_liveness(self):
        self.compute_succ()
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
            return 'CSPILL; return f_%08X(c, esp, ecx, eax);' % target
        return 'SPILL; rt_dispatch(c, %s); return RETC;' % h(target)

    def is_noreturn_call(self, ins):
        imp = self.an.import_of(ins)
        return imp is not None and imp[1] in NORETURN_IMPORTS

    # ------------------------------------------------------ x87 stack depth
    def x87_fpu_info(self, ins):
        """(stack delta, forces dynamic mode) of an x87 instruction, else None."""
        return self.g.x87_info(ins)

    def call_fpu(self, ins, summ, proven):
        """x87 effect of a call: (delta, proven, callee). delta None: the callee never
        returns according to the analysis. Host imports never touch the x87 stack."""
        t = self.an.direct_target(ins)
        if t is not None:
            if t in self.g.func_set:
                s = summ.get(t)
                if s is None:
                    return None, False, t
                if s is TOP:
                    return 0, False, t
                # setjmp returns twice; the second return is not modelled.
                ok = t != SETJMP_FUNC and (proven is None or proven.get(t, False))
                return s, ok, t
            return 0, False, None               # dispatched at run time
        if self.an.import_of(ins):
            return 0, True, None
        return 0, False, None                   # indirect call

    def exit_targets(self, a):
        """Ways instruction `a` leaves the function: list of ('ret'|'tail'|'dyn', target)."""
        fn = self.fn
        ins = fn.insns[a]
        m = ins.mnem.split()[-1]
        out = []
        nxt = a + ins.size
        if m in ('ret', 'retf', 'iretd'):
            return [('ret', None)]
        if m in ('int3', 'hlt'):
            return []
        if m == 'jmp':
            if a == LONGJMP_JUMP:
                return []
            t = self.an.direct_target(ins)
            if t is not None:
                if a in fn.tail_calls or t not in fn.insns:
                    return [('tail', t)]
                return []
            if self.an.import_of(ins):
                return [('ret', None)]
            if a in fn.jump_tables:
                return []                       # the out-of-table default is unreachable
            return [('dyn', None)]
        if m in JCC:
            t = self.an.direct_target(ins)
            if t not in fn.insns:
                out.append(('tail', t))
        if m == 'call' and self.is_noreturn_call(ins):
            return out
        if nxt not in fn.insns:
            out.append(('tail', nxt))
        return out

    def fpu_flow(self, summ, proven=None):
        """Forward dataflow of the x87 stack depth relative to the function entry.

        Sets self.fdepth (depth before each reached instruction), self.fcalls (call
        site -> (delta, proven)), and returns (exit summary, locally unproven, callees,
        conflict). `conflict` means static depths cannot be used for code generation."""
        self.compute_succ()
        fn = self.fn
        depth = {fn.entry: 0}
        work = [fn.entry]
        exits = set()
        conflict = False
        unproven = False
        callees = set()
        calls = {}
        while work:
            a = work.pop()
            d = depth[a]
            ins = fn.insns[a]
            base = ins.mnem.split()[-1]
            info = self.x87_fpu_info(ins)
            o = d
            if info is not None:
                delta, dyn = info
                if dyn:
                    conflict = True
                o = d + delta
            elif base == 'call':
                if self.is_noreturn_call(ins):
                    continue
                delta, ok, callee = self.call_fpu(ins, summ, proven)
                calls[a] = (delta, ok)
                if callee is not None:
                    callees.add(callee)
                if not ok:
                    unproven = True
                if delta is None:
                    continue                    # never returns (per analysis)
                if callee is not None and summ.get(callee) is TOP:
                    exits.add(TOP)
                o = d + delta
            if o < -8 or o > 8:
                conflict = True
                o = max(-8, min(8, o))
            for kind, t in self.exit_targets(a):
                if kind == 'ret':
                    exits.add(o)
                elif kind == 'dyn':
                    unproven = True
                    exits.add(o)
                elif t in self.g.func_set:
                    callees.add(t)
                    s = summ.get(t)
                    if s is None:
                        unproven = True
                    elif s is TOP:
                        exits.add(TOP)
                        unproven = True
                    else:
                        exits.add(o + s)
                        if proven is not None and not proven.get(t, False):
                            unproven = True
                else:
                    unproven = True
                    exits.add(o)
            for s in self.succ[a]:
                if s not in depth:
                    depth[s] = o
                    work.append(s)
                elif depth[s] != o:
                    conflict = True
        self.fdepth = depth
        self.fcalls = calls
        if conflict or TOP in exits or len(exits) > 1:
            summary = TOP
        elif exits:
            summary = exits.pop()
        else:
            summary = None
        return summary, unproven, callees, conflict

    def compute_fsync(self):
        """Depth at which c->ftop was last written or checked, before each instruction
        (entry: 0; after a call: the depth it returns with); None where paths disagree."""
        sync = {self.fn.entry: 0}
        work = [self.fn.entry]
        while work:
            a = work.pop()
            out = sync[a]
            call = self.fcalls.get(a)
            if call is not None and call[0] is not None and a in self.fdepth:
                out = self.fdepth[a] + call[0]
            for s in self.succ[a]:
                if s not in sync:
                    sync[s] = out
                    work.append(s)
                elif sync[s] is not None and sync[s] != out:
                    sync[s] = None
                    work.append(s)
        self.fsync = sync

    def has_x87(self):
        return any(self.x87_fpu_info(self.fn.insns[a]) is not None for a in self.order)

    # ------------------------------------------------------ dirty registers
    def compute_dirty(self, raw):
        """Registers possibly modified since the context copy was last synchronised
        (entry or the reload after a call), before each instruction. Only these are
        stored back to the context before a call or return."""
        trans = {}
        for a, code in raw.items():
            gen, reset = set(), False
            for m in REGWRITE_RE.finditer(' '.join(code)):
                if m.group(1):
                    gen.add(m.group(1))
                else:
                    # After a direct call eax and esp come back in the return value.
                    gen, reset = (set(RET_REGS) if m.group(2) == 'C' else set()), True
            trans[a] = (gen, reset)
        dirty = {self.fn.entry: frozenset(ARG_REGS)}
        work = [self.fn.entry]
        while work:
            a = work.pop()
            gen, reset = trans.get(a, (set(), False))
            out = frozenset(gen) if reset else dirty[a] | gen
            for s in self.succ[a]:
                old = dirty.get(s)
                new = out if old is None else old | out
                if new != old:
                    dirty[s] = new
                    work.append(s)
        self.dirty = dirty

    # ------------------------------------------------------------ lowering
    def fslot(self, k):
        """C lvalue of x87 stack position k (1 = first value pushed by this function)."""
        if 1 <= k <= 8:
            self.fregs_used.add(k)
            return 'fr%d' % k
        return 'c->st[(fp_top0 + %du) & 7u]' % ((-k) & 7)

    def int_spill(self, dirty):
        return ''.join('c->r[%d] = %s; ' % (i, r) for i, r in enumerate(REGS32) if r in dirty)

    def fpu_flush(self, mode, d, sync=None):
        """Write the x87 state back to the context. `sync` is the depth at which
        c->ftop was last known to be current (static mode), making its store
        redundant when unchanged. The status word is only exchanged by functions
        that use it."""
        if mode == 'none':
            return ''
        sw = 'c->fsw = fp_sw; ' if self.uses_fsw else ''
        if mode == 'dynamic':
            return 'c->ftop = fp_top; ' + sw
        s = ''
        for k in range(1, min(d, 8) + 1):
            self.fregs_used.add(k)
            s += 'c->st[(fp_top0 + %du) & 7u] = fr%d; ' % ((-k) & 7, k)
        if sync != d:
            s += 'c->ftop = (fp_top0 + %du) & 7u; ' % ((-d) & 7)
        return s + sw

    def lower(self, a, code, mode, prefix, d, dirty):
        """Expand the generator tokens (ST, fpush, fpop, FTOP, SPILL, RELOAD, ...) of
        one instruction for a code copy. Returns (lines, depth after, transitioned)."""
        st = {'d': d, 'dirty': set(dirty), 'dspill': d, 'trans': False,
              'sync': self.fsync.get(a) if mode == 'static' else None}
        lines = [SELF_ASSIGN_RE.sub('', self.lower_text(a, line, mode, prefix, st)) for line in code]
        return lines, st['d'], st['trans']

    def lower_text(self, a, text, mode, prefix, st):
        out = []
        pos = 0
        while True:
            m = TOKEN_RE.search(text, pos)
            if not m:
                out.append(text[pos:])
                break
            out.append(text[pos:m.start()])
            tok = m.group(0)
            pos = m.end()
            if m.group(1) is not None:                  # ST(i)
                i = int(m.group(1))
                if mode == 'static':
                    out.append(self.fslot(st['d'] - i))
                else:
                    out.append('c->st[(fp_top + %du) & 7u]' % i)
            elif tok.startswith('fpush'):
                depth, j = 1, pos
                while depth:
                    if text[j] == '(':
                        depth += 1
                    elif text[j] == ')':
                        depth -= 1
                    j += 1
                expr = self.lower_text(a, text[pos:j - 1], mode, prefix, st)
                pos = j
                if pos < len(text) and text[pos] == ';':
                    pos += 1
                if mode == 'static':
                    st['d'] += 1
                    out.append('%s = %s;' % (self.fslot(st['d']), expr))
                else:
                    out.append('{ double fv = %s; fp_top = (fp_top - 1u) & 7u; c->st[fp_top] = fv; }' % expr)
            elif tok.startswith('fpop'):
                if mode == 'static':
                    st['d'] -= 1
                else:
                    out.append('fp_top = (fp_top + 1u) & 7u;')
            elif tok == 'FTOP':
                out.append('((fp_top0 + %du) & 7u)' % ((-st['d']) & 7) if mode == 'static' else 'fp_top')
            elif tok.startswith('goto'):
                out.append('goto %s_' % prefix)
            elif m.group(2) is not None:                # register write
                st['dirty'].add(m.group(2))
                out.append(tok)
            elif tok == 'XSPILL;':
                assert mode == 'dynamic'
                out.append('c->ftop = fp_top; c->fsw = fp_sw;')
            elif tok == 'XRELOAD;':
                assert mode == 'dynamic'
                out.append('fp_top = c->ftop; fp_sw = c->fsw;')
            elif tok.endswith('SPILL;'):
                # CSPILL: call of a generated function (eax, ecx, esp passed as
                # arguments); RSPILL: return (eax, esp in the return value).
                keep = st['dirty'] - {'CSPILL;': ARG_REGS, 'RSPILL;': RET_REGS}.get(tok, set())
                s = self.int_spill(keep)
                if tok != 'ISPILL;':
                    s += self.fpu_flush(mode, st['d'], st['sync'])
                    st['dspill'] = st['d']
                out.append(s.rstrip())
            elif tok.endswith('RELOAD;'):
                if tok == 'CRELOAD;':
                    s = ('eax = (uint32_t)rv; esp = (uint32_t)(rv >> 32); ecx = c->r[1]; edx = c->r[2]; '
                         'ebx = c->r[3]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7];')
                    st['dirty'] = set(RET_REGS)
                else:
                    s = ('eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; '
                         'esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7];')
                    st['dirty'] = set()
                if tok != 'IRELOAD;':
                    s += self.fpu_reload(a, mode, st)
                out.append(s)
            elif tok == 'RETV':
                out.append('(((uint64_t)esp << 32) | eax)')
            elif tok == 'RETC':
                out.append('(((uint64_t)c->r[4] << 32) | c->r[0])')
            else:
                raise AssertionError(tok)
        return ''.join(out)

    def fpu_reload(self, a, mode, st):
        if mode == 'none':
            return ''
        if mode == 'dynamic':
            return ' fp_top = c->ftop;' + (' fp_sw = c->fsw;' if self.uses_fsw else '')
        delta, ok = self.fcalls[a]
        s = ' fp_sw = c->fsw;' if self.uses_fsw else ''
        if delta is None:
            # The analysis says the callee never returns: continue in the dynamic copy.
            st['trans'] = True
            return s + ' fp_top = c->ftop; ' + self.dyn_continue(a, st['dirty'])
        d2 = st['dspill'] + delta
        expect = '(fp_top0 + %du) & 7u' % ((-d2) & 7)
        if ok:
            s += ' X87_CHECK(%s, %s);' % (expect, h(a))
        else:
            st['trans'] = True
            s += ' if (__builtin_expect(c->ftop != (%s), 0)) { fp_top = c->ftop; %s }' % (
                expect, self.dyn_continue(a, st['dirty']))
        for k in range(1, min(d2, 8) + 1):
            s += ' fr%d = c->st[(fp_top0 + %du) & 7u];' % (k, (-k) & 7)
            self.fregs_used.add(k)
        st['d'] = d2
        st['sync'] = d2
        return s

    def dyn_continue(self, a, dirty):
        """Jump from the static copy into the dynamic copy after the call at `a`."""
        self.need_dyn = True
        ins = self.fn.insns[a]
        nxt = a + ins.size
        if nxt in self.fn.insns:
            return 'goto D_%08X;' % nxt
        code = [self.tail_call(nxt)]
        return self.lower(a, code, 'dynamic', 'D', 0, dirty)[0][0]

    # --------------------------------------------------------------- emit
    def emit(self):
        self.compute_liveness()
        fn = self.fn
        items = []
        for idx, a in enumerate(self.order):
            ins = fn.insns[a]
            if a in self.fused:
                continue
            try:
                code = self.emit_insn(ins, idx)
            except Unsupported as e:
                code = ['SPILL; rt_unimplemented(c, %s, "%s"); return RETC;' % (h(a), str(e).replace('"', "'"))]
            nxt = a + ins.size
            falls = self.falls_through(ins)
            if a in self.fused_pairs:
                last = fn.insns[self.fused_pairs[a]]
                nxt = last.addr + last.size
                falls = self.falls_through(last)
            items.append((a, ins.text, code, falls, nxt))
        items = self.drop_dead_fsw(items)
        self.compute_dirty({a: code for a, _, code, _, _ in items})
        self.fregs_used = set()
        self.need_dyn = False
        if not self.has_x87():
            self.fmode = 'none'
        else:
            _, _, _, conflict = self.fpu_flow(self.g.fsumm, self.g.fproven)
            self.fmode = 'dynamic' if conflict or self.g.x87_force_dynamic else 'static'
            text = ' '.join(' '.join(code) for _, _, code, _, _ in items)
            self.uses_fsw = any(t in text for t in ('fp_sw', 'FCOM', 'FSYNC', 'XSPILL'))
            if self.fmode == 'static':
                self.compute_fsync()
        out = self.emit_copy(items, self.fmode, 'L')
        if self.need_dyn:
            out += self.emit_copy(items, 'dynamic', 'D')
        # Emit only the labels some goto refers to.
        text = '\n'.join(out)
        used = set(re.findall(r'goto ([LD]_[0-9A-F]{8});', text))
        return [line for line in out if not (line.endswith(':;') and line[:-2] not in used)]

    def backward_live(self, use, kill):
        """Backward may-liveness of one bit of state: live_out per instruction."""
        live_in = {a: False for a in self.order}
        live_out = {}
        changed = True
        while changed:
            changed = False
            for a in reversed(self.order):
                out = any(live_in[s] for s in self.succ[a])
                live_out[a] = out
                inn = use[a] or (out and not kill[a])
                if inn != live_in[a]:
                    live_in[a] = inn
                    changed = True
        return live_out

    def drop_dead_fsw(self, items):
        """Status word dead-code elimination on the emitted code.

        1. `fnstsw ax` whose result is overwritten before anything reads ax (the
           fcom/fnstsw/test/jcc idiom once the test branches on the compared values)
           is dropped.
        2. Compares update the status word lazily (see below): most of them only
           feed fused tests and are overwritten before anything reads the status
           word (fnstsw, fxam, a call, a return...)."""
        fn = self.fn
        code_of = {a: code for a, _, code, _, _ in items}
        ax_names = ('eax', 'ax', 'ah', 'al')
        use, kill = {}, {}
        for a in self.order:
            ins = fn.insns[a]
            text = ' '.join(code_of.get(a, ()))
            base = ins.mnem.split()[-1]
            if a in self.fcom_tests:
                rd, wr = [], []
            else:
                try:
                    rd, wr = ins.raw.regs_access()
                except Exception:           # noqa: BLE001 - unknown to capstone: assume a use
                    rd, wr = [X.X86_REG_EAX], []
                rd = [self.rname(r) for r in rd]
                wr = [self.rname(r) for r in wr]
                if base in ('xor', 'sub') and len(ins.ops) == 2 and all(o.type == X.X86_OP_REG for o in ins.ops) \
                        and ins.ops[0].reg == ins.ops[1].reg:
                    rd = []                     # zeroing idiom
            u = (any(r in ax_names for r in rd) or base in ('call', 'ret', 'retf', 'iretd', 'int', 'int3')
                 or bool(self.exit_targets(a)) or 'SPILL' in text or 'rt_unimplemented' in text)
            use[a] = u
            kill[a] = not u and ('eax' in wr or 'ax' in wr)
        ax_out = self.backward_live(use, kill)
        dropped = set()
        for a in self.order:
            ins = fn.insns[a]
            if ins.mnem == 'fnstsw' and ins.ops and ins.ops[0].type == X.X86_OP_REG \
                    and self.rname(ins.ops[0].reg) == 'ax' and not ax_out[a] and a in code_of:
                dropped.add(a)
        # Lazy condition codes: compares only record their operands (FCOMX), and the
        # status word is brought up to date (FSYNC) right before an instruction that
        # reads it, where the last status word update on every path is a compare
        # (fp_ca/fp_cb always hold the operands of the latest one). Where paths
        # disagree, a run-time flag says whether the update is pending (FCOML/FSYNCL).
        kind = {}
        for a in self.order:
            text = '' if a in dropped else ' '.join(code_of.get(a, ()))
            if any(k in text for k in ('fp_sw', 'rt_fstsw', 'SPILL', 'RELOAD', 'rt_unimplemented', 'return')) \
                    or self.exit_targets(a):
                kind[a] = 'use'
            elif 'FCOM(' in text:
                kind[a] = 'lazy'
        state = {fn.entry: 'E'}
        work = [fn.entry]
        while work:
            a = work.pop()
            k = kind.get(a)
            out = 'L' if k == 'lazy' else 'E' if k == 'use' else state[a]
            for s in self.succ[a]:
                old = state.get(s)
                new = out if old is None or old == out else 'M'
                if new != old:
                    state[s] = new
                    work.append(s)
        flag = any(kind.get(a) == 'use' and state.get(a) == 'M' for a in self.order)
        out = []
        for a, text, code, falls, nxt in items:
            k = kind.get(a)
            if a in dropped:
                code = []
            elif k == 'lazy':
                code = [line.replace('FCOM(', 'FCOML(' if flag else 'FCOMX(') for line in code]
            elif k == 'use' and code:
                if flag:
                    code = [line.replace('FCOM(', 'FCOME(') for line in code]
                if state.get(a) in ('L', 'M'):
                    code = [('FSYNCL; ' if flag else 'FSYNC; ') + code[0]] + list(code[1:])
            out.append((a, text, code, falls, nxt))
        return out

    def emit_copy(self, items, mode, prefix):
        fn = self.fn
        if mode == 'static':
            items = [it for it in items if it[0] in self.fdepth]
        out = []
        if prefix == 'L' and items and items[0][0] != fn.entry:
            # Blocks below the entry address (backward jumps) are emitted first.
            out.append('    goto L_%08X;' % fn.entry)
        for n, (a, text, code, falls, nxt) in enumerate(items):
            d = self.fdepth.get(a, 0) if mode == 'static' else 0
            code = list(code)
            following = items[n + 1][0] if n + 1 < len(items) else None
            if mode == 'static' and self.fcalls.get(a, (0,))[0] is None:
                falls = False                   # continues in the dynamic copy
            if falls and following != nxt:
                code.append(self.goto(nxt))
            lines, d2, trans = self.lower(a, code, mode, prefix, d, self.dirty.get(a, frozenset(REGS32)))
            out.append('%s_%08X:;' % (prefix, a))
            out.append('    /* %08X: %s */' % (a, text))
            for line in lines:
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
            out.append('uint8_t *ep = %s;' % self.mptr(d_op))
            av = 'ep'
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
        fcc = self.fcom_test(ins, idx) if is_test and cc in ('e', 'ne') else None
        if fcc is not None:
            # fcom ... fnstsw ax; test ah, K; je/jne: branch on the compared values.
            e = fcc if cc == 'e' else '!(%s)' % fcc
            if not self.live_out[n.addr]:
                self.fcom_tests.add(ins.addr)   # does not read ah
        elif is_test:
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

    # test ah, K after fnstsw ax: the condition "(C3,C2,C0 & K) == 0" in terms of the
    # compared values (C0: a < b, C3: a == b, all three set when unordered).
    FCOM_ZERO = {0x01: 'fp_ca >= fp_cb', 0x05: 'fp_ca >= fp_cb',
                 0x40: '(fp_ca < fp_cb || fp_ca > fp_cb)', 0x44: '(fp_ca < fp_cb || fp_ca > fp_cb)',
                 0x41: 'fp_ca > fp_cb', 0x45: 'fp_ca > fp_cb'}

    def fcom_test(self, ins, idx):
        """If `ins` is `test ah, K` reading the status word stored by `fnstsw ax` right
        before it, and the last x87 instruction before that (same basic block, no call
        in between) is a compare, return the C condition for "result is zero"."""
        d_op, s_op = ins.ops
        if d_op.type != X.X86_OP_REG or self.rname(d_op.reg) != 'ah' or s_op.type != X.X86_OP_IMM:
            return None
        cond = self.FCOM_ZERO.get(s_op.imm & 0xFF)
        if cond is None or idx < 2:
            return None
        fn = self.fn
        prev = fn.insns[self.order[idx - 1]]
        if prev.addr + prev.size != ins.addr or ins.addr in fn.block_starts or prev.mnem != 'fnstsw' \
                or not prev.ops or self.rname(prev.ops[0].reg) != 'ax':
            return None
        cur = prev
        for j in range(idx - 2, -1, -1):
            p = fn.insns[self.order[j]]
            if p.addr + p.size != cur.addr or cur.addr in fn.block_starts:
                return None
            if p.mnem == 'call':
                return None
            info = self.g.x87_info(p)
            if info is not None:
                code = ' '.join(self.emit_x87(p, bytes(p.raw.bytes)))
                if 'FCOM(' in code:
                    return cond
                # Loads, stores and arithmetic in between leave the status word
                # alone (the compared values were captured in fp_ca/fp_cb).
                if any(k in code for k in ('fp_sw', 'SPILL', 'RELOAD', 'rt_f')):
                    return None
            cur = p
        return None

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
            out.append('uint8_t *ep = %s;' % self.mptr(o))
            av = 'ep'
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
            out.append('uint8_t *ep = %s;' % self.mptr(o))
            av = 'ep'
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
            return ['{ uint8_t *ep = %s; %s }' % (self.mptr(o), self.wr(o, '~%s' % self.rd(o, size, 'ep'), size, 'ep'))]
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
            out.append('uint8_t *ep = %s;' % self.mptr(d_op))
            av = 'ep'
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
            out.append('uint8_t *ep = %s;' % self.mptr(d_op))
            av = 'ep'
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
                    'if (!v) { SPILL; rt_divide_error(c, %s); return RETC; } '
                    # 32-bit division when the dividend fits (no 64-bit libgcc call on ARM).
                    'if (edx == 0) { uint32_t q = eax / v; edx = eax %% v; eax = q; } '
                    'else { eax = (uint32_t)(n / v); edx = (uint32_t)(n %% v); } }' % (self.rd(o), h(ins.addr))]
        if o.size == 1:
            return ['{ uint32_t v = %s; uint32_t n = eax & 0xFFFFu; '
                    'if (!v) { SPILL; rt_divide_error(c, %s); return RETC; } '
                    'eax = (eax & 0xFFFF0000u) | ((n %% v) << 8) | ((n / v) & 0xFFu); }' % (self.rd(o), h(ins.addr))]
        raise Unsupported('div16')

    def i_idiv(self, ins):
        o = ins.ops[0]
        if o.size != 4:
            raise Unsupported('idiv8/16')
        return ['{ int32_t v = (int32_t)%s; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); '
                'if (!v) { SPILL; rt_divide_error(c, %s); return RETC; } '
                # 32-bit division when edx is the sign extension of eax (cdq; idiv).
                'if (edx == (uint32_t)((int32_t)eax >> 31) && v != -1) { int32_t a = (int32_t)eax; '
                'eax = (uint32_t)(a / v); edx = (uint32_t)(a %% v); } '
                'else { eax = (uint32_t)(int32_t)(n / v); edx = (uint32_t)(int32_t)(n %% v); } }' % (self.rd(o), h(ins.addr))]

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
            return ['{ uint8_t *ep = %s; uint32_t t = %s; %s %s }' % (
                self.mptr(b_), self.rd(a_), self.wr(a_, self.rd(b_, None, 'ep')), self.wr(b_, 't', None, 'ep'))]
        return ['{ uint32_t t = %s; %s %s }' % (self.rd(a_), self.wr(a_, self.rd(b_)), self.wr(b_, 't'))]

    def i_cdq(self, ins):
        return ['edx = (uint32_t)((int32_t)eax >> 31);']

    def i_cwde(self, ins):
        return ['eax = (uint32_t)(int32_t)(int16_t)(eax & 0xFFFFu);']

    def i_xlatb(self, ins):
        return [self.reg_write('al', 'GRD8((uint32_t)(ebx + (eax & 0xFFu)))')]

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
        return ['esp -= 4u; GWR32(esp, (uint32_t)(cf | (pf << 2) | (zf << 6) | (sf << 7) | (of << 11)) | (c->df << 10) | 0x202u | c->eflags_extra);']

    def i_popfd(self, ins):
        return ['{ uint32_t f = GRD32(esp); esp += 4u; cf = f & 1u; pf = (f >> 2) & 1u; zf = (f >> 6) & 1u; '
                'sf = (f >> 7) & 1u; of = (f >> 11) & 1u; c->df = (f >> 10) & 1u; c->eflags_extra = f & 0x00240000u; }']

    def i_setcc(self, ins, cc):
        return [self.wr(ins.ops[0], '(uint32_t)(%s)' % COND[cc][0], 1)]

    # ------------------------------------------------------------ stack
    def i_push(self, ins):
        o = ins.ops[0]
        if o.type == X.X86_OP_REG and self.rname(o.reg) not in REGS32:
            raise Unsupported('push %s' % self.rname(o.reg))
        if o.type == X.X86_OP_IMM:
            return ['esp -= 4u; GWR32(esp, %s);' % h(o.imm)]
        return ['{ uint32_t v = %s; esp -= 4u; GWR32(esp, v); }' % self.rd(o, 4)]

    def i_pop(self, ins):
        o = ins.ops[0]
        if o.type == X.X86_OP_REG:
            if self.rname(o.reg) not in REGS32:
                raise Unsupported('pop %s' % self.rname(o.reg))
            if self.rname(o.reg) == 'esp':
                return ['esp = GRD32(esp);']
            return ['%s = GRD32(esp); esp += 4u;' % self.rname(o.reg)]
        return ['{ uint32_t v = GRD32(esp); esp += 4u; %s }' % self.wr(o, 'v', 4)]

    def i_leave(self, ins):
        return ['esp = ebp; ebp = GRD32(esp); esp += 4u;']

    # ---------------------------------------------------------- control
    def i_jmp(self, ins):
        fn = self.fn
        a = ins.addr
        if a == LONGJMP_JUMP:
            # Registers are already restored from the jump buffer (edx points at it).
            return ['SPILL; rt_longjmp(c, edx);']
        t = self.an.direct_target(ins)
        if t is not None:
            if a in fn.tail_calls or t not in fn.insns:
                return [self.tail_call(t)]
            return [self.goto(t)]
        imp = self.an.import_of(ins)
        if imp:
            return ['SPILL; %s return RETC;' % self.import_call(ins, imp)]
        o = ins.ops[0]
        if a in fn.jump_tables:
            cases = ' '.join('case %s: %s' % (h(x), self.goto(x)) for x in fn.jump_tables[a])
            return ['{ uint32_t t = %s; switch (t) { %s default: SPILL; rt_dispatch(c, t); return RETC; } }' % (
                self.rd(o, 4), cases)]
        return ['{ uint32_t t = %s; CSPILL; %s return %s; }' % (self.rd(o, 4), INDIRECT_CACHE, INDIRECT_CALL)]

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
        if t == SETJMP_FUNC:
            return ['esp -= 4u; GWR32(esp, %s); SPILL;' % ret,
                    'if (setjmp(*rt_jmpbuf(GRD32(esp + 4u)))) { RELOAD; PROF_REENTER(%s); } ' % h(self.fn.entry) +
                    'else { uint64_t rv = f_%08X(c, esp, ecx, eax); CRELOAD; }' % t]
        if t is not None:
            if t in self.g.func_set:
                return ['esp -= 4u; GWR32(esp, %s); CSPILL; { uint64_t rv = f_%08X(c, esp, ecx, eax); CRELOAD; }' % (ret, t)]
            return ['esp -= 4u; GWR32(esp, %s); SPILL; rt_dispatch(c, %s); RELOAD;' % (ret, h(t))]
        imp = self.an.import_of(ins)
        if imp:
            call = 'esp -= 4u; GWR32(esp, %s); SPILL; %s' % (ret, self.import_call(ins, imp))
            if imp[1] in NORETURN_IMPORTS:
                return [call + ' rt_noreturn(c, %s); return RETC;' % h(ins.addr)]
            return [call + ' RELOAD;']
        # Indirect call: rt_call_indirect calls generated targets with the register
        # convention and host thunks through rt_dispatch.
        return ['{ uint32_t t = %s; esp -= 4u; GWR32(esp, %s); CSPILL; %s uint64_t rv = %s; CRELOAD; }' % (
            self.rd(ins.ops[0], 4), ret, INDIRECT_CACHE, INDIRECT_CALL)]

    def import_call(self, ins, imp):
        """Call of a host import; under IGI_PROFILE the profiler attributes the time to
        the import (identified by its IAT slot)."""
        slot = ins.ops[0].mem.disp & 0xFFFFFFFF
        return 'PROF_HOST(%s); %s(c); PROF_BACK(%s);' % (h(slot), self.g.import_fn(imp), h(self.fn.entry))

    def i_ret(self, ins):
        n = 4 + (ins.ops[0].imm if ins.ops else 0)
        return ['esp += %du; RSPILL; return RETV;' % n]

    def i_int3(self, ins):
        return ['SPILL; rt_unimplemented(c, %s, "int3"); return RETC;' % h(ins.addr)]

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
                return ['{ RtStr m = rt_rep_movs(gm, c->df, ecx, esi, edi, %du); ecx = m.cx; esi = m.si; edi = m.di; }' % size]
            return ['%s(edi, %s(esi)); esi += %s; edi += %s;' % (WR[size], RD[size], step, step)]
        if kind == 'stos':
            val = {1: '(eax & 0xFFu)', 2: '(eax & 0xFFFFu)', 4: 'eax'}[size]
            if rep:
                return ['{ RtStr m = rt_rep_stos(gm, c->df, ecx, edi, eax, %du); ecx = m.cx; edi = m.di; }' % size]
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
            loop = ('while (ecx != 0) { uint32_t d = %s; uint32_t s = %s; uint32_t r = %s; ecx -= 1u; %s %s '
                    'if (%s) break; }' % (d, s, r, adv, fl + ' (void)r;', stop))
            if rep == 'repne' and kind == 'scas' and size == 1:
                # Inline strlen idiom: search forward with a word-at-a-time scan; the
                # flags come from the last comparison, as in the byte loop.
                return ['{ RtScas m = { 0, 0, 0, 0 }; if (ecx != 0 && !c->df && (m = rt_scasb_fwd(gm, ecx, edi, '
                        'eax & 0xFFu)).ok) { ecx = m.cx; edi = m.di; { uint32_t d = eax & 0xFFu; uint32_t s = m.last; '
                        'uint32_t r = (d - s) & 0xFFu; %s (void)r; } } else { %s } }' % (fl, loop)]
            return [loop]
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
            ep = self.mptr(mop)
        arith = ['+', '*', None, None, '-', 'R-', '/', 'R/']

        def arith_expr(k, x, y):
            o = arith[k]
            if o.startswith('R'):
                return '%s %s %s' % (y, o[1], x)
            return '%s %s %s' % (x, o, y)

        if op in (0xD8, 0xDC, 0xDA, 0xDE) and mem:
            load = {0xD8: 'PRDF32(%s)', 0xDC: 'PRDF64(%s)', 0xDA: '(x87_t)(int32_t)PRD32(%s)',
                    0xDE: '(x87_t)(int16_t)PRD16(%s)'}[op] % ep
            if reg in (2, 3):
                pop = ' fpop(c);' if reg == 3 else ''
                return ['FCOM(ST(0), %s);%s' % (load, pop)]
            return ['{ x87_t m = %s; ST(0) = %s; }' % (load, arith_expr(reg, 'ST(0)', 'm'))]
        if op == 0xD8 and not mem:
            if reg in (2, 3):
                return ['FCOM(ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 3 else '')]
            return ['ST(0) = %s;' % arith_expr(reg, 'ST(0)', 'ST(%d)' % rm)]
        if op in (0xDC, 0xDE) and not mem:
            if op == 0xDE and modrm == 0xD9:
                return ['FCOM(ST(0), ST(1)); fpop(c); fpop(c);']
            if reg in (2, 3):
                return ['FCOM(ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 3 else '')]
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
                    return ['fpush(c, PRDF32(%s));' % ep]
                if reg == 2:
                    return ['PWRF32(%s, ST(0));' % ep]
                if reg == 3:
                    return ['PWRF32(%s, ST(0)); fpop(c);' % ep]
                if reg == 4:
                    return ['XSPILL; rt_fldenv(c, %s); XRELOAD;' % ea]
                if reg == 5:
                    return ['c->fcw = PRD16(%s);' % ep]
                if reg == 6:
                    return ['XSPILL; rt_fnstenv(c, %s); XRELOAD;' % ea]
                if reg == 7:
                    return ['PWR16(%s, c->fcw);' % ep]
                raise Unsupported('D9 /%d' % reg)
            if reg == 0:
                return ['{ x87_t v = ST(%d); fpush(c, v); }' % rm]
            if reg == 1:
                return ['{ x87_t v = ST(0); ST(0) = ST(%d); ST(%d) = v; }' % (rm, rm)]
            simple = {
                0xD0: '',
                0xE0: 'ST(0) = -ST(0);', 0xE1: 'ST(0) = fabs(ST(0));',
                0xE4: 'FCOM(ST(0), 0.0);', 0xE5: 'fp_sw = rt_fxam(fp_sw, ST(0));',
                0xE8: 'fpush(c, 1.0);', 0xE9: 'fpush(c, 3.321928094887362);',
                0xEA: 'fpush(c, 1.4426950408889634);', 0xEB: 'fpush(c, 3.141592653589793);',
                0xEC: 'fpush(c, 0.30102999566398120);', 0xED: 'fpush(c, 0.6931471805599453);',
                0xEE: 'fpush(c, 0.0);',
                0xF0: 'ST(0) = exp2(ST(0)) - 1.0;',
                0xF1: 'ST(1) = ST(1) * log2(ST(0)); fpop(c);',
                0xF2: 'ST(0) = tan(ST(0)); fpush(c, 1.0); fp_sw &= ~0x400u;',
                0xF3: 'ST(1) = atan2(ST(1), ST(0)); fpop(c);',
                0xF5: 'ST(0) = remainder(ST(0), ST(1)); fp_sw &= ~0x400u;',
                0xF8: 'ST(0) = fmod(ST(0), ST(1)); fp_sw &= ~0x400u;',
                0xFA: 'ST(0) = rt_fsqrt(ST(0));',
                0xFB: '{ x87_t v = ST(0); ST(0) = sin(v); fpush(c, cos(v)); fp_sw &= ~0x400u; }',
                0xFC: 'ST(0) = rt_fround(c, ST(0));',
                0xFD: 'ST(0) = rt_fscale(ST(0), ST(1));',
                0xFE: 'ST(0) = sin(ST(0)); fp_sw &= ~0x400u;',
                0xFF: 'ST(0) = cos(ST(0)); fp_sw &= ~0x400u;',
            }
            if modrm in simple:
                return [simple[modrm]] if simple[modrm] else []
            raise Unsupported('D9 %02X' % modrm)
        if op == 0xDD:
            if mem:
                if reg == 0:
                    return ['fpush(c, PRDF64(%s));' % ep]
                if reg == 2:
                    return ['PWRF64(%s, ST(0));' % ep]
                if reg == 3:
                    return ['PWRF64(%s, ST(0)); fpop(c);' % ep]
                if reg == 4:
                    return ['XSPILL; rt_frstor(c, %s); XRELOAD;' % ea]
                if reg == 6:
                    return ['XSPILL; rt_fnsave(c, %s); XRELOAD;' % ea]
                if reg == 7:
                    return ['PWR16(%s, rt_fstsw(fp_sw, FTOP));' % ep]
                raise Unsupported('DD /%d' % reg)
            if reg == 0:
                return []                               # ffree
            if reg == 2:
                return ['ST(%d) = ST(0);' % rm]
            if reg == 3:
                return ['ST(%d) = ST(0); fpop(c);' % rm]
            if reg in (4, 5):
                return ['FCOM(ST(0), ST(%d));%s' % (rm, ' fpop(c);' if reg == 5 else '')]
            raise Unsupported('DD %02X' % modrm)
        if op == 0xDB:
            if mem:
                if reg == 0:
                    return ['fpush(c, (x87_t)(int32_t)PRD32(%s));' % ep]
                if reg in (2, 3):
                    return ['PWR32(%s, (uint32_t)rt_fist32(c, ST(0)));%s' % (ep, ' fpop(c);' if reg == 3 else '')]
                if reg == 5:
                    return ['fpush(c, rt_ld80(%s));' % ea]
                if reg == 7:
                    return ['rt_st80(%s, ST(0)); fpop(c);' % ea]
                raise Unsupported('DB /%d' % reg)
            if modrm == 0xE2:
                return ['fp_sw &= 0x7F00u;']
            if modrm == 0xE3:
                return ['XSPILL; rt_fninit(c); XRELOAD;']
            raise Unsupported('DB %02X' % modrm)
        if op == 0xDF:
            if mem:
                if reg == 0:
                    return ['fpush(c, (x87_t)(int16_t)PRD16(%s));' % ep]
                if reg in (2, 3):
                    return ['PWR16(%s, (uint16_t)rt_fist16(c, ST(0)));%s' % (ep, ' fpop(c);' if reg == 3 else '')]
                if reg == 5:
                    return ['fpush(c, rt_fild64(PRD64(%s)));' % ep]
                if reg == 7:
                    return ['PWR64(%s, (uint64_t)rt_fist64(c, ST(0))); fpop(c);' % ep]
                raise Unsupported('DF /%d' % reg)
            if modrm == 0xE0:
                return [self.reg_write('ax', 'rt_fstsw(fp_sw, FTOP)')]
            raise Unsupported('DF %02X' % modrm)
        if op == 0xDA and modrm == 0xE9:
            return ['FCOM(ST(0), ST(1)); fpop(c); fpop(c);']
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
        self._x87 = {}
        self._x87_gen = None
        # IGI_X87_DYNAMIC=1 disables the static x87 stack allocation (for A/B checks).
        self.x87_force_dynamic = os.environ.get('IGI_X87_DYNAMIC') == '1'
        self.fsumm = {}
        self.fproven = {}
        self.hle = {}
        # IGI_HLE=0 keeps the guest code for every C runtime routine (for A/B checks).
        for a, (name, size, digest) in (HLE.items() if os.environ.get('IGI_HLE') != '0' else ()):
            if a in self.funcs and hashlib.sha1(self.an.pe.read(a, size)).hexdigest() == digest:
                self.hle[a] = name
            else:
                print('warning: %08X does not match %s; running the guest code' % (a, name))
        for a, (name, pieces) in (NATIVE.items() if os.environ.get('IGI_NATIVE') != '0' else ()):
            if a in self.funcs and all(hashlib.sha1(self.an.pe.read(p, n)).hexdigest() == d for p, n, d in pieces):
                self.hle[a] = name
            else:
                print('warning: %08X does not match %s; running the guest code' % (a, name))

    def x87_info(self, ins):
        """(stack delta, needs dynamic mode) for an x87 instruction, None otherwise."""
        a = ins.addr
        if a in self._x87:
            return self._x87[a]
        raw = bytes(ins.raw.bytes)
        res = None
        if raw[0] in (0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF):
            if self._x87_gen is None:
                self._x87_gen = FuncGen(self, Function(a))
            try:
                code = ' '.join(self._x87_gen.emit_x87(ins, raw))
                res = (code.count('fpush(c, ') - code.count('fpop(c);'), 'XSPILL' in code)
            except Unsupported:
                res = (0, True)
        self._x87[a] = res
        return res

    def fpu_summaries(self):
        """Interprocedural x87 stack effect of every function (least fixpoint).

        fsumm[f] is the stack depth change from entry to return (None: never returns,
        TOP: unknown). fproven[f] is true when that effect does not rest on any
        assumption about indirect or unknown calls, so callers may rely on it without
        a run-time check."""
        gens = {e: FuncGen(self, f) for e, f in self.funcs.items()}
        summ = {}
        callers = {}
        info = {}
        work = sorted(self.funcs, reverse=True)
        queued = set(work)
        while work:
            e = work.pop()
            queued.discard(e)
            s, unproven, callees, _ = gens[e].fpu_flow(summ)
            info[e] = (unproven, callees)
            for t in callees:
                callers.setdefault(t, set()).add(e)
            if e not in summ or s is not summ[e] and s != summ[e]:
                summ[e] = s
                for p in sorted(callers.get(e, ()), reverse=True):
                    if p not in queued:
                        queued.add(p)
                        work.append(p)
        proven = {e: not info[e][0] and summ[e] is not TOP for e in self.funcs}
        changed = True
        while changed:
            changed = False
            for e in sorted(self.funcs):
                if proven[e] and any(not proven.get(t, False) for t in info[e][1]):
                    proven[e] = False
                    changed = True
        self.fsumm = summ
        self.fproven = proven

    def import_fn(self, imp):
        dll, name = imp
        cname = 'imp_%s_%s' % (dll.split('.')[0], re.sub(r'[^A-Za-z0-9_]', '_', name))
        self.imports_used[cname] = imp
        return cname

    def run(self, out_dir):
        os.makedirs(out_dir, exist_ok=True)
        self.fpu_summaries()
        self.stats = {}
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
                    mode = fg.fmode + ('+dyn' if fg.need_dyn else '')
                    self.stats[mode] = self.stats.get(mode, 0) + 1
                    for line in body:
                        if 'rt_unimplemented' in line:
                            k = line.split('"')[1]
                            unsupported[k] = unsupported.get(k, 0) + 1
                    f.write(self.wrap(e, body, fg))
        self.write_header(out_dir)
        self.write_table(out_dir, entries)
        with open(os.path.join(out_dir, 'files.txt'), 'w') as f:
            f.write('\n'.join(files + ['dispatch_table.c', 'import_stubs.c']) + '\n')
        print('functions: %d, files: %d' % (len(entries), len(files)))
        print('x87 modes: %s' % ', '.join('%s %d' % kv for kv in sorted(self.stats.items())))
        for k, v in sorted(unsupported.items(), key=lambda x: -x[1]):
            print('  unsupported %-30s %d' % (k, v))

    def wrap(self, entry, body, fg):
        if entry in self.probes:
            # Diagnostic probe requested at configure time (IGI_PROBES).
            body = ['    c->r[0] = eax; c->r[1] = ecx; c->r[4] = esp; rt_probe(c, 0x%08Xu);' % entry] + body
        hle = self.hle.get(entry)
        if hle:
            body = ['    { uint64_t hr; if (%s(c, esp, ecx, eax, &hr)) { return hr; } }' % hle] + body
        # Profiler hooks (empty unless IGI_PROFILE): every exit restores the caller.
        body = [re.sub(r'\breturn\b', 'PROF_EXIT; return', line) for line in body]
        text = '\n'.join(body)
        out = [FUNC_PROTO % entry, '{'] + self.prologue(text, fg) + ['    PROF_ENTER(%s);' % h(entry), text, '}\n\n']
        if 'setjmp(' in text:
            # After a longjmp every register local is reloaded from the context and
            # the other locals read afterwards are never modified after setjmp, so
            # GCC's -Wclobbered reports are false positives here.
            out = ['#if defined(__GNUC__) && !defined(__clang__)', '#pragma GCC diagnostic push',
                   '#pragma GCC diagnostic ignored "-Wclobbered"', '#endif'] + out + [
                '#if defined(__GNUC__) && !defined(__clang__)', '#pragma GCC diagnostic pop', '#endif\n']
        return '\n'.join(out)

    @staticmethod
    def prologue(text, fg):
        """Local declarations of a generated function: guest registers, the guest memory
        base, used flags and the x87 state for the function's x87 mode."""
        out = ['    uint32_t edx = c->r[2], ebx = c->r[3], ebp = c->r[5], esi = c->r[6], edi = c->r[7];',
               '    uint8_t *const gm = g_mem;']
        used_flags = [n for _, n in FLAG_NAMES if re.search(r'\b%s\b' % n, text)]
        if used_flags:
            out.append('    int %s;' % ', '.join('%s = 0' % n for n in used_flags))
        # Registers are stored back only when modified, so some are never read.
        voids = REGS32 + ['gm']
        if fg.fmode == 'static':
            out.append('    const uint32_t fp_top0 = c->ftop;')
            voids.append('fp_top0')
        if fg.fmode == 'dynamic' or fg.need_dyn:
            out.append('    uint32_t fp_top = c->ftop;')
            voids.append('fp_top')
        if fg.fmode != 'none':
            out.append('    uint32_t fp_sw = c->fsw;')
            voids.append('fp_sw')
        if 'FCOM' in text or 'FSYNC' in text or 'fp_ca' in text:
            out.append('    x87_t fp_ca = 0.0, fp_cb = 0.0;')
            voids += ['fp_ca', 'fp_cb']
        if 'FCOML(' in text or 'FSYNCL' in text:
            out.append('    int fp_lz = 0;')
            voids.append('fp_lz')
        regs = sorted(fg.fregs_used)
        if regs:
            out.append('    x87_t %s;' % ', '.join('fr%d = 0.0' % k for k in regs))
            voids += ['fr%d' % k for k in regs]
        out.append('    %s' % ' '.join('(void)%s;' % v for v in voids))
        return out

    def write_header(self, out_dir):
        with open(os.path.join(out_dir, 'recomp_gen.h'), 'w') as f:
            f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n')
            f.write('#ifndef RECOMP_GEN_H\n#define RECOMP_GEN_H\n#include "recomp.h"\n\n')
            f.write('#define SPILL do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; '
                    'c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; } while (0)\n')
            f.write('#define RELOAD do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; '
                    'esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; } while (0)\n\n')
            for e in sorted(self.funcs):
                f.write('%s;\nvoid w_%08X(Ctx *c);\n' % (FUNC_PROTO % e, e))
            f.write('\n')
            for cname in sorted(self.imports_used):
                f.write('void %s(Ctx *c);\n' % cname)
            f.write('\n#endif\n')

    def write_table(self, out_dir, entries):
        an = self.an
        with open(os.path.join(out_dir, 'dispatch_table.c'), 'w') as f:
            f.write('/* Generated by tools/recomp/gen.py - do not edit. */\n#include "recomp_gen.h"\n\n')
            # Dispatch entry points: the context-only GuestFn signature.
            f.write('#define GUEST_WRAPPER(a) void w_##a(Ctx *c) { uint64_t r = f_##a(c, c->r[4], c->r[1], c->r[0]); '
                    'c->r[0] = (uint32_t)r; c->r[4] = (uint32_t)(r >> 32); }\n')
            for e in entries:
                f.write('GUEST_WRAPPER(%08X)\n' % e)
            f.write('\nconst GuestFunc g_guest_funcs[] = {\n')
            for e in entries:
                f.write('    { 0x%08Xu, w_%08X, f_%08X },\n' % (e, e, e))
            f.write('};\nconst uint32_t g_guest_func_count = %d;\n\n' % len(entries))
            # rt_dispatch hash table (GUEST_FUNC_HASH in recomp.h, linear probing, <= 50% load).
            size = 1
            while size < 2 * len(entries):
                size *= 2
            slots = [None] * size
            for e in entries:
                i = (((e * 0x9E3779B1) & 0xFFFFFFFF) >> 16) & (size - 1)
                while slots[i] is not None:
                    i = (i + 1) & (size - 1)
                slots[i] = e
            f.write('const GuestFunc g_guest_func_hash[] = {\n')
            for e in slots:
                f.write('    { 0x%08Xu, w_%08X, f_%08X },\n' % (e, e, e) if e is not None else '    { 0u, 0, 0 },\n')
            f.write('};\nconst uint32_t g_guest_func_hash_mask = 0x%Xu;\n\n' % (size - 1))
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
