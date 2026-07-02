# Lane A GLM Latest Post-Swapchain Diagnosis - 2026-07-02 13:48

## DXMT Redeploy

- Source: `/Users/timurtoby/Documents/MacRunner/Main/MacRunner-graphics-prep/engine/graphics/dist/dxmt`
- Source head: `333c038`
- Hash report: `reports/research/dxmt-glm-latest-redeploy-20260702-132040.sha256.txt`
- Deployed hashes:
  - `aarch64-windows/d3d11.dll` -> `6560e9f9f8f2075fe400e972172cbda25948d82fe1fffa36f8050a5f00564579`
  - `aarch64-windows/dxgi.dll` -> `08143054d704ec2b4003995b3c9e34a283811868e960a354ee7b4b05850598a2`
  - `aarch64-unix/winemetal.so` -> `c5c1417e98aa3a4edc258d6c7e847160498d743bfc9a118013f403fa1a353e67`

## Runs

- 420s run: `reports/phase4-hollow-knight/laneA-hk-glm-latest-postswap-132055-try1-132055`
- 900s monitored run: `reports/phase4-hollow-knight/laneA-hk-glm-latest-postswap-long-132919-try1-132920`
- Post-swapchain sample: `reports/phase4-hollow-knight/laneA-hk-glm-latest-postswap-long-132919-try1-132920/sample-hk-postswap-133633.txt`

## Last-Call Evidence

- `CreateSwapChainForHwnd` succeeds:
  - `slot=15`
  - `hwnd=0x20054`
  - `rc=0x0`
  - `swapchain=0xed8b06380`
- HWND binding succeeds:
  - `client_cocoa_view=0x9ff64cf00`
  - `ret_view=0x9ff659b80`
  - `ret_layer=0xa607e4f00`
  - `attached_to_hwnd=yes`
- Last confirmed DXGI call is `MakeWindowAssociation(hwnd=0x20054, flags=3)`, returned `rc=0x0`.
- No `GetBuffer(0)`, no `CreateRenderTargetView`, and no real swapchain `Present` marker.

## End State

- Run ended by watchdog `exit=143`.
- Raw post-swapchain HB diagnostic appears after `MakeWindowAssociation`:
  - `UnityPlayer.dll+0x2b5605`
  - bytes `49 8b 84 f0 88 04 00 00 ...`
  - disasm: `mov rax, qword ptr [r8 + rsi*8 + 0x488]`
  - effective address `0xcfe355638` -> `MEMORY_FAULT`
- Post-swapchain sample is CPU-active in HB infrastructure, dominated by:
  - `macrunner_hb_sync_virtual_region`
  - `hb_memory_protect`
  - `macrunner_hb_try_kernel32_handle_semantic`
- AssetGarbageCollector helper threads are waiting in `NtWaitForSingleObject/server_wait`, but the active stack is HB memory-protect sync.

## Classifier

- Patched `tools/triage/analyze_d3d_gate.py` to ignore `macrunner-hb-dxgi-swapchain: candidate method=Present` lines. Those are unknown-object probes and can be factory `MakeWindowAssociation`, not real swapchain Present.
- Reclassified long run:
  - `LADDER_RUNG: 11 (swapchain)`
  - `PRIMARY_CLASS=PRESENT_MISSING`
  - raw D3D counts: `real_factory=4`, `d3d11_device_markers=1`, `swapchain=2`, `present=0`

## Pixel Gate

- Not run. Real Present did not fire.
