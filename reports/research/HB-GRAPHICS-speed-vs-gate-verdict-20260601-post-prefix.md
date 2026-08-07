# HB Graphics Speed-vs-Gate Verdict — Post-Prefix Check (2026-06-01)

Run artifact: `reports/phase4-hollow-knight/run-20260601-post-prefix-speed-vs-gate900/`

Verdict: after the runner warm-prefix fix, the setupapi bootstrap gate is gone and the current
blocker is inside HyperBridge/JIT CPU execution, not a Win32 wait gate.

Evidence:
- `wineboot=0`, `setupapi,InstallHinfSection=0`; the disposable prefix starts from the warm template.
- `D3D11CreateDevice=0`, `GfxDevice=0`, `CreateSwapChain=0`, `Present=0` through the 900s timeout.
- `macrunner-hb-jit-fallback=0`, `JIT codegen failed=0`.
- Heartbeats stop changing after the 60s sample: 488 heartbeats total, last Unity RVA `0x5544b8`.
- Samples at 60/180/360/600/900s show the real `Hollow Knight.exe` process at ~99-100% CPU.
- A live macOS sample of that PID during the run landed in executable anonymous VM memory
  (`VM_ALLOCATE 0x87fff960000-0x87fffa60000`, sampled PC around `0x87fff9ce678`), consistent with a
  long-running JIT/native block rather than a kernel wait.

Wait-gate status from `run-20260601-post-prefix-waittrace240/`:
- `0xcba8b2` remains `WaitForSingleObject(GetCurrentProcess(), 0)` returning `WAIT_TIMEOUT`.
- `0x577f44` startup waits are released by matching `ReleaseSemaphore` calls.
- `0x577c92` worker waits are idle worker semaphores.

Next action:
Map the sampled anonymous native/JIT PC back to its guest block with `MACRUNNER_HB_TRACE_JIT_HOT_BLOCKS`
and/or `MACRUNNER_HB_TRACE_JIT_NATIVE_ADDR`, then fix or specialize that finite hot block family. Do
not patch wait/event semantics without new wait evidence.
