#!/usr/bin/env bash
# Lane A HK run wrapper: mr-run + auto-retry on the xtajit64-c0000135 boot flake
# (exit=53, ~150-line log, no forward markers — load_dll race in fresh prefix).
# Usage: laneA-run-hk.sh <tag> <timeout> [max_tries]   (extra env via environment)
set -u
TAG="${1:?need tag}"; TMO="${2:-420}"; MAX="${3:-3}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HK="$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe"
for try in $(seq 1 "$MAX"); do
  # wait out any other live wine run (parallel PE32 lane) to avoid bootstrap races
  n=0
  while ps ax -o command | grep -E '\.exe|mr-run\.sh' | grep -vE 'grep|winedevice|explorer.exe /desktop|svchost|plugplay' | grep -q .; do
    sleep 15; n=$((n+1)); [ "$n" -gt 40 ] && break
  done
  RUNDIR="$ROOT/reports/phase4-hollow-knight/laneA-$TAG-try$try-$(date +%H%M%S)"
  mkdir -p "$RUNDIR"; echo "$RUNDIR" > /tmp/laneA-current-rundir.txt
  MACRUNNER_RUN_DIR="$RUNDIR" MACRUNNER_HB_TRANSLATION_CACHE="${MACRUNNER_HB_TRANSLATION_CACHE:-0}" \
    MACRUNNER_GRAPHICS_BACKEND="${MACRUNNER_GRAPHICS_BACKEND:-dxmt}" \
    MACRUNNER_MR_RUN_START_SERVICES="${MACRUNNER_MR_RUN_START_SERVICES:-1}" \
    WINEDEBUG="${WINEDEBUG:--all}" "$ROOT/scripts/mr-run.sh" \
    "$ROOT/engine/wine/dist-arm64ec-spike" "$HK" "$TMO" > "$RUNDIR/run.log" 2>&1
  rc=$?
  lines=$(wc -l < "$RUNDIR/run.log")
  echo "try$try rc=$rc lines=$lines $RUNDIR"
  if grep -q 'Mono path' "$RUNDIR/run.log" 2>/dev/null; then
    echo "VALID_RUN=$RUNDIR"; exit 0
  fi
  sleep 5
done
echo "ALL_TRIES_FLAKED"; exit 1
