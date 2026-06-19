#!/usr/bin/env bash
set -euo pipefail

# Build zstd for Android arm64-v8a using the NDK toolchain.
# Output goes to third_party/zstd-android so CMake's pkg-config setup
# picks it up automatically.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="${SCRIPT_DIR}/zstd-android"
BUILD_DIR="${SCRIPT_DIR}/zstd-build"

ZSTD_VERSION="1.5.7"
ZSTD_TARBALL="zstd-${ZSTD_VERSION}.tar.gz"
ZSTD_URL="https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/${ZSTD_TARBALL}"

: "${ANDROID_NDK_HOME:=/opt/android-ndk}"
: "${ANDROID_API:=28}"
: "${ANDROID_ABI:=arm64-v8a}"

TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64"
if [ ! -d "${TOOLCHAIN}" ]; then
    echo "NDK toolchain not found at ${TOOLCHAIN}" >&2
    echo "Set ANDROID_NDK_HOME to the NDK root directory." >&2
    exit 1
fi

case "${ANDROID_ABI}" in
    arm64-v8a)
        HOST="aarch64-linux-android"
        ;;
    armeabi-v7a)
        HOST="armv7a-linux-androideabi"
        ;;
    *)
        echo "Unsupported ABI: ${ANDROID_ABI}" >&2
        exit 1
        ;;
esac

export CC="${TOOLCHAIN}/bin/${HOST}${ANDROID_API}-clang"
export AR="${TOOLCHAIN}/bin/llvm-ar"
export RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
export STRIP="${TOOLCHAIN}/bin/llvm-strip"
export CFLAGS="-fPIC -O2"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "${ZSTD_TARBALL}" ]; then
    echo "Downloading ${ZSTD_URL}..."
    curl -fsSL -o "${ZSTD_TARBALL}" "${ZSTD_URL}"
fi

if [ ! -d "zstd-${ZSTD_VERSION}" ]; then
    echo "Extracting ${ZSTD_TARBALL}..."
    tar -xzf "${ZSTD_TARBALL}"
fi

cd "zstd-${ZSTD_VERSION}"
make -C lib clean

echo "Building zstd ${ZSTD_VERSION} for ${ANDROID_ABI}..."
make -C lib -j"$(nproc)" libzstd.a

echo "Installing zstd to ${INSTALL_PREFIX}..."
rm -rf "${INSTALL_PREFIX}"
mkdir -p "${INSTALL_PREFIX}/include" "${INSTALL_PREFIX}/lib/pkgconfig"
cp lib/libzstd.a "${INSTALL_PREFIX}/lib/"
cp lib/zstd.h lib/zdict.h lib/zstd_errors.h "${INSTALL_PREFIX}/include/"
"${STRIP}" -g "${INSTALL_PREFIX}/lib/libzstd.a" || true

cat > "${INSTALL_PREFIX}/lib/pkgconfig/libzstd.pc" <<EOF
prefix=${INSTALL_PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: zstd
Description: fast lossless compression algorithm library
Version: ${ZSTD_VERSION}
Libs: -L\${libdir} -lzstd
Cflags: -I\${includedir}
EOF

echo "zstd installed to ${INSTALL_PREFIX}"
