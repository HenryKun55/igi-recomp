#!/bin/sh
# Automated in-game benchmark of the Vita build in Vita3K (see shim/src/bench.c):
# installs the build's eboot.bin into the emulated PIGI00001, requests a benchmark
# through ux0:data/igi/bench.txt, launches Vita3K, waits for bench_result.txt,
# closes that Vita3K instance and prints the result (and the top of profile.txt
# for an IGI_PROFILE build).
#
# usage: tools/bench_vita3k.sh [seconds]    (default 30)
# env:   BUILD    Vita build directory (default build-vita-prof if built, else build-vita)
#        MHZ      emulated CPU clock for Vita3K (default 444)
#        VITA3K   Vita3K binary (default the local source build)
#        TIMEOUT  seconds before giving up (default seconds + 900)
#        SHOT     screenshot every SHOT measured frames into ux0:data/igi/bench_shot_*.bmp
#                 (for visual checks; the read backs slow those frames down)
#
# Only the instance started here is killed ("Vita3K -r PIGI00001"); other Vita3K
# instances and other apps' data are left alone.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SECS=${1:-30}
if [ -z "${BUILD:-}" ]; then
    BUILD=$ROOT/build-vita-prof
    [ -f "$BUILD/igi.vpk" ] || BUILD=$ROOT/build-vita
fi
VITA3K=${VITA3K:-$HOME/Documents/workspace/games/Vita3K/build/macos-ninja/bin/Release/Vita3K.app/Contents/MacOS/Vita3K}
TIMEOUT=${TIMEOUT:-$((SECS + 900))}
UX0="$HOME/Library/Application Support/Vita3K/Vita3K/fs/ux0"
APP="$UX0/app/PIGI00001"
DATA="$UX0/data/igi"
LOG=${TMPDIR:-/tmp}/igi_bench_vita3k.log

[ -f "$BUILD/igi.vpk" ] || { echo "no $BUILD/igi.vpk; build first" >&2; exit 1; }
[ -x "$VITA3K" ] || { echo "no Vita3K at $VITA3K" >&2; exit 1; }
[ -d "$APP" ] || { echo "PIGI00001 is not installed in Vita3K ($APP)" >&2; exit 1; }
[ -d "$DATA" ] || { echo "no game data in $DATA" >&2; exit 1; }
if pgrep -f "Vita3K -r PIGI00001" > /dev/null; then
    echo "a Vita3K instance running PIGI00001 already exists; close it first" >&2
    exit 1
fi

unzip -p "$BUILD/igi.vpk" eboot.bin > "$APP/eboot.bin" || exit 1
echo "installed $BUILD/igi.vpk eboot.bin"
rm -f "$DATA/bench_result.txt"
touch "$DATA/.bench_start"
rm -f "$DATA"/bench_shot_*.bmp
echo "$SECS ${SHOT:-0}" > "$DATA/bench.txt"

stop() {
    pkill -9 -f "Vita3K -r PIGI00001" 2>/dev/null
    rm -f "$DATA/bench.txt"
}
trap 'stop; exit 1' INT TERM

# In a subshell, so the shell does not report the kill below.
(VITA3K_CPU_MHZ=${MHZ:-444} "$VITA3K" -r PIGI00001 > "$LOG" 2>&1 &)
echo "Vita3K started (CPU ${MHZ:-444} MHz), waiting for the result (timeout $TIMEOUT s)"

waited=0
while [ ! -s "$DATA/bench_result.txt" ] && [ $waited -lt "$TIMEOUT" ]; do
    if ! pgrep -f "Vita3K -r PIGI00001" > /dev/null; then
        break
    fi
    sleep 2
    waited=$((waited + 2))
done
# The game writes the profile right after the result and exits (Vita3K stays open).
sleep 2
stop

if [ ! -s "$DATA/bench_result.txt" ]; then
    echo "no result after $waited s; last lines of ux0:data/igi/igi.log:" >&2
    tail -n 15 "$DATA/igi.log" >&2
    exit 1
fi
if [ "$DATA/profile.txt" -nt "$DATA/.bench_start" ]; then
    echo "---- ux0:data/igi/profile.txt (top) ----"
    head -n 40 "$DATA/profile.txt"
fi
if [ "${SHOT:-0}" -gt 0 ]; then
    echo "screenshots: $(ls "$DATA"/bench_shot_*.bmp 2>/dev/null | wc -l | tr -d ' ') in $DATA"
fi
cat "$DATA/bench_result.txt"
