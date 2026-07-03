# Pixel Truth-Gate Harness

Script: `scripts/pixel-truth-gate.sh`

## Commands

```sh
scripts/pixel-truth-gate.sh --pid <PID>
scripts/pixel-truth-gate.sh --name "Hollow Knight" --watch --timeout-mins 10 --interval-sec 5
scripts/pixel-truth-gate.sh "window or process substring" --rundir reports/graphics-prep/pixelgate-runs
```

Each run prints exactly one machine-readable JSON object on stdout. Artifacts are
written under `artifacts/pixel-truth-gate/runs` unless `--rundir` is provided.

## Verdicts

- `NO_WINDOW`: no matching on-screen CG window.
- `BLACK`: capture succeeded, but thresholds classify it as effectively black.
- `NONBLACK`: non-black, non-background pixels are present.
- `COLORFUL`: colored pixels are present.

`--watch` polls until the first `NONBLACK` or `COLORFUL`, then exits 0. If the
timeout expires, it prints the last JSON object with `watch_timeout:true` and
exits 5.

## JSON Fields

- `schema`: currently `pixel-truth-gate.v1`.
- `target`: requested `type`, `value`, and `strict` flag.
- `window`: selected largest visible layer-0 matching CG window, including
  `window_id`, `pid`, `owner`, `name`, logical bounds, alpha, and selection rule.
- `metrics`: `total_px`, capture dimensions, logical dimensions, Retina scale,
  `non_background_px`, `non_black_px`, `colorful_px`, `dominant_color`,
  `dominant_px`, `dominant_ratio`, and thresholds.
- `evidence`: paths for result JSON, probe JSON, capture JSON, PNG evidence, and
  source BMP.
- `verdict`, `exit_code`, optional `error`, optional `watch_timeout`.
- `limitations`: currently documents that minimized windows and windows on
  another macOS Space are invisible to `CGWindowList(.optionOnScreenOnly)`.

## Exit Codes

- `0`: `NONBLACK` or `COLORFUL`.
- `2`: `NO_WINDOW`.
- `3`: `BLACK`.
- `4`: helper compile, capture, or artifact error.
- `5`: watch timeout before `NONBLACK` or `COLORFUL`.
- `64`: usage error.

## Self-Test Evidence

- Negative case: `reports/graphics-prep/selftest/no-window/20260701T231129Z-82130/result.json`
- Live AppKit window: `reports/graphics-prep/selftest/live-window/20260701T231310Z-90331/result.json`
- Live PNG: `reports/graphics-prep/selftest/live-window/20260701T231310Z-90331/window-35617.png`

## Fresh-session self-test (2026-07-04, lane pixelgate)

Three cases, all PASS:

- Positive — live AppKit color-bars window (`tools/pixelgate_selftest_window.swift`):
  COLORFUL, exit 0, colorful=1061261. Artifacts: `reports/graphics-prep/selftest/fresh-live-window/*/`.
- Negative 1 — nonexistent PID: NO_WINDOW, exit 2. Artifacts: `reports/graphics-prep/selftest/fresh-no-window/*/`.
- Negative 2 — borderless all-black window (`tools/pixelgate_selftest_black.swift`): BLACK, exit 3,
  non_black=0, colorful=0, dominant=#000000. Artifacts: `reports/graphics-prep/selftest/fresh-black-window/*/`.

### Known limitation

The gate captures the whole CG window including the title bar. A *titled* window
with traffic-light buttons leaks non-black pixels and can false-COLORFUL even
with a black render. The target titles (HK / ABZU) are Unity borderless/fullscreen,
so this does not bite them; for windowed apps with title bars, capture against a
borderless window or (future) crop to the client/content rect.
