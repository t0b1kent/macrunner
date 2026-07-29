#!/usr/bin/env bash
# Answer ONE question the operator asked: does Hollow Knight get sound now that
# winecoreaudio.drv actually reaches the prefix (commit 2aa93b2b)?
#
# Runs on the only configuration known to reach the menu: the driver install deferred with
# MACRUNNER_MACDRV_SELFINIT_DELAY_MS. Without that, the boot spends ~558 extra seconds in the
# Mono load phase and never gets as far as audio mattering.
#
# The verdict is written as a BEFORE/AFTER against the recorded control, because the negative
# is the interesting reading here: the marker to hunt is FMOD's own line
#   "FMOD failed to initialize any audio devices, running on emulated software output"
# which was present at +55.7 s in laneA-HK-E2E-23-SELFINITDELAY-t1. If it is gone AND
# winecoreaudio appears, sound is initialising. If it is still there, the driver arriving was
# necessary but not sufficient, and that is a different (honest) answer — not a failure of the
# experiment.
#
# Rules encoded, each of which has cost this project time:
#   - Count live runs on `comm`, never `ps -Awwo args | grep`: the latter matches this script's
#     own command line and has reported "busy" against an idle machine.
#   - Never pkill; sister lanes share this slot and this script only ever WAITS for it.
#   - exit=53 is the known xtajit64 boot flake — retry rather than reporting a regression.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
OUT=/tmp/mr-agents/audio-check-verdict.txt
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$OUT"; }
: > "$OUT"; say "armed — waiting for the title slot"

W=0
while [ "$(ps -Ao comm 2>/dev/null | grep -cE 'wineserver|Hollow Knight\.exe')" -gt 0 ]; do
  sleep 60; W=$((W+1))
  [ $((W % 20)) -eq 0 ] && say "waiting (${W} min)"
  [ "$W" -ge 240 ] && { say "ABORT: slot busy 4h"; exit 3; }
done
say "slot free after ${W} min"

# Confirm the fix is actually in the script that will build the prefix, not just in git.
say "sync list carries winecoreaudio.drv: $(grep -c 'winecoreaudio.drv' scripts/sync-prefix-from-dist.sh)"

export MACRUNNER_MACDRV_SELFINIT_DELAY_MS=420000
export MACRUNNER_HK_GOOD_BOOT_MARKER='Loaded Objects now'
say "launching (selfinit delay 420000 ms — the config that reached the menu)"
scripts/hk-run-try12-config.sh AUDIOCHECK 2700 3 > /tmp/mr-agents/audio-check-run.log 2>&1
say "launcher rc=$?"

RD=$(ls -dt reports/*/laneA*AUDIOCHECK* 2>/dev/null | head -1)
say "run dir: ${RD:-none}"
[ -z "$RD" ] && exit 1
L="$RD/run.log"

{
  echo "── the marker that matters (FMOD's own words) ──"
  grep -aiE 'FMOD.*(failed|initial|device)' "$L" | head -5
  echo "   occurrences of 'failed to initialize any audio devices': $(grep -ac 'failed to initialize any audio devices' "$L")"
  echo "── did the driver reach the process ──"
  for s in winecoreaudio mmdevapi 'audio device' AudioUnit; do
    printf '   %-24s %s\n' "$s" "$(grep -aci "$s" "$L")"
  done
  echo "── how far the boot got ──"
  for s in 'Restored language' 'Loaded Objects now' 'UnloadTime'; do
    printf '   %-24s %s\n' "$s" "$(grep -ac "$s" "$L")"
  done
  echo "── control, for comparison (the recorded menu run) ──"
  C=$(ls -dt reports/*/laneA*E2E-23-SELFINITDELAY-t1* 2>/dev/null | head -1)
  [ -n "$C" ] && echo "   control FMOD-failed count: $(grep -ac 'failed to initialize any audio devices' "$C/run.log")"
} >> "$OUT"
say "done"
