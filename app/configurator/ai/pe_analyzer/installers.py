"""MacRunner PE Analyzer — Installer type detection.

Identifies NSIS, MSI, and InnoSetup installers from PE structure.
"""

from __future__ import annotations

from app.configurator.ai.pe_analyzer import PEAnalysisResult


def detect_installer(result: PEAnalysisResult) -> str:
    """Detect installer type. Returns one of: nsis, msi, innosetup, none."""
    imps = {imp.dll_name.lower() for imp in result.imports}
    secs = {s.name.lower() for s in result.sections}
    path_lower = result.path.lower()

    # MSI — Windows Installer database
    if "msi.dll" in imps or path_lower.endswith(".msi"):
        return "msi"

    # NSIS — Nullsoft Scriptable Install System
    # Typical sections: .ndata, .text, .rdata
    # Typical imports: zlib, kernel32, user32
    if ".ndata" in secs:
        return "nsis"
    # NSIS compiled installers often have specific string patterns
    # (heuristic: high .text entropy + specific import mix)
    if "zlib.dll" in imps and result.number_of_sections <= 5:
        return "nsis"

    # Inno Setup — usually has specific resource patterns
    # Inno Setup installers typically have rich resources and specific section names
    if any(s.startswith("inno") for s in secs):
        return "innosetup"
    # Filename heuristic as fallback
    if any(kw in path_lower for kw in ("setup", "install", "_inst", "_setup")):
        # Not a strong signal alone; check for resource richness
        if result.resources.icon_count > 0 and result.number_of_sections <= 6:
            return "innosetup"

    return "none"


def is_installer(result: PEAnalysisResult) -> bool:
    """Quick check if binary is likely an installer."""
    return detect_installer(result) != "none"
