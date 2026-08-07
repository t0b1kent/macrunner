# GEMINI TASK 4 — Offline i386 ISA coverage tool (capstone-diff) — CODE, non-colliding

**Agent:** Gemini (agy) — strong systems coder (proven: native unarc macOS ARM64 / `___chkstk_darwin`
fix). This is a CODE + analysis task. Owned: `tools/hb_isa_coverage/**` (NEW dir) ONLY. **Read-only on
engine/** (you analyze it, you do NOT edit it). Output is a tool + a gap matrix that PE32 consumes.
**Date:** 2026-06-07

## Why this, why now (no collisions)
- The window's critical path is graphics (Lane D = Sonnet, ACTIVE — do NOT touch engine/dxmt|graphics).
- PE32 (Sonnet, resuming) drives the i386 CPU bring-up and will IMPLEMENT ISA fixes — do NOT touch
  `engine/wine/dlls/xtajit/**` (that's PE32's).
- Your job is the **offline analysis that finds WHAT to fix**, so PE32 implements in bulk instead of
  one-crash-at-a-time. This is exactly the "BULK-FIRST" reflex in
  `docs/CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md` (Phase 3). Pure tooling → zero collision.

## READ FIRST (ctx, language=javascript — NOT ctx_batch_execute, it dies on /bin/zsh ENOENT)
- `docs/CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md` Phase 3 (the coverage plan).
- Locate the i386 decoder in HyperBridge (the `hb_decode_x86` / x86-32 decode path — find it under
  `engine/hyperbridge/**` or `engine/wine/dlls/xtajit/**`; READ ONLY, don't edit).
- The vendored capstone at `engine/wine/libs/capstone` (your semantic/decode reference).
- Any existing `X86-32*COVERAGE*ATLAS*` doc (known gaps: MOVZX/MOVSX/MUL/DIV/ADC/SBB/rotates).

## BUILD (tools/hb_isa_coverage/)
1. A harness that enumerates a large corpus of x86-32 instructions (generate systematically across the
   opcode map + pull real bytes from the i386 test exes' .text if useful) and **diffs the MacRunner
   i386 decoder against capstone**: for each instruction, compare mnemonic/operands/length. Flag every
   MISSING (MacRunner can't decode) and MISMATCH (decodes differently).
2. Emit `reports/research/HB-X86-32-ISA-COVERAGE-matrix.md`: a matrix of opcode family → status
   (decoded-correct / mismatch / missing), with concrete examples (bytes + expected vs actual), and a
   PRIORITY list (the atlas families first: MOVZX/MOVSX/MUL/DIV/ADC/SBB/rotates, then full integer,
   then x87, then SSE).
3. Where feasible, extend to a SEMANTIC check: for implemented opcodes, compare register/flag/memory
   effects against the x64 interpreter golden oracle (if a usable oracle entry exists) — mark
   semantic mismatches separately. If the oracle hookup is too invasive, document the exact interface
   PE32 would need and leave it as a stretch.
4. Make it re-runnable (clean CLI, machine-readable summary line) so it can become a CI coverage gate.

## DISCIPLINE
- `tools/hb_isa_coverage/**` is yours; you may READ anything under engine/ but EDIT nothing there.
- Heartbeat each step → `reports/research/LANE-E-PROGRESS.md`.
- Commit only `tools/hb_isa_coverage/**` + the matrix doc (`feat(isa-coverage): ...`), never `git add -A`.

## DONE WHEN
The tool runs, diffs the i386 decoder vs capstone over a large corpus, and
`HB-X86-32-ISA-COVERAGE-matrix.md` lists every missing/mismatched family with a priority order PE32
can implement against. Coordinator routes the matrix to PE32 Phase 3.
