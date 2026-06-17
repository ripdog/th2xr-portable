# HACKING

Developer-oriented build instructions, CI information, and architecture
notes for [th2xr-portable].

## Quick start

A root `Makefile` wraps the CMake presets for desktop builds and
Gradle for Android:

```bash
make desktop          # configure and build (Release)
make run              # build and launch the engine
make test             # build and run the test suite

make android          # assemble the debug APK
make install-android  # install on a connected device
make logcat           # launch the app and tail logcat

make clean            # remove all build artifacts
```

## Desktop build

### Linux (Ubuntu 24.04+)

SDL3 and SDL3_ttf are not yet packaged; build them from source:

```bash
# System packages
sudo apt install cmake g++ libfontconfig-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev glslang-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev \
  libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libasound2-dev libpulse-dev

# SDL3
git clone https://github.com/libsdl-org/SDL.git --depth 1 --branch release-3.4.10
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL/build -- -j$(nproc)
sudo cmake --install SDL/build

# SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git --depth 1 --branch release-3.2.2
cmake -S SDL_ttf -B SDL_ttf/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL_ttf/build -- -j$(nproc)
sudo cmake --install SDL_ttf/build
```

### macOS

```bash
brew install cmake sdl3 sdl3_ttf fontconfig ffmpeg glslang
```

### Windows (MSVC)

Use [vcpkg]:

```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install --triplet x64-windows `
  pkgconf sdl3 sdl3-ttf fontconfig ffmpeg glslang
```

Then build the project (see the Makefile or CMake presets below).

## CMake presets

The project ships `CMakePresets.json` (requires CMake ≥ 3.25).  The
root `Makefile` targets `desktop`, `test`, and `clean` use these
presets under the hood.

| Preset | Description |
|---|---|
| `desktop-debug` | Debug build with tests, Ninja, `build/debug/` |
| `desktop-release` | Release build with tests, Ninja, `build/release/` |
| `ci` | Inherits `desktop-release`, `build/ci/` |

Build and test presets share the same names as the configure presets.

### Makefile commands

```bash
make desktop           # cmake --preset desktop-release && cmake --build --preset desktop-release
make test              # make desktop && ctest --preset desktop-release
make clean             # rm -rf build/
```

### Raw preset commands

```bash
cmake --preset desktop-release              # configure
cmake --build --preset desktop-release      # build
ctest --preset desktop-release              # test
```

### LSP / editor

The Ninja generator exports `compile_commands.json` to the build tree
(e.g. `build/release/compile_commands.json`).  Point your LSP or
editor to that file, or symlink it:

```bash
ln -sf build/release/compile_commands.json compile_commands.json
```

### User presets

`CMakeUserPresets.json` (gitignored) can extend or override the
shipped presets.

## CI

GitHub Actions workflows build and upload artifacts on every push / PR
(`.github/workflows/build.yml`) and on tagged releases
(`.github/workflows/release.yml`).

**Desktop jobs** produce:
- `toheart2-linux-x86_64.AppImage`
- `toheart2-macos.dmg`
- `toheart2-windows-x64.zip`

They use `libsdl-org/setup-sdl@main` (Linux), Homebrew (macOS), and
MSYS2 / UCRT64 (Windows).

**Android job** installs the Android SDK from command-line tools,
downloads or caches the SDL3 AAR, builds SDL3\_ttf from source with
16‑KB page alignment, cross-compiles FFmpeg and GNU libiconv, runs
`./gradlew :app:assembleDebug`, and uploads the APK.

## Android build

The Android build requires Java 21, Android SDK (API 37), NDK 29.0,
and CMake 3.22+. The native code is compiled by Gradle via the SDK's
CMake, using the same `CMakeLists.txt` as the desktop build.

### Dependencies

Several native dependencies must be obtained before the Gradle build.
The scripts live under `third_party/`.

| Dependency | Source |
|---|---|
| SDL3 3.4.10 | Pre-built AAR from [SDL releases] (already 16‑KB aligned) |
| SDL3\_ttf 3.2.2 | Built from source with 16‑KB page alignment (`third_party/build-sdl3-ttf-aar.sh`) |
| FFmpeg 7.1.1 | Cross-compiled for arm64‑v8a (`third_party/build-ffmpeg-android.sh`) |
| GNU libiconv 1.19 | Cross-compiled for arm64‑v8a (`third_party/build-libiconv-android.sh`) |

The APK targets **16‑KB page alignment** (required by Android 15+ /
API 35+). The alignment is enforced by linker flags in
`CMakeLists.txt`:

```cmake
target_link_options(toheart2 PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384")
```

SDL3 3.4.10 is already 16‑KB aligned; SDL3\_ttf 3.2.2 must be
rebuilt because the official AAR uses 4‑KB alignment.

### Build commands

```bash
make android           # assemble the debug APK
make install-android   # install on a connected device
make logcat            # launch the app and tail logcat
```

Or manually:

```bash
cd android && ./gradlew :app:assembleDebug
```

### Architecture

See the [Android architecture](#android-architecture) section.

## Android architecture

### ImGui

ImGui is rendered directly to the window backbuffer. Android windows
(`Panel Config`, `Player Name`) fill the screen, are non-movable and
non-resizable, and are wrapped in a scrollable child region.

The display scale from `SDL_GetWindowDisplayScale()` is capped at 2.5×
before it is fed to ImGui. On a typical phone (3.75× native scale)
this keeps the logical width usable (~576 px) while still producing
sharp text.

### Touch input pipeline

SDL's touch-to-mouse synthesis is **disabled** on Android
(`SDL_HINT_TOUCH_MOUSE_EVENTS=0`) to avoid duplicate events and
spurious down/up sequences that broke choices, menus, and movies.

- **Taps** are detected by `th2::TouchInput` and re-injected as
  synthetic left mouse-button down+up events with `which =
  SDL_TOUCH_MOUSEID`.  ImGui's `process_event()` discriminates on
  this ID to set `ImGuiMouseSource_TouchScreen`.
- **Continuous motion** (finger drags) is forwarded directly to
  ImGui via `on_touch_down/motion/up()` methods in `ImGuiLayer`.
  This enables drag-to-scroll inside the config and name-input
  windows without going through the SDL event queue.
- The sidebar / backlog / movie / choice mouse handlers **guard
  against synthetic events** to prevent double-processing.

Relevant files:
- `src/touch_input.hpp` / `src/touch_input.cpp` — gesture detection
- `src/imgui_layer.hpp` / `src/imgui_layer.cpp` — direct touch feed,
  `touch_drag_scroll()`, render backend
- `src/game.cpp` — main event loop, ImGui window layout, event
  coordinate conversion, touch action dispatch

### GNU libiconv

Bionic's `iconv` does not support CP932 / Shift JIS, which the game
requires for Japanese text. The project statically links GNU libiconv
1.19 built for arm64‑v8a (see `third_party/build-libiconv-android.sh`).
On Android the CMake build pulls in a local static import instead of
`find_package(Iconv)`.

### FFmpeg

FFmpeg 7.1.1 is cross-compiled as a set of shared libraries
(`libavcodec`, `libavformat`, `libavutil`, `libswscale`,
`libswresample`, `libavdevice`, `libavfilter`). The Gradle build
copies the `.so` files into the APK's `jniLibs` via a
`copyFfmpegJniLibs` task.  pkg-config `.pc` files in
`third_party/ffmpeg-android/lib/pkgconfig` let the CMake build
discover them with `pkg_check_modules`.

### Fonts

The APK bundles two font files under `assets/fonts/`:
- **Noto Sans Regular** — used by ImGui for the config UI and
  player-name entry.
- **Liberation Serif Regular** — used as the in-game modern font
  fallback.

`fontconfig` is disabled on Android (`find_package` is skipped).
Font paths are injected at compile time via `TH2_ANDROID_FONT_PATH`
and `TH2_ANDROID_IMGUI_FONT_PATH`.

### Activity lifecycle

`GameActivity` extends `org.libsdl.app.SDLActivity` and runs
immersive fullscreen with `keepScreenOn`. The native side handles
pause/resume by resetting the render state (upscaler targets, shake
target, title mask, ImGui font atlas) and blocking on pause via
`SDL_HINT_ANDROID_BLOCK_ON_PAUSE`.

### Config persistence

Config writes are atomic (temp file + rename) and a background sync
is triggered after each save.  `android:allowBackup` is set to
`"false"` in the manifest to avoid stale configs being restored on
reinstall.

[th2xr-portable]: https://github.com/ripdog/th2xr-portable
[SDL releases]: https://github.com/libsdl-org/SDL/releases
[vcpkg]: https://github.com/microsoft/vcpkg
