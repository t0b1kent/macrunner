# CODEX MEGA-PROGRAM — from "x64 hello-world runs" to "MacRunner runs real games"

**Agent:** Codex / Opus (engine lane — widest scope)
**Date:** 2026-05-30
**Horizon:** ~6 months. This is a PROGRAM, not a task. Phased, gated, evidence-driven.
**Where we start (DONE, verified):** a plain x86_64 PE (`hello_x64.exe`, `stdout_stderr_x64.exe`)
runs end-to-end through the ARM64EC/xtajit64 path to `run_exit=0`, no Rosetta/FEX; system DLLs
native ARM64, only game x64 emulated by HyperBridge. x87 family 10/10. Spike build green.
**Where we finish:** the Tier-1 green-list games are playable on MacRunner, fast and stable.

This brief is the engine track. Graphics (DXMT) = Kimi's lane; store/launcher layer = separate;
both have integration contracts called out below but are NOT your implementation.

---

## CROSS-CUTTING RULES (apply to EVERY phase)
- **Evidence, not status.** Every milestone = a pasted log / benchmark number / passing test, not
  "done ✅". "Reached X, blocked at Y @ RIP Z" is a valid result.
- **Mine the working code.** 32-bit `dlls/xtajit` runs real PE32 — oracle for the HyperBridge/exec
  and memory/threading patterns. arm64ec entry has no 32-bit analog (use `load_arm64ec_module`,
  `arm64ec_process_init`, `signal_arm64ec.c`).
- **Golden x64 snapshot = read-only oracle.** Use it for differential correctness; never edit it.
- **Lane boundaries:** never touch `engine/graphics/**` (Kimi). Don't edit the baseline build
  script. ntdll/loader/HyperBridge are in-scope. Back up shared untracked files before risky edits
  (as done for loader.c at `reports/backups/`).
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
3. **Audio**: XAudio2 / WASAPI → CoreAudio (coordinate with the audio lane).
4. **Graphics integration (contract with Kimi, not your impl)**: define + wire the CPU↔GPU handoff
   to **DXMT (DX11→Metal)** — DXGI swapchain, present, frame pacing. You own the x64-side calling
   into the native graphics DLLs across the EC boundary; Kimi owns Metal.
- **Gate:** the FIRST Tier-1 game (Hades or Stardew Valley) **boots to its main menu** with input +
  audio + a rendered frame. Screenshot + log.

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
