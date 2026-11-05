#!/usr/bin/env bash
# 按功能集中验证入口；每个配置保留独立进程和原始步骤。
# Feature-grouped verification entry points; each profile retains its own process and original steps.
case "${1:-}" in
staged-output)
# BEGIN PROFILE staged-output
#!/usr/bin/env bash
# 运行 staged-output 验证配方；依据同目录验证调度器的步骤约定。
# Run the staged-output verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Verify synthetic archives and Python ZIP oracle' . 0 <<'VERIFY_STEP'
node --test web/tests/archive-codecs.test.mjs web/tests/browser-lifecycle.test.mjs
VERIFY_STEP

step step4 'Install pinned browser proof tooling' . 0 <<'VERIFY_STEP'
npm install --prefix "$RUNNER_TEMP/staged-playwright" --no-audit --no-fund playwright@1.64.1
"$RUNNER_TEMP/staged-playwright/node_modules/.bin/playwright" install --with-deps chromium
VERIFY_STEP

step step5 'Exercise real Chromium OPFS staging and cancellation' . 0 <<'VERIFY_STEP'
PLAYWRIGHT_ENTRY="$RUNNER_TEMP/staged-playwright/node_modules/playwright/index.mjs" \
  node web/toolchain/staged-output-spike/browser-smoke.mjs
VERIFY_STEP

finish
# END PROFILE staged-output
;;
zip64)
# BEGIN PROFILE zip64
#!/usr/bin/env bash
# 运行 zip64 验证配方；依据同目录验证调度器的步骤约定。
# Run the zip64 verification recipe under the sibling verification scheduler's step contract.

set -euo pipefail

source "$(dirname "$0")/../steps.sh"

step step3 'Confirm independent Python oracle prerequisite' web 0 <<'VERIFY_STEP'
python3 --version
VERIFY_STEP

step step4 'Stream real large payload without retaining output' web 300 <<'VERIFY_STEP'
export NODE_OPTIONS="--max-old-space-size=256"
export ZIP64_LARGE_STREAM_TEST="1"
node --test --test-timeout=240000 tests/archive-codecs.test.mjs
VERIFY_STEP

always_step step5 'Record validation boundary' web 0 <<'VERIFY_STEP'
{
  echo '### ZIP64 bounded stream validation'
  echo 'The test processes 4 GiB + 4 MiB of synthetic zero bytes through the real CRC loop, with one reused 4 MiB input buffer and a discard/count sink. Python zlib independently checks CRC; a second entry verifies a central-directory offset above 4 GiB.'
  echo 'Small real archives are independently extracted with Python zipfile. Metadata-only tests cover exact offsets above 2^53.'
  echo 'No multi-gigabyte archive is retained or independently extracted. Browser destination quotas, game data, resource-export integration, IPA installation and device compatibility are not established by this job.'
} >> "$GITHUB_STEP_SUMMARY"
VERIFY_STEP

finish
# END PROFILE zip64
;;
*) printf 'Unknown verification profile: %s\n' "${1:-}" >&2; exit 2 ;;
esac
