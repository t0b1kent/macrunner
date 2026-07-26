#!/usr/bin/env bash
# Wait for a plain (no-failover) autoloop to finish, then relaunch the same lane on the
# failover chain. Used to upgrade a lane that is ALREADY MID-RUN without interrupting it:
# killing the loop would abort an in-flight 40-minute Hollow Knight run.
#
# Polls by PID; exits if the PID is already gone. Never signals anything.
#
# Usage: CHAIN="..." scripts/lane-failover-handover.sh <pid> <lane> <progress-file> <max-iters> <prompt-file>
set -uo pipefail

PID="${1:?pid of the running loop}"
LANE="${2:?lane name}"
PROGRESS="${3:?progress file}"
MAX="${4:-20}"
PROMPTFILE="${5:?prompt file}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
LOG="/tmp/mr-agents/${LANE}-autoloop.log"
mkdir -p /tmp/mr-agents

echo "=== ${LANE} handover armed $(date +%H:%M:%S): waiting for pid $PID, then failover chain '${CHAIN:-codex kimi claude}' ===" >> "$LOG"
while kill -0 "$PID" 2>/dev/null; do sleep 60; done
echo "=== ${LANE} handover firing $(date +%H:%M:%S): pid $PID gone, starting failover loop ===" >> "$LOG"

exec scripts/lane-autoloop-failover.sh "$LANE" "$PROGRESS" "$MAX" "$PROMPTFILE"
