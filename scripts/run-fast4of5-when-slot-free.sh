#!/usr/bin/env bash
# Launch the 4-of-5 fast-path run as soon as the JIT title slot frees. Never preempts a live run.
# NB: count into a variable — `ps | grep -q` under `set -o pipefail` reads FALSE while a match
# exists (grep -q exits early, ps takes SIGPIPE, pipefail propagates). That misfire deployed a
# binary under a live run on 2026-07-28.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
LOG=/tmp/mr-agents/fast4of5.log; mkdir -p /tmp/mr-agents
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
say "armed, waiting for the slot"
W=0
while [ "$(ps -Awwo args 2>/dev/null | grep -cE '[m]r-run\.sh|[e]xtracted-hollow-knight')" -gt 0 ]; do
  sleep 60; W=$((W+1)); [ $((W%20)) -eq 0 ] && say "still waiting (${W}m)"
  [ "$W" -ge 180 ] && { say "abort: slot busy 3h"; exit 3; }
done
say "slot free after ${W}m — launching FAST 4-of-5"
exec bash reports/phase4-hollow-knight/run-FAST4of5.sh
