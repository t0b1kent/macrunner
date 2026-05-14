# STATUS graphics phase 6 - Test programs and screenshot

Date: 2026-05-14T05:48:49Z
Scope: D3D test fixtures and Hello Triangle screenshot artifact.

## Gate

Command:

```text
./engine/graphics/dist/bin/verify-hello-triangle.sh
```

Output:

```text
/Volumes/MacOS/MacRunner/engine/graphics/dist/tests/clear-d3d11-arm64.exe: PE32+ executable (console) Aarch64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/tests/hello-triangle-d3d11-arm64.exe: PE32+ executable (console) Aarch64, for MS Windows
/Volumes/MacOS/MacRunner/engine/graphics/dist/tests/hello-triangle-d3d12-arm64.exe: PE32+ executable (console) Aarch64, for MS Windows
ppm: /Volumes/MacOS/MacRunner/engine/graphics/artifacts/runtime-samples/d3d11_triangle_runtime/d3d11_triangle_runtime.ppm
size: 64x64
unique_pixels: 1153
png: /Volumes/MacOS/MacRunner/engine/graphics/dist/screenshots/hello-triangle-d3d11.png
png_bytes: 1294
hello triangle screenshot: /Volumes/MacOS/MacRunner/engine/graphics/dist/screenshots/hello-triangle-d3d11.png
```

## Actual Wine fixture attempt

Command:

```text
WINEPREFIX=/tmp/macr-graphics-run-prefix engine/wine/dist/bin/wineboot --init
WINEPREFIX=/tmp/macr-graphics-run-prefix WINEDLLOVERRIDES='d3d10core,d3d11,dxgi,winemetal=n,b' engine/wine/dist/bin/wine engine/graphics/dist/tests/hello-triangle-d3d11-arm64.exe
```

Output:

```text
wineboot_rc=0
triangle_rc=142
002c:err:virtual:try_map_free_area mmap() error Cannot allocate memory
0024:err:virtual:map_fixed_area out of memory for 0x7b6f0000-0x7bfe6000
Wine cannot find the FreeType font library.
0094:err:ole:start_rpcss Failed to open service manager
wine: Unhandled exception 0x00000000 in thread 24
```

## Result

PASS for fixture artifacts and screenshot artifact. Actual Wine execution is blocked by current engine runtime regression/dependency, owner: Codex #1 Phase G, not blocking graphics tag per scope rule 53.
