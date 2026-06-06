#!/usr/bin/env python3
"""
MacRunner — AI Configurator: Unified PE binary analyzer (facade).

Combines the lightweight stdlib fallback with the deep lief-based
ai/pe_analyzer when lief is available.  Always outputs ExeAnalysis.
ExeMetadata (old API) is kept for backwards compatibility.

Entry points
------------
analyze(path)       → ExeMetadata      (backwards compat, stdlib only)
analyze_full(path)  → ExeAnalysis      (unified: lief if available + string scan)
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any


# PE/COFF константы (https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)
IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64

SUBSYSTEM_NAMES = {
    1: "native", 2: "gui", 3: "console",
    9: "wince_gui", 10: "efi_app", 11: "efi_boot_driver",
    12: "efi_runtime_driver", 13: "efi_rom", 14: "xbox", 16: "windows_boot",
}


@dataclass
class ExeMetadata:
    path: str
    sha256: str
    arch: str  # "i386" | "x86_64" | "arm64" | "unknown"
    subsystem: str
    imports: list[str]
    directx_version: str | None  # "9" | "10" | "11" | "12" | None
    dotnet: bool
    is_installer: bool

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, ensure_ascii=False)


def analyze(path: Path) -> ExeMetadata:
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()

    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE/MZ binary")

    # PE header offset
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        raise ValueError(f"{path}: missing PE signature")

    # COFF header
    coff = pe_off + 4
    machine = struct.unpack_from("<H", data, coff)[0]
    arch = {
        IMAGE_FILE_MACHINE_I386: "i386",
        IMAGE_FILE_MACHINE_AMD64: "x86_64",
        IMAGE_FILE_MACHINE_ARM64: "arm64",
    }.get(machine, f"unknown(0x{machine:04x})")

    # Optional header magic — определяет PE32 vs PE32+
    opt_off = coff + 20
    magic = struct.unpack_from("<H", data, opt_off)[0]
    is_pe32_plus = (magic == 0x20B)

    # Subsystem at offset 68 in OptionalHeader for both PE32 and PE32+
    # (PE32+ replaces 4-byte BaseOfData+ImageBase with 8-byte ImageBase, net same).
    subsystem_off = opt_off + 68
    subsystem_id = struct.unpack_from("<H", data, subsystem_off)[0]
    subsystem = SUBSYSTEM_NAMES.get(subsystem_id, f"id={subsystem_id}")

    # Import table сидит в директории #1, смещение зависит от PE32/PE32+
    data_dir_off = opt_off + (112 if is_pe32_plus else 96)
    import_rva = struct.unpack_from("<I", data, data_dir_off + 8)[0]
    import_size = struct.unpack_from("<I", data, data_dir_off + 12)[0]

    imports = _read_imports(data, pe_off, import_rva) if import_rva else []
    imports_lower = [i.lower() for i in imports]

    # Эвристики: какой DirectX, есть ли .NET
    dx = None
    if "d3d12.dll" in imports_lower:
        dx = "12"
    elif "d3d11.dll" in imports_lower:
        dx = "11"
    elif "d3d10.dll" in imports_lower or "d3d10_1.dll" in imports_lower:
        dx = "10"
    elif "d3d9.dll" in imports_lower or "d3d9.lib" in imports_lower:
        dx = "9"
    elif "ddraw.dll" in imports_lower or "d3d8.dll" in imports_lower:
        dx = "7-8"

    dotnet = any("mscoree" in i or "clr" in i for i in imports_lower)

    # Простая эвристика для инсталлеров
    name_lower = path.name.lower()
    is_installer = any(
        kw in name_lower for kw in ("setup", "install", "_inst", "_setup")
    )

    return ExeMetadata(
        path=str(path),
        sha256=sha,
        arch=arch,
        subsystem=subsystem,
        imports=sorted(set(imports)),
        directx_version=dx,
        dotnet=dotnet,
        is_installer=is_installer,
    )


def _read_imports(data: bytes, pe_off: int, import_rva: int) -> list[str]:
    """Resolve import table → list of DLL names."""
    # Section table — для перевода RVA → file offset
    coff = pe_off + 4
    n_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    sect_off = coff + 20 + opt_size

    sections = []
    for i in range(n_sections):
        s = sect_off + i * 40
        v_size = struct.unpack_from("<I", data, s + 8)[0]
        v_addr = struct.unpack_from("<I", data, s + 12)[0]
        r_size = struct.unpack_from("<I", data, s + 16)[0]
        r_addr = struct.unpack_from("<I", data, s + 20)[0]
        sections.append((v_addr, v_size, r_addr, r_size))

    def rva_to_off(rva: int) -> int | None:
        for v_addr, v_size, r_addr, _ in sections:
            if v_addr <= rva < v_addr + v_size:
                return r_addr + (rva - v_addr)
        return None

    off = rva_to_off(import_rva)
    if off is None:
        return []

    dlls: list[str] = []
    while True:
        # IMAGE_IMPORT_DESCRIPTOR — 20 bytes
        chunk = data[off:off + 20]
        if len(chunk) < 20 or chunk == b"\x00" * 20:
            break
        name_rva = struct.unpack_from("<I", chunk, 12)[0]
        if name_rva == 0:
            break
        name_off = rva_to_off(name_rva)
        if name_off is None:
            break
        end = data.find(b"\x00", name_off)
        dlls.append(data[name_off:end].decode("ascii", errors="replace"))
        off += 20
    return dlls


# ---------------------------------------------------------------------------
# Unified output (new API)
# ---------------------------------------------------------------------------

@dataclass
class ExeAnalysis:
    """Unified PE analysis: stdlib baseline + string scanning + optional lief."""
    path: str
    sha256: str
    arch: str
    subsystem: str
    imports_static: list[str]           # DLL names from import table
    imports_dynamic: list[str]          # DLL names from LoadLibrary string scan
    directx_version: str | None
    dotnet: bool
    is_installer: bool
    engine: str | None                  # "Unity"|"Unreal"|"Godot"|"GameMaker"|...
    engine_version: str | None
    engine_confidence: float
    anti_cheat: dict[str, Any]          # note-117 kill-filter applied
    profile_match: dict[str, Any] | None
    features: dict[str, float] | None   # 80-dim vector (None if lief unavailable)

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, ensure_ascii=False)

    def to_metadata(self) -> ExeMetadata:
        """Downcast to legacy ExeMetadata."""
        return ExeMetadata(
            path=self.path,
            sha256=self.sha256,
            arch=self.arch,
            subsystem=self.subsystem,
            imports=self.imports_static,
            directx_version=self.directx_version,
            dotnet=self.dotnet,
            is_installer=self.is_installer,
        )


# ---------------------------------------------------------------------------
# Unified deep analysis
# ---------------------------------------------------------------------------

def analyze_full(path: Path) -> ExeAnalysis:
    """Deep PE analysis combining stdlib + string scanning + optional lief + profile lookup."""
    from app.configurator.ai.pe_analyzer.heuristics import (
        extract_strings,
        detect_anticheat_full,
        detect_engine_fingerprint,
        scan_dynamic_imports,
    )
    from app.configurator.profiles import lookup_profile_for_exe

    raw = path.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()

    if raw[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE/MZ binary")

    # stdlib PE parse
    md = analyze(path)
    static_imports = md.imports
    imports_lower = [i.lower() for i in static_imports]

    dx = md.directx_version
    dotnet = md.dotnet
    is_installer = md.is_installer
    arch = md.arch
    subsystem = md.subsystem

    # String scanning (no lief required)
    strings = extract_strings(raw)

    # Anti-cheat (note-117 kill-filter)
    ac = detect_anticheat_full(strings, imports=set(static_imports))

    # Engine fingerprint
    engine_info = detect_engine_fingerprint(imports=static_imports, raw_or_strings=strings)

    # Dynamic import scan (subtract static imports)
    static_lower = {i.lower() for i in static_imports}
    dynamic_imports = [
        d for d in scan_dynamic_imports(strings)
        if d not in static_lower
    ]

    # Enrich dx from dynamic imports if not found statically
    if dx is None:
        dyn_lower = set(dynamic_imports)
        if "d3d12.dll" in dyn_lower:
            dx = "12"
        elif "d3d11.dll" in dyn_lower:
            dx = "11"
        elif any(i in dyn_lower for i in ("d3d10.dll", "d3d10_1.dll")):
            dx = "10"
        elif any(i in dyn_lower for i in ("d3d9.dll",)):
            dx = "9"

    # Rich lief feature vector (optional)
    features: dict[str, float] | None = None
    try:
        from app.configurator.ai.pe_analyzer import analyze as lief_analyze
        from app.configurator.ai.pe_analyzer.features import build_feature_vector
        lief_result = lief_analyze(path)
        features = build_feature_vector(lief_result)
    except Exception:
        pass

    # Profile lookup
    profile_match = lookup_profile_for_exe(path=path, sha256=sha)

    return ExeAnalysis(
        path=str(path),
        sha256=sha,
        arch=arch,
        subsystem=subsystem,
        imports_static=static_imports,
        imports_dynamic=dynamic_imports,
        directx_version=dx,
        dotnet=dotnet,
        is_installer=is_installer,
        engine=engine_info["engine"],
        engine_version=engine_info.get("version"),
        engine_confidence=engine_info["confidence"],
        anti_cheat=ac,
        profile_match=profile_match,
        features=features,
    )


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("Usage: exe_analyzer.py <path-to.exe> [--full]", file=sys.stderr)
        return 2
    path = Path(argv[1])
    if "--full" in argv:
        result = analyze_full(path)
        print(result.to_json())
    else:
        md = analyze(path)
        print(md.to_json())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
