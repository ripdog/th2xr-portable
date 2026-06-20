#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <ffmpeg|libiconv|zstd|sqlite>" >&2
  exit 2
fi

export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/29.0.14206865"

case "$1" in
  ffmpeg) bash third_party/build-ffmpeg-android.sh ;;
  libiconv) bash third_party/build-libiconv-android.sh ;;
  zstd) bash third_party/build-zstd-android.sh ;;
  sqlite) bash third_party/build-sqlite-android.sh ;;
  *)
    echo "unknown android third-party component: $1" >&2
    exit 2
    ;;
esac
