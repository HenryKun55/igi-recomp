#!/usr/bin/env python3
"""Decoder/encoder for Project IGI compiled scripts (*.qvm, "LOOP" version 8).

File layout (little endian), all offsets from the start of the file:
  0x00 "LOOP"
  0x04 u32 version (8)       0x08 u32 minimum version (5)
  0x0C u32 identifier offset table      0x10 u32 identifier string data
  0x14 u32 identifier table size (bytes) 0x18 u32 identifier data size
  0x1C u32 string offset table          0x20 u32 string data
  0x24 u32 string table size (bytes)    0x28 u32 string data size
  0x2C u32 code offset                  0x30 u32 code size
  0x34 u32 stack size (0 = default)     0x38 u32 call stack size (0 = default)
Identifier and string tables hold offsets into their (zero-terminated,
cp1252) string data. The interpreter (0x4BCFD0 in IGI.exe) runs a byte code
whose opcode names come from the executable (0x4BECB0):

  BRK NOP PUSH PUSHB PUSHW PUSHF PUSHA PUSHS PUSHSI PUSHSIB PUSHSIW PUSHI
  PUSHII PUSHIIB PUSHIIW PUSH0 PUSH1 PUSHM POP RET BRA BF BT JSR CALL ADD SUB
  MUL DIV SHL SHR AND OR XOR LAND LOR EQ NE LT LE GT GE ASSIGN PLUS MINUS INV
  NOT BLK

The menu scripts only use a small declarative subset: every statement is a
call `Name(arg, ...)` followed by POP; the program ends with BRK. A call is PUSHII* <identifier>, then
CALL n <n code offsets>, BRA over the argument code, and the arguments, each
an expression ended by BRK (evaluated by the callee). Arguments are strings
(PUSHSI*), integers (PUSH0/PUSH1/PUSHB/PUSHW/PUSH, MINUS for negatives),
floats (PUSHF) or nested calls. PUSHB/PUSHW operands are signed.

This module decodes that subset into a tree, prints it as script source and
encodes it back; `check` verifies a byte-identical round trip.

Usage:
  qvm.py dump FILE            # script source
  qvm.py check FILE...        # decode + encode round trip
"""
import struct
import sys

ENCODING = "cp1252"
OPS = ("BRK NOP PUSH PUSHB PUSHW PUSHF PUSHA PUSHS PUSHSI PUSHSIB PUSHSIW PUSHI PUSHII PUSHIIB PUSHIIW PUSH0 "
       "PUSH1 PUSHM POP RET BRA BF BT JSR CALL ADD SUB MUL DIV SHL SHR AND OR XOR LAND LOR EQ NE LT LE GT GE "
       "ASSIGN PLUS MINUS INV NOT BLK").split()
OP = {name: i for i, name in enumerate(OPS)}
HEADER = struct.Struct("<4s14I")


class Call:
    def __init__(self, name, args):
        self.name = name
        self.args = args

    def __repr__(self):
        return "Call(%r, %r)" % (self.name, self.args)


class Float(float):
    pass


class Neg:
    """Unary minus applied to an operand (PUSH* x, MINUS)."""
    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return "Neg(%r)" % (self.value,)


class Str(str):
    pass


class Ident:
    """Bare identifier operand (e.g. TRUE)."""
    def __init__(self, name):
        self.name = name

    def __repr__(self):
        return "Ident(%r)" % (self.name,)


class Script:
    def __init__(self, statements, ids, strings, version=8, minver=5, stack=0, calls=0):
        self.statements = statements
        self.ids = ids            # original identifier table order
        self.strings = strings    # original string table order
        self.version = version
        self.minver = minver
        self.stack = stack
        self.calls = calls


def _table(blob, toff, doff, tsize):
    out = []
    for i in range(tsize // 4):
        o = struct.unpack_from("<I", blob, toff + 4 * i)[0]
        end = blob.index(b"\0", doff + o)
        out.append(blob[doff + o:end].decode(ENCODING))
    return out


class _Decoder:
    def __init__(self, code, ids, strings):
        self.code = code
        self.ids = ids
        self.strings = strings

    def u(self, pc, n):
        return int.from_bytes(self.code[pc:pc + n], "little"), pc + n

    def expr(self, pc):
        """Decode one expression at pc; return (node, pc after it)."""
        op = self.code[pc]
        pc += 1
        name = OPS[op] if op < len(OPS) else "?"
        if name in ("PUSHIIB", "PUSHIIW", "PUSHII"):
            idx, pc = self.u(pc, {"PUSHIIB": 1, "PUSHIIW": 2, "PUSHII": 4}[name])
            if self.code[pc] != OP["CALL"]:
                return Ident(self.ids[idx]), pc
            n, pc = self.u(pc + 1, 4)
            offs = struct.unpack_from("<%dI" % n, self.code, pc)
            pc += 4 * n
            if self.code[pc] != OP["BRA"]:
                raise ValueError("call without BRA at %#x" % pc)
            skip, pc = self.u(pc + 1, 4)
            end = pc + skip
            args = []
            for o in offs:
                if o != pc:
                    raise ValueError("unexpected argument layout at %#x" % pc)
                node, pc = self.operand(pc)
                if self.code[pc] != OP["BRK"]:
                    raise ValueError("argument without BRK at %#x" % pc)
                pc += 1
                args.append(node)
            if pc != end:
                raise ValueError("argument code size mismatch at %#x" % pc)
            return Call(self.ids[idx], args), pc
        if name in ("PUSHSIB", "PUSHSIW", "PUSHSI"):
            idx, pc = self.u(pc, {"PUSHSIB": 1, "PUSHSIW": 2, "PUSHSI": 4}[name])
            return Str(self.strings[idx]), pc
        if name == "PUSH0":
            return 0, pc
        if name == "PUSH1":
            return 1, pc
        if name in ("PUSHB", "PUSHW", "PUSH"):
            n = {"PUSHB": 1, "PUSHW": 2, "PUSH": 4}[name]
            return int.from_bytes(self.code[pc:pc + n], "little", signed=True), pc + n
        if name == "PUSHF":
            return Float(struct.unpack_from("<f", self.code, pc)[0]), pc + 4
        raise ValueError("unsupported opcode %s at %#x" % (name, pc - 1))

    def operand(self, pc):
        """Expression possibly followed by unary MINUS."""
        node, pc = self.expr(pc)
        while pc < len(self.code) and self.code[pc] == OP["MINUS"]:
            node = Neg(node)
            pc += 1
        return node, pc


def parse(blob):
    (magic, version, minver, idt, idd, idts, idds, st, sd, sts, sds, co, cs, stack, calls) = HEADER.unpack_from(blob)
    if magic != b"LOOP" or version != 8:
        raise ValueError("not a version 8 QVM")
    ids = _table(blob, idt, idd, idts)
    strings = _table(blob, st, sd, sts)
    code = blob[co:co + cs]
    dec = _Decoder(code, ids, strings)
    statements = []
    pc = 0
    while pc < len(code):
        if code[pc] == OP["BRK"] and pc == len(code) - 1:
            break   # the program ends with BRK
        node, pc = dec.operand(pc)
        if code[pc] != OP["POP"]:
            raise ValueError("statement without POP at %#x" % pc)
        pc += 1
        statements.append(node)
    return Script(statements, ids, strings, version, minver, stack, calls)


class _Encoder:
    def __init__(self, ids, strings):
        self.ids = list(ids)
        self.strings = list(strings)
        self.id_index = {s: i for i, s in reversed(list(enumerate(self.ids)))}
        self.str_index = {s: i for i, s in reversed(list(enumerate(self.strings)))}

    def intern(self, table, index, s):
        if s not in index:
            index[s] = len(table)
            table.append(s)
        return index[s]

    @staticmethod
    def sized(ops, v, signed=False):
        if signed:
            if v < 0x80:
                return bytes([OP[ops[0]], v])
            if v < 0x8000:
                return bytes([OP[ops[1]]]) + struct.pack("<h", v)
            return bytes([OP[ops[2]]]) + struct.pack("<i", v)
        if v < 0x100:
            return bytes([OP[ops[0]], v])
        if v < 0x10000:
            return bytes([OP[ops[1]]]) + struct.pack("<H", v)
        return bytes([OP[ops[2]]]) + struct.pack("<I", v)

    def expr(self, node, base):
        """Encode node whose code starts at code offset `base`."""
        if isinstance(node, Call):
            head = self.sized(("PUSHIIB", "PUSHIIW", "PUSHII"), self.intern(self.ids, self.id_index, node.name))
            n = len(node.args)
            args_start = base + len(head) + 5 + 4 * n + 5
            body = bytearray()
            offs = []
            for a in node.args:
                offs.append(args_start + len(body))
                body += self.expr(a, args_start + len(body)) + bytes([OP["BRK"]])
            out = bytearray(head)
            out += bytes([OP["CALL"]]) + struct.pack("<I%dI" % n, n, *offs)
            out += bytes([OP["BRA"]]) + struct.pack("<I", len(body))
            return bytes(out + body)
        if isinstance(node, Ident):
            return self.sized(("PUSHIIB", "PUSHIIW", "PUSHII"), self.intern(self.ids, self.id_index, node.name))
        if isinstance(node, Neg):
            return self.expr(node.value, base) + bytes([OP["MINUS"]])
        if isinstance(node, Float):
            return bytes([OP["PUSHF"]]) + struct.pack("<f", node)
        if isinstance(node, str):
            return self.sized(("PUSHSIB", "PUSHSIW", "PUSHSI"), self.intern(self.strings, self.str_index, node))
        if isinstance(node, int):
            if node < 0:
                return self.expr(Neg(-node), base)
            if node == 0:
                return bytes([OP["PUSH0"]])
            if node == 1:
                return bytes([OP["PUSH1"]])
            return self.sized(("PUSHB", "PUSHW", "PUSH"), node, signed=True)
        raise TypeError("cannot encode %r" % (node,))


def _pack_table(strings):
    offs = bytearray()
    data = bytearray()
    for s in strings:
        offs += struct.pack("<I", len(data))
        data += s.encode(ENCODING) + b"\0"
    return bytes(offs), bytes(data)


def build(script, keep_tables=True):
    enc = _Encoder(script.ids if keep_tables else [], script.strings if keep_tables else [])
    code = bytearray()
    for st in script.statements:
        code += enc.expr(st, len(code)) + bytes([OP["POP"]])
    code.append(OP["BRK"])
    idt, idd = _pack_table(enc.ids)
    st, sd = _pack_table(enc.strings)
    pos = HEADER.size
    layout = []
    for part in (idt, idd, st, sd, code):
        layout.append(pos)
        pos += len(part)
    hdr = HEADER.pack(b"LOOP", script.version, script.minver, layout[0], layout[1], len(idt), len(idd),
                      layout[2], layout[3], len(st), len(sd), layout[4], len(code), script.stack, script.calls)
    return hdr + idt + idd + st + sd + bytes(code)


def read(path):
    with open(path, "rb") as f:
        return parse(f.read())


def _src(node, indent=0):
    pad = "   " * indent
    if isinstance(node, Call):
        if not node.args:
            return pad + node.name + "()"
        simple = all(not isinstance(a, Call) for a in node.args)
        if simple:
            return pad + node.name + "(" + ", ".join(_src(a) for a in node.args) + ")"
        inner = ",\n".join(_src(a, indent + 1) for a in node.args)
        return pad + node.name + "(\n" + inner + ")"
    if isinstance(node, str):
        return pad + '"' + node.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
    if isinstance(node, Neg):
        return pad + "-" + _src(node.value)
    if isinstance(node, Ident):
        return pad + node.name
    return pad + repr(node)


def dump(script):
    return "".join(_src(s) + ";\n" for s in script.statements)


def walk(node):
    """Yield every call in a tree (pre-order)."""
    if isinstance(node, Call):
        yield node
        for a in node.args:
            yield from walk(a)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    if argv[1] == "dump":
        sys.stdout.write(dump(read(argv[2])))
        return 0
    if argv[1] == "check":
        bad = 0
        for p in argv[2:]:
            with open(p, "rb") as f:
                blob = f.read()
            try:
                ok = build(parse(blob)) == blob
            except (ValueError, TypeError) as e:
                print("ERROR %s: %s" % (p, e))
                bad += 1
                continue
            if not ok:
                print("DIFF %s" % p)
                bad += 1
        print("%d files, %d failures" % (len(argv) - 2, bad))
        return 1 if bad else 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
