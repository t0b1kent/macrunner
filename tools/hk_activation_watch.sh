#!/bin/bash
# MacRunner 2026-07-28 (HK input lane) — measure the ACTIVATION POLICY of the live
# Hollow Knight process while it runs.
#
# Why this run is necessary even though the brief says to prefer the 40s winecfg
# probe: the winecfg/notepad probe reproduces `app_sendEvent_enter = 0`, but it
# does so on a process (explorer.exe) whose only window never reaches the screen
# (kCGWindowIsOnscreen=false).  A process with no on-screen window is *expected*
# to sit at activationPolicy=prohibited, so that probe cannot tell us whether HK
# — which demonstrably DOES render an on-screen window — is also Prohibited.
# That distinction is the whole question, and only a title run answers it.
#
# Wrapper choice matters and cost one slot today: laneA-run-hk.sh is NOT a neutral
# wrapper (its own header says it reaches level start in ~1.2% of runs and that
# using it confounds a fix with a low-yield configuration), and invoking it bare
# also leaves MACRUNNER_MR_RUN_{ACTXPRXY,CRT_CASE_FUSION,WWISE_OBSERVER} unset, so
# the run-contract ledger returns BLOCKED and mr-run exits *before wine*.
# hk-run-try12-config.sh replicates try12 — the run that reached the main menu on
# 2026-07-28 — and already sets MACRUNNER_TRACE_WINEMAC_INPUT=1.  Do not override
# its environment: deviating is exactly what wasted the earlier slot.
#
# Usage: tools/hk_activation_watch.sh <tag> [timeout_seconds] [max_tries]

set -u
ROOT=/Users/timurtoby/Documents/MacRunner/Main/MacRunner
TAG="${1:-nsapp-activation}"
TMO="${2:-2400}"
MAX="${3:-2}"
OUT="$ROOT/reports/phase4-hollow-knight/hk-activation-policy-$TAG.txt"

# MACRUNNER_WINE_BIN TRAP (cost a slot on 2026-07-28, exit=66, ladder rung 1 vs best 11):
# `. config/env.sh` exports MACRUNNER_WINE_BIN=$MACRUNNER_ROOT/engine/wine/dist/bin/wine —
# the *May 13* dist — and mr-run.sh:65 is `WINE="${MACRUNNER_WINE_BIN:-$DIST/bin/wine}"`,
# so the exported value OVERRIDES the dist-arm64ec-spike argument silently.  The run then
# looks like a catastrophic ladder regression when it is just the wrong wine binary.
# final-child.json records the binary actually used — check it before believing any verdict.
#
# Unsetting is NOT enough: mr-run.sh:56 sources config/env.sh ITSELF, and env.sh uses
#   : "${MACRUNNER_WINE_BIN:=$MACRUNNER_ROOT/engine/wine/dist/bin/wine}"
# so the variable is always set by the time mr-run.sh:65 reads it and the
# `${MACRUNNER_WINE_BIN:-$DIST/bin/wine}` fallback can never fire.  The only way for a
# caller to get the dist it asked for is to export the binary explicitly.
export MACRUNNER_WINE_BIN="$ROOT/engine/wine/dist-arm64ec-spike/bin/wine"

: > "$OUT"
echo "# HK activation-policy watch tag=$TAG timeout=${TMO}s max_tries=$MAX" >> "$OUT"
echo "# MACRUNNER_WINE_BIN unset (env.sh would force the stale engine/wine/dist)" >> "$OUT"
echo "# wrapper=scripts/hk-run-try12-config.sh (try12 env, unmodified)" >> "$OUT"

"$ROOT/scripts/hk-run-try12-config.sh" "$TAG" "$TMO" "$MAX" > /tmp/mr-agents/hk-run-$TAG.log 2>&1 &
RUNPID=$!

# Poll for the game process and record its LaunchServices state over time.  Match
# on comm, never on the full command line: `ps -Awwo args | grep <pattern>` matches
# this script's own command line and has produced phantom hits repeatedly.  Also
# note the real path is `game-hollow.knight-(89718)` with a DOT, so a
# 'hollow knight' pattern with a space silently misses a running game.
seen_game=0
last=""
for i in $(seq 1 100000); do
    sleep 10
    kill -0 $RUNPID 2>/dev/null || { echo "## run wrapper exited at t=$((i*10))s" >> "$OUT"; break; }

    PIDS=$(ps -Awwo pid,comm | awk '$2 ~ /\.exe$/ {print $1}' | tr '\n' ' ')
    [ -z "$PIDS" ] && continue
    GAME=$(ps -Awwo pid,comm | awk '$2 ~ /Hollow Knight\.exe$/ {print $1}' | head -1)
    [ -n "$GAME" ] && seen_game=1

    # Only append when the interesting line actually changes, so a 40-minute run
    # does not produce 240 identical blocks.
    cur=$( { /tmp/mr-agents/ls_probe $PIDS 2>&1 | grep -vE 'NIL|^runningApplications|^frontmost'
             /tmp/mr-agents/cg_list  $PIDS 2>&1 | grep -v 'no windows for'; } )
    if [ "$cur" != "$last" ]; then
        { echo "--- t=$((i*10))s game_pid=${GAME:-none} ---"; echo "$cur"; } >> "$OUT"
        last="$cur"
    fi
done

echo "## watch finished (saw Hollow Knight.exe: $seen_game)" >> "$OUT"
wait $RUNPID 2>/dev/null
echo "## run wrapper rc=$?" >> "$OUT"
