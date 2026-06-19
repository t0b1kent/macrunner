#!/usr/bin/env bash
# Gate A: long HK run + sample + 8-boot stale-exec watch + classify.
# Run this on a host where wineserver is allowed to bind (NOT in this sandbox).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. config/env.sh 2>/dev/null || true

TAG="${1:-A-fix-1800}"
TMO="${2:-1800}"
HK="${MACRUNNER_HK_EXE:-$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe}"
RUNDIR="$ROOT/reports/lane-a/$TAG"
mkdir -p "$RUNDIR"

export DXMT_HEADLESS=1
export MACRUNNER_HB_TRANSLATION_CACHE=0

echo "[gate-A] long run: $TAG, TMO=$TMO, HK=$HK"
scripts/laneA-run-hk.sh "$TAG" "$TMO" 1 >"$RUNDIR/laneA-run.log" 2>&1 || true

# sample if process appeared
HKPID=$(pgrep -x "Hollow Knight" || true)
if [ -n "$HKPID" ]; then
    echo "[gate-A] sampling HK pid=$HKPID for 300s"
    sample "$HKPID" 300 -f "$RUNDIR/sample-hk-300s.txt" 2>"$RUNDIR/sample.err" || true
else
    echo "[gate-A] WARNING: Hollow Knight process not found; no sample"
fi

# 8-boot gate
echo "[gate-A] 8-boot gate (60s each)"
for i in $(seq 1 8); do
    scripts/laneA-run-hk.sh "${TAG}-boot-$i" 60 1 >"$RUNDIR/boot-$i.log" 2>&1 || true
    if grep -Eqi 'c0000005|access violation|segmentation fault|Unhandled exception|page fault' "$RUNDIR/boot-$i.log"; then
        echo "[gate-A] FAULT on boot $i"
        echo "FAULT boot=$i" >>"$RUNDIR/faults.log"
    fi
done

# classify
if [ -d "$RUNDIR" ] && [ -f "$RUNDIR/run.log" ]; then
    python3 tools/triage/classify_run.py "$RUNDIR" >"$RUNDIR/classify.log" 2>&1 || true
fi

echo "[gate-A] results in $RUNDIR"
echo "[gate-A] check:"
echo "  - sample: grep -E 'munmap|mmap|pthread_jit_write_protect_np|memset|block_cache_reset|hb_jit_buffer_reset|hb_jit_runtime_reset' $RUNDIR/sample-hk-300s.txt"
echo "  - ladder: tail $RUNDIR/classify.log"
echo "  - faults: cat $RUNDIR/faults.log"
