# th2xr-portable

A portable, cross-platform reimplementation of the ToHeart2 XRATED visual novel engine.

Runs on Linux, macOS, and Windows using SDL3.

## How the project came about

Thanks to the GPL version of ToHeart2 ADPlus being released by Aquaplus,
the game source code was published. th2xr-portable is essentially a ground-up
rewrite of that original source, meticulously done to behave identically
to the original binary, while porting from DX8 to SDL3 and C++20.

Because the original source depended on a lot of unportable Win32/DX8 code,
we started from scratch by doing a clean-room reverse engineering of the
game's data formats and scripting engine, using the published source as a
reference. The result is a modern, cross-platform engine that can run the
original game data on any platform supported by SDL3.

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

On Debian/Ubuntu:
```bash
sudo apt install cmake g++ libsdl3-dev libsdl3-ttf-dev libsndfile-dev \
  libfontconfig-dev libavformat-dev libavcodec-dev libavutil-dev \
  libswscale-dev libswresample-dev glslang-tools
```

On Fedora:
```bash
sudo dnf install cmake gcc-c++ SDL3-devel SDL3_ttf-devel libsndfile-devel \
  fontconfig-devel ffmpeg-devel glslang
```

On macOS:
```bash
brew install cmake sdl3 sdl3_ttf libsndfile fontconfig ffmpeg glslang
```

## Building

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build -- -j$(nproc)
```

## Running tests

```bash
ctest --test-dir build
```

## Running the engine

Point the engine at your ToHeart2 XRATED `game-data` directory:

```bash
./build/toheart2 --data /path/to/toheart2/game-data
```

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
