"""MacRunner PE Analyzer — Feature Vector Builder.

Transforms PEAnalysisResult into a structured 80-dimension feature dict
ready for ML model input (scikit-learn / XGBoost).
"""

from __future__ import annotations

import math
from typing import Any

from app.configurator.ai.pe_analyzer import (
    PEAnalysisResult,
    SectionFeatures,
)


def _encode_arch(arch: str) -> tuple[float, float, float, float, float]:
    """One-hot encode architecture: [i386, x86_64, arm64, arm64ec, arm64x]."""
    mapping = {
        "i386": (1.0, 0.0, 0.0, 0.0, 0.0),
        "x86_64": (0.0, 1.0, 0.0, 0.0, 0.0),
        "arm64": (0.0, 0.0, 1.0, 0.0, 0.0),
        "arm64ec": (0.0, 0.0, 0.0, 1.0, 0.0),
        "arm64x": (0.0, 0.0, 0.0, 0.0, 1.0),
    }
    return mapping.get(arch, (0.0, 0.0, 0.0, 0.0, 0.0))


def _encode_subsystem(subsystem: str) -> tuple[float, float, float, float, float, float, float, float]:
    """One-hot encode subsystem: [native, gui, console, wince_gui, efi_app, efi_boot, efi_runtime, efi_rom, xbox, windows_boot]."""
    # Simplified to most common: native, gui, console, other
    if subsystem == "native":
        return (1.0, 0.0, 0.0, 0.0)
    if subsystem == "gui":
        return (0.0, 1.0, 0.0, 0.0)
    if subsystem == "console":
        return (0.0, 0.0, 1.0, 0.0)
    return (0.0, 0.0, 0.0, 1.0)


def _encode_framework(guess: str) -> tuple[float, float, float, float, float]:
    """One-hot encode framework: [qt, wpf, electron, gtk, win32]."""
    mapping = {
        "qt": (1.0, 0.0, 0.0, 0.0, 0.0),
        "wpf": (0.0, 1.0, 0.0, 0.0, 0.0),
        "electron": (0.0, 0.0, 1.0, 0.0, 0.0),
        "gtk": (0.0, 0.0, 0.0, 1.0, 0.0),
        "win32": (0.0, 0.0, 0.0, 0.0, 1.0),
    }
    return mapping.get(guess, (0.0, 0.0, 0.0, 0.0, 1.0))


def _log2_plus1(val: int) -> float:
    return math.log2(val + 1) if val > 0 else 0.0


def _boolf(val: bool) -> float:
    return 1.0 if val else 0.0


def _safe_div(a: int | float, b: int | float) -> float:
    return a / b if b != 0 else 0.0


def build_feature_vector(result: PEAnalysisResult) -> dict[str, Any]:
    """Build an 80-dimension feature dict from PEAnalysisResult."""
    secs = result.sections
    imps = result.imports
    rsrc = result.resources
    sigs = result.signatures

    # ---- Section aggregates ----
    text_sec = next((s for s in secs if s.name.lower() == ".text"), None)
    rdata_sec = next((s for s in secs if s.name.lower() == ".rdata"), None)
    data_sec = next((s for s in secs if s.name.lower() == ".data"), None)
    rsrc_sec = next((s for s in secs if s.name.lower() == ".rsrc"), None)
    reloc_sec = next((s for s in secs if s.name.lower() == ".reloc"), None)

    total_raw_size = sum(s.raw_size for s in secs) or 1
    exec_secs = sum(1 for s in secs if s.is_executable)
    write_secs = sum(1 for s in secs if s.is_writable)
    entropies = [s.entropy for s in secs if s.raw_size > 0]
    max_ent = max(entropies) if entropies else 0.0
    min_ent = min(entropies) if entropies else 0.0

    text_size = text_sec.raw_size if text_sec else 0
    text_entropy = text_sec.entropy if text_sec else 0.0
    rdata_size = rdata_sec.raw_size if rdata_sec else 0
    data_size = data_sec.raw_size if data_sec else 0

    # ---- Import aggregates ----
    dll_names_lower = {imp.dll_name.lower() for imp in imps}
    total_imports = sum(imp.entries_count for imp in imps)

    def has_dll(name: str) -> bool:
        return name.lower() in dll_names_lower

    # ---- Build features ----
    arch_enc = _encode_arch(result.arch)
    subsys_enc = _encode_subsystem(result.subsystem)

    features: dict[str, Any] = {
        # PE header (8 + expanded to match 80 total)
        "pe_header.arch_i386": arch_enc[0],
        "pe_header.arch_x86_64": arch_enc[1],
        "pe_header.arch_arm64": arch_enc[2],
        "pe_header.arch_arm64ec": arch_enc[3],
        "pe_header.arch_arm64x": arch_enc[4],
        "pe_header.subsystem_native": subsys_enc[0],
        "pe_header.subsystem_gui": subsys_enc[1],
        "pe_header.subsystem_console": subsys_enc[2],
        "pe_header.subsystem_other": subsys_enc[3],
        "pe_header.image_base_log2": _log2_plus1(result.image_base),
        "pe_header.entry_point_norm": _safe_div(result.entry_point, result.image_base or 1),
        "pe_header.timestamp_age_days": result.timestamp / 86400.0 if result.timestamp else 0.0,
        "pe_header.has_checksum": _boolf(result.checksum != 0),
        "pe_header.number_of_sections": float(result.number_of_sections),
        "pe_header.dll_characteristics": float(result.dll_characteristics),

        # Sections (12)
        "sections.text_size_log": _log2_plus1(text_size),
        "sections.text_entropy": text_entropy,
        "sections.rdata_size_log": _log2_plus1(rdata_size),
        "sections.data_size_log": _log2_plus1(data_size),
        "sections.rsrc_present": _boolf(rsrc_sec is not None),
        "sections.reloc_present": _boolf(reloc_sec is not None),
        "sections.code_ratio": _safe_div(text_size, total_raw_size),
        "sections.data_ratio": _safe_div(data_size, total_raw_size),
        "sections.max_entropy": max_ent,
        "sections.min_entropy": min_ent,
        "sections.executable_sections": float(exec_secs),
        "sections.writable_sections": float(write_secs),

        # Imports (20)
        "imports.dll_count": float(len(imps)),
        "imports.total_imports": float(total_imports),
        "imports.has_d3d9": _boolf(has_dll("d3d9.dll")),
        "imports.has_d3d10": _boolf(has_dll("d3d10.dll") or has_dll("d3d10_1.dll")),
        "imports.has_d3d11": _boolf(has_dll("d3d11.dll")),
        "imports.has_d3d12": _boolf(has_dll("d3d12.dll")),
        "imports.has_dxgi": _boolf(has_dll("dxgi.dll")),
        "imports.has_opengl32": _boolf(has_dll("opengl32.dll")),
        "imports.has_vulkan": _boolf(has_dll("vulkan-1.dll")),
        "imports.has_mscoree": _boolf(has_dll("mscoree.dll")),
        "imports.has_kernel32": _boolf(has_dll("kernel32.dll")),
        "imports.has_user32": _boolf(has_dll("user32.dll")),
        "imports.has_gdi32": _boolf(has_dll("gdi32.dll")),
        "imports.has_shell32": _boolf(has_dll("shell32.dll")),
        "imports.has_ole32": _boolf(has_dll("ole32.dll")),
        "imports.has_ws2_32": _boolf(has_dll("ws2_32.dll")),
        "imports.has_winmm": _boolf(has_dll("winmm.dll")),
        "imports.has_advapi32": _boolf(has_dll("advapi32.dll")),
        "imports.has_crypt32": _boolf(has_dll("crypt32.dll")),
        "imports.has_ntdll": _boolf(has_dll("ntdll.dll")),

        # Exports (4)
        "exports.count": float(result.exports_count),
        "exports.has_forwarded": _boolf(result.has_forwarded_exports),
        "exports.has_reexports": _boolf(result.has_reexports),
        "exports.name_length_avg": result.avg_export_name_length,

        # Resources (8)
        "resources.has_manifest": _boolf(rsrc.has_manifest),
        "resources.has_version_info": _boolf(rsrc.has_version_info),
        "resources.icon_count": float(rsrc.icon_count),
        "resources.string_table_count": float(rsrc.string_table_count),
        "resources.language_id": float(rsrc.language_id or -1),
        "resources.company_name_length": float(len(rsrc.company_name or "")),
        "resources.product_name_length": float(len(rsrc.product_name or "")),
        "resources.has_file_version": _boolf(rsrc.file_version is not None),

        # Signatures (4)
        "signatures.has_signature": _boolf(sigs.has_signature),
        "signatures.signature_valid": _boolf(sigs.signature_valid),
        "signatures.certificate_count": float(sigs.certificate_count),
        "signatures.has_countersignature": _boolf(sigs.has_countersignature),

        # Heuristics placeholders — populated by caller from heuristics module
        "heuristics.is_installer": 0.0,
        "heuristics.is_packed": 0.0,
        "heuristics.is_dotnet": 0.0,
        "heuristics.framework_qt": 0.0,
        "heuristics.framework_wpf": 0.0,
        "heuristics.framework_electron": 0.0,
        "heuristics.framework_gtk": 0.0,
        "heuristics.framework_unity": 0.0,
        "heuristics.framework_unreal": 0.0,
        "heuristics.framework_win32": 1.0,
        "heuristics.is_text_editor": 0.0,
        "heuristics.is_console": _boolf(result.subsystem == "console"),
        "heuristics.is_gui": _boolf(result.subsystem == "gui"),
        "heuristics.has_tls": _boolf(result.has_tls),
        "heuristics.has_exception_dir": _boolf(result.has_exception_dir),
        "heuristics.has_debug_dir": _boolf(result.has_debug_dir),
        "heuristics.has_load_config": _boolf(result.has_load_config),
        "heuristics.is_64bit": _boolf(result.is_pe32_plus),
        "heuristics.is_arm64": _boolf(result.arch in ("arm64", "arm64ec", "arm64x")),

        # Anti-cheat / DRM placeholders (8)
        "anticheat.battleye": 0.0,
        "anticheat.eac": 0.0,
        "anticheat.vanguard": 0.0,
        "anticheat.punkbuster": 0.0,
        "anticheat.denuvo": 0.0,
        "anticheat.vmprotect": 0.0,
        "anticheat.themida": 0.0,
        "anticheat.widevine": 0.0,

        # User context placeholders (4) — populated by caller
        "user.macos_version_major": -1.0,
        "user.ram_gb": -1.0,
        "user.cpu_core_count": -1.0,
        "user.has_discrete_gpu": -1.0,
    }

    # Update installer/packed/dotnet/framework from heuristics
    from app.configurator.ai.pe_analyzer import heuristics as heur
    heur.update_features(result, features)

    return features


def build_dense_vector(features: dict[str, Any]) -> list[float]:
    """Flatten feature dict to ordered list for ML input."""
    order = [
        # PE header (16 expanded)
        "pe_header.arch_i386", "pe_header.arch_x86_64", "pe_header.arch_arm64",
        "pe_header.arch_arm64ec", "pe_header.arch_arm64x",
        "pe_header.subsystem_native", "pe_header.subsystem_gui",
        "pe_header.subsystem_console", "pe_header.subsystem_other",
        "pe_header.image_base_log2", "pe_header.entry_point_norm",
        "pe_header.timestamp_age_days", "pe_header.has_checksum",
        "pe_header.number_of_sections", "pe_header.dll_characteristics",
        # Sections (12)
        "sections.text_size_log", "sections.text_entropy",
        "sections.rdata_size_log", "sections.data_size_log",
        "sections.rsrc_present", "sections.reloc_present",
        "sections.code_ratio", "sections.data_ratio",
        "sections.max_entropy", "sections.min_entropy",
        "sections.executable_sections", "sections.writable_sections",
        # Imports (20)
        "imports.dll_count", "imports.total_imports",
        "imports.has_d3d9", "imports.has_d3d10", "imports.has_d3d11",
        "imports.has_d3d12", "imports.has_dxgi", "imports.has_opengl32",
        "imports.has_vulkan", "imports.has_mscoree", "imports.has_kernel32",
        "imports.has_user32", "imports.has_gdi32", "imports.has_shell32",
        "imports.has_ole32", "imports.has_ws2_32", "imports.has_winmm",
        "imports.has_advapi32", "imports.has_crypt32", "imports.has_ntdll",
        # Exports (4)
        "exports.count", "exports.has_forwarded", "exports.has_reexports",
        "exports.name_length_avg",
        # Resources (8)
        "resources.has_manifest", "resources.has_version_info",
        "resources.icon_count", "resources.string_table_count",
        "resources.language_id", "resources.company_name_length",
        "resources.product_name_length", "resources.has_file_version",
        # Signatures (4)
        "signatures.has_signature", "signatures.signature_valid",
        "signatures.certificate_count", "signatures.has_countersignature",
        # Heuristics (13)
        "heuristics.is_installer", "heuristics.is_packed", "heuristics.is_dotnet",
        "heuristics.framework_qt", "heuristics.framework_wpf",
        "heuristics.framework_electron", "heuristics.framework_gtk",
        "heuristics.framework_win32", "heuristics.is_text_editor",
        "heuristics.is_console", "heuristics.is_gui", "heuristics.has_tls",
        "heuristics.has_exception_dir", "heuristics.has_debug_dir",
        "heuristics.has_load_config", "heuristics.is_64bit",
        "heuristics.is_arm64",
        # Anti-cheat (8)
        "anticheat.battleye", "anticheat.eac", "anticheat.vanguard",
        "anticheat.punkbuster", "anticheat.denuvo", "anticheat.vmprotect",
        "anticheat.themida", "anticheat.widevine",
        # User context (4)
        "user.macos_version_major", "user.ram_gb", "user.cpu_core_count",
        "user.has_discrete_gpu",
    ]
    return [float(features.get(k, -1.0)) for k in order]
