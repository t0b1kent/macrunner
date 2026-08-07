#!/usr/bin/env bash
set -u

ROOT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner-abzu"
OUT="/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/research/checkpoints/abzu-game-diagnostic-rtlwait-producer-20260713-NOT_GOLDEN"
DIST="$ROOT/artifacts/_abzu-guest-pc-movement-overlay-runtime-20260713/dist"
GAME="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe"

if [ -e "$OUT/ATTEMPT-CONSUMED" ]; then
    echo "REFUSED_RETRY=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 89
fi

ps -axo pid=,etime=,command= | awk 'BEGIN { IGNORECASE=1 }
    /(^|[ /])wine(server)?([ ]|$)|AbzuGame-Win64-Shipping|scripts\/mr-run\.sh|cmake --build|(^|[ /])ninja([ ]|$)|(^|[ /])make([ ]|$)/ &&
    !/awk/ && !/run-once\.sh/ { print }' > "$OUT/PRE-RUN-PROCESSES.txt"
if [ -s "$OUT/PRE-RUN-PROCESSES.txt" ]; then
    echo "SERIALIZATION_BLOCKED=1" > "$OUT/RUNTIME-STATUS.txt"
    exit 90
fi

: > "$OUT/ATTEMPT-CONSUMED"

unset MACRUNNER_HB_TRACE_SYNC_CAUSALITY
unset MACRUNNER_HB_TRACE_SYNC_CAUSALITY_BUDGET
unset MACRUNNER_HB_TRACE_SYNC_CAUSALITY_SIGNAL_BUDGET
unset MACRUNNER_HB_TRACE_WAIT_SEMANTIC
unset MACRUNNER_HB_TRACE_WAIT_SEMANTIC_BUDGET
unset MACRUNNER_HB_WAIT_OBJECT_OBSERVER
unset MACRUNNER_HB_TRACE_DIRECT_WAIT_ALERT
unset MACRUNNER_HB_TRACE_DIRECT_WAIT_ALERT_WAIT_BUDGET
unset MACRUNNER_HB_TRACE_DIRECT_WAIT_ALERT_ALERT_BUDGET
unset MACRUNNER_HB_TRACE_WAITADDR
unset MACRUNNER_HB_TRACE_WAITADDR_MACH
unset MACRUNNER_HB_TRACE_WAITADDR_STACK
unset MACRUNNER_HB_TRACE_GUEST_PC_MOVEMENT
unset MACRUNNER_HB_TRACE_GUEST_PC_MOVEMENT_BUDGET
unset MACRUNNER_HB_WWISE_FORCE_SIGNAL
unset MACRUNNER_HB_WWISE_FORCE_SIGNAL_AFTER_TIMEOUTS
unset MACRUNNER_HB_DISABLE_KERNEL32_HANDLE_SYNC_SEMANTIC
unset MACRUNNER_HB_DISABLE_WAIT_ADDRESS_SEMANTIC

export MACRUNNER_GRAPHICS_BACKEND=dxmt
export MACRUNNER_PREFIX_SYSTEM32_ARCH=x86_64-windows
export MACRUNNER_MR_RUN_USE_WARM_PREFIX=0
export MACRUNNER_MR_RUN_SKIP_WINEBOOT=1
export MACRUNNER_MR_RUN_START_SERVICES=1
export MACRUNNER_MR_RUN_REGSVR32_ACTXPRXY=1
export MACRUNNER_MR_RUN_WIRE_VULKAN_ICD=1
export MACRUNNER_MR_RUN_SERVICES_WAIT=6
export WINEDLLOVERRIDES=d3d11,dxgi,d3d10core,winemetal=n,b
export WINEMSYNC=1
export WINEDEBUG=-all,+sync
export MACRUNNER_HB_BACKEND=jit
export MACRUNNER_HB_CRT_CASE_FUSION=1
export MACRUNNER_D3D11_CREATEDEVICE_BOUNDARY_TRACE=1
export MACRUNNER_HB_MAIN_ENTRY_TRACE=1

env -0 > "$OUT/CHILD-ENV.bin"
date -u '+WRAPPER_START_UTC=%Y-%m-%dT%H:%M:%SZ' > "$OUT/RUNTIME-STATUS.txt"
(
    cd "$ROOT"
    scripts/mr-run.sh "$DIST" "$GAME" 90
) > "$OUT/run.log" 2>&1 &
wrapper_pid=$!
echo "WRAPPER_PID=$wrapper_pid" >> "$OUT/RUNTIME-STATUS.txt"

game_pid=""
for _ in $(seq 1 90); do
    game_pid=$(ps -axo pid=,args= | awk -v game="$GAME" '$0 ~ game && $0 ~ /wine/ { pid=$1 } END { if (pid) print pid }')
    [ -n "$game_pid" ] && break
    kill -0 "$wrapper_pid" 2>/dev/null || break
    sleep 1
done

if [ -n "$game_pid" ]; then
    echo "GAME_PID=$game_pid" >> "$OUT/RUNTIME-STATUS.txt"
    while kill -0 "$game_pid" 2>/dev/null; do
        etime=$(ps -p "$game_pid" -o etime= | tr -d ' ')
        age=$(python3 - "$etime" <<'PY'
import sys
s=sys.argv[1]; d=0
if '-' in s: ds,s=s.split('-',1); d=int(ds)
p=[int(x) for x in s.split(':')]
print((p[-1] if p else 0)+(p[-2]*60 if len(p)>1 else 0)+(p[-3]*3600 if len(p)>2 else 0)+d*86400)
PY
)
        if [ "$age" -ge 58 ]; then
            echo "SAMPLE_AGE=$age" >> "$OUT/RUNTIME-STATUS.txt"
            ps -M -p "$game_pid" -o pid=,ppid=,state=,etime=,time=,comm=,args= > "$OUT/HOST-THREADS.txt" 2>&1 || true
            sample "$game_pid" 4 -file "$OUT/HOST-SAMPLE.txt" > "$OUT/HOST-SAMPLE-COMMAND.txt" 2>&1 || true
            break
        fi
        sleep 1
    done
else
    echo "GAME_PID=NOT_FOUND" >> "$OUT/RUNTIME-STATUS.txt"
fi

wait "$wrapper_pid"
runner_rc=$?
echo "RUNNER_EXIT=$runner_rc" >> "$OUT/RUNTIME-STATUS.txt"
ps -axo pid=,etime=,command= | awk 'BEGIN { IGNORECASE=1 }
    /(^|[ /])wine(server)?([ ]|$)|AbzuGame-Win64-Shipping|scripts\/mr-run\.sh/ &&
    !/awk/ && !/run-once\.sh/ { print }' > "$OUT/POST-RUN-PROCESSES.txt"
exit 0
