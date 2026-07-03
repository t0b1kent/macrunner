# PIXELGATE CHECKPOINT

Lane: pixelgate
Workspace: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-graphics-prep`
Rule: first action each iteration is to read this file; update after every step.

## Phase 1 - Pixel Truth-Gate Harness

- [x] Inventory existing CG Swift helpers.
- [x] Implement universal `scripts/pixel-truth-gate.sh`.
- [x] Handle process/window/PID input, largest visible window selection, Retina metrics, JSON line output, PNG evidence, and `--watch`.
- [x] Self-test live macOS window: expect `NONBLACK` or `COLORFUL`.
- [x] Self-test nonexistent target: expect `NO_WINDOW`.
- [x] Document command, JSON fields, and exit codes.

## Phase 2 - DXMT Coverage Gap Matrix

- [x] Gather real HK/Unity D3D11/DXGI calls from main reports read-only. Inputs located.
- [x] Gather ABZU IAT/bind API list from MacRunner-abzu read-only. Inputs located.
- [x] Map APIs against local `engine/dxmt/src` implementation status with file:line.
- [x] Write `reports/graphics-prep/DXMT-COVERAGE-GAP-MATRIX-20260702.md`.

## Iteration Log

- 09:06 - checkpoint missing; initialized phase checklist. Next: inventory helpers and repo layout.
- 09:07 - inventory complete: found `tools/cg_window_probe.swift`, `tools/cg_window_capture.swift`, and existing `scripts/window-capture-verdict.sh`; worktree has pre-existing unrelated edits. Next: inspect helper CLIs.
- 09:11 - implemented executable `scripts/pixel-truth-gate.sh`; `bash -n` PASS. Next: run `NO_WINDOW` self-test.
- 09:11 - `NO_WINDOW` self-test PASS: rc=2, JSON `reports/graphics-prep/selftest/no-window/20260701T231129Z-82130/result.json`, probe `reports/graphics-prep/selftest/no-window/20260701T231129Z-82130/probe.json`. Next: run live-window self-test.
- 09:12 - added `tools/pixelgate_selftest_window.swift` AppKit color-bars helper for isolated live-window self-test. Next: compile and capture it by PID.
- 09:12 - disk guard PASS (33GiB free) and self-test helper compiled to `artifacts/pixel-truth-gate/bin/pixelgate_selftest_window`. Next: run live-window gate capture.
- 09:13 - live-window self-test PASS via `--watch`: rc=0, verdict `COLORFUL`, metrics total=1696512 non_background=1156274 non_black=1156296 colorful=1061261 dominant=#000000, PNG `reports/graphics-prep/selftest/live-window/20260701T231310Z-90331/window-35617.png`, JSON `reports/graphics-prep/selftest/live-window/20260701T231310Z-90331/result.json`. Next: write README.
- 09:14 - README written at `reports/graphics-prep/PIXEL-TRUTH-GATE-README.md`; artifact verification PASS for both JSON files and PNG signature. Phase 1 complete. Next: gather Phase 2 HK/ABZU/DXMT inputs.
- 09:14 - Phase 2 file inventory complete: HK logs in main `reports/phase4-hollow-knight`, ABZU logs in `MacRunner-abzu/reports/abzu`, DXMT source in `engine/dxmt/src`. Next: extract API call sets.
- 23:21 - Phase 2 complete: DXMT-COVERAGE-GAP-MATRIX-20260702.md written (155 lines). Top gaps: ID3D11Fence (now implemented, needs live verify), D3D11_FEATURE_OPTIONS4 (missing), CreateGeometryShaderWithStreamOutput (stub), GetFrameStatistics (stub), cross-process swapchain (missing). HK and ABZU call patterns mapped against DXMT source with file:line.
- 23:24 - REVERIFY (fresh session): NO_WINDOW self-test PASS rc=2. COLORFUL self-test PASS rc=0, metrics colorful=1061261, PNG+JSON+probe artifacts present. Matrix file 155 lines, 6 top gaps, all API tables present. README 55 lines. All bins compiled. Both phases confirmed solid.


## Phase 3 - Pixel-gate ladder rungs 12-14 + hardening (2026-07-04, fresh session)

- [x] pixel-truth-gate.sh self-tested end-to-end: positive COLORFUL (live color-bars window, colorful=1061261), negative NO_WINDOW (exit 2), negative BLACK (borderless all-black window, non_black=0). New helper `tools/pixelgate_selftest_black.swift`.
- [x] Ladder rungs 12-14 replaced soft present/window-visible with hard trio: getbuffer (12, real-method slot=9 rc=0), rtv (13, trace-gap placeholder for d3d11_texture_device.cpp:162), real-present (14, real-method slot=8 rc=0, candidate/IAT/rc!=0 rejected).
- [x] `_swapchain_real_call_hit` hard matcher added; `pixel_truth_confirmed(run_dir)` scans `<run-dir>/pixel-truth-gate/*/result.json` for NONBLACK/COLORFUL.
- [x] PIXEL_MOMENT_REACHED = (rung 14 AND pixel_truth_confirmed) — the only frame credit. Reported in stdout, triage-summary.txt, triage-summary.json.
- [x] Unit tests `tools/triage/tests/test_pixelgate_ladder.py` — 11/11 PASS.
- [x] Regression matrix clean: HK rung-11 (swapchain/PRESENT_MISSING), HK rung-9 (dxgi-factory/WINED3D_FALLBACK), ABZU rc=53 (DLL_LOAD_FAILURE) all unchanged. slot-8 candidate ложняк in rung-11 log stays barred from rung 14.
- [x] Title-bar limitation documented (titled windows leak traffic-light pixels; HK/ABZU borderless, not affected).
- [x] Handoff: reports/graphics-prep/PIXELGATE-HANDOFF.md (156 lines).

## Iteration Log (Phase 3)

- 23:21 - fresh session: read checkpoint + AUTOLOOP-PROMPT. Found gate + prior self-test intact. Next: re-self-test + ladder hardening.
- 23:22 - re-ran gate self-test: NO_WINDOW (exit 2) PASS, COLORFUL (exit 0, colorful=1061261) PASS. Next: BLACK negative + ladder.
- 23:23 - added borderless black-window helper; first attempt with titled window leaked traffic-light pixels (false COLORFUL) -> switched to .borderless, BLACK exit 3 PASS (non_black=0). Documented title-bar limitation. Next: ladder rungs.
- 23:27 - studied macrunner_hb.c:4623-4755 swapchain tracer: real-method (:4749, known-swapchain) vs candidate (:4715, slot 8/22 ложняк). Designed hard markers. RTV has no trace -> trace-gap placeholder. Next: edit classify_run.py.
- 23:31 - classify_run.py: replaced soft present/window-visible rungs with getbuffer/rtv/real-present; added _swapchain_real_call_hit (rejects candidate/IAT/rc!=0) + pixel_truth_confirmed + PIXEL_MOMENT_REACHED in stdout/txt/json. Syntax OK. Next: unit tests.
- 23:33 - 11/11 unit tests PASS (real GetBuffer->12, real Present->14, candidate slot-8 rejected, IAT rejected, rc!=0 rejected, rtv trace-gap, pixel_truth confirmed/black/no-artifact, PIXEL_MOMENT needs marker+artifact). Next: regression.
- 23:35 - regression matrix clean: rung-11->11 swapchain PRESENT_MISSING, rung-9->9 dxgi-factory WINED3D_FALLBACK, rc=53->DLL_LOAD_FAILURE. slot-8 candidate in rung-11 log barred from rung 14. Next: handoff + commit.
- 23:38 - handoff + README + checkpoint written. LOOP-STATUS: GOAL.
