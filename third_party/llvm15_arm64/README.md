# LLVM 15.0.7 for native airconv

Source: shallow `llvmorg-15.0.7` checkout from https://github.com/llvm/llvm-project.
No LLVM patches. Build is Release, arm64, no target backends, tests or docs.
Only the `bitwriter passes` dependency closure and llvm-config are built.

```sh
git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git third_party/llvm15_arm64/source
cmake -S third_party/llvm15_arm64/source/llvm -B third_party/llvm15_arm64/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DLLVM_TARGETS_TO_BUILD= -DLLVM_ENABLE_PROJECTS= \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF
cmake --build third_party/llvm15_arm64/build --target LLVMPasses LLVMBitWriter llvm-config -j4
python3 third_party/airconv_build/configure.py --llvm third_party/llvm15_arm64/build
cmake -S third_party/airconv_build -B third_party/airconv_build/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build third_party/airconv_build/build -j4
```

LLVM's license is in `source/llvm/LICENSE.TXT`; the dynamically loaded DXMT
library is separately LGPL (see `../airconv_build/COPYING.LIB`).
