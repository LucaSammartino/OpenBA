# Third-party notices (macOS build)

Full licence texts are in the files named below. OpenBA contains no Banished game data.

## DXMT (shader converter "airconv") - LGPL-2.1
- Project: https://github.com/3Shain/dxmt, commit `50102f994b406f58f56d5d09f2ad20f9b90390a2`, used unmodified.
- Licence: GNU LGPL 2.1, `third_party/airconv_build/COPYING.LIB` (and `COPYING.LIB` in the DXMT repository).
- How it is used: built as a **separate shared library**, `libairconv.dylib` (inside the app at
  `OpenBA.app/Contents/Frameworks/`). OpenBA loads it with `dlopen`; no DXMT code is linked into the
  OpenBA executable or copied into `src/`.
- How to replace it: build your own (modified) DXMT airconv as a dylib and point the environment variable
  `OPENBA_AIRCONV=/path/to/libairconv.dylib` at it. OpenBA will load that instead.
- Corresponding source: the commit above, plus `third_party/airconv_build/` (build recipe) in this repository.
  `tools/macos/setup_deps.sh` fetches exactly that commit.

## LLVM 15.0.7 - Apache-2.0 with LLVM Exceptions
- Source: https://github.com/llvm/llvm-project, tag `llvmorg-15.0.7`, unpatched.
- Licence: `third_party/llvm15_arm64/source/llvm/LICENSE.TXT` (present after `setup_deps.sh` has run).
- Use: only the bitwriter and passes components, linked statically **inside** `libairconv.dylib`.

## SDL3 - zlib licence
- `third_party/SDL/LICENSE.txt` (Copyright Sam Lantinga and contributors).

## FreeType - FreeType License (FTL) or GPLv2, at your choice
- `third_party/freetype/LICENSE.TXT`.

## Dear ImGui - MIT
- `third_party/imgui/LICENSE.txt` (Copyright Omar Cornut).
