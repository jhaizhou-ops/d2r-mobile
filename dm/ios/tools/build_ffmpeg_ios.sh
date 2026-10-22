#!/bin/bash
# 为 iOS 设备构建只含 Matroska/WebM、VP8 与 NV12 输出所需模块的 FFmpeg 静态库。
# Build the minimal FFmpeg static libraries needed for Matroska/WebM, VP8, and NV12 output on iOS devices.
set -eu
ROOT=${1:-${DM_ROOT:-$HOME/dm-work}/build/cache/dm/ffmpeg-ios}
VERSION=7.1.1
ARCHIVE=ffmpeg-$VERSION.tar.xz
SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
URL=https://ffmpeg.org/releases/$ARCHIVE
MIN_IOS=${DM_IOS_DEPLOYMENT_MIN:-18.0}
SRC=$ROOT/src/ffmpeg-$VERSION
OUT=$ROOT/out
mkdir -p "$ROOT/src" "$OUT"

if [ -f "$OUT/lib/libavcodec.a" ] && [ -f "$OUT/include/libavcodec/avcodec.h" ]; then
  echo "FFmpeg iOS already built: $OUT"
  exit 0
fi

if [ ! -f "$ROOT/src/$ARCHIVE" ]; then
  curl -L --fail --retry 3 "$URL" -o "$ROOT/src/$ARCHIVE"
fi
echo "$SHA256  $ROOT/src/$ARCHIVE" | shasum -a 256 -c -
if [ ! -f "$SRC/configure" ]; then
  tar -xf "$ROOT/src/$ARCHIVE" -C "$ROOT/src"
fi

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
cd "$SRC"
./configure \
  --prefix="$OUT" \
  --target-os=darwin \
  --arch=arm64 \
  --enable-cross-compile \
  --cc="$CC" \
  --sysroot="$SDK" \
  --extra-cflags="-arch arm64 -miphoneos-version-min=$MIN_IOS -fembed-bitcode=off" \
  --extra-ldflags="-arch arm64 -miphoneos-version-min=$MIN_IOS" \
  --disable-everything \
  --disable-programs \
  --disable-doc \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --disable-swresample \
  --disable-network \
  --disable-autodetect \
  --disable-zlib \
  --disable-iconv \
  --disable-debug \
  --enable-avformat \
  --enable-avcodec \
  --enable-avutil \
  --enable-swscale \
  --enable-demuxer=matroska \
  --enable-decoder=vp8 \
  --enable-parser=vp8 \
  --enable-pthreads
make -j"$(sysctl -n hw.logicalcpu)"
make install

for library in libavformat libavcodec libswscale libavutil; do
  [ -f "$OUT/lib/$library.a" ] || { echo "missing $OUT/lib/$library.a" >&2; exit 1; }
done
echo "FFmpeg iOS built: $OUT"
echo "  DM_MOVIE_FFMPEG=1 DM_MOVIE_FFMPEG_PREFIX=$OUT"
