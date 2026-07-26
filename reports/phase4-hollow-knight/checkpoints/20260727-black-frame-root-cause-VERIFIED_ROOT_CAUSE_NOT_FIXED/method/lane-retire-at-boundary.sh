#!/usr/bin/env bash
# Retire a running autoloop AT AN ITERATION BOUNDARY, so a handover can install a newer loop
# without stranding work.
#
# Needed because lane-autoloop2.sh caches its prompt once before the loop (line 30): a lane started
# hours ago keeps executing the prompt it was born with, and notes appended to the progress file are
# read by the agent as data, not as instructions. The newer failover loop re-reads the prompt every
# iteration, so the fix is to let the handover swap the loop — but only at a safe moment.
#
# Safe moment = BOTH of:
#   - no agent child of the loop is mid-turn (killing then would abandon a turn's work), and
#   - no Hollow Knight run is in flight (killing then would strand a ~40-minute run nobody reads).
#
# Sends TERM to the loop's own PID only. Never pkill, never touch the run or the agent.
#
# Usage: scripts/lane-retire-at-boundary.sh <loop-pid> <lane>
set -uo pipefail

PID="${1:?loop pid}"
LANE="${2:?lane name}"
LOG="/tmp/mr-agents/${LANE}-autoloop.log"
mkdir -p /tmp/mr-agents

echo "=== ${LANE} boundary-retire armed $(date +%H:%M:%S) for pid $PID (waits for: no agent child AND no live run) ===" >> "$LOG"
while kill -0 "$PID" 2>/dev/null; do
  CHILD="$(pgrep -P "$PID" 2>/dev/null | tr '\n' ' ')"
  RUN="$(ps -Ao args 2>/dev/null | grep 'mr-run.sh' | grep -vc grep)"
  if [ -z "${CHILD// /}" ] && [ "$RUN" -eq 0 ]; then
    echo "=== ${LANE} boundary reached $(date +%H:%M:%S): no agent child, no live run — retiring loop pid $PID ===" >> "$LOG"
    kill -TERM "$PID" 2>/dev/null
    exit 0
  fi
  sleep 5
done
echo "=== ${LANE} boundary-retire: pid $PID exited on its own $(date +%H:%M:%S) ===" >> "$LOG"
