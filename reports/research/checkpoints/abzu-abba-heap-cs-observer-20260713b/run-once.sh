#!/usr/bin/env bash
# ABZU ABBA heap-CS observer — attempt 2 (NOT_GOLDEN, diagnostic only).
# Fixes vs attempt 1: set -euo pipefail (fail closed), pgrep preflight instead of
# BSD-hostile awk regex literals, and a game-detect window separated from the
# 90s observation window (attempt 1 lost its whole budget to cold-prefix setup).
set -euo pipefail

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713b"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
OBSERVER_DLL="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/build-arm64ec-spike/dlls/ntdll/aarch64-windows/ntdll.dll"
RUNTIME_DLL="$DIST/lib/wine/aarch64-windows/ntdll.dll"
BACKUP_DLL="$OUT/PRE-OBSERVER-ntdll.dll"
OBSERVER_SHA="a9da52c99f915bccce75a9bf0ba7500b0dea8a0af9c47b6c69b7415f74c8f787"
RUNTIME_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"

GAME_TIMEOUT=90      # observation window, per brief
DETECT_WINDOW=420    # allowance for cold prefix + services before the game appears

: > "$OUT/RUNTIME-STATUS.txt"
status() { echo "$1" >> "$OUT/RUNTIME-STATUS.txt"; }

if [ -e "$OUT/ATTEMPT-CONSUMED" ]; then status "REFUSED_RETRY=1"; exit 89; fi

# --- serialization preflight: fail CLOSED (no awk regex literals) -------------
pgrep -fl 'wineserver|/mr-run\.sh|AbzuGame-Win64-Shipping|wine-preloader|[^a-z]wine ' \
    > "$OUT/PRE-RUN-PROCESSES.txt" 2>/dev/null || true
pgrep -fl 'cmake --build|ninja |[^a-z]make ' >> "$OUT/PRE-RUN-PROCESSES.txt" 2>/dev/null || true
if [ -s "$OUT/PRE-RUN-PROCESSES.txt" ]; then
    status "SERIALIZATION_BLOCKED=1"
    exit 90
fi

# --- hash preflight ----------------------------------------------------------
obs_now="$(shasum -a 256 "$OBSERVER_DLL" | cut -d' ' -f1)"
rt_now="$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)"
if [ "$obs_now" != "$OBSERVER_SHA" ] || [ "$rt_now" != "$RUNTIME_SHA" ]; then
    status "HASH_PREFLIGHT_FAILED=1"
    status "OBSERVER_NOW=$obs_now"
    status "RUNTIME_NOW=$rt_now"
    exit 91
fi

# --- deploy with guaranteed restore ------------------------------------------
cp -p "$RUNTIME_DLL" "$BACKUP_DLL"
restore_runtime() {
    cp -p "$BACKUP_DLL" "$RUNTIME_DLL"
    status "RESTORED_RUNTIME_SHA=$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)"
}
trap restore_runtime EXIT
cp -p "$OBSERVER_DLL" "$RUNTIME_DLL"
[ "$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)" = "$OBSERVER_SHA" ] || { status "DEPLOY_HASH_FAILED=1"; exit 92; }

: > "$OUT/ATTEMPT-CONSUMED"
status "DEPLOYED_RUNTIME_SHA=$OBSERVER_SHA"
date -u '+WRAPPER_START_UTC=%Y-%m-%dT%H:%M:%SZ' >> "$OUT/RUNTIME-STATUS.txt"

# --- environment (pinned to attempt-1 manifest + brief) ----------------------
unset MACRUNNER_TRACE_UI_INPUT MACRUNNER_HB_TRACE_GUEST_PC_MOVEMENT \
      MACRUNNER_HB_TRACE_SYNC_CAUSALITY MACRUNNER_HB_TRACE_WAIT_SEMANTIC \
      MACRUNNER_HB_WAIT_OBJECT_OBSERVER MACRUNNER_HB_TRACE_WAITADDR 2>/dev/null || true

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
export MACRUNNER_D3D11_CREATEDEVICE_BOUNDARY_TRACE=1 MACRUNNER_HB_MAIN_ENTRY_TRACE=1
export MACRUNNER_NO_AUTOTRIAGE=1

env -0 > "$OUT/CHILD-ENV.bin"

( cd "$ROOT" && scripts/mr-run.sh "$DIST" "$GAME" "$GAME_TIMEOUT" ) > "$OUT/run.log" 2>&1 &
wrapper_pid=$!
status "WRAPPER_PID=$wrapper_pid"

game_pid=""
for _ in $(seq 1 "$DETECT_WINDOW"); do
    game_pid="$(pgrep -f 'AbzuGame-Win64-Shipping' | head -1 || true)"
    [ -n "$game_pid" ] && break
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 1
done

if [ -n "$game_pid" ]; then
    status "GAME_PID=$game_pid"
    date -u '+GAME_SEEN_UTC=%Y-%m-%dT%H:%M:%SZ' >> "$OUT/RUNTIME-STATUS.txt"
else
    status "GAME_PID=NOT_FOUND"
fi

set +e
wait "$wrapper_pid"
status "RUNNER_EXIT=$?"
set -e

pgrep -fl 'wineserver|AbzuGame-Win64-Shipping|/mr-run\.sh' > "$OUT/POST-RUN-PROCESSES.txt" 2>/dev/null || true
exit 0
