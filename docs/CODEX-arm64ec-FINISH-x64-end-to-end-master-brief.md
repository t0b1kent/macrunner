# CODEX MASTER BRIEF — finish ARM64EC x64 END-TO-END (run a plain x86_64 PE to exit)

**Agent:** Codex / Opus (engine lane — widest scope, no hand-holding)
**Date:** 2026-05-30
**Mission (one line):** make a plain **x86_64 PE run to its own clean exit on our engine** — no
Rosetta, no FEX — via the ARM64EC path (arm64x-native system DLLs + HyperBridge-emulated game x64).
**This is the finish-line task.** Multi-phase. Each phase ends in a PASTED-LOG milestone, not a
status checklist. Honest partials are success.

---

## 0. STATE (verified — do not re-derive)
- **Part 1 GREEN:** lld links arm64x; `aarch64-windows/ntdll.dll` exports
  `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`.
- **2b GREEN-ish:** `x86_64` arch now builds (`x86_64-windows/ntdll.dll` present); `c0000135`
  gone; the run boots the x64 loader and installs the base-thread bridge. **First-ever x64
  execution happened** (HyperBridge interp `steps=7`).
- **The wall:** `ProcessInit`/`ThreadInit`/`BeginSimulation` are NOT invoked for the plain-x64 PE.
  The `steps=7` were the interpreter being fed **ARM64-native arm64x bytes** (kernelbase/kernel32
  `__arm64x_native_entrypoint` = `b DllMain`) — i.e. the EC boundary is not yet enforced.
- Your wire (`dlls/xtajit64/{cpu.c,unixlib.c,xtajit64_private.h}`) was reviewed and is sound; the
  blocker is the **loader EC-entry boundary**, not the wire.
- Full evidence: `reports/research/ARM64EC-2B-execution-result-20260529.md` +
  `ARM64EC-2A-ROOT-CAUSE-c0000135-no-x86_64-arch-20260529.md`.

## 1. THE KEY INSIGHT — TWO ENTRY PATHS, and the x64 PE must take the EC one
In `dlls/ntdll/loader.c`:
- `#ifdef __arm64ec__ → load_arm64ec_module()` (≈line 7083) loads `xtajit64.dll` and calls
  `arm64ec_process_init(wm->ldr.DllBase)` — **THIS is what invokes xtajit64's `ProcessInit`** and
  wires the EC dispatcher. It is compiled ONLY into the `__arm64ec__` (EC) view of ntdll.
- The native **aa64** ntdll that is currently driving the run does NOT execute `load_arm64ec_module`,
  so xtajit64 is mapped but never initialized → no `ProcessInit`/`BeginSimulation`.
- On Windows-on-ARM, a plain x64 PE runs **via the ARM64EC emulator** (arm64x ntdll EC view +
  `KiUserEmulationDispatcher` → `xtajit64.BeginSimulation`), NOT via classic i386-style wow64.
- **So the root fix is loader-side:** for a plain-x64 image on the aa64 host, bring the process up
  in the ARM64EC environment so `load_arm64ec_module`/`arm64ec_process_init` run and the dispatcher
  routes x64 RIPs into `xtajit64.BeginSimulation`. (This is exactly your own 2b diagnosis.)

## 2. REFERENCE DISCIPLINE — when stuck, READ the working code (operator's instruction)
- **32-bit `dlls/xtajit` WORKS — it runs real PE32 today.** It is your ORACLE for the
  **execution side**: `unixlib.c` (HyperBridge import/simulate/export, memory notify), the
  simulate loop shape, `ProcessInit`/`ThreadInit` bodies, `status_from_hb`, the HyperBridge call
  pattern. When the x64 exec path misbehaves, **diff your xtajit64 against xtajit** and match it.
  Byte-identical backup: `reports/backups/xtajit-32bit-reference/xtajit-cpu.c....reference`.
- **BUT the ENTRY boundary has NO 32-bit analog.** 32-bit enters via wow64 + BOP-code
  (`BTCpuGetBopCode`, `Wow64Transition`); x64-on-ARM64 enters via **ARM64EC +
  `KiUserEmulationDispatcher`** (no BOP). For the entry/loader-EC work, the references are
  `load_arm64ec_module` + `arm64ec_process_init` (loader.c) and `KiUserEmulationDispatcher` +
  `get_arm64ec_cpu_area()` (`signal_arm64ec.c`). Don't try to copy the wow64 BOP path for x64.
- Net: **mine 32-bit for the HyperBridge/exec half; mine the arm64ec/EC code for the entry half.**

## 3. PHASES (each = a pasted-log milestone)

### Phase 1 — reach the emulator lifecycle (THE current wall)
Get the plain-x64 PE to bring up the ARM64EC environment so xtajit64 is initialized and the
dispatcher fires. Diagnose why the aa64 x64-loader path does not reach
`load_arm64ec_module`/`arm64ec_process_init`; fix the routing so it does.
**Milestone:** logs show `macrunner-xtajit64: ProcessInit …` then `ThreadInit …` then
`macrunner-xtajit64: BeginSimulation REACHED rip=… rsp=…` with a real x64 RIP.

### Phase 2 — enforce the EC boundary + implement the transition thunks
The emulator must run ONLY the game's x64 code; arm64x system DLLs (kernel32/kernelbase/ntdll/…)
run **native ARM64**. The `steps=7`-on-ARM64-bytes bug is the boundary leaking.
- Use `is_ec_code_ptr` / `PEB->EcCodeBitMap` (you already have it in unixlib) to stop emulating at
  the first non-EC (native) target, and hand off via the transition thunks.
- Implement `DispatchJump` / `RetToEntryThunk` / `ExitToX64` (currently they MESSAGE + kill with
  0x6503/4/5) as the real x64↔ARM64 transitions (`__os_arm64x_*` semantics: target in `x9`,
  `RtlIsEcCode` → native entry-thunk vs re-enter emulator). Lean on the arm64x native-entry
  dispatch you started in loader.c (`macrunner_hb_arm64x_native_dispatch_ret` etc.).
**Milestone:** x64 game code executes (`steps>0` on REAL x64 bytes, not arm64x), calls a native
ARM64 API, returns, and continues — no UNSUPPORTED_OPCODE on `b DllMain` thunks.

### Phase 3 — run to exit
`tests/native-fixtures/build/hello_x64.exe` runs to its own exit (`run_exit` = the program's
code, typically 0), not a `timeout`(124) hot-spin. Then try one slightly larger x64 fixture.
**Milestone:** clean exit + the program's observable effect (stdout / exit code) in the log.

## 4. SCOPE (corrected + expanded from the earlier briefs)
**You MAY edit:**
- `dlls/xtajit64/*` (cpu.c, unixlib.c, xtajit64_private.h, Makefile.in).
- **HyperBridge core** (`hb_*_x64.c`, `hb_interpreter.c`, `hb_abi_x64.c`, headers).
- **`dlls/ntdll/loader.c` + `dlls/ntdll/ntdll_misc.h`** — explicitly blessed now (the EC-entry fix
  lives here). Earlier briefs said "ntdll read-only"; that was wrong — this work requires loader.c.
  Backups already archived at `reports/backups/ntdll-loader-codex-2b/`.
- `dlls/ntdll/signal_arm64ec.c` — edit ONLY if strictly required for the EC dispatcher wiring;
  keep it minimal and call it out in the report.
**You MUST NOT:**
- **Regress the working i386 + aarch64 paths.** They are BOTH production AND your reference. After
  any loader.c change, sanity-check that a PE32 still runs (the 32-bit xtajit path) — paste proof.
- Touch the **baseline build script** `scripts/build-wine-pure-arm64-experiment.sh`, the **golden
  x64 snapshot** (read-only oracle — escalate if it seems to need changing), or **`engine/graphics/**`** (Kimi).
- **Commit** without the operator asking.

## 5. RUNTIME DISCIPLINE (you hit the MCP 120s ceiling — obey this)
Each context-mode/MCP tool-call has a hard ~120s ceiling; a full Wine build (minutes) or a
hot-spinning run NEVER fits.
- **Build → background + poll:** `nohup bash scripts/build-wine-arm64ec-spike.sh > /tmp/log 2>&1 &`
  (returns instantly), then poll in SEPARATE short calls with `tail -n 80 /tmp/log`.
- **Run → `timeout 120 … > /tmp/run.log 2>&1`** + redirect; on timeout, **scoped** cleanup
  `WINEPREFIX=<p> <dist>/bin/wineserver -k` — NEVER global `pkill/killall wine` (Kimi runs Wine in
  parallel). `/tmp` may reject prefixes on this host — use a prefix under `artifacts/` (as in 2b).
- Never stream MB of trace back through one call; redirect to a file and grep bounded windows.

## 6. DELIVERABLE
Maintain `reports/research/ARM64EC-FINISH-x64-result-20260530.md`, updated per phase, each with:
the milestone reached + PASTED log lines as evidence + the next blocker named. Evidence, not
status. "Reached BeginSimulation; x64 executes N steps; blocked at <thunk/opcode/API> @ RIP <…>"
is a real win at any phase. Do NOT claim done until `hello_x64.exe` exits cleanly with proof.
```
