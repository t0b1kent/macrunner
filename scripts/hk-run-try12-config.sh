#!/usr/bin/env bash
# Run Hollow Knight with EXACTLY the environment of try12 — the run that reached the main
# menu on 2026-07-28 — so the only thing that differs is whatever the caller changed.
#
# Why this exists: laneA-run-hk.sh is a different configuration, not a neutral wrapper. It
# defaults MACRUNNER_HB_DIRECT_MEM=1 and MACRUNNER_HB_SINGLE_LOOKUP=1, while try12 ran with
# both EMPTY, and it reaches level start in roughly 1.2% of runs. Using it to test an
# unrelated fix confounded the fix with a low-yield configuration and cost a title slot.
# Every value below was read out of try12's own run-contract.json, not reconstructed.
#
# exit=53 is the known xtajit64-c0000135 boot flake (load_dll race in a fresh prefix), so
# retry it rather than reporting it as a regression — that misread cost three runs today.
#
# Never `ps | grep -q` under pipefail, and never pkill: count matches on `comm` (the exe
# name) so the check cannot match this script's own command line, which is what made an
# earlier slot check read "busy" against an empty machine.
set -uo pipefail

TAG="${1:?need tag}"; TMO="${2:-5400}"; MAX="${3:-5}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

EXE="$ROOT/../game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/Hollow Knight.exe"
[ -f "$EXE" ] || { echo "no exe: $EXE" >&2; exit 2; }

NTDLL_SHA="$(shasum -a 256 engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so 2>/dev/null | cut -c1-16)"
CACHE_ROOT="$ROOT/artifacts/hb-translation-cache/ntdll-${NTDLL_SHA}"
echo "[hk] translation cache root: $CACHE_ROOT ($([ -d "$CACHE_ROOT" ] && echo WARM || echo COLD))"

for try in $(seq 1 "$MAX"); do
  BUSY="$(ps -Awwo comm 2>/dev/null | grep -cE 'wineserver|Hollow Knight\.exe')"
  if [ "$BUSY" -gt 0 ]; then echo "[hk] slot busy, waiting"; sleep 30; continue; fi

  RUNDIR="$ROOT/reports/phase4-hollow-knight/laneA-${TAG}-try${try}-$(date +%H%M%S)"
  mkdir -p "$RUNDIR"
  echo "$RUNDIR" > /tmp/mr-agents/hk-current-run.txt
  echo "[hk] try ${try}/${MAX} -> $(basename "$RUNDIR")"

  env \
    MACRUNNER_RUN_DIR="$RUNDIR" \
    MACRUNNER_GRAPHICS_BACKEND=dxmt \
    MACRUNNER_HB_BACKEND=jit \
    MACRUNNER_HB_X64_LOADER=1 \
    MACRUNNER_HB_JIT_DIRECT_MEM=0 \
    MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=1 \
    MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1 \
    MACRUNNER_HB_JIT_DIRECT_STACK=1 \
    MACRUNNER_HB_TRANSLATION_CACHE=1 \
    MACRUNNER_HB_TRANSLATION_CACHE_ROOT="$CACHE_ROOT" \
    MACRUNNER_HB_TRACE_TRANSLATION_CACHE=1 \
    MACRUNNER_FLIGHT_RECORDER=1 \
    MACRUNNER_PREFIX_SYSTEM32_ARCH=x86_64-windows \
    MACRUNNER_MR_RUN_PREFIX_TEMPLATE="$ROOT/artifacts/hk-prefix-template-NOSERVICES" \
    MACRUNNER_MR_RUN_SKIP_WINEBOOT=1 \
    MACRUNNER_MR_RUN_START_SERVICES=0 \
    MACRUNNER_MR_RUN_ACTXPRXY=0 \
    MACRUNNER_MR_RUN_CRT_CASE_FUSION=0 \
    MACRUNNER_MR_RUN_WWISE_OBSERVER=0 \
    MACRUNNER_TRACE_WINEMAC_INPUT=1 \
    WINEDEBUG=-all \
    WINEDLLOVERRIDES='mono-profiler-hk_language=n;d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b' \
    scripts/mr-run.sh engine/wine/dist-arm64ec-spike "$EXE" "$TMO" > "$RUNDIR/run.log" 2>&1
  RC=$?

  echo "[hk] try ${try} rc=$RC"
  if [ "$RC" -eq 53 ]; then
    echo "[hk] known boot flake (53), retrying"
    sleep 10
    continue
  fi
  exit "$RC"
done
echo "[hk] gave up after $MAX tries"
exit 53
