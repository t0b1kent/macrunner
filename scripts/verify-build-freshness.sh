#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$ROOT" <<'PY'
from __future__ import annotations

import glob
import os
import sys
from pathlib import Path

root = Path(sys.argv[1])
failures: list[str] = []


def rel(path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def mtime(path: Path) -> float | None:
    try:
        return path.stat().st_mtime
    except FileNotFoundError:
        return None


def newest(patterns: list[str]) -> Path | None:
    files: list[Path] = []
    for pattern in patterns:
        files.extend(root.glob(pattern))
    files = [p for p in files if p.is_file()]
    if not files:
        return None
    return max(files, key=lambda p: p.stat().st_mtime)


def check_newer(label: str, artifact: str, inputs: list[str]) -> None:
    artifact_path = root / artifact
    artifact_mtime = mtime(artifact_path)
    newest_input = newest(inputs)
    if artifact_mtime is None:
        print(f"FAIL {label}: missing artifact {artifact}")
        failures.append(label)
        return
    if newest_input is None:
        print(f"FAIL {label}: no inputs matched {inputs}")
        failures.append(label)
        return
    if artifact_mtime + 0.001 < newest_input.stat().st_mtime:
        print(
            f"FAIL {label}: {artifact} older than {rel(newest_input)}"
        )
        failures.append(label)
        return
    print(f"PASS {label}: {artifact} >= {rel(newest_input)}")


check_newer(
    "notepad_ui_helper",
    "reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.exe",
    ["reports/phase-h/MILESTONE/smoke-tools/npp_ui_smoke_helper.c"],
)
check_newer(
    "libhyperbridge",
    "engine/hyperbridge/libhyperbridge.a",
    ["engine/hyperbridge/src/*.c", "engine/hyperbridge/include/*.h"],
)
check_newer(
    "ntdll_links_current_hyperbridge",
    "engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so",
    ["engine/hyperbridge/libhyperbridge.a"],
)
check_newer(
    "win32u_dist",
    "engine/wine/dist-pure-arm64/lib/wine/aarch64-unix/win32u.so",
    ["engine/wine/dlls/win32u/*.c", "engine/wine/dlls/win32u/*.h"],
)
check_newer(
    "comctl32_dist",
    "engine/wine/dist-pure-arm64/lib/wine/aarch64-windows/comctl32.dll",
    ["engine/wine/dlls/comctl32/*.c", "engine/wine/dlls/comctl32/*.h", "engine/wine/dlls/comctl32/*.rc"],
)
check_newer(
    "comctl32_v6_dist",
    "engine/wine/dist-pure-arm64/lib/wine/aarch64-windows/comctl32_v6.dll",
    ["engine/wine/dlls/comctl32/*.c", "engine/wine/dlls/comctl32/*.h", "engine/wine/dlls/comctl32/*.rc"],
)


def check_arch_pair(dll: str, max_skew_sec: float = 900.0) -> None:
    """Both PE arches of a Wine DLL must be installed together.
    The x64 guest app loads x86_64-windows; host loads aarch64-windows.
    If one arch is stale (only one reinstalled, or a leftover trace build),
    the app loads a mismatched DLL and aborts on 'unimplemented function'.
    Catches the class that left a traced x86_64 user32.dll installed.
    """
    label = f"arch_pair_{dll}"
    x64 = root / f"engine/wine/dist-pure-arm64/lib/wine/x86_64-windows/{dll}.dll"
    a64 = root / f"engine/wine/dist-pure-arm64/lib/wine/aarch64-windows/{dll}.dll"
    mt_x64, mt_a64 = mtime(x64), mtime(a64)
    if mt_x64 is None or mt_a64 is None:
        missing = "x86_64-windows" if mt_x64 is None else "aarch64-windows"
        print(f"FAIL {label}: missing {missing}/{dll}.dll (install BOTH arches)")
        failures.append(label)
        return
    skew = abs(mt_x64 - mt_a64)
    if skew > max_skew_sec:
        older = "x86_64-windows" if mt_x64 < mt_a64 else "aarch64-windows"
        print(
            f"FAIL {label}: {dll}.dll arch skew {skew:.0f}s > {max_skew_sec:.0f}s "
            f"({older} is stale — reinstall BOTH arches in one make install)"
        )
        failures.append(label)
        return
    print(f"PASS {label}: {dll}.dll arches consistent (skew {skew:.0f}s)")


# Dual-arch consistency for actively-edited render/icon DLLs.
for _dll in ("user32", "shell32", "comdlg32", "comctl32", "gdi32"):
    check_arch_pair(_dll)


def check_prefix_synced(dll: str) -> None:
    """The x64 app runs from the PREFIX, not from dist. `make install` writes
    dist; the prefix has its OWN copies. If the prefix copy is older than dist
    (not re-synced after a rebuild), the app loads a stale DLL -> missing-export
    aborts or c000007b. This is the root of the build-error whack-a-mole.
    Prefix system32 (64-bit) must be >= dist x86_64-windows build.
    SKIP gracefully if the prefix is absent (running outside that context).
    """
    label = f"prefix_synced_{dll}"
    pfx = root / f"artifacts/phase-h/prefix-npp-x64-current/drive_c/windows/system32/{dll}.dll"
    dist = root / f"engine/wine/dist-pure-arm64/lib/wine/x86_64-windows/{dll}.dll"
    mt_pfx, mt_dist = mtime(pfx), mtime(dist)
    if mt_dist is None:
        print(f"SKIP {label}: no dist x86_64 {dll}.dll")
        return
    if mt_pfx is None:
        print(f"SKIP {label}: prefix not present (no system32/{dll}.dll)")
        return
    if mt_pfx + 0.001 < mt_dist:
        print(
            f"FAIL {label}: prefix system32/{dll}.dll older than dist build "
            f"(re-sync prefix from dist before run — app loads from PREFIX, not dist)"
        )
        failures.append(label)
        return
    print(f"PASS {label}: prefix system32/{dll}.dll >= dist build")


# Prefix-vs-dist sync: app runs from prefix, so a stale prefix = stale runtime.
for _dll in ("user32", "gdi32", "shell32", "comdlg32", "kernelbase", "ntdll", "win32u", "comctl32", "comctl32_v6"):
    check_prefix_synced(_dll)

if failures:
    print("build_freshness=FAIL count=%d" % len(failures))
    sys.exit(1)

print("build_freshness=PASS")
PY
