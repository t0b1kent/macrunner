# STATUS graphics phase 4 - vkd3d

Date: 2026-05-14T05:48:49Z
Scope: vkd3d-proton D3D12 DLL build.

## Gate

Command:

```text
./scripts/build-vkd3d.sh
```

Output:

```text
MacRunner graphics: vkd3d-proton D3D12 build
source: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/vkd3d-proton
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/vkd3d/aarch64-windows/d3d12.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/vkd3d/aarch64-windows/d3d12core.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/vkd3d/x86_64-windows/d3d12.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
/Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/vkd3d/x86_64-windows/d3d12core.dll: PE32+ executable (DLL) (GUI) x86-64, for MS Windows
vkd3d PASS: /Users/timurtoby/Documents/MacRunner/Main/MacRunner/engine/graphics/dist/vkd3d
```

## Result

PASS. D3D12 physical DLLs exist for arm64 and x86_64.
