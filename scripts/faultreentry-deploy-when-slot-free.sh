#!/usr/bin/env bash
# Link + deploy the always-on fault-router re-entrancy guard once the JIT title slot frees.
#
# Verifies by CONTENT as well as by SHA. An "artifact unchanged" SHA check alone gave a false
# abort on 2026-07-29 because a sister lane had already built the identical sources; and a
# wine-only build has previously reported success while shipping a byte-identical ntdll.so.
# Count live runs on `comm`, never `ps -Awwo args | grep` (that matches this script itself).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
OUT=/tmp/mr-agents/faultreentry-deploy.txt
NTDLL=engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so
MARK=macrunner-hb-fault-reentry-break
say(){ printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$OUT"; }
: > "$OUT"; say "armed"

W=0
while [ "$(ps -Ao comm 2>/dev/null | grep -cE 'wineserver|Hollow Knight\.exe')" -gt 0 ]; do
  sleep 60; W=$((W+1))
  [ $((W % 20)) -eq 0 ] && say "waiting (${W} min)"
  [ "$W" -ge 240 ] && { say "ABORT: slot busy 4h"; exit 3; }
done
say "slot free after ${W} min"

BEFORE=$(shasum -a 256 "$NTDLL" 2>/dev/null | cut -c1-16)
HAD=$(strings -a "$NTDLL" 2>/dev/null | grep -c "$MARK")
say "before: sha=${BEFORE} guard_present=${HAD}"
if ! scripts/build-wine-arm64ec-spike.sh > /tmp/mr-agents/faultreentry-build.log 2>&1; then
  say "BUILD FAILED"; grep -aE 'error:' /tmp/mr-agents/faultreentry-build.log | head -5 >> "$OUT"; exit 2
fi
AFTER=$(shasum -a 256 "$NTDLL" 2>/dev/null | cut -c1-16)
NOW=$(strings -a "$NTDLL" 2>/dev/null | grep -c "$MARK")
say "after:  sha=${AFTER} guard_present=${NOW}"
if [ "$NOW" -eq 0 ]; then say "CONTENT CHECK FAILED — guard string absent from shipped ntdll"; exit 2; fi
say "DEPLOYED. Any run from now on that would have hung in the fault router prints"
say "'${MARK}' once and falls through to normal handling instead."
