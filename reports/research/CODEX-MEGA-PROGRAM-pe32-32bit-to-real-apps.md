# CODEX MEGA-PROGRAM — from "PE32 maps but i386 CPU never starts" to "32-bit Windows apps & games run"

**Agent:** Codex 5.5 (xhigh) — Lane PE32/WOW64 (the entire 32-bit front).
**Date:** 2026-06-07
**Horizon:** ~months. This is a PROGRAM, not a task. Phased, gated, evidence-driven, AUTONOMOUS.
**Where we start (verified, `PE32-PROGRESS.md`):** a PE32/i386 `notepad++.exe` MAPS, but the WOW64
CPU-provider never loads (`syswow64` has no `wow64.dll/wow64cpu.dll/wow64win.dll/xtajit.dll`), so
`BTCpuProcessInit` is never called and the i386 CPU never starts.
**Where we finish:** real 32-bit Windows software runs on MacRunner — Notepad++ x86 window on screen,
Inno/NSIS/FreeArc installers complete, and legacy 32-bit games (Diablo 1 DDraw, GTA Vice City D3D8)
boot to a frame. The 32-bit path is as solid as the x64 ARM64EC path.

---

## ⚡ AUTONOMY — FIND **AND FIX**, CHAIN ALL PHASES, DO NOT STOP (read first)
- **This is NOT "find a bug → report → wait → find next → report".** That is forbidden. When you
  find a blocker, you **FIX it at the root in your files, rebuild, re-run, verify, and immediately
  continue to the next blocker.** Chain Phase 0 → 6 without pausing for approval. The answer to
  "may I continue?" is always YES.
- **Only stop for:** (a) a true hard blocker you cannot resolve after real attempts (paste what you
  tried into `LANE-PE32-PROGRESS.md`), (b) something only the operator can supply (a paid asset, a
  signing decision), (c) risk of regressing a shipped milestone. Everything else: keep fixing.
- **Work in long autonomous stretches.** Background heavy builds + poll; timeout+redirect runs.

## 🟣 SURVIVAL DISCIPLINE — so you never die on context (this is HOW you run for months)
The size of the mission is NOT the problem; reading raw bulk into your conversation is. Follow this
and your session survives indefinitely + auto-compaction keeps working:
- **ALWAYS use context-mode** for logs/files/analysis: `ctx_execute_file`, `ctx_batch_execute`,
  `ctx_search`. NEVER raw `Read`/`Bash cat|grep|tail` on big logs (that is what killed Lane A at 1M
  tokens). Raw bytes stay in the sandbox; only the derived answer enters your context.
- **Checkpoint after EVERY step** to `reports/research/LANE-PE32-PROGRESS.md`: one line
  `TIME · phase · action · result · next`. This is your resume anchor + the coordinator's heartbeat.
- **If you ever approach the context limit:** write full state to `LANE-PE32-PROGRESS.md`, then a
  FRESH thread reads it and continues from the exact spot. The conversation is disposable; the
  durable state is the progress file + committed code.
- Engine edits → also `docs/ENGINE-CHANGE-JOURNAL.md`. Commit named files only; never `git add -A`.

## 🛑 BULK-FIRST REFLEX (the single biggest speed lever — same as the x64 program)
Before grinding one failing run at a time, ask: **is this a finite, externally-specified set with a
reference?** If yes → cover the WHOLE set ONCE and publish a matrix.
- x86-32 opcodes are finite and `engine/wine/libs/capstone` is vendored → diff `hb_decode_x86`
  against capstone over a big corpus, fix every mismatch in bulk. The x64 interpreter + golden
  oracle are the semantic reference for shared IR.
- Per `X86-32BIT-COVERAGE-ATLAS` the known-missing families are MOVZX/MOVSX/MUL/DIV/ADC/SBB/rotates
  — cover the families, don't discover them one crash at a time.

## SCOPE / OWNERSHIP (hard)
- **YOURS:** `engine/wine/dlls/xtajit/**` (32-bit, NOT `xtajit64`), `engine/wine/dlls/wow64`,
  `wow64cpu`, `wow64win`, the i386 decode/lift/interp paths you own in HyperBridge for x86-32,
  and the prefix/deploy bits that place i386 syswow64 DLLs.
- **NEVER touch (Lane A x64):** `xtajit64/**`, `macrunner_hb.c`, `signal_arm64.c`, the x64 hb_*
  paths. If you need a change there → `reports/research/PE32-NEEDS.md` + ping coordinator, don't edit.
- x64 path = read-only ORACLE. Mine it: the working x64 `xtajit64`/WOW64 shows how the i386 path
  should wire BTCpu, handle syscalls, and return struct layouts. Mirror it, don't reinvent.
- Hygiene: scoped kill `WINEPREFIX=$PWD/bottles/generic-x86 wineserver -k`, NEVER global pkill
  (other lanes run concurrently). Verdict = real on-screen window (CG-capture), not "process alive".

---

## PHASE 0 — Deploy the WOW64 CPU provider into syswow64 (the live blocker)
Root cause (verified): prefix `syswow64/` lacks i386 `wow64.dll/wow64cpu.dll/wow64win.dll/xtajit.dll`
(only `system32` has them) → WOW64 never enters `wow64/syscall.c:process_init` → no `BTCpuProcessInit`.
FIX: find where the prefix is populated (sync-prefix / mr-run prefix setup / dist install) and ensure
the i386 CPU-provider DLLs land in `syswow64`. Rebuild prefix, re-run NPP x86, verify `wow64cpu`
loads + `BTCpuProcessInit` is called. **Gate:** `BTCpuProcessInit`/`BTCpuThreadInit` reached in log.

## PHASE 1 — i386 CPU executes (BTCpuSimulate runs real i386 code)
Drive from BTCpuProcessInit → `BTCpuSimulate` actually executing i386 instructions. Mirror the x64
oracle for context/syscall/return wiring. Fix the WoW64 guest32↔host pointer boundary issues as they
surface (TEB32 mirror, fs:[..], NtContinue/NtRaiseException pointer marshalling). **Gate:** i386
instructions execute under the translator (heartbeat shows guest EIP advancing, not PC=0).

## PHASE 2 — Notepad++ x86 → real window on screen
Past CPU bring-up, drive the GUI. Known prior blocker: 100% CPU hot-spin at
`NtQuerySystemInformation class=102` (SystemModuleInformation) — classify (malformed 32-bit struct
layout from the WOW64 thunk vs opcode loop vs unimplemented class), fix at root. Then CreateWindow /
NtUser / NtGdi path to a visible Notepad++ x86 window. **Gate:** Notepad++ x86 window on screen (CG
screenshot with real content), exit clean.

## PHASE 3 — i386 ISA / JIT bulk coverage (find AND fix the whole class)
BULK: enumerate the x86-32 opcode map, diff `hb_decode_x86` vs vendored capstone over a large corpus;
implement every missing decode + lift + interp + JIT, diff semantics (regs+flags+memory) vs the
golden oracle. Prioritize the atlas gaps (MOVZX/MOVSX/MUL/DIV/ADC/SBB/rotates) + full integer ISA +
x87 + SSE. Publish `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md`. **Gate:** decoder matches
capstone (0 unexplained), oracle-diff green for implemented families, matrix published.

## PHASE 4 — Installers (Inno / NSIS / InstallShield / FreeArc / unarc / lolz)
Make 32-bit installers complete. Needs (Gemini's checklist `GEMINI-laneg-gtavc-d3d8-installer.md`):
Large-Address-Aware 3GB, async named pipes (stdout/stderr) for decompressors, CreateProcess +
WaitForSingleObject, file I/O, temp dirs, large VirtualAlloc, syswow64 i386 DLL hygiene. Validate on
the in-tree installers (Notepad++ x86 installer, KeePass, OldClassicCalc) + a FreeArc/unarc repack.
**Gate:** an Inno/NSIS installer runs to completion and the installed app launches.

## PHASE 5 — Legacy 32-bit games (the payoff)
- **Diablo 1** (PE32 + DirectDraw): palette-oracle 8-bit indexed → Metal texture via shader
  (`GEMINI-lanef-diablo-ddraw.md`); SetEntries/Lock/Blit path. Coordinate graphics with Lane D.
- **GTA Vice City** (PE32 + D3D8 + RenderWare + DInput/DSound): D3D8→D3D9→DXMT proxy
  (`GEMINI-laneg-gtavc-d3d8-installer.md`). **Gate:** each boots to a rendered frame / menu on screen.

## PHASE 6 — Regression / CI
Wire the capstone-diff + oracle-diff fuzz for x86-32 into CI (permanent gate, like x64). Smoke the
Phase 2/4/5 targets as regression anchors. **Gate:** CI green; coverage can't regress.

---

## LIVING REPORT
`reports/research/LANE-PE32-PROGRESS.md` — resume anchor + heartbeat. At the START of every run/thread:
read it, resume the first phase not DONE, chain forward. Update it as you pass each gate. This is how
the operator and coordinator follow along ASYNCHRONOUSLY — and how a fresh thread continues after a
context reset.

## DEFINITION OF DONE
Notepad++ x86 window on screen · an installer completes · Diablo 1 / GTA VC boot to a frame · x86-32
ISA coverage matrix green in CI. The 32-bit front is production-grade, not a demo.
