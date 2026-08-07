# MacRunner — COORDINATOR HANDOFF (read this first, then CLAUDE.md)
Purpose: make a second coordinator account operate the lanes the SAME way. This captures (1) the
current state, (2) HOW to coordinate (the thinking), (3) how to write lane prompts, (4) guardrails.
Pair with `CLAUDE.md` (hard rules) — this doc is the OPERATING PHILOSOPHY + live state.

---
## 0. WHAT MACRUNNER IS
Run unmodified x86-64 Windows games on Apple Silicon **without Rosetta**: HyperBridge (x86→ARM64
binary translator) + pure-ARM64 Wine 11 + **ARM64EC** + **DXMT** (D3D11→Metal). You (the coordinator)
are NOT the one writing engine code turn-by-turn — you are the **strategist / memory-keeper / diagnostician**
who reads lane activity, writes precise prompts, integrates research, and keeps lanes on the critical path.

## 1. CURRENT STATE (update as it moves; verify before trusting)
**Critical path = the Hollow Knight (Unity/Mono x64) WINDOW.** Boot ladder climbed so far:
loader → Mono init → (vtable gate PASSED) → (thread-context gate PASSED) → (TSO/atomics done) →
(JIT-helper/stack gate) → **GfxDevice reached (06-07)** → SEH host-boundary `c0000026` PASSED via bulk
CFI → ARM64EC entry/dispatch gates → **loader phases 0-9 run, reaches x64 RtlUserThreadStart** → now
on **ARM64EC native-call ABI dispatch** → next walls are graphics (CreateDXGIFactory→D3D11CreateDevice).
**Graphics is PROVEN ready** (Lane D: 82/82 HK pixel shaders render headless through DXMT→Metal).
So the window is gated on the x64 runtime reaching D3D11, NOT on graphics.

**Lane map / models (quota-aware — Codex quota exhausts periodically; concentrate scarce models on the
hardest lane):**
- **Lane A** (x64 runtime→window; owns `macrunner_hb.c`, `signal_arm64.c`, `xtajit64`, `hb_*`): the
  keystone. Hardest, novel debugging → strongest model. Currently the single active terminal ("fable-5").
- **Lane D** (graphics/DXMT; `engine/dxmt|graphics|vkd3d`): D3D11 surface done; D3D9/D3D8→Metal in
  progress (Terraria/XNA, GTA VC/RenderWare). Mechanical+test-gated → can run on a fast model.
- **PE32** (32-bit; `xtajit` NOT xtajit64, `wow64`/`wow64cpu`/`wow64win`, i386 ntdll `heap.c`, i386
  loader): i386 CPU executes; Diablo/Terraria reach BTCpu; grinding WOW64/teb32 plumbing.
- **Lane B / ISA**: x64+i386 ISA coverage matrices + differential fuzzer DONE (`tools/hb_isa_coverage`).
  Remaining ISA = engine-lane *implementation*, not re-coverage.
- **Gemini (agy)**: proven code agent (built native macOS-arm64 unarc, repack CRC harness, ISA tools).
  Now Audio+Input lane (DSound/XAudio2/DInput/XInput → CoreAudio/GameController).
- **Lane C** (ntdll loader/server): mostly parked (real blocker was SEH, not loader).
- **Triage**: `tools/triage/classify_run.py` auto-runs via `mr-run.sh` when `MACRUNNER_RUN_DIR=$RUNDIR`
  is set — gives OWNER/CLASS/EVIDENCE/NEXT_ACTION per run. READ IT; it is gold for next steps.

## 2. HOW TO COORDINATE (the thinking — this is what "think like me" means)
1. **READ THE ACTIVITY BEFORE ANSWERING ANY STATUS QUESTION.** Sources of truth in order:
   (a) the agent's terminal output the operator pastes — that IS ground truth, trust it over everything;
   (b) the lane's `LANE-*-PROGRESS.md` / `PE32-PROGRESS.md` heartbeat; (c) the run's `triage-summary.txt`.
   **NEVER infer "idle/stopped/done/almost-window" from absence of commits / run-dirs / low CPU** — those
   LAG badly (agents churn 50+ min uncommitted, runs in progress). Do not contradict a live terminal.
2. **Evidence over status.** A pasted log line / exported symbol / pixel readback / triage CLASS is truth.
   "Agent says done / green checklist" is not. "Blocked at X" with the exact blocker named is a VALID,
   useful result — never fake forward progress.
3. **Spot rabbit-holes and break them.** Red flags: a lane permutes many variants of the same thing
   (context/stack/tag/dispatch) with NO forward marker, dirty tree uncommitted, same failure each run.
   → Tell it to STOP, restore the last verified-forward baseline (git stash/checkout the uncommitted
   thrash; the discipline is "commit only on verified-forward" so dirty thrash is safe to drop), confirm
   the last good milestone still holds (regression check), then change ONE thing at a time. (This exact
   intervention recovered Lane A on 06-10.)
4. **Concentrate on the critical path.** The window is gated on ONE lane. Off-path lanes are insurance —
   don't burn quota/attention re-feeding them every 10 min while the window waits. Freeze them at clean
   stops; concentrate the scarce strong model on the keystone.
5. **Bulk-over-reactive.** If a failure class is a finite set with a reference impl (x86 opcodes vs
   capstone; IR ops vs interpreter), cover the WHOLE set ONCE + publish a coverage matrix. Never one-fix-
   per-failing-run. Proactively convert a lane that's grinding one-at-a-time into a bulk sweep.
6. **Single-turn runtimes RETURN at end of turn** (`codex exec`, `agy --print`, `claude -p`). "Don't stop
   for months" is advisory and CANNOT override the runtime. Autonomous lanes MUST be wrapped in an
   autoloop (`scripts/lane-autoloop2.sh` for codex, `lane-autoloop-agy.sh` for Gemini; Claude lanes =
   Agent tool background + re-spawn). Verify the loop is alive (ps the driver+agent), don't just trust it.
7. **Use research agents (ChatGPT/Gemini) at decision points**, then INTEGRATE: write the answer into the
   gate-diagnosis (`CLAUDE-GATE-DIAGNOSIS-*.md`) as a dated UPDATE + the standing reference
   (`PRISM-ARCHITECTURE-REFERENCE.md`), and distill it into a precise lane prompt. Don't just relay.
8. **Coordinate shared-file edits.** Lanes overlap on ntdll-unix (`loader.c`/`virtual.c`/`thread.c`).
   If two lanes are live in the same files → conflict. Either sequence them or hard-partition file scope
   in the prompts (cross-lane needs → write to `*-NEEDS.md`, don't edit the other lane's file).

## 3. HOW TO WRITE A LANE PROMPT (the template that works)
Every standing-lane prompt has these parts:
- **Resume anchor:** "resume from `<MEGA-PROGRAM doc>` + `<LANE-PROGRESS.md>` heartbeat (read via ctx)."
- **Autonomy:** "find blocker → fix at ROOT → rebuild → rerun → chain to NEXT. Not find-report-stop. On
  'continue?' the answer is always YES. Stop only on the 3 STOP conditions."
- **Bulk-first:** "if you hit a finite set (opcodes/formats), close the whole family vs the reference +
  matrix, never one-by-one."
- **Scope (hard):** "edit ONLY <owned files>. Cross-lane root → record in `<LANE>-NEEDS.md` and keep
  going; do NOT edit another lane's file." (Name the forbidden files explicitly.)
- **Discipline:** ctx (`ctx_execute` javascript) for logs NOT raw cat/grep (a hook enforces this); runs
  via `MACRUNNER_RUN_DIR=$RUNDIR scripts/mr-run.sh` under timeout (auto-triage); scoped `wineserver -k`
  never global pkill; `./scripts/disk-guard.sh` before long cycles; heartbeat ONE line per step in the
  PROGRESS file; commit NAMED files only (never `-A`), code yes / docs no; **verdict = real on-screen
  window (CG-capture) or pasted evidence, not "process alive"**.
- **Verified-forward:** "change ONE thing at a time; commit only when a milestone holds AND the blocker
  advanced; regression-check the last milestone every build."

## 4. HARD GUARDRAILS (never violate — same as CLAUDE.md)
- NEVER global `pkill -9 wine`/`killall wine` — scoped `WINEPREFIX=<p> <dist>/bin/wineserver -k` only.
- NEVER `git add -A`/`git add .` — repo massively untracked (secrets/big binaries). Named files only.
- NEVER commit unless the operator asks. NEVER edit the golden x64 snapshot. NEVER touch git config.
- Disk: no `cp -r $WINEPREFIX`; rolling max-3 snapshots; clean orphan `artifacts/_mr-run.*` when no live
  run; `disk-guard.sh` before long cycles (STOP if <30GB). Runs through `mr-run.sh` + `timeout`, clean
  with `mr-clean.sh --prune`.
- context-mode for heavy reads (enforced by `scripts/ctx-guard-hook.py` PreToolUse hook — blocks raw
  `cat/grep/tail` on logs, `grep -r`, `rg`; allows pipes, single-file grep, make, heredoc writes).

## 5. SOURCES OF TRUTH (where to look)
- Live: operator's pasted terminal output > `LANE-*-PROGRESS.md` / `PE32-PROGRESS.md` > `triage-summary.txt`.
- Plan/state: `CLAUDE.md` (rules + current lane map), this file, `MEGA-PROGRAM-*.md`, `AGENT-TEAM-OWNERSHIP.md`.
- Diagnosis: `CLAUDE-GATE-DIAGNOSIS-20260603.md` (dated UPDATEs), `PRISM-ARCHITECTURE-REFERENCE.md`,
  `CHATGPT-BRIEF-*.md`, per-lane `LANE-*-NEEDS.md` / fix-briefs.
- Tools: `tools/triage/classify_run.py`, `tools/hb_isa_coverage`, `tools/repack`, `scripts/mr-run.sh`,
  `scripts/lane-autoloop*.sh`, `scripts/disk-guard.sh`, `scripts/ctx-guard-hook.py`.

## 6. ONE-LINE ETHOS
Read activity first. Evidence not status. Keep the keystone on the critical path, freeze the rest.
Break rabbit-holes back to the last verified-forward baseline. Bulk finite sets. Integrate research.
Coordinate file scope. Verdict = pixels on screen.
