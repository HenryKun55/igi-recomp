#!/bin/sh
# Update-rate A/B run (see shim/src/tick_test.c and docs/tick_rate.md): plays the
# same scripted scenario in mission 1 at each given rate with a deterministic
# clock and writes, per rate, a trace of watched values over game time plus the
# requested snapshots and screenshots, ready for tools/tick_compare.py.
#
# usage: tools/tick_ab.sh NAME HZ...        e.g. tools/tick_ab.sh walk 30 60
# env:   BUILD   build directory with the igi binary (default build)
#        GAME    game data directory (default gamedata/pc)
#        IGI_TICK_INPUT, IGI_TICK_WATCH, IGI_TICK_SNAP, IGI_TICK_SHOT, IGI_TICK_END
#                the scenario (IGI_TICK_END defaults to 10 s of game time)
#        CLOCK   IGI_FAKE_CLOCK step in microseconds (default 300)
#        TIMEOUT seconds before giving up on one run (default 240)
# Output: $BUILD/tick/NAME_HZ_trace.txt (+ _snap_T.bin, _shot_T.bmp, .log).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:?name}
shift
BUILD=${BUILD:-$ROOT/build}
BUILD=$(cd "$BUILD" && pwd) || exit 1
GAME=${GAME:-$ROOT/gamedata/pc}
TIMEOUT=${TIMEOUT:-240}
RUN=$BUILD/tick
mkdir -p "$RUN" || exit 1
cd "$RUN" || exit 1

for HZ in "$@"; do
    OUT=${NAME}_$HZ
    rm -f "${OUT}"_trace.txt "${OUT}"_snap_*.bin "${OUT}"_shot_*.bmp
    IGI_VSYNC=0 IGI_NO_HOST_INPUT=1 IGI_FAKE_CLOCK=${CLOCK:-300} IGI_GAME_HZ=$HZ IGI_BENCH=1000 \
        IGI_TICK_TEST=$OUT IGI_TICK_END=${IGI_TICK_END:-10} "$BUILD/igi" "$GAME" > /dev/null 2>&1 &
    pid=$!
    waited=0
    while kill -0 $pid 2>/dev/null && [ $waited -lt "$TIMEOUT" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 $pid 2>/dev/null; then
        echo "$OUT: timeout after $TIMEOUT s" >&2
        kill $pid 2>/dev/null
    fi
    wait $pid 2>/dev/null
    cp igi.log "${OUT}.log"
    echo "$OUT: $(grep -c . "${OUT}_trace.txt" 2>/dev/null || echo 0) trace lines, $(grep 'tick test: end' igi.log | tail -n 1)"
done
