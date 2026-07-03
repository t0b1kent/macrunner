# TRIAGEFIX-CHECKPOINT

Lane: triagefix · Branch: codex-hk-graphics-prep · 2026-07-04

## Status: GOAL achieved

Root cause of both false positives understood + 2 classes added + rc=53
classifies correctly + regression matrix clean + handoff written.

## Commits (this branch)
- `93ec66f` — sync triage from main (IAT-bind rejection in classify_run +
  current analyzers). BRANCH-ONLY (main already has these).
- `4c7a02c` — add DLL_LOAD_FAILURE + SYSTEM_DLL_INIT_CASCADE classes
  (analyze_dll_load.py + classify_run.py new-class hunks). CHERRY-PICKABLE
  to main.

## False positives fixed
1. IAT-bind marker counted as real call → `_marker_hit` now rejects
   iatentry/cpdecision/cpresult/iatfinal lines. (brought from main)
2. ABZU rc=53 → was `WINED3D_FALLBACK` (FP); now `DLL_LOAD_FAILURE` (correct).
   Root: d3d gate matched a `wined3d.dll` import-prefix line while
   dxgi/d3d11 failed to load (c0000135 + map_fixed_area OOM) before graphics.

## New classes (priority above graphics)
- `DLL_LOAD_FAILURE` (96): graphics DLL c0000135 + map_fixed_area OOM →
  address-space/layout, not graphics.
- `SYSTEM_DLL_INIT_CASCADE` (94): ≥3 helpers c0000005 at same PC in
  loader_init → corrupted/incompatible system DLL (binary-graft).

## Regression matrix
- rc=53: WINED3D_FALLBACK → DLL_LOAD_FAILURE (FIXED).
- 9 saved runs + 16 fixtures: all match current main baseline (no regression).
- A real WINED3D_FALLBACK (graphics ran) is preserved — suppression fires only
  when an earlier DLL-load/init blocker is present.

## Files
- tools/triage/analyze_dll_load.py (new)
- tools/triage/classify_run.py (IAT fix + new-class hunks)
- tools/triage/analyze_window_gate.py + restored analyzers (sync from main)
- reports/graphics-prep/TRIAGEFIX-HANDOFF.md (full handoff + cherry-pick recipe)

## Next for coordinator
Cherry-pick `4c7a02c` to main. See TRIAGEFIX-HANDOFF.md for follow-ups
(tighten analyze_d3d_gate wined3d_loaded trigger; tighten window-gate
load-fail evidence).
