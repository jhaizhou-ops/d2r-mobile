#!/usr/bin/env bash
# 本地与 CI 配方共用执行语义；命令保持普通 Bash 形式。
# Shared execution semantics for local/CI recipes. Commands stay in plain Bash.
set -euo pipefail
: "${VERIFY_ROOT:?Use web/verification/verify.mjs}"
VERIFY_FAILED=0
step() {
  local id="$1" label="$2" cwd="$3" seconds="$4" body status
  body="$(mktemp "$RUNNER_TEMP/verify-step.XXXXXX")"
  cat > "$body"
  if [ "$VERIFY_FAILED" != 0 ] && [ "${VERIFY_ALWAYS:-0}" != 1 ]; then
    export "VERIFY_OUTCOME_${id}=skipped"
    rm -f "$body"
    return
  fi
  printf '\n== %s ==\n' "$label"
  set +e
  node "$VERIFY_ROOT/web/verification/run-step.mjs" "$body" "$VERIFY_ROOT/$cwd" "$seconds"
  status=$?
  set -e
  rm -f "$body"
  if [ "$status" = 0 ]; then
    export "VERIFY_OUTCOME_${id}=success"
  else
    export "VERIFY_OUTCOME_${id}=failure"
    VERIFY_FAILED=1
  fi
}
always_step() { VERIFY_ALWAYS=1 step "$@"; }
finish() { exit "$VERIFY_FAILED"; }
