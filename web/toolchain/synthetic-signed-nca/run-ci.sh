#!/usr/bin/env bash
# 在隔离构建副本中运行合成签名 NCA 验证；依据 web/toolchain/synthetic-signed-nca 的模块与调用约定。
# Run synthetic signed NCA verification in an isolated build copy using the module and calling contracts in web/toolchain/synthetic-signed-nca.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PRODUCTION=$(cd "$HERE/../nsp-extract-spike" && pwd)
# 此临时目录不在生产 dist 目录下，也不会上传。
# This scratch directory is never under the production dist tree or uploaded.
WORK=$(mktemp -d "${RUNNER_TEMP:-/tmp}/synthetic-signed-nca.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
node --test "$HERE/generate.test.mjs"
node "$HERE/generate.mjs" "$WORK/fixtures"
NSP_BUILD_DIR="$WORK/prod-build" NSP_OUTPUT_DIR="$WORK/prod-dist" bash "$PRODUCTION/build-wasm.sh"
mkdir "$WORK/test-source"
# 仅复制源文件；已有输出或缓存构建不得进入测试构建。
# Copy source files only. No existing output or cached build can enter the test build.
find "$PRODUCTION" -maxdepth 1 -type f -exec cp {} "$WORK/test-source/" \;
cp "$HERE/inject-test-root.py" "$WORK/inject-test-root.py"
# 仅供测试的准备钩子只存在于此临时构建副本中。
# Test-only preparation hook exists exclusively in this temporary build copy.
cat >> "$WORK/test-source/prepare-source.py" <<'PY'
import subprocess
subprocess.run([sys.executable, str(Path(__file__).parent.parent / 'inject-test-root.py'), str(root), str(Path(__file__).parent.parent / 'fixtures/test-root.bin')], check=True)
PY
NSP_BUILD_DIR="$WORK/test-build" NSP_OUTPUT_DIR="$WORK/test-dist" bash "$WORK/test-source/build-wasm.sh"
# 确保实际认证边界及上游代码一致，仅一个模数除外。
# Ensure the actual authenticated boundary and all upstream code except one modulus match.
cmp "$PRODUCTION/boundary.c" "$WORK/test-source/boundary.c"
cmp "$PRODUCTION/build-wasm.sh" "$WORK/test-source/build-wasm.sh"
python3 "$HERE/verify-build-isolation.py" "$WORK"
node "$HERE/prove.mjs" "$WORK/prod-dist/hactool-boundary.mjs" "$WORK/test-dist/hactool-boundary.mjs" "$WORK/fixtures"
