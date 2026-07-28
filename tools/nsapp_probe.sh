#!/bin/bash
# MacRunner 2026-07-28 (HK input lane) — 40-second winecfg reproduction of the
# "[NSApp run] dispatches no NSEvent" defect, with the activation state of the
# live Cocoa process measured from outside.
#
# Why a script and not ad-hoc shell: the wine session only lives ~60s, so
# launching in one tool call and probing in the next races the teardown, and the
# startup is flaky under memory pressure (err:virtual:try_map_free_area), so a
# single attempt regularly measures nothing at all.  Launch, wait, probe and
# scoped-kill therefore all happen inside one process, and the whole thing
# retries until it actually gets a Cocoa app to look at.
#
# Usage: tools/nsapp_probe.sh <logfile> [attempts] [program...]
#
# The program matters: `explorer /desktop=…` produces a window that never reaches
# the screen (kCGWindowIsOnscreen=false), which cannot distinguish "the order-in
# path never ran" from "it ran and -setActivationPolicy: failed".  `notepad`
# produces an ordinary top-level window that upstream does order in.

set -u

REPO=/Users/timurtoby/Documents/MacRunner/Main/MacRunner
DIST="$REPO/engine/wine/dist-arm64ec-spike"
LOG="${1:-/tmp/mr-agents/nsapp-probe.log}"
ATTEMPTS="${2:-3}"
shift 2 2>/dev/null || true
if [ "$#" -gt 0 ]; then PROGRAM=("$@"); else PROGRAM=(explorer /desktop=nsappprobe,900x700 winecfg); fi
export WINEPREFIX=/tmp/mr-agents/nsapp-probe
export MACRUNNER_TRACE_WINEMAC_INPUT=1

cleanup() {
    "$DIST/bin/wineserver" -k >/dev/null 2>&1
    sleep 2
    # Scoped to this prefix's tempdir loader only — never a global pkill.
    for p in $(ps -Awwo pid,comm | awk '$2 ~ /\.exe$/ {print $1}'); do
        kill -TERM "$p" 2>/dev/null
    done
}

for attempt in $(seq 1 "$ATTEMPTS"); do
    echo "### attempt $attempt" >&2
    cleanup
    : > "$LOG"
    "$DIST/bin/wine" "${PROGRAM[@]}" >> "$LOG" 2>&1 &
    WPID=$!

    COCOA=""
    for i in $(seq 1 45); do
        sleep 2
        n=$(grep -c 'stage=create_cocoa_window' "$LOG" 2>/dev/null)
        [ -z "$n" ] && n=0
        if [ "$n" -gt 0 ]; then
            COCOA=$(grep 'stage=create_cocoa_window' "$LOG" | tail -1 | sed 's/.*pid=\([0-9]*\).*/\1/')
            kill -0 "$COCOA" 2>/dev/null && break
            COCOA=""
        fi
    done

    if [ -z "$COCOA" ]; then
        echo "### attempt $attempt: no live Cocoa process (startup flake) — retrying" >&2
        kill -TERM $WPID 2>/dev/null
        continue
    fi

    echo "=== COCOA_PID=$COCOA ==="
    PIDS=$(ps -Awwo pid,comm | awk '$2 ~ /\.exe$/ {print $1}' | tr '\n' ' ')
    echo "=== LaunchServices / activation ==="
    /tmp/mr-agents/ls_probe $PIDS 2>&1 | grep -v 'NIL'
    echo "=== WindowServer windows ==="
    /tmp/mr-agents/cg_list $PIDS 2>&1
    echo "=== winemac stage counts ==="
    grep -o 'stage=[a-zA-Z_]*' "$LOG" | sort | uniq -c | sort -rn | head -25
    echo "=== transform / order_in / activation traces ==="
    grep -E 'stage=(transform_to_foreground|transform_to_foreground_entry|order_in|applicationDidBecomeActive|app_sendEvent_enter|nsapp_run_returned|run_cocoa_app_decide)' "$LOG" | head -25
    cleanup
    exit 0
done

echo "### all $ATTEMPTS attempts failed to produce a live Cocoa process" >&2
cleanup
exit 1
