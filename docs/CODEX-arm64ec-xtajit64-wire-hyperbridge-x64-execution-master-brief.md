# CODEX master brief — ARM64EC: wire xtajit64 → HyperBridge x64 (the real emulator)

**Agent:** Codex / Opus (engine lane)
**Date:** 2026-05-29
**Type:** Core engine implementation. The actual x64 execution wire. Peer-level — no hand-holding.
**One-line mission:** make a plain x86_64 PE *execute* under our emulator — stand up the xtajit64
**unix side** and the BeginSimulation→HyperBridge-x64 loop, mirroring the WORKING 32-bit `xtajit`.

---

## 0. WHERE WE ARE (so you can make calls without re-deriving)
- **Part 1 = GREEN:** our lld links arm64x; `aarch64-windows/ntdll.dll` exports
  `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`.
  (`reports/research/ARM64EC-SPIKE-build-result-20260529.md`)
- **2a runtime:** reached the loader but died at `c0000135`. Root-caused
  (`reports/research/ARM64EC-2A-ROOT-CAUSE-c0000135-no-x86_64-arch-20260529.md`): the spike build
  lacked `x86_64`; a plain-x64 PE needs the WoW64 x64-guest ntdll. `xtajit64.dll` itself maps fine.
- **2b (Cline, in flight):** adds `x86_64` to `--enable-archs` and proves whether runtime now
  reaches our `xtajit64.dll!BeginSimulation` (cpu.c currently logs + exits `0x6502`, no execution).
  (`docs/CLINE-arm64ec-part2b-rebuild-x86_64-arch-reach-beginsimulation-brief.md`)
- **THIS task = everything AFTER BeginSimulation is reached:** actually run the x64 code. You can
  build the machinery now in parallel; it goes live the moment 2b confirms the reach.
- Implementation contract from research: `reports/research/ARM64EC-XTAJIT64-IMPL-chatgpt-20260529.md`.

## 1. THE HANDOFF MECHANISM (verified in-tree, do not re-derive)
`dlls/ntdll/signal_arm64ec.c`:
- `get_arm64ec_cpu_area()` = `NtCurrentTeb()->ChpeV2CpuAreaInfo` (`CHPE_V2_CPU_AREA_INFO *`).
- `KiUserEmulationDispatcher` does:
  `context_arm_to_x64(get_arm64ec_cpu_area()->ContextAmd64, arm_ctx); cpu_area->InSimulation=1; pBeginSimulation();`
- So **`BeginSimulation()` takes NO args** and must read the amd64 `CONTEXT *` from
  `NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64` (Rip/Rsp/Rax/Rcx/…), run x64 until it hits an
  ARM64/EC address, write the resulting state back into that same `ContextAmd64`, clear
  `InSimulation`, and return. (Unlike 32-bit xtajit there is NO BOP-code; entry is via the
  dispatcher + transition thunks.)

## 2. THE WORKING TEMPLATE — mirror 32-bit `dlls/xtajit` (READ-ONLY reference)
The 32-bit module already routes BTCpu → HyperBridge and runs real PE32. Mirror its STRUCTURE for
x64. Key pieces (already indexed; paths below):
- **`dlls/xtajit/xtajit_private.h`** — the PE↔unix ABI: `enum xtajit_unix_funcs`
  (process_init, thread_init, thread_term, process_term, **simulate**, notify_memory_*, map/unmap,
  flush_icache), `struct xtajit_i386_context`, `struct xtajit_simulate_params`
  { context, max_code_bytes, status, hb_result, faulted, steps, blocks }.
- **`dlls/xtajit/cpu.c`** (PE side) — `BTCpuSimulate_impl` packs the i386 context into
  `xtajit_simulate_params` and calls `xtajit_unix_call(unix_simulate, &params)` (=
  `__wine_init_unix_call()` then `WINE_UNIX_CALL`).
- **`dlls/xtajit/unixlib.c`** (unix side) — `unix_simulate_impl` flow you will mirror for x64:
  1. `ensure_thread()` (→ `ensure_process()`)
  2. `import_context(&in, &params->context)` (PE struct → `hb_wow64_i386_context_t`)
  3. `hb_wow64cpu_import_i386_context(&thread, &in)`
  4. **`hb_wow64cpu_simulate(&thread, HB_BACKEND_INTERP, max_code_bytes?:4096, &exec)`** ← the run
  5. export back; set `params->status/faulted/steps/blocks`.
  `__wine_unix_call_funcs[]` maps the enum to the `unix_*_impl` array (C_ASSERT count match).
- A **byte-identical archived copy** of the 32-bit cpu.c is at
  `reports/backups/xtajit-32bit-reference/xtajit-cpu.c.20260529-164803.reference`.

## 3. HYPERBRIDGE x64 BUILDING BLOCKS (in `engine/hyperbridge/src`)
- `hb_decode_x64.c`, `hb_lift_x64.c`, `hb_interpreter.c`, `hb_abi_x64.c` (+ `hb_abi.h`,
  `hb_memory.h`, `hb_runtime.h`).
- `hb_context_t` carries `regs.x64.{rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15,rip…}`, `mode`
  (`HB_MODE_64BIT`), `memory`, `pc`.
- `hb_interpreter_create(ctx)` / `hb_interpreter_run(interp, func, &exec_result)` / `_destroy`.
- `hb_abi_x64_call(ctx, target, call, out)` — runs an x64 CALL under the Win64 ABI (shadow space,
  16-byte stack alignment) — useful for entry-thunk/call semantics.
- **The gap to close (your call):** the 32-bit side has a high-level `hb_wow64cpu_simulate`
  (import context → run interp until boundary → export). There is likely **no x64 analog yet**.
  Decide: add an `hb_x64cpu_simulate(...)`-style entry to HyperBridge (your lane), OR drive
  `hb_interpreter_run` + decode/lift directly from `unix_simulate_impl`. Keep the boundary
  condition explicit: stop when the next target is an ARM64/EC address (use `RtlIsEcCode`-style
  test on the PE side / an address-class check), export, return.

## 4. WHAT TO BUILD
Create the xtajit64 unix side + finish the PE side:
1. **`dlls/xtajit64/xtajit64_private.h`** — the x64 analog of `xtajit_private.h`:
   `enum` of unix funcs, `struct xtajit64_amd64_context` (or reuse an amd64 `CONTEXT` subset:
   Rip/Rsp/Rbp/Rax/Rcx/Rdx/Rbx/Rsi/Rdi/R8–R15/EFlags/segregs/XMM as needed),
   `struct xtajit64_simulate_params { context; max_code_bytes; status; hb_result; faulted; steps; blocks; }`.
2. **`dlls/xtajit64/unixlib.c`** (NEW; `#pragma makedep unix`) — mirror `xtajit/unixlib.c`:
   `unix_process_init/thread_init/.../simulate/...` + `__wine_unix_call_funcs[]` with the C_ASSERT.
   `unix_simulate_impl` = the x64 run (section 3).
3. **`dlls/xtajit64/cpu.c`** (PE side) — replace the 2a logging stubs:
   - `BeginSimulation`: read `NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64`, pack into
     `xtajit64_simulate_params`, `WINE_UNIX_CALL(unix_simulate, &params)`, write results back into
     `ContextAmd64`, return. (Keep the existing MESSAGE log + a marker for "executed N steps".)
   - `ProcessInit`/`ThreadInit` → call the unix init (like 32-bit), keep STATUS_SUCCESS.
   - `DispatchJump`/`RetToEntryThunk`/`ExitToX64` → implement the x64↔ARM64 transition thunks
     (target in `x9`, `RtlIsEcCode` → ARM64 entry-thunk vs re-enter emulator). Per the impl
     contract these are `__os_arm64x_*` analogs.
   - `UpdateProcessorInformation`/`BTCpu64IsProcessorFeaturePresent` are already fine.
4. **`dlls/xtajit64/Makefile.in`** — add `unixlib.c`, the `EXTRADLLFLAGS`/unix wiring like
   `dlls/xtajit/Makefile.in`. **`dlls/xtajit64/xtajit64.spec`** already exports the right names.

## 5. SCOPE — boundaries (you have the widest lane, but these are hard)
**You MAY:** create `dlls/xtajit64/{unixlib.c,xtajit64_private.h}`, edit `dlls/xtajit64/{cpu.c,
Makefile.in}`, and edit **HyperBridge core** (`hb_*_x64.c`, `hb_interpreter.c`, `hb_abi_x64.c`,
their headers) — e.g. to add the x64 simulate entry. This is the one task allowed in HyperBridge.
**You MUST NOT:**
- Break the **32-bit `dlls/xtajit`** path — it runs real PE32 today; it is the reference. If you
  refactor shared HyperBridge code, keep `hb_wow64cpu_*` / the i386 path working (regression-check).
- Edit the **baseline** build script `scripts/build-wine-pure-arm64-experiment.sh`. Use the spike
  script (`build-wine-arm64ec-spike.sh`, now with `x86_64` after 2b).
- Edit the **golden x64 snapshot** (read-only oracle) — escalate if you think it must change.
- Touch **`engine/graphics/**`** (Kimi) or **`dlls/ntdll/signal_arm64ec.c`** beyond reading.
- **Commit** without the operator asking.

## 6. RUNTIME / VERIFICATION
- Build the spike tree (`x86_64` arch present). Run under `timeout` in a **dedicated prefix**;
  kill **scoped** only (`WINEPREFIX=<p> <dist>/bin/wineserver -k`) — never global `pkill wine`
  (Kimi runs Wine in parallel). x64 may hot-spin → timeout mandatory.
- **Target:** `tests/native-fixtures/build/hello_x64.exe` actually executes x64 — HyperBridge
  `exec.steps_executed > 0`, then a clean transition (program exit / ARM64 return), NOT
  `UNSUPPORTED_OPCODE`-death at instruction 0. Best proof: the program runs to its own exit.
- **Verdict = evidence:** paste the `macrunner-xtajit64`/hb exec log showing steps executed +
  exit, OR the first real opcode/ABI gap with its RIP. "Reached execution, blocked at opcode X /
  RIP Y after N steps" is a valid, useful result. Not a status checklist.

## 7. COORDINATION
- Gated on / parallel with Cline's 2b (x86_64 arch + BeginSimulation reach-proof). Develop the wire
  now; integrate once 2b is green. If 2b shows BeginSimulation is NOT reached even with x86_64, ping
  operator before going deeper — the blocker would be loader-layer, not emulator-layer.
- Opcode gaps you hit feed the existing x64 opcode-coverage campaign (game x64 code is always
  emulated); CRT/ntdll opcodes are less urgent under arm64ec (system DLLs native).
