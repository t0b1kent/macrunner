# HB Graphics Speed-vs-Gate Verdict - 2026-06-01

## Experiment

- Run artifact: `reports/phase4-hollow-knight/run-20260601-hk-dxmt-speed-vs-gate900/`
- Command shape: `scripts/mr-run.sh engine/wine/dist-arm64ec-spike Hollow Knight.exe 900 -- -logFile -`
- Env: `MACRUNNER_GRAPHICS_BACKEND=dxmt`, `MACRUNNER_HB_TRACE_HEARTBEAT=1`,
  `MACRUNNER_HB_TRACE_WAIT_SEMANTIC=1`, `WINEDEBUG=+loaddll,+module,+d3d11,+dxgi`
- Result: timeout `143`, cleanup/prune `0`.

## Verdict

This is a gate, not throughput. Stop blind JIT loop-warming until the gate is explained.

Evidence:

- Real D3D11 device-create calls: `0`.
  `d3d11-real.txt` is empty after filtering out thunk/import setup lines
  (`redirected|registered|rewrote|import thunk`). In this run even raw
  `D3D11CreateDevice` string count is `0`.
- Renderer markers: `GfxDevice=0`, `D3D11CreateDevice=0`, `CreateSwapChain=0`.
- JIT/runtime fault counters stayed clean for the path that matters:
  `macrunner-hb-jit-fallback=0`, `JIT codegen failed=0`, `JIT helper fault=0`,
  `MEMORY_FAULT=0`, `UNSUPPORTED_OPCODE=0`, `c000007b=0`.
- Progress plateaued by the first 60s sample and stayed there through timeout:
  `trace_bytes=9876573`, `blocks=56205`, `steps=1e7b0e`, `rva=0x4e7e60`,
  `real_d3d11_count=0` at 60s, 121s, 181s, 242s, 302s, 363s, 423s, 484s,
  544s, 604s, 665s, 725s, 786s, 846s, 907s. Final 967s only gained the
  mr-run timeout line (`trace_bytes=9876621`), not guest progress.
- Wait callers map to known Unity wait sites:
  `0xcba8b2` zero-timeout poll (`18` samples), `0x577f44` infinite wait
  (`14` samples), `0x577c92` infinite wait (`7` samples). Wait trace stopped at
  `wait_before=23`, `wait_after=16`, matching the frozen heartbeat/progress.
- Process CPU sample during the run showed Hollow Knight, wineserver, and
  services at `0.0%`, consistent with parked wait state rather than slow CPU-bound
  translation.

## Next Action

Treat UnityPlayer RVA `0xcba8b2` as the active gate first. The worker waits at
`0x577c92/0x577f44` match prior worker parking evidence; the main zero-timeout poll
is the likely scheduler/init condition preventing renderer startup. Diagnose what
handle/event/timer/result the poll expects, then fix the missing semantic or thread
state at that root. Do not continue optimizing `0x283xxx` / `0x19d4xxx` loops until
this gate is resolved or falsified.
