#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"

ARCH="${1:-aarch64}"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-smoke-logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTER_LOG="$LOG_DIR/lane-d-stability-$STAMP.outer.log"

DXMT_STABILITY_REPEAT_COUNT="${DXMT_STABILITY_REPEAT_COUNT:-2}"
DXMT_STABILITY_FRAMES="${DXMT_STABILITY_FRAMES:-1200}"
DXMT_STABILITY_MIN_MS="${DXMT_STABILITY_MIN_MS:-60000}"
DXMT_STABILITY_TIMEOUT_SECONDS="${DXMT_STABILITY_TIMEOUT_SECONDS:-180}"

mkdir -p "$LOG_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

set +e
(
  SMOKE_TIMEOUT_SECONDS="$DXMT_STABILITY_TIMEOUT_SECONDS" \
  SMOKE_REPEAT_COUNT="$DXMT_STABILITY_REPEAT_COUNT" \
  SMOKE_STABILITY_FRAMES="$DXMT_STABILITY_FRAMES" \
  SMOKE_STABILITY_MIN_MS="$DXMT_STABILITY_MIN_MS" \
  "$PROJECT_ROOT/engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh" "$ARCH"
) >"$OUTER_LOG" 2>&1
rc=$?
set -e

echo "outer=$OUTER_LOG"
echo "arch=$ARCH"
echo "repeat_count=$DXMT_STABILITY_REPEAT_COUNT"
echo "stability_frames=$DXMT_STABILITY_FRAMES"
echo "stability_min_ms=$DXMT_STABILITY_MIN_MS"
grep -E "^(run=|run_exit_code=|UnityMultithreadProbe|UnityBatchProbe|UnityStabilityProbe|exit_code=)" "$OUTER_LOG" | tail -240 || true

if [[ "$rc" -ne 0 ]]; then
  echo "stability_smoke_result=FAIL rc=$rc"
  exit "$rc"
fi

expected_runs="$DXMT_STABILITY_REPEAT_COUNT"
pass_runs="$(rg -c "UnityStabilityProbe result=PASS" "$OUTER_LOG" || true)"
batch_runs="$(rg -c "UnityBatchProbe result=PASS" "$OUTER_LOG" || true)"
multithread_runs="$(rg -c "UnityMultithreadProbe result=PASS" "$OUTER_LOG" || true)"

echo "stability_pass_runs=$pass_runs"
echo "batch_pass_runs=$batch_runs"
echo "multithread_pass_runs=$multithread_runs"

if [[ "$pass_runs" -lt "$expected_runs" || "$batch_runs" -lt "$expected_runs" || "$multithread_runs" -lt "$expected_runs" ]]; then
  echo "stability_smoke_result=FAIL reason=missing_phase6_markers"
  exit 1
fi

echo "stability_smoke_result=PASS"
