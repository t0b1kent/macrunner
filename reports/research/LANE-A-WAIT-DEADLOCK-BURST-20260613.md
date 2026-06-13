# Lane A WAIT_DEADLOCK burst synthesis — 2026-06-13

Scope: read-only/design-only diagnostic burst for Hollow Knight rung-6 stall after clean boot.

Current facts:
- Clean boot state: segv=0, c0000026=0, host-boundary/module_from_pc hot=0, no D3D11/DXGI/GfxDevice.
- Stable progress reaches `Input System module state changed to: Initialized`.
- Recent wait-only traces show `address-wait-before=25`, `address-wake=0`; these are most likely idle Unity job workers unless a producer/wake for the same address is proven.
- Current live samples point at the primary thread in `NtUserCallOneParam(GetSystemMetrics) -> get_virtual_screen_rect -> lock_display_devices -> load_display_driver -> get_desktop_window -> register_builtin_classes -> LoadImageW -> KeUserModeCallback -> macrunner_hb_route_x64_callback_fault -> macrunner_hb_redirect_arm64x_hexpthk_sigill -> virtual_check_buffer_for_read`.

Ranked root classes:
1. Callback/bootstrap wall in win32u/ARM64X `LoadImageW` callback path; current samples support this most directly.
2. Lost wake in split `WaitOnAddress` queues; plausible engine bug, but current evidence only proves waiters, not a missing producer.
3. Thread lifecycle bug: render/Gfx thread not created or created but not started; needs explicit CreateThread/start-entry correlation.
4. Window/desktop bootstrap wait; possible because the stack is in desktop/class registration, but no `NtUserCreateWindowEx`/message-pump evidence yet.
5. Hot-loop/TSO spin disguised as wait; historically relevant, but current samples favor callback/bootstrap.

One-run decisive probe:
- Rebuild and redeploy Lane A's own `ntdll.so` before running; Lane D may have overwritten shared dist.
- Run Hollow Knight once with high wait budget plus thread/window/callback/D3D/process-exit markers and samples at 45/90/135/200s.
- Verdicts:
  - Same wait address has a producer wake in native/Wine traces but no MacRunner wake/return: split/lost wake.
  - CreateThread logs render/Gfx start but no `macrunner_hb_x64_thread_entry`/first guest PC: thread-start bug.
  - No render thread/DXGI and primary samples stay in `LoadImageW -> KeUserModeCallback -> hexpthk`: callback/bootstrap wall.
  - `NtUserCreateWindowEx`/message markers appear before stall: window bootstrap.
  - Repeated hot guest RVA with no blocking wait: hot-loop/TSO path.

Fix rule:
- Implement only the class proven by the decisive probe.
- One edit at a time, then HK ladder gate + 8-boot consistency gate.
- Snapshot immediately after verified-forward landing; commit only named Lane A files, no foreign WIP.
