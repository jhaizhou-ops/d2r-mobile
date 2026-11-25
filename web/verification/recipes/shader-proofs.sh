#!/usr/bin/env bash
# 按功能集中验证入口；每个配置保留独立进程和原始步骤。
# Feature-grouped verification entry points; each profile retains its own process and original steps.
case "${1:-}" in
shader)
# BEGIN PROFILE shader
#!/usr/bin/env bash
# 运行 shader 验证配方；依据同目录验证调度器的步骤约定。
# Run the shader verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

export DOTNET_CLI_TELEMETRY_OPTOUT=1

export DOTNET_NOLOGO=1

export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=true

export SDK_VERSION=10.0.402

export WORKLOAD_VERSION=10.0.402

step step8 'Pin SDK and workload (also applies to native commands from this cwd)' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
printf '{"sdk":{"version":"%s","rollForward":"disable","allowPrerelease":false,"workloadVersion":"%s"}}\n' "$SDK_VERSION" "$WORKLOAD_VERSION" > global.json
test "$(dotnet --version)" = "$SDK_VERSION"
dotnet --info
git rev-parse HEAD
mkdir -p "$RUNNER_TEMP/shader-parity"
VERIFY_STEP

step step9 'Install official developer prerequisites on CI only' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
dotnet workload install wasm-tools
dotnet workload --version
dotnet workload list
npm ci --ignore-scripts
npm audit --audit-level=high
npx --no-install playwright install --with-deps chromium
sudo apt-get update
apt-cache policy spirv-tools
sudo apt-get install --no-install-recommends -y spirv-tools
spirv-val --version
npx --no-install playwright --version
df -h .
VERIFY_STEP

step publish 'Publish actual browser WASM bundle' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
dotnet restore ShaderSpike.csproj
dotnet format ShaderSpike.csproj --no-restore --verify-no-changes --include ShaderBridge.cs EmbeddedResources.cs --verbosity normal
dotnet build ShaderSpike.csproj -c Release --no-restore -p:EnableNETAnalyzers=true -p:AnalysisLevel=latest -p:TreatWarningsAsErrors=true
dotnet publish ShaderSpike.csproj -c Release --no-restore -p:EnableNETAnalyzers=true -p:AnalysisLevel=latest -p:TreatWarningsAsErrors=true
VERIFY_STEP

step native 'Prepare tiny authored fixture and build independent original native oracle' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
node verify-parity.mjs prepare "$RUNNER_TEMP/shader-parity/fixture"
native_project="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj"
dotnet restore "$native_project"
dotnet build "$native_project" -c Release --no-restore
native_dll="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/bin/Release/net10.0/dmsh_xlate.dll"
test -f "$native_dll"
actual=$(dotnet "$native_dll" --source-sha)
expected=$(cat "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/Program.cs" "$native_project" | sha256sum | cut -c1-16)
test "$actual" = "$expected"
echo "Verified original native embedded source: $actual"
sha256sum "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/Program.cs" "$native_project" ShaderBridge.cs ShaderSpike.csproj EmbeddedResources.cs wwwroot/synthetic-fixture.mjs "$RUNNER_TEMP/shader-parity/fixture/synthetic-exit.bin" "$RUNNER_TEMP/shader-parity/fixture/jobs.json"
git rev-parse HEAD:dm/third_party/ryujinx
timeout --kill-after=5s 60s dotnet "$native_dll" "$RUNNER_TEMP/shader-parity/fixture/jobs.json" "$RUNNER_TEMP/shader-parity/native"
test -s "$RUNNER_TEMP/shader-parity/native/synthetic-exit.spv"
test -s "$RUNNER_TEMP/shader-parity/native/synthetic-exit.res.json"
VERIFY_STEP

step browser 'Execute published module worker in real Chromium and capture download' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
timeout --kill-after=5s 120s node capture-browser.mjs bin/Release/net10.0/publish "$RUNNER_TEMP/shader-parity/browser"
VERIFY_STEP

step parity 'Require exact binary and complete sidecar parity' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
node verify-parity.mjs compare "$RUNNER_TEMP/shader-parity/native" "$RUNNER_TEMP/shader-parity/browser/browser-result.json" | tee "$RUNNER_TEMP/shader-parity/parity.json"
VERIFY_STEP

step validation 'Validate both emitted SPIR-V files without target relaxation' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
timeout --kill-after=5s 30s spirv-val --target-env vulkan1.1 "$RUNNER_TEMP/shader-parity/native/synthetic-exit.spv"
timeout --kill-after=5s 30s spirv-val --target-env vulkan1.1 "$RUNNER_TEMP/shader-parity/native/browser-synthetic-exit.spv"
VERIFY_STEP

always_step step15 'Record bounded evidence and scope (no artifacts uploaded)' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
export VALIDATION="${VERIFY_OUTCOME_validation:-skipped}"
export PARITY="${VERIFY_OUTCOME_parity:-skipped}"
export BROWSER="${VERIFY_OUTCOME_browser:-skipped}"
export NATIVE="${VERIFY_OUTCOME_native:-skipped}"
export PUBLISH="${VERIFY_OUTCOME_publish:-skipped}"
{
  echo '### Synthetic EXIT runtime gate'
  echo "Publish: $PUBLISH; independent native: $NATIVE; Chromium worker: $BROWSER; exact parity: $PARITY; Vulkan 1.1 SPIR-V validation: $VALIDATION."
  echo 'This is synthetic Linux Chromium translation evidence only. Windows/macOS browsers, resource/arithmetic fixtures, graphics, GPU execution, games, Metal conversion and working unsigned IPA output remain unverified.'
  echo 'No credentials, game data, binary artifacts, runtime bundle or browser profile is uploaded.'
} >> "$GITHUB_STEP_SUMMARY"
node --input-type=module <<'JS'
import { stat, readFile } from 'node:fs/promises';
import { join } from 'node:path';
for (const name of ['browser/bundle-diagnostics.json', 'browser/browser-diagnostics.json', 'parity.json']) {
  const path = join(process.env.RUNNER_TEMP, 'shader-parity', name);
  try {
    if ((await stat(path)).size > 128 * 1024) throw new Error('Diagnostic exceeds cap');
    console.log(name, await readFile(path, 'utf8'));
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
    console.log(name, 'not produced');
  }
}
JS
VERIFY_STEP

finish
# END PROFILE shader
;;
offline-dotnet)
# BEGIN PROFILE offline-dotnet
#!/usr/bin/env bash
# 运行 offline-dotnet 验证配方；依据同目录验证调度器的步骤约定。
# Run the offline-dotnet verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

export DOTNET_CLI_TELEMETRY_OPTOUT=1

export DOTNET_NOLOGO=1

export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=true

export SDK_VERSION=10.0.402

export WORKLOAD_VERSION=10.0.402

step step8 'Pin SDK and workload (also applies to native commands from this cwd)' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
printf '{"sdk":{"version":"%s","rollForward":"disable","allowPrerelease":false,"workloadVersion":"%s"}}\n' "$SDK_VERSION" "$WORKLOAD_VERSION" > global.json
test "$(dotnet --version)" = "$SDK_VERSION"
dotnet --info
git rev-parse HEAD
mkdir -p "$RUNNER_TEMP/shader-parity"
VERIFY_STEP

step step9 'Install official developer prerequisites on CI only' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
dotnet workload install wasm-tools
dotnet workload --version
dotnet workload list
npm ci --ignore-scripts
npm audit --audit-level=high
npx --no-install playwright install --with-deps chromium
sudo apt-get update
apt-cache policy spirv-tools
sudo apt-get install --no-install-recommends -y spirv-tools
spirv-val --version
npx --no-install playwright --version
df -h .
VERIFY_STEP

step publish 'Publish actual browser WASM bundle' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
dotnet restore ShaderSpike.csproj
dotnet build ShaderSpike.csproj -c Release --no-restore -p:WasmInlineBootConfig=false -p:EnableNETAnalyzers=true -p:AnalysisLevel=latest -p:TreatWarningsAsErrors=true
dotnet publish ShaderSpike.csproj -c Release --no-restore -p:WasmInlineBootConfig=false -p:EnableNETAnalyzers=true -p:AnalysisLevel=latest -p:TreatWarningsAsErrors=true
VERIFY_STEP

step native 'Prepare tiny authored fixture and build independent original native oracle' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
node verify-parity.mjs prepare "$RUNNER_TEMP/shader-parity/fixture"
native_project="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj"
dotnet restore "$native_project"
dotnet build "$native_project" -c Release --no-restore
native_dll="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/bin/Release/net10.0/dmsh_xlate.dll"
test -f "$native_dll"
actual=$(dotnet "$native_dll" --source-sha)
expected=$(cat "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/Program.cs" "$native_project" | sha256sum | cut -c1-16)
test "$actual" = "$expected"
echo "Verified original native embedded source: $actual"
sha256sum "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/Program.cs" "$native_project" ShaderBridge.cs ShaderSpike.csproj EmbeddedResources.cs wwwroot/synthetic-fixture.mjs "$RUNNER_TEMP/shader-parity/fixture/synthetic-exit.bin" "$RUNNER_TEMP/shader-parity/fixture/jobs.json"
git rev-parse HEAD:dm/third_party/ryujinx
timeout --kill-after=5s 60s dotnet "$native_dll" "$RUNNER_TEMP/shader-parity/fixture/jobs.json" "$RUNNER_TEMP/shader-parity/native"
test -s "$RUNNER_TEMP/shader-parity/native/synthetic-exit.spv"
test -s "$RUNNER_TEMP/shader-parity/native/synthetic-exit.res.json"
VERIFY_STEP

step step12 'Hash complete published public runtime closure' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
node ../offline-runtime-spike/dotnet-prepare.mjs bin/Release/net10.0/publish
VERIFY_STEP

step browser 'Execute fresh sealed blob worker and demand zero HTTP receipts' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$PWD/node_modules/playwright/index.mjs" timeout --kill-after=5s 180s node ../offline-runtime-spike/dotnet-browser.mjs "$RUNNER_TEMP/shader-parity/browser"
VERIFY_STEP

step parity 'Require exact binary and complete sidecar parity' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
node verify-parity.mjs compare "$RUNNER_TEMP/shader-parity/native" "$RUNNER_TEMP/shader-parity/browser/browser-result.json" | tee "$RUNNER_TEMP/shader-parity/parity.json"
VERIFY_STEP

step validation 'Validate both emitted SPIR-V files without target relaxation' web/toolchain/shader-spike 0 <<'VERIFY_STEP'
timeout --kill-after=5s 30s spirv-val --target-env vulkan1.1 "$RUNNER_TEMP/shader-parity/native/synthetic-exit.spv"
timeout --kill-after=5s 30s spirv-val --target-env vulkan1.1 "$RUNNER_TEMP/shader-parity/native/browser-synthetic-exit.spv"
VERIFY_STEP

finish
# END PROFILE offline-dotnet
;;
spirv-cross)
# BEGIN PROFILE spirv-cross
#!/usr/bin/env bash
# 运行 spirv-cross 验证配方；依据同目录验证调度器的步骤约定。
# Run the spirv-cross verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Verify pure byte boundary and native replay reflection' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
node --test test.mjs
VERIFY_STEP

step step4 'Fetch exact official upstream sources and SDK' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
git clone --depth 1 --branch vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Cross.git "$RUNNER_TEMP/spirv-cross"
test "$(git -C "$RUNNER_TEMP/spirv-cross" rev-parse HEAD)" = f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/spirv-emsdk"
test "$(git -C "$RUNNER_TEMP/spirv-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/spirv-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/spirv-emsdk/emsdk" activate 4.0.20
VERIFY_STEP

step step5 'Build original CLI natively and as browser WASM' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
bash build.sh native "$RUNNER_TEMP/spirv-cross" "$RUNNER_TEMP/spirv-native"
source "$RUNNER_TEMP/spirv-emsdk/emsdk_env.sh"
bash build.sh wasm "$RUNNER_TEMP/spirv-cross" "$RUNNER_TEMP/spirv-wasm"
VERIFY_STEP

step step6 'Install CI-only synthetic assembly and browser tools' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
sudo apt-get update
sudo apt-get install --no-install-recommends -y spirv-tools
spirv-as --version
npm ci --ignore-scripts --prefix ../shader-spike
(cd ../shader-spike && npx --no-install playwright install --with-deps chromium)
VERIFY_STEP

step step7 'Assemble authored fixtures and run original native oracle' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
mkdir -p results
python3 provenance.py > results/native-chain-observed.json
for fixture in fixture fragment storage; do
  spirv-as --target-env spv1.0 "$fixture.spvasm" -o "wwwroot/$fixture.spv"
  spirv-val --target-env spv1.0 "wwwroot/$fixture.spv"
  "$RUNNER_TEMP/spirv-native/spirv-cross" --msl --msl-version 20300 --msl-argument-buffers --msl-texture-buffer-native --msl-pad-fragment-output "wwwroot/$fixture.spv" --output "results/$fixture.native.metal"
  "$RUNNER_TEMP/spirv-native/spirv-cross" --reflect json "wwwroot/$fixture.spv" --output "results/$fixture.native.json"
done
sha256sum toolchain-lock.json build.sh *.spvasm wwwroot/*.mjs wwwroot/*.wasm wwwroot/*.spv "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_build.py" "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_chain.py" "$GITHUB_WORKSPACE/dm/gfx/tools/nometal_run.py" > results/source-chain.sha256
VERIFY_STEP

step step8 'Compare actual browser worker to native source and reflection' web/toolchain/spirv-cross-spike 0 <<'VERIFY_STEP'
node capture-browser.mjs results
VERIFY_STEP

finish
# END PROFILE spirv-cross
;;
shader-pipeline)
# BEGIN PROFILE shader-pipeline
#!/usr/bin/env bash
# 运行 shader-pipeline 验证配方；依据同目录验证调度器的步骤约定。
# Run the shader-pipeline verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

export DOTNET_CLI_TELEMETRY_OPTOUT=1

export DOTNET_NOLOGO=1

export DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=true

step step6 'Pin native and browser translator SDK and validate adapter' web/toolchain/shader-pipeline-spike 0 <<'VERIFY_STEP'
printf '{"sdk":{"version":"10.0.402","rollForward":"disable","allowPrerelease":false,"workloadVersion":"10.0.402"}}\n' > "$GITHUB_WORKSPACE/global.json"
test "$(dotnet --version)" = 10.0.402
node --test ../../tests/shader-pipeline.test.mjs
dotnet workload install wasm-tools
dotnet workload --version
npm ci --ignore-scripts --prefix ../shader-spike
(cd ../shader-spike && npx --no-install playwright install --with-deps chromium)
sudo apt-get update
sudo apt-get install --no-install-recommends -y spirv-tools
spirv-val --version
VERIFY_STEP

step step7 'Build original translator and unchanged browser spike' web/toolchain/shader-pipeline-spike 0 <<'VERIFY_STEP'
dotnet publish ../shader-spike/ShaderSpike.csproj -c Release -p:EnableNETAnalyzers=true -p:AnalysisLevel=latest -p:TreatWarningsAsErrors=true
native_project="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj"
dotnet build "$native_project" -c Release
native_dll="$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/bin/Release/net10.0/dmsh_xlate.dll"
actual=$(dotnet "$native_dll" --source-sha)
expected=$(cat "$GITHUB_WORKSPACE/dm/gfx/tools/dmsh_xlate/Program.cs" "$native_project" | sha256sum | cut -c1-16)
test "$actual" = "$expected"
node capture-browser.mjs prepare "$RUNNER_TEMP/linked-shader/fixture"
timeout --kill-after=5s 60s dotnet "$native_dll" "$RUNNER_TEMP/linked-shader/fixture/jobs.json" "$RUNNER_TEMP/linked-shader/native"
for key in 0000000000000001 0000000000000002 0000000000000003; do
  spirv-val --target-env vulkan1.1 "$RUNNER_TEMP/linked-shader/native/$key.spv"
done
VERIFY_STEP

step step8 'Build pinned native and WASM SPIRV-Cross with existing recipe' web/toolchain/shader-pipeline-spike 0 <<'VERIFY_STEP'
git clone --depth 1 --branch vulkan-sdk-1.4.363.0 https://github.com/KhronosGroup/SPIRV-Cross.git "$RUNNER_TEMP/linked-cross"
test "$(git -C "$RUNNER_TEMP/linked-cross" rev-parse HEAD)" = f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7
git clone --depth 1 --branch 4.0.20 https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/linked-emsdk"
test "$(git -C "$RUNNER_TEMP/linked-emsdk" rev-parse HEAD)" = e4fe26ef59168ff44f4c23c466e497bf60b3411e
"$RUNNER_TEMP/linked-emsdk/emsdk" install 4.0.20
"$RUNNER_TEMP/linked-emsdk/emsdk" activate 4.0.20
bash ../spirv-cross-spike/build.sh native "$RUNNER_TEMP/linked-cross" "$RUNNER_TEMP/linked-cross-native"
source "$RUNNER_TEMP/linked-emsdk/emsdk_env.sh"
bash ../spirv-cross-spike/build.sh wasm "$RUNNER_TEMP/linked-cross" "$RUNNER_TEMP/linked-cross-wasm"
VERIFY_STEP

step step9 'Independent native Metal and upstream reflection oracle' web/toolchain/shader-pipeline-spike 0 <<'VERIFY_STEP'
native="$RUNNER_TEMP/linked-shader/native"
for key in 0000000000000001 0000000000000002 0000000000000003; do
  "$RUNNER_TEMP/linked-cross-native/spirv-cross" --msl --msl-version 20300 --msl-argument-buffers --msl-texture-buffer-native --msl-pad-fragment-output "$native/$key.spv" --output "$native/${key}_b.metal"
  "$RUNNER_TEMP/linked-cross-native/spirv-cross" --reflect json "$native/$key.spv" --output "$native/$key.cross.json"
done
VERIFY_STEP

step step10 'Real Chromium linked pipeline versus original native outputs' web/toolchain/shader-pipeline-spike 0 <<'VERIFY_STEP'
timeout --kill-after=5s 180s node capture-browser.mjs capture ../shader-spike/bin/Release/net10.0/publish "$RUNNER_TEMP/linked-shader/native" "$RUNNER_TEMP/linked-shader/browser"
sha256sum graphics-fixtures.mjs capture-browser.mjs ../../shader-pipeline.mjs ../shader-spike/ShaderBridge.cs ../spirv-cross-spike/wwwroot/*.mjs ../spirv-cross-spike/wwwroot/*.wasm "$RUNNER_TEMP/linked-shader/native/"* > "$RUNNER_TEMP/linked-shader/browser/source-chain.sha256"
VERIFY_STEP

finish
# END PROFILE shader-pipeline
;;
*) printf 'Unknown verification profile: %s\n' "${1:-}" >&2; exit 2 ;;
esac
