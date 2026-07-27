#!/usr/bin/env bash
# Finish the advapi32 W-registry stub removal unattended: wait for the JIT title slot,
# then link+install, verify the artifact by SHA, and run the 30-second probe.
#
# Written because waiting on a busy slot and then asking the operator is wasted time — the
# whole sequence is deterministic, so it belongs in a script, not in a conversation.
#
# Safety rules this encodes, each of which has cost this project real time:
#   - NEVER build/install while a title run is live: replacing ntdll.so under a running game
#     produces the stale-artifact confusion that cost a full day (makedep.c dependency bug).
#   - VERIFY BY SHA what actually lands in the dist, not that `make install` exited 0.
#   - The probe is the acceptance test and it must be run against the SAME dist the game uses
#     (dist-arm64ec-spike), which is what run-probe.sh already targets.
#   - Scoped checks only; never pkill.
#
# Usage: scripts/registry-fix-finish-when-slot-free.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
LOG="/tmp/mr-agents/registry-fix-finish.log"
mkdir -p /tmp/mr-agents
JOURNAL="reports/research/LANE-HK-REGISTRY-PROGRESS.md"
DIST_NTDLL="engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so"

say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
note() { printf '%s · %s\n' "$(date +%H:%M)" "$*" >> "$JOURNAL"; }

say "=== registry-fix finisher armed ==="
BEFORE_SHA="$(shasum -a 256 "$DIST_NTDLL" 2>/dev/null | cut -c1-16)"
say "dist ntdll before: ${BEFORE_SHA:-absent}"

# 1. wait for the title slot — a run may legitimately take an hour
WAITED=0
# NB: never `ps | grep -q` under `set -o pipefail` — grep -q exits on first match, ps takes
# SIGPIPE, pipefail propagates the failure and the condition reads FALSE while a run is live.
# That misfire deployed ntdll.so under a live run on 2026-07-28. Count into a variable instead.
while [ "$(ps -Awwo args 2>/dev/null | grep -cE '[m]r-run\.sh|[e]xtracted-hollow-knight')" -gt 0 ]; do
  sleep 60; WAITED=$((WAITED+1))
  [ $((WAITED % 15)) -eq 0 ] && say "still waiting for the slot (${WAITED} min)"
  if [ "$WAITED" -ge 240 ]; then
    say "ABORT: slot still busy after 4h"
    note "COORDINATOR AUTOMATION · registry-fix finisher ABORTED: the JIT title slot was still busy after 4 hours, so link+install+probe never ran. The source patch IS applied and compiles (macrunner_hb.o rebuilt clean); only the deploy and acceptance test are outstanding."
    exit 3
  fi
done
say "slot free after ${WAITED} min"

# 2. build + install
say "building (build-wine-arm64ec-spike.sh)"
if ! scripts/build-wine-arm64ec-spike.sh >> "$LOG" 2>&1; then
  say "BUILD FAILED"
  note "COORDINATOR AUTOMATION · registry-fix finisher: BUILD FAILED after the W-registry stub removal. Log: $LOG. The pre-patch engine diff is preserved at reports/phase4-hollow-knight/checkpoints/20260728-pre-registry-patch-engine-dirty-PRESERVE/."
  exit 2
fi

# 3. verify the artifact actually changed — `make install` exiting 0 is not evidence
AFTER_SHA="$(shasum -a 256 "$DIST_NTDLL" 2>/dev/null | cut -c1-16)"
say "dist ntdll after: ${AFTER_SHA:-absent}"
if [ "$AFTER_SHA" = "$BEFORE_SHA" ]; then
  say "ARTIFACT UNCHANGED — install did not land"
  note "COORDINATOR AUTOMATION · registry-fix finisher: STOPPED. dist ntdll.so SHA is unchanged (${BEFORE_SHA}) after a successful build, i.e. the install did not land — the exact stale-artifact shape the makedep.c bug produced. Not running the probe against a stale binary."
  exit 2
fi

# 4. acceptance test — 30 seconds, not a title run
say "running the registry probe"
PROBE_OUT="/tmp/mr-agents/registry-probe-after-fix.txt"
tools/hk_registry_probe/run-probe.sh > "$PROBE_OUT" 2>&1
PRC=$?
say "probe rc=$PRC"
VERDICT="$(grep -aoE 'GameLangSet[^|]{0,60}|M2H_lastLanguage[^|]{0,40}|val=[0-9]+|rc=[0-9]+' "$PROBE_OUT" 2>/dev/null | head -8 | tr '\n' ' ')"
say "probe verdict: $VERDICT"

note "COORDINATOR AUTOMATION · registry-fix finisher COMPLETED unattended. Slot waited ${WAITED} min. dist ntdll.so ${BEFORE_SHA} -> ${AFTER_SHA} (artifact verified changed by SHA, not by exit code). Probe rc=${PRC}; extracted: ${VERDICT}. Full probe output: ${PROBE_OUT}; build log: ${LOG}. If GameLangSet now reads through the W path, the language gate disappears entirely and the confirmedLanguage poll is never entered."
say "=== done ==="
