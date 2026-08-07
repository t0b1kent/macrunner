# Lane A HK Post-Device Diagnosis - 2026-07-02 12:33

Snapshot:
- `artifacts/milestone-dist/hk-rung9-post-device-warm-e8e0-20260702-122625`
- Captures current deployed closure: `dist-arm64ec-spike`, current GLM DXMT dist,
  warm-default runner policy, deployed `ntdll.so` hash family `e8e0...`.

Diagnostic run:
- `reports/phase4-hollow-knight/laneA-postdevice-diag-122847-try1-122847`
- Sample: `sample-hk-75526-postdevice-20260702-123307.txt`
- Counts: real D3D boundary `31`, `D3D11CreateDevice=1`,
  `macrunner-dxmt-fence=3`, `CreateSwapChain=0`, `Present=0`, exit `3`.

Last-call evidence:
- Line 1593: DXMT fence path completed successfully:
  `device CreateFence ENTER`, `CreateFence EXIT hr=0x0 local_kmt=0x0`.
- Lines 1614-1616: Unity calls `CreateDXGIFactory`; return `rc=0x0`,
  factory object `out1=0x10bcdefa0`.
- Lines 1617-1618: factory slot 7 (`EnumAdapters`) returns `rc=0x0`,
  adapter pointer in `out2=0x10464ee20`.
- Lines 1619-1620: release on the temporary factory returns refcount `1`.
- Line 1621: Unity enters factory slot 15
  (`IDXGIFactory2::CreateSwapChainForHwnd`) with `hwnd=0x20054`,
  desc `a3=0x117d8de90`, fullscreen desc `a4=0x117d8de78`,
  ppSwapChain `a6=0x117d8df30`.
- There is no matching `after` marker for slot 15 and no
  `macrunner-hb-dxgi-swapchain: create` marker.
- Line 1625 is the root evidence:
  `winemetal[HWND]: error: hwnd=0x20054 macdrv HWND->CAMetalLayer binding failed;
  refusing silent orphan NSWindow fallback ... returning STATUS_UNSUCCESSFUL.`
- Line 1626: `macrunner-msvcrt-exit: status=0x3`.
- Line 1633: `mr-run exit=3`.

Sample verdict:
- The post-device sample is not a Mono metadata/string throughput wall.
- Main thread is parked in `__wine_main -> CFRunLoopRun -> mach_msg`.
- Many Unity worker threads are parked in `NtWaitForAlertByThreadId`; sample
  also shows `server_wait`.
- No callback storm: `route_x64_callback_fault=0`, `pc_in_executable_section`
  low.
- HB/JIT helper frames exist on side stacks, but the raw last-call trail names
  the blocker: DXMT/Winemetal HWND-to-CAMetalLayer binding failure during
  `CreateSwapChainForHwnd`.

Verdict:
- This is a DXMT/Winemetal HWND binding gap, not an HB warm-cache, JIT
  throughput, or wait/sync bug.
- Next lever: graphics/DXMT GAP-A HWND binding fix path.
