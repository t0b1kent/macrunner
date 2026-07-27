#!/usr/bin/env bash
# Unattended verdict for run30 (advapi32 W-registry fix): does Hollow Knight now read its
# saved language and SKIP the language-select menu, without the actuator?
#
# The decisive discriminator, from the managed lane's decompile of the shipped assembly:
#   menu shown  <=>  PlayerPrefs GameLangSet read returns 0
# so the oracle's SAVED branch appearing is the pass condition and the SYSTEM branch is the fail.
#
# NB: never `ps | grep -q` under pipefail — grep -q exits early, ps takes SIGPIPE, pipefail
# propagates it and the condition reads FALSE while the process is alive. Count into a variable.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
D="${1:?run dir}"
LOG=/tmp/mr-agents/run30-verdict.log; mkdir -p /tmp/mr-agents
J=reports/research/LANE-HK-MONO-PROGRESS.md
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
cnt(){ cat "$D"/run.log "$D"/launch.stdout 2>/dev/null | grep -cF "$1"; }

say "watching $D"
for i in $(seq 1 60); do
  SAVED=$(cnt "Loaded saved language code")
  SYSTEM=$(cnt "Loaded system language")
  MENU=$(cnt "Loaded language")
  LEVEL=$(cnt "Performing automatic level start.")
  UIMENU=$(cnt "Making UI menu lean.")
  ALIVE=$(ps -Awwo args 2>/dev/null | grep -cE '[m]r-run\.sh|[e]xtracted-hollow-knight')
  say "t=${i}m saved=$SAVED system=$SYSTEM level=$LEVEL uimenu=$UIMENU alive=$ALIVE"
  if [ "$SAVED" -gt 0 ] || [ "$SYSTEM" -gt 0 ]; then
    if [ "$SAVED" -gt 0 ]; then
      V="PASS — SAVED branch: \`Loaded saved language code\` x$SAVED (oracle idx33). The registry read is repaired end-to-end; the game found its language without the actuator."
    else
      V="FAIL — SYSTEM branch: \`Loaded system language\` x$SYSTEM and zero \`Loaded saved language code\`. The probe proves the W read works at the API level, so something else re-derives the state — a NEW finding, not a regression of the fix."
    fi
    printf '%s · ★ run30 REGISTRY-FIX VERDICT (coordinator automation, actuator OFF) · %s Counters at +%dm: saved=%s system=%s level-start=%s making-ui-menu-lean=%s. Run dir %s. Reminder: the probe already proved the API-level read (RegOpenKeyExW real handle 0x38, RegQueryValueExW GameLangSet rc=0 dword=1), so this run tests the END-TO-END consequence, not the fix itself.\n' \
      "$(date +%H:%M)" "$V" "$i" "$SAVED" "$SYSTEM" "$LEVEL" "$UIMENU" "$D" >> "$J"
    say "VERDICT WRITTEN: $V"
    exit 0
  fi
  if [ "$ALIVE" -eq 0 ] && [ "$i" -gt 3 ]; then
    printf '%s · ★ run30 ENDED WITHOUT A LANGUAGE LINE (coordinator automation) · Neither `Loaded saved language code` nor `Loaded system language` ever appeared and the process is gone at +%dm; level-start=%s. The run died before the language stage, so it says NOTHING about the registry fix — re-run before concluding. Run dir %s.\n' \
      "$(date +%H:%M)" "$i" "$LEVEL" "$D" >> "$J"
    say "process gone, no language line"; exit 3
  fi
  sleep 60
done
say "timeout"
