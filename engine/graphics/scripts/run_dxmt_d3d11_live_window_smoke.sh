#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"

ARCH="${1:-aarch64}"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-smoke-logs"
LIVE_DIR="$PROJECT_ROOT/artifacts/dxmt-live-window"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTER_LOG="$LOG_DIR/lane-d-live-window-present-$STAMP.outer.log"
RAW_LOG="$LOG_DIR/dx11-headless-$ARCH.log"
PROBE_JSON="$LIVE_DIR/lane-d-live-window-present-$STAMP.probe.json"
CAPTURE_JSON="$LIVE_DIR/lane-d-live-window-present-$STAMP.capture.json"
CAPTURE_BMP="$LIVE_DIR/lane-d-live-window-present-$STAMP.bmp"
PREFIX="$PROJECT_ROOT/artifacts/dxmt-live-window-prefix"

LIVE_TIMEOUT_SECONDS="${LIVE_TIMEOUT_SECONDS:-60}"
LIVE_STABILITY_FRAMES="${LIVE_STABILITY_FRAMES:-1200}"
LIVE_STABILITY_MIN_MS="${LIVE_STABILITY_MIN_MS:-12000}"
LIVE_POLL_ATTEMPTS="${LIVE_POLL_ATTEMPTS:-120}"
LIVE_CAPTURE_DELAY_SECONDS="${LIVE_CAPTURE_DELAY_SECONDS:-1}"
LIVE_MIN_NONBLACK_PIXELS="${LIVE_MIN_NONBLACK_PIXELS:-1000}"
# CGWindowList can omit CAMetalLayer client pixels on this host. The capture
# gate proves a visible Wine HWND; GPU color correctness comes from readback.
LIVE_MIN_COLORFUL_PIXELS="${LIVE_MIN_COLORFUL_PIXELS:-0}"

mkdir -p "$LOG_DIR" "$LIVE_DIR"
"$PROJECT_ROOT/scripts/disk-guard.sh" --check-only >/dev/null

smoke_pid=""
cleanup() {
  local rc=$?
  if [[ -n "${smoke_pid:-}" ]] && kill -0 "$smoke_pid" 2>/dev/null; then
    wait "$smoke_pid" || true
  fi
  exit "$rc"
}
trap cleanup EXIT

(
  SMOKE_TIMEOUT_SECONDS="$LIVE_TIMEOUT_SECONDS" \
  SMOKE_REPEAT_COUNT=1 \
  SMOKE_STABILITY_FRAMES="$LIVE_STABILITY_FRAMES" \
  SMOKE_STABILITY_MIN_MS="$LIVE_STABILITY_MIN_MS" \
  SMOKE_VISIBLE_WINDOW=1 \
  DXMT_SMOKE_PREFIX="$PREFIX" \
  WINEDEBUG_SMOKE="${WINEDEBUG_SMOKE:--all,+loaddll}" \
  "$PROJECT_ROOT/engine/graphics/scripts/run_dxmt_d3d11_headless_smoke.sh" "$ARCH"
) >"$OUTER_LOG" 2>&1 &
smoke_pid=$!

window_id=""
for _ in $(seq 1 "$LIVE_POLL_ATTEMPTS"); do
  if ! kill -0 "$smoke_pid" 2>/dev/null; then
    break
  fi
  if swift "$PROJECT_ROOT/tools/cg_window_probe.swift" dxmt >"$PROBE_JSON.tmp" 2>"$PROBE_JSON.err"; then
    python3 - "$PROBE_JSON.tmp" >"$PROBE_JSON.sel" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
for window in payload.get("windows", []):
    searchable = ((window.get("owner") or "") + " " + (window.get("name") or "")).lower()
    if "dxmt headless d3d11 smoke" in searchable or "dxmt" in searchable:
        print(window.get("window_id") or "")
        break
PY
    window_id="$(cat "$PROBE_JSON.sel")"
    cp "$PROBE_JSON.tmp" "$PROBE_JSON"
    if [[ -n "$window_id" ]]; then
      break
    fi
  fi
  sleep 0.25
done

capture_status="SKIP"
if [[ -n "$window_id" ]]; then
  sleep "$LIVE_CAPTURE_DELAY_SECONDS"
  if swift "$PROJECT_ROOT/tools/cg_window_capture.swift" "$window_id" "$CAPTURE_BMP" >"$CAPTURE_JSON" 2>"$CAPTURE_JSON.err"; then
    capture_status="PASS"
  else
    capture_status="FAIL"
  fi
fi

set +e
wait "$smoke_pid"
smoke_rc=$?
set -e
smoke_pid=""

capture_report="status=NONE width=0 height=0 nonblack=0 colorful=0"
capture_ok=0
if [[ -s "$CAPTURE_JSON" ]]; then
  capture_report="$(python3 - "$CAPTURE_JSON" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
fields = {
    "status": payload.get("status"),
    "width": payload.get("width", 0),
    "height": payload.get("height", 0),
    "nonblack": payload.get("nonblack", 0),
    "colorful": payload.get("colorful", 0),
    "path": payload.get("path"),
}
print(" ".join(f"{key}={value}" for key, value in fields.items()))
PY
)"
  capture_ok="$(python3 - "$CAPTURE_JSON" "$LIVE_MIN_NONBLACK_PIXELS" "$LIVE_MIN_COLORFUL_PIXELS" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
min_nonblack = int(sys.argv[2])
min_colorful = int(sys.argv[3])
ok = (
    payload.get("status") == "PASS"
    and int(payload.get("nonblack", 0)) >= min_nonblack
    and int(payload.get("colorful", 0)) >= min_colorful
)
print(1 if ok else 0)
PY
)"
fi

failures=()
[[ "$smoke_rc" -eq 0 ]] || failures+=("smoke_rc=$smoke_rc")
[[ -n "$window_id" ]] || failures+=("window_not_found")
[[ "$capture_status" == "PASS" ]] || failures+=("capture_status=$capture_status")
[[ "$capture_ok" == "1" ]] || failures+=("capture_pixels_below_threshold")
rg -q "window_visible=1" "$OUTER_LOG" || failures+=("visible_window_marker_missing")
rg -q "UnityStabilityProbe result=PASS" "$OUTER_LOG" || failures+=("stability_probe_missing")
rg -q "Present\\(resized1\\) hr=0x00000000" "$OUTER_LOG" || failures+=("present_resized1_missing")
rg -q "pixel_readback=PASS" "$OUTER_LOG" || failures+=("pixel_readback_missing")

echo "outer=$OUTER_LOG"
echo "raw=$RAW_LOG"
echo "smoke_rc=$smoke_rc"
echo "window_id=${window_id:-NONE}"
echo "capture_status=$capture_status"
echo "capture=$capture_report"
echo "probe_json=$PROBE_JSON"
echo "capture_json=$CAPTURE_JSON"
echo "capture_bmp=$CAPTURE_BMP"

if [[ "${#failures[@]}" -gt 0 ]]; then
  printf 'live_window_result=FAIL failures=%s\n' "${failures[*]}"
  exit 1
fi

echo "live_window_result=PASS"
