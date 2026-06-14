# th2xr-portable

A portable, cross-platform reimplementation of the ToHeart2 XRATED visual novel engine.

Runs on Linux, macOS, and Windows using SDL3.

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
- SDL3
- SDL3_ttf
- FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample)
- libsndfile
- fontconfig
- glslangValidator (for shader compilation)

### Installing dependencies

**Ubuntu 24.04+** — SDL3 is not yet packaged, so it is built from source:
```bash
# System packages
sudo apt install cmake g++ libsndfile-dev libfontconfig-dev \
  libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev glslang-tools \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxfixes-dev libxi-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libdrm-dev libgbm-dev \
  libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev

# Build SDL3
git clone https://github.com/libsdl-org/SDL.git --depth 1 --branch release-3.4.10
cmake -S SDL -B SDL/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL/build -- -j$(nproc)
sudo cmake --install SDL/build

# Build SDL3_ttf
git clone https://github.com/libsdl-org/SDL_ttf.git --depth 1 --branch release-3.2.2
cmake -S SDL_ttf -B SDL_ttf/build -DCMAKE_BUILD_TYPE=Release
cmake --build SDL_ttf/build -- -j$(nproc)
sudo cmake --install SDL_ttf/build
```

**macOS**:
```bash
brew install cmake sdl3 sdl3_ttf libsndfile fontconfig ffmpeg glslang
```

**Windows** — use vcpkg:
```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install --triplet x64-windows `
  pkgconf sdl3 sdl3-ttf libsndfile fontconfig ffmpeg glslang
```

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

### Automated route soak

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
