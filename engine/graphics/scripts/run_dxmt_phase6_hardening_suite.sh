#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"

ARCH="${1:-aarch64}"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-phase6-hardening"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTER_LOG="$LOG_DIR/phase6-hardening-$ARCH-$STAMP.outer.log"

DXMT_PHASE6_REPEAT_COUNT="${DXMT_PHASE6_REPEAT_COUNT:-3}"
DXMT_PHASE6_FRAMES="${DXMT_PHASE6_FRAMES:-1800}"
DXMT_PHASE6_MIN_MS="${DXMT_PHASE6_MIN_MS:-120000}"
DXMT_PHASE6_TIMEOUT_SECONDS="${DXMT_PHASE6_TIMEOUT_SECONDS:-300}"
DXMT_PHASE6_LEAK_BUDGET_KB="${DXMT_PHASE6_LEAK_BUDGET_KB:-262144}"

mkdir -p "$LOG_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

set +e
(
  SMOKE_TIMEOUT_SECONDS="$DXMT_PHASE6_TIMEOUT_SECONDS" \
  SMOKE_REPEAT_COUNT="$DXMT_PHASE6_REPEAT_COUNT" \
  SMOKE_STABILITY_FRAMES="$DXMT_PHASE6_FRAMES" \
  SMOKE_STABILITY_MIN_MS="$DXMT_PHASE6_MIN_MS" \
  DXMT_SMOKE_LEAK_BUDGET_KB="$DXMT_PHASE6_LEAK_BUDGET_KB" \
  "$PROJECT_ROOT/engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh" "$ARCH"
) >"$OUTER_LOG" 2>&1
rc=$?
set -e

echo "outer=$OUTER_LOG"
echo "arch=$ARCH"
echo "repeat_count=$DXMT_PHASE6_REPEAT_COUNT"
echo "frames=$DXMT_PHASE6_FRAMES"
echo "min_ms=$DXMT_PHASE6_MIN_MS"
echo "leak_budget_kb=$DXMT_PHASE6_LEAK_BUDGET_KB"
grep -E "^(run=|run_exit_code=|UnityStabilityProbe|UnityResidencyProbe result=|UnityLeakProbe|UnityMultithreadProbe result=|UnityBatchProbe result=|exit_code=)" "$OUTER_LOG" | tail -260 || true

if [[ "$rc" -ne 0 ]]; then
  echo "phase6_hardening_result=FAIL rc=$rc"
  exit "$rc"
fi

expected_runs="$DXMT_PHASE6_REPEAT_COUNT"
stability_runs="$(rg -c "UnityStabilityProbe result=PASS" "$OUTER_LOG" || true)"
residency_runs="$(rg -c "UnityResidencyProbe result=PASS" "$OUTER_LOG" || true)"
leak_runs="$(rg -c "UnityLeakProbe .* result=PASS" "$OUTER_LOG" || true)"
batch_runs="$(rg -c "UnityBatchProbe result=PASS" "$OUTER_LOG" || true)"
multithread_runs="$(rg -c "UnityMultithreadProbe result=PASS" "$OUTER_LOG" || true)"

echo "stability_pass_runs=$stability_runs"
echo "residency_pass_runs=$residency_runs"
echo "leak_pass_runs=$leak_runs"
echo "batch_pass_runs=$batch_runs"
echo "multithread_pass_runs=$multithread_runs"

if [[ "$stability_runs" -lt "$expected_runs" ||
      "$residency_runs" -lt "$expected_runs" ||
      "$leak_runs" -lt "$expected_runs" ||
      "$batch_runs" -lt "$expected_runs" ||
      "$multithread_runs" -lt "$expected_runs" ]]; then
  echo "phase6_hardening_result=FAIL reason=missing_phase6_markers"
  exit 1
fi

echo "phase6_hardening_result=PASS"
