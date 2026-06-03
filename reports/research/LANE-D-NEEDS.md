# Lane D Needs

Updated: 2026-06-03

This file records cross-lane needs only after Lane D has exhausted in-scope graphics workarounds. Lane D continues with DXMT coverage using the isolated smoke prefix and mixed binding path.

## Native `winemetal=n` Unix-func registration

Status: tracked gap, not a Lane D stop.

Evidence:

```text
WINEDLLOVERRIDES=d3d11,dxgi,winemetal=n
winemetal_init_unix_call status=0xc0000135
LoadLibraryExW(winemetal) module=0 gle=1114
```

Lane D attempts completed:

- Installed built `d3d11.dll`, `dxgi.dll`, `winemetal.dll`, and `winemetal.so` into `artifacts/dxmt-smoke-prefix/drive_c/windows/system32`.
- Added prefix-local app-dir copies and `WINEDLLPATH` / `WINESYSTEMDLLPATH` bindings.
- Added a prefix-local builtin overlay with `MACRUNNER_DXMT_ROOT`.
- Built raw native DXMT DLLs and separately postprocessed `winemetal.dll` as builtin.
- Landed mixed binding: raw native `d3d11.dll`/`dxgi.dll` plus builtin `winemetal.dll`, all from the Lane D prefix overlay.

Need:

Native ARM64 PE loading needs a supported way to register `winemetal` Unix funcs for raw `winemetal.dll`, or the existing `MACRUNNER_DXMT_ROOT` Unix-func fallback needs to apply to the native ARM64 `NtQueryVirtualMemory(MemoryWineUnixFuncs)` path. Lane D will keep the mixed binding path green while this is resolved.

## Real Hollow Knight Unity present blocked before D3D/DXGI

Status: external Lane A/C runtime blocker; Lane D continues other graphics backlog.

Evidence:

```text
reports/phase5-hollow-knight/run-20260603-171554-hk-dxmt-fresh-prefix/summary.txt
DXMT sync: PASS (d3d10core,d3d11,dxgi,winemetal copied from engine/graphics/dist/dxmt/x86_64-windows)
D3D/DXGI reached: no
Observed blocker: repeated HyperBridge/SEH EXCEPTION_INVALID_DISPOSITION before any D3D/DXGI marker
```

Lane D attempts completed:

- Warm real HK run with DXMT DLLs synchronized into the x64 prefix.
- Fresh-prefix HK run after DXMT sync.
- Low-log HK run to exclude log-volume/runtime trace noise.
- Separate x64 DXMT binding smoke proves `winemetal.dll`, `DXGI.DLL`, and `d3d11.dll` load native from the synchronized graphics prefix.
- In-scope substitutes are green: owned headless/live/fullscreen DXMT smokes and asset-aware HK Unity DXBC corpus extraction.

Need:

Fix the pre-D3D HyperBridge/SEH invalid-disposition/callback path so the real x64 Unity process reaches `CreateDXGIFactory` / `D3D11CreateDevice`. Lane D cannot edit `ntdll` or `hb_*`; once the process reaches D3D/DXGI, Lane D can resume real-game present/correctness validation.

## VKD3D native D3D12 load bypassed by builtin dependency routing

Status: external Lane C/A loader blocker; prefix deployment remains green.

Evidence:

```text
reports/phase5-vkd3d/run-20260603-200745-arm64-winedllpath/d3d12-create-device-arm64-winedllpath.log
engine/graphics/dist/vkd3d/aarch64-windows/d3d12.dll copied into system32
macrunner_hb_open_native_builtin_dependency MacRunner HyperBridge builtin dependency "d3d12.dll"
  => engine/wine/dist-arm64ec-spike/lib/wine/aarch64-windows/d3d12.dll
```

Lane D attempts completed:

- Built VKD3D after fixing the local glslang dylib IDs.
- Deployed `d3d12.dll` and `d3d12core.dll` into isolated aarch64/x64 graphics prefixes.
- Verified prefix byte matches with `engine/graphics/scripts/run_vkd3d_prefix_sync_smoke.sh`.
- Tried app-local/system32/WINEDLLPATH native-load variants.

Need:

The loader/builtin dependency path must honor the graphics prefix/app-local native `d3d12.dll` / `d3d12core.dll` before resolving to Wine's builtin `dist-arm64ec-spike` copy. Lane D cannot patch loader or HyperBridge code.
