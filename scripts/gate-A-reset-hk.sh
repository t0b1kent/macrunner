#!/usr/bin/env bash
# Gate A: HK profiling with live background sampler.
# Survives early exits by sampling while HK is alive and captures triage + classify.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. config/env.sh 2>/dev/null || true

TAG="${1:-A-fix-live}"
TMO="${2:-300}"
HK="${MACRUNNER_HK_EXE:-$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe}"
RUNDIR="$ROOT/reports/lane-a/$TAG"
mkdir -p "$RUNDIR"

export DXMT_HEADLESS=1
export MACRUNNER_HB_TRANSLATION_CACHE=0

# Cleanup previous
rm -f "$RUNDIR"/*.log "$RUNDIR"/*.txt "$RUNDIR/.sample-done" "$RUNDIR/.hk-found" 2>/dev/null || true

echo "[gate-A] long run: $TAG, TMO=$TMO, HK=$HK"

# Background sampler: wait for Hollow Knight process and sample it until it dies.
# Capture one full-duration sample so the profile covers the whole live window,
# not a single overwritten 5 s snapshot at the end.
(
    n=0
    while [ "$n" -lt 300 ]; do
        # MacRunner: the actual HK Wine process appears as the PE executable path
        # (comm = "/.../Hollow Knight.exe"), not as a bare "Hollow Knight" name.
        HKPID=$(pgrep -x "Hollow Knight.exe" 2>/dev/null || pgrep "Hollow Knight" 2>/dev/null | head -1 || true)
        if [ -n "$HKPID" ]; then
            echo "$HKPID" > "$RUNDIR/.hk-found"
            echo "[gate-A] background sampler: HK pid=$HKPID"
            # Full run-length sample (stops automatically when HK exits).
            sample "$HKPID" "$TMO" -f "$RUNDIR/sample-hk-${TMO}s-live.txt" 2> "$RUNDIR/sample.err" || true
            touch "$RUNDIR/.sample-done"
            exit 0
        fi
        sleep 1
        n=$((n+1))
    done
    echo "[gate-A] background sampler: HK never appeared within 300s"
) &
SAMPLE_PID=$!

# Long run
scripts/laneA-run-hk.sh "$TAG" "$TMO" 1 >"$RUNDIR/laneA-run.log" 2>&1 || true

# Wait for sampler to finish or process gone
wait "$SAMPLE_PID" 2>/dev/null || true

# 8-boot gate
echo "[gate-A] 8-boot gate (60s each)"
for i in $(seq 1 8); do
    scripts/laneA-run-hk.sh "${TAG}-boot-$i" 60 1 >"$RUNDIR/boot-$i.log" 2>&1 || true
    if grep -Eqi 'c0000005|access violation|segmentation fault|Unhandled exception|page fault|c0000017' "$RUNDIR/boot-$i.log"; then
        echo "[gate-A] FAULT on boot $i"
        echo "FAULT boot=$i" >>"$RUNDIR/faults.log"
    fi
done

# classify long run if available
LONG_RUN=$(grep -oE '/Users/.*laneA-'"$TAG"'-try1-[0-9]+' "$RUNDIR/laneA-run.log" | head -1 || true)
if [ -n "$LONG_RUN" ] && [ -f "$LONG_RUN/run.log" ]; then
    python3 tools/triage/classify_run.py "$LONG_RUN" >"$RUNDIR/classify.log" 2>&1 || true
fi

echo "[gate-A] results in $RUNDIR"
echo "[gate-A] check:"
echo "  - sample live: ls $RUNDIR/sample-hk-*-live.txt"
echo "  - ladder: tail $RUNDIR/classify.log"
echo "  - faults: cat $RUNDIR/faults.log"
