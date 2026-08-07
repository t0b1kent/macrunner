#!/usr/bin/env bash
# ABZU ABBA heap-CS observer — attempt 4 (NOT_GOLDEN, diagnostic only).
# Key fix vs attempt 2: the PE ntdll.dll is built IN THE MacRunner-abzu TREE, so it
# is ABI-matched to the frozen dist's unix ntdll.so. Attempt 2 deployed a Main-tree
# PE ntdll next to abzu's unix ntdll.so -> pre-main recursive fault storm, exit=1.
set -euo pipefail

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713d"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
OBSERVER_DLL="$OUT/OBSERVER-ntdll-abzu-v2.dll"
RUNTIME_DLL="$DIST/lib/wine/aarch64-windows/ntdll.dll"
BACKUP_DLL="$OUT/PRE-OBSERVER-ntdll.dll"
OBSERVER_SHA="11852858b310aa6476f23c783d4e08072a23ac538894198a399cc9925343dfc3"
RUNTIME_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"

GAME_TIMEOUT=90
DETECT_WINDOW=420

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

for _ in $(seq 1 "$DETECT_WINDOW"); do
    pgrep -f 'AbzuGame-Win64-Shipping\.exe' >/dev/null 2>&1 && { status "GAME_SEEN=1"; break; }
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 1
done

set +e
wait "$wrapper_pid"
status "RUNNER_EXIT=$?"
set -e
exit 0
