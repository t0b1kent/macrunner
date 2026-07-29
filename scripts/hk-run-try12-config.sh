#!/usr/bin/env bash
# Run Hollow Knight with the configuration of try12 — the run that reached the main menu
# on 2026-07-28 — by DELEGATING to laneA-run-hk.sh rather than re-deriving its environment.
#
# ── Why this was rewritten (measured 2026-07-29, not assumed) ──────────────────────────
# The previous version rebuilt try12's environment by hand from try12's captured
# run-contract.json and failed `d3d11: failed to create device and context (887a0002)`
# on every single run. Reading that contract shows why:
#
#   MACRUNNER_DXMT_ROOT = <try12-rundir>/dxmt-builtin-overlay
#   WINEDLLPATH         = <try12-rundir>/dxmt-builtin-overlay:.../x86_64-windows:...
#   WINESYSTEMDLLPATH   = <try12-rundir>/dxmt-builtin-overlay/x86_64-windows
#
# That overlay directory is created by laneA-run-hk.sh, per run, inside its own run dir.
# try12 WAS a laneA-run-hk.sh run. The hand-built version set none of those three, so the
# loader never found DXMT's ARM64EC twins and fell back to wine's own d3d11/dxgi. The
# overlay is the keystone (see laneA-run-hk.sh's own comment at the ARM64EC twin copy):
# it must be built per-run, so it cannot be replayed from a recorded environment at all.
#
# Second failure, also measured: mr-run.sh's run-contract preflight BLOCKS before wine
# with `runner.branch_map.{actxprxy,crt_case_fusion,wwise_observer} = branch_input_absent`
# unless those three are passed EXPLICITLY. They are branch *decisions*, so "absent from a
# recorded env dump" does not mean "safe to leave unset" — it means the contract refuses to
# run. A run that omits them exits rc=2 with a ~1250-line log and never reaches wine; that
# is NOT the exit=53 boot flake and retrying it cannot help.
#
# The general lesson, which is why this file now delegates: a run-contract records the FINAL
# CHILD environment, including values mr-run.sh and the wrapper DERIVE. Feeding derived
# values back in as inputs reproduces the symptom, not the run.
#
# exit=53 remains the known xtajit64-c0000135 boot flake (load_dll race in a fresh prefix);
# laneA-run-hk.sh already retries, so pass a try count rather than treating it as a regression.
#
# Usage: hk-run-try12-config.sh <tag> [timeout] [max_tries]
#   MACRUNNER_HK_INPUT_EVIDENCE=1  also enable the keyboard/guest input observers (below).
set -uo pipefail

TAG="${1:?need tag}"; TMO="${2:-5400}"; MAX="${3:-5}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# Count matches on `comm` (the exe name), never on `ps -Awwo args | grep`: the latter matches
# this script's own command line and has reported "busy" against an empty machine.
#
# Sister lanes (the mouse lane) share this slot, and laneA-run-hk.sh's own retry loop waits
# ~60s for a live wine and then force-runs mr-clean.sh — which would KILL their run. So we
# never let laneA retry: it is invoked with max_tries=1, and WE re-check for a free slot
# before each attempt. laneA's internal wait then exits immediately and never force-cleans.
#
# Do NOT match on `winetemp`.  Measured 2026-07-29: a leftover `services.exe` lives at
#   /var/folders/.../T//winetemp-<id>/services.exe
# so `winetemp` matches a PATH COMPONENT of a stale helper rather than a live game, and
# mr-clean.sh deliberately refuses to kill it ("winetemp lives in common TMPDIR and is not
# worktree-scoped").  The result was a launcher that waited out its full 2400 s against a
# completely idle machine.  `wineserver` alone is the honest slot indicator: every live run
# has exactly one, and a sister lane's run still registers.
wait_for_slot() {
  local waited=0
  while [ "$(ps -Ao comm | grep -cE 'wineserver')" -gt 0 ]; do
    echo "[hk] slot busy (${waited}s) — waiting for the sister lane to release it"
    sleep 20; waited=$((waited+20))
    if [ "$waited" -ge 2400 ]; then echo "[hk] slot still busy after ${waited}s" >&2; return 3; fi
  done
  echo "[hk] slot free after ${waited}s"
  return 0
}

# ── MACRUNNER_WINE_BIN MUST BE UNSET, and this is load-bearing ────────────────────────
# Measured 2026-07-29: 8/8 runs from this lane died `exit=66` at +23s, never reaching
# `macrunner-xtajit64: ThreadInit` or `Initialize engine version`. final-child.json ->
# argv.entries[0].value showed why: they ran
#   engine/wine/dist/bin/wine            <- the STALE May-13 dist
# while EVERY run that got further (try12 -> menu, try20 -> ENG+LANG+SWAP, nsapp-policy3
# -> ENG) ran
#   engine/wine/dist-arm64ec-spike/bin/wine
#
# Mechanism, read from mr-run.sh:56-67. It captures MR_WINE_BIN_OVERRIDE="$MACRUNNER_WINE_BIN"
# BEFORE sourcing config/env.sh (whose line 93 assigns it with `:=`), then uses
# `WINE="${MR_WINE_BIN_OVERRIDE:-$DIST/bin/wine}"`. That correctly stops env.sh's default from
# beating the dist argument — but it CANNOT distinguish "the caller deliberately set this" from
# "this was already exported in the invoking shell". An agent/terminal whose profile exports
# MACRUNNER_WINE_BIN therefore silently runs the stale wine on every single run, and the only
# place it shows is final-child.json. try12/try20 came from a shell where it was NOT exported
# (it appears in their contract only because env.sh set it DURING the run, after the capture).
#
# So: unset it here. Anyone who genuinely wants a different binary can still pass
# MACRUNNER_LANEA_WINE_DIST, which laneA-run-hk.sh honours.
if [ -n "${MACRUNNER_WINE_BIN:-}" ]; then
  echo "[hk] unsetting inherited MACRUNNER_WINE_BIN=$MACRUNNER_WINE_BIN (would force the stale dist)"
  unset MACRUNNER_WINE_BIN
fi

# try12's own configuration, read out of its run-contract.json.
export MACRUNNER_HB_BACKEND=jit
export MACRUNNER_HB_X64_LOADER=1
# Default 0 — byte-identical behaviour for every existing caller. The override exists so the
# HK speed lane can A/B the JIT direct-memory path by ENVIRONMENT ONLY, with one binary and
# no deploy: `[laneA] direct_mem=1` prints MACRUNNER_HB_*DIRECT_MEM (the special_read copy
# path), NOT this one, so the codegen gate has been off in every HK run and read as on.
export MACRUNNER_HB_JIT_DIRECT_MEM="${MACRUNNER_HK_FORCE_JIT_DIRECT_MEM:-0}"
export MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=1
export MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1
export MACRUNNER_HB_JIT_DIRECT_STACK=1
export MACRUNNER_HB_TRANSLATION_CACHE=1
export MACRUNNER_HB_TRACE_TRANSLATION_CACHE=1
export MACRUNNER_FLIGHT_RECORDER=1
export MACRUNNER_MR_RUN_PREFIX_TEMPLATE="$ROOT/artifacts/hk-prefix-template-NOSERVICES"
export MACRUNNER_MR_RUN_SKIP_WINEBOOT=1
export MACRUNNER_MR_RUN_START_SERVICES=0
export WINEDLLOVERRIDES='mono-profiler-hk_language=n;d3d9=n,b;d3d11,dxgi,d3d10core,winemetal=n,b'
# try12 ran WINEDEBUG=-all and that stays the default: the boot is throughput-starved and any
# broad channel both slows it and buries the run log.  But a NARROW channel is sometimes the
# only way to answer a loader question -- e.g. `-all,+loaddll` to see why LdrLoadDll returns
# STATUS_SUCCESS for winemac.drv in HK's process without ever mapping the PE or running its
# DllMain.  Override deliberately, never widen it by default.
export WINEDEBUG="${MACRUNNER_HK_WINEDEBUG:--all}"

# Required explicitly or the contract preflight blocks before wine (see header).
export MACRUNNER_MR_RUN_ACTXPRXY=0
export MACRUNNER_MR_RUN_CRT_CASE_FUSION=0
export MACRUNNER_MR_RUN_WWISE_OBSERVER=0

# Input evidence, opt-in. Both gates are deliberately NARROW:
#  - MACRUNNER_TRACE_WINEMAC_KEYS lights only keyboard.c's two stages (macdrv_key_event,
#    macdrv_send_keyboard_input_sent). The broader MACRUNNER_TRACE_WINEMAC_INPUT would also
#    light event.c's ProcessEvents_enter/_exit, which fprintf+fflush on EVERY ProcessEvents
#    call — at Unity frame rates that is a multi-hundred-MB log, a real slowdown on an
#    already throughput-starved boot, and it chokes the auto-triage.
#  - MACRUNNER_HB_RETURN_ROUTE_OBSERVER prints the GUEST side (win32u/input.c), but note it
#    early-returns unless keycode == VK_RETURN, so it only ever reports the Return key, and
#    it is capped (…_MAX, hard limit 256).
if [ "${MACRUNNER_HK_INPUT_EVIDENCE:-0}" != "0" ]; then
  export MACRUNNER_TRACE_WINEMAC_KEYS=1
  export MACRUNNER_HB_RETURN_ROUTE_OBSERVER=1
  export MACRUNNER_HB_RETURN_ROUTE_OBSERVER_MAX=256
  # Window show/activate path (window.c's macrunner_trace_winshow). Bounded to the
  # realize/show transitions, not per-frame, so it is safe to keep on with the keys gate:
  # without it, "the on-demand window was shown and activated" is unobservable, and that is
  # exactly the state that decides whether AppKit has a key window to route keys to.
  export MACRUNNER_HB_TRACE_WINSHOW=1
  # MacRunner 2026-07-29 (HK master lane iter 8) — the OWNER of HK's window, which is now the
  # single unmeasured term in the activation predicate.  winshow measured HK's window as
  # style=0x94000000 ex_style=0x0 on two runs; with ex_style==0, `excluded_by_cycle`
  # (window.c:259-260) reduces EXACTLY to "GW_OWNER != NULL", and excluded_by_cycle removes
  # ParticipatesInCycle -> -isExcludedFromWindowsMenu YES -> -canBecomeKeyWindow NO, i.e. a
  # window that can never take key focus and so can never activate its app.  The print already
  # ships at window.c:626 and fires only for WS_POPUP/DLGMODALFRAME windows, so this costs about
  # one line per popup per run — no rebuild, and no per-frame cost.
  export MACRUNNER_TRACE_SECONDARY_WINDOW=1
  echo "[hk] input evidence ON (winemac keys + guest return-route observer + winshow + owner)"
fi

# HK's boot on the current engine (ntdll 4808c18e) is stochastic across several distinct
# failure modes, measured over the day's runs: exit=66 (dies before `macrunner-xtajit64:
# ThreadInit`, ~1442-1449 lines, never reaches `Initialize engine version`), exit=5, exit=53
# (the known xtajit64-c0000135 flake), and exit=1 with a d3d11 887a0002. Consecutive runs of
# ONE unchanged config produced both 66 and a run that reached engine init, so these are
# flakes to be retried, not walls to be diagnosed from a single sample. Hence: keep attempting
# until a run gets past Mono init, which is laneA's own success condition.
for try in $(seq 1 "$MAX"); do
  wait_for_slot || exit 3
  echo "[hk] === attempt ${try}/${MAX} ==="
  "$ROOT/scripts/laneA-run-hk.sh" "${TAG}-a${try}" "$TMO" 1
  rc=$?
  # RUNDIR must be OURS.  `/tmp/laneA-current-rundir.txt` is a single global latch that EVERY
  # lane writes, so with a sibling lane running it names the sibling's run — and then the menu
  # grep below reads the wrong log.  Measured 2026-07-29 18:37: attempt 1 of BLACKFRAME-DRAWTRACE
  # reached `Restored language` at +373.155 s and `Loaded Objects now` at +427.445 s, and this
  # loop still printed "attempt 1 did not reach the menu ... — retrying" and burned a second
  # 20-minute slot, because sibling lane SYNCBLOCK-a1 had overwritten the latch. Every
  # successful run pays that twice while any sibling is alive.
  # Prefer the newest run dir carrying OUR tag; fall back to the latch only if that finds
  # nothing (single-lane machines, or a tag whose dir naming changed).
  RUNDIR="$(ls -dt "$ROOT"/reports/phase4-hollow-knight/laneA-"${TAG}-a${try}"-* 2>/dev/null | head -1)"
  if [ -z "$RUNDIR" ]; then
    RUNDIR="$(cat /tmp/laneA-current-rundir.txt 2>/dev/null || true)"
    case "$RUNDIR" in
      *"laneA-${TAG}-a${try}-"*) : ;;                      # the latch happens to be ours — fine
      "") : ;;
      *) echo "[hk] ignoring /tmp/laneA-current-rundir.txt='$RUNDIR' — it is not this run's tag"
         RUNDIR="" ;;
    esac
  fi
  # The success gate is the MENU, not Mono init.  Measured 2026-07-29 across the day's 20
  # run dirs: `Mono path` prints at ~+50 s in EVERY boot, including the ones that die at
  # ~+150 s immediately after the first CreateSwapChainForHwnd — so gating on it declared
  # GOOD BOOT for runs that never drew a menu and, worse, stopped the retry loop from
  # retrying. `Restored language` is try12's own menu signature (E2E-05: +370 s, followed by
  # `Loaded Objects now` at +424 s and a run that lived to +1061 s) and no dead boot has ever
  # printed it.  Override with MACRUNNER_HK_GOOD_BOOT_MARKER if a different title is run.
  GOOD_MARKER="${MACRUNNER_HK_GOOD_BOOT_MARKER:-Restored language}"
  if [ -n "$RUNDIR" ] && [ -f "$RUNDIR/run.log" ] && grep -q "$GOOD_MARKER" "$RUNDIR/run.log" 2>/dev/null; then
    echo "[hk] GOOD BOOT on attempt ${try} (marker: ${GOOD_MARKER}): $RUNDIR"
    echo "$RUNDIR" > /tmp/hk-e2e-good-rundir.txt
    exit 0
  fi
  echo "[hk] attempt ${try} did not reach the menu (rc=$rc, marker '${GOOD_MARKER}' absent) — retrying"
  sleep 5
done
echo "[hk] no attempt reached Mono init in $MAX tries"
exit 1
