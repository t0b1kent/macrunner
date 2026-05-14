# STATUS graphics phase 2 - DXVK

Date: 2026-05-14T05:48:49Z
Scope: DXVK Windows DLL build for arm64 and x86_64.

## Gate

Command:

```text
./scripts/build-dxvk.sh
```

Output:

```text
MacRunner graphics: DXVK Windows DLL build
source: /Volumes/MacOS/MacRunner/engine/dxvk
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/aarch64-windows/d3d9.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/aarch64-windows/d3d11.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/aarch64-windows/dxgi.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/x86_64-windows/d3d9.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/x86_64-windows/d3d11.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk/x86_64-windows/dxgi.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
DXVK PASS: /Volumes/MacOS/MacRunner/engine/graphics/dist/dxvk
```

## Result

PASS. DXVK D3D9/D3D11/DXGI DLLs built for both target architectures.

## Limitation

D3D10 is deferred for DXVK because DXVK v1.10.3 D3D10 sources conflict with current llvm-mingw GUID/header declarations. DXMT supplies D3D10 core for the selected v0.1 backend.
