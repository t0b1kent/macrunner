#!/usr/bin/env bash
# DISCRIMINATOR: same abzu-built observer ntdll, but the observer gate is OFF.
# enabled() (and thus RtlQueryEnvironmentVariable_U on the CS hot path) still runs;
# the ERR/get_name path does not.
#   still storms  -> the crash is in the always-executed env query
#   no storm      -> the crash is in the gated ERR/crit_section_get_name path
set -euo pipefail

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-abba-heap-cs-observer-20260713c"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
OBSERVER_DLL="$OUT/OBSERVER-ntdll-abzu.dll"
RUNTIME_DLL="$DIST/lib/wine/aarch64-windows/ntdll.dll"
BACKUP_DLL="$OUT/PRE-GATEOFF-ntdll.dll"
BASELINE_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"

pgrep -f 'wineserver|/mr-run\.sh|AbzuGame-Win64-Shipping|Hollow Knight' >/dev/null 2>&1 && exit 90
[ "$(shasum -a 256 "$RUNTIME_DLL" | cut -d' ' -f1)" = "$BASELINE_SHA" ] || exit 91

cp -p "$RUNTIME_DLL" "$BACKUP_DLL"
trap 'cp -p "$BACKUP_DLL" "$RUNTIME_DLL"; echo "GATEOFF_RESTORED=$(shasum -a 256 "$RUNTIME_DLL" | cut -d" " -f1)" >> "$OUT/GATEOFF-STATUS.txt"' EXIT
cp -p "$OBSERVER_DLL" "$RUNTIME_DLL"

: > "$OUT/GATEOFF-STATUS.txt"
unset MACRUNNER_HB_HEAP_CS_OBSERVER   # <-- the single variable
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
( cd "$ROOT" && scripts/mr-run.sh "$DIST" "$GAME" 90 ) > "$OUT/gate-off-run.log" 2>&1
echo "GATEOFF_RUNNER_EXIT=$?" >> "$OUT/GATEOFF-STATUS.txt"
exit 0
