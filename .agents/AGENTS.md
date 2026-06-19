# Agent Instructions — Aquaplus Port Project

## Animations: NEVER tie to framerate

Time passes at a fixed rate. Frame rate does not. Use `std::chrono::steady_clock` for
anything that needs to animate, blink, pulse, or cycle. Frame-counter division is always
wrong because vsync, system load, and display refresh rates vary unpredictably.

**Bad** (frame-counting):
```cpp
const int frame = (frame_counter_ / 4) % 30;  // breaks at any other framerate
```

**Good** (time-based):
```cpp
const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
const int frame = (ms / 33) % 30;  // correct at any framerate
```

If porting from the original engine which uses `GlobalCount/N%M` at 60fps, convert to
milliseconds. The formula is: `(ms / (N * 1000/60)) % M` or equivalently `(ms * 60 / N / 1000) % M`.

## When the user says "validate against the original"

They mean it. Read the original source code in `/ToHeart2/`, `/鎖/`, `/TtT/`, etc. Do not
guess. Do not assume. The original engines are right there in the repo.

## Original game architecture

- `ToHeart2/` — ToHeart2 XRATED engine (primary port target)
- `鎖/` — Kusari engine (similar architecture, sometimes helpful for comparison)
- `src/` — Modern SDL3 C++20 reimplementation

## Codebase conventions

- C++20, SDL3, CMake
- Do not add legacy compatibility, migrations, fallback lookup paths, or
  old-format handling unless the user explicitly requests it. This project has
  no external users yet; prefer one clean current behavior even when it
  intentionally breaks development saves or generated files.
- No external test framework — tests are standalone `int main()` returning 0 on pass
- Libraries defined in `CMakeLists.txt` with `add_library()` and linked explicitly
- All game state lives in the anonymous-namespace `Game` class in `game.cpp`
- Asset loading: archives are KCAP or LAC format, accessed via `th2::Archive`
- Images: `.tga` and `.bmp` via `th2::load_image()`, converted to SDL textures
- Audio: `.wav`, `.ogg` via `th2::decode_audio()`, played through SDL audio streams
- Avoid creating over-long files. 1000 lines is the limit. Once over that, break into new cpp files.
- 

## Save file format

Little-endian binary with explicit `write_u32`/`read_u32` helpers. No struct dumps.
Versioned. File naming: `save_XX.sav`. Roundtrip tests in `save_test.cpp`.

## UI Assets

All UI sprites are in `game-data/GRP.PAK`:
- `sys0000.tga` / `sys0001.tga` — sidebar scroll track and buttons
- `sys0010.tga` / `sys0011.tga` — click-to-continue cursor (wait / stop)
- `sys0100.tga` / `sys0110.tga` / `sys0111.tga` — system menu
- `sys02xx.tga` / `sys03xx.tga` — save/load screens
