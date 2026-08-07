# SONNET TASK 2 — Configurator end-to-end auto-config + repack CRC stress-harness (Lane Product)

**Agent:** Claude Sonnet (effort: high). Lane Product — `app/configurator/**`, `profiles/**`,
`tools/repack/**` (new). **NOT engine** (`engine/**` is off-limits), NOT `tools/triage` (Lane X, done).
**Date:** 2026-06-07
**Why now:** Lane X (triage) and the AI-analyzer hardening (`analyze_full()→ExeAnalysis`, anti-cheat
detection, engine-fingerprint, dynamic-imports, profile lookup, 23 tests) are BOTH already DONE. The
analyzer now *produces* rich metadata but nothing *consumes* it end-to-end into a ready-to-run config,
and our repack differentiator (CrossOver fails ISDone/Unarc -11) has a PLAN but no runnable gate.
Both are isolated, product-layer, Sonnet-fit, and don't touch the engine.

## MANDATORY discipline
- Use context-mode (`ctx_execute_file`/`ctx_execute`/`ctx_search`) for reading code/logs — not raw
  Read/Bash on big files. Heartbeat EACH step → `reports/research/LANE-PRODUCT-PROGRESS.md`
  (one line `TIME · task · action · result · next`).
- Commit named files only (never `git add -A`): `feat(configurator): ...` / `feat(repack): ...`.
- **Do NOT re-do already-done work.** FIRST inventory the existing pipeline, find the real gap, then close it.

## TASK A — End-to-end auto-config (analyzer → recommendation → bottle/env), tested on REAL games
1. Inventory what already exists end-to-end: `compatibility.py`, `bottles.py`, `engines.py`,
   `launcher.py`, `rosetta.py`, `profiles.py`, `__main__.py`. Map the CURRENT flow from "an .exe path"
   to "a chosen bottle + env + engine". Write the gap (what's missing to a working auto-config) into
   LANE-PRODUCT-PROGRESS.md BEFORE coding.
2. Close the gap so this works: `python3 -m app.configurator <path-to-exe>` →
   (a) runs `analyze_full()` (arch, engine FP, anti-cheat flag, dynamic imports, SHA),
   (b) matches a profile via `profiles.py` lookup (SHA → name → engine/arch fallback to
   `game-generic-dx11/dx9/dx12`), (c) emits a concrete recommendation: target lane (x64-EC / PE32),
   DLL overrides, env vars, bottle settings, and an explicit "unsupported: kernel anti-cheat" refusal
   when note-117 kill-filter trips. Output both human-readable + a machine JSON.
3. Validate on the user's REAL downloaded titles (do NOT assume paths — discover them):
   - AI War 2 (GOG, x64) — the user noted a path under `.../game-ai.war.2-(91319)`; locate the .exe.
   - Hollow Knight, plus 2–3 more from `profiles/` (cuphead, ori, outer-wilds…) if present locally.
   Paste each analysis + recommendation into LANE-PRODUCT-PROGRESS.md as evidence (real numbers).
4. Tests: extend `app/configurator/tests/` with end-to-end cases (analyze→match→recommend) using
   small fixtures or the real exes; `python3 -m py_compile app/configurator/**/*.py` clean.

## TASK B — Repack CRC self-validating stress-harness (the differentiator gate)
Source of truth: `reports/research/GEMINI-repack-support-plan.md` (§5 self-validating test design,
§6 test ladder). Build `tools/repack/` (NEW dir, yours):
1. A harness `tools/repack/run_repack_smoke.sh` (+ a small Python driver) that takes a repack/archive
   and: extracts it (native arm64 `unarc` at `/tmp/freearc-build/unarc/unarc` for open formats; or
   the Windows decompressor under MacRunner for lolz once PE32 is ready) and **CRC-verifies** the
   output against a reference manifest. PASS = every extracted file matches reference CRC. This is an
   OBJECTIVE i386 CPU-correctness gate (unlike "game looks fine").
2. Implement the ladder rungs as separate invocations: (a) native `unarc` open-format smoke (works
   today), (b) simple Inno/NSIS (no lolz), (c) FreeArc repack (no lolz), (d) full lolz repack —
   each gated behind a flag; (a) must pass NOW, (b)-(d) marked `BLOCKED on PE32 Phase 4` with a clear
   skip message (no fake PASS).
3. Emit a triage-style verdict line (PASS/FAIL + which rung + CRC mismatch detail) so it can later be
   wired into CI next to the triage analyzer.
4. Do NOT reimplement lolz; the plan is to run the Windows decompressor as an x86 program under PE32.
   Document the exact PE32/Lane C dependencies (already in GEMINI-repack-support-plan §3) in
   `tools/repack/README.md` and cross-link to PE32 Phase 4.

## DONE WHEN
`python3 -m app.configurator <exe>` gives a correct lane+env+override recommendation for ≥3 real
titles (with anti-cheat refusal working); rung (a) of the repack CRC harness passes on a real
open-format archive; rungs (b)-(d) cleanly report BLOCKED-on-PE32 (no fake PASS); tests + py_compile
green; everything heartbeated in LANE-PRODUCT-PROGRESS.md. Commit named files only.
