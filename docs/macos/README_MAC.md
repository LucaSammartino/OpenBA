# OpenBA on macOS (Apple Silicon)

OpenBA is a free re-implementation of the Banished engine. It contains **no game data**:
you need your own copy of Banished (bought on Steam). OpenBA only *reads* from it.

## Requirements
- A Mac with Apple Silicon (M1 or newer), macOS 11 or newer.
- Your own Banished install on this Mac (see "Where is my game?" below).
- To build it yourself: Xcode command line tools, CMake and Ninja.

## Build from source
Prerequisites (Apple Silicon Mac): Xcode (for the Metal compiler) with command line tools, and
`brew install cmake ninja meson python git`. Then, in the `openba_mac` folder:

    tools/macos/make_app.sh

First run: the script notices `libairconv.dylib` is missing and runs `tools/macos/setup_deps.sh`, which
clones DXMT (pinned commit, into `third_party/dxmt_src`; override with `DXMT_SRC=...`), builds LLVM 15.0.7
(the slow part: minutes on a fast Mac, much longer on a small one) and the shader converter, then builds the
engine into `build/mac-arm64` and packs `dist/OpenBA.app`. The engine is rebuilt (incrementally) every time,
so the app is never stale. `setup_deps.sh --dry-run` shows what it would do; `--jobs N` sets parallelism.
Later runs skip finished steps.

The game is never shipped or copied: OpenBA finds your own Banished install at run time (see "Where is my game?").

Licence note: the shader converter is DXMT (LGPL-2.1), a separate `libairconv.dylib` loaded dynamically; you can
replace it by setting `OPENBA_AIRCONV=/path/to/libairconv.dylib`. Source: https://github.com/3Shain/dxmt,
commit `50102f994b406f58f56d5d09f2ad20f9b90390a2`. See `docs/macos/THIRD_PARTY_NOTICES.md`.

## Build the app (once)
Open Terminal in the `openba_mac` folder and run:

    tools/macos/make_app.sh

This builds the engine (takes a while the first time) and creates `dist/OpenBA.app`,
signed ad-hoc (release flavour, -O3; `FLAVOUR=perf tools/macos/make_app.sh` for symbols). A ready zip is `dist/OpenBA-mac-arm64.zip` (SHA-256 in `dist/OpenBA-mac-arm64.zip.sha256`; no game data inside). Unzip and drag it anywhere (Applications, Desktop). `make_app.sh BUILD_DIR OUT.app`
lets you choose the build folder and output.

## Run it
- Double-click `OpenBA.app`, or run `tools/macos/run_openba.sh`.
- First launch: if OpenBA cannot find Banished by itself, a window asks you to pick the
  folder. Choose the `Banished` folder (the one that holds `WinData`) or `WinData` itself.
  The choice is remembered.
- Options for `run_openba.sh`: `--game-dir <folder>`, `--background` (small window that does
  not grab focus; for testing).

## Where is my game? (how OpenBA looks, in this order)
1. `--game-dir <folder>` on the command line.
2. The `BANISHED_DIR` environment variable (a Banished or WinData folder).
3. The folder you picked before (`~/Library/Application Support/OpenBA/gamedir.txt`).
4. Common places, checked automatically:
   - `~/Library/Application Support/Steam/steamapps/common/Banished`
   - Windows-style prefixes under `drive_c/Program Files (x86)/Steam/steamapps/common/Banished`
     (also `Program Files`, and `GOG Games/Banished`) for Porting Kit (Desktop, Applications
     and Application Support), Whisky, CrossOver, Kegworks/Wineskin and `~/.wine`.
5. The folder picker.

A folder counts only if it contains `WinData/data0.pkg`. Nothing is copied or modified in it.
To forget the choice, delete `~/Library/Application Support/OpenBA/gamedir.txt`.

## Where do saves and logs go?
- Saves: `~/Documents/Banished/Save`. Settings (`settings.cfg`): `~/Documents/Banished`.
  Same folders as the original game, so existing saves are found.
- Logs: `~/Library/Application Support/OpenBA/Run/standalone_*.log` (send `standalone_error.log`
  with any bug report). That folder also holds two shortcuts: `WinData` (your game) and
  `OpenBA` (the engine inside the app).
- Compiled shader cache: the system temporary folder (`OpenBA-metal-v1-airconv26`); it is
  rebuilt automatically if it disappears (first start after that is slower).
The app itself is never written to, so its signature stays valid.

## Quit
Cmd+Q or OpenBA menu, Quit OpenBA (checked with System Events: the process exits cleanly).

## Testing without touching your home (developers)
`OpenBA --run-dir <folder>` keeps the Run folder, remembered game folder, saves/settings
(via USERPROFILE) and shader cache under `<folder>`. `OPENBA_BACKGROUND=1` gives a small window
that does not take focus.

## How to report a problem
Send `standalone_error.log` (and `standalone_crash.log` if it is not empty) from the Run folder,
the macOS version and Mac model, and what you did just before.

## Gatekeeper and distribution (honest status)
- The app is **ad-hoc signed** (`codesign --sign -`), not notarised. `spctl --assess` says "rejected" (expected for any non-notarised app); run from the Mac that
  built it, it still opens normally (no quarantine flag).
- If you move it to another Mac by download, AirDrop or chat, macOS marks it as quarantined
  and says it cannot be verified. Workaround: right-click the app, Open, Open (or
  `xattr -dr com.apple.quarantine OpenBA.app`). Do this only for builds you trust.
- Giving it to other people without that warning needs an Apple Developer ID (paid
  developer account), signing with it and notarising (`notarytool`). Not done, not possible
  without that account.

## Known gaps
- The Metal renderer is young: the main menu and a 2000-citizen town render, but AO,
  billboard-shadow rewrite, foliage anti-aliasing and MSAA are off, and shadows, water and
  terrain parity with the Windows game are not confirmed. Resizing the window and changing
  video modes are not fully handled. Details: `METAL_PORT_STATUS.md`.
- Gameplay, saves and long sessions have had little testing on macOS.
- The app icon is upscaled (Lanczos) from a 48x48 picture, so it is still soft.
- Apple Silicon only (the shader converter is arm64).
- The launcher passes unchanged any extra arguments to the engine (for example `-nomods`).
