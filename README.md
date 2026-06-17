# th2xr-portable

A portable, cross-platform reimplementation of the ToHeart2 XRATED visual novel engine.

Runs on Linux, macOS, Windows, and Android using SDL3.

## How the project came about

Thanks to the GPL version of ToHeart2 ADPlus being released by Aquaplus,
the game source code was published. th2xr-portable is a ground-up rewrite
of that original source, meticulously done to behave identically to the
original binary, while porting from DX8 to SDL3 and C++20.

Because the original source depended on a lot of unportable Win32/DX8 code,
we started from scratch with a fresh C++20 codebase, using the published
source as a reference for data formats and engine behavior. The result is a
modern, cross-platform engine that can run the original game data on any
platform supported by SDL3.

## Prerequisites

- CMake 3.20+
- C++20 compiler (GCC 13+, Clang 17+, MSVC 19.40+)
- SDL3, SDL3_ttf, fontconfig
- FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample)
- glslangValidator

See [HACKING.md](HACKING.md#desktop-build) for platform-specific
dependency installation and [HACKING.md](HACKING.md#android-build)
for the Android build.

## Building

```bash
# Linux / macOS
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -- -j$(nproc)

# Windows (MSVC, with vcpkg)
cmake -S . -B build -DBUILD_TESTING=ON `
  -DCMAKE_TOOLCHAIN_FILE="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

## Running tests

```bash
ctest --test-dir build
```

## Running the engine

Place your ToHeart2 XRATED game data in a `game-data/` directory next to the
binary—the engine looks there by default:

```
th2xr-portable/
├── build/
│   └── toheart2          (or toheart2.exe)
└── game-data/
    ├── bgm.PAK
    ├── GRP.PAK
    ├── SDT.PAK
    ├── SE.PAK
    ├── voice.pak
    └── ...
```

Then simply run:
```bash
./build/toheart2
```

You can also point to an alternate data directory or launch directly into a
scenario:
```bash
./build/toheart2 /path/to/data
./build/toheart2 /path/to/data --scenario 010301000.sdt
```

## Android

See [HACKING.md](HACKING.md#android-build) for build instructions and
[HACKING.md](HACKING.md#android-architecture) for architecture details.

### Game data

On first launch the app opens the Android **Storage Access Framework**
document tree picker. Select the folder that contains `TOHEART2.EXE`
(typically the root of your ToHeart2 XRATED installation). The game
data is imported into internal storage and persists across restarts.

### Touch controls

The touch interface provides two-finger and swipe gestures as shortcuts
to common actions. Taps act as left mouse clicks for menus, choices,
and game advance.

| Gesture | Action |
|---|---|
| Single-finger tap | Left click (advance text, select menu item, confirm choice) |
| Swipe down | Open the backlog; additional swipes scroll older entries |
| Swipe up | Scroll newer entries; at the newest, close the backlog |
| Two-finger tap | Close the backlog; if not in backlog, hide/show the textbox |
| Swipe right (quick) | Toggle skip mode (respects the "skip-unread" setting) |
| Swipe right (hold) | Hold to skip unconditionally, release to stop |
| Android back button | Open the system menu in-game; close save/load menus |

### Configuration panel

Open with a two-finger tap when the textbox is hidden, or via the
system menu. On small screens the panel and the player-name entry
screen are full-size with drag-to-scroll.  Controls are padded and
spaced for finger-friendly operation.

## Automated route soak

The soak explorer drives the normal game runtime through text, choices, maps,
effects, movies, and endings. It persists newly discovered decision paths and
resumes unfinished work after interruption:

```bash
# Explore one route and store progress in logs/soak/
./build/toheart2 game-data --soak

# Explore up to 20 queued routes in this process
./build/toheart2 game-data --soak --soak-runs 20

# Use a separate state/report directory
./build/toheart2 game-data --soak-state /tmp/th2-soak --soak-runs 20
```

Soak configuration and completion flags are isolated from the normal player
configuration. `state.txt` contains the persistent decision tree and
`runs.log` records completed or failed paths.

The currently known remaining run count can be inspected without loading the
game:

```bash
python3 tools/soak_status.py
python3 tools/soak_status.py --json
```

The count is the current exploration frontier. It can increase when a queued
run reaches a choice or map branch that has not previously been discovered.

Independent routes can be processed concurrently:

```bash
# Run up to 20 routes using four engine processes.
python3 tools/soak_parallel.py --workers 4 --runs 20
```

If a decision baseline was recorded from invalid engine state, stop the
parallel coordinator and preview pruning that decision and its descendants:

```sh
python3 tools/soak_prune.py 1,0,0 --state logs/soak
python3 tools/soak_prune.py 1,0,0 --state logs/soak --apply
```

The decision prefix is queued again so its current options and all descendant
routes are rediscovered without discarding unrelated campaign progress.

Each worker receives an isolated state and configuration directory. The
coordinator leases distinct paths, waits for the batch, then atomically merges
new branches and results into `logs/soak/state.txt`. Only one coordinator or
single-process soak should use a campaign directory at a time. Four workers is
the conservative default because every process also owns SDL GPU resources.

## License

This project is licensed under the GNU General Public License v2.0. See
[LICENSE.txt](LICENSE.txt) for details.

## Acknowledgements

The [original ToHeart2 source code](https://github.com/autch/aquaplus_gpl)
was released by Aquaplus under the GPLv2. This project contains a complete
rewrite of that source targeting modern platforms.

This project uses:
- [SDL3](https://libsdl.org/) for cross-platform windowing, input, and rendering
- [Dear ImGui](https://github.com/ocornut/imgui) for the user interface
- [Anime4K](https://github.com/bloc97/Anime4K) for real-time upscaling
- [FFmpeg](https://ffmpeg.org/) for video playback
