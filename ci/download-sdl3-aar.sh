#!/usr/bin/env bash
set -euo pipefail

mkdir -p android/app/libs
curl -fsSL -o /tmp/sdl3-android.zip \
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.10/SDL3-devel-3.4.10-android.zip
unzip -jo /tmp/sdl3-android.zip '*.aar' -d android/app/libs/
test -f android/app/libs/SDL3-3.4.10.aar || \
  (echo "ERROR: SDL3 AAR not found in archive" && unzip -l /tmp/sdl3-android.zip && exit 1)
