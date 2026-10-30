#!/usr/bin/env bash
# 基于官方固定源码与已有 C++ 编译器，仅验证自造图像的原生桥接，不安装工具。
# Uses official pinned source and an installed C++ compiler for synthetic native bridge checks; installs nothing.
set -euo pipefail
cd "$(dirname "$0")"
PIN=2ff200e08c98a61775351687ea2b5d38e455ad8b
CXX=${CXX:-c++}
command -v "$CXX" >/dev/null
mkdir -p .build
if [[ ! -d .build/source/.git ]]; then
  git init -q .build/source
  git -C .build/source fetch --depth 1 https://github.com/ARM-software/astc-encoder.git "$PIN"
  git -C .build/source checkout --detach FETCH_HEAD
fi
test "$(git -C .build/source rev-parse HEAD)" = "$PIN"
test -z "$(git -C .build/source status --porcelain)"
# 直接读取已验证上游静态库源文件清单，避免编译 CLI 或下载二进制。
# Read the verified upstream static-library source list; do not compile CLI or download binaries.
mapfile -t sources < <(sed -n '/^add_library(${ASTCENC_TARGET}-static/,/^target_include_directories/p' \
  .build/source/Source/cmake_core.cmake | sed -n 's/^[[:space:]]*\(astcenc_[a-z_]*\.cpp\)).*/.build\/source\/Source\/\1/p; s/^[[:space:]]*\(astcenc_[a-z_]*\.cpp\)$/.build\/source\/Source\/\1/p')
test "${#sources[@]}" -eq 22
"$CXX" --version > .build/native-compiler-version.txt
"$CXX" -std=c++14 -O1 -pthread -ffp-contract=off \
  -DASTCENC_NEON=0 -DASTCENC_SVE=0 -DASTCENC_SSE=0 -DASTCENC_AVX=0 \
  -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -I.build/source/Source \
  "${sources[@]}" bridge.cpp native-smoke.cpp -o .build/native-smoke
.build/native-smoke | tee .build/native-smoke-result.txt
