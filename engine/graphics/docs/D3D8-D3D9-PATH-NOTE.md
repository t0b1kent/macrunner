# D3D8/D3D9 Future Path Note

This is a future-lane note only. Lane D should not build the D3D8/D3D9 path until
the D3D11 DXMT->Metal route is stable on real Unity/game workloads.

## Target Shape

- Keep Wine's D3D8/D3D9 API surface as the frontend contract.
- Prefer D3D8->D3D9 thunking for D3D8 titles, then one D3D9 translation path.
- Translate D3D9 to DXMT-owned Metal directly or through the mature DXMT D3D11
  resource/pipeline backend if that gives a smaller, testable bridge.
- Do not route through Rosetta-only, GPTK-only, or external Windows GPU stacks.

## Required Semantics

- Device lost/reset, swapchain reset, fullscreen/windowed transitions, gamma ramp,
  cursor, and presentation timing.
- D3DPOOL_DEFAULT/MANAGED/SYSTEMMEM behavior, Lock/Unlock, UpdateTexture,
  dynamic vertex/index buffers, and render-target/depth-surface lifetimes.
- Fixed-function transforms, lighting, fog, texture-stage combiners, alpha test,
  clip planes, point sprites, and user clip state.
- Shader Model 1.x/2.x/3.x bytecode, constant registers, declarations, predication,
  and fixed-function-to-shader lowering.
- Common formats including A8R8G8B8/X8R8G8B8, R5G6B5, A1R5G5B5, D16/D24S8,
  paletted/legacy texture uploads where Wine exposes them, and MSAA resolve.

## Validation Ladder

- D3D9 fixed-function triangle, textured quad, alpha-test quad, fog, and lighting.
- D3D9 dynamic vertex/index streaming with DISCARD and NOOVERWRITE locks.
- D3D9 shader-model 2/3 draw, render-to-texture, depth sampling, and reset smoke.
- D3D8 thunk smoke with a fixed-function textured draw.
- Real-game target later: GTA Vice City class apps after D3D11 coverage is stable.

## Prerequisites

- DXMT D3D11 headless smoke remains green through feature levels 10_0-11_1,
  resources, views, shader stages, MSAA, mips, queries, and stability loops.
- A D3D9 coverage matrix exists before implementation starts.
- A future lane owns D3D8/D3D9 files explicitly; Lane D should only revisit this
  note when ownership expands or the operator assigns that work.
