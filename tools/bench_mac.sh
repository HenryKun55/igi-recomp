#!/bin/sh
# Automated in-game benchmark with the desktop build (see shim/src/bench.c): boots
# into mission 1, runs a fixed camera/movement pattern for the given seconds and
# prints the summary (and the top of the profile for an IGI_PROFILE build).
#
# usage: tools/bench_mac.sh [seconds]      (default 30)
# env:   BUILD    build directory with the igi binary (default build)
#        GAME     game data directory (default gamedata/pc)
#        TIMEOUT  seconds before giving up (default seconds + 180)
#        IGI_VSYNC  default 0, so the swap does not wait for the display
#        IGI_BENCH_SHOT  screenshot every N measured frames (bench_shot_*.bmp in the run directory)
# The run happens in $BUILD/bench (igi.log, bench_result.txt, igi_profile.txt).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-30}
BUILD=${BUILD:-$ROOT/build}
BUILD=$(cd "$BUILD" && pwd) || exit 1
GAME=${GAME:-$ROOT/gamedata/pc}
TIMEOUT=${TIMEOUT:-$((SECS + 180))}
RUN=$BUILD/bench

[ -x "$BUILD/igi" ] || { echo "no $BUILD/igi; build first" >&2; exit 1; }
mkdir -p "$RUN" || exit 1
cd "$RUN" || exit 1
rm -f bench_result.txt bench_shot_*.bmp
touch .bench_start

IGI_VSYNC=${IGI_VSYNC:-0} IGI_BENCH=$SECS "$BUILD/igi" "$GAME" > stdout.txt 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null' INT TERM

waited=0
while kill -0 $pid 2>/dev/null && [ $waited -lt "$TIMEOUT" ]; do
    sleep 1
    waited=$((waited + 1))
done
if kill -0 $pid 2>/dev/null; then
    echo "timeout after $TIMEOUT s; last log lines:" >&2
    kill $pid 2>/dev/null
    tail -n 15 igi.log >&2
    exit 1
fi
wait $pid 2>/dev/null

if [ ! -s bench_result.txt ]; then
    echo "no result; last log lines:" >&2
    tail -n 15 igi.log >&2
    exit 1
fi
if [ igi_profile.txt -nt .bench_start ]; then
    echo "---- $RUN/igi_profile.txt (top) ----"
    head -n 40 igi_profile.txt
fi
cat bench_result.txt
