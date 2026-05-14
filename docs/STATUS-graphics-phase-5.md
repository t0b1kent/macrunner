# STATUS graphics phase 5 - Router and bottle installer

Date: 2026-05-14T05:48:49Z
Scope: backend installer/router using per-bottle DLL copy plus env marker.

## Gate

Command:

```text
rm -rf /tmp/macr-graphics-dxmt-prefix && mkdir -p /tmp/macr-graphics-dxmt-prefix
./engine/graphics/dist/bin/install-graphics.sh /tmp/macr-graphics-dxmt-prefix dxmt
python3 engine/graphics/tools/verify_graphics_artifacts.py --install-prefix /tmp/macr-graphics-dxmt-prefix
```

Output:

```text
/tmp/macr-graphics-dxmt-prefix/drive_c/windows/system32/d3d10core.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/tmp/macr-graphics-dxmt-prefix/drive_c/windows/system32/d3d11.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/tmp/macr-graphics-dxmt-prefix/drive_c/windows/system32/dxgi.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
/tmp/macr-graphics-dxmt-prefix/drive_c/windows/system32/winemetal.dll: PE32+ executable (DLL) (GUI) Aarch64, for MS Windows
graphics backend installed: dxmt
dxmt Metal strings: 71
installed backend: dxmt
```

## Result

PASS. `macr-graphics.env` and `macr-graphics-backend` are written in the bottle. No direct engine linking is introduced; launcher integration remains Process/env based.
