# STATUS graphics master - DXMT/DXVK pipeline

Date: 2026-05-14T05:48:49Z
Base commit: 1315413
Target tag: v0.1-graphics-arm64-dxmt

## Phase Results

- Gamma 1 MoltenVK: PASS
- Gamma 2 DXVK: PASS
- Gamma 3 DXMT: PASS
- Gamma 4 vkd3d: PASS
- Gamma 5 router/installer: PASS
- Gamma 6 test programs/screenshot: PASS with engine-run limitation
- Gamma 7 HUD wired: PASS
- Gamma 8 Control Center backend picker: SKIP, optional and app scope intentionally untouched

## Selected Backend

DXMT arm64 is selected for v0.1. Installed bottle backend marker: `dxmt`.

## Physical Artifacts

- `engine/graphics/dist/lib/libMoltenVK.dylib`
- `engine/graphics/dist/dxmt/aarch64-windows/d3d10core.dll`
- `engine/graphics/dist/dxmt/aarch64-windows/d3d11.dll`
- `engine/graphics/dist/dxmt/aarch64-windows/dxgi.dll`
- `engine/graphics/dist/dxmt/aarch64-windows/winemetal.dll`
- `engine/graphics/dist/dxmt/aarch64-unix/winemetal.so`
- `engine/graphics/dist/dxvk/aarch64-windows/d3d9.dll`
- `engine/graphics/dist/dxvk/aarch64-windows/d3d11.dll`
- `engine/graphics/dist/dxvk/aarch64-windows/dxgi.dll`
- `engine/graphics/dist/vkd3d/aarch64-windows/d3d12.dll`
- `engine/graphics/dist/vkd3d/aarch64-windows/d3d12core.dll`
- `engine/graphics/dist/tests/hello-triangle-d3d11-arm64.exe`
- `engine/graphics/dist/screenshots/hello-triangle-d3d11.png`
- `engine/graphics/dist/hud/out/hud-events.jsonl`

## Vendor Patches

- `engine/graphics/vendor-patches/dxvk/0001-macr-arm64-windows-portability.patch`
- `engine/graphics/vendor-patches/dxmt/0001-macr-apple-silicon-llvm15-link.patch`

## Gates

```text
./scripts/build-moltenvk.sh: PASS
./scripts/build-dxvk.sh: PASS
./scripts/build-dxmt.sh: PASS
./scripts/build-vkd3d.sh: PASS
./engine/graphics/dist/bin/install-graphics.sh /tmp/macr-graphics-dxmt-prefix dxmt: PASS
python3 engine/graphics/tools/verify_graphics_artifacts.py --install-prefix /tmp/macr-graphics-dxmt-prefix: PASS
./engine/graphics/dist/bin/verify-hello-triangle.sh: PASS
python3 engine/graphics/dist/hud/graphics-hud-proxy.py --synthetic 24 --output engine/graphics/dist/hud/out/hud-events.jsonl: PASS
```

## Out-of-scope Safety

No engine/wine source edits were made by this graphics run. The integration is file/env/Process based; no direct linking to Wine or MacRunner engine source was added.

## Exact Remaining Limitations

- DXVK D3D10 is deferred because DXVK v1.10.3 D3D10 source conflicts with current llvm-mingw headers. DXMT provides D3D10 core in this v0.1 stack.
- DXMT x86_64 PE DLLs build, but x86_64 Unix-side `winemetal.so` waits for Phase G x86_64 Wine Unix libraries.
- Actual Wine run of `hello-triangle-d3d11-arm64.exe` currently fails after successful `wineboot` with mmap/FreeType/RPCSS errors. Owner: Codex #1 Phase G; not blocking graphics tag per scope rule 53.
- HUD live capture is parser-ready and synthetic-debug verified; live DXVK HUD capture waits for the same Wine fixture unblock.
- MoltenVK default script uses verified Homebrew native dylib; source rebuild mode is available via `MACRUNNER_MOLTENVK_FROM_SOURCE=1`.
