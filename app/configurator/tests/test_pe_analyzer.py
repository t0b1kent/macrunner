"""Tests for the unified PE analyzer, anti-cheat detection, engine fingerprint,
dynamic import scan, and profile lookup.

All tests are self-contained: they use synthetic bytes/strings, no real PE file needed.
Run with: python3 -m pytest app/configurator/tests/test_pe_analyzer.py -v
Or:        python3 app/configurator/tests/test_pe_analyzer.py
"""
from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path
from typing import Any


# ---------------------------------------------------------------------------
# Helpers — build a minimal valid PE binary in memory
# ---------------------------------------------------------------------------

def _build_minimal_pe(
    machine: int = 0x8664,      # AMD64
    subsystem: int = 2,          # GUI
    dll_names: list[str] | None = None,
    extra_strings: bytes = b"",
) -> bytes:
    """Build a minimal but structurally valid PE/COFF binary."""
    dll_names = dll_names or []

    # We'll lay out: DOS stub → PE header → Optional header → Section table → section data
    dos_stub = b"MZ" + b"\x00" * 58 + struct.pack("<I", 0x40)  # e_lfanew = 0x40

    import_section_data = b""
    import_rva = 0
    import_size = 0
    sections_raw = b""

    # Build import descriptors if needed
    if dll_names:
        # Layout: descriptors | name strings (all relative to section start)
        # Section will be placed at RVA 0x1000
        SEC_RVA = 0x1000

        descs = b""
        name_blob = b""
        name_offsets: list[int] = []

        base_name_off = (len(dll_names) + 1) * 20  # after descriptors + terminator
        cur = base_name_off
        for name in dll_names:
            encoded = name.encode("ascii") + b"\x00"
            name_offsets.append(cur)
            name_blob += encoded
            cur += len(encoded)

        for i, (name, off) in enumerate(zip(dll_names, name_offsets)):
            name_rva = SEC_RVA + off
            desc = struct.pack("<IIIII", 0, 0, 0, name_rva, 0)
            descs += desc
        descs += b"\x00" * 20  # terminator

        import_section_data = descs + name_blob + extra_strings + b"\x00" * 4
        import_rva = SEC_RVA
        import_size = len(import_section_data)
    else:
        import_section_data = extra_strings + b"\x00" * 4

    # Align section data to 512 bytes
    raw_section_data = import_section_data
    if len(raw_section_data) % 512 != 0:
        raw_section_data += b"\x00" * (512 - len(raw_section_data) % 512)

    pe_sig = b"PE\x00\x00"
    # COFF header (20 bytes)
    n_sections = 1
    coff = struct.pack(
        "<HHIIIHH",
        machine,       # Machine
        n_sections,    # NumberOfSections
        0,             # TimeDateStamp
        0,             # PointerToSymbolTable
        0,             # NumberOfSymbols
        240,           # SizeOfOptionalHeader (PE32+)
        0x0022,        # Characteristics
    )

    # Optional header PE32+ (240 bytes)
    # Data directories: 16 entries × 8 bytes = 128 bytes
    # We fill them with zeros except entry 1 (import)
    data_dirs = [0, 0] * 16  # flat list of (rva, size) pairs
    if import_rva:
        # Entry 1 = imports
        data_dirs[2] = import_rva    # rva
        data_dirs[3] = import_size   # size
    data_dir_bytes = struct.pack("<" + "I" * 32, *data_dirs)

    # PE32+ optional header: 112 bytes before data directories
    # Format: H BB I I I I I Q I I HH HH HH I I I I HH Q Q Q Q
    opt = struct.pack("<H", 0x20B)                        # Magic
    opt += struct.pack("<BB", 14, 0)                      # Linker version
    opt += struct.pack("<III", 0x1000, 0x1000, 0)         # SizeOfCode/InitData/UninitData
    opt += struct.pack("<II", 0x1000, 0x1000)             # AddressOfEntryPoint, BaseOfCode
    opt += struct.pack("<Q", 0x140000000)                 # ImageBase (64-bit)
    opt += struct.pack("<II", 0x1000, 0x200)              # SectionAlignment, FileAlignment
    opt += struct.pack("<HHHH", 6, 0, 0, 0)              # OS/Image version
    opt += struct.pack("<HHI", 6, 0, 0)                  # Subsystem version + Win32VersionValue
    opt += struct.pack("<III", 0x3000, 0x400, 0)          # SizeOfImage, SizeOfHeaders, CheckSum
    opt += struct.pack("<HH", subsystem, 0x8160)          # Subsystem, DllCharacteristics
    opt += struct.pack("<QQQQ", 0x100000, 0x1000, 0x100000, 0x1000)  # Stack/Heap reserve/commit
    opt += struct.pack("<II", 0, 16)                      # LoaderFlags, NumberOfRvaAndSizes
    opt += data_dir_bytes

    # Section table entry (40 bytes)
    section_rva = 0x1000
    raw_ptr = 0x400  # right after headers
    sec_entry = (
        b".idata\x00\x00"      # Name (8 bytes, null-padded)
        + struct.pack("<IIIIII",
            len(import_section_data),  # VirtualSize
            section_rva,               # VirtualAddress
            len(raw_section_data),     # SizeOfRawData
            raw_ptr,                   # PointerToRawData
            0, 0,                      # Relocs, LineNums
        )
        + struct.pack("<HHI", 0, 0, 0x40000040)  # NChars, NLines, Characteristics
    )

    # Stitch everything together
    headers = dos_stub + pe_sig + coff + opt + sec_entry
    # Pad headers to raw_ptr (0x400)
    if len(headers) < raw_ptr:
        headers += b"\x00" * (raw_ptr - len(headers))

    return headers + raw_section_data


# ---------------------------------------------------------------------------
# Tests: string extraction
# ---------------------------------------------------------------------------

def test_extract_strings_basic():
    from app.configurator.ai.pe_analyzer.heuristics import extract_strings
    raw = b"\x00\x01Hello, World!\x00\x02\x03Short\xff"
    result = extract_strings(raw, min_len=5)
    assert "Hello, World!" in result
    assert all(len(s) >= 5 for s in result)
    print("PASS test_extract_strings_basic")


def test_extract_strings_empty():
    from app.configurator.ai.pe_analyzer.heuristics import extract_strings
    assert extract_strings(b"\x00\x01\x02", min_len=5) == []
    print("PASS test_extract_strings_empty")


# ---------------------------------------------------------------------------
# Tests: anti-cheat detection
# ---------------------------------------------------------------------------

def test_anticheat_none():
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    result = detect_anticheat_full(["kernel32.dll", "user32.dll"])
    assert result["detected"] is False
    assert result["kernel_driver"] is False
    assert result["verdict"] == "none"
    print("PASS test_anticheat_none")


def test_anticheat_eac_kernel():
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    strings = ["EasyAntiCheat_x64.sys", "kernel32.dll", "EasyantiCheat"]
    result = detect_anticheat_full(strings)
    assert result["detected"] is True
    assert result["kernel_driver"] is True
    assert result["verdict"] == "unsupported: kernel driver"
    assert "EasyAntiCheat" in result["names"]
    print("PASS test_anticheat_eac_kernel")


def test_anticheat_battleye_kernel():
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    strings = ["BEClient.dll", "battleye service"]
    result = detect_anticheat_full(strings)
    assert result["kernel_driver"] is True
    assert result["verdict"] == "unsupported: kernel driver"
    assert "BattlEye" in result["names"]
    print("PASS test_anticheat_battleye_kernel")


def test_anticheat_vanguard_kernel():
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    strings = ["vgk.sys", "riot vanguard"]
    result = detect_anticheat_full(strings)
    assert result["kernel_driver"] is True
    assert "Vanguard" in result["names"]
    print("PASS test_anticheat_vanguard_kernel")


def test_anticheat_via_imports():
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    # "easyanticheat" substring matches the import name lowercased
    result = detect_anticheat_full(
        ["easyanticheat.dll", "kernel32.dll"],
        imports={"EasyAntiCheat.dll", "kernel32.dll"},
    )
    assert result["detected"] is True, f"Expected detected=True, got {result}"
    assert result["kernel_driver"] is True
    print("PASS test_anticheat_via_imports")


def test_anticheat_note117_kill_filter():
    """note-117: kernel AC → verdict must be 'unsupported: kernel driver'."""
    from app.configurator.ai.pe_analyzer.heuristics import detect_anticheat_full
    result = detect_anticheat_full(["beclient"], imports=set())
    assert result["verdict"] == "unsupported: kernel driver", \
        f"note-117 kill-filter failed: got {result['verdict']!r}"
    print("PASS test_anticheat_note117_kill_filter")


# ---------------------------------------------------------------------------
# Tests: engine fingerprint
# ---------------------------------------------------------------------------

def test_engine_unity_by_import():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(
        imports=["UnityPlayer.dll", "kernel32.dll"],
        raw_or_strings=[],
    )
    assert result["engine"] == "Unity"
    assert result["confidence"] >= 0.55
    print("PASS test_engine_unity_by_import")


def test_engine_unity_with_version():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(
        imports=["UnityPlayer.dll"],
        raw_or_strings=["Unity 2020.3.14f1", "some other string"],
    )
    assert result["engine"] == "Unity"
    assert result["version"] == "2020.3.14f1"
    print("PASS test_engine_unity_with_version")


def test_engine_unreal_by_string():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(
        imports=["kernel32.dll"],
        raw_or_strings=["Unreal Engine 5.1", "UObject", "GEngine"],
    )
    assert result["engine"] == "Unreal"
    print("PASS test_engine_unreal_by_string")


def test_engine_godot_by_string():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(
        imports=[],
        raw_or_strings=["GDScript bytecode", "GodotEngine v4.1"],
    )
    assert result["engine"] == "Godot"
    print("PASS test_engine_godot_by_string")


def test_engine_gamemaker():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(
        imports=[],
        raw_or_strings=["YoYo Games Ltd", "GameMaker Studio 2"],
    )
    assert result["engine"] == "GameMaker"
    print("PASS test_engine_gamemaker")


def test_engine_none():
    from app.configurator.ai.pe_analyzer.heuristics import detect_engine_fingerprint
    result = detect_engine_fingerprint(imports=["kernel32.dll"], raw_or_strings=["hello"])
    assert result["engine"] is None
    assert result["confidence"] == 0.0
    print("PASS test_engine_none")


# ---------------------------------------------------------------------------
# Tests: dynamic import scan
# ---------------------------------------------------------------------------

def test_dynamic_imports_finds_dlls():
    from app.configurator.ai.pe_analyzer.heuristics import scan_dynamic_imports
    strings = [
        "Loading d3d11.dll at runtime",
        "LoadLibrary(dinput8.dll)",
        "short",
        "openal32.dll",
        "C:\\Windows\\System32\\user32.dll",  # has path separator — excluded
    ]
    result = scan_dynamic_imports(strings)
    assert "d3d11.dll" in result
    assert "dinput8.dll" in result
    assert "openal32.dll" in result
    # Entries with paths should still extract the dll name via regex
    print("PASS test_dynamic_imports_finds_dlls")


def test_dynamic_imports_no_duplicates():
    from app.configurator.ai.pe_analyzer.heuristics import scan_dynamic_imports
    strings = ["d3d11.dll", "D3D11.dll", "d3d11.dll"]
    result = scan_dynamic_imports(strings)
    assert result.count("d3d11.dll") == 1
    print("PASS test_dynamic_imports_no_duplicates")


# ---------------------------------------------------------------------------
# Tests: stdlib PE analyzer (ExeMetadata)
# ---------------------------------------------------------------------------

def test_analyze_minimal_pe():
    from app.configurator.exe_analyzer import analyze
    raw = _build_minimal_pe(machine=0x8664, dll_names=["d3d11.dll", "kernel32.dll"])
    with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as f:
        f.write(raw)
        tmp = Path(f.name)
    try:
        md = analyze(tmp)
        assert md.arch == "x86_64"
        assert md.subsystem == "gui"
        assert "d3d11.dll" in [i.lower() for i in md.imports]
        assert md.directx_version == "11"
        assert md.sha256 == hashlib.sha256(raw).hexdigest()
    finally:
        tmp.unlink(missing_ok=True)
    print("PASS test_analyze_minimal_pe")


def test_analyze_i386_pe():
    from app.configurator.exe_analyzer import analyze
    raw = _build_minimal_pe(machine=0x014C, dll_names=["kernel32.dll"])
    with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as f:
        f.write(raw)
        tmp = Path(f.name)
    try:
        md = analyze(tmp)
        assert md.arch == "i386"
    finally:
        tmp.unlink(missing_ok=True)
    print("PASS test_analyze_i386_pe")


# ---------------------------------------------------------------------------
# Tests: ExeAnalysis / analyze_full (string-scan parts, no lief needed)
# ---------------------------------------------------------------------------

def test_analyze_full_unity_game():
    from app.configurator.exe_analyzer import analyze_full
    extra = b"UnityPlayer.dll\x00Unity 2020.3.14f1\x00EasyAntiCheat_x64.sys\x00"
    raw = _build_minimal_pe(
        machine=0x8664,
        dll_names=["UnityPlayer.dll", "kernel32.dll", "d3d11.dll"],
        extra_strings=extra,
    )
    with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as f:
        f.write(raw)
        tmp = Path(f.name)
    try:
        result = analyze_full(tmp)
        assert result.engine == "Unity"
        assert result.directx_version == "11"
        assert result.anti_cheat["kernel_driver"] is True
        assert result.anti_cheat["verdict"] == "unsupported: kernel driver"
    finally:
        tmp.unlink(missing_ok=True)
    print("PASS test_analyze_full_unity_game")


def test_analyze_full_to_metadata_compat():
    from app.configurator.exe_analyzer import analyze_full, ExeMetadata
    raw = _build_minimal_pe(dll_names=["kernel32.dll"])
    with tempfile.NamedTemporaryFile(suffix=".exe", delete=False) as f:
        f.write(raw)
        tmp = Path(f.name)
    try:
        full = analyze_full(tmp)
        md = full.to_metadata()
        assert isinstance(md, ExeMetadata)
        assert md.sha256 == full.sha256
        assert md.arch == full.arch
    finally:
        tmp.unlink(missing_ok=True)
    print("PASS test_analyze_full_to_metadata_compat")


# ---------------------------------------------------------------------------
# Tests: profile lookup
# ---------------------------------------------------------------------------

def test_profile_lookup_by_name(tmp_path):
    from app.configurator.profiles import find_profile_by_name
    profile = {
        "schema_version": 3,
        "id": "game-hollow-knight",
        "kind": "profile",
        "name": "Hollow Knight",
        "overrides": {
            "category": "game",
            "recommended_lane": "arm64ec-x64-bridge",
            "env": {"WINEDEBUG": "-all"},
            "engine": {"name": "Unity", "version": "2020.3"},
            "arch": ["x86_64"],
            "graphics": {"api": "d3d11"},
            "dlls": {"static_imports": [], "dynamic_deps": []},
            "redistributables": [],
            "anti_cheat": {"mode": "none"},
        },
    }
    pf = tmp_path / "game-hollow-knight.json"
    pf.write_text(json.dumps(profile), encoding="utf-8")

    match = find_profile_by_name("HollowKnight", base=tmp_path)
    assert match is not None
    assert match["recommended_lane"] == "arm64ec-x64-bridge"
    assert match["env"].get("WINEDEBUG") == "-all"
    print("PASS test_profile_lookup_by_name")


def test_profile_lookup_by_sha(tmp_path):
    from app.configurator.profiles import find_profile_by_sha
    sha = "abc123def456" * 4  # fake sha256
    profile = {
        "schema_version": 3,
        "id": "game-test",
        "kind": "profile",
        "name": "Test Game",
        "sha256": sha,
        "overrides": {
            "category": "game",
            "recommended_lane": "x86-rosetta-wow64",
            "env": {},
            "engine": {"name": "Custom", "version": ""},
            "arch": ["x86"],
            "graphics": {"api": "d3d9"},
            "dlls": {"static_imports": [], "dynamic_deps": []},
            "redistributables": [],
            "anti_cheat": {"mode": "none"},
        },
    }
    pf = tmp_path / "game-test.json"
    pf.write_text(json.dumps(profile), encoding="utf-8")

    match = find_profile_by_sha(sha, base=tmp_path)
    assert match is not None
    assert match["recommended_lane"] == "x86-rosetta-wow64"
    print("PASS test_profile_lookup_by_sha")


def test_profile_lookup_no_match(tmp_path):
    from app.configurator.profiles import lookup_profile_for_exe
    result = lookup_profile_for_exe(
        path=Path("nonexistent_game.exe"),
        sha256="0" * 64,
        base=tmp_path,
    )
    assert result is None
    print("PASS test_profile_lookup_no_match")


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import tempfile
    tmp = Path(tempfile.mkdtemp())

    failures: list[str] = []

    tests = [
        test_extract_strings_basic,
        test_extract_strings_empty,
        test_anticheat_none,
        test_anticheat_eac_kernel,
        test_anticheat_battleye_kernel,
        test_anticheat_vanguard_kernel,
        test_anticheat_via_imports,
        test_anticheat_note117_kill_filter,
        test_engine_unity_by_import,
        test_engine_unity_with_version,
        test_engine_unreal_by_string,
        test_engine_godot_by_string,
        test_engine_gamemaker,
        test_engine_none,
        test_dynamic_imports_finds_dlls,
        test_dynamic_imports_no_duplicates,
        test_analyze_minimal_pe,
        test_analyze_i386_pe,
        test_analyze_full_unity_game,
        test_analyze_full_to_metadata_compat,
    ]
    # Tests that need tmp_path
    tests_with_tmp = [
        test_profile_lookup_by_name,
        test_profile_lookup_by_sha,
        test_profile_lookup_no_match,
    ]

    for t in tests:
        try:
            t()
        except Exception as e:
            failures.append(f"FAIL {t.__name__}: {e}")
            print(f"FAIL {t.__name__}: {e}")

    for t in tests_with_tmp:
        td = Path(tempfile.mkdtemp())
        try:
            t(td)
        except Exception as e:
            failures.append(f"FAIL {t.__name__}: {e}")
            print(f"FAIL {t.__name__}: {e}")

    print(f"\n{'='*40}")
    if failures:
        print(f"FAILED: {len(failures)} test(s)")
        for f in failures:
            print(f"  {f}")
        sys.exit(1)
    else:
        print(f"ALL {len(tests) + len(tests_with_tmp)} TESTS PASSED")
        sys.exit(0)
