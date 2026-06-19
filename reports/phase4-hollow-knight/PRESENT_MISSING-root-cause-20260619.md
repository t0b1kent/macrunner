# Hollow Knight — PRESENT_MISSING root-cause note

Date: 2026-06-19
Runs analyzed:
- `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139`
- `reports/phase4-hollow-knight/laneA-A-fix-1800-try1-115115`

## Triage verdict

All recent A-fix-* Hollow Knight gates end at the same blocker:

- `VERDICT: BLOCKED`
- `CLASS: PRESENT_MISSING`
- `LADDER_RUNG: 9 (dxgi-factory)`
- `LADDER_REGRESSION: no`

This is the current stable frontier for Hollow Knight, not a regression introduced by the (A) reset-pool work.

## Key observation

After the second `D3D11CreateDevice` call (which uses `Flags=0x820`, i.e. `D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT`), Unity logs:

```
GpuFence::Create(): Failed to create ID3D11Fence, error 0x80004005
```

(`0x80004005` = `E_FAIL`).

Immediately after this error the process records several `macrunner-hb-callback-exception-stack` frames and then exits with `rc=143` (timeout/killed). No `Present`, `SwapChain` or frame-render markers follow.

## Interpretation

1. Unity’s graphics worker creates a second D3D11 device with the `VIDEO_SUPPORT` flag (likely for in-game video playback / intro cutscenes).
2. Unity then tries to create an `ID3D11Fence` (D3D11.3 feature) on that device.
3. The active DXMT backend (`engine/graphics/dist/dxmt/*/d3d11.dll`) does not implement `ID3D11Fence` / `ID3D11Device2::CreateFence`; the call returns `E_FAIL`.
4. Unity treats fence creation failure as fatal and never reaches `Present`, so `analyze_d3d_gate.py` classifies the run as `PRESENT_MISSING`.

## Why this is the dxgi-factory blocker

The ladder has already progressed through:
- process start
- window creation
- DXGI factory creation (`CreateDXGIFactory` / `CreateDXGIFactory1` / `CreateDXGIFactory2`)
- first `D3D11CreateDevice` (flags `0x20`, success)

It stops at the **second** `D3D11CreateDevice` + `ID3D11Fence` creation, which happens while the ladder is still attributed to rung 9 (`dxgi-factory`).

## Next possible fixes

1. **DXMT: implement `ID3D11Fence` stub** — minimal implementation that returns a no-op fence object and reports `S_OK`. This is the cleanest fix and lives in the DXMT backend, outside this repo.
2. **Wine/DXMT overlay: strip `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`** — intercept `D3D11CreateDevice` and clear the video-support flag before forwarding to DXMT. Unity would then fall back to a non-video code path, possibly avoiding the fence. Risk: video playback may break silently.
3. **Unity launch option workaround** — try flags such as `-novid`, `-nointro`, or engine-specific settings that disable intro videos. If effective, it moves the game past the fence failure without code changes.
4. **Flight recorder capture with `MACRUNNER_FLIGHT_RECORDER=1` and `MACRUNNER_HB_TRACE_D3D=1`** — confirm the exact COM interface queried (`ID3D11Fence`, `ID3D11Device2`, etc.) and the return path.

## Files to inspect

- `reports/phase4-hollow-knight/laneA-A-fix-live2-try1-131139/run.log`
- `reports/lane-a/A-fix-live2/classify.log`
- `tools/triage/analyze_d3d_gate.py`

## Conclusion

The `PRESENT_MISSING` classification for Hollow Knight is currently a **downstream symptom** of the missing `ID3D11Fence` support in the DXMT backend. The (A) reset-pool change is unrelated. The next actionable item belongs to Lane C / graphics: either add `ID3D11Fence` to DXMT or find a Wine-side workaround that prevents Unity from requesting it.
