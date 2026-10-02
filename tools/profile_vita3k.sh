#!/bin/sh
# Benchmark (tools/bench_vita3k.sh) under the Vita3K guest profiler (VITA3K_PROFILE, see
# Vita3K's dynarmic_cpu.cpp): unlike the in-game sampler, it bills HLE work (sceGxm*,
# sceClib*: calls into system modules) at the import stub, so the cost of every
# vitaGL call into libgxm shows up where it is made. Only one phase is kept
# (difference of two snapshots) and symbolized with tools/vita3k_profile_sym.py.
#
# usage: tools/profile_vita3k.sh [seconds]   (env: BUILD as for bench_vita3k.sh, TOP)
# env:   PHASE  run (default): the measured gameplay;
#               boot: from the start to the main menu;
#               loading: from leaving the mission list to gameplay (the mission load).
#        Vita3K rewrites the profile every 5 s: for boot and loading the game holds
#        still (HOLD, default 7 s) at the phase boundaries while a dump is taken.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build-vita-prof}
export BUILD
DATA="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0/data/igi"
OUT=${TMPDIR:-/tmp}/igi_v3k_profile.$$   # per run: concurrent runs must not share it
mkdir -p "$OUT"
rm -f "$OUT"/live.txt "$OUT"/start.txt "$OUT"/end.txt
export VITA3K_PROFILE="$OUT/live.txt"
case "${PHASE:-run}" in
boot | loading) export HOLD=${HOLD:-7} ;;
esac

# bench.c creates bench_running.txt when the measured run starts and writes the
# current phase's name to bench_phase.txt.
rm -f "$DATA/bench_running.txt" "$DATA/bench_phase.txt"
"$ROOT/tools/bench_vita3k.sh" "${1:-30}" > "$OUT/bench.txt" 2>&1 &
bench=$!

# First line only: Vita3K does not truncate a file reopened for writing.
phase_is() {
    [ "$(head -n 1 "$DATA/bench_phase.txt" 2>/dev/null)" = "$1" ]
}

# Waits until the bench is in phase $1.
wait_phase() {
    while kill -0 $bench 2>/dev/null && ! phase_is "$1"; do
        sleep 1
    done
}

# Waits for the next profile dump and copies it to $1.
next_dump() {
    old=$(stat -f %m "$OUT/live.txt" 2>/dev/null || echo 0)
    while kill -0 $bench 2>/dev/null && [ "$(stat -f %m "$OUT/live.txt" 2>/dev/null || echo 0)" = "$old" ]; do
        sleep 1
    done
    sleep 1
    cp "$OUT/live.txt" "$1" 2>/dev/null
}

case "${PHASE:-run}" in
boot)
    : > "$OUT/start.txt"
    wait_phase "hold: main menu"
    next_dump "$OUT/end.txt"
    ;;
loading)
    wait_phase "hold: mission start"
    next_dump "$OUT/start.txt"
    wait_phase "hold: settle"
    next_dump "$OUT/end.txt"
    ;;
*)
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
    ;;
esac
wait $bench
tail -n 1 "$OUT/bench.txt"
python3 "$ROOT/tools/vita3k_profile_sym.py" "$BUILD/igi" "$OUT/start.txt" "$OUT/end.txt" "${TOP:-40}"
