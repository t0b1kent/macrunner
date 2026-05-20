# Visual Regression Dashboard

## Latest App Analyzed

| Field | Value |
|---|---|
| App | Notepad++ |
| Version | 8.7.5 |
| Arch | x64 |

## Latest Capture Paths

| Type | Path | Status |
|---|---|---|
| Toolbar CG capture | `reports/phase-h/npp-x64-20260520-182340/toolbar-cg-capture.bmp` | FAIL (colorful_pixels=316 below floor=900) |
| Full window CG | `reports/phase-h/npp-x64-20260520-183237/full-window-cg.bmp` | Analyzed |
| Window PNG | `reports/phase-h/npp-x64-20260520-183517/window.png` | Analyzed |

## Visual Pass/Fail

| Class | Status | Confidence |
|---|---|---|
| Bottom band | FAIL | HIGH (44-76 px detected in recent runs) |
| Right band | PASS | HIGH (≤3 px, within border tolerance) |
| Scrollbar | UNVERIFIED | LOW (no zoomed capture yet) |
| Toolbar | FAIL | HIGH (colorful_pixels below floor) |
| Statusbar | FAIL | HIGH (black_ratio > max) |
| Folder icons | UNVERIFIED | LOW (no dialog capture yet) |

## Black Band Metrics

| Run | Bottom Band (px) | Right Band (px) | Black Ratio |
|---|---|---|---|
| npp-x64-20260520-182340 | 44 | 2 | 0.0373 |
| npp-x64-20260520-175547 | ~30 | 2 | ~0.04 |
| npp-x64-20260520-133836 | ~76 | 3 | 0.0627 |

## Toolbar Color Metrics

| Run | Colorful Pixels | Floor | Pass/Fail |
|---|---|---|---|
| npp-x64-20260520-182340 | 316 | 900 | FAIL |
| npp-x64-20260520-175547 | 1524 | 900 | PASS (CG) |
| npp-x64-20260520-133836 | 0 | 900 | FAIL |

## Statusbar Metrics

| Run | Black Pixels | Total | Black Ratio | Bad Background |
|---|---|---|---|---|
| npp-x64-20260520-182340 | 25104 | 53520 | 0.469 | YES |
| npp-x64-20260520-133836 | 45231 | 53520 | 0.845 | YES |

## Scrollbar / Folder Icon Status

- **Scrollbar:** No zoomed CG capture available. Needs `main_window_cg_capture` zoomed on right edge.
- **Folder icons:** No dialog CG capture available. Needs Open/Save dialog opened and captured.

## Current Codex Recommended Action

1. **Statusbar / bottom band:** Codex is actively fixing. Do not interfere.
2. **Toolbar:** Use CG as ground truth. If CG ≥ floor but helper shows black → fix helper readback.
3. **Scrollbar:** Capture zoomed CG and run `analyze_capture.py` on scrollbar region.
4. **Folder icons:** Open dialog, capture CG, analyze icon zones.

## Issue Classification

| Issue | Layer | Is Runtime? | Is Harness? |
|---|---|---|---|
| Bottom black band | WINE | YES | NO |
| Toolbar low color | WINE_COMCTL32 / HELPER | maybe | maybe |
| Scrollbar black | WINE_UXTHEME | YES | NO |
| Folder icons black | WINE_COMCTL32 | YES | NO |
| Helper false-negative | HELPER | NO | YES |
| Capture missing | INFRA / HELPER | NO | YES |

## Next Steps

1. Add Windows baseline artifact for all metrics.
2. Capture zoomed scrollbar region CG.
3. Capture Open/Save dialog CG with folder icons.
4. Improve helper bitmap readback or replace with CG validation.
5. Do NOT touch engine/wine/hyperbridge for visual lab work.

---

*Dashboard version: v1*
*Last updated: 2026-05-20*
