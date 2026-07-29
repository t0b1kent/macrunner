#!/usr/bin/env bash
# Catch a wedged Hollow Knight the moment it wedges, and sample it before it dies.
#
# Why this exists: on 2026-07-29 a run froze at +58 s and sat at ~82 % CPU for 30 minutes; by the
# time anyone went to sample it, a supervisor had torn it down and the evidence was gone. A live
# `sample` has refuted more hypotheses on this project than any rerun, so it must be taken
# automatically, not when someone remembers.
#
# Definition of wedged used here: the run log has not grown for STALL_S seconds while the process
# is still alive and burning CPU. That is exactly the signature of the class we keep hitting —
# alive, hot, silent — and it is cheap to test.
#
# Rules encoded:
#   - Match processes on `comm`, never `ps -Awwo args | grep`: the latter matches this script's
#     own command line and has reported a busy slot against an idle machine.
#   - Never kill anything. This observes only; the run's own supervisor owns its lifetime.
#   - One sample per run dir, so a long stall does not produce a pile of identical profiles.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
STALL_S="${STALL_S:-180}"
OUTDIR=/tmp/mr-agents/wedges; mkdir -p "$OUTDIR"
LOG="$OUTDIR/sampler.log"
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
say "=== wedge sampler armed (stall=${STALL_S}s) ==="

declare -a SAMPLED=()
already(){ local d; for d in "${SAMPLED[@]:-}"; do [ "$d" = "$1" ] && return 0; done; return 1; }

while :; do
  sleep 30
  # `ps -Ao pid,comm` prints the full executable PATH, which contains spaces ("Hollow
  # Knight.exe"), so awk splits it across $2 and $3 and `$2 ~ /Hollow Knight\.exe$/` can never
  # match. The first version of this sampler used exactly that and was therefore blind: it sat
  # armed through a live 15-minute run and caught nothing, while reporting "0 runs" to me.
  # Match the whole line instead, and take the pid from field 1.
  PID=$(ps -Ao pid,comm 2>/dev/null | grep 'Hollow Knight\.exe' | awk '{print $1}' | head -1)
  [ -z "$PID" ] && continue

  RD=$(ls -dt reports/*/laneA-* 2>/dev/null | head -1)
  [ -z "$RD" ] && continue
  L="$RD/run.log"; [ -f "$L" ] || continue
  already "$RD" && continue

  AGE=$(( $(date +%s) - $(stat -f %m "$L" 2>/dev/null || echo 0) ))
  [ "$AGE" -lt "$STALL_S" ] && continue

  CPU=$(ps -o %cpu= -p "$PID" 2>/dev/null | tr -d ' ')
  STAMP=$(date +%H%M%S)
  OUT="$OUTDIR/wedge-${STAMP}-pid${PID}.sample"
  say "WEDGE: pid=$PID cpu=${CPU} log_age=${AGE}s dir=$(basename "$RD") -> $(basename "$OUT")"

  # 6 seconds at 10 ms; -mayDie so a process that dies mid-sample still yields what it had.
  sample "$PID" 6 10 -mayDie -file "$OUT" >/dev/null 2>&1

  {
    echo "── wedge $(date '+%Y-%m-%d %H:%M:%S') pid=$PID cpu=${CPU} log_age=${AGE}s"
    echo "── run dir: $RD"
    echo "── last non-noise log line:"
    grep -avE 'pe-relocate|dynamic-import|ldr-init|tlsset' "$L" 2>/dev/null | tail -1 | cut -c1-160
    echo "── heaviest frames:"
    grep -oE '^ *[0-9]+ +[A-Za-z_][A-Za-z0-9_:]*' "$OUT" 2>/dev/null \
      | awk '{print $2}' | sort | uniq -c | sort -rn | head -14
    echo "── fault-router guard fired: $(grep -ac 'fault-reentry-break' "$L" 2>/dev/null)"
  } >> "$OUTDIR/wedge-${STAMP}-summary.txt" 2>/dev/null

  SAMPLED+=("$RD")
  say "sample written; summary at wedge-${STAMP}-summary.txt"
done
