#!/usr/bin/env bash
# Watch the NOMUI run for the ONE decisive edge: does kernelbase's DllMain survive far enough
# that macdrv_init runs and the Cocoa event loop is entered?
#
# The markers below are UNGATED fprintf's in the shipped winemac.so (verified by `strings`
# before this ran), so a zero here means "did not execute" and not "printer was off" — the
# distinction that cost this project eight wrong hypotheses on 2026-07-28.
# Never `ps | grep -q` under pipefail: grep -q exits early, ps takes SIGPIPE, and the test
# reads FALSE while the run is alive. Count into a variable.
set -uo pipefail
RUNDIR="${1:?run dir}"
F="$RUNDIR/run.log"
OUT="$RUNDIR/VERDICT-input.txt"
say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$OUT"; }

say "watcher armed on $(basename "$RUNDIR")"
ANNOUNCED=0
for i in $(seq 1 180); do   # 180 x 30s = 90 min
  sleep 30
  ALIVE=$(ps -Awwo args 2>/dev/null | grep -cE '[m]r-run\.sh|[e]xtracted-hollow.knight')
  INIT=$(grep -ac 'macdrv_init_entry' "$F" 2>/dev/null)
  CALL=$(grep -ac 'macdrv_init_start_cocoa_call' "$F" 2>/dev/null)
  RUNC=$(grep -ac 'run_cocoa_app_entry' "$F" 2>/dev/null)
  NOGFX=$(grep -ac 'macdrv_init_no_graphic_access' "$F" 2>/dev/null)
  LOCALE=$(grep -ac 'init_locale done' "$F" 2>/dev/null)

  if [ "$ANNOUNCED" -eq 0 ] && [ "$INIT" -gt 0 ]; then
    ANNOUNCED=1
    say "*** macdrv_init RAN (entry=$INIT) — kernelbase survived, the MUI workaround holds ***"
  fi
  if [ "$RUNC" -gt 0 ]; then
    say "*** run_cocoa_app ENTERED (=$RUNC) — the Cocoa event loop is up; input is now POSSIBLE ***"
    say "GOAL: init_locale_done=$LOCALE macdrv_init=$INIT start_cocoa=$CALL run_cocoa=$RUNC"
    exit 0
  fi
  if [ "$NOGFX" -gt 0 ]; then
    say "BLOCKED: macdrv_init reached but SessionGetInfo denied graphic access — driver not initialised"
    exit 2
  fi
  if [ "$ALIVE" -eq 0 ]; then
    say "run ended. init_locale_done=$LOCALE macdrv_init=$INIT start_cocoa=$CALL run_cocoa=$RUNC"
    [ "$INIT" -gt 0 ] && say "PARTIAL: kernelbase survived but the Cocoa loop never started." \
                      || say "NEGATIVE: macdrv_init never ran — kernelbase still dies, NOMUI insufficient."
    exit 1
  fi
  [ $((i % 10)) -eq 0 ] && say "…${i}0s: alive=$ALIVE locale_done=$LOCALE macdrv_init=$INIT run_cocoa=$RUNC"
done
say "watcher timed out at 90 min"
