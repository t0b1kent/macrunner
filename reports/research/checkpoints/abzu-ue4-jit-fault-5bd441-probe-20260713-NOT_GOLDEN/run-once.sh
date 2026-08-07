#!/usr/bin/env bash
set -u

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
MAIN="/Users/timurtoby/Documents/MacRunner/Main/MacRunner"
OUT="$MAIN/reports/research/checkpoints/abzu-ue4-jit-fault-5bd441-probe-20260713-NOT_GOLDEN"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
FIX_UNIX="$MAIN/engine/wine/build-arm64ec-spike/dlls/ntdll/ntdll.so"
FIX_PE="$MAIN/engine/wine/build-arm64ec-spike/dlls/ntdll/aarch64-windows/ntdll.dll"
RUNTIME_UNIX="$DIST/lib/wine/aarch64-unix/ntdll.so"
RUNTIME_PE="$DIST/lib/wine/aarch64-windows/ntdll.dll"
OVERLAY_WINEMETAL="$DIST/lib/wine/x86_64-windows/winemetal.dll"
SOURCE_D3D11="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll"
SOURCE_DXGI="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/dxgi.dll"
SOURCE_WINEMETAL="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/winemetal.dll"
DXMT_MANIFEST="$ROOT/engine/graphics/dist/manifest/dxmt.json"
RUNNER="$ROOT/scripts/mr-run.sh"
CACHE_ROOT="$ROOT/engine/hyperbridge/build/hyperbridge-cache"
BACKUP_UNIX="$OUT/PRE-VERIFY-ntdll.so"
BACKUP_PE="$OUT/PRE-VERIFY-ntdll.dll"

FIX_UNIX_SHA="4b16eda1e30b0d1678524b5414a4a098a5a8b85cb2abbe6553b9112ed3d8f272"
FIX_PE_SHA="38841810948dd4e46abafa0bd103099bb3576ad95408e3645a9a7907f1828ea3"
RUNTIME_UNIX_SHA="14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5"
RUNTIME_PE_SHA="8e2a5691dfbec4dacf614d4b280a4ec8f07148a9bd7dea8230fa95246a3a43dc"
OVERLAY_WINEMETAL_SHA="7aa914d654101470b9fa5440dd3a82331087208f5094a5d7c9cb0cb875d9fc24"
SOURCE_D3D11_SHA="842fa9e7228184ba46d1b81e2c75c0952fa813516b812f072c3d8cb9ff818ce8"
SOURCE_DXGI_SHA="30eb89bf1b37e2d650006105087c4c2e3e13cd1c9b067d47e793dcc6b93f8035"
SOURCE_WINEMETAL_SHA="e49765a9e1a2f0f0522d24c07b0db48f769afa4e84908eca9f8b289289a55929"
DXMT_MANIFEST_SHA="06e8f90784215761e1163ea54ee12a92f604ac55de0dad55d6563cbf56968358"
RUNNER_SHA="d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2"
GAME_SHA="b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7"
LOADER_SOURCE_SHA="f68c801b25eacd29f3dbcb0965816360325ed1983d01ffbf01b7c38b131be8b5"
HB_SOURCE_SHA="9e1b9ec42bb27271f7cba1e647388a650a7453c7e981a42a25699ddb211897af"
SYNC_SOURCE_SHA="2d17e9a6bb78be743ef5a0906fdaa1a744a482b1cabb3d12e0c7f643039581ab"
CLASS_GUARD_SHA="a60b3cdfae62385319a513b1ae851d8e757b7b7b8c93f79bbbf590a9fc078a33"
JIT_GUARD_SHA="50bcfccf05fc715f6de5d534fbcd55d24212e6be98ca0b2ee662773be2f8b6bc"
CS_GUARD_SHA="67fb683fd589b8b48fb1e8eb5889a17b275576befd349383f72b10b69e348b5f"
IAT_GUARD_SHA="9c3c9ab4a29fad0a5c93fc258127b78da7d5e72d05bd2f670afa66c94952127b"
BUILD_LOG_SHA="717e9c18d51c46f63b6ebe8b2b3cea17fe05111a3c35cf68b1202ec75c1122fa"
MAIN_HEAD="4be5ec135492d622b13acc7a22a53738a0776024"
ABZU_HEAD="2b62f6b7ac71f1c64c00ce39e0ff6fb998dc01c8"

sha()
{
    shasum -a 256 "$1" | awk '{print $1}'
}

cache_inventory()
{
    output="$1"
    : > "$output"
    if [ -d "$CACHE_ROOT" ]; then
        find "$CACHE_ROOT" -type f -print0 | LC_ALL=C sort -z | while IFS= read -r -d '' path; do
            stat -f '%N|%z|%m|%p' "$path"
            shasum -a 256 "$path"
        done >> "$output"
    fi
}

if [ -e "$OUT/ATTEMPT-CONSUMED" ]; then
    echo "REFUSED_RETRY=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 89
fi

date -u '+UTC=%Y-%m-%dT%H:%M:%SZ' > "$OUT/HOST-CONTRACT.txt"
date '+LOCAL=%Y-%m-%dT%H:%M:%S%z %Z' >> "$OUT/HOST-CONTRACT.txt"
uname -a >> "$OUT/HOST-CONTRACT.txt"
sw_vers >> "$OUT/HOST-CONTRACT.txt"
locale >> "$OUT/HOST-CONTRACT.txt"
git -C "$MAIN" branch --show-current > "$OUT/BRANCH-STATUS.txt"
git -C "$MAIN" status -s >> "$OUT/BRANCH-STATUS.txt"
git -C "$ROOT" branch --show-current >> "$OUT/BRANCH-STATUS.txt"
git -C "$ROOT" status -s >> "$OUT/BRANCH-STATUS.txt"
df -h . > "$OUT/DF-PRE.txt"
cache_inventory "$OUT/CACHE-INVENTORY-PRE.txt"
ps -axo pid=,ppid=,etime=,command= > "$OUT/PRE-RUN-PROCESSES-ALL.txt"

if ! python3 - "$OUT/PRE-RUN-PROCESSES-ALL.txt" "$OUT/PRE-RUN-PROCESSES.txt" <<'PY'
import re
import sys

source, output = sys.argv[1:]
patterns = (
    re.compile(r"(?:^|[ /])wine(?:server|boot|64|preloader)?(?:[ .]|$)", re.I),
    re.compile(r"AbzuGame-Win64-Shipping|Hollow Knight\.exe", re.I),
    re.compile(r"scripts/mr-run\.sh|cmake --build", re.I),
    re.compile(r"(?:^|[ /])(?:ninja|make)(?:[ ]|$)", re.I),
)
hits = []
for line in open(source, errors="replace"):
    if "run-once.sh" in line or "PRE-RUN-PROCESSES" in line:
        continue
    if any(pattern.search(line) for pattern in patterns):
        hits.append(line)
open(output, "w").writelines(hits)
raise SystemExit(bool(hits))
PY
then
    echo "SERIALIZATION_BLOCKED=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 90
fi

if [ "$(git -C "$MAIN" rev-parse HEAD)" != "$MAIN_HEAD" ] ||
   [ "$(git -C "$ROOT" rev-parse HEAD)" != "$ABZU_HEAD" ] ||
   [ "$(sha "$FIX_UNIX")" != "$FIX_UNIX_SHA" ] ||
   [ "$(sha "$FIX_PE")" != "$FIX_PE_SHA" ] ||
   [ "$(sha "$RUNTIME_UNIX")" != "$RUNTIME_UNIX_SHA" ] ||
   [ "$(sha "$RUNTIME_PE")" != "$RUNTIME_PE_SHA" ] ||
   [ "$(sha "$OVERLAY_WINEMETAL")" != "$OVERLAY_WINEMETAL_SHA" ] ||
   [ "$(sha "$SOURCE_D3D11")" != "$SOURCE_D3D11_SHA" ] ||
   [ "$(sha "$SOURCE_DXGI")" != "$SOURCE_DXGI_SHA" ] ||
   [ "$(sha "$SOURCE_WINEMETAL")" != "$SOURCE_WINEMETAL_SHA" ] ||
   [ "$(sha "$DXMT_MANIFEST")" != "$DXMT_MANIFEST_SHA" ] ||
   [ "$(sha "$RUNNER")" != "$RUNNER_SHA" ] ||
   [ "$(sha "$GAME")" != "$GAME_SHA" ] ||
   [ "$(sha "$MAIN/engine/wine/dlls/ntdll/loader.c")" != "$LOADER_SOURCE_SHA" ] ||
   [ "$(sha "$MAIN/engine/wine/dlls/ntdll/unix/macrunner_hb.c")" != "$HB_SOURCE_SHA" ] ||
   [ "$(sha "$MAIN/engine/wine/dlls/ntdll/unix/sync.c")" != "$SYNC_SOURCE_SHA" ] ||
   [ "$(sha "$MAIN/tools/test_hb_builtin_unixlib_resolve_source.py")" != "$CLASS_GUARD_SHA" ] ||
   [ "$(sha "$MAIN/tools/test_hb_jit_fault_trace_source.py")" != "$JIT_GUARD_SHA" ] ||
   [ "$(sha "$MAIN/tools/test_hb_cs_forward_source.py")" != "$CS_GUARD_SHA" ] ||
   [ "$(sha "$MAIN/tools/test_hb_dynamic_iat_recovery_source.py")" != "$IAT_GUARD_SHA" ] ||
   [ "$(sha "$MAIN/reports/research/build-logs/abzu-jit-fault-trace-ntdll-20260713.log")" != "$BUILD_LOG_SHA" ]; then
    echo "HASH_OR_HEAD_PREFLIGHT_FAILED=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 91
fi

cp -p "$RUNTIME_UNIX" "$BACKUP_UNIX"
cp -p "$RUNTIME_PE" "$BACKUP_PE"
restore_runtime()
{
    cp -p "$BACKUP_UNIX" "$RUNTIME_UNIX"
    cp -p "$BACKUP_PE" "$RUNTIME_PE"
    echo "RESTORED_UNIX_SHA=$(sha "$RUNTIME_UNIX")" >> "$OUT/RUNTIME-STATUS.txt"
    echo "RESTORED_PE_SHA=$(sha "$RUNTIME_PE")" >> "$OUT/RUNTIME-STATUS.txt"
    cache_inventory "$OUT/CACHE-INVENTORY-POST.txt"
    df -h . > "$OUT/DF-POST.txt"
}
trap restore_runtime EXIT

cp -p "$FIX_UNIX" "$RUNTIME_UNIX"
cp -p "$FIX_PE" "$RUNTIME_PE"
if [ "$(sha "$RUNTIME_UNIX")" != "$FIX_UNIX_SHA" ] ||
   [ "$(sha "$RUNTIME_PE")" != "$FIX_PE_SHA" ]; then
    echo "COHERENT_DEPLOY_HASH_FAILED=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 92
fi

: > "$OUT/ATTEMPT-CONSUMED"

unset MACRUNNER_HB_TRACE_SYNC_CAUSALITY
unset MACRUNNER_HB_TRACE_WAIT_SEMANTIC
unset MACRUNNER_HB_WAIT_OBJECT_OBSERVER
unset MACRUNNER_HB_TRACE_DIRECT_WAIT_ALERT
unset MACRUNNER_HB_TRACE_WAITADDR
unset MACRUNNER_HB_TRACE_GUEST_PC_MOVEMENT
unset MACRUNNER_HB_HEAP_CS_OBSERVER
unset MACRUNNER_HB_EVENT_LIFECYCLE_PROBE
unset MACRUNNER_HB_MAIN_009C_PROBE
unset MACRUNNER_HB_JIT_FAULT_TRACE
unset MACRUNNER_TRACE_UI_INPUT

unset MACRUNNER_HB_BUILTIN_X64_DLLMAIN
export MACRUNNER_HB_BUILTIN_UNIXLIB_RESOLVE=1
export MACRUNNER_HB_JIT_FAULT_TRACE=1
export MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1
export MACRUNNER_HB_WINEMETAL_UNIX_FALLBACK=1
export MACRUNNER_GRAPHICS_BACKEND=dxmt
export MACRUNNER_MR_RUN_START_SERVICES=1
export MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1
export MACRUNNER_HB_ARM64_SYSCALL_BRIDGE=1
export MACRUNNER_HB_NATIVE_SYSCALL_SPLIT=1
export MACRUNNER_HB_EH_BENIGN_NOOP=1
export MACRUNNER_HB_NATIVE_DIRECT_CALLBACK_SPLIT=1
export MACRUNNER_HB_CS_FORWARD_NTDLL=1
export WINEMSYNC=1
export MACRUNNER_HB_IR_CACHE_SIZE=524288
export MACRUNNER_HB_JIT_DIRECT_MEM=1
export MACRUNNER_HB_NATIVE_MEMMOVE=1
export MACRUNNER_HB_SINGLE_LOOKUP=1

export MACRUNNER_PREFIX_SYSTEM32_ARCH=x86_64-windows
export MACRUNNER_MR_RUN_USE_WARM_PREFIX=0
export MACRUNNER_MR_RUN_SKIP_WINEBOOT=1
export MACRUNNER_MR_RUN_WIRE_VULKAN_ICD=1
export MACRUNNER_MR_RUN_SERVICES_WAIT=6
export WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n,b
export WINEDEBUG=-all,+sync
export MACRUNNER_HB_BACKEND=jit
export MACRUNNER_HB_CRT_CASE_FUSION=1
export MACRUNNER_D3D11_CREATEDEVICE_BOUNDARY_TRACE=1
export MACRUNNER_HB_TRACE_DXGI_SWAPCHAIN=1
export MACRUNNER_HB_MAIN_ENTRY_TRACE=1

env -0 > "$OUT/CHILD-ENV.bin"
date -u '+WRAPPER_START_UTC=%Y-%m-%dT%H:%M:%SZ' > "$OUT/RUNTIME-STATUS.txt"
echo "DEPLOYED_UNIX_SHA=$FIX_UNIX_SHA" >> "$OUT/RUNTIME-STATUS.txt"
echo "DEPLOYED_PE_SHA=$FIX_PE_SHA" >> "$OUT/RUNTIME-STATUS.txt"
echo "BUILTIN_UNIXLIB_RESOLVE=1" >> "$OUT/RUNTIME-STATUS.txt"
echo "JIT_FAULT_TRACE=1" >> "$OUT/RUNTIME-STATUS.txt"
echo "CS_FORWARD=1" >> "$OUT/RUNTIME-STATUS.txt"
(
    cd "$ROOT"
    scripts/mr-run.sh "$DIST" "$GAME" 120
) > "$OUT/run.log" 2>&1 &
wrapper_pid=$!
echo "WRAPPER_PID=$wrapper_pid" >> "$OUT/RUNTIME-STATUS.txt"

wait "$wrapper_pid"
runner_rc=$?
echo "RUNNER_EXIT=$runner_rc" >> "$OUT/RUNTIME-STATUS.txt"
date -u '+WRAPPER_END_UTC=%Y-%m-%dT%H:%M:%SZ' >> "$OUT/RUNTIME-STATUS.txt"
ps -axo pid=,ppid=,etime=,command= > "$OUT/POST-RUN-PROCESSES.txt"
exit 0
