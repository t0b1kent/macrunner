# CLINE master brief — ARM64EC Part 2a: stand up xtajit64 lifecycle + PROVE BeginSimulation is reached

**Agent:** Cline on Codex 5.5
**Date:** 2026-05-29
**Type:** Bounded implementation + runtime-proof milestone. NOT the full x64 emulator.
**Prereq:** Part 1 = GREEN (`reports/research/ARM64EC-SPIKE-build-result-20260529.md`): our lld
links arm64x, ntdll exports `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`.
**Deliverable:** a pasted LOG LINE proving whether ARM64EC runtime plumbing reaches our
`BeginSimulation` with a real x64 RIP — OR the exact earlier blocker. Read to the end first.

---

## 0. ANTI-HANG RULES (READ FIRST — you have a known tendency to freeze)

1. **NEVER run a long command in foreground and wait.** Builds → background + logfile, poll with
   `tail -n 80 <log>`. Do NOT `tail -f`. Do NOT block.
2. **EVERY Wine / x86_64 PE run goes through `timeout`** (e.g. `timeout 90 ...`). An x64 PE under
   our half-wired emulator WILL likely hot-spin or hang at 100% CPU — that is EXPECTED here, so
   the timeout is mandatory, and after it fires you kill scoped (rule 5) and read the log.
3. **NEVER `cat`/`tail -f`/read a full trace log.** Use `tail -n 200` / `grep`.
4. **`WINEDEBUG=-all`** unless you deliberately need a channel; then scope it narrowly.
5. **Kill ONLY scoped by prefix:** `WINEPREFIX=<prefix> wineserver -k`. **NEVER** global
   `pkill -9 wine` / `killall wine` — Kimi runs Wine in parallel; you would destroy his work.
6. No interactive commands (`-i`, editors, pagers without `| cat`).
7. If a command exceeds expected wall-clock with no log progress → kill it, capture last 80 lines,
   report. Never sit waiting.

---

## 1. CONTEXT (so you can make judgment calls)

MacRunner runs x86_64 Windows on Apple Silicon **without Rosetta**: HyperBridge (our x86→ARM64
translator) behind a pure-ARM64 Wine 11, using the **ARM64EC** model — system DLLs run native
ARM, only the game's x64 code is emulated. Microsoft's reference emulator is `xtajit64.dll`;
Wine selects the emulator **by DLL name**, so OUR `xtajit64.dll` is the hook point.

Part 1 proved the **build** works (lld links the arm64x ntdll; required exports present). The
next unknown is **runtime**: when an x86_64 PE runs, does ntdll's `KiUserEmulationDispatcher`
actually hand control to our `xtajit64.dll!BeginSimulation` with a valid x64 context? Nobody has
observed this yet because `xtajit64/cpu.c` is a stub that just `ERR`s and kills the process.

### How the handoff works (already verified in the tree — do not re-derive)
`dlls/ntdll/signal_arm64ec.c`:
- `get_arm64ec_cpu_area()` = `NtCurrentTeb()->ChpeV2CpuAreaInfo` (a `CHPE_V2_CPU_AREA_INFO *`).
- `KiUserEmulationDispatcher` (line ~1246) does:
  ```c
  context_arm_to_x64( get_arm64ec_cpu_area()->ContextAmd64, arm_ctx );
  get_arm64ec_cpu_area()->InSimulation = 1;
  pBeginSimulation();          /* <- calls xtajit64.dll!BeginSimulation, NO args */
  ```
So our `BeginSimulation()` takes **no arguments** and must read the x64 context itself from
`NtCurrentTeb()->ChpeV2CpuAreaInfo->ContextAmd64` (an AMD64 `CONTEXT *` with Rip/Rsp/Rax/…).

---

## 2. SCOPE — what you MAY and MUST NOT touch

**MAY edit (only this):** `engine/wine/dlls/xtajit64/cpu.c`.
- If you genuinely need a header for `CHPE_V2_CPU_AREA_INFO` / amd64 `CONTEXT`, include existing
  Wine headers (`winternl.h`, `winnt.h`, `ddk/wdm.h` as the 32-bit side does). Do NOT invent new
  private headers for 2a unless a build error forces it; if so, keep it inside `xtajit64/`.
- You MAY adjust `xtajit64/Makefile.in` ONLY if the build needs it (unlikely for 2a).

**MUST NOT touch (hard boundaries):**
- **HyperBridge core** — `hb_decode_x64.c`, `hb_lift_x64.c`, `hb_interpreter.c`, `hb_*` anything.
  You are NOT wiring the actual x64 execution in 2a. That is Part 2b, reserved for Codex/Opus.
  Do not even call them yet.
- **`dlls/xtajit/` (the 32-bit module)** — it works; it's your structural reference (READ-ONLY).
- **`dlls/ntdll/signal_arm64ec.c`** and the rest of ntdll — read-only reference.
- **The baseline build script** `scripts/build-wine-pure-arm64-experiment.sh` — untouched.
  Build with the Part-1 spike script `scripts/build-wine-arm64ec-spike.sh` into the existing
  `engine/wine/build-arm64ec-spike` / `dist-arm64ec-spike`.
- **`engine/graphics/**`** — Kimi's lane, hands off.
- **No commits** unless the operator says so.

---

## 3. WHAT TO IMPLEMENT IN `xtajit64/cpu.c` (2a only)

Goal: make the emulator DLL *survive* lifecycle + capture/log the x64 context, instead of
ERR-and-die. Mirror the LIFECYCLE STRUCTURE of the working 32-bit `dlls/xtajit/cpu.c`
(BTCpuProcessInit/ThreadInit/etc.) — but the x64 entry mechanism differs (no BOP; entry via
`BeginSimulation`), so structure-mirror, not copy.

1. **`ProcessInit` / `ThreadInit`** — keep returning `STATUS_SUCCESS`, but add a `MESSAGE(...)`
   log so we can see lifecycle ordering at runtime (e.g. `"macrunner-xtajit64: ProcessInit\n"`).
2. **`UpdateProcessorInformation` / `BTCpu64IsProcessorFeaturePresent`** — already fine; leave.
3. **`BeginSimulation`** — replace the `ERR(...) + NtTerminateProcess` with:
   - `CHPE_V2_CPU_AREA_INFO *cpu = NtCurrentTeb()->ChpeV2CpuAreaInfo;`
   - read the AMD64 context: `cpu->ContextAmd64` (cast to the amd64 `CONTEXT *`).
   - `MESSAGE` log the proof: `Rip`, `Rsp`, `Rax`, `Rcx`, `Rdx`, `InSimulation` —
     e.g. `"macrunner-xtajit64: BeginSimulation REACHED rip=%p rsp=%p rax=%p\n"`.
   - For 2a, do NOT execute anything. After logging, terminate cleanly with a **unique marker**
     exit code so we know we got here: `NtTerminateProcess( GetCurrentProcess(), 0x6502 );`
     (add a comment: "2a: plumbing-reach proof only; real exec = Part 2b → HyperBridge x64").
4. **`DispatchJump` / `RetToEntryThunk` / `ExitToX64`** — replace silent ERR-death with a
   `MESSAGE` naming which thunk was hit + then terminate with a distinct marker
   (e.g. `0x6503/4/5`). This tells us if a transition thunk fires before BeginSimulation.

Keep it minimal and readable. No HyperBridge calls. No execution loop.

---

## 4. BUILD + RUN + PROVE

1. **Build** the spike tree in background:
   ```bash
   cd /Users/timurtoby/Documents/MacRunner/Main/MacRunner
   WINEDEBUG=-all nohup bash scripts/build-wine-arm64ec-spike.sh > /tmp/arm64ec-2a-build.log 2>&1 &
   ```
   Poll `tail -n 60 /tmp/arm64ec-2a-build.log`; confirm it ends with "installed" and
   `grep -c "ld.lld: error\| Error [0-9]"` == 0. Confirm `xtajit64.dll` is in the dist
   (`find dist-arm64ec-spike -name xtajit64.dll`).
2. **Pick the smallest x86_64 PE test** available (e.g. an existing
   `dx11_clear_present_x64.exe` or any tiny x64 `.exe` in `artifacts/`/`engine/.../tests`).
   Smaller/simpler is better — you only need it to start and trigger emulation.
3. **Set up a scoped prefix** pointing at the spike dist, ensure `xtajit64.dll` is the
   registered emulator (it is the default name; verify via the loader path / registry
   `HKLM\Software\Microsoft\Wow64\amd64` if needed). Use a DEDICATED prefix dir so you can
   `wineserver -k` it without touching anyone else.
4. **Run under `timeout`** with the emulator's MESSAGE channel visible (MESSAGE prints
   regardless of WINEDEBUG), capture to a log:
   ```bash
   timeout 90 env WINEPREFIX=<spike-prefix> WINEDEBUG=-all <spike-dist>/bin/wine <x64test>.exe \
       > /tmp/arm64ec-2a-run.log 2>&1; echo "exit=$?"
   WINEPREFIX=<spike-prefix> <spike-dist>/bin/wineserver -k   # scoped cleanup
   ```
5. **Inspect** `grep -niE "xtajit64|BeginSimulation|ProcessInit|ThreadInit|REACHED|EmulationDispatcher" /tmp/arm64ec-2a-run.log | head`.

---

## 5. THE DELIVERABLE (this is what "done" means — NOT a checklist)

Write `reports/research/ARM64EC-2A-begin-simulation-result-20260529.md` AND give the operator a
short summary answering, with **pasted log lines as evidence**:
- Did the build succeed and is `xtajit64.dll` present in the spike dist?
- At runtime, what is the **furthest point reached**? One of:
  - (best) `BeginSimulation REACHED rip=… rsp=…` → paste the line. Plumbing works end-to-end.
  - a transition thunk fired first (DispatchJump/etc.) → paste it.
  - blocked BEFORE our DLL (loader/ntdll error, never reached ProcessInit) → paste the error +
    where. This is still a valid, useful result — it names the next blocker.
- The process exit code (did our `0x6502` marker fire?).

⚠️ **Do NOT report "BeginSimulation wired ✅" as a status.** The verdict is the pasted log line
showing what actually happened at runtime. (In Part 1 the discipline checklist was filled but the
real verdict — the export check — was missing; do not repeat that. Evidence, not status.)

If the honest answer is "blocked before BeginSimulation at X" — that is a SUCCESS for this
milestone, because it tells us exactly what to fix next. Do not fake forward progress.

## 6. OUT OF SCOPE (Part 2b — do NOT start)
Creating `xtajit64/unixlib.c`, an `xtajit64_private.h` simulate ABI, packing the amd64 context
into HyperBridge, and calling `hb_decode_x64`/`hb_lift_x64`/`hb_interpreter` to actually execute
x64 — that is the heavy engine wire, reserved for Codex/Opus. Your 2a job ends at proving (or
disproving) that the runtime reaches `BeginSimulation`.
