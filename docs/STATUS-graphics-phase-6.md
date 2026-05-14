# STATUS graphics phase 6 - Test programs and native shader validation

Date: 2026-05-14T05:48:49Z
Scope: D3D test fixtures and native shader validation artifact.

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
png: /Volumes/MacOS/MacRunner/engine/graphics/dist/screenshots/native-shader-validation.png
png_bytes: 1294
native shader validation screenshot: /Volumes/MacOS/MacRunner/engine/graphics/dist/screenshots/native-shader-validation.png
wine dxmt e2e screenshot: WAITING on Phase G runtime fix -> /Volumes/MacOS/MacRunner/engine/graphics/dist/screenshots/hello-triangle-d3d11-via-wine.png
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

PASS for fixture artifacts and native shader validation. This screenshot is not a Wine+DXMT end-to-end capture. Actual Wine execution is blocked by current engine runtime regression/dependency, owner: Codex #1 Phase G, not blocking graphics tag per scope rule 53. The future Wine+DXMT screenshot path is `engine/graphics/dist/screenshots/hello-triangle-d3d11-via-wine.png`.
