#!/usr/bin/env bash
# CONTROL: identical harness/env, but the frozen ABZU dist is NOT touched.
# Isolates the single variable: did the Main-tree PE ntdll.dll cause the
# pre-main recursive fault storm, or does ABZU storm on this dist anyway?
set -euo pipefail

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713b"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
RUNTIME_DLL="$DIST/lib/wine/aarch64-windows/ntdll.dll"
BASELINE_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"

pgrep -f 'wineserver|/mr-run\.sh|AbzuGame-Win64-Shipping|Hollow Knight' >/dev/null 2>&1 && {
    echo "CONTROL_SERIALIZATION_BLOCKED=1" >> "$OUT/CONTROL-STATUS.txt"; exit 90; }

now="$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)"
: > "$OUT/CONTROL-STATUS.txt"
echo "RUNTIME_SHA_AT_START=$now" >> "$OUT/CONTROL-STATUS.txt"
[ "$now" = "$BASELINE_SHA" ] || { echo "CONTROL_ABORT_NOT_BASELINE=1" >> "$OUT/CONTROL-STATUS.txt"; exit 91; }

# same env as the observer run, minus the observer gate
export MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1 MACRUNNER_HB_WINEMETAL_UNIX_FALLBACK=1
export MACRUNNER_GRAPHICS_BACKEND=dxmt
export MACRUNNER_MR_RUN_START_SERVICES=1 MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1
export MACRUNNER_HB_ARM64_SYSCALL_BRIDGE=1 MACRUNNER_HB_NATIVE_SYSCALL_SPLIT=1
export MACRUNNER_HB_EH_BENIGN_NOOP=1 MACRUNNER_HB_NATIVE_DIRECT_CALLBACK_SPLIT=1
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

set +e
( cd "$ROOT" && scripts/mr-run.sh "$DIST" "$GAME" 90 ) > "$OUT/control-run.log" 2>&1
echo "CONTROL_RUNNER_EXIT=$?" >> "$OUT/CONTROL-STATUS.txt"
set -e
echo "RUNTIME_SHA_AT_END=$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)" >> "$OUT/CONTROL-STATUS.txt"
exit 0
