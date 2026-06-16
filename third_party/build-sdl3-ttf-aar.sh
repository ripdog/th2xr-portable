#!/usr/bin/env bash
set -euo pipefail

# Build SDL3_ttf for Android with 16 KB page alignment and repack the AAR.
#
# Required environment variables:
#   ANDROID_NDK_HOME   Path to the Android NDK root
# Optional:
#   SDL3_AAR           Path to the SDL3 AAR (default: android/app/libs/SDL3-3.4.10.aar)
#   ANDROID_API        Target Android API level (default: 28)
#   ANDROID_ABI        Target ABI (default: arm64-v8a)
#
# Output: android/app/libs/SDL3_ttf-3.2.2.aar (updated with 16 KB-aligned .so)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDL3_TTF_VERSION="3.2.2"
SDL3_TTF_TAG="release-${SDL3_TTF_VERSION}"
SDL3_TTF_URL="https://github.com/libsdl-org/SDL_ttf.git"
AAR_OUTPUT_NAME="SDL3_ttf-${SDL3_TTF_VERSION}.aar"

: "${ANDROID_NDK_HOME:=/opt/android-ndk}"
: "${ANDROID_API:=28}"
: "${ANDROID_ABI:=arm64-v8a}"
: "${SDL3_AAR:=${REPO_ROOT}/android/app/libs/SDL3-3.4.10.aar}"

NDK="${ANDROID_NDK_HOME}"
TOOLCHAIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64"
CMAKE_TOOLCHAIN="${NDK}/build/cmake/android.toolchain.cmake"

if [ ! -f "${SDL3_AAR}" ]; then
    echo "SDL3 AAR not found at ${SDL3_AAR}" >&2
    exit 1
fi
if [ ! -d "${TOOLCHAIN}" ]; then
    echo "NDK toolchain not found at ${TOOLCHAIN}" >&2
    exit 1
fi

case "${ANDROID_ABI}" in
    arm64-v8a) PREFAB_ABI="android.arm64-v8a" ;;
    armeabi-v7a) PREFAB_ABI="android.armeabi-v7a" ;;
    *) echo "Unsupported ABI: ${ANDROID_ABI}" >&2; exit 1 ;;
esac

WORKDIR="/tmp/build-sdl3-ttf-aar-$$"
trap 'rm -rf "${WORKDIR}"' EXIT
mkdir -p "${WORKDIR}"

# ---------------------------------------------------------------------------
# 1. Extract SDL3 headers and .so from the AAR
# ---------------------------------------------------------------------------
echo "Extracting SDL3 from ${SDL3_AAR}..."
SDL3_PREFIX="${WORKDIR}/sdl3-prefix"
mkdir -p "${SDL3_PREFIX}"

unzip -o "${SDL3_AAR}" \
    "prefab/modules/SDL3-shared/libs/${PREFAB_ABI}/libSDL3.so" \
    "prefab/modules/SDL3-Headers/include/SDL3/*" \
    -d "${WORKDIR}/sdl3-extract" > /dev/null

cp "${WORKDIR}/sdl3-extract/prefab/modules/SDL3-shared/libs/${PREFAB_ABI}/libSDL3.so" \
   "${SDL3_PREFIX}/"
cp -r "${WORKDIR}/sdl3-extract/prefab/modules/SDL3-Headers/include/SDL3" \
      "${SDL3_PREFIX}/"

# Create a minimal SDL3Config.cmake so SDL3_ttf's find_package(SDL3) works.
cat > "${SDL3_PREFIX}/SDL3Config.cmake" << 'CMAKE'
set(SDL3_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(SDL3_LIBRARY "${CMAKE_CURRENT_LIST_DIR}/libSDL3.so")
if(NOT TARGET SDL3::Headers)
    add_library(SDL3::Headers INTERFACE IMPORTED)
    set_target_properties(SDL3::Headers PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SDL3_INCLUDE_DIR}")
endif()
if(NOT TARGET SDL3::SDL3-shared)
    add_library(SDL3::SDL3-shared SHARED IMPORTED)
    set_target_properties(SDL3::SDL3-shared PROPERTIES
        IMPORTED_LOCATION "${SDL3_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SDL3_INCLUDE_DIR}")
endif()
if(NOT TARGET SDL3::SDL3)
    add_library(SDL3::SDL3 ALIAS SDL3::SDL3-shared)
endif()
if(NOT TARGET SDL3-shared)
    add_library(SDL3-shared ALIAS SDL3::SDL3-shared)
endif()
if(NOT TARGET Headers)
    add_library(Headers ALIAS SDL3::Headers)
endif()
CMAKE

cat > "${SDL3_PREFIX}/SDL3ConfigVersion.cmake" << 'CMAKE'
set(PACKAGE_VERSION "3.4.10")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
CMAKE

# ---------------------------------------------------------------------------
# 2. Clone SDL3_ttf source
# ---------------------------------------------------------------------------
echo "Cloning SDL_ttf ${SDL3_TTF_TAG}..."
git clone --depth 1 --branch "${SDL3_TTF_TAG}" --recurse-submodules \
    "${SDL3_TTF_URL}" "${WORKDIR}/sdl3_ttf-src" 2>&1 | tail -3

# ---------------------------------------------------------------------------
# 3. Configure and build
# ---------------------------------------------------------------------------
echo "Configuring SDL3_ttf..."
cmake -S "${WORKDIR}/sdl3_ttf-src" \
      -B "${WORKDIR}/sdl3_ttf-build" \
      -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN}" \
      -DANDROID_ABI="${ANDROID_ABI}" \
      -DANDROID_PLATFORM="android-${ANDROID_API}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DSDL3_DIR="${SDL3_PREFIX}" \
      -DSDLTTF_SAMPLES=OFF \
      -DSDLTTF_INSTALL=OFF \
      -DSDLTTF_INSTALL_CPACK=OFF \
      -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384"

echo "Building SDL3_ttf..."
cmake --build "${WORKDIR}/sdl3_ttf-build" -- -j"$(nproc)"

# ---------------------------------------------------------------------------
# 4. Verify 16 KB alignment
# ---------------------------------------------------------------------------
BUILT_SO="${WORKDIR}/sdl3_ttf-build/libSDL3_ttf.so"
ALIGNMENT=$("${TOOLCHAIN}/bin/llvm-readelf" -l "${BUILT_SO}" 2>/dev/null \
    | grep 'LOAD' | awk '{print $NF}' | sort -u)
if [ "${ALIGNMENT}" != "0x4000" ]; then
    echo "ERROR: SDL3_ttf .so does not have 16 KB alignment (found: ${ALIGNMENT})" >&2
    exit 1
fi
echo "SDL3_ttf .so alignment verified: 0x4000"

# ---------------------------------------------------------------------------
# 5. Repack the AAR with the new .so
# ---------------------------------------------------------------------------
OUTDIR="${REPO_ROOT}/android/app/libs"
mkdir -p "${OUTDIR}"

# Extract the existing AAR as a template, or build a fresh one.
# For simplicity, extract the current AAR (if it exists) and replace only
# the .so; this preserves the prefab metadata and headers.
if [ -f "${OUTDIR}/${AAR_OUTPUT_NAME}" ]; then
    echo "Repacking existing AAR..."
    mkdir -p "${WORKDIR}/aar-repack"
    cd "${WORKDIR}/aar-repack"
    unzip -o "${OUTDIR}/${AAR_OUTPUT_NAME}" > /dev/null
    cp "${BUILT_SO}" \
       "prefab/modules/SDL3_ttf-shared/libs/${PREFAB_ABI}/libSDL3_ttf.so"
    zip -r "${WORKDIR}/${AAR_OUTPUT_NAME}" . > /dev/null
    cp "${WORKDIR}/${AAR_OUTPUT_NAME}" "${OUTDIR}/${AAR_OUTPUT_NAME}"
else
    echo "No existing AAR to repack; copying .so only (not a full AAR)."
    echo "The Gradle build expects an AAR at ${OUTDIR}/${AAR_OUTPUT_NAME}"
    exit 1
fi

echo "Done: ${OUTDIR}/${AAR_OUTPUT_NAME}"
