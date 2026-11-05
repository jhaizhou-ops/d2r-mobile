#!/usr/bin/env bash
# 按功能集中验证入口；每个配置保留独立进程和原始步骤。
# Feature-grouped verification entry points; each profile retains its own process and original steps.
case "${1:-}" in
texture)
# BEGIN PROFILE texture
#!/usr/bin/env bash
# 运行 texture 验证配方；依据同目录验证调度器的步骤约定。
# Run the texture verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Install pinned official Emscripten SDK' . 0 <<'VERIFY_STEP'
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/texture-emsdk"
test "$(git -C "$RUNNER_TEMP/texture-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/texture-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/texture-emsdk/emsdk" activate 4.0.20
VERIFY_STEP

step step4 'Build bounded texture WASM API' . 0 <<'VERIFY_STEP'
source "$RUNNER_TEMP/texture-emsdk/emsdk_env.sh"
bash web/toolchain/texture-spike/build-wasm.sh
VERIFY_STEP

step step5 'Generate native synthetic oracle and validate bounded C' . 0 <<'VERIFY_STEP'
python3 web/toolchain/texture-spike/native-fixtures.py
VERIFY_STEP

step step6 'Compare native and WASM layout, mip decoding and malformed inputs' . 0 <<'VERIFY_STEP'
node --test web/toolchain/texture-spike/texture.test.mjs
VERIFY_STEP

step step7 'Generate original BC3 encoding and packing oracle' . 0 <<'VERIFY_STEP'
python3 -m venv "$RUNNER_TEMP/texture-reference"
"$RUNNER_TEMP/texture-reference/bin/pip" install 'numpy==2.0.2' 'Pillow==11.1.0'
"$RUNNER_TEMP/texture-reference/bin/python" web/toolchain/texture-spike/native-encoder-fixtures.py
node --test web/toolchain/texture-spike/encoder.test.mjs
VERIFY_STEP

step step8 'Install browser test tools from official npm package' . 0 <<'VERIFY_STEP'
npm install --prefix "$RUNNER_TEMP/texture-playwright" --no-audit --no-fund playwright@1.64.1
"$RUNNER_TEMP/texture-playwright/node_modules/.bin/playwright" install --with-deps chromium
VERIFY_STEP

step step9 'Exercise WASM in Chromium dedicated Worker' . 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$RUNNER_TEMP/texture-playwright/node_modules/playwright/index.mjs" \
  node web/toolchain/texture-spike/browser-smoke.mjs
VERIFY_STEP

step step10 'Prepare pinned self-hosted original Python codec runtime' web/toolchain/asset-generation-spike 0 <<'VERIFY_STEP'
npm ci --ignore-scripts --no-audit --no-fund
npm run prepare:runtime
VERIFY_STEP

step step11 'Verify original BC3 encoding, packing and cancellation in Chromium' . 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$RUNNER_TEMP/texture-playwright/node_modules/playwright/index.mjs" \
  timeout --kill-after=5s 240s node web/toolchain/texture-spike/encoder-browser-smoke.mjs
VERIFY_STEP

finish
# END PROFILE texture
;;
offline-wasm)
# BEGIN PROFILE offline-wasm
#!/usr/bin/env bash
# 运行 offline-wasm 验证配方；依据同目录验证调度器的步骤约定。
# Run the offline-wasm verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Install pinned official Emscripten SDK' . 0 <<'VERIFY_STEP'
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/texture-emsdk"
test "$(git -C "$RUNNER_TEMP/texture-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/texture-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/texture-emsdk/emsdk" activate 4.0.20
VERIFY_STEP

step step4 'Build bounded texture WASM API' . 0 <<'VERIFY_STEP'
source "$RUNNER_TEMP/texture-emsdk/emsdk_env.sh"
bash web/toolchain/texture-spike/build-wasm.sh
VERIFY_STEP

step step5 'Prepare hashed offline runtime bundle' . 0 <<'VERIFY_STEP'
node web/toolchain/offline-runtime-spike/prepare-bundle.mjs
VERIFY_STEP

step step6 'Install browser proof tooling' . 0 <<'VERIFY_STEP'
npm install --prefix "$RUNNER_TEMP/offline-playwright" --no-audit --no-fund playwright@1.64.1
"$RUNNER_TEMP/offline-playwright/node_modules/.bin/playwright" install --with-deps chromium
VERIFY_STEP

step step7 'Prove sealed worker conversion and blocked network without request interception' . 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$RUNNER_TEMP/offline-playwright/node_modules/playwright/index.mjs" \
  node web/toolchain/offline-runtime-spike/browser-proof.mjs
VERIFY_STEP

finish
# END PROFILE offline-wasm
;;
desktop-build)
# BEGIN PROFILE desktop-build
#!/usr/bin/env bash
# 运行 desktop-build 验证配方；依据同目录验证调度器的步骤约定。
# Run the desktop-build verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Check transfer-integrity negative cases' . 0 <<'VERIFY_STEP'
node --test web/toolchain/desktop-browser-spike/verify-transfer.test.mjs
VERIFY_STEP

step step4 'Build public WASM once with pinned official SDK' . 0 <<'VERIFY_STEP'
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/desktop-emsdk"
test "$(git -C "$RUNNER_TEMP/desktop-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/desktop-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/desktop-emsdk/emsdk" activate 4.0.20
source "$RUNNER_TEMP/desktop-emsdk/emsdk_env.sh"
bash web/toolchain/texture-spike/build-wasm.sh
node web/toolchain/offline-runtime-spike/prepare-bundle.mjs
VERIFY_STEP

step seal 'Seal exact public runtime transfer tree' . 0 <<'VERIFY_STEP'
node web/toolchain/desktop-browser-spike/verify-transfer.mjs seal web/toolchain/offline-runtime-spike/dist
VERIFY_STEP

finish
# END PROFILE desktop-build
;;
desktop)
# BEGIN PROFILE desktop
#!/usr/bin/env bash
# 运行 desktop 验证配方；依据同目录验证调度器的步骤约定。
# Run the desktop verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Verify transfer against trusted producing-job digest' . 0 <<'VERIFY_STEP'
export EXPECTED_DIGEST="$EXPECTED_DIGEST"
node web/toolchain/desktop-browser-spike/verify-transfer.mjs verify web/toolchain/offline-runtime-spike/dist "$EXPECTED_DIGEST"
VERIFY_STEP

step step4 'Install pinned official Chromium proof tooling' . 0 <<'VERIFY_STEP'
npm install --prefix "$RUNNER_TEMP/desktop-playwright" --ignore-scripts --no-audit --no-fund playwright@1.64.1
node "$RUNNER_TEMP/desktop-playwright/node_modules/playwright/cli.js" install chromium
VERIFY_STEP

step step5 'Exercise real OPFS commit and cancellation' . 180 <<'VERIFY_STEP'
export PLAYWRIGHT_ENTRY="$RUNNER_TEMP/desktop-playwright/node_modules/playwright/index.mjs"
node web/toolchain/staged-output-spike/browser-smoke.mjs
VERIFY_STEP

step step6 'Exercise sealed Emscripten Worker and negative preload cases' . 180 <<'VERIFY_STEP'
export PLAYWRIGHT_ENTRY="$RUNNER_TEMP/desktop-playwright/node_modules/playwright/index.mjs"
node web/toolchain/offline-runtime-spike/browser-proof.mjs
VERIFY_STEP

always_step step7 'Record environment and evidence boundary' . 0 <<'VERIFY_STEP'
node --input-type=module <<'DESKTOP_ENVIRONMENT_REPORT'
// 记录浏览器验证所用的环境信息；依据 web/toolchain/desktop-browser-spike 的模块与调用约定。
// Record environment information for browser verification using the module and calling contracts in web/toolchain/desktop-browser-spike.
import { appendFile } from "node:fs/promises";
import os from "node:os";
import process from "node:process";
const summary = `Synthetic desktop Chromium environment: ${os.platform()} ${os.release()} ${os.arch()}, Node ${process.version}.\n\nIndividual proof steps and their logs determine pass/fail and record the actual Chromium version. The checks cover small synthetic OPFS commit/reread, cancellation, quota rejection, sealed Emscripten conversion and fail-closed preload errors.\n\nThis is Playwright Chromium, not installed Chrome or Edge. It does not automate or validate an OS save picker, multi-GB disk endurance, all Windows/macOS versions, game inputs, real keys, IPA installation or device compatibility. Only public generated runtime files are transferred between jobs; no user data is read or uploaded.\n`;
console.log(summary);
if (process.env.GITHUB_STEP_SUMMARY) await appendFile(process.env.GITHUB_STEP_SUMMARY, summary);
DESKTOP_ENVIRONMENT_REPORT
VERIFY_STEP

finish
# END PROFILE desktop
;;
*) printf 'Unknown verification profile: %s\n' "${1:-}" >&2; exit 2 ;;
esac
