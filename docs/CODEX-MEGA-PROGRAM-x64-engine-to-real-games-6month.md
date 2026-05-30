# CODEX MEGA-PROGRAM — from "x64 hello-world runs" to "MacRunner runs real games"

**Agent:** Codex / Opus (engine lane — widest scope)
**Date:** 2026-05-30
**Horizon:** ~6 months. This is a PROGRAM, not a task. Phased, gated, evidence-driven.
**Where we start (DONE, verified):** a plain x86_64 PE (`hello_x64.exe`, `stdout_stderr_x64.exe`)
runs end-to-end through the ARM64EC/xtajit64 path to `run_exit=0`, no Rosetta/FEX; system DLLs
native ARM64, only game x64 emulated by HyperBridge. x87 family 10/10. Spike build green.
**Where we finish:** the Tier-1 green-list games are playable on MacRunner, fast and stable.

**SCOPE CHANGE (2026-05-30): Codex owns EVERYTHING now.** Kimi is sidelined for this effort, so
graphics (DXMT/DX11→Metal) is YOUR lane too — you continue and finish it, not just a contract.
Audio is already largely built (`engine/wine/dlls/xaudio2_*`, `x3daudio*`, `winecoreaudio.drv`,
`faudio`) → integrate + verify, don't rebuild. DXMT is well-started (`engine/dxmt/` =
winemetal/nativemetal/airconv; `engine/graphics/` = metal_ir/shader_ingest/runtime_backend;
`engine/vkd3d` Metal shader runner) → continue it. The store/launcher layer remains separate
(out of scope here).

---

## ⚡ AUTONOMY — RUN CONTINUOUSLY, DO NOT STOP BETWEEN PHASES (read first)
- **Chain all phases 0 → 5 without pausing for approval.** Do NOT stop to report "Phase N done,
  may I continue?" — the answer is always YES. Finish a phase, log its evidence, immediately start
  the next. Keep going until the LAST phase is done or you hit a true hard blocker.
- **Gates are SELF-CHECKS, not approval stops.** A gate means: prove the milestone with pasted
  evidence in the living report, then proceed. You do NOT wait for a human reply at a gate.
- **Update the living report `reports/research/CODEX-MEGA-PROGRAM-status.md` as you pass each gate**
  — that is how the operator follows along ASYNCHRONOUSLY. Writing the report ≠ stopping.
- **Only stop / escalate to the operator for:** (a) a genuine hard blocker you cannot resolve after
  real attempts (paste what you tried), (b) a decision that changes product direction or needs a
  human/external resource (signing, a paid asset, a legal call), (c) risk of regressing the
  shipped milestone. Everything else: keep working.
- **Work in long autonomous stretches.** Respect the MCP 120s rule (background builds + poll,
  timeout+redirect runs) so a long run never stalls on one blocked call. If you approach a context
  limit, write your state to the living report so you can resume, then continue.
- Phase 0 starts with a ready diagnosis:
  `reports/research/ARM64EC-PHASE0-c000007b-tls-callback-misaddress-diagnosis.md`.

## CROSS-CUTTING RULES (apply to EVERY phase)
- **Evidence, not status.** Every milestone = a pasted log / benchmark number / passing test, not
  "done ✅". "Reached X, blocked at Y @ RIP Z" is a valid result.
- **Mine the working code.** 32-bit `dlls/xtajit` runs real PE32 — oracle for the HyperBridge/exec
  and memory/threading patterns. arm64ec entry has no 32-bit analog (use `load_arm64ec_module`,
  `arm64ec_process_init`, `signal_arm64ec.c`).
- **Golden x64 snapshot = read-only oracle.** Use it for differential correctness; never edit it.
- **Lane boundaries:** ntdll/loader/HyperBridge/xtajit64 AND graphics (`engine/dxmt/**`,
  `engine/graphics/**`, `engine/vkd3d/**`) AND audio are ALL in-scope (Kimi sidelined). Don't edit
  the baseline build script; golden snapshot stays read-only. Back up shared untracked files before
  risky edits (as done for loader.c at `reports/backups/`). If Kimi's separate worktree exists,
  don't reach into it — work only in this canonical tree.
- **MCP 120s ceiling:** builds → `nohup … &` + poll; runs → `timeout … > log` + redirect; kill
  **scoped** (`WINEPREFIX=<p> wineserver -k`), never global. Prefixes under `artifacts/` (not /tmp).
- **No commits without the operator.** Keep a running result report per phase.
- **Kill-filter:** do NOT spend effort on kernel-anti-cheat / DX12-only titles
  (`reports/research/GAME-TARGET-LADDER-*`). Target the green list.

---

## PHASE 0 — close out the bring-up (days, do FIRST)
The final run exits 0 but the log still shows a non-fatal
`macrunner-hb-runtime-fail … x64-signal-callback block_pc=0x14000150f … MEMORY_FAULT` →
`status=c000007b`. Root-cause and fix it; a latent signal/callback fault will bite under real
games (which use exceptions heavily).
- **Gate:** `hello_x64` + `stdout_stderr_x64` run with ZERO `runtime-fail`/`MEMORY_FAULT` lines,
  exit 0. Paste the clean log.

## PHASE 1 — x86_64 ISA completeness & correctness (≈weeks 1-6)
Real games exercise the whole ISA; hello-world used a sliver.
1. **Integer ISA**: all opcodes/addressing modes, REX/prefix combos, string ops, BMI1/2, bit ops.
2. **Flags correctness** — the perennial emulator killer. Differential-test EFLAGS against the
   golden oracle for arithmetic/logic/shifts.
3. **Vector**: SSE/SSE2/SSE3/SSSE3/SSE4.1/4.2 complete; **AVX/AVX2** (many modern games hard-
   require AVX — high priority); finish x87 edge cases.
4. **Atomics & memory model**: `lock` ops, `cmpxchg8b/16b`, fences — and the **ARM weak-memory
   ordering** mapping (x86 TSO → ARM64 barriers). Get this wrong and games corrupt under threads.
5. **Exceptions/SEH across the EC boundary**: x64 `__C_specific_handler`, `RtlUnwindEx`,
   vectored handlers, faults → SEH. Mirror how 32-bit handles it; respect EC dispatcher.
- **Gate:** an expanded torture/fuzz suite (extend `engine/hyperbridge/tests`) passes vs the golden
  oracle; ≥3 non-trivial real x64 console programs (threads + SSE + exceptions) run correct.

## PHASE 2 — performance: interpreter → JIT + translation cache (≈weeks 6-16)
An interpreter cannot drive a game. This is the make-or-break phase.
1. **ARM64 codegen backend** from the existing IR (`hb_lift_x64` → ARM64 emit), per-block compile.
2. **Block chaining / trace linking** so hot paths don't re-enter the dispatcher each block.
3. **Register allocation** x64 regs → ARM64 regs; flag lazy-evaluation (don't materialize EFLAGS
   every op).
4. **Self-modifying code + icache**: honor `BTCpu64FlushInstructionCache`/NotifyMemoryDirty;
   invalidate compiled blocks on write.
5. **Persistent AOT translation cache** keyed by module hash → warm starts.
6. **Hot EC transition thunks** — the x64↔ARM64 call boundary is on every API call; make it cheap.
- **Gate:** ≥10× over the interpreter on a CPU benchmark; documented per-block compile + chaining;
  AOT cache hit on second launch. Paste benchmark numbers.

## PHASE 3 — runtime/Win32 surface for games (≈weeks 12-20, overlaps Ph2)
1. **Threading/TLS/sync/timers**: many-thread correctness, QueryPerformanceCounter, fibers, TLS
   under EC.
2. **Input**: XInput + DirectInput + raw input → macOS HID/GameController.
3. **Audio (already built → integrate + verify)**: `xaudio2_*`, `x3daudio*`, `winecoreaudio.drv`,
   `faudio` exist. Verify the full path (XAudio2/WASAPI → CoreAudio) works for an x64 game under
   emulation; fix EC-boundary issues; do NOT rebuild from scratch.
4. **Graphics — YOU own DXMT now (Kimi sidelined), CONTINUE don't restart**: DXMT is well-started
   (`engine/dxmt/` winemetal/nativemetal/airconv shader→AIR/metallib; `engine/graphics/` metal_ir/
   shader_ingest/runtime_backend; `engine/vkd3d` Metal shader runner). Drive DX11→Metal to working:
   DXGI swapchain + present + frame pacing, shader translation coverage (airconv), the CPU↔GPU
   handoff across the EC boundary (x64 game → native ARM64 d3d11/dxgi → Metal). Read the existing
   graphics tests/traces first; extend, don't greenfield.
- **Gate:** the FIRST Tier-1 game (Hades or Stardew Valley) **boots to its main menu** with input +
  audio + a rendered frame on screen. Screenshot + log.

## PHASE 4 — green-list bring-up ladder (≈weeks 18-26)
Drive the ladder; each game is a fix-loop feeding Phases 1-3.
- Tier 1: Hades, Stardew Valley, Hollow Knight, Celeste, Dead Cells, Limbo, Hyper Light Drifter.
- Then Tier 2: Cuphead, Talos Principle, Inside, Obra Dinn, The Witness, Axiom Verge.
- For each: boot → menu → gameplay → soak; log the blocker ladder; fix at the right layer.
- **Gate:** ≥5 Tier-1 games playable start→gameplay, stable for ≥30 min each. Per-game report.

## PHASE 5 — hardening / regression / CI (continuous from Ph1)
1. **Differential fuzzing** of decoder+lifter vs the golden oracle (random valid x64 → compare).
2. **Per-game regression suite** + perf-regression gates so Phase-2 speed doesn't rot.
3. **Crash triage automation**: symbolicate x64 RIP → module+offset automatically.
4. **Soak/stability**: long-run tests, leak checks.
- **Gate:** green CI across the suite; no perf/correctness regressions merge-blocked.

---

## SEQUENCING & PRIORITY
Phase 0 now → Phase 1 (correctness is the foundation; JIT on a buggy interpreter just makes fast
bugs) → Phase 2 (perf, the existential one for games) → Phases 3-4 overlap (surface + games) →
Phase 5 continuous. Re-baseline priorities with the operator at each gate.

## DELIVERABLE
A living program report `reports/research/CODEX-MEGA-PROGRAM-status.md`: current phase, gate
status, the evidence (logs/benchmarks/tests/screenshots) for each milestone hit, and the next
blocker named. Update it as you go; this is how the operator tracks the 6-month arc.
