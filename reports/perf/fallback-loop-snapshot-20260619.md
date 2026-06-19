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
