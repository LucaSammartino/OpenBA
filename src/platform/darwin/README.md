# src/platform/darwin — macOS twins

Empty until the macOS port lands. The rule this directory follows:

- On Apple, CMake globs `src/platform/posix/*.cpp`, `common/`, `sdl/` (as on Linux)
  **and** `src/platform/darwin/*.cpp` (`CMakeLists.txt`, the `PLATFORM_SRCS` block).
- A file `darwin/<name>_darwin.cpp` **replaces** `posix/<name>_posix.cpp` whole-file:
  the POSIX twin is removed from the build and the Darwin twin is compiled instead.
  There is no per-function mixing — the Darwin twin defines every function its POSIX
  twin did (copy the POSIX file and change what macOS needs).
- A Darwin file with no POSIX twin of the same `<name>` is simply added.
- The unit test applies the same substitution (`CMakeLists.txt`, the `UNIT_TREE_SRCS`
  twin block), so `tools/build.sh unit` on a Mac links the same twins as the game.
- Seams first, then twins: platform-specific behaviour goes behind a `Platform::` hook
  declared in `src/platform/common/platform_*.h` (for example `platform_app.h`,
  whose win / posix bodies are empty), never behind `#if defined(__APPLE__)` in
  `src/app`, `src/game` or `src/engine`.

`src/diag/posix` has no Darwin directory: it is globbed only inside the Linux `OpenBA`
target (`DIAG_POSIX_SRCS`), not through `PLATFORM_SRCS`.
