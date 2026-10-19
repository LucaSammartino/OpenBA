#!/bin/bash
# Build dist/OpenBA.app from a finished build of the OpenBA target.
#   tools/macos/make_app.sh [BUILD_DIR] [OUT_APP]
# BUILD_DIR defaults to build/mac-arm64. The OpenBA target is ALWAYS configured and built
# (incremental) before packing, so the app is never stale. If libairconv.dylib is missing,
# tools/macos/setup_deps.sh runs first (clones DXMT, builds LLVM + airconv; long the first time).
# OUT_APP defaults to dist/OpenBA.app.
# The app contains NO game data (see docs/macos/README_MAC.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build/mac-arm64}"; [[ "$BUILD" = /* ]] || BUILD="$ROOT/$BUILD"
APP="${2:-$ROOT/dist/OpenBA.app}"; [[ "$APP" = /* ]] || APP="$ROOT/$APP"
MINOS=11.0
AIRCONV="$ROOT/third_party/airconv_build/build/libairconv.dylib"

if [[ ! -f "$AIRCONV" ]]; then
  echo "== libairconv.dylib missing: running setup_deps.sh (first run is slow)"
  "$ROOT/tools/macos/setup_deps.sh"
fi
[[ -f "$AIRCONV" ]] || { echo "missing $AIRCONV after setup_deps.sh"; exit 1; }
echo "== building OpenBA in $BUILD"
cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DBANISHED_USE_CCACHE=OFF \
  -DBANISHED_FLAVOUR="${FLAVOUR:-release}"
cmake --build "$BUILD" -t OpenBA -j3
file "$BUILD/OpenBA" | grep -q arm64 || { echo "$BUILD/OpenBA is not arm64"; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/Resources"

# 1. engine + launcher
cp "$BUILD/OpenBA" "$APP/Contents/MacOS/OpenBA-engine"
xcrun clang++ -std=c++17 -fobjc-arc -arch arm64 -mmacosx-version-min=$MINOS -O1 \
  -framework Cocoa -o "$APP/Contents/MacOS/OpenBA" "$ROOT/packaging/macos/launcher.mm"

# 2. shader converter dylib; its only deps are system libs (otool -L below proves it)
cp "$AIRCONV" "$APP/Contents/Frameworks/libairconv.dylib"
chmod u+w "$APP/Contents/Frameworks/libairconv.dylib"
install_name_tool -id "@rpath/libairconv.dylib" "$APP/Contents/Frameworks/libairconv.dylib"
install_name_tool -add_rpath "@executable_path/../Frameworks" "$APP/Contents/MacOS/OpenBA-engine" 2>/dev/null || true
if otool -L "$APP/Contents/MacOS/OpenBA-engine" "$APP/Contents/Frameworks/libairconv.dylib" \
     | grep -vE '^\S|/System/Library/|/usr/lib/|@rpath/libairconv' | grep -q .; then
  echo "unexpected non-system dependency:"; otool -L "$APP/Contents/MacOS/OpenBA-engine" "$APP/Contents/Frameworks/libairconv.dylib"; exit 1
fi

# 3. icon (source is only 48x48; Lanczos-upscaled with PIL when available, else sips)
ICONSET="$(mktemp -d)/OpenBA.iconset"; mkdir -p "$ICONSET"
if python3 -c "import PIL" 2>/dev/null; then
  python3 - "$ROOT/images/ba_icon.png" "$ICONSET" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
for s in (16, 32, 64, 128, 256, 512, 1024):
    im.resize((s, s), Image.LANCZOS).save("%s/t%d.png" % (sys.argv[2], s))
PY
else
  for s in 16 32 64 128 256 512 1024; do sips -z $s $s "$ROOT/images/ba_icon.png" --out "$ICONSET/t$s.png" >/dev/null; done
fi
cp "$ICONSET/t16.png" "$ICONSET/icon_16x16.png";      cp "$ICONSET/t32.png" "$ICONSET/icon_16x16@2x.png"
cp "$ICONSET/t32.png" "$ICONSET/icon_32x32.png";      cp "$ICONSET/t64.png" "$ICONSET/icon_32x32@2x.png"
cp "$ICONSET/t128.png" "$ICONSET/icon_128x128.png";   cp "$ICONSET/t256.png" "$ICONSET/icon_128x128@2x.png"
cp "$ICONSET/t256.png" "$ICONSET/icon_256x256.png";   cp "$ICONSET/t512.png" "$ICONSET/icon_256x256@2x.png"
cp "$ICONSET/t512.png" "$ICONSET/icon_512x512.png";   cp "$ICONSET/t1024.png" "$ICONSET/icon_512x512@2x.png"
rm "$ICONSET"/t*.png
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/OpenBA.icns"

# 4. Info.plist
cp "$ROOT/packaging/macos/Info.plist" "$APP/Contents/Info.plist"

# 5. ad-hoc signature (inside-out; --deep is also passed as the brief asks)
codesign --force --sign - "$APP/Contents/Frameworks/libairconv.dylib"
codesign --force --sign - "$APP/Contents/MacOS/OpenBA-engine"
codesign --force --sign - "$APP/Contents/MacOS/OpenBA"
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict --verbose=2 "$APP"

# 6. no game data may ever be inside the bundle
if find "$APP" \( -name '*.pkg' -o -name '*.pkm' -o -name 'WinData' \) | grep -q .; then
  echo "game data found inside the bundle, refusing"; rm -rf "$APP"; exit 1
fi
echo "built $APP ($(du -sh "$APP" | cut -f1))"
