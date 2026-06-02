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
