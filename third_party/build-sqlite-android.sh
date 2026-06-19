#!/usr/bin/env bash
set -euo pipefail

# Build SQLite for Android arm64-v8a using the NDK toolchain.
# Output goes to third_party/sqlite-android so CMake can pick it up.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="${SCRIPT_DIR}/sqlite-android"
BUILD_DIR="${SCRIPT_DIR}/sqlite-build"

: "${SQLITE_VERSION:=3530200}"
: "${SQLITE_YEAR:=2026}"
: "${SQLITE_ZIP:=sqlite-amalgamation-${SQLITE_VERSION}.zip}"
: "${SQLITE_URL:=https://www.sqlite.org/${SQLITE_YEAR}/${SQLITE_ZIP}}"
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

CC="${TOOLCHAIN}/bin/${HOST}${ANDROID_API}-clang"
AR="${TOOLCHAIN}/bin/llvm-ar"
RANLIB="${TOOLCHAIN}/bin/llvm-ranlib"
STRIP="${TOOLCHAIN}/bin/llvm-strip"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

if [ ! -f "${SQLITE_ZIP}" ]; then
    echo "Downloading ${SQLITE_URL}..."
    curl -fsSL -o "${SQLITE_ZIP}" "${SQLITE_URL}"
fi

SOURCE_DIR="sqlite-amalgamation-${SQLITE_VERSION}"
if [ ! -d "${SOURCE_DIR}" ]; then
    echo "Extracting ${SQLITE_ZIP}..."
    unzip -q "${SQLITE_ZIP}"
fi

cd "${SOURCE_DIR}"

echo "Building SQLite ${SQLITE_VERSION} for ${ANDROID_ABI} (API ${ANDROID_API})..."
"${CC}" -fPIC -O2 \
    -DSQLITE_THREADSAFE=1 \
    -DSQLITE_OMIT_LOAD_EXTENSION \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -c sqlite3.c -o sqlite3.o
"${AR}" rcs libsqlite3.a sqlite3.o
"${RANLIB}" libsqlite3.a

echo "Installing SQLite to ${INSTALL_PREFIX}..."
rm -rf "${INSTALL_PREFIX}"
mkdir -p "${INSTALL_PREFIX}/include" "${INSTALL_PREFIX}/lib"
cp sqlite3.h sqlite3ext.h "${INSTALL_PREFIX}/include/"
cp libsqlite3.a "${INSTALL_PREFIX}/lib/"
"${STRIP}" -g "${INSTALL_PREFIX}/lib/libsqlite3.a" || true

echo "SQLite installed to ${INSTALL_PREFIX}"
