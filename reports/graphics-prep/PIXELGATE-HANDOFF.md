# PIXELGATE Handoff — lane pixelgate (МОМЕНТ ПИКСЕЛЯ hardened)

Workspace: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-graphics-prep`
Scope: `scripts/pixel-truth-gate.sh`, `tools/triage/classify_run.py`,
`tools/triage/tests/test_pixelgate_ladder.py`, `tools/pixelgate_selftest_*.swift`,
`reports/graphics-prep/**`. Engine/DXMT source NOT touched (Codex scope).

## 1. What was hardened

### 1a. pixel-truth-gate.sh — self-tested end-to-end (fresh session 2026-07-04)

Three-case self-test, all PASS:

| Case | Input | Expected | Got | Artifacts |
|---|---|---|---|---|
| Positive | live AppKit color-bars window (PID) | COLORFUL, exit 0 | COLORFUL, exit 0, colorful=1061261, non_black=1156296 | `reports/graphics-prep/selftest/fresh-live-window/*/window-*.png` + `result.json` |
| Negative 1 | nonexistent PID 999999999 | NO_WINDOW, exit 2 | NO_WINDOW, exit 2 | `reports/graphics-prep/selftest/fresh-no-window/*/result.json` |
| Negative 2 | borderless all-black window (PID) | BLACK, exit 3 | BLACK, exit 3, non_black=0, colorful=0, dominant=#000000 | `reports/graphics-prep/selftest/fresh-black-window/*/result.json` |

The BLACK negative models the HK rung-11 case: a visible window exists but no
frame was drawn. Helpers: `tools/pixelgate_selftest_window.swift` (color bars)
and `tools/pixelgate_selftest_black.swift` (borderless black), compiled to
`artifacts/pixel-truth-gate/bin/pixelgate_selftest_{window,black}`.

**Known limitation (documented, not blocking):** the gate captures the whole CG
window including the title bar. A *titled* window with traffic-light buttons
leaks ~87k non-black pixels and would false-COLORFUL even with a black render.
Mitigation: the gate is fit-for-purpose for the target titles (HK / ABZU are
Unity, borderless/fullscreen) and for borderless windows. For windowed apps with
title bars, capture against a borderless/fullscreen window, or (future) crop to
the client/content rect via the backbuffer texture dimensions.

### 1b. Ladder rungs 12-14 — hard signatures, no raw string-count markers

`tools/triage/classify_run.py` `LADDER_RUNGS`: the old soft rungs
`("present", ["present_reached=YES","Present hr=0","present_count="])` and
`("window-visible", ["window-visible","CG-capture","non_background_pixels"])`
were REMOVED — they were the historical ложняк source (slot-8 /
MakeWindowAssociation false Present, "Present string count" heuristics).
Replaced with the hard trio:

| Rung | Name | Hard marker (log) | DXMT source anchor | Rejects |
|---|---|---|---|---|
| 12 | `getbuffer` | `macrunner-hb-dxgi-swapchain: method=GetBuffer slot=9` (real-method trace, rc=0) | `d3d11_swapchain.cpp:283` | candidate lines, IAT-bind, rc≠0 |
| 13 | `rtv` | `macrunner-hb-d3d-rtv: CreateRenderTargetView rc=0x0` (TRACE-GAP placeholder) | `d3d11_texture_device.cpp:162` | not yet emitted — see §3 |
| 14 | `real-present` | `macrunner-hb-dxgi-swapchain: method=Present slot=8` (real-method trace, rc=0) | `d3d11_swapchain.cpp:278/:784` | candidate lines (slot-8 ложняк), IAT-bind, rc≠0 |

The HB swapchain vtable tracer (`macrunner_hb.c:4623-4755`) emits two shapes:
- REAL (known swapchain only, :4749): `... method=<m> slot=<n> swapchain=<ptr> rc=<ptr> ...`
- CANDIDATE (unconfirmed, :4715, fires ONLY for slot 8/22): `... candidate method=<m> slot=<n> object=<ptr> rc=<ptr> ...`

slot 8=Present, 9=GetBuffer, 13=ResizeBuffers, 22=Present1. The slot-8
"candidate" line is the MakeWindowAssociation ложняк (factory slot 8 misread as
Present). The new `_swapchain_real_call_hit` matcher credits a rung ONLY for a
real-method line (no "candidate"), not an IAT-bind line, with rc=0. GetBuffer
(slot 9) has no candidate path at all, so it is inherently ложняк-proof.

### 1c. PIXEL_MOMENT_REACHED — the only frame credit

The ladder marks CALL-level progress (GetBuffer/RTV/real-Present reached). A
real Present call does NOT prove a pixel was drawn — the first Present can blit
an uninitialized (black) backbuffer (PRESENTPATH-ENGINE-HANDOFF §(e)). The
operator rule "frame on 12-14 gated ONLY via pixel-truth-gate.sh" is enforced by
the orthogonal `PIXEL_MOMENT_REACHED` flag:

```
PIXEL_MOMENT_REACHED = (LADDER_RUNG == 14 real-present) AND pixel_truth_confirmed(run_dir)
```

`pixel_truth_confirmed(run_dir)` scans `<run-dir>/pixel-truth-gate/*/result.json`
(legacy single `result.json` also accepted) for verdict NONBLACK or COLORFUL.
The classifier now prints and persists:

- `PIXEL_TRUTH: CONFIRMED|NOT_CONFIRMED|NO_ARTIFACT verdict=<v> artifact=<path>`
- `PIXEL_MOMENT_REACHED: yes|no`
- `!! PIXEL_PENDING: ...` warning when rung 14 marker is present but no pixel artifact confirms a frame.

## 2. How lanes invoke the gate (one command)

```sh
# After a run reaches rung 14 (real-Present marker), confirm the frame:
scripts/pixel-truth-gate.sh --pid <wine-PID> --rundir <run-dir>/pixel-truth-gate
# or watch for the first non-black frame (up to 10 min):
scripts/pixel-truth-gate.sh --name "Hollow Knight" --watch --rundir <run-dir>/pixel-truth-gate
```

The artifact lands inside the run dir, so the next `classify_run.py <run-dir>`
automatically picks it up and sets `PIXEL_MOMENT_REACHED: yes` when rung 14 +
COLORFUL/NONBLACK coincide. Lanes must NOT credit a pixel from raw
`Present hr=0` / `present_count=` / `non_background_pixels` markers — only the
gate artifact counts.

## 3. RTV trace gap (action for Codex / engine lane)

Rung 13 (`rtv`) has NO log trace today. `CreateRenderTargetView` is a vtable
call on `ID3D11Device` (`d3d11_texture_device.cpp:162`); it is not an import
thunk, so `macrunner-hb-d3d-boundary:` never fires for it, and DXMT emits no
fprintf there. The rung is defined with the marker that SHOULD be emitted:

`macrunner-hb-d3d-rtv: CreateRenderTargetView rc=0x0 swapchain=<ptr>`

Until the engine lane instruments that call site, rung 13 never lights from
logs — which is correct (no false positive). The ladder simply reports rung 12
(getbuffer) as the furthest reached when only GetBuffer is traced. When Codex
adds the one-line trace, rung 13 lights automatically — no classifier change
needed.

## 4. Regression matrix — clean

Re-classified with the new ladder; core verdict/class/rung unchanged vs baseline:

| Run | Baseline | After pixelgate | OK |
|---|---|---|---|
| HK rung-11 `laneA-hk-hwnd-bind-fix-125609-try1-125711` | rung 11 swapchain, PRESENT_MISSING, SELF_CHECK PASS | rung 11 swapchain, PRESENT_MISSING, SELF_CHECK PASS, PIXEL_MOMENT no | yes |
| HK rung-9 `laneA-syscall-stall-probe-20260614-044900` | rung 9 dxgi-factory, WINED3D_FALLBACK, SELF_CHECK FAIL | rung 9 dxgi-factory, WINED3D_FALLBACK, SELF_CHECK FAIL, PIXEL_MOMENT no | yes |
| ABZU rc=53 `abzu-dxmtpoll-singlelookup-900` | DLL_LOAD_FAILURE (triagefix fix), rung 1 | DLL_LOAD_FAILURE, rung 1, PIXEL_MOMENT no | yes |

The HK rung-11 log contains the slot-8 candidate ложняк line
(`macrunner-hb-dxgi-swapchain: candidate method=Present slot=8 object=...`) —
with the old soft rung it stayed at 11 (markers didn't substring-match); with
the new hard rung it STILL stays at 11 and is structurally barred from rung 14
(candidate + rc match alone no longer credit, and "candidate" is rejected).

Unit tests: `tools/triage/tests/test_pixelgate_ladder.py` — 11/11 PASS (real
GetBuffer lights 12, real Present lights 14, candidate slot-8 rejected, IAT
rejected, rc≠0 rejected, rtv trace-gap placeholder, pixel_truth
confirmed/black/no-artifact, PIXEL_MOMENT needs both marker+artifact).

Note: classify_run.py regenerates `triage-summary.{json,txt}` inside each run
dir it is pointed at (unchanged existing behavior, same as triagefix lane). The
verdict/class/rung core fields are identical to baseline; the only addition is
the new `PIXEL_TRUTH` / `PIXEL_MOMENT_REACHED` fields.

## 5. Cherry-pick plan into main

Files to cherry-pick into `main` (all tooling/reports, no engine code):

1. `scripts/pixel-truth-gate.sh` (unchanged this session — already in worktree)
2. `tools/triage/classify_run.py` — hard rungs 12-14 + pixel-truth confirmation
3. `tools/triage/tests/test_pixelgate_ladder.py` — new unit tests
4. `tools/pixelgate_selftest_black.swift` — new BLACK negative helper
5. `reports/graphics-prep/PIXELGATE-HANDOFF.md` — this file
6. `reports/graphics-prep/PIXELGATE-CHECKPOINT.md` — updated phase log
7. `reports/graphics-prep/PIXEL-TRUTH-GATE-README.md` — append BLACK case + limitation

Cherry-pick order: `classify_run.py` first (the behavior change), then tests,
then docs. No merge conflict expected with triagefix (disjoint: triagefix
touched the early-blocker demotion block + analyzers; pixelgate touched
LADDER_RUNGS + ladder_rung + main reporting). Both edits coexist in this
worktree already and classify cleanly together.

## 6. Loop status

LOOP-STATUS: GOAL — gate self-tested (positive + 2 negatives), rungs 12-14 hard
with ЖЁСТКИЕ criteria, pixel-truth-gate mandatory for frame credit, regression
clean (rung 11 / rung 9 / rc=53 unchanged), unit tests 11/11, handoff written.
