#!/usr/bin/env bash
# Prepared HK long run. Execute only when the host is assigned to Lane A.
# Usage: scripts/laneA-run-hk-1800-ready.sh [tag] [timeout_sec] [max_tries]
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-hk-1800-native-memmove-heartbeat}"
TMO="${2:-1800}"
MAX="${3:-1}"
WINDOW_SEC="${HK_POST_SWAPCHAIN_WINDOW_SEC:-770}"

export MACRUNNER_HB_NATIVE_MEMMOVE=1
export MACRUNNER_HB_TRACE_HEARTBEAT="${MACRUNNER_HB_TRACE_HEARTBEAT:-1}"
export MACRUNNER_HB_TRACE_D3D_BOUNDARY="${MACRUNNER_HB_TRACE_D3D_BOUNDARY:-1}"
export MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN="${MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN:-1}"
export MACRUNNER_NO_AUTOTRIAGE="${MACRUNNER_NO_AUTOTRIAGE:-1}"

echo "[laneA-1800] prepared tag=$TAG timeout=${TMO}s max_tries=$MAX window=${WINDOW_SEC}s"
echo "[laneA-1800] env MACRUNNER_HB_NATIVE_MEMMOVE=$MACRUNNER_HB_NATIVE_MEMMOVE MACRUNNER_HB_TRACE_HEARTBEAT=$MACRUNNER_HB_TRACE_HEARTBEAT"

set +e
"$ROOT/scripts/laneA-run-hk.sh" "$TAG" "$TMO" "$MAX"
rc=$?

RUNDIR="$(cat /tmp/laneA-current-rundir.txt 2>/dev/null || true)"
if [ -z "$RUNDIR" ] || [ ! -f "$RUNDIR/run.log" ]; then
  echo "[laneA-1800] missing run.log for post-run analysis: ${RUNDIR:-<none>}" >&2
  exit "$rc"
fi

python3 - "$RUNDIR/run.log" "$RUNDIR/heartbeat-post-swapchain.txt" "$RUNDIR/heartbeat-post-swapchain.json" "$WINDOW_SEC" <<'PY'
import collections
import json
import re
import sys

log_path, txt_path, json_path, window_s = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
with open(log_path, "r", encoding="utf-8", errors="ignore") as f:
    lines = f.read().splitlines()

marker = "macrunner-hb-dxgi-swapchain: create method=CreateSwapChainForHwnd"
swap_idx = None
swap_t = None
for i, line in enumerate(lines):
    if marker in line and ("rc=0x0" in line or "rc=(nil)" in line or "rc=0x00000000" in line):
        swap_idx = i
        m = re.search(r"\+([0-9]+(?:\.[0-9]+)?)s\]", line)
        if m:
            swap_t = float(m.group(1))
        break

def field(line, name):
    m = re.search(r"\b" + re.escape(name) + r"=([^ ]+)", line)
    return m.group(1) if m else "?"

hist = collections.Counter()
sampled = 0
for i, line in enumerate(lines):
    if "macrunner-hb-heartbeat:" not in line:
        continue
    if swap_idx is not None and i <= swap_idx:
        continue
    m = re.search(r"\+([0-9]+(?:\.[0-9]+)?)s\]", line)
    if swap_t is not None and m and float(m.group(1)) > swap_t + window_s:
        continue
    key = (field(line, "label"), field(line, "rva"), field(line, "block_pc"))
    hist[key] += 1
    sampled += 1

memmove_hits = 0
memmove_bytes = 0
memmove_lines = 0
for line in lines:
    m = re.search(r"macrunner-hb-native-memmove: hits=([0-9]+) bytes=([0-9]+)", line)
    if m:
        memmove_hits = int(m.group(1))
        memmove_bytes = int(m.group(2))
        memmove_lines += 1

top = [
    {"count": count, "label": label, "rva": rva, "block_pc": block_pc}
    for (label, rva, block_pc), count in hist.most_common(30)
]
payload = {
    "log": log_path,
    "swapchain_line_index": swap_idx,
    "swapchain_time_sec": swap_t,
    "window_sec": window_s,
    "heartbeat_samples": sampled,
    "top": top,
    "native_memmove": {
        "hits": memmove_hits,
        "bytes": memmove_bytes,
        "trace_lines": memmove_lines,
    },
}

with open(json_path, "w", encoding="utf-8") as f:
    json.dump(payload, f, indent=2)

with open(txt_path, "w", encoding="utf-8") as f:
    f.write("post_swapchain_heartbeat_window_sec=%s\n" % int(window_s))
    f.write("swapchain_time_sec=%s\n" % ("UNKNOWN" if swap_t is None else ("%.3f" % swap_t)))
    f.write("heartbeat_samples=%d\n" % sampled)
    f.write("native_memmove_hits=%d native_memmove_bytes=%d trace_lines=%d\n" % (memmove_hits, memmove_bytes, memmove_lines))
    f.write("TOP_HEARTBEAT_RVAS:\n")
    for row in top:
        f.write("  count=%d label=%s rva=%s block_pc=%s\n" % (row["count"], row["label"], row["rva"], row["block_pc"]))

print("[laneA-1800] heartbeat_histogram=%s samples=%d native_memmove_hits=%d bytes=%d" %
      (txt_path, sampled, memmove_hits, memmove_bytes))
PY

exit "$rc"
