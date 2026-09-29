"""Control-flow recovery for IGI.exe.

Finds function entry points (direct call targets, code pointers stored in data or
used as immediates), decodes each function by recursive descent, and resolves
MSVC switch jump tables. The result drives the C code generator.
"""
import struct

import capstone
from capstone import x86 as X

from pe import PE

JCC = {
    'jo', 'jno', 'jb', 'jae', 'je', 'jne', 'jbe', 'ja', 'js', 'jns', 'jp', 'jnp',
    'jl', 'jge', 'jle', 'jg', 'jecxz', 'loop', 'loope', 'loopne',
}


class Insn:
    __slots__ = ('addr', 'size', 'mnem', 'ops', 'prefix', 'raw', 'text')

    def __init__(self, ci):
        self.addr = ci.address
        self.size = ci.size
        self.mnem = ci.mnemonic
        self.ops = list(ci.operands)
        self.prefix = list(ci.prefix)
        self.raw = ci
        self.text = '%s %s' % (ci.mnemonic, ci.op_str)


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = {}          # addr -> Insn
        self.block_starts = {entry}
        self.jump_tables = {}    # jmp insn addr -> [targets]
        self.tail_calls = set()  # addrs of jmp insns leaving the function
        self.bad = None          # reason if decoding failed

    def end(self):
        last = max(self.insns)
        return last + self.insns[last].size


class Analyzer:
    def __init__(self, path):
        self.pe = PE(path)
        self.text = self.pe.section('.text')
        self.text_lo = self.text.va
        self.text_hi = self.text.va + self.text.vsize
        self.code = self.pe.read(self.text_lo, self.text.vsize)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self._cache = {}
        self.noreturn_imports = {'ExitProcess', 'TerminateProcess', 'RaiseException'}
        self.noreturn = set()

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def decode(self, a):
        if a in self._cache:
            return self._cache[a]
        off = a - self.text_lo
        ci = next(self.md.disasm(self.code[off:off + 16], a), None)
        ins = Insn(ci) if ci else None
        self._cache[a] = ins
        return ins

    # ------------------------------------------------------------------ helpers
    def direct_target(self, ins):
        op = ins.ops[0] if ins.ops else None
        if op is not None and op.type == X.X86_OP_IMM:
            return op.imm & 0xFFFFFFFF
        return None

    def import_of(self, ins):
        """If ins is `call/jmp [iat_slot]`, return (dll, name)."""
        if not ins.ops:
            return None
        op = ins.ops[0]
        if op.type == X.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
            return self.pe.imports.get(op.mem.disp & 0xFFFFFFFF)
        return None

    def find_jump_table(self, fn, ins):
        """`jmp [reg*4 + table]`: return list of targets or None."""
        op = ins.ops[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        count = self._table_bound(fn, ins, op.mem.index)
        targets = []
        a = table
        limit = count if count is not None else 1024
        for _ in range(limit):
            v = self.pe.u32(a)
            a += 4
            if v is None or not self.in_text(v):
                if count is None:
                    break
                continue    # unreachable slot (e.g. memcpy's case 0)
            if count is None and targets and a - 4 in fn.insns:
                break
            targets.append(v)
        return sorted(set(targets)) or None

    def _table_bound(self, fn, jmp, reg):
        """Walk back a few instructions for `cmp reg, N` + `ja` to size the table.

        Also handles the two-level form `movzx reg, byte [reg2 + idx_table]`, where the
        table size is (max byte in the index table) + 1.
        """
        back = self._preceding(fn, jmp.addr, 6)
        for p in back:
            if p.mnem == 'movzx' and p.ops[0].type == X.X86_OP_REG and p.ops[0].reg == reg \
                    and p.ops[1].type == X.X86_OP_MEM and p.ops[1].size == 1:
                idx_table = p.ops[1].mem.disp & 0xFFFFFFFF
                n = self._table_bound(fn, p, p.ops[1].mem.index)
                if n is None:
                    return None
                return max(self.pe.read(idx_table, n)) + 1
            if p.mnem == 'cmp' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_IMM:
                if self._same_reg(p.ops[0].reg, reg):
                    return (p.ops[1].imm & 0xFFFFFFFF) + 1
            if p.mnem == 'and' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_IMM:
                if self._same_reg(p.ops[0].reg, reg):
                    mask = p.ops[1].imm & 0xFFFFFFFF
                    if mask < 0x1000:
                        return mask + 1
            if p.mnem == 'mov' and p.ops[0].type == X.X86_OP_REG and p.ops[1].type == X.X86_OP_REG \
                    and p.ops[0].reg == reg:
                reg = p.ops[1].reg
        return None

    def _same_reg(self, a, b):
        return self.md.reg_name(a)[-2:] == self.md.reg_name(b)[-2:]

    def _preceding(self, fn, addr, n):
        """Instructions before addr in straight-line order (best effort)."""
        out = []
        cands = sorted((a for a in fn.insns if a < addr), reverse=True)
        cur = addr
        for a in cands:
            if a + fn.insns[a].size != cur:
                break
            out.append(fn.insns[a])
            cur = a
            if len(out) >= n:
                break
        return out

    # --------------------------------------------------------------- discovery
    def analyze_function(self, entry, known_entries):
        fn = Function(entry)
        work = [entry]
        seen = set()
        while work:
            a = work.pop()
            if a in seen:
                continue
            seen.add(a)
            while True:
                if not self.in_text(a):
                    fn.bad = 'left .text at %08x' % a
                    return fn
                ins = self.decode(a)
                if ins is None:
                    fn.bad = 'undecodable at %08x' % a
                    return fn
                if a in fn.insns:
                    break
                if a != entry and a in known_entries and a not in fn.block_starts:
                    # Falling into another function: treat as tail call.
                    fn.tail_calls.add(a)
                    break
                fn.insns[a] = ins
                m = ins.mnem
                nxt = a + ins.size
                if m in ('ret', 'retf', 'iretd'):
                    break
                if m == 'int3' or m == 'hlt':
                    break
                if m in JCC:
                    t = self.direct_target(ins)
                    fn.block_starts.update((t, nxt))
                    work.append(t)
                    a = nxt
                    continue
                if m == 'jmp':
                    t = self.direct_target(ins)
                    if t is not None:
                        if t in known_entries and t != entry:
                            fn.tail_calls.add(a)
                        else:
                            fn.block_starts.add(t)
                            work.append(t)
                        break
                    if self.import_of(ins):
                        fn.tail_calls.add(a)
                        break
                    tab = self.find_jump_table(fn, ins)
                    if tab:
                        fn.jump_tables[a] = tab
                        fn.block_starts.update(tab)
                        work.extend(tab)
                    else:
                        fn.tail_calls.add(a)   # computed jump: dispatched at runtime
                    break
                if m == 'call':
                    imp = self.import_of(ins)
                    if imp and imp[1] in self.noreturn_imports:
                        break
                    t = self.direct_target(ins)
                    if t is not None and t in self.noreturn:
                        break
                a = nxt
        return fn

    def code_pointers_in_data(self):
        """Aligned dwords in .rdata/.data that point into .text."""
        ptrs = set()
        for s in self.pe.sections:
            if s.name not in ('.rdata', '.data'):
                continue
            raw = self.pe.data[s.raw_off:s.raw_off + s.raw_size]
            for off in range(0, len(raw) - 3, 4):
                v = struct.unpack_from('<I', raw, off)[0]
                if self.in_text(v):
                    ptrs.add(v)
        return ptrs

    def run(self):
        banned = set()
        while True:
            funcs, entries = self._run_once(banned)
            bad = {e for e, f in funcs.items() if f.bad and e != self.pe.entry}
            if not bad:
                break
            banned |= bad
        self.alt_entries = self._alt_entries(funcs)
        for a in sorted(self.alt_entries):
            funcs[a] = self.analyze_function(a, entries)
        return funcs, entries

    def _alt_entries(self, funcs):
        """Data-referenced labels inside functions (SEH handlers, etc.): they get their
        own C entry so runtime dispatch can reach them."""
        inner = {}
        for fn in funcs.values():
            for a in fn.insns:
                if a != fn.entry:
                    inner[a] = fn
        table_targets = {t for f in funcs.values() for tab in f.jump_tables.values() for t in tab}
        return {p for p in self.code_pointers_in_data()
                if p in inner and p not in funcs and p not in table_targets}

    def _run_once(self, banned):
        entries = {self.pe.entry}
        data_ptrs = self.code_pointers_in_data()
        funcs = {}
        imm_ptrs = set()
        work = [self.pe.entry]
        from_data = set()
        # Pass 1: direct call graph from the entry point.
        while work:
            e = work.pop()
            if e in funcs:
                continue
            fn = self.analyze_function(e, entries)
            if not fn.bad and e in from_data and self.looks_like_data(fn):
                fn.bad = 'looks like data'
            funcs[e] = fn
            for ins in fn.insns.values():
                if ins.mnem == 'call':
                    t = self.direct_target(ins)
                    if t is not None and self.in_text(t) and t not in entries:
                        entries.add(t)
                        work.append(t)
                for op in ins.ops:
                    if op.type == X.X86_OP_IMM and ins.mnem not in JCC and ins.mnem not in ('call', 'jmp'):
                        v = op.imm & 0xFFFFFFFF
                        if self.in_text(v):
                            imm_ptrs.add(v)
            if not work:
                # Pass 2: pointers taken as immediates, then pointers stored in data.
                for p in sorted(imm_ptrs | data_ptrs):
                    if p in entries or p in funcs or p in banned:
                        continue
                    if not self._plausible_entry(p, funcs):
                        continue
                    entries.add(p)
                    work.append(p)
                    from_data.add(p)
                imm_ptrs = set()
                data_ptrs = set()
        return funcs, entries

    SUSPICIOUS = {'in', 'out', 'insb', 'insd', 'outsb', 'outsd', 'les', 'lds', 'lcall', 'ljmp', 'arpl',
                  'hlt', 'cli', 'sti', 'into', 'iretd', 'retf', 'bound', 'daa', 'das', 'aaa', 'aas', 'aam',
                  'aad', 'salc', 'sldt', 'wbinvd', 'int1', 'enter'}

    def looks_like_data(self, fn):
        for ins in fn.insns.values():
            if ins.mnem in self.SUSPICIOUS:
                return True
            if ins.mnem == 'int' or ins.raw.bytes[:2] == b'\0\0':
                return True
            if ins.mnem in ('push', 'pop') and ins.ops and ins.ops[0].type == X.X86_OP_REG \
                    and self.md.reg_name(ins.ops[0].reg) in ('cs', 'ds', 'es', 'ss', 'fs', 'gs'):
                return True
            if ins.mnem == 'mov' and any(o.type == X.X86_OP_REG and self.md.reg_name(o.reg) in
                                         ('cs', 'ds', 'es', 'ss', 'fs', 'gs') for o in ins.ops):
                return True
            if ins.mnem.startswith('fcmov') or ins.mnem == 'fstpnce':
                return True
        return False

    def _plausible_entry(self, a, funcs):
        # Pointers that land inside an already decoded instruction stream are
        # jump-table targets or mid-function labels, not entries.
        for fn in funcs.values():
            if a in fn.insns and a != fn.entry:
                return False
        ins = self.decode(a)
        return ins is not None and ins.mnem not in ('int3', 'nop')


def main():
    import sys
    an = Analyzer(sys.argv[1])
    funcs, entries = an.run()
    bad = [f for f in funcs.values() if f.bad]
    covered = set()
    for f in funcs.values():
        if f.bad:
            continue
        for a, ins in f.insns.items():
            covered.update(range(a, a + ins.size))
    print('functions:', len(funcs), 'bad:', len(bad))
    print('text bytes covered: %d / %d (%.1f%%)' % (len(covered), an.text.vsize, 100.0 * len(covered) / an.text.vsize))
    print('jump tables:', sum(len(f.jump_tables) for f in funcs.values()))
    print('computed jumps:', sum(1 for f in funcs.values() for a in f.tail_calls
                                 if f.insns.get(a) and f.insns[a].mnem == 'jmp'
                                 and an.direct_target(f.insns[a]) is None and not an.import_of(f.insns[a])))
    for f in bad[:40]:
        print('  bad %08x: %s' % (f.entry, f.bad))


if __name__ == '__main__':
    main()
