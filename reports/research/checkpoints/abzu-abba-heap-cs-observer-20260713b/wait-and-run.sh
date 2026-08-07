#!/usr/bin/env bash
# Wait for an exclusive host, then fire the single observer run.
# run-once.sh is fail-closed and only consumes the attempt AFTER its preflight
# passes, so a lost race costs nothing but a retry.
set -uo pipefail

OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713b"
CLEAN_NEEDED=6      # consecutive clean samples (5s each) => 30s of silence
SAMPLE=5
MAX_WAIT=1500       # ~25 min

host_busy() {
    pgrep -f 'wineserver|/mr-run\.sh|AbzuGame-Win64-Shipping|Hollow Knight' >/dev/null 2>&1
}

clean=0
waited=0
while [ "$waited" -lt "$MAX_WAIT" ]; do
    if host_busy; then
        [ "$clean" -gt 0 ] && echo "[wait] host busy again, resetting (was ${clean} clean)"
        clean=0
    else
        clean=$((clean + 1))
        echo "[wait] clean ${clean}/${CLEAN_NEEDED}"
    fi

    if [ "$clean" -ge "$CLEAN_NEEDED" ]; then
        echo "[wait] host quiet, firing observer run"
        bash "$OUT/run-once.sh"
        rc=$?
        echo "[wait] run-once rc=$rc"
        if [ "$rc" -eq 90 ]; then
            echo "[wait] lost the race (serialization); attempt NOT consumed, retrying"
            clean=0
        else
            exit "$rc"
        fi
    fi
    sleep "$SAMPLE"
    waited=$((waited + SAMPLE))
done

echo "[wait] TIMED OUT after ${MAX_WAIT}s — host never went quiet; attempt NOT consumed"
exit 75
