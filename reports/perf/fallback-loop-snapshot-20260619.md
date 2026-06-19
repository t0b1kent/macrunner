# Gate A — Snapshot Report (dfc2148)

Date: 2026-06-19
Commit: `dfc2148`
Tag: `A-fallback-loop-fixed`

## What changed
- `engine/hyperbridge/src/hb_arm64_codegen.c`: route codegen-unsupported (but lifter-valid) IR ops to `emit_interp_ir_helper` instead of returning `HB_ERR_UNSUPPORTED_OPCODE`. The interpreter executes the single op inline, advances PC, and the JIT block continues.
- This kills the fallback-loop that hung HK at `Begin MonoManager ReloadAssembly` / post-PhysX.

## Verification
- 8-boot gate (`A-fallback-8boot`, 180 s main + 8×60 s boots):
  - All 8 boots reached `Mono path`, `Direct3D 11.0`, `Physics::Module`.
  - 0 `jit-fallback` / `UNSUPPORTED_OPCODE` / `c0000005` / `c000007b` markers across all 8 boots.
  - 0 entries in `faults.log`.
- Long run (`A-fallback-long`, 600 s):
  - Reached `Input initialized` and `Loaded All Assemblies` (~118 s), `PhysX Selected backend`.
  - Did not present within TMO; triage class `PRESENT_MISSING` (expected, headless DXMT).

## Current blocker
Post-input-init hang / exit before present. Two candidate causes:
1. `ID3D11Fence::Create` still returns `0x80004005` in this run (DXMT log disabled earlier; re-enabled log did not print the warning, so status unclear — need targeted instrumentation).
2. Headless present path / no real swapchain under `DXMT_HEADLESS=1`.

## Next steps
- Re-enable a minimal safe DXMT fence log or add a return-value probe to confirm `CreateFence=S_OK`.
- If fence is OK: focus on headless present/swapchain harness or run non-headless to check actual frame output.
- If fence still fails: the `af237cc` degrade path is not taking effect in this build/deployment; debug `GetLocalD3DKMT()` / shared-flag path.

## 2026-06-20 follow-up — visible run + CreateFence instrumentation

DXMT commit: `3e4a38b` (submodule `engine/dxmt`)

### Changes
- Instrumented `ID3D11Device::CreateFence` and `dxmt::CreateFence` with `fprintf(stderr, ...)` to trace entry, flags, feature flags, and exit `hr`.
- Removed the `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` early `E_FAIL` return in `d3d11_device.cpp`; VIDEO_SUPPORT devices now fall through to the degraded local-fence path.
- Rebuilt + deployed `d3d11.dll` (x86_64-windows and aarch64-windows) and `winemac.so` (aarch64-unix).
- Rebuilt `winemac.so` with the existing `0001-cocoa-window-arm64-CAMetalLayer-fallback.patch` so `macdrv_functions` is exported on arm64.

### Visible run (`A-visible-fence-4`, 600 s, no `DXMT_HEADLESS`)
- HK booted to `Input initialized` and created a window (`winemetal[HWND]`: real-NSWindow fallback, 1512x982, scale=2.0).
- `CreateFence` called with `Flags=0x0`, `feature_flags=0x820`, returned `S_OK` (`hr=0x0`, `local_kmt=0x0`).
- `MakeWindowAssociation` logged; no `Present`/swapchain/drawable log lines appeared within TMO.
- `window-capture-verdict.sh --watch` did not detect a capturable window in 10 minutes.
- Triage: `PRESENT_MISSING` — now in visible mode, so this is a real blocker, not a headless artifact.

### New blocker: macdrv HWND binding
- `macdrv_functions` is now found by `winemetal.so`, but `get_win_data(hwnd)` returns `NULL` for the game HWND.
- This forces the real-NSWindow fallback (when `DXMT_ALLOW_ORPHAN_WINDOW` is present). Without it, the path would abort.
- Root cause likely: the Wine/macdrv window data for the Unity game window is not populated at swapchain-creation time, or the HWND passed to DXMT is not a macdrv-managed top-level window.
- `aarch64-windows/winemac.drv` rebuild was attempted but failed at link stage (`clang: posix_spawn failed: No such file or directory` via `winegcc`); the existing PE `.drv` is still from an earlier build.

### Next step
Resolve `get_win_data(NULL)` so `macdrv_view_create_metal_view` succeeds on the real game window, then verify `Present` is called and pixels are captured.
