# GEMINI TASK 3 — IMPLEMENT the repack CRC stress-harness (tools/repack/) — CODE

**Agent:** Gemini (agy) — you write code well; this is a CODE task, not research. Owned: `tools/repack/**`
(NEW dir) ONLY. NOT engine (`engine/**` off-limits), NOT `app/configurator`, NOT `tools/triage`.
**Date:** 2026-06-07
**Why:** You already wrote the SPEC (`reports/research/GEMINI-repack-testspec.md`: 3 targets test_open /
Limbo / Inside) and the plan (`GEMINI-repack-support-plan.md` §5-6). Nothing implements it yet —
`tools/repack/` does not exist. Build the runnable gate. This is our differentiator (CrossOver dies
ISDone/Unarc -11) and an OBJECTIVE i386-CPU correctness gate for PE32 once the 32-bit path is live.

## READ FIRST
- `reports/research/GEMINI-repack-testspec.md` (your 3 targets + acceptance + ladder mapping)
- `reports/research/GEMINI-repack-support-plan.md` (§5 self-validating design, §6 ladder)
- Prior memory: `ctx_search(queries:["macrunner-lolz-stress-test","native unarc arm64","FreeArc CRC"])`
  — native arm64 `unarc` is at `/tmp/freearc-build/unarc/unarc` (handles open ArC/LZMA/PPMD/tornado/rep,
  NOT proprietary lolz).

## BUILD (tools/repack/)
1. `tools/repack/run_repack_smoke.sh` + a Python driver (`extract_and_verify.py`): take an archive +
   a reference CRC manifest → extract (native `unarc` for open formats now; the Windows decompressor
   under MacRunner for lolz LATER) → **CRC-verify every extracted file vs the manifest**. PASS = all
   CRCs match. Emit a triage-style verdict line: `REPACK <rung> PASS|FAIL (n/m files, mismatch: ...)`.
2. Implement the ladder rungs as separate flags, each with an explicit state — NO fake PASS:
   - (a) native `unarc` open-format smoke — **must PASS NOW** (build a tiny ArC/LZMA test archive
     with known contents + reference CRC, extract, verify). This proves the harness end-to-end today.
   - (b) Inno/NSIS (no lolz), (c) FreeArc repack (no lolz), (d) full lolz repack — each prints
     `SKIP: BLOCKED on PE32 Phase 4 (32-bit CPU not live)` with the reason, until the engine is ready.
3. `tools/repack/README.md`: the ladder, the exact PE32/Lane C engine deps (from repack-support-plan
   §3 + ENGINE-REQUIREMENTS-BACKLOG), and how to run each rung. Cross-link the testspec.
4. Make rung (a) wireable into CI later (clean exit codes, machine-readable verdict line).
5. Do NOT reimplement lolz — the plan is to run the Windows decompressor as an x86 program under PE32.

## DISCIPLINE
- ctx for reading logs/specs (use `ctx_execute` language=javascript; avoid `ctx_batch_execute` — it
  fails here with spawn /bin/zsh ENOENT). Don't dump big output into context.
- Heartbeat each step → `reports/research/LANE-E-PROGRESS.md` (one line `TIME · action · result · next`).
- Commit ONLY `tools/repack/**` named files (`feat(repack): ...`), never `git add -A`.

## DONE WHEN
`tools/repack/run_repack_smoke.sh` exists; rung (a) PASSES on a real open-format archive with a CRC
match (paste the verdict line + numbers); rungs (b)-(d) cleanly report BLOCKED-on-PE32 (no fake PASS);
README documents the ladder + engine deps; committed. End LANE-E-PROGRESS with a clear status line.
