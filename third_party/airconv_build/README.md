# Native dynamically loaded airconv

DXMT is read unchanged from the supplied sibling
`third_party/dxmt_src` (cloned by `tools/macos/setup_deps.sh`). No DXMT implementation source is copied into
OpenBA's `src/`. Its LGPL text is `COPYING.LIB`. OpenBA loads the separate arm64
`libairconv.dylib` using dlopen; `OPENBA_AIRCONV=/path/libairconv.dylib` permits
replacement. No LGPL implementation is statically linked into the executable.
LLVM's permissively licensed static components are inside the separate dylib.

Build LLVM first using `../llvm15_arm64/README.md`, then:

```sh
python3 third_party/airconv_build/configure.py --llvm third_party/llvm15_arm64/build
cmake -S third_party/airconv_build -B third_party/airconv_build/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build third_party/airconv_build/build -j4
```

The generator reads the external DXMT build's compile_commands.json to select
21 airconv and DXBCParser translation units and generated shader headers. It
uses LLVM CMake component mapping for `bitwriter passes`, not every archive.
No DXMT patches were necessary. This is a local dependency build, not yet a
redistributable package: redistribution must include the corresponding LGPL
source/build material and notices, with any future patches retained separately
under `third_party/patches/`.

`metal-shader-check` exercises the same converter as the backend without opening
a window. 465 extracted retail blobs converted and loaded successfully on arm64.
The historical x86_64-link failure is superseded by this successful native build.
