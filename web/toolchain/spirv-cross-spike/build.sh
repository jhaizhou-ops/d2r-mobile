#!/usr/bin/env bash
# 构建 SPIRV-Cross 浏览器桥接；依据 web/toolchain/spirv-cross-spike 的模块与调用约定。
# Build the SPIRV-Cross browser bridge using the module and calling contracts in web/toolchain/spirv-cross-spike.
set -euo pipefail
cd "$(dirname "$0")"
mode=${1:?Usage: build.sh native|wasm upstream-source build-directory}
source_dir=$(realpath "${2:?}")
build_dir=$(realpath -m "${3:?}")
test "$(git -C "$source_dir" rev-parse HEAD)" = f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7
# 使用已验证上游提交的 UTC 时间，避免配置时钟改变编译器字节；这不是实际构建时间。
# Use the verified upstream commit time in UTC, not the configure clock or actual build time.
SOURCE_DATE_EPOCH=$(git -C "$source_dir" show -s --format=%ct HEAD)
[[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ ]] || { echo "Invalid upstream commit epoch" >&2; exit 1; }
export SOURCE_DATE_EPOCH TZ=UTC
options=(-DCMAKE_BUILD_TYPE=Release -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_CLI=ON -DSPIRV_CROSS_ENABLE_C_API=OFF)
if [[ $mode == native ]]; then
  cmake -S "$source_dir" -B "$build_dir" "${options[@]}"
elif [[ $mode == wasm ]]; then
  emcc --version | head -1 | grep -F '4.0.20'
  emcmake cmake -S "$source_dir" -B "$build_dir" "${options[@]}" \
    '-DCMAKE_CXX_FLAGS=-fwasm-exceptions' \
    "-DCMAKE_EXE_LINKER_FLAGS=-fwasm-exceptions -sMODULARIZE=1 -sEXPORT_ES6=1 -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sSTACK_SIZE=8388608 -sSTACK_OVERFLOW_CHECK=2 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 -sENVIRONMENT=${D2R_EMSCRIPTEN_ENVIRONMENT:-web,worker,node} -sEXPORTED_RUNTIME_METHODS=FS,callMain"
else
  echo "Unknown mode: $mode" >&2; exit 2
fi
cmake --build "$build_dir" --target spirv-cross --parallel 2
if [[ $mode == wasm ]]; then
  cp "$build_dir/spirv-cross.js" wwwroot/spirv-cross.mjs
  cp "$build_dir/spirv-cross.wasm" wwwroot/spirv-cross.wasm
  cp "$source_dir/LICENSE" wwwroot/SPIRV-Cross-LICENSE.txt
  if [[ -f "$source_dir/NOTICE" ]]; then cp "$source_dir/NOTICE" wwwroot/SPIRV-Cross-NOTICE.txt; fi
fi
