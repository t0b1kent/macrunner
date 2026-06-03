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
DISPLAY_CAPTURE_JSON="$LIVE_DIR/lane-d-live-window-present-$STAMP.display.json"
DISPLAY_CAPTURE_PNG="$LIVE_DIR/lane-d-live-window-present-$STAMP.display.png"
DISPLAY_CAPTURE_BMP="$LIVE_DIR/lane-d-live-window-present-$STAMP.display.bmp"
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
LIVE_MIN_CAPTURE_WIDTH="${LIVE_MIN_CAPTURE_WIDTH:-0}"
LIVE_MIN_CAPTURE_HEIGHT="${LIVE_MIN_CAPTURE_HEIGHT:-0}"
LIVE_FULLSCREEN_ENTER="${LIVE_FULLSCREEN_ENTER:-0}"
LIVE_FORCE_MESSAGE_WINDOW="${LIVE_FORCE_MESSAGE_WINDOW:-0}"
LIVE_DISPLAY_CAPTURE="${LIVE_DISPLAY_CAPTURE:-$LIVE_FULLSCREEN_ENTER}"
LIVE_MIN_DISPLAY_NONBLACK_PIXELS="${LIVE_MIN_DISPLAY_NONBLACK_PIXELS:-1000}"
LIVE_MIN_DISPLAY_COLORFUL_PIXELS="${LIVE_MIN_DISPLAY_COLORFUL_PIXELS:-0}"
LIVE_MIN_DISPLAY_WIDTH="${LIVE_MIN_DISPLAY_WIDTH:-640}"
LIVE_MIN_DISPLAY_HEIGHT="${LIVE_MIN_DISPLAY_HEIGHT:-400}"
if [[ -z "${LIVE_REQUIRE_WINDOW_CAPTURE+x}" ]]; then
  if [[ "$LIVE_FULLSCREEN_ENTER" == "1" ]]; then
    LIVE_REQUIRE_WINDOW_CAPTURE=0
  else
    LIVE_REQUIRE_WINDOW_CAPTURE=1
  fi
fi

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
  DXMT_SMOKE_FULLSCREEN_ENTER="$LIVE_FULLSCREEN_ENTER" \
  DXMT_SMOKE_FORCE_MESSAGE_WINDOW="$LIVE_FORCE_MESSAGE_WINDOW" \
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

display_capture_status="SKIP"
if [[ "$LIVE_DISPLAY_CAPTURE" == "1" ]]; then
  if screencapture -x "$DISPLAY_CAPTURE_PNG" >"$DISPLAY_CAPTURE_JSON.screencapture.out" 2>"$DISPLAY_CAPTURE_JSON.screencapture.err" &&
     sips -s format bmp "$DISPLAY_CAPTURE_PNG" --out "$DISPLAY_CAPTURE_BMP" >"$DISPLAY_CAPTURE_JSON.sips.out" 2>"$DISPLAY_CAPTURE_JSON.sips.err" &&
     python3 - "$DISPLAY_CAPTURE_BMP" "$DISPLAY_CAPTURE_PNG" >"$DISPLAY_CAPTURE_JSON" <<'PY'
import json
import struct
import sys

bmp_path, png_path = sys.argv[1], sys.argv[2]
data = open(bmp_path, "rb").read()
if data[:2] != b"BM":
    raise SystemExit("not a BMP")

pixel_offset = struct.unpack_from("<I", data, 10)[0]
width = struct.unpack_from("<i", data, 18)[0]
height_raw = struct.unpack_from("<i", data, 22)[0]
bpp = struct.unpack_from("<H", data, 28)[0]
if width <= 0 or height_raw == 0 or bpp not in (24, 32):
    raise SystemExit("unsupported BMP geometry")

height = abs(height_raw)
bytes_per_pixel = bpp // 8
stride = ((width * bpp + 31) // 32) * 4
nonblack = 0
colorful = 0
alpha_nonzero = 0

for y in range(height):
    row = pixel_offset + y * stride
    for x in range(width):
        offset = row + x * bytes_per_pixel
        b = data[offset]
        g = data[offset + 1]
        r = data[offset + 2]
        a = data[offset + 3] if bpp == 32 else 255
        maxc = max(r, g, b)
        minc = min(r, g, b)
        if a:
            alpha_nonzero += 1
        if r > 8 or g > 8 or b > 8:
            nonblack += 1
        if maxc - minc > 24 and maxc > 40:
            colorful += 1

payload = {
    "status": "PASS",
    "width": width,
    "height": height,
    "bpp": bpp,
    "alpha_nonzero": alpha_nonzero,
    "nonblack": nonblack,
    "colorful": colorful,
    "path": png_path,
    "bmp_path": bmp_path,
}
print(json.dumps(payload, sort_keys=True))
PY
  then
    display_capture_status="PASS"
  else
    display_capture_status="FAIL"
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
  capture_ok="$(python3 - "$CAPTURE_JSON" "$LIVE_MIN_NONBLACK_PIXELS" "$LIVE_MIN_COLORFUL_PIXELS" "$LIVE_MIN_CAPTURE_WIDTH" "$LIVE_MIN_CAPTURE_HEIGHT" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
min_nonblack = int(sys.argv[2])
min_colorful = int(sys.argv[3])
min_width = int(sys.argv[4])
min_height = int(sys.argv[5])
ok = (
    payload.get("status") == "PASS"
    and int(payload.get("nonblack", 0)) >= min_nonblack
    and int(payload.get("colorful", 0)) >= min_colorful
    and int(payload.get("width", 0)) >= min_width
    and int(payload.get("height", 0)) >= min_height
)
print(1 if ok else 0)
PY
)"
fi

display_capture_report="status=NONE width=0 height=0 nonblack=0 colorful=0"
display_capture_ok=0
if [[ -s "$DISPLAY_CAPTURE_JSON" ]]; then
  display_capture_report="$(python3 - "$DISPLAY_CAPTURE_JSON" <<'PY'
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
  display_capture_ok="$(python3 - "$DISPLAY_CAPTURE_JSON" "$LIVE_MIN_DISPLAY_NONBLACK_PIXELS" "$LIVE_MIN_DISPLAY_COLORFUL_PIXELS" "$LIVE_MIN_DISPLAY_WIDTH" "$LIVE_MIN_DISPLAY_HEIGHT" <<'PY'
import json
import sys

payload = json.load(open(sys.argv[1], encoding="utf-8"))
min_nonblack = int(sys.argv[2])
min_colorful = int(sys.argv[3])
min_width = int(sys.argv[4])
min_height = int(sys.argv[5])
ok = (
    payload.get("status") == "PASS"
    and int(payload.get("nonblack", 0)) >= min_nonblack
    and int(payload.get("colorful", 0)) >= min_colorful
    and int(payload.get("width", 0)) >= min_width
    and int(payload.get("height", 0)) >= min_height
)
print(1 if ok else 0)
PY
)"
fi

failures=()
[[ "$smoke_rc" -eq 0 ]] || failures+=("smoke_rc=$smoke_rc")
[[ -n "$window_id" ]] || failures+=("window_not_found")
if [[ "$LIVE_REQUIRE_WINDOW_CAPTURE" == "1" ]]; then
  [[ "$capture_status" == "PASS" ]] || failures+=("capture_status=$capture_status")
  [[ "$capture_ok" == "1" ]] || failures+=("capture_pixels_below_threshold")
fi
if [[ "$LIVE_DISPLAY_CAPTURE" == "1" ]]; then
  [[ "$display_capture_status" == "PASS" ]] || failures+=("display_capture_status=$display_capture_status")
  [[ "$display_capture_ok" == "1" ]] || failures+=("display_capture_below_threshold")
fi
rg -q "window_visible=1" "$OUTER_LOG" || failures+=("visible_window_marker_missing")
rg -q "UnityStabilityProbe result=PASS" "$OUTER_LOG" || failures+=("stability_probe_missing")
rg -q "Present\\(resized1\\) hr=0x00000000" "$OUTER_LOG" || failures+=("present_resized1_missing")
rg -q "pixel_readback=PASS" "$OUTER_LOG" || failures+=("pixel_readback_missing")
if [[ "$LIVE_FULLSCREEN_ENTER" == "1" ]]; then
  rg -q "UnitySwapchainProbe SetFullscreenState\\(TRUE\\) hr=0x00000000" "$OUTER_LOG" || failures+=("fullscreen_enter_missing")
  rg -q "UnitySwapchainProbe fullscreen_after_true=1" "$OUTER_LOG" "$RAW_LOG" || failures+=("fullscreen_state_missing")
  rg -q "UnitySwapchainProbe SetFullscreenState\\(FALSE restore\\) hr=0x00000000" "$OUTER_LOG" || failures+=("fullscreen_restore_missing")
fi

echo "outer=$OUTER_LOG"
echo "raw=$RAW_LOG"
echo "smoke_rc=$smoke_rc"
echo "fullscreen_enter=$LIVE_FULLSCREEN_ENTER force_message_window=$LIVE_FORCE_MESSAGE_WINDOW"
echo "window_capture_required=$LIVE_REQUIRE_WINDOW_CAPTURE"
echo "capture_thresholds nonblack=$LIVE_MIN_NONBLACK_PIXELS colorful=$LIVE_MIN_COLORFUL_PIXELS width=$LIVE_MIN_CAPTURE_WIDTH height=$LIVE_MIN_CAPTURE_HEIGHT"
echo "display_capture=$LIVE_DISPLAY_CAPTURE"
echo "display_capture_thresholds nonblack=$LIVE_MIN_DISPLAY_NONBLACK_PIXELS colorful=$LIVE_MIN_DISPLAY_COLORFUL_PIXELS width=$LIVE_MIN_DISPLAY_WIDTH height=$LIVE_MIN_DISPLAY_HEIGHT"
echo "window_id=${window_id:-NONE}"
echo "capture_status=$capture_status"
echo "capture=$capture_report"
echo "display_capture_status=$display_capture_status"
echo "display_capture_result=$display_capture_report"
echo "probe_json=$PROBE_JSON"
echo "capture_json=$CAPTURE_JSON"
echo "capture_bmp=$CAPTURE_BMP"
echo "display_capture_json=$DISPLAY_CAPTURE_JSON"
echo "display_capture_png=$DISPLAY_CAPTURE_PNG"

if [[ "${#failures[@]}" -gt 0 ]]; then
  printf 'live_window_result=FAIL failures=%s\n' "${failures[*]}"
  exit 1
fi

echo "live_window_result=PASS"
