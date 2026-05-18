#!/usr/bin/env bash
# Canonical Notepad++ x64 launcher via HyperBridge.
# Encapsulates cwd dependency, FreeType path, prefix, env vars.
# Use this from any pwd — never invoke notepad++.exe directly.
#
# Usage:
#   ./scripts/run-notepad-x64.sh                          # bounded 18s held run, then cleanup
#   ./scripts/run-notepad-x64.sh --hold                   # hold alive until you Ctrl-C
#   ./scripts/run-notepad-x64.sh --trace=fileinfo,abi     # add MACRUNNER_HB_TRACE_* knobs
#   MACRUNNER_NPP_PREFIX=/some/other/prefix ./scripts/run-notepad-x64.sh
set -euo pipefail

# --- Anchored paths (do not edit without updating AGENTS.md) ---
ROOT="${MACRUNNER_ROOT:-/Volumes/MacOS/MacRunner}"
APPDIR="${MACRUNNER_NPP_X64_DIR:-$ROOT/artifacts/phase-h/npp-x64}"
APP="$APPDIR/notepad++.exe"
WINE_DIST="${MACRUNNER_WINE_DIST_ARM64:-$ROOT/engine/wine/dist-pure-arm64}"
WINE="$WINE_DIST/bin/wine"
WINESERVER="$WINE_DIST/bin/wineserver"
PREFIX="${MACRUNNER_NPP_PREFIX:-$ROOT/artifacts/phase-h/prefix-npp-x64-current}"
REPAIR_SCRIPT="$ROOT/scripts/repair-wine-window-metrics.sh"

# --- Sanity checks (fail loudly with hints) ---
[[ -f "$APP"  ]] || { echo "ERROR: notepad++.exe not found at $APP" >&2; echo "  Did the NSIS payload get extracted to artifacts/phase-h/npp-x64/?" >&2; exit 2; }
[[ -x "$WINE" ]] || { echo "ERROR: wine not found at $WINE" >&2; echo "  Build pure-arm64 wine: ./scripts/build-wine-pure-arm64-experiment.sh" >&2; exit 2; }

# --- Arg parsing ---
HOLD=0
HOLD_SECS=18
TRACES=""
EXTRA_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --hold)              HOLD=1 ;;
        --hold=*)            HOLD=1; HOLD_SECS="${arg#--hold=}" ;;
        --trace=*)           TRACES="${arg#--trace=}" ;;
        --prefix=*)          PREFIX="${arg#--prefix=}" ;;
        --help|-h)
            sed -n '1,12p' "$0"; exit 0 ;;
        *)                   EXTRA_ARGS+=("$arg") ;;
    esac
done

# --- Cleanup any prior Wine tails (mandatory hygiene) ---
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver' 2>/dev/null || true
sleep 1

# --- Env setup ---
mkdir -p "$PREFIX"
export WINEPREFIX="$PREFIX"
export MACRUNNER_WINE_DIST="$WINE_DIST"
export MACRUNNER_HB_X64_LOADER=1
# FreeType lives in Homebrew but our Wine is built without explicit rpath.
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
export WINEDEBUG="${WINEDEBUG:--all}"

# --- Optional traces ---
IFS=',' read -ra TRACE_LIST <<< "$TRACES"
for t in "${TRACE_LIST[@]}"; do
    case "$t" in
        fileinfo)    export MACRUNNER_TRACE_FILEINFO=1 ;;
        abi)         export MACRUNNER_HB_TRACE_ABI=1; export MACRUNNER_HB_TRACE_ABI_BUDGET="${MACRUNNER_HB_TRACE_ABI_BUDGET:-500}" ;;
        file_api)    export MACRUNNER_HB_TRACE_FILE_API=1; export MACRUNNER_HB_TRACE_FILE_API_BUDGET="${MACRUNNER_HB_TRACE_FILE_API_BUDGET:-500}" ;;
        geometry)    export MACRUNNER_HB_TRACE_GEOMETRY=1 ;;
        faults)      export MACRUNNER_HB_TRACE_FAULTS=1 ;;
        '')          ;;
        *)           echo "WARN: unknown trace '$t' (known: fileinfo,abi,file_api,geometry,faults)" >&2 ;;
    esac
done

# --- Prepare prefix (wineboot + window-metrics repair) ---
RUN_DIR="$ROOT/reports/phase-h/npp-x64-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN_DIR"
echo "RUN=$RUN_DIR"
echo "PREFIX=$PREFIX"
echo "WINE=$WINE"
echo "APP=$APP"

"$WINE" wineboot --init   >/dev/null 2>"$RUN_DIR/wineboot-init.err"   || true
"$WINE" wineboot --update >/dev/null 2>"$RUN_DIR/wineboot-update.err" || true
"$WINESERVER" -w >/dev/null 2>&1 || true
[[ -x "$REPAIR_SCRIPT" ]] && "$REPAIR_SCRIPT" --prefix "$PREFIX" --fix >"$RUN_DIR/metrics-repair.log" 2>&1 || true

# --- Launch (cwd MUST be APPDIR — notepad++ looks for langs.xml etc. in cwd) ---
cd "$APPDIR"
(
    if ((${#EXTRA_ARGS[@]})); then
        "$WINE" ./notepad++.exe -noPlugin -nosession "${EXTRA_ARGS[@]}" \
            >"$RUN_DIR/stdout.log" 2>"$RUN_DIR/stderr.log"
    else
        "$WINE" ./notepad++.exe -noPlugin -nosession \
            >"$RUN_DIR/stdout.log" 2>"$RUN_DIR/stderr.log"
    fi
    echo $? >"$RUN_DIR/exit.code"
) &
WPID=$!
echo "$WPID" >"$RUN_DIR/wine.pid"
echo "WPID=$WPID"

if [[ "$HOLD" == 1 ]]; then
    echo "Holding alive (Ctrl-C to release). Logs: $RUN_DIR/"
    wait "$WPID" || true
    exit 0
fi

sleep "$HOLD_SECS"
ALIVE=0; kill -0 "$WPID" 2>/dev/null && ALIVE=1
echo "alive_after_${HOLD_SECS}s=$ALIVE"
echo "stderr_bytes=$(wc -c <"$RUN_DIR/stderr.log" | tr -d ' ')"
echo "---key lines---"
grep -E 'macrunner-(hb-|kernelbase-|fileinfo)|Load langs|UNSUPPORTED|fault|wineserver crashed|err:' "$RUN_DIR/stderr.log" 2>/dev/null | tail -40 || true

# Cleanup unless alive AND user passed --hold (already handled above).
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver' 2>/dev/null || true
