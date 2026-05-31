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
- **"Blocked on an external asset" ≠ stop and idle.** If a gate needs something only the operator
  can supply (e.g. a licensed game binary), DO NOT yield yet — first exhaust ALL remaining
  game-INDEPENDENT work (other phases, hardening/fuzz/CI, proxy real-app targets already in the
  workspace like Notepad++ x64 / KeePass, a bring-up harness). Record the asset need in the status
  file's NEXT, keep building everything that doesn't require it, and only yield when nothing
  game-independent is left. Distinguish a CODE blocker (push through it) from an EXTERNAL-ASSET
  blocker (note it, route around it, keep working).
- **Work in long autonomous stretches.** Respect the MCP 120s rule (background builds + poll,
  timeout+redirect runs) so a long run never stalls on one blocked call. If you approach a context
  limit, write your state to the living report so you can resume, then continue.
- Phase 0 starts with a ready diagnosis:
  `reports/research/ARM64EC-PHASE0-c000007b-tls-callback-misaddress-diagnosis.md`.

## ★ PRIORITY INSERT — BULK ISA COVERAGE (do this proactively, alongside Hollow Knight)
**Rationale:** chasing one missing/mis-decoded opcode per game-run is slow. The full x86-64
instruction set is finite and externally specified — cover it in bulk instead of reactively.
This kills the *opcode* failure class up front. (It does NOT remove Win32-API or correctness
failures — those still surface only by running real software; see Phase 3/5.)

**Reference material (use, don't reinvent):**
- **capstone is ALREADY vendored** at `engine/wine/libs/capstone` — use it as the authoritative
  x86-64 decode reference (mnemonic, operands, length) to diff our `hb_decode_x64` against.
- Intel SDM Vol.2 / AMD APM Vol.3 opcode maps; open tables from Zydis/XED/iced as cross-checks.
- Our golden oracle + `tools/hb_oracle/` (compare.py, fast_validate_family.sh) for *semantic*
  correctness, not just decode.

**Task (engine lane):**
1. **Decode coverage:** enumerate the x86-64 opcode map (1-byte, 0F, 0F38, 0F3A, VEX, EVEX,
   prefixes/REX/operand-size, ModRM/SIB) and make `hb_decode_x64` decode every defined encoding —
   diff each against capstone over a large random + manual corpus. Any encoding capstone decodes
   that we don't (or decode differently, e.g. the BSF-vs-TZCNT bug) is a finding to fix.
2. **Lift+execute coverage:** for each decoded op, ensure `hb_lift_x64`/interp/JIT implement it,
   and **diff semantics (regs+flags+memory) against the golden oracle** — flags correctness is the
   usual killer. Prioritize families real games use: full integer ISA, SSE/SSE2/SSE3/SSSE3/
   SSE4.1/4.2, AVX/AVX2, BMI1/2, atomics (lock/cmpxchg8b/16b), x87.
3. **Coverage matrix:** produce `reports/research/HB-X64-ISA-COVERAGE-matrix.md` — per opcode
   group: decoded? lifted? interp? jit? oracle-verified? — so the gap is visible, not guessed.
4. **Wire into CI (Phase 5):** the capstone-diff + oracle-diff fuzz becomes a permanent gate so
   coverage can't regress.
**Gate:** decoder matches capstone across the corpus (0 unexplained mismatches); oracle-diff green
for all implemented families; coverage matrix published. After this, game runs should hit far
fewer opcode stalls — remaining failures will be Win32-API/correctness/ABI, which is expected.
**Do this in parallel with the Hollow Knight bring-up — both feed each other.**

## ★ PRIORITY INSERT #2 — BULK JIT CODEGEN COVERAGE (do proactively, like bulk-ISA)
**Rationale:** Hollow Knight no longer crashes — it is throughput-bound because the JIT falls back
to the interpreter on uncovered IR ops (`macrunner-hb-jit-fallback: ... JIT codegen failed`).
Chasing one fallback PC per run is slow. The IR-op set is **finite and externally known** (enum in
`engine/hyperbridge/include/hb_ir.h`, **163 ops**), and the **interpreter already implements ALL of
them** — so JIT codegen needs no new semantics, only an ARM64 emit per op mirroring the interpreter.
Current state: JIT codegen references only ~65 of 163 IR ops → the rest fall back. Close the gap in
bulk.

**Reference (already in tree, don't reinvent):**
- `engine/hyperbridge/include/hb_ir.h` — the authoritative 163-op checklist.
- `engine/hyperbridge/src/hb_interpreter.c` — the correctness reference for EACH op (it covers all).
- `engine/hyperbridge/src/hb_arm64_codegen.c` — where the ARM64 emit lives (extend it).
- Golden oracle + `tools/hb_oracle/` — diff JIT result vs interpreter/oracle per op.

**Task (engine lane):**
1. Enumerate all 163 `HB_IR_*` ops; mark which the JIT codegen already emits vs which fall back.
2. For every op the interpreter handles but JIT doesn't, add an ARM64 codegen pattern (mirror the
   interpreter's semantics: regs + **flags** + memory width + addressing). Flags are the usual bug.
3. **Diff-test each JIT op against the interpreter and the golden oracle** — JIT result MUST equal
   interpreter result. A wrong-but-fast codegen is worse than a fallback.
4. Coverage matrix `reports/research/HB-JIT-CODEGEN-COVERAGE-matrix.md`: per IR op →
   jit-emitted? oracle-verified? Goal: **zero JIT fallbacks on the Hollow Knight hot path**.
5. Keep interpreter fallback as a SAFETY net for genuinely hard ops (don't remove it), but it
   should stop firing on the common path.
**Gate:** JIT covers all interpreter-supported IR ops used by Hollow Knight (no fallback spam in
the run log); JIT-vs-interpreter diff green across the corpus; matrix published; Hollow Knight
throughput high enough that the window/menu can appear within the run timeout.

## GENERAL PRINCIPLE — BULK/EXHAUSTIVE FIRST, REACTIVE ONLY WHEN UNAVOIDABLE (MANDATORY REFLEX)
**Before grinding one-item-per-run on ANY failure class, STOP and ask: is this a finite,
externally-specified set with a reference implementation already available?** If yes → cover the
WHOLE set ONCE against that reference, publish a coverage matrix, and move on. Do NOT discover the
set one failing run at a time. This is mandatory, not optional — it is the single biggest speed
lever in this program.

Decision test (apply at the start of every new blocker):
1. Is the failure class enumerable from an external spec or an existing list? (e.g. an enum, an ISA
   manual, a header) → likely BULK.
2. Is there already a reference that handles the whole set correctly? (capstone for decode, the
   interpreter for IR semantics, an OS header for an API table) → definitely BULK: mirror it
   exhaustively + diff-test against it.
3. If NO finite list and NO reference (Win32-API *behavior*, memory/ABI correctness, game-specific
   bugs) → only then go reactive (run real software, fix what surfaces).

Established BULK sets: x86-64 opcodes (ref = vendored `engine/wine/libs/capstone`); JIT IR codegen
(163 ops in `hb_ir.h`, ref = interpreter). **Whenever you hit a new blocker, run the decision test
first; if it's BULK, do it in bulk + matrix BEFORE the next game run.** Reactive grind on a
bulk-able set is a process error — call it out in the status file and convert it.

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
- **HOUSEKEEPING (MANDATORY — use the provided scripts, do not hand-roll).** Every run leaks a
  ~1.5G throwaway prefix + large trace logs, and a timed-out run leaves an ORPHANED wine tree
  (explorer.exe hot-spins ~80% CPU forever). To leave NO tails:
  - **Launch runs via `scripts/mr-run.sh <dist> <exe> [timeout]`** — it runs in a throwaway prefix
    and ALWAYS scoped-kills + removes that prefix on exit/timeout/Ctrl-C (trap-based). Copy any
    evidence out during the run.
  - **After every sub-run / at each gate, run `scripts/mr-clean.sh`** (kills orphan spike wine +
    winetemp) and **`scripts/mr-clean.sh --prune`** at end of a phase (also deletes throwaway
    drive_c prefixes + giant >50M logs, keeping summaries/replay/ppm).
  - NEVER a global `pkill wine` (the scripts are scoped to `dist-arm64ec-spike`). Keep the small
    summary, delete the giant raw `.log`. Goal: artifacts/ + reports/ do not grow unbounded and no
    orphan wine survives a run.
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
