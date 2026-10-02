#!/bin/sh
# Build a private vitaGL for the Vita build: the pinned upstream commit plus the
# patches in tools/vitagl/*.patch (per-draw CPU cost fixes, profiler phase hooks),
# without vitaGL's animated startup logo (NO_SPLASHSCREEN=1): the app's own boot
# picture (sce_sys/pic0.png) and then the game's art show instead.
# Output: deps/vita/lib/libvitaGL.a and deps/vita/include/vitaGL.h, which
# CMakeLists.txt links instead of the SDK's libvitaGL.a when present.
# The SDK's own libvitaGL.a is never touched.
#
# env: VITAGL_SRC     checkout to use (default deps/src/vitaGL; reset to the pinned commit)
#      VITAGL_PREFIX  install prefix (default deps/vita)
#      VITAGL_FLAGS   extra make variables (default: NO_DEBUG=1 PROF_HOOKS=1 NO_SPLASHSCREEN=1)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${VITAGL_SRC:-$ROOT/deps/src/vitaGL}
PREFIX=${VITAGL_PREFIX:-$ROOT/deps/vita}
COMMIT=34017a4f575b13439dbab088b0e8be4b19a6af33
FLAGS=${VITAGL_FLAGS:-NO_DEBUG=1 PROF_HOOKS=1 NO_SPLASHSCREEN=1}
: "${VITASDK:?set VITASDK}"
export PATH="$VITASDK/bin:$PATH"

if [ ! -d "$SRC/.git" ]; then
    mkdir -p "$(dirname "$SRC")"
    git clone https://github.com/Rinnegatamante/vitaGL "$SRC"
fi
git -C "$SRC" checkout -q -f "$COMMIT" 2>/dev/null || {
    git -C "$SRC" fetch -q origin
    git -C "$SRC" checkout -q -f "$COMMIT"
}
git -C "$SRC" clean -q -fdx
for p in "$ROOT"/tools/vitagl/*.patch; do
    git -C "$SRC" apply "$p"
    echo "applied $(basename "$p")"
done

# shellcheck disable=SC2086
make -C "$SRC" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" $FLAGS
mkdir -p "$PREFIX/lib" "$PREFIX/include"
cp "$SRC/libvitaGL.a" "$PREFIX/lib/"
cp "$SRC/source/vitaGL.h" "$PREFIX/include/"
echo "installed to $PREFIX (vitaGL $COMMIT + $(ls "$ROOT"/tools/vitagl/*.patch | wc -l | tr -d ' ') patches)"
