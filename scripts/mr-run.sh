#!/usr/bin/env bash
# MacRunner run wrapper — runs an exe under the spike Wine in a throwaway prefix and
# ALWAYS cleans up (scoped wineserver -k + remove the prefix) on exit, timeout, or Ctrl-C.
# This structurally prevents orphan hot-spinning Wine and leftover 1.5G prefixes.
#
# Usage:
#   scripts/mr-run.sh <dist-dir> <exe-path> [timeout_sec] [-- extra wine args...]
# Output (stdout/stderr of the run) goes to the terminal; redirect yourself if you want a log.
# Keeps NO prefix afterwards: copy any evidence you need out during the run.
set -u
RUN_ARTIFACT_DIR="${MACRUNNER_RUN_DIR:-}"
RUN_ARTIFACTS_REQUIRED=0
WINE_CHILD_PID_PATH=""
if [ -n "$RUN_ARTIFACT_DIR" ]; then
  mkdir -p "$RUN_ARTIFACT_DIR"
  chmod 700 "$RUN_ARTIFACT_DIR" || {
    echo "[mr-run] cannot make mandatory run artifact directory private: $RUN_ARTIFACT_DIR" >&2
    exit 125
  }
  RUN_ARTIFACTS_REQUIRED=1
  if [ -z "${MACRUNNER_RUN_CONTRACT_LEDGER_PATH:-}" ]; then
    if [ "${MACRUNNER_RUN_CONTRACT_LEDGER_PREFLIGHT_ONLY:-0}" = "1" ]; then
      export MACRUNNER_RUN_CONTRACT_LEDGER_PATH="$RUN_ARTIFACT_DIR/preflight-run-contract.json"
    else
      export MACRUNNER_RUN_CONTRACT_LEDGER_PATH="$RUN_ARTIFACT_DIR/run-contract.json"
    fi
  fi
  export MACRUNNER_FINAL_CHILD_CAPTURE_PATH="$RUN_ARTIFACT_DIR/final-child.json"
  export MACRUNNER_FLIGHT_RECORDER=1
  export MACRUNNER_FLIGHT_RECORDER_PATH="$RUN_ARTIFACT_DIR/flight.jsonl"
  WINE_CHILD_PID_PATH="$RUN_ARTIFACT_DIR/wine-child.pid"
  rm -f "$WINE_CHILD_PID_PATH"
fi
FINAL_CHILD_CAPTURE_ENABLED=0
FINAL_CHILD_CAPTURE_PATH=""
if [ "${MACRUNNER_FINAL_CHILD_CAPTURE_PATH+x}" = "x" ]; then
  FINAL_CHILD_CAPTURE_ENABLED=1
  FINAL_CHILD_CAPTURE_PATH="$MACRUNNER_FINAL_CHILD_CAPTURE_PATH"
fi
unset MACRUNNER_FINAL_CHILD_CAPTURE_PATH
if [ -n "${MACRUNNER_RUN_CONTRACT_LEDGER_PATH:-}" ]; then
  RUN_CONTRACT_CALLER_EXPLICIT_SHA256="$(
    python3 "$(cd "$(dirname "$0")/.." && pwd)/tools/run_contract_identity_ledger.py" \
      --capture-caller-explicit-sha256
  )" || {
    echo "[mr-run] caller-explicit environment capture failed" >&2
    exit 125
  }
  unset MACRUNNER_RUN_CONTRACT_CALLER_EXPLICIT_SHA256
fi
DIST="${1:?need dist dir}"; EXE="${2:?need exe path}"; TMO="${3:-120}"
shift 3 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Capture an EXPLICIT caller override BEFORE sourcing env.sh. env.sh:93 assigns
# MACRUNNER_WINE_BIN with `:=`, so after sourcing it is ALWAYS set (to
# $MACRUNNER_WINE_DIST/bin/wine) and a `${MACRUNNER_WINE_BIN:-$DIST/bin/wine}` fallback placed
# after the source can never fire. Writing it that way on 2026-07-28 silently pointed every run
# at engine/wine/dist instead of the dist passed as $1 — measured on HK as ladder rung 1 vs
# rung 8 for the correct binary. The whole point of $1 is to choose the dist, so the ARGUMENT
# must win over env.sh's default, and only a value the caller set explicitly may override it.
MR_WINE_BIN_OVERRIDE="${MACRUNNER_WINE_BIN:-}"
. "$ROOT/config/env.sh"
# Set MACRUNNER_WINE_BIN in the caller to launch through e.g. an .app bundle
# (<bundle>.app/Contents/MacOS/wine); otherwise the dist argument decides.
WINE="${MR_WINE_BIN_OVERRIDE:-$DIST/bin/wine}"
WSRV="$DIST/bin/wineserver"
WINE_UNIX_LIB="$DIST/lib/wine/aarch64-unix"
RUN_DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
RUN_DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}"
RUN_CONTRACT_LEDGER_PATH="${MACRUNNER_RUN_CONTRACT_LEDGER_PATH:-}"
if [ "${MACRUNNER_RUN_CONTRACT_LEDGER_PREFLIGHT_ONLY+x}" = "x" ]; then
  RUN_CONTRACT_PREFLIGHT_ONLY="$MACRUNNER_RUN_CONTRACT_LEDGER_PREFLIGHT_ONLY"
else
  RUN_CONTRACT_PREFLIGHT_ONLY="0"
fi

case "$RUN_CONTRACT_PREFLIGHT_ONLY" in
  0|1) ;;
  *)
    echo "[mr-run] malformed MACRUNNER_RUN_CONTRACT_LEDGER_PREFLIGHT_ONLY=$RUN_CONTRACT_PREFLIGHT_ONLY" >&2
    exit 125
    ;;
esac
if [ "${MACRUNNER_MR_RUN_IDENTITY_LEDGER+x}" = "x" ] || \
   [ "${MACRUNNER_MR_RUN_IDENTITY_PREFLIGHT_ONLY+x}" = "x" ]; then
  echo "[mr-run] legacy MACRUNNER_MR_RUN_IDENTITY_* controls are not accepted" >&2
  exit 125
fi
if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ] && [ -z "$RUN_CONTRACT_LEDGER_PATH" ]; then
  echo "[mr-run] run-contract ledger path required when preflight-only is enabled" >&2
  exit 125
fi

if [ "$(uname -s)" = "Darwin" ]; then
  # macOS msync is the in-process synchronization fast path.  Leaving it unset
  # makes ntdll fall back to wineserver waits for every handle wait.
  export WINEMSYNC="${WINEMSYNC:-1}"
fi

if [ -d "$WINE_UNIX_LIB" ]; then
  RUN_DYLD_LIBRARY_PATH="$WINE_UNIX_LIB${RUN_DYLD_LIBRARY_PATH:+:$RUN_DYLD_LIBRARY_PATH}"
  RUN_DYLD_FALLBACK_LIBRARY_PATH="$WINE_UNIX_LIB${RUN_DYLD_FALLBACK_LIBRARY_PATH:+:$RUN_DYLD_FALLBACK_LIBRARY_PATH}"
fi

if [ "$(basename "$DIST")" = "dist-arm64ec-spike" ]; then
  # x64 lane is forced by selected image architecture; keep explicit override
  # out of this generic wrapper for PE32/manual x86 paths.
  if [ "${MACRUNNER_HB_X64_LOADER:-}" = "" ]; then
    export MACRUNNER_HB_X64_LOADER="1"
  fi
  export MACRUNNER_HB_BACKEND="${MACRUNNER_HB_BACKEND:-jit}"
  export MACRUNNER_HB_JIT_DIRECT_MEM="${MACRUNNER_HB_JIT_DIRECT_MEM:-0}"
  export MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN="${MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN:-1}"
  export MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM="${MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM:-0}"
  export MACRUNNER_HB_JIT_DIRECT_STACK="${MACRUNNER_HB_JIT_DIRECT_STACK:-0}"
  export MACRUNNER_HB_TRANSLATION_CACHE="${MACRUNNER_HB_TRANSLATION_CACHE:-1}"
  export MACRUNNER_HB_TRANSLATION_CACHE_ROOT="${MACRUNNER_HB_TRANSLATION_CACHE_ROOT:-$ROOT/engine/hyperbridge/build/hyperbridge-cache}"
  export MACRUNNER_HB_TRACE_TRANSLATION_CACHE="${MACRUNNER_HB_TRACE_TRANSLATION_CACHE:-1}"
  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" != "1" ]; then
    mkdir -p "$MACRUNNER_HB_TRANSLATION_CACHE_ROOT"
  fi
  export WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}"
fi

if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ]; then
  if ! mkdir -p "$(dirname "$RUN_CONTRACT_LEDGER_PATH")"; then
    echo "[mr-run] run-contract ledger parent creation failed" >&2
    exit 125
  fi
  RUN_CONTRACT_PREFLIGHT_PREFIX="${MACRUNNER_MR_RUN_PREFIX_TEMPLATE:-}"
  WINEPREFIX="$RUN_CONTRACT_PREFLIGHT_PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    MACRUNNER_RUN_CONTRACT_CALLER_EXPLICIT_SHA256="$RUN_CONTRACT_CALLER_EXPLICIT_SHA256" \
    python3 "$ROOT/tools/run_contract_identity_ledger.py" \
      --output "$RUN_CONTRACT_LEDGER_PATH" \
      --root "$ROOT" \
      --dist "$DIST" \
      --wine "$WINE" \
      --wineserver "$WSRV" \
      --exe "$EXE" \
      --exec-underscore-value "$WINE" \
      --timeout "$TMO" \
      --prefix "$RUN_CONTRACT_PREFLIGHT_PREFIX" \
      --prefix-template "$RUN_CONTRACT_PREFLIGHT_PREFIX" \
      --data-path "${MACRUNNER_RUN_CONTRACT_DATA_PATH:-}" \
      --save-path "${MACRUNNER_RUN_CONTRACT_SAVE_PATH:-}" \
      --config-path "${MACRUNNER_RUN_CONTRACT_CONFIG_PATH:-}" \
      --title "${MACRUNNER_RUN_CONTRACT_TITLE:-hollow-knight}" \
      --runner-file "$ROOT/scripts/mr-run.sh" \
      --runner-file "$ROOT/scripts/sync-prefix-from-dist.sh" \
      --branch "graphics=${MACRUNNER_GRAPHICS_BACKEND:-default}" \
      --branch "prefix_template=${RUN_CONTRACT_PREFLIGHT_PREFIX:-none}" \
      --branch "wineboot=preflight_only_suppressed" \
      --branch "system32_sync_arch=${MACRUNNER_PREFIX_SYSTEM32_ARCH:-not_applicable}" \
      --branch "com_rpcss_seed=preflight_only_suppressed" \
      --branch "actxprxy=${MACRUNNER_MR_RUN_ACTXPRXY:-not_requested}" \
      --branch "standalone_services_rpcss=preflight_only_suppressed" \
      --branch "vulkan_icd_wiring=${VK_ICD_FILENAMES:-none}" \
      --branch "backend=${MACRUNNER_HB_BACKEND:-not_requested}" \
      --branch "crt_case_fusion=${MACRUNNER_MR_RUN_CRT_CASE_FUSION:-not_requested}" \
      --branch "wwise_observer=${MACRUNNER_MR_RUN_WWISE_OBSERVER:-not_requested}" \
      --branch "stage_or_active_dist=$DIST" \
      --branch "x64_loader=${MACRUNNER_HB_X64_LOADER:-not_requested}" \
      -- "$@"
  RUN_CONTRACT_LEDGER_RC=$?
  if [ "$RUN_CONTRACT_LEDGER_RC" -eq 0 ]; then
    echo "[mr-run] run-contract-ledger=$RUN_CONTRACT_LEDGER_PATH" >&2
    echo "[mr-run] run-contract-ledger preflight-only exit before wine" >&2
    exit 0
  fi
  if [ "$RUN_CONTRACT_LEDGER_RC" -eq 2 ]; then
    echo "[mr-run] run-contract-ledger=$RUN_CONTRACT_LEDGER_PATH" >&2
    echo "[mr-run] run-contract-ledger status=BLOCKED exit before wine" >&2
    exit 2
  fi
  echo "[mr-run] run-contract ledger capture failed" >&2
  exit 125
fi

# /tmp rejects prefixes on this host (ownership) — use artifacts scratch.
# A/A diagnostics may pin the pathname as part of the child environment.  The
# caller must provide a fresh path below artifacts; cleanup remains scoped to it.
if [ -n "${MACRUNNER_MR_RUN_PREFIX_PATH:-}" ]; then
  case "$MACRUNNER_MR_RUN_PREFIX_PATH" in
    "$ROOT"/artifacts/_mr-run-aa-*) ;;
    *) echo "[mr-run] refusing fixed prefix outside artifacts/_mr-run-aa-*" >&2; exit 125 ;;
  esac
  if [ -e "$MACRUNNER_MR_RUN_PREFIX_PATH" ]; then
    echo "[mr-run] fixed prefix already exists: $MACRUNNER_MR_RUN_PREFIX_PATH" >&2
    exit 125
  fi
  mkdir -m 700 "$MACRUNNER_MR_RUN_PREFIX_PATH" || exit 125
  PREFIX="$MACRUNNER_MR_RUN_PREFIX_PATH"
else
  PREFIX="$(mktemp -d "$ROOT/artifacts/_mr-run.XXXXXX")"
fi

if [ "${MACRUNNER_FLIGHT_RECORDER:-0}" != "0" ]; then
  FLIGHT_PATH="${MACRUNNER_FLIGHT_RECORDER_PATH:-${MACRUNNER_FLIGHT_RECORDER_FILE:-${MACRUNNER_FLIGHT_PATH:-}}}"
  if [ -z "$FLIGHT_PATH" ]; then
    if [ -n "${MACRUNNER_RUN_DIR:-}" ]; then
      # TRIAGE-NEEDS fix: keep flight.jsonl inside the run dir so the analyzer gets deep data
      # (not the throwaway artifacts/flight/<ts>-<pid>/ dir that triage never sees).
      FLIGHT_PATH="$MACRUNNER_RUN_DIR/flight.jsonl"
    else
      FLIGHT_DIR="$ROOT/artifacts/flight/$(date +%Y%m%d-%H%M%S)-$$"
      FLIGHT_PATH="$FLIGHT_DIR/flight.jsonl"
    fi
  fi
  mkdir -p "$(dirname "$FLIGHT_PATH")"
  printf '{"schema":"macrunner-flight/v1","event":"mr-run-prelaunch","pid":%d,"epoch_s":%s}\n' \
    "$$" "$(date +%s)" >"$FLIGHT_PATH" || {
    echo "[mr-run] cannot seed mandatory flight recorder: $FLIGHT_PATH" >&2
    exit 125
  }
  export MACRUNNER_FLIGHT_RECORDER_PATH="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_RECORDER_FILE="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_PATH="$FLIGHT_PATH"
  echo "[mr-run] flight=$FLIGHT_PATH" >&2
fi

services_bg_pid=""
rpcss_bg_pid=""

# ── Title-slot mutex ───────────────────────────────────────────────────────────────────────
# MacRunner 2026-07-29: every launcher used to do `ps | grep; then launch`, which is
# check-then-act with a gap. On 2026-07-29 12:14 that gap put THREE Hollow Knight processes on
# one machine at once (11:41:23, 11:42:37, 12:10:13) — each stuck near 76 % CPU, starving the
# others, and the newest froze at +66 s having measured nothing. Two launchers can both see a
# free slot in the same second and both be right at the time they looked.
#
# `mkdir` is atomic on every filesystem we run on, so it is the lock. It lives here, in the one
# place every launcher funnels through, rather than in each wrapper — a lock only some callers
# take is not a lock.
#
# Stale locks are broken by PID liveness, not by age: a run legitimately lasts 45+ minutes, so
# any timeout long enough to be safe would be too long to be useful. Opt out with
# MACRUNNER_MR_RUN_NO_SLOT_LOCK=1 for a deliberately concurrent experiment.
MR_SLOT_LOCK="${TMPDIR:-/tmp}/macrunner-title-slot.lock"
MR_SLOT_LOCK_HELD=0

acquire_title_slot() {
  local waited=0 owner
  [ "${MACRUNNER_MR_RUN_NO_SLOT_LOCK:-0}" = "1" ] && return 0
  while :; do
    if mkdir "$MR_SLOT_LOCK" 2>/dev/null; then
      echo $$ > "$MR_SLOT_LOCK/pid"
      MR_SLOT_LOCK_HELD=1
      [ "$waited" -gt 0 ] && echo "[mr-run] title slot acquired after ${waited}s" >&2
      return 0
    fi
    owner="$(cat "$MR_SLOT_LOCK/pid" 2>/dev/null)"
    if [ -z "$owner" ] || ! kill -0 "$owner" 2>/dev/null; then
      echo "[mr-run] breaking stale title-slot lock (owner=${owner:-unknown} not alive)" >&2
      rm -rf "$MR_SLOT_LOCK" 2>/dev/null || true
      continue
    fi
    [ $((waited % 300)) -eq 0 ] && echo "[mr-run] waiting for title slot (owner=$owner, ${waited}s)" >&2
    sleep 15; waited=$((waited+15))
    if [ "$waited" -ge "${MACRUNNER_MR_RUN_SLOT_WAIT_MAX:-5400}" ]; then
      echo "[mr-run] giving up on the title slot after ${waited}s (owner=$owner)" >&2
      return 1
    fi
  done
}

release_title_slot() {
  [ "$MR_SLOT_LOCK_HELD" = "1" ] || return 0
  rm -rf "$MR_SLOT_LOCK" 2>/dev/null || true
  MR_SLOT_LOCK_HELD=0
}

cleanup() {
  release_title_slot
  [ -n "$services_bg_pid" ] && kill "$services_bg_pid" 2>/dev/null || true
  [ -n "$rpcss_bg_pid" ] && kill "$rpcss_bg_pid" 2>/dev/null || true
  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" != "1" ]; then
    WINEPREFIX="$PREFIX" DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      "$WSRV" -k >/dev/null 2>&1 || true
    pkill -f "$PREFIX" 2>/dev/null || true
  fi
  sleep 1
  if [ "${MACRUNNER_MR_RUN_KEEP_PREFIX:-0}" = "1" ]; then
    echo "[mr-run] keep-prefix=$PREFIX" >&2
  else
    rm -rf "$PREFIX" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# Taken AFTER the trap is installed so cleanup() always releases it, and before anything
# touches the prefix. A caller that cannot get the slot exits rather than piling on.
if ! acquire_title_slot; then
  echo "[mr-run] ABORT: another run holds the title slot" >&2
  exit 75
fi

find_warm_prefix_template() {
  local dir candidate best=""
  dir="$ROOT/artifacts/warm-prefix"
  [ -d "$dir" ] || return 1
  for candidate in "$dir"/*; do
    [ -d "$candidate" ] || continue
    [ -f "$candidate/.valid" ] || continue
    [ -f "$candidate/system.reg" ] || continue
    [ -f "$candidate/user.reg" ] || continue
    [ -f "$candidate/.update-timestamp" ] || continue
    if [ -z "$best" ] || [ "$candidate/.valid" -nt "$best/.valid" ]; then
      best="$candidate"
    fi
  done
  [ -n "$best" ] || return 1
  printf '%s\n' "$best"
}

copy_prefix_template() {
  local template="$1"
  [ -d "$template" ] || return 1
  if command -v ditto >/dev/null 2>&1; then
    ditto "$template" "$PREFIX"
  else
    (cd "$template" && tar cf - .) | (cd "$PREFIX" && tar xpf -)
  fi
}

stamp_prefix_wine_inf() {
  local inf="$DIST/share/wine/wine.inf"
  [ -f "$inf" ] || return 0
  python3 - "$inf" "$PREFIX/.update-timestamp" <<'PY'
import os
import sys

timestamp = int(os.stat(sys.argv[1]).st_mtime)
with open(sys.argv[2], "w", encoding="ascii") as f:
    f.write(f"{timestamp}\n")
PY
}

register_dxmt_windowscodecs() {
  local payload_arch="${MACRUNNER_PREFIX_SYSTEM32_ARCH:-x86_64-windows}"
  local regsvr32_exe="$DIST/lib/wine/aarch64-windows/regsvr32.exe"
  local windowscodecs_dll="C:\\windows\\system32\\windowscodecs.dll"
  local reg_tmo="${MACRUNNER_MR_RUN_REGSVR32_TIMEOUT:-30}"
  local reg_rc

  if [ ! -f "$PREFIX/drive_c/windows/system32/windowscodecs.dll" ]; then
    echo "[mr-run] dxmt regsvr32-windowscodecs blocked: System32 payload missing" >&2
    return 2
  fi
  if [ ! -f "$regsvr32_exe" ]; then
    regsvr32_exe="regsvr32.exe"
  fi

  echo "[mr-run] dxmt regsvr32-windowscodecs launcher_arch=aarch64-windows payload_arch=$payload_arch timeout=${reg_tmo}s dll=$windowscodecs_dll" >&2
  if command -v timeout >/dev/null 2>&1; then
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      timeout "$reg_tmo" "$WINE" "$regsvr32_exe" /s "$windowscodecs_dll" >&2
    reg_rc=$?
  elif command -v gtimeout >/dev/null 2>&1; then
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      gtimeout "$reg_tmo" "$WINE" "$regsvr32_exe" /s "$windowscodecs_dll" >&2
    reg_rc=$?
  else
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      "$WINE" "$regsvr32_exe" /s "$windowscodecs_dll" >&2
    reg_rc=$?
  fi
  if [ "$reg_rc" -ne 0 ]; then
    echo "[mr-run] dxmt regsvr32-windowscodecs failed exit=$reg_rc" >&2
    return "$reg_rc"
  fi
  echo "[mr-run] dxmt regsvr32-windowscodecs done" >&2
}

if [ "${MACRUNNER_GRAPHICS_BACKEND:-}" = "dxmt" ]; then
  DXMT_ROOT="${MACRUNNER_DXMT_ROOT:-$ROOT/engine/graphics/dist/dxmt}"
  SYSTEM32_ARCH="${MACRUNNER_PREFIX_SYSTEM32_ARCH:-x86_64-windows}"
  BOOT_TIMEOUT="${MACRUNNER_MR_RUN_BOOT_TIMEOUT:-45}"
  PREFIX_TEMPLATE="${MACRUNNER_MR_RUN_PREFIX_TEMPLATE:-}"

  if [ -z "$PREFIX_TEMPLATE" ] && [ "${MACRUNNER_MR_RUN_USE_WARM_PREFIX:-1}" = "1" ]; then
    PREFIX_TEMPLATE="$(find_warm_prefix_template || true)"
  fi

  if [ -n "$PREFIX_TEMPLATE" ]; then
    echo "[mr-run] graphics=dxmt seed-prefix template=$PREFIX_TEMPLATE" >&2
    if ! copy_prefix_template "$PREFIX_TEMPLATE"; then
      echo "[mr-run] dxmt prefix template copy failed: $PREFIX_TEMPLATE" >&2
      exit 2
    fi
    if [ "${MACRUNNER_MR_RUN_SKIP_WINEBOOT:-1}" = "1" ]; then
      if ! stamp_prefix_wine_inf; then
        echo "[mr-run] dxmt prefix wine.inf timestamp stamp failed" >&2
        exit 2
      fi
    fi
  else
    echo "[mr-run] graphics=dxmt seed-prefix template=none" >&2
  fi

  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ]; then
    echo "[mr-run] run-contract preflight skips dxmt wineboot" >&2
  elif [ "${MACRUNNER_MR_RUN_SKIP_WINEBOOT:-1}" = "1" ]; then
    echo "[mr-run] graphics=dxmt init-prefix skipped (MACRUNNER_MR_RUN_SKIP_WINEBOOT=1)" >&2
  else
    echo "[mr-run] graphics=dxmt init-prefix timeout=${BOOT_TIMEOUT}s" >&2
    if command -v timeout >/dev/null 2>&1; then
      WINEPREFIX="$PREFIX" WINEDEBUG="${MACRUNNER_MR_RUN_BOOT_WINEDEBUG:--all}" \
        DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
        DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
        timeout "$BOOT_TIMEOUT" "$WINE" wineboot -u >&2
      boot_rc=$?
    elif command -v gtimeout >/dev/null 2>&1; then
      WINEPREFIX="$PREFIX" WINEDEBUG="${MACRUNNER_MR_RUN_BOOT_WINEDEBUG:--all}" \
        DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
        DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
        gtimeout "$BOOT_TIMEOUT" "$WINE" wineboot -u >&2
      boot_rc=$?
    else
      WINEPREFIX="$PREFIX" WINEDEBUG="${MACRUNNER_MR_RUN_BOOT_WINEDEBUG:--all}" \
        DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
        DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
        "$WINE" wineboot -u >&2 &
      boot_child=$!
      boot_rc=124
      boot_elapsed=0
      while kill -0 "$boot_child" 2>/dev/null; do
        if [ "$boot_elapsed" -ge "$BOOT_TIMEOUT" ]; then
          kill "$boot_child" 2>/dev/null || true
          sleep 1
          kill -9 "$boot_child" 2>/dev/null || true
          wait "$boot_child" 2>/dev/null || true
          break
        fi
        sleep 1
        boot_elapsed=$((boot_elapsed + 1))
      done
      if ! kill -0 "$boot_child" 2>/dev/null; then
        wait "$boot_child" 2>/dev/null || true
        boot_rc=$?
      fi
    fi
    if [ "$boot_rc" -ne 0 ]; then
      echo "[mr-run] dxmt prefix init failed exit=$boot_rc" >&2
      exit "$boot_rc"
    fi
  fi

  echo "[mr-run] graphics=dxmt sync-prefix system32_arch=$SYSTEM32_ARCH" >&2
  if ! "$ROOT/scripts/sync-prefix-from-dist.sh" --dist "$DIST" --prefix "$PREFIX" --system32-arch "$SYSTEM32_ARCH" >&2; then
    echo "[mr-run] dxmt prefix sync failed" >&2
    exit 2
  fi
  # Empty DXMT throwaway prefixes skip wineboot by default.  wine.inf points TEMP/TMP
  # at %SystemRoot%\\temp, and Unity aborts cursor setup before creating a window if
  # C:\\windows\\temp is absent.
  mkdir -p "$PREFIX/drive_c/windows/temp" "$PREFIX/drive_c/windows/Temp"
  mkdir -p "$PREFIX/dosdevices"
  ln -sfn ../drive_c "$PREFIX/dosdevices/c:"
  ln -sfn / "$PREFIX/dosdevices/z:"

  if [ -f "$DXMT_ROOT/$SYSTEM32_ARCH/d3d9.dll" ]; then
    cp -f "$DXMT_ROOT/$SYSTEM32_ARCH/d3d9.dll" "$PREFIX/drive_c/windows/system32/d3d9.dll"
    echo "[mr-run] graphics=dxmt overlay-d3d9 <= $DXMT_ROOT/$SYSTEM32_ARCH/d3d9.dll" >&2
  fi

  if [ -d "$DXMT_ROOT" ]; then
    export MACRUNNER_DXMT_ROOT="$DXMT_ROOT"
    if [ -n "${WINEDLLPATH:-}" ]; then
      export WINEDLLPATH="$DXMT_ROOT:$WINEDLLPATH"
    else
      export WINEDLLPATH="$DXMT_ROOT"
    fi
  fi
  export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d11,dxgi,d3d10core,winemetal=n,b}"
  # Route DXMT file logging to run dir so ERR/WARN are visible when WINEDEBUG=-all silences __wine_dbg_output.
  if [ -z "${DXMT_LOG_PATH:-}" ]; then
    export DXMT_LOG_PATH="${MACRUNNER_RUN_DIR:-$PREFIX}"
  fi
  export DXMT_LOG_LEVEL="${DXMT_LOG_LEVEL:-info}"

  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" != "1" ] && \
     [ "${MACRUNNER_MR_RUN_REGSVR32_WINCODECS:-0}" = "1" ]; then
    if ! register_dxmt_windowscodecs; then
      echo "[mr-run] dxmt windowscodecs regsvr32 failed" >&2
      exit 2
    fi
  fi

fi

# Non-DXMT prefix seeding from a pre-warmed template (e.g. bottles/generic-x86).
# Faster than wineboot -u; provides registry so rpcss/services.exe work → fixes 0x6ba.
# Opt-in: MACRUNNER_MR_RUN_PREFIX_TEMPLATE=<path> (path to a valid WINEPREFIX dir).
if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" != "1" ] && \
   [ "${MACRUNNER_GRAPHICS_BACKEND:-}" != "dxmt" ] && \
   [ -n "${MACRUNNER_MR_RUN_PREFIX_TEMPLATE:-}" ]; then
  echo "[mr-run] seed-prefix template=$MACRUNNER_MR_RUN_PREFIX_TEMPLATE" >&2
  if ! copy_prefix_template "$MACRUNNER_MR_RUN_PREFIX_TEMPLATE"; then
    echo "[mr-run] prefix template copy failed: $MACRUNNER_MR_RUN_PREFIX_TEMPLATE" >&2
    exit 2
  fi
  stamp_prefix_wine_inf || true
  echo "[mr-run] seed-prefix done" >&2
fi

# Non-DXMT prefix seeding: run wineboot -u so the registry exists before
# services.exe starts (needed for rpcss registration → fixes 0x6ba).
# Opt-in via MACRUNNER_MR_RUN_WINEBOOT=1 (or auto-enabled when START_SERVICES=1
# and no stamped template is pre-warmed with the CURRENT wine build).
# Opt-out via MACRUNNER_MR_RUN_WINEBOOT=0 to keep the empty-prefix fast path.
if [ "${MACRUNNER_GRAPHICS_BACKEND:-}" != "dxmt" ] && \
   [ "${MACRUNNER_MR_RUN_WINEBOOT:-0}" = "1" ]; then
  _boot_tmo="${MACRUNNER_MR_RUN_BOOT_TIMEOUT:-45}"
  echo "[mr-run] seed-prefix wineboot timeout=${_boot_tmo}s" >&2
  if command -v timeout >/dev/null 2>&1; then
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      timeout "$_boot_tmo" "$WINE" wineboot -u >&2 || true
  elif command -v gtimeout >/dev/null 2>&1; then
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      gtimeout "$_boot_tmo" "$WINE" wineboot -u >&2 || true
  else
    WINEPREFIX="$PREFIX" WINEDEBUG=-all \
      DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
      DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
      "$WINE" wineboot -u >&2 &
    _boot_child=$!
    _boot_rc=0
    _boot_elapsed=0
    while kill -0 "$_boot_child" 2>/dev/null; do
      if [ "$_boot_elapsed" -ge "$_boot_tmo" ]; then
        kill "$_boot_child" 2>/dev/null || true
        sleep 1
        kill -9 "$_boot_child" 2>/dev/null || true
        wait "$_boot_child" 2>/dev/null || true
        break
      fi
      sleep 1
      _boot_elapsed=$((_boot_elapsed + 1))
    done
    if ! kill -0 "$_boot_child" 2>/dev/null; then
      wait "$_boot_child" 2>/dev/null || true
    fi
  fi
  echo "[mr-run] seed-prefix wineboot done" >&2
fi

# Start services.exe (Wine SCM) → auto-starts rpcss.exe → creates \pipe\epmapper.
# Required for COM/RPC calls (CoInitialize, RPC) for any game that hits 0x6ba.
# Opt-in: set MACRUNNER_MR_RUN_START_SERVICES=1 (works for dxmt, PE32, HK — any dist).
if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" != "1" ] && \
   [ "${MACRUNNER_MR_RUN_START_SERVICES:-0}" = "1" ]; then
  echo "[mr-run] starting services.exe + rpcss.exe (standalone) → epmapper + ncalrpc:[irpcss]" >&2
  WINEPREFIX="$PREFIX" WINEDEBUG=-all \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    "$WINE" "C:\\windows\\system32\\services.exe" >/dev/null 2>&1 &
  services_bg_pid=$!
  # Launch rpcss.exe directly.  On the dxmt / skip-wineboot fast path the SCM never
  # demand-starts RpcSs (StartType=3) and `sc start` is not usable, so the OLE SCM endpoint
  # ncalrpc:[irpcss] would never be created -> the guest's first CoInitialize bind hits
  # \\.\pipe\lrpc\irpcss with error=2 -> unhandled 0x6ba RPC_S_SERVER_UNAVAILABLE that kills
  # the OLE init threads.  rpcss.exe carries a MacRunner standalone fallback
  # (programs/rpcss/rpcss_main.c): when raw-exec'd its StartServiceCtrlDispatcherW returns
  # FAILED_SERVICE_CONTROLLER_CONNECT and it then runs RPCSS_Initialize() itself, serving
  # the epmapper + irpcss endpoints for its lifetime.
  # Launch by FULL Windows path, not the bare name: `wine rpcss.exe` resolves the bare
  # name against the cwd (Z:\...) which has no rpcss.exe, so wine falls back to start.exe
  # which PATH-searches (rpcss.exe.com/.exe/.bat/...) and fails on the unprovisioned
  # fast-path prefix ("Environment variable not found / ShellExecuteEx failed"), never
  # reaching the real builtin in system32.  The explicit C:\ path loads it directly.
  WINEPREFIX="$PREFIX" WINEDEBUG="${MACRUNNER_MR_RUN_RPCSS_DEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    "$WINE" "C:\\windows\\system32\\rpcss.exe" >"${MACRUNNER_MR_RUN_RPCSS_LOG:-/dev/null}" 2>&1 &
  rpcss_bg_pid=$!
  # rpcss creates its ncalrpc endpoints within ~1-2s; the guest binds irpcss much later
  # (deep into engine init).  Bounded wait so the endpoint is up before any early client.
  sleep "${MACRUNNER_MR_RUN_SERVICES_WAIT:-6}"
  echo "[mr-run] services.exe pid=$services_bg_pid rpcss.exe pid=$rpcss_bg_pid started" >&2
fi

if [ -n "$RUN_CONTRACT_LEDGER_PATH" ] || [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ]; then
  if [ -z "$RUN_CONTRACT_LEDGER_PATH" ]; then
    echo "[mr-run] run-contract ledger path required when preflight-only is enabled" >&2
    exit 125
  fi
  mkdir -p "$(dirname "$RUN_CONTRACT_LEDGER_PATH")"
  RUN_CONTRACT_PREFIX_TEMPLATE="${PREFIX_TEMPLATE:-${MACRUNNER_MR_RUN_PREFIX_TEMPLATE:-}}"
  RUN_CONTRACT_WINEBOOT_BRANCH="non_dxmt_wineboot=${MACRUNNER_MR_RUN_WINEBOOT:-0}"
  RUN_CONTRACT_SERVICES_BRANCH="start_services=${MACRUNNER_MR_RUN_START_SERVICES:-0}"
  RUN_CONTRACT_SYSTEM32_ARCH="${SYSTEM32_ARCH:-not_applicable}"
  if [ "${MACRUNNER_GRAPHICS_BACKEND:-}" = "dxmt" ]; then
    RUN_CONTRACT_WINEBOOT_BRANCH="dxmt_skip_wineboot=${MACRUNNER_MR_RUN_SKIP_WINEBOOT:-1}"
    RUN_CONTRACT_SYSTEM32_ARCH="${SYSTEM32_ARCH:-${MACRUNNER_PREFIX_SYSTEM32_ARCH:-x86_64-windows}}"
  fi
  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ]; then
    RUN_CONTRACT_WINEBOOT_BRANCH="preflight_only_suppressed"
    if [ "${MACRUNNER_MR_RUN_START_SERVICES:-0}" = "1" ]; then
      RUN_CONTRACT_SERVICES_BRANCH="preflight_only_suppressed"
    fi
  fi
  WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    MACRUNNER_RUN_CONTRACT_CALLER_EXPLICIT_SHA256="$RUN_CONTRACT_CALLER_EXPLICIT_SHA256" \
    python3 "$ROOT/tools/run_contract_identity_ledger.py" \
      --output "$RUN_CONTRACT_LEDGER_PATH" \
      --root "$ROOT" \
      --dist "$DIST" \
      --wine "$WINE" \
      --wineserver "$WSRV" \
      --exe "$EXE" \
      --exec-underscore-value "$WINE" \
      --timeout "$TMO" \
      --prefix "$PREFIX" \
      --prefix-template "$RUN_CONTRACT_PREFIX_TEMPLATE" \
      --data-path "${MACRUNNER_RUN_CONTRACT_DATA_PATH:-}" \
      --save-path "${MACRUNNER_RUN_CONTRACT_SAVE_PATH:-}" \
      --config-path "${MACRUNNER_RUN_CONTRACT_CONFIG_PATH:-}" \
      --title "${MACRUNNER_RUN_CONTRACT_TITLE:-hollow-knight}" \
      --runner-file "$ROOT/scripts/mr-run.sh" \
      --runner-file "$ROOT/scripts/sync-prefix-from-dist.sh" \
      --branch "graphics=${MACRUNNER_GRAPHICS_BACKEND:-default}" \
      --branch "prefix_template=${RUN_CONTRACT_PREFIX_TEMPLATE:-none}" \
      --branch "wineboot=$RUN_CONTRACT_WINEBOOT_BRANCH" \
      --branch "system32_sync_arch=$RUN_CONTRACT_SYSTEM32_ARCH" \
      --branch "com_rpcss_seed=$RUN_CONTRACT_SERVICES_BRANCH" \
      --branch "actxprxy=${MACRUNNER_MR_RUN_ACTXPRXY:-}" \
      --branch "standalone_services_rpcss=$RUN_CONTRACT_SERVICES_BRANCH" \
      --branch "vulkan_icd_wiring=${VK_ICD_FILENAMES:-none}" \
      --branch "backend=${MACRUNNER_HB_BACKEND:-}" \
      --branch "crt_case_fusion=${MACRUNNER_MR_RUN_CRT_CASE_FUSION:-}" \
      --branch "wwise_observer=${MACRUNNER_MR_RUN_WWISE_OBSERVER:-}" \
      --branch "stage_or_active_dist=$DIST" \
      --branch "x64_loader=${MACRUNNER_HB_X64_LOADER:-}" \
      -- "$@"
  RUN_CONTRACT_LEDGER_RC=$?
  if [ "$RUN_CONTRACT_LEDGER_RC" -ne 0 ] && [ "$RUN_CONTRACT_LEDGER_RC" -ne 2 ]; then
    echo "[mr-run] run-contract ledger capture failed" >&2
    exit 125
  fi
  echo "[mr-run] run-contract-ledger=$RUN_CONTRACT_LEDGER_PATH" >&2
  if [ "$RUN_CONTRACT_LEDGER_RC" -eq 2 ]; then
    echo "[mr-run] run-contract-ledger status=BLOCKED exit before wine" >&2
    exit 2
  fi
  if [ "$RUN_CONTRACT_PREFLIGHT_ONLY" = "1" ]; then
    echo "[mr-run] run-contract-ledger preflight-only exit before wine" >&2
    exit 0
  fi
fi

unset MACRUNNER_RUN_CONTRACT_CONFIG_PATH \
  MACRUNNER_RUN_CONTRACT_DATA_PATH \
  MACRUNNER_RUN_CONTRACT_LEDGER \
  MACRUNNER_RUN_CONTRACT_LEDGER_PATH \
  MACRUNNER_RUN_CONTRACT_LEDGER_PREFLIGHT_ONLY \
  MACRUNNER_RUN_CONTRACT_SAVE_PATH \
  MACRUNNER_RUN_CONTRACT_TITLE \
  MACRUNNER_RUN_CONTRACT_TITLE_DATA_PATH

# Launch Wine directly.  A shell/timeout intermediary both hides the exact Wine
# PID and, on macOS, strips DYLD_* while rewriting `_`, invalidating the sealed
# final-child environment.  The bounded wait below owns timeout enforcement.
FINAL_CHILD_DISPATCH="direct"
FINAL_CHILD_DISPATCH_COMMAND="$WINE"

if [ "$FINAL_CHILD_CAPTURE_ENABLED" = "1" ]; then
  WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    python3 "$ROOT/tools/run_contract_final_child_capture.py" \
      --output "$FINAL_CHILD_CAPTURE_PATH" \
      --exec-underscore-value "$FINAL_CHILD_DISPATCH_COMMAND" \
      --environment-from-process \
      -- "$WINE" "$EXE" "$@"
  FINAL_CHILD_CAPTURE_STATUS=$?
  if [ "$FINAL_CHILD_CAPTURE_STATUS" -ne 0 ]; then
    echo "[mr-run] final child capture failed; exit before wine" >&2
    exit 125
  fi
  echo "[mr-run] final-child-capture=$FINAL_CHILD_CAPTURE_PATH" >&2
fi

echo "[mr-run] prefix=$PREFIX timeout=${TMO}s exe=$EXE" >&2
# MacRunner HK lane (2026-07-27): dyld strips DYLD_INSERT_LIBRARIES from the env of
# every intermediate exec, so passing it from the caller never reaches the game.
# Set it fresh here when the lane requests an in-process dylib (e.g. NSEvent
# auto-injection for the HK language picker).
if [ -n "${MACRUNNER_MR_RUN_DYLD_INSERT:-}" ]; then
  echo "[mr-run] dyld-insert=$MACRUNNER_MR_RUN_DYLD_INSERT" >&2
fi
WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
  DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
  DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
  env ${MACRUNNER_MR_RUN_DYLD_INSERT:+DYLD_INSERT_LIBRARIES="$MACRUNNER_MR_RUN_DYLD_INSERT"} \
  "$WINE" "$EXE" "$@" &
child=$!
if [ -n "$WINE_CHILD_PID_PATH" ]; then
  pid_tmp="${WINE_CHILD_PID_PATH}.tmp.$$"
  if ! printf '%s\n' "$child" >"$pid_tmp" || ! mv "$pid_tmp" "$WINE_CHILD_PID_PATH"; then
    rm -f "$pid_tmp"
    kill "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
    echo "[mr-run] exact Wine child PID capture failed" >&2
    exit 125
  fi
fi

rc=124
elapsed=0
timed_out=0
while kill -0 "$child" 2>/dev/null; do
  if [ "$elapsed" -ge "$TMO" ]; then
    timed_out=1
    kill "$child" 2>/dev/null || true
    sleep 1
    kill -9 "$child" 2>/dev/null || true
    wait "$child" 2>/dev/null || true
    break
  fi
  sleep 1
  elapsed=$((elapsed + 1))
done
if [ "$timed_out" = "0" ]; then
  wait "$child"
  rc=$?
fi
echo "[mr-run] exit=$rc (prefix auto-removed on exit)" >&2

if [ -n "${FLIGHT_PATH:-}" ]; then
  printf '{"schema":"macrunner-flight/v1","event":"mr-run-exit","status":%d,"epoch_s":%s}\n' \
    "$rc" "$(date +%s)" >>"$FLIGHT_PATH" || rc=125
fi

if [ "$RUN_ARTIFACTS_REQUIRED" = "1" ]; then
  RUN_ARTIFACT_FAILURE=0
  for required in "$RUN_ARTIFACT_DIR/run-contract.json" "$RUN_ARTIFACT_DIR/final-child.json" \
                  "$RUN_ARTIFACT_DIR/flight.jsonl" "$WINE_CHILD_PID_PATH"; do
    if [ ! -s "$required" ]; then
      echo "[mr-run] mandatory run artifact missing or empty: $required" >&2
      RUN_ARTIFACT_FAILURE=1
    fi
  done
  if [ -s "$WINE_CHILD_PID_PATH" ] && ! grep -Eq '^[0-9]+$' "$WINE_CHILD_PID_PATH"; then
    echo "[mr-run] malformed Wine child PID: $WINE_CHILD_PID_PATH" >&2
    RUN_ARTIFACT_FAILURE=1
  fi
  if [ "$RUN_ARTIFACT_FAILURE" = "1" ]; then
    rc=125
  fi
fi

# Auto-triage: when the caller exports MACRUNNER_RUN_DIR (the dir it captured run.log into), classify
# the run so EVERY lane gets a triage-summary (OWNER/CLASS/CONFIDENCE/NEXT_ACTION) without invoking
# the analyzer by hand. Opt-out with MACRUNNER_NO_AUTOTRIAGE=1. Never affects the run's exit code.
if [ -n "${MACRUNNER_RUN_DIR:-}" ] && [ "${MACRUNNER_NO_AUTOTRIAGE:-0}" = "0" ] \
   && [ -f "$ROOT/tools/triage/classify_run.py" ]; then
  python3 "$ROOT/tools/triage/classify_run.py" "$MACRUNNER_RUN_DIR" >/dev/null 2>&1 || true
  [ -f "$MACRUNNER_RUN_DIR/triage-summary.txt" ] && \
    echo "[mr-run] triage -> $MACRUNNER_RUN_DIR/triage-summary.txt" >&2
fi
exit $rc
