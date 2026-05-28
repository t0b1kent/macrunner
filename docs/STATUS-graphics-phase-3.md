# STATUS graphics phase 3 - DXMT

Date: 2026-05-14T05:48:49Z
Scope: DXMT Metal-backed D3D10/D3D11/DXGI build.

## Gate

Command:

```text
./scripts/build-dxmt.sh
```

Output:

```text
MacRunner graphics: DXMT Metal backend build
source: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/dxmt
llvm: 15.0.7
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/aarch64-windows/d3d10core.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/aarch64-windows/d3d11.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/aarch64-windows/dxgi.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/aarch64-windows/winemetal.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/aarch64-unix/winemetal.so: Mach-O 64-bit dynamically linked shared library arm64
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/x86_64-windows/d3d10core.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/x86_64-windows/d3d11.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/x86_64-windows/dxgi.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt/x86_64-windows/winemetal.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
DXMT d3d11 Metal strings: 71
DXMT PASS: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/dxmt
```

## Result

PASS. DXMT is the selected v0.1 backend.

## Limitation

x86_64 PE DLLs build; x86_64 Unix-side `winemetal.so` install is skipped until Phase G provides matching x86_64 Wine Unix libraries.
