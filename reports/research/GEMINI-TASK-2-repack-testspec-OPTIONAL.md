# GEMINI TASK 2 (OPTIONAL — park is also valid) — Operationalize the repack test ladder (Lane E)

**Agent:** Gemini (agy) — Lane E research/docs ONLY, NO engine code. Output = a runnable TEST SPEC.
**Date:** 2026-06-07
**Status of Gemini's program:** COMPLETED. The 7-phase compat-intelligence program + repack plan +
unified ENGINE-REQUIREMENTS-BACKLOG are all done and verified. Research is genuinely AHEAD of the
engine, so **parking Gemini is the honest default.** This task exists ONLY so the terminal isn't idle
— and it is *additive*, not redundant: it turns the repack PLAN into a concrete, runnable spec that
Sonnet's CRC harness (`tools/repack/`) and PE32 Phase 4 directly consume.

## MANDATORY
- context-mode + cited sources only (`ctx_fetch_and_index`, `ctx_search`). NEVER touch `engine/**`.
- Pull prior memory first: `ctx_search(queries:["macrunner-lolz-stress-test","dixen18 LIMBO INSIDE",
  "FreeArc unarc CRC"], sort:"timeline")`.

## DELIVERABLE: `reports/research/GEMINI-repack-testspec.md`
1. **Curate 3 concrete small repack targets** (objective + small, e.g. dixen18 LIMBO / INSIDE, or a
   tiny FreeArc sample) — for each: exact source/how-to-obtain, size, the compression chain it uses
   (srep/precomp/lolz?), and whether it needs lolz (proprietary) or is open-format (native unarc OK).
2. **Per-target acceptance:** the reference CRC/manifest expectation + the exact command Sonnet's
   `tools/repack/run_repack_smoke.sh` should run, and the precise PASS condition.
3. **Map each target to the ladder rung** (open-format-now vs BLOCKED-on-PE32-Phase4) and to the
   specific PE32/Lane C engine requirements from GEMINI-repack-support-plan §3 + ENGINE-REQUIREMENTS-
   BACKLOG (so when PE32 lands i386 CPU, the repack gate is immediately runnable).
4. **No fake readiness:** clearly mark which rungs are runnable TODAY vs blocked, and on what.

## DONE WHEN
The test-spec exists with 3 cited concrete targets, per-target acceptance commands + CRC expectations,
and the ladder-rung/engine-dependency mapping. Coordinator routes it to Sonnet (tools/repack) + PE32.
**If the operator prefers to park Gemini instead, that is fully acceptable — this is optional.**
