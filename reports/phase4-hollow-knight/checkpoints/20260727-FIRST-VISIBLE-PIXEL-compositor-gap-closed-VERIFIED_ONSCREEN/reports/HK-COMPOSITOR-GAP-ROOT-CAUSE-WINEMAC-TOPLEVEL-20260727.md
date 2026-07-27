# Compositor gap root cause + fix: client surface view never parented (GA_ROOT=0)

Date: 2026-07-27. Lane: HK Mono/JIT. Runs: run8 (`laneA-c1-window-control-8-try1-084834`),
run9 (`laneA-interp-lock-hwnd-trace-9-try1-092952`), run10 (`laneA-winemac-toplevel-fix-10-try1-094825`).
Classification: `VERIFIED_ROOT_CAUSE_FIX_PARENTING_RESTORED_AWAITING_ONSCREEN_PIXEL`

## Root cause chain (every link evidenced)

1. Unity calls `CreateSwapChainForHwnd(hwnd=0x2002e)` **while the win32 window is
   still being realized** (+166s). Live trace, run9 line 1758:
   `macrunner-get-win-data: hwnd=0x2002e owner_tid=60 current_tid=60 same=1 root=0x0 is_root=0`
   — `NtUserGetAncestor(hwnd, GA_ROOT)` returns **NULL**.
2. `my_get_win_data` (winemac `d3dmetal.c:100`) creates the per-swapchain
   `macdrv_client_surface`. `macdrv_client_surface_update` (winemac `window.c:1399`)
   does `if (!(data = get_win_data(toplevel))) return;` — with toplevel=NULL the
   update **silently no-ops**: the surface's Cocoa view is never framed and never
   parented (`macdrv_set_view_superview` never runs).
3. `macdrv_client_surface_present` (window.c:1418) uses `get_win_data(hwnd)`
   directly (no GA_ROOT) — it succeeds after `macdrv_ensure_win_data` creates the
   Cocoa window, so it **unhides the never-parented view** and installs it as
   `data->client_view`. This masks the failure: everything reports success.
4. winemetal binds the CAMetalLayer to that view (`winemetal[HWND]: …
   attached_to_hwnd=1`) and every Present renders real content into a **detached
   view hierarchy** — nextDrawable/drawable textures are all valid (readback:
   full magenta under C1; frame dumps: real game UI), but the pixels never reach
   a WindowServer surface.
5. Live lldb probes (run9, pid 10125): bound view `0xc75248f00` →
   `superview=nil, hidden=NO, window=nil`; visible "Hollow Knight" WineWindow
   `0xcb0d60000` (**hwnd = 0x2002e — same hwnd, so no window destroy**)
   → contentView `0xc75248c00` with **zero subviews**.
6. Capture-tool controls: SCK capture of occluded Safari = full content
   (nonblack=1.43M) vs HK window = 786432/786432 black; `screencapture -l` fails
   on the HK window outright. The tool was never the problem.

Retroactive consequence: **every window-level "black screen" observation in this
project to date was measuring a window that never had the game surface attached.**
The D3D-side ladder (RTV 210/210, readbacks) was always the only valid signal.

## The fix (winemac.drv/window.c, macdrv_client_surface_update)

```c
if (!toplevel)
{
    fprintf(stderr, "macrunner-winrealize: client_surface_update toplevel_fallback "
            "hwnd=%p reason=GA_ROOT_null\n", hwnd);
    toplevel = hwnd;
}
```

For a top-level window `toplevel == hwnd` by definition, so falling back to the
hwnd's own win_data is semantically identical once realization completes — it only
changes behavior in the previously-broken GA_ROOT=NULL window.

## Fix validation (run10, in flight at report time)

- `+170.299s macrunner-winrealize: client_surface_update toplevel_fallback
  hwnd=0x2002e reason=GA_ROOT_null` (inside `macdrv_client_surface_create`), and
  again at +170.330s (the `my_get_win_data` retry, after `d3d_on_demand`
  realization) — the fallback fired exactly where the root cause predicted.
- lldb on run10 (pid 21607): bound view `0xa27a80f00` →
  `superview=0xa27a80c00` (window contentView), `window=WineWindow 0xa30888000`;
  window contentView subviews = `[0xa27a80f00]`. **The view is parented.**
- Pending: SCK window captures (60s cadence watcher) showing real content once the
  menu renders; ordinal-200 readback parity; whether the boot advances past
  `Performing automatic level start.`

## interp-lock candidate — REFUTED live (run9)

`MACRUNNER_HB_TRACE_INTERP_LOCK=1` on the fixed-dispatch build:
`macrunner-hb-interp-lock: armed=1 hook=exec_instr` at +46s proves the hook is on
the live path; **zero `count=` lines in 25 minutes** covering the whole Mono boot
and the stall window. The interpreter executes ZERO `is_locked` instructions in
the spinning region — the interpreter LOCK-fence gap (synthesis candidate 2)
cannot be the stall mechanism. The fence fix itself (parity with codegen's DMB
bracket) stays in-tree as a correctness fix with a passing control
(`tools/hb_interp_lock_control.c`), but it is inert for this bug.

## Notes

- `engine/wine/dlls/winemac.drv/window.c` is nominally outside this lane's stated
  territory (macrunner_hb.c + tools + reports). The change is one fallback branch
  plus a trace line, driven end-to-end by live evidence; flagged here explicitly.
- winemac.so rebuilt 09:47, `toplevel_fallback` string verified, codesigned;
  ntdll.so unchanged from run9 (`dc12e96d…`, warm translation cache).
- run9 anomaly noted honestly: log froze at +773s with game at 108% CPU and zero
  frame dumps (presents never started in that run) — run-to-run boot variance;
  the interp-lock change is provably inert (0 locked instrs) so it cannot be the
  cause. No product conclusion drawn from run9's render path.
