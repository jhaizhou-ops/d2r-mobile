#!/usr/bin/env bash
# 运行 quick 验证配方；依据同目录验证调度器的步骤约定。
# Run the quick verification recipe under the sibling verification scheduler's step contract.
set -euo pipefail
cd "$VERIFY_ROOT/web"
# 在安装依赖或构建运行时前检查共享文字表及其派生数据来源。
# Check shared UI text and derived-data provenance before dependency or runtime builds.
node toolchain/verify-asset-sources.mjs hud
node toolchain/verify-asset-sources.mjs overlay
node toolchain/verify-asset-sources.mjs art-recipes
node toolchain/verify-asset-sources.mjs asset-transforms
python3 toolchain/texture-spike/package-encoder.py
bun install --frozen-lockfile
# 资产来源包含已安装的 hash-wasm；冻结安装后、昂贵编译前校验全部来源。
# Asset sources include installed hash-wasm; verify every source after frozen install, before expensive builds.
node toolchain/asset-runtime-js/build.mjs --verify-sources
bun audit --audit-level=high
bun run build
bun run format:docs:check
bun run format:workflows:check
if [[ "${GITHUB_ACTIONS:-}" == true ]]; then
  python3 -m venv "$RUNNER_TEMP/style-tools"
  "$RUNNER_TEMP/style-tools/bin/pip" install --index-url https://pypi.org/simple --only-binary=:all: ruff==0.14.0 clang-format==21.1.2
  export PATH="$RUNNER_TEMP/style-tools/bin:$PATH"
else
  if [[ "$(ruff --version 2>/dev/null || true)" != "ruff 0.14.0" ]] || [[ "$(clang-format --version 2>/dev/null || true)" != "clang-format version 21.1.2" ]]; then
    echo "Install ruff==0.14.0 and clang-format==21.1.2 into an isolated environment and add its bin directory to PATH; see web/docs/build-policy.md." >&2
    exit 1
  fi
fi
bash scripts/check-authored-style.sh
node verification/preview-manifest.mjs
