# SONNET TASK — Triage analyzer hardening (Lane X tooling)

**Agent:** Claude Sonnet (effort: high). Lane X tooling — `tools/triage/**` only. NOT engine.
**Date:** 2026-06-07
**Why now:** the triage analyzer is the mandatory gate for EVERY lane run, but today it produced
**noise that confused the coordinator**: it false-classified a healthy window path as
`CLASS_ATOM_MISMATCH`, and it falls to `WAIT_TRACE_INSUFFICIENT @0.30 / SELF_CHECK=FAIL` whenever
`flight.jsonl` is absent (which is every run right now). Sharper triage = every lane gets correct
`OWNER`/`CLASS`/`NEXT_ACTION` instead of the coordinator hand-reading logs. This is immediate,
isolated, high-leverage tooling work.

## OWNERSHIP / BOUNDARIES (hard)
- **YOURS:** `tools/triage/**` (analyzers, `classify_run.py`, `triage_common.py`, `fixtures/`,
  `regression_tracker.py`).
- **NEVER touch:** `engine/**`, `scripts/mr-run.sh` (Lane A), `app/configurator/**`, any agent's
  lane source. If a fix needs the run harness (e.g. where `flight.jsonl` is written), DO NOT edit
  `mr-run.sh` — document the exact needed change in `reports/research/TRIAGE-NEEDS.md` for Lane A.
- Use context-mode (`ctx_execute_file`/`ctx_batch_execute`/`ctx_search`) to read run logs/fixtures —
  not raw Read/Bash on big logs. Heartbeat each step → `reports/research/LANE-X-PROGRESS.md`.

## TASKS (do in order, commit each as a named-file checkpoint)

1. **Kill the `CLASS_ATOM_MISMATCH` false-positive.** Evidence: run
   `reports/phase4-hollow-knight/run-20260606-222856-lane-a-seh-boundary-final-420` shows
   `UnityWndClass` RegisterClass/CreateWindow SUCCEEDED (atom=c030, server_error=0, handle created),
   yet `analyze_window_gate.py` still emitted `CLASS_ATOM_MISMATCH` as primary. Suppress the
   atom-mismatch verdict when the log shows a successful class registration + window handle. Add the
   fixture so it can't regress.

2. **Implement the generic-fallback analyzer** per the design in
   `reports/research/GEMINI-triage-fallback.md`: new `tools/triage/analyze_generic_fallback.py` that,
   when no specific analyzer has real evidence, routes by exception code (c0000005→Lane C lead,
   c000001d illegal-instr→Lane B, c0000026 INVALID_DISPOSITION/SEH→Lane A, assert→Lane A,
   Vulkan/Metal→Lane D) and emits a hinted CLASS instead of bare `UNKNOWN/NO_ANALYZER_RESULTS`.
   Follow the exact output contract of the other `analyze_*.py` (VERDICT/OWNER/CLASS/CONFIDENCE/
   EVIDENCE/NEXT_ACTION). Wire it into `classify_run.py` at LOW priority (only wins when everything
   else is INSUFFICIENT). Add fixtures.

3. **`flight.jsonl` not captured — diagnose + document.** Every recent run reports "no flight.jsonl
   produced; MACRUNNER_FLIGHT_RECORDER impl not found in owned source" → analyzers stay run.log-only
   and fall to INSUFFICIENT. Investigate (ctx): is the recorder unimplemented, or implemented but
   writing to a path that `mr-run.sh` deletes with the throwaway prefix? Write findings +
   the exact harness change needed to `reports/research/TRIAGE-NEEDS.md` (Lane A owns mr-run.sh).
   Within tools/triage: make `classify_run.py` robustly find `flight.jsonl` in the run-dir if present
   and degrade gracefully (clear message) if not — don't crash to a misleading verdict.

4. **Add fixtures for today's real classes:** `c0000026` INVALID_DISPOSITION SEH-host-boundary
   (Lane A, from run-20260607-032419), and `PE32_WOW64CPU_NOT_LOADED` syswow64-missing (from
   `PE32-PROGRESS.md`). Verify `regression_tracker.py` passes on the full fixture set.

5. **Validate:** `python3 -m py_compile tools/triage/*.py`; run `regression_tracker.py`; run
   `classify_run.py` on 2-3 existing run-dirs and confirm correct OWNER/CLASS.

## DONE WHEN
No false `CLASS_ATOM_MISMATCH` on healthy-window runs; generic-fallback routes novel failures by
exception code (no bare NO_ANALYZER_RESULTS); flight.jsonl handling robust + harness gap documented
in TRIAGE-NEEDS.md; new fixtures green; py_compile + regression_tracker pass. Commit named files only.
