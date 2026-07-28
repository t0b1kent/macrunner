#!/usr/bin/env bash
# Build + verify the reg_mui_cache ARM64X twin fix (kernelbase/registry.c) once the JIT
# title slot frees, then run the lane's A/B acceptance check.
#
# Why a script: the whole sequence is deterministic, and waiting on a busy slot in a
# conversation burns the operator's attention for nothing.
#
# Safety rules encoded here, each of which has already cost this project real time:
#   - NEVER build/install while a title run is live: replacing artifacts under a running
#     game produced the stale-artifact confusion that cost a full day.
#   - NEVER `ps | grep -q` under `set -o pipefail`: grep -q exits on first match, ps takes
#     SIGPIPE, pipefail propagates it, and the condition reads FALSE while a run is very
#     much alive. That misfire deployed ntdll.so under a live run on 2026-07-28. Count into
#     a variable instead.
#   - VERIFY THE ARTIFACT, not the exit code. `make install` returning 0 is not evidence
#     that anything landed; check that the shipped file actually changed.
#   - Scoped checks only, never pkill: several lanes run concurrently.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
LOG="/tmp/mr-agents/muicache-fix-finish.log"
mkdir -p /tmp/mr-agents
JOURNAL="reports/research/LANE-HK-INPUT-PROGRESS.md"
KB="engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows/kernelbase.dll"

say()  { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
note() { printf '%s · %s\n' "$(date +%H:%M)" "$*" >> "$JOURNAL"; }

say "=== muicache-fix finisher armed ==="
BEFORE="$(shasum -a 256 "$KB" 2>/dev/null | cut -c1-16)"
say "kernelbase.dll before: ${BEFORE:-absent}"

WAITED=0
while [ "$(ps -Awwo args 2>/dev/null | grep -cE '[m]r-run\.sh|[e]xtracted-hollow.knight')" -gt 0 ]; do
  sleep 60; WAITED=$((WAITED+1))
  [ $((WAITED % 15)) -eq 0 ] && say "still waiting for the slot (${WAITED} min)"
  if [ "$WAITED" -ge 240 ]; then
    say "ABORT: slot still busy after 4h"
    note "COORDINATOR AUTOMATION · muicache finisher ABORTED: slot busy >4h, build never ran. The source fix IS applied in engine/wine/dlls/kernelbase/registry.c."
    exit 3
  fi
done
say "slot free after ${WAITED} min"

say "building"
if ! scripts/build-wine-arm64ec-spike.sh >> "$LOG" 2>&1; then
  say "BUILD FAILED"
  note "COORDINATOR AUTOMATION · muicache finisher: BUILD FAILED on the reg_mui_cache twin fix. Log: $LOG"
  exit 2
fi

AFTER="$(shasum -a 256 "$KB" 2>/dev/null | cut -c1-16)"
say "kernelbase.dll after: ${AFTER:-absent}"
if [ "$AFTER" = "$BEFORE" ]; then
  say "ARTIFACT UNCHANGED — install did not land"
  note "COORDINATOR AUTOMATION · muicache finisher: STOPPED. kernelbase.dll SHA unchanged (${BEFORE}) after a successful build — the install did not land. Not testing a stale binary."
  exit 2
fi

note "COORDINATOR AUTOMATION · muicache finisher: kernelbase.dll ${BEFORE} -> ${AFTER}, artifact verified changed by SHA. The reg_mui_cache ARM64X twin is now list_init()ed lazily from whichever .data view runs first. ACCEPTANCE = the lane's own A/B: with the STOCK template (not NOMUI) \`init_locale done\` must be >0 and explorer.exe must survive past 1s. If it does, the NOMUI prefix workaround is no longer needed and the remaining input blocker is ONLY the engine-lane native-dispatch reject (\`reason=not-x64-main-process\`)."
say "=== done ==="
