#!/usr/bin/env bash
set -euo pipefail

android_sdk_root=${ANDROID_SDK_ROOT:-$HOME/android-sdk}
{
  echo "ANDROID_SDK_ROOT=$android_sdk_root"
  echo "ANDROID_HOME=$android_sdk_root"
} >> "$GITHUB_ENV"

mkdir -p "$android_sdk_root/cmdline-tools"
curl -fsSL -o /tmp/cmdline-tools.zip \
  https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
unzip -q /tmp/cmdline-tools.zip -d "$android_sdk_root/cmdline-tools"
mv "$android_sdk_root/cmdline-tools/cmdline-tools" \
  "$android_sdk_root/cmdline-tools/latest"

sdkmanager="$android_sdk_root/cmdline-tools/latest/bin/sdkmanager"
yes 2>/dev/null | "$sdkmanager" --licenses > /dev/null 2>&1 || true
"$sdkmanager" --install \
  "platforms;android-37.0" \
  "build-tools;37.0.0" \
  "ndk;29.0.14206865" \
  "cmake;3.22.1" \
  "platform-tools"
