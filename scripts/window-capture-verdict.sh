#!/usr/bin/env bash

set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  window-capture-verdict.sh --pid <PID> [--name <process_name_filter>] [--rundir <dir>] [--strict] [--watch [--watch-timeout-mins N]]
  window-capture-verdict.sh --name <process_name_filter> [--rundir <dir>] [--strict] [--watch [--watch-timeout-mins N]]

Detects a window for a Wine/Native process (via CGWindowList), captures PNG with screencapture,
and computes non-background pixels (simple ladder marker).

Modes:
  --watch                  Poll every 5s up to timeout (default 10 minutes).
                           On first window appearance, performs immediate capture + verdict.
                           Continues with periodic captures every 30s until timeout.
  --watch-timeout-mins N   Override watcher timeout in minutes (default 10).
  --strict                 Name matching uses exact match (case-insensitive).

Default (without --watch): single immediate capture.

Output format:
  WINDOW_VERDICT: PASS non_background_pixels=N size=WxH
or
  WINDOW_VERDICT: FAIL no-window
USAGE
}

PID=""
PROC_FILTER=""
RUNDIR="${RUNDIR:-$PWD}"
STRICT=0
WATCH_MODE=0
WATCH_TIMEOUT_MIN=10

WATCH_POLL_INTERVAL=5
WATCH_CAPTURE_INTERVAL=30

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
    --strict)
      STRICT=1
      shift
      ;;
    --watch)
      WATCH_MODE=1
      shift
      ;;
    --watch-timeout-mins)
      WATCH_TIMEOUT_MIN="$2"
      if ! [[ "$WATCH_TIMEOUT_MIN" =~ ^[0-9]+$ ]]; then
        echo "Invalid --watch-timeout-mins value: $WATCH_TIMEOUT_MIN" >&2
        exit 2
      fi
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

resolve_pid() {
  local filter="$1"
  if [[ -n "$PID" ]]; then
    echo "$PID"
    return
  fi

  if [[ "$STRICT" == "1" ]]; then
    pgrep -x -i -- "$filter" | tail -n 1 || true
  else
    local found_pid
    found_pid="$(pgrep -x -i -- "$filter" | tail -n 1 || true)"
    if [[ -n "$found_pid" ]]; then
      echo "$found_pid"
      return
    fi
    pgrep -f -i -- "$filter" | awk 'NF{print $1}' | tail -n 1 || true
  fi
}

query_window_json() {
  local target_pid="$1"
  local name_filter="$2"
  swift - "$target_pid" "$name_filter" "$STRICT" <<'SWIFT'
import Foundation
import CoreGraphics

guard CommandLine.arguments.count >= 4 else {
    exit(2)
}

let pid = Int(CommandLine.arguments[1]) ?? -1
let filter = CommandLine.arguments[2].lowercased()
let strict = CommandLine.arguments[3] == "1"
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

func matchesFilter(_ windowName: String, _ ownerName: String) -> Bool {
    if nameFilter.isEmpty { return true }
    if strict {
        return ownerName.caseInsensitiveCompare(nameFilter) == .orderedSame ||
               windowName.caseInsensitiveCompare(nameFilter) == .orderedSame
    }
    return windowName.lowercased().contains(nameFilter) ||
           ownerName.lowercased().contains(nameFilter)
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
    if !matchesFilter(windowName, ownerName) {
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
}

parse_window_json() {
  local json="$1"
  local out
  out=$(python3 -c 'import json,sys; j=json.loads(sys.argv[1]); print(j.get("window_id", 0), j.get("width", 0), j.get("height", 0))' "$json")
  echo "$out"
}

capture_once() {
  local window_id="$1"
  local out_path="$2"
  local emit_verdict="$3"

  mkdir -p "$RUNDIR"
  if ! screencapture -x -l "$window_id" "$out_path" >/tmp/window-verdict-screencapture.log 2>&1; then
    return 1
  fi

  local raw
  raw=$(python3 - "$out_path" <<'PY'
from collections import Counter
from pathlib import Path
from PIL import Image
import sys

img = Image.open(sys.argv[1]).convert('RGBA')
pixels = list(img.getdata())
w, h = img.size
if not pixels:
    print('0 0 0')
else:
    bg = Counter(pixels).most_common(1)[0][0]
    non_bg = sum(1 for p in pixels if p != bg)
    print(non_bg, w, h)
PY
)

  read -r NON_BG_PIXELS WIDTH HEIGHT <<<"$raw"
  if [[ -z "$NON_BG_PIXELS" || -z "$WIDTH" || -z "$HEIGHT" ]]; then
    return 1
  fi

  if [[ "$emit_verdict" == "1" ]]; then
    echo "WINDOW_VERDICT: PASS non_background_pixels=$NON_BG_PIXELS size=${WIDTH}x${HEIGHT}"
  else
    echo "WINDOW_CAPTURE: saved=$out_path"
  fi
}

single_shot() {
  local pid="$1"
  local name_filter="$2"

  local window_json window_file
  window_json="$(query_window_json "$pid" "$name_filter" || true)"
  if [[ -z "$window_json" ]]; then
    echo "WINDOW_VERDICT: FAIL no-window"
    return 1
  fi

  local parsed
  parsed=$(parse_window_json "$window_json")
  read -r WINDOW_ID WINDOW_WIDTH WINDOW_HEIGHT <<<"$parsed"

  if [[ -z "$WINDOW_ID" || "$WINDOW_ID" == "0" ]]; then
    echo "WINDOW_VERDICT: FAIL no-window"
    return 1
  fi

  capture_once "$WINDOW_ID" "$RUNDIR/window-capture.png" 1
}

watch_mode() {
  local pid_filter="$1"
  local timeout_min="$2"

  local end_ts now_ts
  local first_found=0
  local next_capture_ts=0
  local seq=0
  local resolved_pid=""
  local window_json parsed window_id

  end_ts=$(( $(date +%s) + timeout_min * 60 ))

  while :; do
    now_ts="$(date +%s)"
    if (( now_ts >= end_ts )); then
      break
    fi

    resolved_pid="$(resolve_pid "$pid_filter")"
    if [[ -z "$resolved_pid" ]]; then
      sleep "$WATCH_POLL_INTERVAL"
      continue
    fi

    window_json="$(query_window_json "$resolved_pid" "$pid_filter" || true)"
    if [[ -z "$window_json" ]]; then
      sleep "$WATCH_POLL_INTERVAL"
      continue
    fi

    parsed=$(parse_window_json "$window_json")
    read -r WINDOW_ID WINDOW_WIDTH WINDOW_HEIGHT <<<"$parsed"
    if [[ -z "$WINDOW_ID" || "$WINDOW_ID" == "0" ]]; then
      sleep "$WATCH_POLL_INTERVAL"
      continue
    fi

    if (( first_found == 0 )); then
      capture_once "$WINDOW_ID" "$RUNDIR/window-capture.png" 1
      first_found=1
      next_capture_ts=$((now_ts + WATCH_CAPTURE_INTERVAL))
    elif (( now_ts >= next_capture_ts )); then
      seq=$((seq + 1))
      capture_once "$WINDOW_ID" "$RUNDIR/window-capture-${seq}.png" 0
      next_capture_ts=$((now_ts + WATCH_CAPTURE_INTERVAL))
    fi

    sleep "$WATCH_POLL_INTERVAL"
  done

  if (( first_found == 0 )); then
    echo "WINDOW_VERDICT: FAIL no-window"
    return 1
  fi

  return 0
}

if (( WATCH_MODE == 1 )); then
  watch_mode "$PROC_FILTER" "$WATCH_TIMEOUT_MIN" || exit 1
  exit 0
fi

PID="$(resolve_pid "$PROC_FILTER")"
if ! [[ "$PID" =~ ^[0-9]+$ ]]; then
  echo "WINDOW_VERDICT: FAIL no-window"
  exit 1
fi

single_shot "$PID" "$PROC_FILTER"
