#!/usr/bin/env bash
# 构建固定的官方源码，不下载或执行预编译二进制文件。
# Build official pinned source, never download or execute prebuilt binaries.
# 产物用于显式实验选项的可选闭包，构建成功不代表浏览器或设备验证。
# Artifacts serve an explicit experimental optional closure; build success is not browser/device validation.
# 基于官方 Arm 5.0.0 源码及项目固定的 Emscripten 4.0.20。
# Based on official Arm 5.0.0 source and project-pinned Emscripten 4.0.20.
set -euo pipefail
cd "$(dirname "$0")"
PIN=2ff200e08c98a61775351687ea2b5d38e455ad8b
command -v emcmake >/dev/null
command -v emcc >/dev/null
mkdir -p .build
emcc --version > .build/compiler-version.txt
if ! head -n 1 .build/compiler-version.txt | grep -Eq '^emcc .* 4\.0\.20([[:space:]]|$)'; then
  printf '%s\n' '需要官方 Emscripten 4.0.20 / Expected official Emscripten 4.0.20' >&2
  exit 1
fi
if [[ ! -d .build/source/.git ]]; then
  git init -q .build/source
fi
if ! git -C .build/source cat-file -e "$PIN^{commit}" 2>/dev/null; then
  git -C .build/source fetch --depth 1 https://github.com/ARM-software/astc-encoder.git "$PIN"
fi
git -C .build/source checkout --detach "$PIN"
test "$(git -C .build/source rev-parse HEAD)" = "$PIN"
test -z "$(git -C .build/source status --porcelain)"
emcmake cmake -S . -B .build/wasm -DCMAKE_BUILD_TYPE=Release \
  -DASTC_SOURCE="$PWD/.build/source"
cmake --build .build/wasm --target astc-encoder --parallel 2
mkdir -p dist
cp .build/wasm/astc-encoder.{mjs,wasm} dist/
cp .build/source/LICENSE.txt dist/ARM-LICENSE.txt
cp .build/compiler-version.txt dist/
printf '%s\n' "$PIN" > dist/ARM-COMMIT.txt
