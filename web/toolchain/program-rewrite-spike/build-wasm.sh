#!/usr/bin/env bash
# 校验来源并构建程序改写 WASM 桥接；依据 web/toolchain/program-rewrite-spike 的模块与调用约定。
# Verify sources and build the program-rewrite WASM bridge using the module and calling contracts in web/toolchain/program-rewrite-spike.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p deps dist/licenses dist/js
# 在下载任何依赖或编译前拒绝来源漂移。
# Reject source drift before any dependency download or compilation.
(cd ../../.. && sha256sum -c web/toolchain/program-rewrite-spike/upstream-source.sha256)
python3 audit-apple-source.py
python3 audit-js-source.py
python3 audit-source.py
fetch() {
  local name="$1" repo tag commit
  read -r repo tag commit < <(python3 -c 'import json,sys; d=json.load(open("toolchain-lock.json"))[sys.argv[1]]; print(d["repository"], d["tag"], d["commit"])' "$name")
  if [ ! -d "deps/$name/.git" ]; then git clone --depth 1 --branch "$tag" "$repo" "deps/$name"; fi
  test "$(git -C "deps/$name" rev-parse HEAD)" = "$commit"
}
fetch capstone
fetch lz4
if ! command -v emcc >/dev/null; then
  fetch emsdk
  deps/emsdk/emsdk install 4.0.20
  deps/emsdk/emsdk activate 4.0.20
  source deps/emsdk/emsdk_env.sh
fi
emcc --version | head -1 | grep -F '4.0.20'
emcmake cmake -S deps/capstone -B deps/capstone-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCAPSTONE_BUILD_TESTS=OFF -DCAPSTONE_BUILD_CSTOOL=OFF \
  -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON
cmake --build deps/capstone-build --parallel 2
emcc --no-entry -O2 -I deps/capstone/include bridge.c \
  deps/capstone-build/libcapstone.a deps/lz4/lib/lz4.c \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=536870912 \
  -sEXPORTED_FUNCTIONS='["_decode_word","_LZ4_decompress_safe","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["ccall","UTF8ToString","HEAPU8"]' \
  -o dist/capstone-lz4.mjs
cp worker.mjs rewrite-core.mjs dist/
for file in js/*.mjs; do
  sed 's@../../../program-release-catalog.mjs@../program-release-catalog.mjs@g' "$file" > "dist/js/$(basename "$file")"
done
cp ../../program-rewriter-adapter.mjs ../../program-release-catalog.mjs ../../guest-framework.mjs dist/
# 保留独立适配器的相对模块路径，并完整复制封闭外壳配置的公开依赖。
# Preserve standalone adapter module paths and copy the closed shell registry's complete public closure.
for file in shell-profiles.mjs shell-v2-evidence.mjs \
  toolchain/shell-release-evidence/v2.0.0.json \
  toolchain/shell-release-evidence/v2.0.0-derived-capacities.json \
  toolchain/shell-release-evidence/v2.0.0-ios18.json \
  toolchain/shell-release-evidence/v2.0.0-ios18-derived-capacities.json; do
  mkdir -p "dist/$(dirname "$file")"
  cp "../../$file" "dist/$file"
done
cp deps/capstone/LICENSE.TXT dist/licenses/capstone.txt
cp deps/lz4/lib/LICENSE dist/licenses/lz4.txt
cp toolchain-lock.json dist/
