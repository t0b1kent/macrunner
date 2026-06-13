# Lane A Needs

## 2026-06-04 - Lane C ntdll rebuild regression blocks Lane A litmus launch

Lane A TSO litmus `mp` passed before the Lane C ntdll changes when running from the
Lane A checkpoint baseline:

- PASS: `reports/phase4-hollow-knight/run-20260604-092108-tso-litmus-mp-envmode`
- Baseline commit: `49fd1fc checkpoint(Lane A): enforce JIT TSO memory ordering`
- Reached `macrunner-hb-ldr-init: phase=after-loader`, then heartbeat and `PASS mp`

After `HEAD=5038f2d`, rebuilding/reinstalling `ntdll.so` from current sources causes
the same clean Lane A baseline to park before `phase=after-loader`:

- FAIL/timeout: `reports/phase4-hollow-knight/run-20260604-101820-tso-litmus-mp-clean-baseline-120`
- Last lines: `phase=before`, `xtajit64 ProcessInit/ThreadInit completed`, then timeout
- Heartbeats: `0`

Current diff since `49fd1fc` in Lane C-owned files is limited to:

- `engine/wine/dlls/ntdll/unix/system.c`
- `engine/wine/dlls/ntdll/unix/virtual.c`

Lane A request for Lane C: validate the `virtual.c` 4GB-boundary ENOMEM-as-occupied
change against the x64 HyperBridge loader path. It currently prevents the litmus exe
from reaching `after-loader` after a current-source ntdll rebuild.

## 2026-06-07 Lane A -> Graphics/DXGI need: CREATE_DXGI_FACTORY_MISSING after SEH repair
- Run: `reports/phase4-hollow-knight/run-20260607-092356-laneA-heartbeat-module`.
- Lane A fixed/committed ARM64EC syscall-frame boundary: `50c1eea Lane A: repair ARM64EC syscall data boundary`.
- Evidence: `GFXDEVICE_SEH_HOST_BOUNDARY_BEFORE_D3D11` is gone (`macrunner-hb-seh-host-boundary-detail=0`, `c0000026=0`), window gate PASS, `GfxDevice` reached once.
- New triage primary: `CREATE_DXGI_FACTORY_MISSING`; `CreateDXGIFactory*` count is 0 and `D3D11CreateDevice` count is 0.
- Module heartbeat after repair reaches graphics-owned DLL init chain:
  - `d3d11.dll`: rva `0x189db0`, `0x189e30`, `0x11e0`
  - `DXGI.DLL`: rva `0x82770`, `0x827f0`, `0x11e0`
  - `winemetal.dll`: rva `0x37b0`, `0x3830`, `0x11e0`
- Request: graphics owner should trace DXGI/winemetal init/export path and why no `CreateDXGIFactory*` call/export resolution is observed before timeout.

## 2026-06-13 - LDR walk epoch hardening follow-up

Pre-commit Lane A hardening bounded the local LDR list walks and rejects obviously
invalid nodes. Full fix still needed: add an epoch/snapshot discipline for
`macrunner_hb_ldr_entry_from_pc()` and related loader-list reads so signal-adjacent
module lookup cannot observe a mutating LDR list mid-update.
