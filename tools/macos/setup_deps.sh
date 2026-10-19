#!/bin/bash
# Build OpenBA's shader converter (libairconv.dylib) from source: DXMT + LLVM 15.0.7.
#   tools/macos/setup_deps.sh [--jobs N] [--dry-run] [--only dxmt|llvm|airconv]
# Idempotent: finished steps are skipped. Environment:
#   DXMT_SRC   where DXMT is cloned (default third_party/dxmt_src)
# Result: third_party/airconv_build/build/libairconv.dylib (LGPL, loaded at run time via dlopen).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DXMT_REPO=https://github.com/3Shain/dxmt
DXMT_COMMIT=50102f994b406f58f56d5d09f2ad20f9b90390a2
LLVM_TAG=llvmorg-15.0.7
DXMT_SRC="${DXMT_SRC:-$ROOT/third_party/dxmt_src}"; [[ "$DXMT_SRC" = /* ]] || DXMT_SRC="$ROOT/$DXMT_SRC"
LLVM_SRC="$ROOT/third_party/llvm15_arm64/source"
LLVM_BUILD="$ROOT/third_party/llvm15_arm64/build"
AC_DIR="$ROOT/third_party/airconv_build"
AC_BUILD="$AC_DIR/build"
DYLIB="$AC_BUILD/libairconv.dylib"
JOBS=$(( $(sysctl -n hw.ncpu 2>/dev/null || echo 4) / 2 )); (( JOBS >= 1 )) || JOBS=1
DRY=0; ONLY=""
while (( $# )); do
  case "$1" in
    --jobs) JOBS="${2:?--jobs needs a number}"; shift 2;;
    --dry-run) DRY=1; shift;;
    --only) ONLY="${2:?--only needs dxmt, llvm or airconv}"; shift 2;;
    -h|--help) sed -n '2,8p' "$0"; exit 0;;
    *) echo "unknown option: $1"; exit 2;;
  esac
done
want() { [[ -z "$ONLY" || "$ONLY" = "$1" ]]; }

# ---- tools -----------------------------------------------------------------
missing=()
need() { command -v "$1" >/dev/null 2>&1 || missing+=("$2"); }
need cmake "brew install cmake"; need ninja "brew install ninja"; need meson "brew install meson"
need python3 "brew install python"; need git "xcode-select --install   (or brew install git)"
xcode-select -p >/dev/null 2>&1 || missing+=("xcode-select --install   (Xcode command line tools)")
xcrun --find metal >/dev/null 2>&1 || missing+=("Metal compiler missing: install Xcode, then: xcodebuild -downloadComponent MetalToolchain")
if (( ${#missing[@]} )); then
  echo "Missing prerequisites. Install them, then run this again:"; printf '  %s\n' "${missing[@]}"
  (( DRY )) || exit 1
fi

# ---- step state ------------------------------------------------------------
dxmt_done()   { [[ -f "$DXMT_SRC/build/compile_commands.json" && -f "$DXMT_SRC/build/src/airconv/darwin/libairconv.a.p/air_msad.h" ]]; }
llvm_done()   { [[ -f "$LLVM_BUILD/lib/libLLVMPasses.a" && -f "$LLVM_BUILD/lib/libLLVMBitWriter.a" ]]; }
airconv_done(){ [[ -f "$DYLIB" ]]; }
plan() { # name label done-fn
  if "$2"; then echo "  [skip] $3 (already done)"; else echo "  [todo] $3"; fi
}
if (( DRY )); then
  echo "Planned steps (jobs=$JOBS, DXMT_SRC=$DXMT_SRC):"
  want dxmt    && plan dxmt dxmt_done "1. clone DXMT $DXMT_COMMIT + meson setup + generate shader headers"
  want llvm    && plan llvm llvm_done "2. clone LLVM $LLVM_TAG (shallow) + cmake configure + build bitwriter/passes (LONG)"
  want airconv && plan airconv airconv_done "3. configure.py + cmake build of libairconv.dylib"
  echo "Output: $DYLIB"; exit 0
fi

# ---- 1. DXMT ---------------------------------------------------------------
if want dxmt; then
  if dxmt_done; then echo "== DXMT: already prepared"; else
    if [[ ! -d "$DXMT_SRC/.git" ]]; then
      echo "== DXMT: cloning $DXMT_REPO into $DXMT_SRC"
      mkdir -p "$DXMT_SRC"; git -C "$DXMT_SRC" init -q
      git -C "$DXMT_SRC" remote add origin "$DXMT_REPO"
      git -C "$DXMT_SRC" fetch -q --depth 1 origin "$DXMT_COMMIT"
      git -C "$DXMT_SRC" checkout -q FETCH_HEAD
    fi
    [[ "$(git -C "$DXMT_SRC" rev-parse HEAD)" = "$DXMT_COMMIT" ]] || echo "warning: $DXMT_SRC is not at $DXMT_COMMIT"
    git -C "$DXMT_SRC" submodule update -q --init --depth 1 include/native/directx
    echo "== DXMT: meson setup + generated shader headers"
    # native_llvm_path only has to exist for meson's configure step; airconv itself is built by our own CMake.
    mkdir -p "$LLVM_BUILD"
    [[ -f "$DXMT_SRC/build/build.ninja" ]] || meson setup "$DXMT_SRC/build" "$DXMT_SRC" -Dbuildtype=release -Dnative_llvm_path="$LLVM_BUILD"
    ninja -C "$DXMT_SRC/build" src/airconv/darwin/libairconv.a.p/air_msad.h \
      src/airconv/darwin/libairconv.a.p/air_samplepos.h src/airconv/darwin/libairconv.a.p/air_tessellation.h
  fi
fi

# ---- 2. LLVM ---------------------------------------------------------------
if want llvm; then
  if llvm_done; then echo "== LLVM: already built"; else
    echo "== LLVM $LLVM_TAG: this step takes a LONG time (a few hundred compile steps; about 5 minutes"
    echo "   on the Mac it was measured on with 4 jobs, much longer on older or smaller Macs). Leave it running."
    [[ -d "$LLVM_SRC/llvm" ]] || git clone -q --depth 1 --branch "$LLVM_TAG" https://github.com/llvm/llvm-project.git "$LLVM_SRC"
    cmake -S "$LLVM_SRC/llvm" -B "$LLVM_BUILD" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS= \
      -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
      -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
      -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF
    cmake --build "$LLVM_BUILD" --target LLVMPasses LLVMBitWriter llvm-config -j "$JOBS"
  fi
fi

# ---- 3. airconv ------------------------------------------------------------
if want airconv; then
  if airconv_done; then echo "== airconv: already built"; else
    dxmt_done || { echo "DXMT not prepared; run without --only or with --only dxmt first"; exit 1; }
    llvm_done || { echo "LLVM not built; run without --only or with --only llvm first"; exit 1; }
    echo "== airconv: generating CMakeLists.txt from DXMT's compile_commands.json"
    # configure.py hard-codes the DXMT path and its own directory; patch a throwaway copy, leave the original alone.
    GEN="$(mktemp -d)/configure_gen.py"
    sed -e "s|^root=.*|root=Path('$AC_DIR')|" \
        -e "s|^src=Path(.*|src=Path('$DXMT_SRC')|" "$AC_DIR/configure.py" > "$GEN"
    python3 "$GEN" --llvm "$LLVM_BUILD"
    cmake -S "$AC_DIR" -B "$AC_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
    cmake --build "$AC_BUILD" -j "$JOBS"
  fi
fi

[[ -z "$ONLY" || "$ONLY" = airconv ]] && { [[ -f "$DYLIB" ]] && echo "libairconv.dylib is at: $DYLIB" || echo "libairconv.dylib not built yet"; }
