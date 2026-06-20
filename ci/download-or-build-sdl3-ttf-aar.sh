#!/usr/bin/env bash
set -euo pipefail

export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/29.0.14206865"
export PATH="$ANDROID_SDK_ROOT/cmake/3.22.1/bin:$PATH"
curl -fsSL -o /tmp/sdl3-ttf-aar.zip \
  https://github.com/libsdl-org/SDL_ttf/releases/download/release-3.2.2/SDL3_ttf-devel-3.2.2-android.zip
unzip -jo /tmp/sdl3-ttf-aar.zip '*.aar' -d android/app/libs/
bash third_party/build-sdl3-ttf-aar.sh
