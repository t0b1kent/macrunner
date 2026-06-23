#!/usr/bin/env bash
# Canonical Notepad++ x64 launcher via HyperBridge.
# Encapsulates cwd dependency, FreeType path, prefix, env vars.
# Use this from any pwd — never invoke notepad++.exe directly.
#
# Usage:
#   ./scripts/run-notepad-x64.sh                          # bounded 18s held run, then cleanup
#   ./scripts/run-notepad-x64.sh --duration=45            # bounded 45s run, then cleanup
#   ./scripts/run-notepad-x64.sh --hold                   # hold alive until you Ctrl-C
#   ./scripts/run-notepad-x64.sh --trace=fileinfo,abi     # add MACRUNNER_HB_TRACE_* knobs
#   MACRUNNER_NPP_PREFIX=/some/other/prefix ./scripts/run-notepad-x64.sh
#   MACRUNNER_NPP_AFTER_LAUNCH_CMD='...' ./scripts/run-notepad-x64.sh --duration=0
set -euo pipefail

# --- Anchored paths (do not edit without updating AGENTS.md) ---
ROOT="${MACRUNNER_ROOT:-/Users/timurtoby/Documents/MacRunner/Main/MacRunner}"
APPDIR="${MACRUNNER_NPP_X64_DIR:-$ROOT/artifacts/phase-h/npp-x64}"
APP="$APPDIR/notepad++.exe"
WINE_DIST="${MACRUNNER_WINE_DIST_ARM64:-$ROOT/engine/wine/dist-pure-arm64}"
WINE="$WINE_DIST/bin/wine"
WINESERVER="$WINE_DIST/bin/wineserver"
PREFIX="${MACRUNNER_NPP_PREFIX:-$ROOT/artifacts/phase-h/prefix-npp-x64-current}"
REPAIR_SCRIPT="$ROOT/scripts/repair-wine-window-metrics.sh"
READINESS_GATE="$ROOT/scripts/npp-readiness-gate.sh"
PREFIX_SYNC_SCRIPT="$ROOT/scripts/sync-prefix-from-dist.sh"
FRESHNESS_SCRIPT="$ROOT/scripts/verify-build-freshness.sh"

# --- Sanity checks (fail loudly with hints) ---
[[ -f "$APP"  ]] || { echo "ERROR: notepad++.exe not found at $APP" >&2; echo "  Did the NSIS payload get extracted to artifacts/phase-h/npp-x64/?" >&2; exit 2; }
[[ -x "$WINE" ]] || { echo "ERROR: wine not found at $WINE" >&2; echo "  Build pure-arm64 wine: ./scripts/build-wine-pure-arm64-experiment.sh" >&2; exit 2; }

# --- Arg parsing ---
HOLD=0
HOLD_SECS=18
TRACES=""
EXTRA_ARGS=()
AFTER_LAUNCH_CMD="${MACRUNNER_NPP_AFTER_LAUNCH_CMD:-}"
unset MACRUNNER_NPP_AFTER_LAUNCH_CMD
for arg in "$@"; do
    case "$arg" in
        --hold)              HOLD=1 ;;
        --hold=*)            HOLD=1; HOLD_SECS="${arg#--hold=}" ;;
        --duration=*)        HOLD_SECS="${arg#--duration=}" ;;
        --trace=*)           TRACES="${arg#--trace=}" ;;
        --prefix=*)          PREFIX="${arg#--prefix=}" ;;
        --help|-h)
            sed -n '1,12p' "$0"; exit 0 ;;
        *)                   EXTRA_ARGS+=("$arg") ;;
    esac
done

export TMPDIR="${MACRUNNER_WINE_TMPDIR:-/private/tmp/macr-wine-$(id -u)}"
mkdir -p "$TMPDIR"

RUN_DIR="$ROOT/reports/phase-h/npp-x64-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN_DIR"
echo "RUN=$RUN_DIR"
echo "PREFIX=$PREFIX"
echo "WINE=$WINE"
echo "APP=$APP"

# --- Telemetry helpers ---
now_ns() {
    python3 -c 'import time; print(time.monotonic_ns())'
}

record_phase() {
    local name="$1"
    local start_ns="$2"
    local end_ns="$3"
    local elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
    echo "${name}_ms=${elapsed_ms}" >> "$RUN_DIR/phases.log"
}

TOTAL_START_NS=$(now_ns)

remove_stale_wineserver_roots() {
    local uid root
    uid="$(id -u)"
    for root in "$TMPDIR/.wine-$uid" "/tmp/.wine-$uid" "/private/tmp/.wine-$uid"; do
        [[ -e "$root" ]] || continue
        rm -rf "$root" 2>/dev/null || true
    done
}

unix_socket_bind_preflight() {
    local log_file="$1"
    local python_bin="${PYTHON:-/usr/bin/python3}"

    if [[ ! -x "$python_bin" ]]; then
        python_bin="$(command -v python3 || true)"
    fi
    if [[ -z "$python_bin" ]]; then
        echo "unix_socket_bind_preflight=SKIP reason=python3_missing" >>"$log_file"
        return 0
    fi

    "$python_bin" - "$TMPDIR" "$log_file" <<'PY'
import os
import socket
import sys

tmpdir, log_path = sys.argv[1], sys.argv[2]
os.makedirs(tmpdir, exist_ok=True)
sock_path = os.path.join(tmpdir, "macrunner-wineserver-preflight-%d.sock" % os.getpid())

def log(line):
    with open(log_path, "a", encoding="utf-8") as f:
        f.write(line + "\n")

try:
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.bind(sock_path)
        s.listen(1)
    finally:
        s.close()
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    log("unix_socket_bind_preflight=PASS tmpdir=%s" % tmpdir)
    sys.exit(0)
except OSError as exc:
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    log("unix_socket_bind_preflight=FAIL errno=%s strerror=%s tmpdir=%s path=%s" %
        (getattr(exc, "errno", ""), getattr(exc, "strerror", ""), tmpdir, sock_path))
    sys.exit(125 if getattr(exc, "errno", None) == 1 else 1)
PY
}

sync_prefix_from_dist() {
    local log_file="$1"
    [[ -x "$PREFIX_SYNC_SCRIPT" ]] || {
        echo "prefix_sync=FAIL reason=missing_sync_script path=$PREFIX_SYNC_SCRIPT" >"$log_file"
        return 126
    }
    "$PREFIX_SYNC_SCRIPT" --dist "$WINE_DIST" --prefix "$PREFIX" >"$log_file" 2>&1
}

run_build_freshness_preflight() {
    local log_file="$1"
    [[ "${MACRUNNER_NPP_VERIFY_FRESHNESS:-1}" != "0" ]] || {
        echo "build_freshness_preflight=SKIP reason=MACRUNNER_NPP_VERIFY_FRESHNESS=0" >"$log_file"
        return 0
    }
    [[ -x "$FRESHNESS_SCRIPT" ]] || {
        echo "build_freshness_preflight=SKIP reason=missing_script path=$FRESHNESS_SCRIPT" >"$log_file"
        return 0
    }
    "$FRESHNESS_SCRIPT" >"$log_file" 2>&1
    local rc=$?
    if (( rc == 0 )); then
        echo "build_freshness_preflight=PASS" >>"$log_file"
    else
        echo "build_freshness_preflight=FAIL rc=$rc" >>"$log_file"
    fi
    return $rc
}

# --- Cleanup any prior Wine tails (mandatory hygiene) ---
PHASE_START=$(now_ns)
if [[ -x "$ROOT/scripts/kill-wine-tree.sh" ]]; then
    "$ROOT/scripts/kill-wine-tree.sh" >/dev/null 2>&1 || true
else
    pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver' 2>/dev/null || true
fi
sleep 1
remove_stale_wineserver_roots
record_phase "cleanup_prewarm" "$PHASE_START" "$(now_ns)"

set +e
PHASE_START=$(now_ns)
unix_socket_bind_preflight "$RUN_DIR/infra-preflight.log"
PREFLIGHT_RC=$?
set -e
record_phase "infra_preflight" "$PHASE_START" "$(now_ns)"
if (( PREFLIGHT_RC != 0 )); then
    {
        echo "status=125"
        echo "reason=INFRA_WINESERVER_BIND_OPERATION_NOT_PERMITTED"
        echo "detail=unix_socket_bind_preflight_failed"
        echo "tmpdir=$TMPDIR"
    } >"$RUN_DIR/infra.status"
    echo "infra_preflight_failure=$PREFLIGHT_RC"
    exit 125
fi

PHASE_START=$(now_ns)
if ! sync_prefix_from_dist "$RUN_DIR/prefix-sync.log"; then
    {
        echo "status=126"
        echo "reason=PREFIX_SYNC_FAILED"
        echo "prefix=$PREFIX"
        echo "dist=$WINE_DIST"
        tail -80 "$RUN_DIR/prefix-sync.log" 2>/dev/null || true
    } >"$RUN_DIR/prefix-sync.status"
    echo "prefix_sync_failure=126"
    exit 126
fi
record_phase "prefix_sync" "$PHASE_START" "$(now_ns)"

PHASE_START=$(now_ns)
if ! run_build_freshness_preflight "$RUN_DIR/freshness-preflight.log"; then
    {
        echo "status=126"
        echo "reason=BUILD_FRESHNESS_FAILED"
        echo "freshness_log=$RUN_DIR/freshness-preflight.log"
        tail -80 "$RUN_DIR/freshness-preflight.log" 2>/dev/null || true
    } >"$RUN_DIR/freshness.status"
    echo "build_freshness_failure=126"
    # Invalidate warm cache on freshness failure to prevent stale warm starts
    if [[ -x "$WARM_MANAGER" ]]; then
        "$WARM_MANAGER" invalidate >"$RUN_DIR/warm-invalidate.log" 2>&1 || true
    fi
    exit 126
fi
record_phase "freshness_check" "$PHASE_START" "$(now_ns)"

# --- Warm prefix restore (after freshness, before wineboot) ---
WARM_RESTORED=0
WARM_CACHE_KEY=""
WARM_MANAGER="$ROOT/scripts/warm-prefix-manager.sh"
if [[ -x "$WARM_MANAGER" ]]; then
    PHASE_START=$(now_ns)
    # Pre-compute key for telemetry even if restore fails
    WARM_CACHE_KEY=$("$WARM_MANAGER" restore "$PREFIX" "$WINE_DIST" >"$RUN_DIR/warm-restore.log" 2>&1 && echo "RESTORE_OK" || echo "RESTORE_FAIL")
    if grep -q 'warm_restore=1' "$RUN_DIR/warm-restore.log" 2>/dev/null; then
        WARM_RESTORED=1
        WARM_CACHE_KEY=$(grep 'warm_restore=1' "$RUN_DIR/warm-restore.log" | head -1 | sed 's/.*key=//')
        export MACRUNNER_SKIP_WINEBOOT=1
    fi
    record_phase "warm_restore" "$PHASE_START" "$(now_ns)"
fi

# --- Env setup ---
mkdir -p "$PREFIX"
export WINEPREFIX="$PREFIX"
export MACRUNNER_WINE_DIST="$WINE_DIST"
EXE_MACHINE="$(python3 - "$ROOT" "$APP" <<'PY'
import json
import subprocess
import sys

try:
    payload = json.loads(subprocess.check_output([f"{sys.argv[1]}/tools/pe_inspector.py", sys.argv[2], "--json"], text=True))
    print(payload.get("machine", ""))
except Exception:
    print("")
PY
    )"
if [[ "$EXE_MACHINE" == "x86" ]]; then
    export MACRUNNER_HB_X64_LOADER=0
elif [[ -z "${MACRUNNER_HB_X64_LOADER+x}" ]]; then
    export MACRUNNER_HB_X64_LOADER=1
fi
# FreeType lives in Homebrew but our Wine is built without explicit rpath.
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}"
export WINEDEBUG="${WINEDEBUG:--all}"

# --- Optional traces ---
# Guard against empty array under set -u: only iterate if TRACES is non-empty.
if [[ -n "$TRACES" ]]; then
    IFS=',' read -ra TRACE_LIST <<< "$TRACES"
else
    TRACE_LIST=()
fi

apply_app_traces() {
    for t in ${TRACE_LIST[@]+"${TRACE_LIST[@]}"}; do
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
}

# Tracing wineboot can perturb bootstrap timing; keep default traces app-only.
if [[ "${MACRUNNER_TRACE_WINEBOOT:-0}" != "0" ]]; then
    apply_app_traces
fi

bounded_wineserver_wait() {
    local log_file="$1"
    local wait_secs="${MACRUNNER_WINESERVER_WAIT_SECS:-20}"
    local max_ticks=$((wait_secs * 10))
    local ticks=0
    local wait_pid

    "$WINESERVER" -w >/dev/null 2>>"$log_file" &
    wait_pid=$!
    while kill -0 "$wait_pid" 2>/dev/null; do
        if ((ticks >= max_ticks)); then
            echo "wineserver_wait_timeout secs=$wait_secs pid=$wait_pid" >>"$log_file"
            kill "$wait_pid" >/dev/null 2>&1 || true
            sleep 0.2
            kill -9 "$wait_pid" >/dev/null 2>&1 || true
            wait "$wait_pid" >/dev/null 2>&1 || true
            return 124
        fi
        sleep 0.1
        ticks=$((ticks + 1))
    done
    wait "$wait_pid" >/dev/null 2>&1 || true
    return 0
}

bounded_command() {
    local label="$1"
    local wait_secs="$2"
    local stdout_file="$3"
    local stderr_file="$4"
    shift 4
    local max_ticks=$((wait_secs * 10))
    local ticks=0
    local cmd_pid
    local cmd_rc

    "$@" >"$stdout_file" 2>"$stderr_file" &
    cmd_pid=$!
    while kill -0 "$cmd_pid" 2>/dev/null; do
        if ((ticks >= max_ticks)); then
            echo "${label}_timeout secs=$wait_secs pid=$cmd_pid" >>"$stderr_file"
            kill "$cmd_pid" >/dev/null 2>&1 || true
            sleep 0.2
            kill -9 "$cmd_pid" >/dev/null 2>&1 || true
            wait "$cmd_pid" >/dev/null 2>&1 || true
            return 124
        fi
        sleep 0.1
        ticks=$((ticks + 1))
    done
    set +e
    wait "$cmd_pid" >/dev/null 2>&1
    cmd_rc=$?
    set -e
    return "$cmd_rc"
}

wine_server_lock_exists() {
    local uid root
    uid="$(id -u)"
    for root in "$TMPDIR/.wine-$uid" "/tmp/.wine-$uid" "/private/tmp/.wine-$uid"; do
        [[ -d "$root" ]] || continue
        if find "$root" -maxdepth 2 -name lock -type f -print -quit 2>/dev/null | grep -q .; then
            return 0
        fi
    done
    return 1
}

wineserver_process_alive() {
    pgrep -x wineserver >/dev/null 2>&1
}

wait_for_wineserver_ready() {
    local app_pid="$1"
    local app_log="$2"
    local ready_log="$3"
    local wait_secs="${MACRUNNER_NPP_AFTER_LAUNCH_READY_SECS:-30}"
    local max_ticks=$((wait_secs * 4))
    local ticks=0

    while ((ticks < max_ticks)); do
        if ! kill -0 "$app_pid" 2>/dev/null; then
            echo "wineserver_ready=0 reason=app_exited ticks=$ticks" >>"$ready_log"
            return 1
        fi
        if grep -q 'wineserver: bind: Operation not permitted' "$app_log" 2>/dev/null; then
            echo "wineserver_ready=0 reason=INFRA_WINESERVER_BIND_OPERATION_NOT_PERMITTED ticks=$ticks" >>"$ready_log"
            return 125
        fi
        if grep -q 'macrunner-hb-signal-init: .*stage=wine-process-start' "$app_log" 2>/dev/null &&
           wineserver_process_alive &&
           wine_server_lock_exists; then
            echo "wineserver_ready=1 ticks=$ticks" >>"$ready_log"
            return 0
        fi
        ticks=$((ticks + 1))
        sleep 0.25
    done
    echo "wineserver_ready=0 reason=timeout secs=$wait_secs" >>"$ready_log"
    return 1
}

# --- Prepare prefix (wineboot + window-metrics repair) ---
PHASE_START=$(now_ns)
if [[ "${MACRUNNER_SKIP_WINEBOOT:-0}" == "1" ]]; then
    echo "wineboot_skipped=1 reason=MACRUNNER_SKIP_WINEBOOT" >"$RUN_DIR/wineboot-skip.log"
else
    WINEBOOT_WAIT_SECS="${MACRUNNER_WINEBOOT_WAIT_SECS:-60}"
    bounded_command wineboot_init "$WINEBOOT_WAIT_SECS" /dev/null "$RUN_DIR/wineboot-init.err" "$WINE" wineboot --init || {
        echo "wineboot_init_bounded_failure rc=$?" >>"$RUN_DIR/wineboot-init.err"
        "$WINESERVER" -k >/dev/null 2>&1 || true
    }
    bounded_command wineboot_update "$WINEBOOT_WAIT_SECS" /dev/null "$RUN_DIR/wineboot-update.err" "$WINE" wineboot --update || {
        echo "wineboot_update_bounded_failure rc=$?" >>"$RUN_DIR/wineboot-update.err"
        "$WINESERVER" -k >/dev/null 2>&1 || true
    }
    bounded_wineserver_wait "$RUN_DIR/wineserver-wait.log" || true
fi
record_phase "wineboot" "$PHASE_START" "$(now_ns)"

PHASE_START=$(now_ns)
[[ -x "$REPAIR_SCRIPT" ]] && "$REPAIR_SCRIPT" --prefix "$PREFIX" --fix >"$RUN_DIR/metrics-repair.log" 2>&1 || true
record_phase "metrics_repair" "$PHASE_START" "$(now_ns)"

# Snapshot only while the prefix is idle. Taking an rsync snapshot after the app
# process starts can race Notepad++/Wine startup and make x64 window probes flaky.
if [[ "$WARM_RESTORED" == "0" && -x "$WARM_MANAGER" ]]; then
    PHASE_START=$(now_ns)
    "$WARM_MANAGER" snapshot "$PREFIX" "$WINE_DIST" >"$RUN_DIR/warm-snapshot.log" 2>&1 || true
    record_phase "warm_snapshot" "$PHASE_START" "$(now_ns)"
fi

# --- Launch (cwd MUST be APPDIR — notepad++ looks for langs.xml etc. in cwd) ---
apply_app_traces
cd "$APPDIR"
PHASE_START=$(now_ns)
(
    set +e
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
record_phase "app_launch" "$PHASE_START" "$(now_ns)"

if [[ -n "$AFTER_LAUNCH_CMD" ]]; then
    export MACRUNNER_NPP_RUN_DIR="$RUN_DIR"
    export MACRUNNER_NPP_WPID="$WPID"
    export MACRUNNER_NPP_APP="$APP"
    export MACRUNNER_NPP_APPDIR="$APPDIR"
    export MACRUNNER_NPP_PREFIX="$PREFIX"
    set +e
    PHASE_START=$(now_ns)
    wait_for_wineserver_ready "$WPID" "$RUN_DIR/stderr.log" "$RUN_DIR/after-launch-readiness.log"
    READY_RC=$?
    record_phase "wineserver_ready" "$PHASE_START" "$(now_ns)"
    set -e
    if (( READY_RC != 0 )); then
        echo "$READY_RC" >"$RUN_DIR/after-launch-hook.exit"
        echo "after_launch_hook_skipped=$READY_RC"
        exit 0
    fi
    if [[ -x "$READINESS_GATE" ]]; then
        "$READINESS_GATE" wait --level WINESERVER_READY --run-dir "$RUN_DIR" --host-only \
            --timeout 2 --app-pid "$WPID" --app-log "$RUN_DIR/stderr.log" >/dev/null 2>&1 || true
    fi
    echo "after_launch_hook=begin"
    set +e
    PHASE_START=$(now_ns)
    /bin/bash -c "$AFTER_LAUNCH_CMD"
    HOOK_RC=$?
    set -e
    record_phase "after_launch_hook" "$PHASE_START" "$(now_ns)"
    echo "$HOOK_RC" >"$RUN_DIR/after-launch-hook.exit"
    echo "after_launch_hook_exit=$HOOK_RC"
fi

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
PHASE_START=$(now_ns)
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver' 2>/dev/null || true
record_phase "cleanup_postrun" "$PHASE_START" "$(now_ns)"

# --- Emit per-run summary JSON ---
TOTAL_END_NS=$(now_ns)
TOTAL_MS=$(( (TOTAL_END_NS - TOTAL_START_NS) / 1000000 ))
APP_RC=$(cat "$RUN_DIR/exit.code" 2>/dev/null || echo -1)

# Build phases JSON from phases.log
PHASES_JSON=""
if [[ -f "$RUN_DIR/phases.log" ]]; then
    PHASES_JSON=$(while IFS='=' read -r key val; do
        printf '    "%s": %s,\n' "$key" "$val"
    done < "$RUN_DIR/phases.log" | sed '$ s/,$//')
fi

# Determine infra status
INFRA_STATUS="FAIL"
if [[ -f "$RUN_DIR/infra-preflight.log" ]] && grep -q 'unix_socket_bind_preflight=PASS' "$RUN_DIR/infra-preflight.log" 2>/dev/null; then
    INFRA_STATUS="PASS"
fi
PREFIX_STATUS="FAIL"
if [[ -f "$RUN_DIR/prefix-sync.log" ]] && grep -q 'prefix_sync=' "$RUN_DIR/prefix-sync.log" 2>/dev/null; then
    PREFIX_STATUS="PASS"
fi
FRESHNESS_STATUS="FAIL"
if [[ -f "$RUN_DIR/freshness-preflight.log" ]] && grep -q 'build_freshness_preflight=PASS' "$RUN_DIR/freshness-preflight.log" 2>/dev/null; then
    FRESHNESS_STATUS="PASS"
fi

python3 - "$RUN_DIR" "$TOTAL_MS" "$APP_RC" "$ALIVE" "$INFRA_STATUS" "$PREFIX_STATUS" "$FRESHNESS_STATUS" "$WARM_RESTORED" "$WARM_CACHE_KEY" <<'PY'
import json, os, sys

run_dir, total_ms, app_rc, alive, infra, prefix, freshness, warm_restored, warm_key = sys.argv[1:10]
phases = {}
phases_log = os.path.join(run_dir, "phases.log")
if os.path.isfile(phases_log):
    with open(phases_log, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            try:
                phases[k] = int(v)
            except ValueError:
                phases[k] = v

summary = {
    "schema_version": 1,
    "run_id": os.path.basename(run_dir),
    "timestamp": __import__("datetime").datetime.now(__import__("datetime").timezone.utc).isoformat().replace("+00:00", "Z"),
    "phases": phases,
    "total_ms": int(total_ms),
    "app_rc": int(app_rc),
    "alive_after_hold": int(alive),
    "stderr_bytes": os.path.getsize(os.path.join(run_dir, "stderr.log")) if os.path.isfile(os.path.join(run_dir, "stderr.log")) else 0,
    "warm_restore": int(warm_restored) if warm_restored else 0,
    "warm_cache_key": warm_key or "",
    "infra": {
        "unix_socket_preflight": infra,
        "prefix_sync": prefix,
        "freshness": freshness
    }
}

out_path = os.path.join(run_dir, "summary.json")
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(summary, f, indent=2, sort_keys=True)
    f.write("\n")
print(f"summary_json={out_path}")
PY
