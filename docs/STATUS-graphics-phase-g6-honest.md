# STATUS graphics phase G6 - Honest validation boundary

Date: 2026-05-14
Scope: Clarify the graphics Gamma 6 screenshot artifact boundary.

## Result

Native renderer validation = PASS. The artifact at `engine/graphics/dist/screenshots/native-shader-validation.png` is generated from the native Metal validation path and proves the DXMT shader translation / Metal-side validation pipeline is alive.

Wine+DXMT end-to-end = WAITING on Phase G runtime fix. The real PE loader path through Wine + DXMT DLLs is not represented by this PNG yet.

When Phase G is fixed, the real `.exe` screenshot must be captured as:

```text
engine/graphics/dist/screenshots/hello-triangle-d3d11-via-wine.png
```

## Current blocker

The attempted Wine fixture run initializes `wineboot` but fails in the engine runtime with mmap/FreeType/RPCSS errors. Owner: Codex #1 Phase G. This is not blocking the `v0.1-graphics-arm64-dxmt` infrastructure tag.
