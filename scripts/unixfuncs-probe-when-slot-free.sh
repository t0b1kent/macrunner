#!/usr/bin/env bash
# Wait for the JIT title slot, build the MemoryWineUnixFuncs probes, verify the artifact by SHA,
# run Hollow Knight once, and write a verdict that answers ONE question:
#
#   does the guest's NtQueryVirtualMemory(MemoryWineUnixFuncs) for winemac.drv ever reach
#   HyperBridge's thunk interception, and if so does it succeed or fail?
#
# The first probe answered none of that, because it sat inside `if (status && info_class == …)`
# — so its zero could mean "handler never ran" OR "handler ran and the query succeeded", which
# need opposite fixes. Both probes are now outside that branch:
#   macrunner-hb-ntdll-memory-semantic  — once per process, the handler was entered at all
#   macrunner-hb-unixfuncs-query        — every MemoryWineUnixFuncs query, with module + status
#
# Rules encoded here, each of which has already cost this project real time:
#   - NEVER build while a title run is live: replacing the binary under a running game produces
#     stale-artifact confusion that has cost a full day.
#   - VERIFY BY SHA that the artifact changed; `make` exiting 0 is not evidence. A wine-only
#     build once reported success and shipped a byte-identical ntdll.so.
#   - Count live processes on `comm`, never `ps -Awwo args | grep` — the latter matches this
#     script's own command line and has reported "busy" against an idle machine.
#   - Scoped checks only, never pkill: sister lanes share this machine.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"
OUT=/tmp/mr-agents/unixfuncs-probe-verdict.txt
NTDLL=engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so
say() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$OUT"; }
: > "$OUT"

say "armed; waiting for the slot"
W=0
while [ "$(ps -Ao comm 2>/dev/null | grep -cE 'wineserver|Hollow Knight\.exe')" -gt 0 ]; do
  sleep 60; W=$((W+1))
  [ $((W % 20)) -eq 0 ] && say "still waiting (${W} min)"
  [ "$W" -ge 180 ] && { say "ABORT: slot busy 3h"; exit 3; }
done
say "slot free after ${W} min"

BEFORE=$(shasum -a 256 "$NTDLL" 2>/dev/null | cut -c1-16)
if ! scripts/build-wine-arm64ec-spike.sh > /tmp/mr-agents/unixfuncs-build.log 2>&1; then
  say "BUILD FAILED — see /tmp/mr-agents/unixfuncs-build.log"; exit 2
fi
AFTER=$(shasum -a 256 "$NTDLL" 2>/dev/null | cut -c1-16)
say "ntdll ${BEFORE} -> ${AFTER}"
[ "$AFTER" = "$BEFORE" ] && { say "ARTIFACT UNCHANGED — not testing a stale binary"; exit 2; }
for s in unixfuncs-query ntdll-memory-semantic; do
  say "probe '$s' present in artifact: $(strings -a "$NTDLL" | grep -c "$s")"
done

say "running HK"
MACRUNNER_HK_INPUT_EVIDENCE=1 scripts/hk-run-try12-config.sh UNIXFUNCS-PROBE 1200 2 \
  > /tmp/mr-agents/unixfuncs-run.log 2>&1
RD=$(ls -dt reports/*/laneA*UNIXFUNCS-PROBE* 2>/dev/null | head -1)
say "run dir: ${RD:-none}"
[ -z "$RD" ] && exit 1

{
  echo "── handler entered at all (once per process) ──"
  grep -a 'ntdll-memory-semantic' "$RD/run.log" 2>/dev/null | head -8
  echo "── every MemoryWineUnixFuncs query: module + status ──"
  grep -ao 'unixfuncs-query: module=[^ ]* status=[0-9a-f]*' "$RD/run.log" 2>/dev/null | sort | uniq -c | sort -rn | head -20
  echo "── downstream ──"
  for s in winemac-unixlib-bridge 'PLACEHOLDER KEPT' macdrv_init_entry macdrv_key_event run_cocoa_app_entry; do
    printf '%-28s %s\n' "$s" "$(grep -ac "$s" "$RD/run.log" 2>/dev/null)"
  done
} >> "$OUT"
say "done"
