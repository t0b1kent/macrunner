#!/usr/bin/env python3
from __future__ import annotations
import argparse
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DIST = ROOT / "engine" / "graphics" / "dist"

REQUIRED = [
    DIST / "lib" / "libMoltenVK.dylib",
    DIST / "dxvk" / "aarch64-windows" / "d3d11.dll",
    DIST / "dxmt" / "aarch64-windows" / "d3d11.dll",
    DIST / "dxmt" / "aarch64-unix" / "winemetal.so",
    DIST / "vkd3d" / "aarch64-windows" / "d3d12.dll",
]

def file_text(path: Path) -> str:
    return subprocess.check_output(["file", str(path)], text=True)

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--install-prefix")
    args = ap.parse_args()
    missing = [str(p) for p in REQUIRED if not p.exists()]
    if missing:
        print("missing artifacts:")
        print("\n".join(missing))
        return 1
    for path in REQUIRED:
        print(file_text(path).strip())
    dxmt_strings = subprocess.check_output(["strings", str(DIST / "dxmt" / "aarch64-windows" / "d3d11.dll")], text=True, errors="ignore")
    print(f"dxmt Metal strings: {dxmt_strings.count('Metal')}")
    if "Metal" not in dxmt_strings:
        return 1
    if args.install_prefix:
        marker = Path(args.install_prefix) / "macr-graphics-backend"
        if not marker.exists():
            print(f"missing install marker: {marker}")
            return 1
        print(f"installed backend: {marker.read_text().strip()}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
