# ARM64EC FINISH x64 result - 2026-05-30

## Status

DONE for the master-brief gate: a plain x86_64 PE runs through the ARM64EC/xtajit64 path to its own clean exit.

Primary proof:
- `reports/arm64ec-finish-run-final-20260530-104000.log`
- `hello from windows pe`
- `run_exit=0`
- `cleanup_exit=0`

Larger follow-up fixture:
- `reports/arm64ec-finish-run-stdoutstderr-20260530-104127.log`
- `stdout ok`
- `stderr ok`
- `run_exit=0`
- `cleanup_exit=0`

Build proof:
- `/tmp/arm64ec-finish-build-final-20260530-103935.log`
- `arm64ec spike installed: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/wine/dist-arm64ec-spike`

HyperBridge opcode-family proof:
- `/tmp/hb-x87-family-final-20260530-104041.log`
- `HyperBridge C test runner`
- `10 passed, 0 failed`

## What changed

### xtajit64 entry path

The previous direct calls into `xtajit64.dll` PE exports were removed. `ntdll/loader.c` now routes x64 startup through the native xtajit64 unixlib surface:
- `ProcessInit`
- `ThreadInit`
- `BeginSimulation`

This avoids calling ARM64EC/x64 PE bytes as native ARM64 code.

### Builtin unixlib binding

`ntdll/unix/loader.c` now special-cases `xtajit64.dll` for `MACRUNNER_HB_X64_LOADER` so `MemoryWineUnixFuncs` resolves to the current-machine builtin unixlib. This fixed the earlier `c0000135` unixlib lookup failure.

### x64 import handoff

Added `unix_macrunner_hb_x64_import_context` to let xtajit64 stop on old HyperBridge import thunks, run the import through the native bridge, copy register state back, and resume x64 execution.

Implemented required x64 startup semantics:
- CRT onexit/atexit/exit family avoids storing x64 callbacks in native ARM64 CRT tables.
- UCRT narrow/wide environment init returns successful x64-side startup status.
- `SetUnhandledExceptionFilter` stores the guest x64 filter on the x64 side.
- `GetStdHandle` reads PEB process parameters.
- `WriteFile` uses `NtWriteFile` and writes the live x64 out-parameter via `NtWriteVirtualMemory`.

### x87 opcode family

Family: x87 environment control
Members covered:
- `9B` FWAIT, already decoded as NOP
- `DB E2` FNCLEX, added for x64 decode/lift as NOP
- `DB E3` FNINIT, added for x64 decode/lift as NOP
- `9B DB E2` and `9B DB E3`, covered as FWAIT plus sibling instruction

Trigger:
- `DB E3` at `hello_x64.exe` RIP `0x1400019b0`

Regression tests:
- `decode_x64_x87_environment_control_family`
- `interp_x64_x87_environment_control_family`
- existing x86 x87 family tests in `--fast-family x87`

Audit completed: yes.

## Evidence timeline

Detailed import-chain proof from `reports/arm64ec-finish-run-exit-20260530-103749.log`:
- `ProcessInit status=00000000`
- `ThreadInit status=00000000`
- `BeginSimulation REACHED rip=00000001400013D0`
- `KERNEL32.dll!WriteFile ... result=OK`
- `hello from windows pe`
- `api-ms-win-crt-runtime-l1-1-0.dll!exit`
- `run_exit=0`

## Notes

The final concise smoke was run without detailed import tracing after cleanup. To re-enable the import-chain log, set:

`MACRUNNER_HB_TRACE_XTAJIT64_IMPORT=1`

No files under `engine/graphics/**` were edited.
