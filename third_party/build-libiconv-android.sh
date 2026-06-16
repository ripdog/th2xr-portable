#!/usr/bin/env bash
set -euo pipefail

# Build GNU libiconv for Android arm64-v8a using the NDK toolchain.
# Output goes to ../libiconv-android so CMake can pick it up.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="${SCRIPT_DIR}/libiconv-android"
BUILD_DIR="${SCRIPT_DIR}/libiconv-build"

LIBICONV_VERSION="1.19"
LIBICONV_TARBALL="libiconv-${LIBICONV_VERSION}.tar.gz"
LIBICONV_URL="https://ftp.gnu.org/pub/gnu/libiconv/${LIBICONV_TARBALL}"

: "${ANDROID_NDK_HOME:=/opt/android-ndk}"
: "${ANDROID_API:=28}"
: "${ANDROID_ABI:=arm64-v8a}"

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
export PATH="${TOOLCHAIN}/bin:${PATH}"

if [ "${ANDROID_ABI}" = "arm64-v8a" ]; then
    HOST="aarch64-linux-android"
    CC="aarch64-linux-android${ANDROID_API}-clang"
    CXX="aarch64-linux-android${ANDROID_API}-clang++"
elif [ "${ANDROID_ABI}" = "armeabi-v7a" ]; then
    HOST="armv7a-linux-androideabi"
    CC="armv7a-linux-androideabi${ANDROID_API}-clang"
    CXX="armv7a-linux-androideabi${ANDROID_API}-clang++"
else
    echo "Unsupported ABI: ${ANDROID_ABI}" >&2
    exit 1
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "${LIBICONV_TARBALL}" ]; then
    echo "Downloading ${LIBICONV_URL}..."
    curl -fsSL -o "${LIBICONV_TARBALL}" "${LIBICONV_URL}"
fi

if [ ! -d "libiconv-${LIBICONV_VERSION}" ]; then
    echo "Extracting ${LIBICONV_TARBALL}..."
    tar -xzf "${LIBICONV_TARBALL}"
fi

cd "libiconv-${LIBICONV_VERSION}"

# Ensure a clean build.
make distclean 2>/dev/null || true

export CC CXX
export AR="${TOOLCHAIN}/bin/llvm-ar"
export AS="${TOOLCHAIN}/bin/llvm-as"
export LD="${TOOLCHAIN}/bin/ld.lld"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export NM="${TOOLCHAIN}/bin/llvm-nm"
export CFLAGS="-fPIC -O2"
export CXXFLAGS="-fPIC -O2"
export LDFLAGS="-fuse-ld=lld"

echo "Configuring libiconv ${LIBICONV_VERSION} for ${ANDROID_ABI} (API ${ANDROID_API})..."
./configure \
    --host="${HOST}" \
    --prefix="${INSTALL_PREFIX}" \
    --enable-static \
    --disable-shared \
    --disable-rpath \
    --with-gnu-ld \
    --enable-extra-encodings

echo "Building libiconv..."
make -j"$(nproc)"

echo "Installing libiconv to ${INSTALL_PREFIX}..."
rm -rf "${INSTALL_PREFIX}"
make install

echo "Done. Installed to ${INSTALL_PREFIX}"
