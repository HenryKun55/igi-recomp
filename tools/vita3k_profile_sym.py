#!/usr/bin/env python3
"""Symbolizes a Vita3K guest profile (VITA3K_PROFILE lines "thread pc lr weight"; the
difference of two snapshots) against the game ELF: weight per function, and the HLE
import calls (their billed cost sits at the import stub) with the calling function.
Vita3K keeps one lr per pc, so an import's caller is the last one seen.
usage: vita3k_profile_sym.py elf start.txt end.txt [top] [fn,fn,...: their callers]"""
import bisect, collections, os, subprocess, sys

elf, fa, fb = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
nm_tool = os.path.join(os.environ.get('VITASDK', os.path.expanduser('~/vitasdk')), 'bin', 'arm-vita-eabi-nm')
nm = subprocess.run([nm_tool, '-n', '--defined-only', elf],
                    capture_output=True, text=True).stdout
syms = []
for line in nm.splitlines():
    p = line.split()
    if len(p) >= 3 and p[1] in 'tTwW':
        syms.append((int(p[0], 16) & ~1, p[2]))
syms.sort()
addrs = [a for a, _ in syms]


def sym(a):
    i = bisect.bisect_right(addrs, a & ~1) - 1
    return syms[i][1] if i >= 0 else '?%x' % a


def load(f):
    d = {}
    try:
        for line in open(f):
            p = line.split()
            if len(p) == 4:
                d[(int(p[0]), int(p[1], 16))] = (int(p[2], 16), int(p[3]))
    except FileNotFoundError:
        pass
    return d


a, b = load(fa), load(fb)
by_fn = collections.Counter()
hle = collections.Counter()
total = 0
for k, (lr, w) in b.items():
    w0 = a.get(k, (0, 0))[1]
    dw = w - w0
    if dw <= 0:
        continue
    total += dw
    s = sym(k[1])
    by_fn[s] += dw
    if not s.startswith('f_') and ('_stub' in s or s.startswith('sce') or s.startswith('_sce')):
        hle[s + ' <- ' + sym(lr)] += dw
print('total weight', total)
for s, w in by_fn.most_common(top):
    print('%6.2f%% %s' % (100.0 * w / total, s))
# Per guest thread (the game, the render thread, audio...): share and top functions.
by_thread = collections.Counter()
thread_fn = collections.defaultdict(collections.Counter)
for k, (lr, w) in b.items():
    dw = w - a.get(k, (0, 0))[1]
    if dw > 0:
        by_thread[k[0]] += dw
        thread_fn[k[0]][sym(k[1])] += dw
print('\nby thread (guest instructions, %):')
for t, w in by_thread.most_common():
    if w * 200 < total:
        continue
    print('  thread %d: %.2f%%  (%.2f s at 444 MHz)' % (t, 100.0 * w / total, w / 444e6))
    for s, fw in thread_fn[t].most_common(8):
        print('      %6.2f%% %s' % (100.0 * fw / total, s))
print('\nHLE calls by caller:')
for s, w in hle.most_common(top):
    print('%6.2f%% %s' % (100.0 * w / total, s))

if len(sys.argv) > 5:
    want = sys.argv[5].split(',')
    c = collections.Counter()
    for k, (lr, w) in b.items():
        dw = w - a.get(k, (0, 0))[1]
        if dw > 0 and sym(k[1]) in want:
            c[sym(k[1]) + ' <- ' + sym(lr)] += dw
    print('\ncallers:')
    for s, w in c.most_common(30):
        print('%6.2f%% %s' % (100.0 * w / total, s))
