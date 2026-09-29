#!/bin/sh
# Build a minimal static FFmpeg for the PS Vita: libavcodec with only the Indeo
# decoders (the game's AVI cutscenes) plus libavutil.
# Output: deps/vita/{include,lib}. Needs $VITASDK.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/deps/src/ffmpeg
PREFIX=$ROOT/deps/vita
VERSION=${FFMPEG_VERSION:-n7.1}
: "${VITASDK:?set VITASDK}"
export PATH="$VITASDK/bin:$PATH"

if [ ! -d "$SRC" ]; then
    mkdir -p "$ROOT/deps/src"
    git clone --depth 1 --branch "$VERSION" https://git.ffmpeg.org/ffmpeg.git "$SRC"
fi

BUILD=$ROOT/deps/build-ffmpeg-vita
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --prefix="$PREFIX" \
    --enable-cross-compile --cross-prefix=arm-vita-eabi- \
    --arch=arm --cpu=cortex-a9 --target-os=none \
    --extra-cflags="-mfpu=neon -mfloat-abi=hard -fno-short-enums -O2" \
    --disable-everything --disable-programs --disable-doc --disable-network \
    --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --disable-pthreads --disable-runtime-cpudetect \
    --enable-decoder=indeo5 --enable-decoder=indeo4 --enable-decoder=indeo3 \
    --enable-static --disable-shared
make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
echo "installed to $PREFIX"
