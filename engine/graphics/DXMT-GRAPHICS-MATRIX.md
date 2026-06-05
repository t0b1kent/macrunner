# DXMT Graphics Matrix

Date: 2026-06-05
Scope: owned graphics/DXMT work only. GPU/Wine smokes were not run while HK Lane A holds the GPU lane.

| Track | Entry Point | Status | Evidence |
|---|---|---|---|
| HK zero-latency present | `engine/graphics/scripts/run_hk_zero_latency_present_harness.sh x86_64` | Ready, not run | `engine/dxmt/tests/dx11/dx11_hk_present_probe.cpp` creates local-DXMT D3D11 device, HWND swapchain, one present, and staging readback. x86_64 llvm-mingw syntax check: PASS. |
| Phase 6 hardening | `engine/graphics/scripts/run_dxmt_phase6_hardening_suite.sh`; CI wrapper `engine/graphics/scripts/run_dxmt_d3d11_stability_smoke.sh` | Ready, not run | Wrappers require stability, residency, leak, command-batch, and multithread markers across repeated long runs. `DXMT_STABILITY_LEAK_BUDGET_KB` now forwards to `DXMT_SMOKE_LEAK_BUDGET_KB` and requires `UnityLeakProbe result=PASS`. |
| D3D9 to DXMT(D3D11) seed | `engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` | CPU PASS | D3D9 device/swapchain/FVF/render-state/texture-stage/indexed-triangle/present, FFP WVP transform, texture modulate, texture-alpha combiner, alpha-test cutout, `D3DFMT_INDEX32`, XNA programmable sprite, XNA alpha-blend sprite, and D3D9 format sweep translate to D3D11-like render state. Pytest: 19 passed. Mock smoke: 9 traces PASS, `present_count=1` each, INDEX32 trace `non_background_pixels=1458`, `unsupported_calls=0`. |
| D3D9 to Metal guarded lane | `engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` | Request PASS, GPU not run | Reuses the same nine D3D9 traces with `--backend metal`; guarded by `ALLOW_GPU_SMOKE=1` for real GPU execution. CPU-only `D3D9_METAL_REQUEST_ONLY=1` validates nine `.metal-request.json` contracts carrying `source_api=d3d9`, render/depth formats, shader mode, D3D9 render states, texture-stage states, WVP transform matrix, texture format, `uint16/uint32` index format, present count, vertices, indices, and texture pixels. Native helper consumes request geometry/texture/WVP/index-format data, chooses `MTLIndexTypeUInt16` or `MTLIndexTypeUInt32`, and emits Metal shader variants for programmable texture*diffuse plus FFP `MODULATE`, `MODULATE2X`, `ADD`, `SUBTRACT`, `BLENDDIFFUSEALPHA`, `BLENDTEXTUREALPHA`, alpha-test discard, and alpha-blend output-merger state. Request smoke: PASS; native helper build-only: PASS. |

Next GPU-permitted evidence:

1. Run HK present harness once Lane A reaches D3D11CreateDevice and releases GPU contention.
2. Run Phase 6 hardening suite and record leak delta, presented frames, elapsed ms, residency result, batch result, and multithread result.
3. When GPU is free, run `ALLOW_GPU_SMOKE=1 engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` to collect real Metal PPM/report evidence for the nine D3D9 traces. Until then, use `D3D9_METAL_REQUEST_ONLY=1` for CPU-only Metal request contract checks.
