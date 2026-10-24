#!/usr/bin/env bash
# 按功能集中验证入口；每个配置保留独立进程和原始步骤。
# Feature-grouped verification entry points; each profile retains its own process and original steps.
case "${1:-}" in
extraction)
# BEGIN PROFILE extraction
#!/usr/bin/env bash
# 运行 extraction 验证配方；依据同目录验证调度器的步骤约定。
# Run the extraction verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Install pinned official Emscripten SDK' . 0 <<'VERIFY_STEP'
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/nsp-emsdk"
test "$(git -C "$RUNNER_TEMP/nsp-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/nsp-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/nsp-emsdk/emsdk" activate 4.0.20
VERIFY_STEP

step step4 'Build pinned upstream and bounded WASM API' . 0 <<'VERIFY_STEP'
source "$RUNNER_TEMP/nsp-emsdk/emsdk_env.sh"
bash web/toolchain/nsp-extract-spike/build-wasm.sh
VERIFY_STEP

step step5 'Run synthetic container tests' . 0 <<'VERIFY_STEP'
node --test web/toolchain/nsp-extract-spike/*.test.mjs web/tests/input-extraction.test.mjs web/tests/packaging-export.test.mjs
VERIFY_STEP

step step6 'Install browser test tools from official npm package' . 0 <<'VERIFY_STEP'
npm install --prefix "$RUNNER_TEMP/nsp-playwright" --no-audit --no-fund playwright@1.64.1
"$RUNNER_TEMP/nsp-playwright/node_modules/.bin/playwright" install --with-deps chromium
VERIFY_STEP

step step7 'Exercise WASM in Chromium dedicated Worker' . 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$RUNNER_TEMP/nsp-playwright/node_modules/playwright/index.mjs" \
  node web/toolchain/nsp-extract-spike/browser-smoke.mjs
VERIFY_STEP

finish
# END PROFILE extraction
;;
signed-nca)
# BEGIN PROFILE signed-nca
#!/usr/bin/env bash
# 运行 signed-nca 验证配方；依据同目录验证调度器的步骤约定。
# Run the signed-nca verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Install pinned official Emscripten SDK' . 0 <<'VERIFY_STEP'
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/nsp-emsdk"
test "$(git -C "$RUNNER_TEMP/nsp-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/nsp-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/nsp-emsdk/emsdk" activate 4.0.20
VERIFY_STEP

step step4 'Prove authenticated toy extraction with isolated test root' . 0 <<'VERIFY_STEP'
source "$RUNNER_TEMP/nsp-emsdk/emsdk_env.sh"
bash web/toolchain/synthetic-signed-nca/run-ci.sh
VERIFY_STEP

finish
# END PROFILE signed-nca
;;
program)
# BEGIN PROFILE program
#!/usr/bin/env bash
# 运行 program 验证配方；依据同目录验证调度器的步骤约定。
# Run the program verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Install exact official registry dependencies' web/toolchain/program-rewrite-spike 0 <<'VERIFY_STEP'
npm ci --ignore-scripts
python3 -m venv .venv
.venv/bin/pip install capstone==5.0.3 lz4==4.4.4
npx playwright install --with-deps chromium
VERIFY_STEP

step step4 'Build real ARM64 Capstone and LZ4 WASM with pinned official SDK' web/toolchain/program-rewrite-spike 0 <<'VERIFY_STEP'
bash build-wasm.sh
VERIFY_STEP

step step5 'Generate synthetic NSOs and original native Python oracle' web/toolchain/program-rewrite-spike 0 <<'VERIFY_STEP'
.venv/bin/python audit-source.py
.venv/bin/python make-fixtures.py
cp -r fixtures dist/
VERIFY_STEP

step step6 'Compare actual Chromium worker outputs byte for byte with native' web/toolchain/program-rewrite-spike 0 <<'VERIFY_STEP'
npm run test:browser
VERIFY_STEP

finish
# END PROFILE program
;;
*) printf 'Unknown verification profile: %s\n' "${1:-}" >&2; exit 2 ;;
esac
