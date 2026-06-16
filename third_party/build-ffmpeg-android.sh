#!/usr/bin/env bash
set -euo pipefail

# Cross-compile FFmpeg for Android arm64-v8a.
# Usage: ANDROID_NDK_HOME=/path/to/ndk ./third_party/build-ffmpeg-android.sh
#
# Output goes to third_party/ffmpeg-android so CMake's pkg-config setup
# picks it up automatically.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="${SCRIPT_DIR}/ffmpeg-android"
BUILD_DIR="${SCRIPT_DIR}/ffmpeg-build"

FFMPEG_VERSION="7.1.1"
FFMPEG_TARBALL="ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TARBALL}"

: "${ANDROID_NDK_HOME:=/opt/android-ndk}"
: "${ANDROID_API:=28}"
: "${ANDROID_ABI:=arm64-v8a}"

# ---------------------------------------------------------------------------
# Toolchain setup
# ---------------------------------------------------------------------------
TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
if [ ! -d "${TOOLCHAIN}" ]; then
    echo "NDK toolchain not found at ${TOOLCHAIN}" >&2
    echo "Set ANDROID_NDK_HOME to the NDK root directory." >&2
    exit 1
fi

case "${ANDROID_ABI}" in
    arm64-v8a)
        HOST="aarch64-linux-android"
        ARCH="aarch64"
        ;;
    armeabi-v7a)
        HOST="armv7a-linux-androideabi"
        ARCH="arm"
        ;;
    *)
        echo "Unsupported ABI: ${ANDROID_ABI}" >&2
        exit 1
        ;;
esac

CC="${TOOLCHAIN}/bin/${HOST}${ANDROID_API}-clang"
CXX="${TOOLCHAIN}/bin/${HOST}${ANDROID_API}-clang++"
AR="${TOOLCHAIN}/bin/llvm-ar"
AS="${TOOLCHAIN}/bin/${HOST}${ANDROID_API}-clang"
LD="${TOOLCHAIN}/bin/ld.lld"
RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
NM="${TOOLCHAIN}/bin/llvm-nm"
STRIP="${TOOLCHAIN}/bin/llvm-strip"
SYSROOT="${TOOLCHAIN}/sysroot"

# ---------------------------------------------------------------------------
# Download and extract
# ---------------------------------------------------------------------------
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "${FFMPEG_TARBALL}" ]; then
    echo "Downloading ${FFMPEG_URL}..."
    curl -fsSL -o "${FFMPEG_TARBALL}" "${FFMPEG_URL}"
fi

if [ ! -d "ffmpeg-${FFMPEG_VERSION}" ]; then
    echo "Extracting ${FFMPEG_TARBALL}..."
    tar -xJf "${FFMPEG_TARBALL}"
fi

cd "ffmpeg-${FFMPEG_VERSION}"

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
# Match the existing build: enable avdevice, enable only the decoders /
# demuxers / parsers we need for a visual novel engine (MPEG-1/2 video
# for OP/ED movies, PNG for stills, OGG/Vorbis for audio, etc.).
echo "Configuring FFmpeg ${FFMPEG_VERSION} for ${ANDROID_ABI}..."
./configure \
    --enable-cross-compile \
    --target-os=android \
    --arch="${ARCH}" \
    --sysroot="${SYSROOT}" \
    --cc="${CC}" \
    --cxx="${CXX}" \
    --ar="${AR}" \
    --ranlib="${RANLIB}" \
    --nm="${NM}" \
    --strip="${STRIP}" \
    --extra-cflags="-fPIC -O2" \
    --extra-ldflags="-fuse-ld=lld" \
    --prefix="${INSTALL_PREFIX}" \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --disable-programs \
    --disable-ffmpeg \
    --disable-ffplay \
    --disable-ffprobe \
    --enable-avdevice \
    --enable-zlib \
    --disable-debug \
    --enable-pthreads \
    --enable-asm \
    --enable-neon \
    || { cat ffbuild/config.log; exit 1; }

# ---------------------------------------------------------------------------
# Build and install
# ---------------------------------------------------------------------------
echo "Building FFmpeg..."
make -j"$(nproc)"

echo "Installing to ${INSTALL_PREFIX}..."
rm -rf "${INSTALL_PREFIX}"
make install

# Strip to keep the APK small.
"${STRIP}" "${INSTALL_PREFIX}/lib/"*.so

echo "FFmpeg installed to ${INSTALL_PREFIX}"
