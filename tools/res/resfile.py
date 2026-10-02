#!/usr/bin/env python3
"""Reader/writer for Project IGI ILFF resource containers (*.res).

Layout (all little endian):
  file header (20 bytes): "ILFF" u32 total_size u32 align u32 0 "IRES",
  zero padded to `align` (4, or 32 for some texture packs), then a chain of chunks, each with a 16-byte header:
      tag[4] u32 length u32 align u32 next
  `next` is the offset from this chunk's start to the next chunk (0 = last);
  normally 16 + length rounded up to `align`. The payload follows the header
  and is zero padded up to `next`.
Resource files are a sequence of NAME / <type> chunk pairs: NAME holds a
zero-terminated resource path, the following chunk (CSTR for text) its data.

Usage:
  resfile.py list FILE
  resfile.py extract FILE OUTDIR
  resfile.py repack FILE OUT          # parse + rewrite (round-trip check)
  resfile.py dumptext FILE            # NAME=text for CSTR resources
  resfile.py check FILE...            # round-trip every file, report diffs
"""
import os
import struct
import sys

ENCODING = "cp1252"


def _round_up(value, align):
    align = max(align, 1)
    return (value + align - 1) // align * align


class Chunk:
    def __init__(self, tag, data, align=4, pad=None):
        self.tag = tag
        self.data = data
        self.align = align
        self.pad = pad  # original padding bytes, kept for exact round trips


class ResFile:
    def __init__(self, chunks, form=b"IRES", align=4, reserved=0):
        self.chunks = chunks
        self.form = form
        self.align = align
        self.reserved = reserved

    @classmethod
    def parse(cls, blob):
        magic, total, align, reserved = struct.unpack_from("<4sIII", blob, 0)
        if magic != b"ILFF":
            raise ValueError("not an ILFF file")
        if total != len(blob):
            raise ValueError("size mismatch: header %d, file %d" % (total, len(blob)))
        form = blob[16:20]
        chunks = []
        pos = _round_up(20, align)
        while pos < len(blob):
            tag, length, calign, nxt = struct.unpack_from("<4sIII", blob, pos)
            data = blob[pos + 16:pos + 16 + length]
            end = pos + nxt if nxt else len(blob)
            pad = blob[pos + 16 + length:end]
            chunks.append(Chunk(tag, data, calign, pad))
            if not nxt:
                break
            pos += nxt
        return cls(chunks, form, align, reserved)

    def build(self):
        start = _round_up(20, self.align)
        out = bytearray(b"\0" * start)
        for i, c in enumerate(self.chunks):
            last = i == len(self.chunks) - 1
            end = len(out) + 16 + len(c.data)
            # chunk ends are aligned to absolute file offsets
            want = _round_up(end, c.align) - end
            if c.pad is not None and (last or len(c.pad) == want):
                pad = c.pad
            else:
                pad = b"\0" * want
            nxt = 0 if last else 16 + len(c.data) + len(pad)
            out += struct.pack("<4sIII", c.tag, len(c.data), c.align, nxt)
            out += c.data + pad
        out[0:20] = struct.pack("<4sIII", b"ILFF", len(out), self.align, self.reserved) + self.form
        return bytes(out)

    def resources(self):
        """Yield (name, chunk) pairs; name is None for an unnamed chunk."""
        it = iter(self.chunks)
        for c in it:
            if c.tag != b"NAME":
                yield None, c
                continue
            name = c.data.split(b"\0", 1)[0].decode(ENCODING)
            yield name, next(it)


def cstr(text):
    return text.encode(ENCODING) + b"\0"


def cstr_text(data):
    return data.split(b"\0", 1)[0].decode(ENCODING)


def read(path):
    with open(path, "rb") as f:
        return ResFile.parse(f.read())


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    if cmd == "check":
        bad = 0
        for p in argv[2:]:
            with open(p, "rb") as f:
                blob = f.read()
            try:
                ok = ResFile.parse(blob).build() == blob
            except ValueError as e:
                print("ERROR %s: %s" % (p, e))
                bad += 1
                continue
            if not ok:
                print("DIFF %s" % p)
                bad += 1
        print("%d files, %d failures" % (len(argv) - 2, bad))
        return 1 if bad else 0
    res = read(path)
    if cmd == "list":
        for name, c in res.resources():
            print("%-4s %7d  %s" % (c.tag.decode(ENCODING), len(c.data), name))
    elif cmd == "dumptext":
        for name, c in res.resources():
            if c.tag == b"CSTR":
                print("%s=%r" % (name, cstr_text(c.data)))
    elif cmd == "extract":
        outdir = argv[3]
        for i, (name, c) in enumerate(res.resources()):
            rel = (name or "chunk%04d" % i).replace(":", "_").replace("\\", "/")
            dst = os.path.join(outdir, rel + "." + c.tag.decode(ENCODING).strip().lower())
            os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
            with open(dst, "wb") as f:
                f.write(c.data)
    elif cmd == "repack":
        with open(argv[3], "wb") as f:
            f.write(res.build())
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
