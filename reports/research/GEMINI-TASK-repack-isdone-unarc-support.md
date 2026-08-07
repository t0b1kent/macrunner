# GEMINI TASK — Repack support (lolz / FreeArc / ISDone / Unarc) research + plan (Lane E)

**Agent:** Gemini (agy) — Lane E research, NO engine code. Output = a report (+ links).
**Date:** 2026-06-07
**Why this matters:** repacks (FitGirl / dixen18 / xatab) are how most CIS users actually install
big games. **CrossOver/Whisky FAIL on them** (`ISDone.dll` / `Unarc.dll` error -11) because their
Wine mis-executes the x86 decompressor helpers (srep/lolz/precomp). MacRunner's fuller i386
translation should run those helpers as "just another x86 program" → **repacks install where
CrossOver can't.** This is a concrete differentiator. Bonus: a repack extraction is **CRC-verified**,
so it doubles as a self-validating i386 CPU/JIT stress test (objective pass/fail, unlike "game looks
fine").

## MANDATORY — use context-mode, cite sources
- First: `ctx_search(queries:["lolz repack stress test","ISDone Unarc","FreeArc unarc"], sort:"timeline")`
  — there is prior memory (`macrunner-lolz-stress-test.md`: dixen18 LIMBO/INSIDE use xt66+srep+lolz;
  CrossOver dies ISDone/Unarc -11; native `unarc` arm64 built at `/tmp/freearc-build/unarc/unarc`
  handles ArC/LZMA/PPMD/tornado/rep but NOT proprietary lolz). Pull it.
- Web research via `ctx_fetch_and_index` + `ctx_search` — never raw page dumps. Cite every fact.
- Repo cross-ref via `ctx_batch_execute`. NEVER touch `engine/**` — research/docs only.

## DELIVERABLE: `reports/research/GEMINI-repack-support-plan.md`
Cover:

1. **Repack ecosystem & compression chain.** FitGirl/dixen18/xatab; the stack: `ISDone.dll`
   (orchestrator) → `Unarc.dll`/`unarc.exe` (FreeArc) → helpers `srep`/`precomp`/`zstd`/`lolz`.
   Which are 32-bit, which spawn child processes, which use anti-debug/timing tricks.

2. **Why CrossOver/Wine fails (root, cited).** The `ISDone.dll error -11` / `Unarc decompression
   fails` class — is it (a) x86 decompressor helper miscompute (CPU/JIT correctness), (b) child-
   process/pipe handling (ISDone spawns workers + reads stdout/progress over pipes), (c) large
   VirtualAlloc / >2GB LAA, (d) timing/anti-debug? Cite CodeWeavers AppDB, ProtonDB, repacker forums.

3. **What MacRunner needs (map to our lanes).** Cross-reference the requirements to:
   - **PE32 mega-program** (`CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md` Phase 4) — full i386 ISA
     for the decompressor inner loops (this is exactly the CPU-bound x86 the JIT must nail).
   - **Lane C** — CreateProcess + async named pipes (ISDone↔worker), large VirtualAlloc, temp dirs,
     syswow64 hygiene (reconcile with your `GEMINI-laneg-gtavc-d3d8-installer.md` FreeArc checklist).
   - Produce a concrete GAP LIST per lane.

4. **lolz vs open FreeArc.** Native `unarc` (arm64, `/tmp/freearc-build`) handles open ArC formats
   but NOT proprietary `lolz`. Plan: run the Windows `lolz.dll`/`srep.exe` under MacRunner's i386
   path (they are ordinary x86 programs) rather than reimplementing lolz. Note the build patches
   already done (Environment.cpp sysctl, entropy.cpp malloc.h, Common.h FILE::close).

5. **Self-validating stress-test design.** A repack that extracts → CRC-checks its output. Use this
   as a CI-grade i386 correctness+perf gate: PASS = extracted files match reference CRC. List 2-3
   concrete small repack targets (e.g. dixen18 LIMBO/INSIDE) + the acceptance command.

6. **Test ladder & acceptance.** Order: native `unarc` open-format smoke → simple Inno/NSIS (no
   lolz) → FreeArc repack (no lolz) → full lolz repack. Per step: command, expected result, what it
   proves about the engine.

## DONE WHEN
The report exists with: cited root cause of the CrossOver -11 failure, per-lane gap list (PE32 / C),
the lolz-as-x86-program plan, and a CRC self-validating test ladder with concrete repack targets.
Coordinator indexes it + routes the gap lists to PE32 Phase 4 and Lane C.
