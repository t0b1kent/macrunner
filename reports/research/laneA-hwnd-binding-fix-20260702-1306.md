# Lane A HWND Binding Fix - 2026-07-02 13:06

## Fix

- Added `macdrv_ensure_win_data(HWND)` in `winemac.drv/window.c`.
- `d3dmetal.c` now realizes missing macdrv window data on D3D swapchain bind and re-runs client-surface update/present before returning `client_cocoa_view`.
- `winemetal_unix.c` now logs one full `winemetal[HWND]` field line with symbol pointers, `win_data`, `client_cocoa_view`, `ret_view`, `ret_layer`, and `attached_to_hwnd`.

## Evidence

- Run: `reports/phase4-hollow-knight/laneA-hk-hwnd-bind-fix-125609-try1-125711`
- Binding fields: `win_data=0x72aa25380`, `client_cocoa_view=0x72aa20f00`, `ret_view=0x72aa31b80`, `ret_layer=0x764179770`, `attached_to_hwnd=1`.
- HWND facts: `hwnd=0x20054`, owner/current thread `36`, `root=0x20054`, `is_root=1`, `parent=0x10020`, desktop-parented, style `0x94000000`.
- Swapchain: `CreateSwapChainForHwnd rc=0x0`, `swapchain=0xedcb05c90`.
- Filtered classify: `LADDER_RUNG: 11 (swapchain)`, `D3D_GATE_SUCCESSFUL`.

## Current Frontier

- No real `GetBuffer`/backbuffer marker yet.
- No real swapchain `Present` yet.
- The raw slot-8 call after swapchain is `MakeWindowAssociation(hwnd=0x20054, flags=3)`, not a real Present, despite the generic candidate label.
- Run ended by watchdog `exit=143`, classified as `SILENT_SPIN_NO_MARKERS`.

## Snapshot

- `artifacts/milestone-dist/hk-rung11-swapchain-hwnd-bind-20260702-130629-20260702-130629`
