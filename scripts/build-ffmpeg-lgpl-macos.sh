#!/usr/bin/env bash
# Build a minimal LGPL-only FFmpeg for Dave, mirroring OverSync's proven pattern.
# Output goes in third_party/ffmpeg-lgpl/ — completely separate from any system
# GPL build (e.g. brew), so there is NO license contamination.
#
# Why LGPL-only: Dave is dual-licensed (GPL + commercial). Linking or shipping
# a GPL FFmpeg (libx264/libx265/libmp3lame enabled) would make the whole app
# GPL-only and kill the commercial option. This build enables ONLY LGPL decoders
# (the built-in h264/hevc/dnxhd decoders are LGPL — it's the *encoders*
# libx264/x265 that are GPL).
#
# Why subprocess (not linking): we invoke this ffmpeg as a separate process over
# a pipe — "mere aggregation," unambiguously not a derivative work. Cleaner
# licensing posture + free process isolation (a crash can't take Dave down).
# Same model OverSync uses for frame-accurate video.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${DAVE_FFMPEG_VERSION:-8.1.1}"
SOURCE_URL="${DAVE_FFMPEG_SOURCE_URL:-https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz}"
BUILD_ROOT="${DAVE_FFMPEG_BUILD_ROOT:-$ROOT/third_party/ffmpeg-lgpl-build}"
INSTALL_ROOT="${DAVE_FFMPEG_INSTALL_ROOT:-$ROOT/third_party/ffmpeg-lgpl}"
DEPLOYMENT_TARGET="${DAVE_FFMPEG_DEPLOYMENT_TARGET:-14.0}"
ARCHS="${DAVE_FFMPEG_ARCHS:-$(uname -m)}"
SOURCE_DIR="$BUILD_ROOT/source/ffmpeg-$VERSION"
BUILD_DIR="$BUILD_ROOT/build"

# Minimal enable-list: ONLY what Dave needs. LGPL-safe — no --enable-gpl, no
# external GPL codecs. Built-in h264/hevc/dnxhd decoders are LGPL.
CONFIGURE_FLAGS=(
  "--cc=clang"
  "--enable-shared"
  "--disable-static"
  "--disable-autodetect"
  "--disable-doc"
  "--disable-debug"
  "--disable-htmlpages"
  "--disable-manpages"
  "--disable-podpages"
  "--disable-txtpages"
  "--disable-everything"
  "--enable-ffmpeg"
  "--enable-ffprobe"
  # Container demuxers (covers MP4/MOV, MKV, MXF, plus audio for probing).
  "--enable-demuxer=mov"
  "--enable-demuxer=matroska"
  "--enable-demuxer=mxf"
  "--enable-demuxer=wav"
  "--enable-demuxer=mp3"
  "--enable-demuxer=aac"
  "--enable-demuxer=flac"
  # Decoders — all LGPL (built-in). h264/hevc for general video, dnxhd for pro.
  "--enable-decoder=h264"
  "--enable-decoder=hevc"
  "--enable-decoder=dnxhd"
  "--enable-decoder=rawvideo"
  "--enable-decoder=pcm_s16le"
  "--enable-decoder=pcm_s24le"
  "--enable-decoder=aac"
  "--enable-decoder=mp3"
  "--enable-decoder=flac"
  # Parsers matching the decoders.
  "--enable-parser=h264"
  "--enable-parser=hevc"
  "--enable-parser=dnxhd"
  "--enable-parser=aac"
  # Filters for RGBA conversion + scaling (we request -pix_fmt rgba + resize).
  "--enable-filter=scale"
  "--enable-filter=format"
  # Protocols: read files, write raw frames to our pipe.
  "--enable-protocol=file"
  "--enable-protocol=pipe"
  # Swscale for pixel format conversion.
  "--enable-swscale"
  # Output raw video (what we read from the pipe).
  "--enable-muxer=rawvideo"
  "--enable-encoder=rawvideo"
  "--enable-muxer=null"
  "--enable-encoder=wrapped_avframe"
  "--prefix=$INSTALL_ROOT"
  # NOT included: --enable-gpl, --enable-libx264, --enable-libx265,
  # --enable-libmp3lame, etc. — those would make this GPL.
)

mkdir -p "$BUILD_ROOT/source" "$BUILD_DIR" "$INSTALL_ROOT"

echo "=== Dave LGPL FFmpeg build ==="
echo "Version: $VERSION"
echo "Arch:    $ARCHS"
echo "Install: $INSTALL_ROOT"

# Download + verify source if not present.
SOURCE_ARCHIVE="$BUILD_ROOT/source/ffmpeg-$VERSION.tar.xz"
if [[ ! -d "$SOURCE_DIR" ]]; then
  if [[ ! -f "$SOURCE_ARCHIVE" ]]; then
    echo "Downloading $SOURCE_URL ..."
    curl -fSL "$SOURCE_URL" -o "$SOURCE_ARCHIVE"
  fi
  echo "Extracting..."
  tar -xf "$SOURCE_ARCHIVE" -C "$BUILD_ROOT/source"
fi

# Build for each arch (usually just one on Apple Silicon).
for ARCH in $ARCHS; do
  echo "=== Configuring for $ARCH ==="
  cd "$SOURCE_DIR"
  # Apple Silicon: arm64; Intel: x86_64. Add cross-compile flags for x86_64.
  EXTRA_FLAGS=""
  if [[ "$ARCH" == "x86_64" ]]; then
    EXTRA_FLAGS="--extra-cflags=-target,x86_64-apple-macos$DEPLOYMENT_TARGET --extra-ldflags=-target,x86_64-apple-macos$DEPLOYMENT_TARGET --arch=x86_64"
  fi
  ./configure "${CONFIGURE_FLAGS[@]}" $EXTRA_FLAGS \
    --enable-cross-compile \
    --dep-cc=clang

  echo "=== Building ($ARCH) ==="
  make -j"$(sysctl -n hw.ncpu)"

  echo "=== Installing ($ARCH) ==="
  make install
done

echo
echo "=== DONE. LGPL FFmpeg installed to: $INSTALL_ROOT ==="
echo "Verify it's LGPL (no GPL in the build):"
"$INSTALL_ROOT/bin/ffmpeg" -version 2>&1 | grep -iE "version|license|enable" | head
echo
echo "This ffmpeg is invoked as a SUBPROCESS by Dave — not linked. The binary"
echo "sits in third_party/ffmpeg-lgpl/ and is launched via pipe for video decode."
