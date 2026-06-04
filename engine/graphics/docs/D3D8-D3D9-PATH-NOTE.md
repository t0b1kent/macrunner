# D3D8/D3D9 Future Path Note

This is a future-lane note only. Lane D should not build the D3D8/D3D9 path until
the operator assigns older-game ownership and Lane C's PE32/WOW64 path is ready
to run 32-bit-era game targets.

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
  resources, views, shader stages, MSAA, mips, queries, BC formats,
  per-swapchain `ResizeTarget`/`ResizeBuffers`, append-UAV counters,
  D3D11.1 constant-buffer offset/partial-update paths, overlap buffer copies,
  and stability loops.
- x64 guest DXMT binding remains strict-native green for `d3d11`, `dxgi`, and
  `winemetal=n` before older-game routing borrows the same prefix/deployment
  machinery.
- vkd3d/D3D12 prefix deployment continues to gate DXMT `dxgi.dll` dependency
  architecture, factory exports, and standalone `d3d12.dll` runtime loading
  with native `d3d12core.dll`, plus root-signature serialization/deserialization
  roundtrips, because older D3D8/9 routing will reuse the same prefix-copy/export
  hygiene.
- PE32/WOW64 is stable enough for 32-bit-era D3D8/D3D9 titles before using real
  game targets as graphics validation.
- A D3D9 coverage matrix exists before implementation starts.
- A future lane owns D3D8/D3D9 files explicitly; Lane D should only revisit this
  note when ownership expands or the operator assigns that work.

## Current Gate

As of the 2026-06-04 Lane D checkpoints, the DXMT D3D11 owned smoke suite is
green across device creation, resources, shader stages, draw/dispatch,
present/readback, fullscreen, MSAA, queries, deferred contexts, BC formats,
per-swapchain target/buffer resize, append-UAV counter copy, constant-buffer
partial updates, shader-visible constant-buffer offsetting, overlap buffer
copies, and two-pass stability (`artifacts/dxmt-smoke-logs/lane-d-stability-20260604-133145.outer.log`).
Strict-native x64 guest `winemetal=n` binding remains green but still does not
reach D3D11/DXGI markers (`artifacts/dxmt-x64-binding/run-20260604-131356/dxmt-x64-binding.log`).
vkd3d/DXGI prefix dependency gates now include aarch64 standalone `d3d12.dll`
runtime loading with native `d3d12core.dll`, plus legacy and versioned
root-signature frontend roundtrips (`artifacts/vkd3d-prefix-sync/run-20260604-131226`).
Real Unity present is therefore still not a D3D8/D3D9 implementation gate.
D3D8/D3D9 work should wait for future ownership plus Lane C PE32/WOW64 readiness
for 32-bit game targets.
