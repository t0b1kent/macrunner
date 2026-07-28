#!/bin/bash
# MacRunner 2026-07-28 (HK input lane) — catch a wine process that owns a
# genuinely VISIBLE top-level window and read its activation policy from
# outside, before the session tears itself down.
#
# Why polling and not "launch, sleep, probe": the earlier probes measured
# explorer.exe, whose four shell windows are not WS_VISIBLE and are therefore
# legitimately never ordered in — so `policy=prohibited` on them proves nothing.
# winecfg's dialog IS visible, but it only lives for a few seconds here, so the
# probe has to fire the moment its pid appears, not on a fixed sleep.
#
# Usage: tools/nsapp_live_probe.sh <target-comm-regex> <logfile> [program...]

set -u

REPO=/Users/timurtoby/Documents/MacRunner/Main/MacRunner
DIST="$REPO/engine/wine/dist-arm64ec-spike"
TARGET="${1:-winecfg\.exe$}"
LOG="${2:-/tmp/mr-agents/it2/live.log}"
shift 2 2>/dev/null || true
if [ "$#" -gt 0 ]; then PROGRAM=("$@"); else PROGRAM=(winecfg); fi

export WINEPREFIX="${WINEPREFIX:-/tmp/mr-agents/nsapp-probe2}"
export MACRUNNER_TRACE_WINEMAC_INPUT=1
export MACRUNNER_HB_TRACE_WINSHOW=1

: > "$LOG"
"$DIST/bin/wine" "${PROGRAM[@]}" >> "$LOG" 2>&1 &
WPID=$!
echo "launcher pid=$WPID prefix=$WINEPREFIX program=${PROGRAM[*]}"

TARGET_PID=""
for i in $(seq 1 120); do
    TARGET_PID=$(ps -Awwo pid,comm | awk -v re="$TARGET" '$2 ~ re {print $1; exit}')
    [ -n "$TARGET_PID" ] && break
    sleep 1
done

if [ -z "$TARGET_PID" ]; then
    echo "### target '$TARGET' never appeared"
else
    echo "### target pid=$TARGET_PID appeared at t+${i}s"
    # Sample it repeatedly: the policy is lifted lazily, on the first window
    # order-in, so a single reading right at spawn would report `prohibited`
    # even in a healthy run.
    for s in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$TARGET_PID" 2>/dev/null || { echo "### t+${s}: target exited"; break; }
        ALL=$(ps -Awwo pid,comm | awk '$2 ~ /\.exe$/ {print $1}' | tr '\n' ' ')
        echo "--- sample $s (live exes: $ALL) ---"
        /tmp/mr-agents/ls_probe $ALL 2>&1 | grep -v '^runningApplications'
        /tmp/mr-agents/cg_list $ALL 2>&1
        sleep 2
    done
fi

kill -TERM $WPID 2>/dev/null
sleep 1
"$DIST/bin/wineserver" -k >/dev/null 2>&1
echo "### done"
