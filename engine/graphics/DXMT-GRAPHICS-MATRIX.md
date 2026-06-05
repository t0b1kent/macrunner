# DXMT Graphics Matrix

Date: 2026-06-05
Scope: owned graphics/DXMT work only. GPU/Wine smokes were not run while HK Lane A holds the GPU lane.

| Track | Entry Point | Status | Evidence |
|---|---|---|---|
| HK zero-latency present | `engine/graphics/scripts/run_hk_zero_latency_present_harness.sh x86_64` | Ready, not run | `engine/dxmt/tests/dx11/dx11_hk_present_probe.cpp` creates local-DXMT D3D11 device, HWND swapchain, one present, and staging readback. x86_64 llvm-mingw syntax check: PASS. |
| Phase 6 hardening | `engine/graphics/scripts/run_dxmt_phase6_hardening_suite.sh`; CI wrapper `engine/graphics/scripts/run_dxmt_d3d11_stability_smoke.sh` | Ready, not run | Wrappers require stability, residency, leak, command-batch, and multithread markers across repeated long runs. `DXMT_STABILITY_LEAK_BUDGET_KB` now forwards to `DXMT_SMOKE_LEAK_BUDGET_KB` and requires `UnityLeakProbe result=PASS`. |
| D3D9 to DXMT(D3D11) seed | `engine/graphics/scripts/run_d3d9_dxmt_headless_smoke.sh` | CPU PASS | D3D9 device/swapchain/FVF/render-state/texture-stage/indexed-triangle/present, FFP texture modulate, alpha-test cutout, XNA programmable sprite, XNA alpha-blend sprite, and D3D9 format sweep translate to D3D11-like render state. Pytest: 11 passed. Mock smoke: 6 traces PASS, `present_count=1` each, `non_background_pixels=841/1152/1352/1682/2304/2704`, `unsupported_calls=0`. |
| D3D9 to Metal guarded lane | `engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` | Ready, not run | Reuses the same six D3D9 traces with `--backend metal`; guarded by `ALLOW_GPU_SMOKE=1` so it cannot accidentally contend with HK Lane A. Shell syntax check: PASS. |

Next GPU-permitted evidence:

1. Run HK present harness once Lane A reaches D3D11CreateDevice and releases GPU contention.
2. Run Phase 6 hardening suite and record leak delta, presented frames, elapsed ms, residency result, batch result, and multithread result.
3. When GPU is free, run `ALLOW_GPU_SMOKE=1 engine/graphics/scripts/run_d3d9_metal_headless_smoke.sh` to collect real Metal PPM/report evidence for the six D3D9 traces.
