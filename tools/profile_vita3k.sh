#!/bin/sh
# Benchmark (tools/bench_vita3k.sh) under the Vita3K guest profiler (VITA3K_PROFILE, see
# Vita3K's dynarmic_cpu.cpp): unlike the in-game sampler, it bills HLE work (sceGxm*,
# sceClib*: calls into system modules) at the import stub, so the cost of every
# vitaGL call into libgxm shows up where it is made. Only the run phase is kept
# (difference of two snapshots) and symbolized with tools/vita3k_profile_sym.py.
#
# usage: tools/profile_vita3k.sh [seconds]   (env: BUILD as for bench_vita3k.sh, TOP)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build-vita-prof}
export BUILD
DATA="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/igi"
OUT=${TMPDIR:-/tmp}/igi_v3k_profile
mkdir -p "$OUT"
rm -f "$OUT"/live.txt "$OUT"/start.txt "$OUT"/end.txt
export VITA3K_PROFILE="$OUT/live.txt"

# bench.c creates bench_running.txt when the measured run starts.
rm -f "$DATA/bench_running.txt"
"$ROOT/tools/bench_vita3k.sh" "${1:-30}" > "$OUT/bench.txt" 2>&1 &
bench=$!
while kill -0 $bench 2>/dev/null && [ ! -f "$DATA/bench_running.txt" ]; do
    sleep 1
done
sleep 6   # the profile is dumped every 5 s
cp "$OUT/live.txt" "$OUT/start.txt" 2>/dev/null
while kill -0 $bench 2>/dev/null; do
    cp "$OUT/live.txt" "$OUT/end.txt" 2>/dev/null
    sleep 1
done
cp "$OUT/live.txt" "$OUT/end.txt" 2>/dev/null
tail -n 1 "$OUT/bench.txt"
python3 "$ROOT/tools/vita3k_profile_sym.py" "$BUILD/igi" "$OUT/start.txt" "$OUT/end.txt" "${TOP:-40}"
