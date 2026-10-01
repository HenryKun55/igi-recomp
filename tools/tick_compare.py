"""Compares update-rate A/B runs (tools/tick_ab.sh, docs/tick_rate.md).

usage: tick_compare.py trace A_trace.txt B_trace.txt [step]
           Watched values of two runs at equal game time (every `step` seconds,
           default 0.5) and, per value, the largest difference over the run.
       tick_compare.py objects A_snap_T.bin B_snap_T.bin [CLASS]
           Game objects (the task tree) of two snapshots taken at the same game
           time: per class, how many objects and which fields differ (with
           CLASS: the differing fields of each object of that class).
"""
import collections
import struct
import sys

CLOCK_OBJ = 0x567C8C
TASK_TYPES = 0xAF5EE0


def load_trace(path):
    rows = [line.split() for line in open(path)]
    names = rows[0][2:]
    data = {}
    for r in rows[1:]:
        data[round(float(r[0]), 4)] = r[2:]
    return names, data


def num(x):
    try:
        return float(x)
    except ValueError:
        return None


def cmd_trace(a_path, b_path, step=0.5):
    names, a = load_trace(a_path)
    _, b = load_trace(b_path)
    common = sorted(set(a) & set(b))
    worst = [0.0] * len(names)
    print('%7s ' % 't' + ' '.join('%24s' % n for n in names))
    next_t = 0.0
    for t in common:
        for i, (x, y) in enumerate(zip(a[t], b[t])):
            fx, fy = num(x), num(y)
            if fx is not None and fy is not None:
                worst[i] = max(worst[i], abs(fy - fx))
        if t + 1e-6 >= next_t:
            cells = []
            for x, y in zip(a[t], b[t]):
                fx, fy = num(x), num(y)
                cells.append('%12.6g %+11.4g' % (fx, fy - fx) if fx is not None and fy is not None else '%24s' % (x + '/' + y))
            print('%7.3f ' % t + ' '.join(cells))
            next_t += step
    print('max |B-A|: ' + ', '.join('%s %.6g' % (n, w) for n, w in zip(names, worst)))


class Snapshot:
    def __init__(self, path):
        self.pages = {}
        with open(path, 'rb') as f:
            while True:
                h = f.read(8)
                if len(h) < 8:
                    break
                a, n = struct.unpack('<II', h)
                self.pages[a] = f.read(n)

    def u32(self, a):
        p = self.pages.get(a & ~0xFFF)
        return struct.unpack_from('<I', p, a & 0xFFF)[0] if p else 0

    def u8(self, a):
        p = self.pages.get(a & ~0xFFF)
        return p[a & 0xFFF] if p else 0

    def cstr(self, a):
        s = bytearray()
        while a and len(s) < 64 and self.u8(a):
            s.append(self.u8(a))
            a += 1
        return s.decode('latin1')

    def types(self):
        names, sizes = {}, {}
        for t in range(0x180):
            e = TASK_TYPES + t * 0x30
            if self.u8(e + 0x25):
                names[t] = self.cstr(self.u32(e))
                sizes[t] = self.u32(e + 4)
        return names, sizes

    def nodes(self):
        """Every task reachable from the game clock's lists: {address: type}."""
        out, seen = {}, set()

        def walk(node):
            n = self.u32(node + 8)
            while n and self.u32(n) and n not in seen:
                seen.add(n)
                out[n] = self.u32(n + 0x1C) & 0xFFFF
                walk(n)
                n = self.u32(n)

        flow = self.u32(CLOCK_OBJ)
        for root in (flow, self.u32(flow + 0x48)):
            out[root] = self.u32(root + 0x1C) & 0xFFFF
            walk(root)
        return out


def cmd_objects(a_path, b_path, detail=None):
    a, b = Snapshot(a_path), Snapshot(b_path)
    names, sizes = a.types()
    na, nb = a.nodes(), b.nodes()
    print('objects: A %d, B %d, common %d' % (len(na), len(nb), len(set(na) & set(nb))))
    per = collections.defaultdict(list)
    for n in sorted(set(na) & set(nb)):
        t = na[n]
        if nb[n] != t:
            continue
        diffs = [o for o in range(0x20, sizes.get(t, 0x20), 4) if a.u32(n + o) != b.u32(n + o)]
        cls = names.get(t, '%x' % t)
        per[cls].append((n, diffs))
        if detail == cls and diffs:
            print('%08x' % n)
            for o in diffs:
                x, y = a.u32(n + o), b.u32(n + o)
                fx, fy = struct.unpack('<ff', struct.pack('<II', x, y))
                print('   +%04x %08x %08x  %-13.6g %-13.6g' % (o, x, y, fx, fy))
    for cls, objs in sorted(per.items()):
        nd = sum(1 for _, d in objs if d)
        if nd:
            offs = collections.Counter(o for _, d in objs for o in d)
            print('%-28s %4d/%-4d differ: %s' % (cls, nd, len(objs),
                                                 ' '.join('%x(%d)' % kv for kv in sorted(offs.items())[:24])))


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'trace':
        cmd_trace(sys.argv[2], sys.argv[3], float(sys.argv[4]) if len(sys.argv) > 4 else 0.5)
    elif len(sys.argv) >= 4 and sys.argv[1] == 'objects':
        cmd_objects(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
    else:
        print(__doc__)
        sys.exit(1)
