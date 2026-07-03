#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  pixel-truth-gate.sh --pid PID [--rundir DIR] [--watch] [--timeout-mins N] [--interval-sec N]
  pixel-truth-gate.sh --name PROCESS_OR_WINDOW [--strict] [--rundir DIR] [--watch]
  pixel-truth-gate.sh PROCESS_OR_WINDOW [options]

Verdicts:
  NO_WINDOW  No matching on-screen CG window.
  BLACK      Capture succeeded, but visible pixels are effectively black.
  NONBLACK   Non-black pixels are present.
  COLORFUL   Colored pixels are present.

Exit codes:
  0  NONBLACK or COLORFUL
  2  NO_WINDOW
  3  BLACK
  4  infrastructure/capture error
  5  --watch timed out before NONBLACK/COLORFUL
  64 usage error

Notes:
  CGWindowList with optionOnScreenOnly cannot see minimized windows or windows on
  another macOS Space. The harness chooses the largest matching visible layer-0
  window and reports both logical CG bounds and captured Retina pixel dimensions.
USAGE
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROBE_SRC="$ROOT/tools/cg_window_probe.swift"
CAPTURE_SRC="$ROOT/tools/cg_window_capture.swift"
BIN_DIR="${PIXELGATE_BIN_DIR:-$ROOT/artifacts/pixel-truth-gate/bin}"
RUNDIR="$ROOT/artifacts/pixel-truth-gate/runs"

TARGET_TYPE=""
TARGET_VALUE=""
STRICT=0
WATCH=0
TIMEOUT_MINS=10
INTERVAL_SEC=5

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pid|-p)
      TARGET_TYPE="pid"
      TARGET_VALUE="${2:-}"
      shift 2
      ;;
    --name|--process|--window|-n)
      TARGET_TYPE="name"
      TARGET_VALUE="${2:-}"
      shift 2
      ;;
    --strict)
      STRICT=1
      shift
      ;;
    --rundir|-d)
      RUNDIR="${2:-}"
      shift 2
      ;;
    --watch)
      WATCH=1
      shift
      ;;
    --timeout-mins|--watch-timeout-mins)
      TIMEOUT_MINS="${2:-}"
      shift 2
      ;;
    --interval-sec)
      INTERVAL_SEC="${2:-}"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --*)
      echo "unknown option: $1" >&2
      usage >&2
      exit 64
      ;;
    *)
      if [[ -n "$TARGET_VALUE" ]]; then
        echo "multiple targets provided" >&2
        usage >&2
        exit 64
      fi
      TARGET_TYPE="name"
      TARGET_VALUE="$1"
      shift
      ;;
  esac
done

if [[ -z "$TARGET_TYPE" || -z "$TARGET_VALUE" ]]; then
  usage >&2
  exit 64
fi

if [[ "$TARGET_TYPE" == "pid" && ! "$TARGET_VALUE" =~ ^[0-9]+$ ]]; then
  echo "--pid must be numeric" >&2
  exit 64
fi
if ! [[ "$TIMEOUT_MINS" =~ ^[0-9]+([.][0-9]+)?$ && "$INTERVAL_SEC" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "--timeout-mins and --interval-sec must be numeric" >&2
  exit 64
fi

json_error() {
  local message="$1"
  python3 - "$TARGET_TYPE" "$TARGET_VALUE" "$message" <<'PY'
import json, sys, time
target_type, target_value, message = sys.argv[1:4]
print(json.dumps({
    "schema": "pixel-truth-gate.v1",
    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "target": {"type": target_type, "value": target_value},
    "verdict": "ERROR",
    "error": message,
    "exit_code": 4,
}, separators=(",", ":"), sort_keys=True))
PY
}

compile_helper() {
  local src="$1"
  local out="$2"
  mkdir -p "$(dirname "$out")"
  if [[ ! -x "$out" || "$src" -nt "$out" ]]; then
    swiftc -O "$src" -o "$out"
  fi
}

ensure_helpers() {
  command -v swiftc >/dev/null 2>&1 || return 1
  command -v python3 >/dev/null 2>&1 || return 1
  compile_helper "$PROBE_SRC" "$BIN_DIR/cg_window_probe"
  compile_helper "$CAPTURE_SRC" "$BIN_DIR/cg_window_capture"
}

select_window() {
  local probe_json="$1"
  python3 - "$TARGET_TYPE" "$TARGET_VALUE" "$STRICT" "$probe_json" <<'PY'
import json, sys
target_type, target_value, strict, probe_path = sys.argv[1:5]
strict = strict == "1"
with open(probe_path, "r", encoding="utf-8") as f:
    payload = json.load(f)
target_lower = target_value.lower()

def visible(w):
    try:
        return int(w.get("layer", 999)) == 0 and float(w.get("alpha", 0)) > 0 and int(w.get("width", 0)) > 0 and int(w.get("height", 0)) > 0
    except Exception:
        return False

def matches(w):
    if target_type == "pid":
        try:
            return int(w.get("pid", -1)) == int(target_value)
        except Exception:
            return False
    owner = str(w.get("owner", "")).lower()
    name = str(w.get("name", "")).lower()
    if strict:
        return owner == target_lower or name == target_lower
    return target_lower in owner or target_lower in name

windows = [w for w in payload.get("windows", []) if visible(w) and matches(w)]
windows.sort(key=lambda w: (int(w.get("width", 0)) * int(w.get("height", 0)), float(w.get("alpha", 0)), int(w.get("window_id", 0))), reverse=True)
if not windows:
    sys.exit(2)
print(json.dumps(windows[0], separators=(",", ":"), sort_keys=True))
PY
}

emit_no_window() {
  local attempt_dir="$1"
  local json_path="$attempt_dir/result.json"
  python3 - "$TARGET_TYPE" "$TARGET_VALUE" "$STRICT" "$json_path" "$attempt_dir/probe.json" <<'PY'
import json, sys, time
target_type, target_value, strict, json_path, probe_path = sys.argv[1:6]
payload = {
    "schema": "pixel-truth-gate.v1",
    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "target": {"type": target_type, "value": target_value, "strict": strict == "1"},
    "window": None,
    "metrics": None,
    "evidence": {"json": json_path, "probe_json": probe_path, "png": None, "bmp": None},
    "limitations": ["Only on-screen CG windows are visible; minimized windows and windows on another Space are reported as NO_WINDOW."],
    "verdict": "NO_WINDOW",
    "exit_code": 2,
}
line = json.dumps(payload, separators=(",", ":"), sort_keys=True)
open(json_path, "w", encoding="utf-8").write(line + "\n")
print(line)
PY
}

emit_capture_result() {
  local attempt_dir="$1"
  local selected_json="$2"
  local capture_json_path="$3"
  local bmp_path="$4"
  local png_path="$5"
  local json_path="$attempt_dir/result.json"

  python3 - "$TARGET_TYPE" "$TARGET_VALUE" "$STRICT" "$selected_json" "$capture_json_path" "$bmp_path" "$png_path" "$json_path" "$attempt_dir/probe.json" <<'PY'
import binascii, collections, json, os, struct, sys, time, zlib

target_type, target_value, strict, selected_path, capture_path, bmp_path, png_path, json_path, probe_path = sys.argv[1:10]
strict = strict == "1"
window = json.load(open(selected_path, "r", encoding="utf-8"))
capture = json.load(open(capture_path, "r", encoding="utf-8"))

def emit_error(message):
    payload = {
        "schema": "pixel-truth-gate.v1",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "target": {"type": target_type, "value": target_value, "strict": strict},
        "window": window,
        "metrics": None,
        "evidence": {"json": json_path, "probe_json": probe_path, "capture_json": capture_path, "png": None, "bmp": bmp_path},
        "verdict": "ERROR",
        "error": message,
        "exit_code": 4,
    }
    line = json.dumps(payload, separators=(",", ":"), sort_keys=True)
    open(json_path, "w", encoding="utf-8").write(line + "\n")
    print(line)
    sys.exit(4)

if capture.get("status") != "PASS":
    emit_error(str(capture.get("error", "capture failed")))

raw = open(bmp_path, "rb").read()
if raw[:2] != b"BM":
    emit_error("capture BMP has invalid signature")
offset = struct.unpack_from("<I", raw, 10)[0]
width = struct.unpack_from("<i", raw, 18)[0]
height_raw = struct.unpack_from("<i", raw, 22)[0]
planes = struct.unpack_from("<H", raw, 26)[0]
bpp = struct.unpack_from("<H", raw, 28)[0]
compression = struct.unpack_from("<I", raw, 30)[0]
if planes != 1 or bpp != 24 or compression != 0 or width <= 0 or height_raw == 0:
    emit_error(f"unsupported BMP format: planes={planes} bpp={bpp} compression={compression} width={width} height={height_raw}")
height = abs(height_raw)
top_down = height_raw < 0
row_stride = ((width * 3 + 3) // 4) * 4

rows = []
bucket_stats = {}
non_black = 0
colorful = 0
total = width * height
black_threshold = int(os.environ.get("PIXELGATE_BLACK_THRESHOLD", "4"))
color_luma_threshold = int(os.environ.get("PIXELGATE_COLOR_LUMA_THRESHOLD", "16"))
color_delta_threshold = int(os.environ.get("PIXELGATE_COLOR_DELTA_THRESHOLD", "24"))

for y in range(height):
    bmp_y = y if top_down else height - 1 - y
    row_start = offset + bmp_y * row_stride
    rgb = bytearray()
    for x in range(width):
        b, g, r = raw[row_start + x * 3: row_start + x * 3 + 3]
        rgb.extend((r, g, b))
        maxc = max(r, g, b)
        minc = min(r, g, b)
        if maxc > black_threshold:
            non_black += 1
        if maxc > color_luma_threshold and maxc - minc > color_delta_threshold:
            colorful += 1
        bucket = (r // 16, g // 16, b // 16)
        stat = bucket_stats.setdefault(bucket, [0, 0, 0, 0])
        stat[0] += 1
        stat[1] += r
        stat[2] += g
        stat[3] += b
    rows.append(bytes(rgb))

dominant_bucket, dominant = max(bucket_stats.items(), key=lambda item: item[1][0])
dom_count, dom_r_sum, dom_g_sum, dom_b_sum = dominant
dom_r = round(dom_r_sum / dom_count)
dom_g = round(dom_g_sum / dom_count)
dom_b = round(dom_b_sum / dom_count)
background_distance = int(os.environ.get("PIXELGATE_BACKGROUND_DISTANCE", "30"))
non_background = 0
for row in rows:
    for i in range(0, len(row), 3):
        if abs(row[i] - dom_r) + abs(row[i + 1] - dom_g) + abs(row[i + 2] - dom_b) > background_distance:
            non_background += 1

def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", binascii.crc32(tag + data) & 0xffffffff)

png_raw = b"".join(b"\x00" + row for row in rows)
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(png_raw, 6))
    + chunk(b"IEND", b"")
)
os.makedirs(os.path.dirname(png_path), exist_ok=True)
open(png_path, "wb").write(png)

min_nonblack = int(os.environ.get("PIXELGATE_MIN_NONBLACK_PIXELS", "64"))
min_colorful = int(os.environ.get("PIXELGATE_MIN_COLORFUL_PIXELS", "64"))
min_nonbackground = int(os.environ.get("PIXELGATE_MIN_NONBACKGROUND_PIXELS", "64"))
if colorful >= min_colorful:
    verdict = "COLORFUL"
    exit_code = 0
elif non_black >= min_nonblack and non_background >= min_nonbackground:
    verdict = "NONBLACK"
    exit_code = 0
else:
    verdict = "BLACK"
    exit_code = 3

logical_width = int(window.get("width", 0) or 0)
logical_height = int(window.get("height", 0) or 0)
metrics = {
    "total_px": total,
    "capture_width": width,
    "capture_height": height,
    "logical_width": logical_width,
    "logical_height": logical_height,
    "retina_scale_x": round(width / logical_width, 4) if logical_width else None,
    "retina_scale_y": round(height / logical_height, 4) if logical_height else None,
    "non_background_px": non_background,
    "non_black_px": non_black,
    "colorful_px": colorful,
    "dominant_color": f"#{dom_r:02x}{dom_g:02x}{dom_b:02x}",
    "dominant_px": dom_count,
    "dominant_ratio": round(dom_count / total, 6) if total else 0,
    "thresholds": {
        "black_max_channel_gt": black_threshold,
        "color_max_channel_gt": color_luma_threshold,
        "color_channel_delta_gt": color_delta_threshold,
        "background_l1_distance_gt": background_distance,
        "min_nonblack_px": min_nonblack,
        "min_nonbackground_px": min_nonbackground,
        "min_colorful_px": min_colorful,
    },
}
payload = {
    "schema": "pixel-truth-gate.v1",
    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "target": {"type": target_type, "value": target_value, "strict": strict},
    "window": {
        "window_id": int(window.get("window_id", 0)),
        "pid": int(window.get("pid", 0)),
        "owner": window.get("owner", ""),
        "name": window.get("name", ""),
        "layer": int(window.get("layer", 0)),
        "alpha": float(window.get("alpha", 0)),
        "x": int(window.get("x", 0)),
        "y": int(window.get("y", 0)),
        "width": logical_width,
        "height": logical_height,
        "selection": "largest_visible_matching_layer0_window",
    },
    "metrics": metrics,
    "evidence": {
        "json": json_path,
        "probe_json": probe_path,
        "capture_json": capture_path,
        "png": png_path,
        "bmp": bmp_path,
    },
    "limitations": ["Only on-screen CG windows are visible; minimized windows and windows on another Space are reported as NO_WINDOW."],
    "verdict": verdict,
    "exit_code": exit_code,
}
line = json.dumps(payload, separators=(",", ":"), sort_keys=True)
open(json_path, "w", encoding="utf-8").write(line + "\n")
print(line)
sys.exit(exit_code)
PY
}

run_once() {
  local stamp attempt_dir probe_json selected_json selected_path window_id bmp_path png_path capture_json capture_json_path rc
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  attempt_dir="$RUNDIR/$stamp-$$"
  mkdir -p "$attempt_dir"
  probe_json="$attempt_dir/probe.json"

  "$BIN_DIR/cg_window_probe" > "$probe_json"
  set +e
  selected_json="$(select_window "$probe_json")"
  rc=$?
  set -e
  if [[ $rc -ne 0 ]]; then
    emit_no_window "$attempt_dir"
    return 2
  fi

  selected_path="$attempt_dir/selected-window.json"
  printf '%s\n' "$selected_json" > "$selected_path"
  window_id="$(python3 - "$selected_path" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], "r", encoding="utf-8"))["window_id"])
PY
)"
  bmp_path="$attempt_dir/window-${window_id}.bmp"
  png_path="$attempt_dir/window-${window_id}.png"
  capture_json_path="$attempt_dir/capture.json"

  set +e
  capture_json="$("$BIN_DIR/cg_window_capture" "$window_id" "$bmp_path")"
  rc=$?
  set -e
  printf '%s\n' "$capture_json" > "$capture_json_path"
  if [[ $rc -ne 0 ]]; then
    emit_capture_result "$attempt_dir" "$selected_path" "$capture_json_path" "$bmp_path" "$png_path"
    return 4
  fi

  set +e
  emit_capture_result "$attempt_dir" "$selected_path" "$capture_json_path" "$bmp_path" "$png_path"
  rc=$?
  set -e
  return "$rc"
}

if ! ensure_helpers; then
  json_error "missing swiftc/python3 or failed to compile CG helpers"
  exit 4
fi

if [[ "$WATCH" == "0" ]]; then
  run_once
  exit $?
fi

deadline="$(python3 - "$TIMEOUT_MINS" <<'PY'
import sys, time
print(time.time() + float(sys.argv[1]) * 60.0)
PY
)"

last_json=""
last_rc=2
while :; do
  set +e
  last_json="$(run_once)"
  last_rc=$?
  set -e
  verdict="$(python3 - <<'PY' "$last_json"
import json, sys
try:
    print(json.loads(sys.argv[1]).get("verdict", "ERROR"))
except Exception:
    print("ERROR")
PY
)"
  if [[ "$verdict" == "NONBLACK" || "$verdict" == "COLORFUL" ]]; then
    printf '%s\n' "$last_json"
    exit 0
  fi
  timed_out="$(python3 - "$deadline" <<'PY'
import sys, time
print("1" if time.time() >= float(sys.argv[1]) else "0")
PY
)"
  if [[ "$timed_out" == "1" ]]; then
    python3 - "$last_json" <<'PY'
import json, sys
payload = json.loads(sys.argv[1])
payload["watch_timeout"] = True
payload["exit_code"] = 5
print(json.dumps(payload, separators=(",", ":"), sort_keys=True))
PY
    exit 5
  fi
  sleep "$INTERVAL_SEC"
done
