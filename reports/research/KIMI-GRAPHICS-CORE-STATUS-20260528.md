# Graphics Core Status

**Role:** Implementer/Owner MacRunner Graphics Core
**Mandate:** Ship code → real pixel. No reports. CPU engine (HB/Wine) read-only.
**Date:** 2026-05-28

## Verified Milestones
1. **x86_64 DXMT PE frontend deployed** — `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, `winemetal.dll` active in `prefix-npp-x64-current/system32`.
2. **Bridge workaround confirmed** — `winemetal_bridge.dll` (ARM64 copy of `winemetal.dll`) loads, its `DllMain` succeeds, and x86_64 `winemetal.dll` borrows its unixlib handle via `NtQueryVirtualMemory(MemoryWineUnixFuncs)`.
3. **D3D11 device creation succeeds** — `dx11_tri_x64_nocrt.exe` prints `info:  Maximum supported feature level: D3D_FEATURE_LEVEL_11_1`.
4. **Native macOS window created** — Wine display driver opens window titled "02. Drawing a Triangle".

## Active Blockers (Engine Read-Only)
- **`dx11_clear_present_x64.exe`**: CRT init crashes at `pc=0x140001cc0` with `UNSUPPORTED_OPCODE` (`blocks=1c steps=74`). Patched `call r12/r13/r15` → NOP; crash signature unchanged. Likely another unsupported SSE or indirect-call instruction in the CRT path.
- **`dx11_tri_x64_nocrt.exe`**: Runs ~816K steps, then crashes at `pc=0x7ffd07b13ab` (ntdll `.text` offset `0x613ab`) with `EXEC_FAULT`. Root cause: Wine WOW64 x86_64 `ntdll.dll` contains ARM64 instructions in `.text`; HB x64 cannot execute them.

## Window Capture Status
- Window bounds probe shows `y: 2022, height: 33` (off-screen, title-bar only).
- `screencapture -l` and `CGWindowListCreateImage` both return empty frame / no valid backing store.
- CW_USEDEFAULT patched to `0,0` in nocrt exe — Wine macOS driver still places window off-screen.

## Next Action
Run `dx11_tri_x64_nocrt.exe` with concurrent screen capture during the ~10-15 s stable execution window before the ntdll crash. Verify if any pixel output appears on the desktop before the engine fault.
