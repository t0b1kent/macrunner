#!/usr/bin/env bash
set -u

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
MAIN="/Users/timurtoby/Documents/MacRunner/Main/MacRunner"
OUT="$MAIN/reports/research/checkpoints/abzu-cs-forward-fix-a-off-control-20260713-NOT_GOLDEN"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"
FIX_DLL="$MAIN/engine/wine/build-arm64ec-spike/dlls/ntdll/ntdll.so"
RUNTIME_DLL="$DIST/lib/wine/aarch64-unix/ntdll.so"
OVERLAY_WINEMETAL="$DIST/lib/wine/x86_64-windows/winemetal.dll"
SOURCE_D3D11="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll"
SOURCE_DXGI="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/dxgi.dll"
SOURCE_WINEMETAL="$ROOT/engine/graphics/dist/dxmt/x86_64-windows/winemetal.dll"
RUNNER="$ROOT/scripts/mr-run.sh"
BACKUP_DLL="$OUT/PRE-CONTROL-ntdll.so"
FIX_SHA="3d7d65c1e2e14ef8413bbe021361ea65bfdf0f4c94a3f4159d6918b888db2b57"
RUNTIME_SHA="14d3563d105defde6efae10563e58b3ac7e88278b2533b96e882c8d6efacf2b5"
OVERLAY_WINEMETAL_SHA="7aa914d654101470b9fa5440dd3a82331087208f5094a5d7c9cb0cb875d9fc24"
SOURCE_D3D11_SHA="842fa9e7228184ba46d1b81e2c75c0952fa813516b812f072c3d8cb9ff818ce8"
SOURCE_DXGI_SHA="30eb89bf1b37e2d650006105087c4c2e3e13cd1c9b067d47e793dcc6b93f8035"
SOURCE_WINEMETAL_SHA="e49765a9e1a2f0f0522d24c07b0db48f769afa4e84908eca9f8b289289a55929"
RUNNER_SHA="d2d23492abd564693cab90cb0c49204b6f357f1af3f4244bd6cda6200eec8ee2"
GAME_SHA="b9bdbc743742f45845b152df7663b042e2b5da5d318894584d70f6d63d6839e7"
MAIN_HEAD="0b09f80511f51e0eb8f5d58da642ae0e6a866745"
ABZU_HEAD="f58219bb31dd808c4da6784f8cb4cd1775a88edf"

sha()
{
    shasum -a 256 "$1" | awk '{print $1}'
}

if [ -e "$OUT/ATTEMPT-CONSUMED" ]; then
    echo "REFUSED_RETRY=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 89
fi

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
   [ "$(sha "$FIX_DLL")" != "$FIX_SHA" ] ||
   [ "$(sha "$RUNTIME_DLL")" != "$RUNTIME_SHA" ] ||
   [ "$(sha "$OVERLAY_WINEMETAL")" != "$OVERLAY_WINEMETAL_SHA" ] ||
   [ "$(sha "$SOURCE_D3D11")" != "$SOURCE_D3D11_SHA" ] ||
   [ "$(sha "$SOURCE_DXGI")" != "$SOURCE_DXGI_SHA" ] ||
   [ "$(sha "$SOURCE_WINEMETAL")" != "$SOURCE_WINEMETAL_SHA" ] ||
   [ "$(sha "$RUNNER")" != "$RUNNER_SHA" ] ||
   [ "$(sha "$GAME")" != "$GAME_SHA" ]; then
    echo "HASH_OR_HEAD_PREFLIGHT_FAILED=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 91
fi

cp -p "$RUNTIME_DLL" "$BACKUP_DLL"
restore_runtime()
{
    cp -p "$BACKUP_DLL" "$RUNTIME_DLL"
    echo "RESTORED_RUNTIME_SHA=$(sha "$RUNTIME_DLL")" >> "$OUT/RUNTIME-STATUS.txt"
}
trap restore_runtime EXIT
cp -p "$FIX_DLL" "$RUNTIME_DLL"
if [ "$(sha "$RUNTIME_DLL")" != "$FIX_SHA" ]; then
    echo "DEPLOY_HASH_FAILED=1" > "$OUT/RUNTIME-STATUS.txt"
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
unset MACRUNNER_TRACE_UI_INPUT

export MACRUNNER_HB_WINEMETAL_X64_DLLMAIN=1
export MACRUNNER_HB_WINEMETAL_UNIX_FALLBACK=1
export MACRUNNER_GRAPHICS_BACKEND=dxmt
export MACRUNNER_MR_RUN_START_SERVICES=1
export MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1
export MACRUNNER_HB_ARM64_SYSCALL_BRIDGE=1
export MACRUNNER_HB_NATIVE_SYSCALL_SPLIT=1
export MACRUNNER_HB_EH_BENIGN_NOOP=1
export MACRUNNER_HB_NATIVE_DIRECT_CALLBACK_SPLIT=1
export MACRUNNER_HB_CS_FORWARD_NTDLL=0
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
echo "DEPLOYED_RUNTIME_SHA=$FIX_SHA" >> "$OUT/RUNTIME-STATUS.txt"
echo "CONTROL_CS_FORWARD=0" >> "$OUT/RUNTIME-STATUS.txt"
(
    cd "$ROOT"
    scripts/mr-run.sh "$DIST" "$GAME" 300
) > "$OUT/run.log" 2>&1 &
wrapper_pid=$!
echo "WRAPPER_PID=$wrapper_pid" >> "$OUT/RUNTIME-STATUS.txt"

game_pid=""
for _ in $(seq 1 180); do
    game_pid=$(ps -axo pid=,args= | awk '$0 ~ /AbzuGame-Win64-Shipping\.exe/ && $0 !~ /awk/ { pid=$1 } END { if (pid) print pid }')
    [ -n "$game_pid" ] && break
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 1
done

if [ -n "$game_pid" ]; then
    echo "GAME_PID=$game_pid" >> "$OUT/RUNTIME-STATUS.txt"
    sampled_120=0
    sampled_270=0
    while kill -0 "$game_pid" 2>/dev/null; do
        etime=$(ps -p "$game_pid" -o etime= | tr -d ' ')
        [ -z "$etime" ] && break
        age=$(python3 - "$etime" <<'PY'
import sys
s=sys.argv[1]; d=0
if '-' in s: ds,s=s.split('-',1); d=int(ds)
p=[int(x) for x in s.split(':')]
print((p[-1] if p else 0)+(p[-2]*60 if len(p)>1 else 0)+(p[-3]*3600 if len(p)>2 else 0)+d*86400)
PY
)
        if [ "$age" -ge 120 ] && [ "$sampled_120" -eq 0 ]; then
            echo "SAMPLE_120_AGE=$age" >> "$OUT/RUNTIME-STATUS.txt"
            sample "$game_pid" 4 -file "$OUT/HOST-SAMPLE-120.txt" > "$OUT/HOST-SAMPLE-120-COMMAND.txt" 2>&1 || true
            sampled_120=1
        fi
        if [ "$age" -ge 270 ] && [ "$sampled_270" -eq 0 ]; then
            echo "SAMPLE_270_AGE=$age" >> "$OUT/RUNTIME-STATUS.txt"
            sample "$game_pid" 4 -file "$OUT/HOST-SAMPLE-270.txt" > "$OUT/HOST-SAMPLE-270-COMMAND.txt" 2>&1 || true
            sampled_270=1
        fi
        sleep 2
    done
else
    echo "GAME_PID=NOT_FOUND" >> "$OUT/RUNTIME-STATUS.txt"
fi

wait "$wrapper_pid"
runner_rc=$?
echo "RUNNER_EXIT=$runner_rc" >> "$OUT/RUNTIME-STATUS.txt"
date -u '+WRAPPER_END_UTC=%Y-%m-%dT%H:%M:%SZ' >> "$OUT/RUNTIME-STATUS.txt"
ps -axo pid=,ppid=,etime=,command= > "$OUT/POST-RUN-PROCESSES.txt"
exit 0
