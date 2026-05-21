#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT_DIR="$ROOT/reports/hyperbridge-validation"
mkdir -p "$OUT_DIR"

JSON_OUT="$OUT_DIR/BUILD-LOOP-TIMING.json"
MD_OUT="$OUT_DIR/BUILD-LOOP-TIMING.md"

RUN_APP_SMOKE="${RUN_APP_SMOKE:-0}"

now_epoch() { date +%s; }

ccache_before="not_installed"
ccache_after="not_installed"
if command -v ccache >/dev/null 2>&1; then
  ccache_before="$(ccache -s 2>/dev/null || true)"
fi

step_build_lib=0
step_build_tests=0
step_ntdll_relink=0
step_app_smoke=0

t0=$(now_epoch)
# TODO-hook: replace with real command used in MacRunner to build libhyperbridge.a
sleep 1
t1=$(now_epoch)
step_build_lib=$((t1 - t0))

t0=$(now_epoch)
# TODO-hook: replace with real hb_test_runner build command
sleep 1
t1=$(now_epoch)
step_build_tests=$((t1 - t0))

t0=$(now_epoch)
# TODO-hook: replace with real ntdll relink command
sleep 1
t1=$(now_epoch)
step_ntdll_relink=$((t1 - t0))

if [[ "$RUN_APP_SMOKE" == "1" ]]; then
  t0=$(now_epoch)
  # TODO-hook: optional app smoke (disabled by default)
  sleep 1
  t1=$(now_epoch)
  step_app_smoke=$((t1 - t0))
fi

if command -v ccache >/dev/null 2>&1; then
  ccache_after="$(ccache -s 2>/dev/null || true)"
fi

total=$((step_build_lib + step_build_tests + step_ntdll_relink + step_app_smoke))

python3 - "$JSON_OUT" "$step_build_lib" "$step_build_tests" "$step_ntdll_relink" "$step_app_smoke" "$total" "$RUN_APP_SMOKE" <<'PY'
import json, sys
out, bl, bt, nr, sm, tot, smoke = sys.argv[1:]
payload = {
    "build_libhyperbridge_a_sec": int(bl),
    "build_hb_test_runner_sec": int(bt),
    "ntdll_relink_sec": int(nr),
    "app_smoke_sec": int(sm),
    "app_smoke_enabled": smoke == "1",
    "total_sec": int(tot),
    "notes": [
        "Scaffold timings use TODO hooks; replace sleep calls with real build commands.",
        "ccache stats are captured in markdown report for before/after comparison."
    ]
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2)
PY

{
  echo "# BUILD LOOP TIMING"
  echo
  echo "- build libhyperbridge.a: ${step_build_lib}s"
  echo "- build hb_test_runner: ${step_build_tests}s"
  echo "- ntdll relink: ${step_ntdll_relink}s"
  echo "- app smoke enabled: ${RUN_APP_SMOKE}"
  echo "- app smoke: ${step_app_smoke}s"
  echo "- total: ${total}s"
  echo
  echo "## ccache before"
  echo '```'
  echo "$ccache_before"
  echo '```'
  echo
  echo "## ccache after"
  echo '```'
  echo "$ccache_after"
  echo '```'
  echo
  echo "## Notes"
  echo "- ccache enabled: $(command -v ccache >/dev/null 2>&1 && echo yes || echo no)"
  echo "- Replace TODO hooks with real MacRunner build/relink commands for production timing."
} > "$MD_OUT"

echo "BUILD LOOP TIMING COMPLETE"
