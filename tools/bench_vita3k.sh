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
#        SHOT     capture every SHOT-th measured frame (the game holds it on screen for a
#                 few seconds) from the Vita3K window into $SHOT_DIR/bench_shot_<frame>.png;
#                 for visual checks only, the timings are off
#        SHOT_DIR where the captures go (default $BUILD/shots)
#        HOLD     seconds the game sleeps at the main menu, before the mission starts
#                 and when gameplay begins (see shim/src/bench.c; for profiles)
#        MISSION  mission list entry to start (default 1)
#        VIEWS    views mode (see shim/src/bench.c): k traced views of a full turn at
#                 the start position, captured like SHOT into $SHOT_DIR; igi.log (with
#                 the traces) is copied there too
#        SLOW     slow frame threshold in ms for the summary's slow-frame breakdown
#                 (default 16); with an IGI_PROFILE build, the profile then keeps only
#                 the samples of frames at least that slow (negative: of the frames
#                 faster than -SLOW ms, for comparisons)
#        STILL    1: the player stands still facing the start direction (a heavy
#                 view in mission 1) instead of the movement pattern
#        FRAMES   where to copy the per-frame breakdown bench_frames.csv (default
#                 $BUILD/bench_frames.csv)
#
# Only the instance started here is killed ("Vita3K -r PIGI00001"); other Vita3K
# instances and other apps' data are left alone.
set -u
if [ "${VIEWS:-0}" -gt 0 ]; then
    SHOT=1
fi
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

# The installed eboot is put back afterwards, so a playtest install isn't replaced.
SAVED_EBOOT=${TMPDIR:-/tmp}/igi_bench_saved_eboot.$$
cp "$APP/eboot.bin" "$SAVED_EBOOT" 2>/dev/null
unzip -p "$BUILD/igi.vpk" eboot.bin > "$APP/eboot.bin" || exit 1
echo "installed $BUILD/igi.vpk eboot.bin"
rm -f "$DATA/bench_result.txt"
touch "$DATA/.bench_start"
echo "$SECS ${SHOT:-0} ${HOLD:-0} ${MISSION:-1} ${VIEWS:-0} ${SLOW:-0} ${STILL:-0}" > "$DATA/bench.txt"

stop() {
    pkill -9 -f "Vita3K -r PIGI00001" 2>/dev/null
    rm -f "$DATA/bench.txt"
    if [ -s "$SAVED_EBOOT" ]; then
        mv -f "$SAVED_EBOOT" "$APP/eboot.bin"
    fi
}
trap 'stop; exit 1' INT TERM

if [ "${SHOT:-0}" -gt 0 ]; then
    SHOT_DIR=${SHOT_DIR:-$BUILD/shots}
    mkdir -p "$SHOT_DIR"
    rm -f "$SHOT_DIR"/bench_shot_*.png "$DATA/bench_shot_now.txt"
    WINID_BIN=${TMPDIR:-/tmp}/igi_window_id
    [ "$WINID_BIN" -nt "$ROOT/tools/window_id.swift" ] || swiftc -O "$ROOT/tools/window_id.swift" -o "$WINID_BIN" || exit 1
fi

# In a subshell, so the shell does not report the kill below.
(VITA3K_CPU_MHZ=${MHZ:-444} "$VITA3K" -r PIGI00001 > "$LOG" 2>&1 &)
echo "Vita3K started (CPU ${MHZ:-444} MHz), waiting for the result (timeout $TIMEOUT s)"

shots=0
waited=0
while [ ! -s "$DATA/bench_result.txt" ] && [ $waited -lt "$TIMEOUT" ]; do
    if ! pgrep -f "Vita3K -r PIGI00001" > /dev/null; then
        break
    fi
    if [ "${SHOT:-0}" -gt 0 ]; then
        # The game writes bench_shot_now.txt (N) while measured frame N is held on screen.
        if [ -s "$DATA/bench_shot_now.txt" ]; then
            n=$(cat "$DATA/bench_shot_now.txt")
            rm -f "$DATA/bench_shot_now.txt"
            f=$(printf "%s/bench_shot_%05d.png" "$SHOT_DIR" "$n")
            pid=$(pgrep -f "Vita3K -r PIGI00001" | head -n 1)
            win=$("$WINID_BIN" "$pid" "(PIGI00001)" | head -n 1)
            if [ -z "$win" ]; then
                echo "shot $n: no window found for Vita3K (pid $pid)" >&2
            elif screencapture -x -o -l "$win" "$f"; then
                shots=$((shots + 1))
            fi
        fi
        sleep 1
        waited=$((waited + 1))
        continue
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
    echo "screenshots: $shots in $SHOT_DIR"
fi
if [ "${VIEWS:-0}" -gt 0 ]; then
    cp "$DATA/igi.log" "$SHOT_DIR/igi.log"
fi
if [ "$DATA/bench_frames.csv" -nt "$DATA/.bench_start" ]; then
    cp "$DATA/bench_frames.csv" "${FRAMES:-$BUILD/bench_frames.csv}"
fi
cat "$DATA/bench_result.txt"
