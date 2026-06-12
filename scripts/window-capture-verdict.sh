#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'EOF'
Usage: window-capture-verdict.sh --pid <PID> [--name <window_name_filter>] [--rundir <dir>]
       window-capture-verdict.sh --name <process_name> [--rundir <dir>]

Снимает окно выбранного процесса через CGWindowList + screencapture, считает
не-фоновые пиксели и печатает:
  WINDOW_VERDICT: PASS non_background_pixels=N size=WxH
или:
  WINDOW_VERDICT: FAIL no-window
EOF
}

PID=""
PROC_FILTER=""
RUNDIR="${RUNDIR:-$PWD}"

while (($# > 0)); do
  case "$1" in
    --pid|-p)
      PID="$2"
      shift 2
      ;;
    --name|-n)
      PROC_FILTER="$2"
      shift 2
      ;;
    --rundir|-d)
      RUNDIR="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$PID" && -z "$PROC_FILTER" ]]; then
  echo "WINDOW_VERDICT: FAIL no-window" >&2
  usage
  exit 2
fi

if [[ -z "$PID" ]]; then
  PID="$(pgrep -x -i -- "$PROC_FILTER" | tail -n 1 || true)"
  if [[ -z "$PID" ]]; then
    PID="$(pgrep -f -i -- "$PROC_FILTER" | awk 'NF{print $1}' | tail -n 1 || true)"
  fi
fi

if [[ -z "$PID" || ! "$PID" =~ ^[0-9]+$ ]]; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

WINDOW_JSON="$(swift - "$PID" "$PROC_FILTER" <<'SWIFT'
import Foundation
import CoreGraphics

guard CommandLine.arguments.count >= 2 else {
    exit(2)
}

let pid = Int(CommandLine.arguments[1]) ?? -1
let filter = CommandLine.arguments.count >= 3 ? CommandLine.arguments[2].lowercased() : ""
let nameFilter = filter.trimmingCharacters(in: .whitespacesAndNewlines)

func toDouble(_ value: Any?) -> Double {
    if let v = value as? Double { return v }
    if let v = value as? CGFloat { return Double(v) }
    if let v = value as? Float { return Double(v) }
    if let v = value as? Int { return Double(v) }
    if let v = value as? Int64 { return Double(v) }
    if let v = value as? UInt64 { return Double(v) }
    if let v = value as? UInt32 { return Double(v) }
    if let v = value as? NSNumber { return v.doubleValue }
    return 0.0
}

func toInt(_ value: Any?) -> Int {
    if let v = value as? Int { return v }
    if let v = value as? Int64 { return Int(v) }
    if let v = value as? UInt64 { return Int(v) }
    if let v = value as? UInt32 { return Int(v) }
    if let v = value as? CGFloat { return Int(v) }
    if let v = value as? Double { return Int(v) }
    if let v = value as? NSNumber { return v.intValue }
    return 0
}

func escapeJSON(_ value: String) -> String {
    return value
        .replacingOccurrences(of: "\\", with: "\\\\")
        .replacingOccurrences(of: "\"", with: "\\\"")
        .replacingOccurrences(of: "\n", with: "\\n")
}

let windows = CGWindowListCopyWindowInfo([.excludeDesktopElements, .optionOnScreenOnly], kCGNullWindowID)
guard let windows = windows as? [[String: Any]] else {
    exit(3)
}

var chosen: (id: Int, name: String, width: Int, height: Int)?
var bestArea = -1

for window in windows {
    guard let ownerPID = window[kCGWindowOwnerPID as String].flatMap(toInt), ownerPID == pid else {
        continue
    }
    guard let layer = window[kCGWindowLayer as String].flatMap(toInt), layer == 0 else {
        continue
    }
    let windowName = (window[kCGWindowName as String] as? String) ?? ""
    let ownerName = (window[kCGWindowOwnerName as String] as? String) ?? ""
    let combinedName = "\(windowName) \(ownerName)".lowercased()
    if !nameFilter.isEmpty && !combinedName.contains(nameFilter) {
        continue
    }

    let windowId = window[kCGWindowNumber as String].flatMap(toInt) ?? 0
    let bounds = window[kCGWindowBounds as String] as? [String: Any]
    let width = Int(toDouble(bounds?["Width"]))
    let height = Int(toDouble(bounds?["Height"]))
    if width <= 0 || height <= 0 {
        continue
    }
    let area = width * height
    if area > bestArea {
        chosen = (windowId, ownerName.isEmpty ? windowName : "\(ownerName): \(windowName)".trimmingCharacters(in: .whitespacesAndNewlines), width, height)
        bestArea = area
    }
}

guard let window = chosen else {
    exit(4)
}

let output = """
{\"window_id\":\(window.id),\"name\":\"\(escapeJSON(window.name))\",\"width\":\(window.width),\"height\":\(window.height)}
"""
print(output)
SWIFT
)"

if [[ -z "$WINDOW_JSON" ]]; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

WINDOW_JSON_FILE="$(mktemp)"
printf '%s\n' "$WINDOW_JSON" > "$WINDOW_JSON_FILE"
read -r WINDOW_ID WINDOW_WIDTH WINDOW_HEIGHT < <(
  python3 -c 'import json,sys; j=json.load(open(sys.argv[1])); print(j.get("window_id", 0), j.get("width", 0), j.get("height", 0))' "$WINDOW_JSON_FILE"
)
rm -f "$WINDOW_JSON_FILE"

if [[ -z "$WINDOW_ID" || "$WINDOW_ID" == "0" ]]; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

mkdir -p "$RUNDIR"
CAPTURE_PATH="$RUNDIR/window-capture.png"

if ! screencapture -x -l "$WINDOW_ID" "$CAPTURE_PATH" >/tmp/window-verdict-screencapture.log 2>&1; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

read -r NON_BG_PIXELS WIDTH HEIGHT <<<"$(python3 - <<PY
from collections import Counter
from pathlib import Path
from PIL import Image
import sys

path = Path('$CAPTURE_PATH')
img = Image.open(path).convert('RGBA')
pixels = list(img.getdata())
w, h = img.size
if not pixels:
    print('0 0 0')
else:
    bg = Counter(pixels).most_common(1)[0][0]
    non_bg = sum(1 for p in pixels if p != bg)
    print(non_bg, w, h)
PY
)"

if [[ -z "$NON_BG_PIXELS" || -z "$WIDTH" || -z "$HEIGHT" ]]; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

echo "WINDOW_VERDICT: PASS non_background_pixels=$NON_BG_PIXELS size=${WIDTH}x${HEIGHT}"
