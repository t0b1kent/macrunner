#!/usr/bin/env bash
# ABZU ABBA heap-CS observer — attempt 9 (NOT_GOLDEN, diagnostic only).
# Fixes vs attempt 4 (which hung 50 min in regedit at 99% CPU):
#   * observer is PROCESS-GATED to AbzuGame — wineboot/regedit/services load the same
#     ntdll and wine reuses TIDs per process, so a regedit thread also carried 0x00a4.
#   * WINEDEBUG err+sync (not +sync): keeps our ERR lines and wine's "blocked by",
#     drops the sync TRACE flood that made the registry import crawl.
#   * HARD watchdog with kill-on-timeout; scoped teardown only.
set -euo pipefail

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713i"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
OBSERVER_DLL="$OUT/OBSERVER-ntdll-abzu-v7.dll"
RUNTIME_DLL="$DIST/lib/wine/aarch64-windows/ntdll.dll"
BACKUP_DLL="$OUT/PRE-OBSERVER-ntdll.dll"
OBSERVER_SHA="31608c464511ad53e4c856636ed3659be4088979748e82d6b94f2ca1891f565a"
RUNTIME_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"

HARD_TIMEOUT=480     # 8 min wall clock, kill-on-timeout
GAME_TIMEOUT=90

: > "$OUT/RUNTIME-STATUS.txt"
status() { echo "$1" >> "$OUT/RUNTIME-STATUS.txt"; }
if [ -e "$OUT/ATTEMPT-CONSUMED" ]; then status "REFUSED_RETRY=1"; exit 89; fi

pgrep -fl 'wineserver|/mr-run\.sh|AbzuGame-Win64-Shipping|Hollow Knight' > "$OUT/PRE-RUN-PROCESSES.txt" 2>/dev/null || true
if [ -s "$OUT/PRE-RUN-PROCESSES.txt" ]; then status "SERIALIZATION_BLOCKED=1"; exit 90; fi

obs_now="$(shasum -a 256 "$OBSERVER_DLL" | cut -d' ' -f1)"
rt_now="$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)"
if [ "$obs_now" != "$OBSERVER_SHA" ] || [ "$rt_now" != "$RUNTIME_SHA" ]; then
    status "HASH_PREFLIGHT_FAILED=1"; status "OBSERVER_NOW=$obs_now"; status "RUNTIME_NOW=$rt_now"; exit 91
fi

cp -p "$RUNTIME_DLL" "$BACKUP_DLL"
teardown() {
    # scoped only: our own wine tree, never a global pkill
    pkill -f "$DIST/" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$DIST/" 2>/dev/null || true
    cp -p "$BACKUP_DLL" "$RUNTIME_DLL"
    status "RESTORED_RUNTIME_SHA=$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)"
}
trap teardown EXIT INT TERM
cp -p "$OBSERVER_DLL" "$RUNTIME_DLL"
[ "$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)" = "$OBSERVER_SHA" ] || { status "DEPLOY_HASH_FAILED=1"; exit 92; }

: > "$OUT/ATTEMPT-CONSUMED"
status "DEPLOYED_RUNTIME_SHA=$OBSERVER_SHA"

export MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1 MACRUNNER_HB_WINEMETAL_UNIX_FALLBACK=1
export MACRUNNER_GRAPHICS_BACKEND=dxmt
export MACRUNNER_MR_RUN_START_SERVICES=1 MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1
export MACRUNNER_HB_ARM64_SYSCALL_BRIDGE=1 MACRUNNER_HB_NATIVE_SYSCALL_SPLIT=1
export MACRUNNER_HB_EH_BENIGN_NOOP=1 MACRUNNER_HB_NATIVE_DIRECT_CALLBACK_SPLIT=1
export MACRUNNER_HB_HEAP_CS_OBSERVER=1
export WINEMSYNC=1 MACRUNNER_HB_IR_CACHE_SIZE=524288
export MACRUNNER_HB_JIT_DIRECT_MEM=1 MACRUNNER_HB_NATIVE_MEMMOVE=1 MACRUNNER_HB_SINGLE_LOOKUP=1
export MACRUNNER_PREFIX_SYSTEM32_ARCH=x86_64-windows
export MACRUNNER_MR_RUN_USE_WARM_PREFIX=0 MACRUNNER_MR_RUN_SKIP_WINEBOOT=1
export MACRUNNER_MR_RUN_WIRE_VULKAN_ICD=1 MACRUNNER_MR_RUN_SERVICES_WAIT=6
export WINEDLLOVERRIDES='d3d11,dxgi,d3d10core,winemetal=n,b'
export WINEDEBUG='-all,+sync'
export MACRUNNER_HB_BACKEND=jit MACRUNNER_HB_CRT_CASE_FUSION=1
export MACRUNNER_HB_MAIN_ENTRY_TRACE=1
export MACRUNNER_NO_AUTOTRIAGE=1

( cd "$ROOT" && scripts/mr-run.sh "$DIST" "$GAME" "$GAME_TIMEOUT" ) > "$OUT/run.log" 2>&1 &
wrapper_pid=$!
status "WRAPPER_PID=$wrapper_pid"
date -u '+WRAPPER_START_UTC=%Y-%m-%dT%H:%M:%SZ' >> "$OUT/RUNTIME-STATUS.txt"

waited=0
while kill -0 "$wrapper_pid" 2>/dev/null; do
    if [ "$waited" -ge "$HARD_TIMEOUT" ]; then
        status "HARD_TIMEOUT_HIT=${HARD_TIMEOUT}s"
        ps -axo pid=,etime=,command= | grep "$DIST/" | grep -v grep > "$OUT/TIMEOUT-PROCESSES.txt" 2>/dev/null || true
        pkill -f "$DIST/" 2>/dev/null || true
        sleep 2
        pkill -9 -f "$DIST/" 2>/dev/null || true
        kill -9 "$wrapper_pid" 2>/dev/null || true
        break
    fi
    sleep 5
    waited=$((waited + 5))
done

set +e
wait "$wrapper_pid" 2>/dev/null
status "RUNNER_EXIT=$?"
status "WALL_SECONDS=$waited"
set -e
exit 0
