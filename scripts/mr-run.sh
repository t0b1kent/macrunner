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
DIST="${1:?need dist dir}"; EXE="${2:?need exe path}"; TMO="${3:-120}"
shift 3 2>/dev/null || shift $#
[ "${1:-}" = "--" ] && shift

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/config/env.sh"
# /tmp rejects prefixes on this host (ownership) — use artifacts scratch.
PREFIX="$(mktemp -d "$ROOT/artifacts/_mr-run.XXXXXX")"
WINE="$DIST/bin/wine"
WSRV="$DIST/bin/wineserver"
WINE_UNIX_LIB="$DIST/lib/wine/aarch64-unix"
RUN_DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-}"
RUN_DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-}"

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
  mkdir -p "$MACRUNNER_HB_TRANSLATION_CACHE_ROOT"
  export WINELOADERNOEXEC="${WINELOADERNOEXEC:-1}"
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
  export MACRUNNER_FLIGHT_RECORDER_PATH="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_RECORDER_FILE="$FLIGHT_PATH"
  export MACRUNNER_FLIGHT_PATH="$FLIGHT_PATH"
  echo "[mr-run] flight=$FLIGHT_PATH" >&2
fi

services_bg_pid=""
rpcss_bg_pid=""

cleanup() {
  [ -n "$services_bg_pid" ] && kill "$services_bg_pid" 2>/dev/null || true
  [ -n "$rpcss_bg_pid" ] && kill "$rpcss_bg_pid" 2>/dev/null || true
  WINEPREFIX="$PREFIX" DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    "$WSRV" -k >/dev/null 2>&1 || true
  pkill -f "$PREFIX" 2>/dev/null || true
  sleep 1
  if [ "${MACRUNNER_MR_RUN_KEEP_PREFIX:-0}" = "1" ]; then
    echo "[mr-run] keep-prefix=$PREFIX" >&2
  else
    rm -rf "$PREFIX" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

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

  if [ "${MACRUNNER_MR_RUN_SKIP_WINEBOOT:-1}" = "1" ]; then
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

fi

# Non-DXMT prefix seeding from a pre-warmed template (e.g. bottles/generic-x86).
# Faster than wineboot -u; provides registry so rpcss/services.exe work → fixes 0x6ba.
# Opt-in: MACRUNNER_MR_RUN_PREFIX_TEMPLATE=<path> (path to a valid WINEPREFIX dir).
if [ "${MACRUNNER_GRAPHICS_BACKEND:-}" != "dxmt" ] && \
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
if [ "${MACRUNNER_MR_RUN_START_SERVICES:-0}" = "1" ]; then
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

echo "[mr-run] prefix=$PREFIX timeout=${TMO}s exe=$EXE" >&2
if command -v timeout >/dev/null 2>&1; then
  WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    timeout "$TMO" "$WINE" "$EXE" "$@"
  rc=$?
elif command -v gtimeout >/dev/null 2>&1; then
  WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    gtimeout "$TMO" "$WINE" "$EXE" "$@"
  rc=$?
else
  WINEPREFIX="$PREFIX" WINEDEBUG="${WINEDEBUG:--all}" \
    DYLD_LIBRARY_PATH="$RUN_DYLD_LIBRARY_PATH" \
    DYLD_FALLBACK_LIBRARY_PATH="$RUN_DYLD_FALLBACK_LIBRARY_PATH" \
    "$WINE" "$EXE" "$@" &
  child=$!
  rc=124
  elapsed=0
  while kill -0 "$child" 2>/dev/null; do
    if [ "$elapsed" -ge "$TMO" ]; then
      kill "$child" 2>/dev/null || true
      sleep 1
      kill -9 "$child" 2>/dev/null || true
      wait "$child" 2>/dev/null || true
      break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  if ! kill -0 "$child" 2>/dev/null; then
    wait "$child"
    rc=$?
  fi
fi
echo "[mr-run] exit=$rc (prefix auto-removed on exit)" >&2

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
