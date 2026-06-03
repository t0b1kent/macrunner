# D3D8/D3D9 Future Path Note

This is a future-lane note only. Lane D should not build the D3D8/D3D9 path until
the D3D11 DXMT->Metal route is stable on real Unity/game workloads.

## Target Shape

- Keep Wine's D3D8/D3D9 API surface as the frontend contract.
- Prefer D3D8->D3D9 thunking for D3D8 titles, then one D3D9 translation path.
- Translate D3D9 to DXMT-owned Metal directly or through the mature DXMT D3D11
  resource/pipeline backend if that gives a smaller, testable bridge.
- Do not route through Rosetta-only, GPTK-only, or external Windows GPU stacks.
- Treat packaged DXVK/D3D9 as an interim compatibility route only. The native
  product path should converge on D3D8/9 frontend semantics over a MacRunner-owned
  Metal backend or a narrow D3D9->DXMT11 bridge.

## Required Semantics

- Device lost/reset, swapchain reset, fullscreen/windowed transitions, gamma ramp,
  cursor, and presentation timing.
- State blocks, render states, texture-stage state, sampler state, clip status, and
  SetTransform/GetTransform round trips.
- D3DPOOL_DEFAULT/MANAGED/SYSTEMMEM behavior, Lock/Unlock, UpdateTexture,
  dynamic vertex/index buffers, and render-target/depth-surface lifetimes.
- Fixed-function transforms, lighting, fog, texture-stage combiners, alpha test,
  clip planes, point sprites, and user clip state.
- Shader Model 1.x/2.x/3.x bytecode, constant registers, declarations, predication,
  and fixed-function-to-shader lowering.
- Common formats including A8R8G8B8/X8R8G8B8, R5G6B5, A1R5G5B5, D16/D24S8,
  paletted/legacy texture uploads where Wine exposes them, and MSAA resolve.

## Validation Ladder

- D3D9 create-device/reset/lost-device smoke with present/readback.
- D3D9 fixed-function triangle, textured quad, alpha-test quad, fog, and lighting.
- D3D9 dynamic vertex/index streaming with DISCARD and NOOVERWRITE locks.
- D3D9 shader-model 2/3 draw, render-to-texture, depth sampling, and reset smoke.
- D3D8 thunk smoke with a fixed-function textured draw.
- Real-game target later: GTA Vice City class apps after D3D11 coverage is stable.

## Prerequisites

- DXMT D3D11 headless smoke remains green through feature levels 10_0-11_1,
  resources, views, shader stages, MSAA, mips, queries, and stability loops.
- PE32/WOW64 is stable enough for 32-bit-era D3D8/D3D9 titles before using real
  game targets as graphics validation.
- A D3D9 coverage matrix exists before implementation starts.
- A future lane owns D3D8/D3D9 files explicitly; Lane D should only revisit this
  note when ownership expands or the operator assigns that work.

## Current Gate

As of the 2026-06-03 Lane D checkpoints, the DXMT D3D11 owned smoke suite is green
across device creation, resources, shader stages, draw/dispatch, present/readback,
fullscreen, MSAA, queries, deferred contexts, and stability. D3D8/D3D9 work should
still wait until the real x64 Unity path reaches D3D11/DXGI and produces pixels, and
until the PE32/WOW64 lane can run 32-bit game targets without CPU/loader blockers.
