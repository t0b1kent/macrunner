"""MacRunner PE Analyzer — Heuristic detection engine.

Detects frameworks, anti-cheat/DRM signatures, packers, .NET apps,
and other non-trivial PE characteristics that cannot be read directly
from a single PE field.

All detections are HEURISTIC — they identify, they do NOT bypass.
Legal constraint: never suggest circumvention.
"""

from __future__ import annotations

from typing import Any

from app.configurator.ai.pe_analyzer import PEAnalysisResult


def _has_import(imports: Any, names: tuple[str, ...]) -> bool:
    """Check if any of the named DLLs appear in imports."""
    dlls = {imp.dll_name.lower() for imp in imports}
    return any(name.lower() in dlls for name in names)


def _has_section(sections: Any, names: tuple[str, ...]) -> bool:
    sec_names = {s.name.lower() for s in sections}
    return any(name.lower() in sec_names for name in names)


def _text_entropy(result: PEAnalysisResult) -> float:
    for sec in result.sections:
        if sec.name.lower() == ".text":
            return sec.entropy
    return 0.0


# ---------------------------------------------------------------------------
# Framework detection
# ---------------------------------------------------------------------------

def detect_framework(result: PEAnalysisResult) -> str:
    """Guess UI framework from imports + resources."""
    imports = result.imports
    imps = {imp.dll_name.lower() for imp in imports}

    # Qt — multiple Qt5/Qt6 DLLs
    qt_dlls = {"qt5core.dll", "qt6core.dll", "qt5gui.dll", "qt6gui.dll",
                 "qt5widgets.dll", "qt6widgets.dll", "qwindows.dll"}
    if any(d in imps for d in qt_dlls):
        return "qt"

    # WPF / .NET Framework
    if any(d in imps for d in {"presentationcore.dll", "presentationframework.dll",
                                "windowsbase.dll", "wpfgfx_cor3.dll"}):
        return "wpf"

    # Electron — looks for chromium signatures in imports or resources
    if any(d in imps for d in {"chrome_elf.dll", "libcef.dll", "electron.dll"}):
        return "electron"

    # Unity — UnityPlayer.dll or Mono/IL2CPP runtime imports
    unity_dlls = {"unityplayer.dll", "mono.dll", "il2cpp.dll", "baselib.dll"}
    if any(d in imps for d in unity_dlls):
        return "unity"

    # Unreal Engine — core UE modules
    ue_dlls = {
        "ue4game.dll", "ue4editor.dll", "ue5game.dll", "ue5editor.dll",
        "core.dll", "coreuobject.dll", "engine.dll", "rendercore.dll",
        "d3d11rhi.dll", "d3d12rhi.dll", "vulkanrhi.dll",
    }
    if any(d in imps for d in ue_dlls):
        return "unreal"

    # GTK (rare on Windows but exists)
    if any(d in imps for d in {"gtk-3.dll", "gtk-2.dll", "gdk-3.dll"}):
        return "gtk"

    # Default: plain Win32
    return "win32"


def detect_text_editor(result: PEAnalysisResult) -> bool:
    """Heuristic for text editors (Notepad++, VS Code, Sublime, etc.).

    Detects by looking for Scintilla/SciLexer imports (Notepad++) or
    code editor signatures (VS Code/Sublime use Electron or custom).
    """
    imps = {imp.dll_name.lower() for imp in result.imports}

    # Notepad++ / Scintilla-based editors
    scintilla_dlls = {
        "scilexer.dll", "scintilla.dll",
        "qtscintilla2.dll", "qscintilla2.dll",
    }
    if any(d in imps for d in scintilla_dlls):
        return True

    # Resource hints: look for common text editor resource strings
    rsrc = result.resources
    if rsrc.product_name:
        prod = rsrc.product_name.lower()
        if any(name in prod for name in (
            "notepad", "scintilla", "text editor", "code editor",
            "sublime", "vscode", "visual studio code",
        )):
            return True

    # Company name hints
    if rsrc.company_name:
        company = rsrc.company_name.lower()
        if any(name in company for name in (
            "notepad++", "don ho", "scintilla", "sublime",
        )):
            return True

    return False


# ---------------------------------------------------------------------------
# Anti-cheat / DRM detection (heuristic only)
# ---------------------------------------------------------------------------

def detect_anticheat_drm(result: PEAnalysisResult) -> dict[str, bool]:
    """Heuristic signatures for anti-cheat and DRM systems.

    Returns dict of booleans. These are detection flags, NOT bypass flags.
    The configurator uses them to set anti_cheat.mode = "safe" or warn user.
    """
    imps = {imp.dll_name.lower() for imp in result.imports}
    secs = {s.name.lower() for s in result.sections}
    out: dict[str, bool] = {}

    # BattlEye — typically BEClient.dll or BEService.exe nearby
    out["battleye"] = "beclient.dll" in imps or "beservice.exe" in str(result.path).lower()

    # Easy Anti-Cheat — EAC-related strings in imports or sections
    out["eac"] = any(
        x in imps for x in {
            "easyanticheat.dll",
            "eac_launcher.exe",
        }
    ) or any("easyanticheat" in s for s in secs)

    # Vanguard (Riot) — vgc.exe, vgk.sys references
    out["vanguard"] = "vgc.exe" in str(result.path).lower() or any(
        "vgk" in s for s in secs
    )

    # PunkBuster — pbcl.dll,pbsv.dll
    out["punkbuster"] = any(x in imps for x in {"pbcl.dll", "pbsv.dll", "pbsv.exe"})

    # Denuvo — anti-tamper, usually high entropy .text, specific section names
    out["denuvo"] = (
        _text_entropy(result) > 7.5 and
        result.number_of_sections >= 5 and
        any(s in secs for s in {".arch", "denuvo", "__denuvo"})
    )

    # VMProtect — section names, high entropy
    out["vmprotect"] = (
        any("vmp" in s for s in secs) or
        any("vmprotect" in s for s in secs)
    )

    # Themida — section names .themida or high entropy + import packing
    out["themida"] = (
        any("themida" in s for s in secs) or
        (result.exports_count == 0 and _text_entropy(result) > 7.2)
    )

    # Widevine (DRM, not anti-cheat but related)
    out["widevine"] = "widevinecdmadapter.dll" in imps or "widevine.dll" in imps

    return out


# ---------------------------------------------------------------------------
# Packer detection
# ---------------------------------------------------------------------------

def detect_packed(result: PEAnalysisResult) -> bool:
    """Heuristic packer detection."""
    text_ent = _text_entropy(result)
    # High entropy in .text suggests packing or obfuscation
    if text_ent > 7.5:
        return True
    # Very few imports with small .text but large image
    if len(result.imports) < 3 and result.number_of_sections <= 3:
        return True
    # Specific packer sections
    secs = {s.name.lower() for s in result.sections}
    if any(s in secs for s in {"upx0", "upx1", "aspack", "petite"}):
        return True
    return False


# ---------------------------------------------------------------------------
# .NET detection (deep)
# ---------------------------------------------------------------------------

def detect_dotnet(result: PEAnalysisResult) -> bool:
    """Detect .NET/CLR applications beyond simple mscoree.dll import."""
    imps = {imp.dll_name.lower() for imp in result.imports}
    if "mscoree.dll" in imps or "clr.dll" in imps:
        return True
    # .NET metadata directory present (detected by lief via CLI header)
    # lief exposes has_dotnet_net on newer versions; fallback to imports
    return False


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def update_features(result: PEAnalysisResult, features: dict[str, Any]) -> None:
    """Mutate the feature dict in-place with heuristic-derived fields."""
    # Framework
    framework = detect_framework(result)
    fw_enc = {
        "qt": (1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0),
        "wpf": (0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0),
        "electron": (0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0),
        "gtk": (0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0),
        "unity": (0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0),
        "unreal": (0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0),
        "win32": (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
    }.get(framework, (0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0))
    features["heuristics.framework_qt"] = fw_enc[0]
    features["heuristics.framework_wpf"] = fw_enc[1]
    features["heuristics.framework_electron"] = fw_enc[2]
    features["heuristics.framework_gtk"] = fw_enc[3]
    features["heuristics.framework_unity"] = fw_enc[4]
    features["heuristics.framework_unreal"] = fw_enc[5]
    features["heuristics.framework_win32"] = fw_enc[6]

    # Packed / .NET / Installer
    features["heuristics.is_packed"] = 1.0 if detect_packed(result) else 0.0
    features["heuristics.is_dotnet"] = 1.0 if detect_dotnet(result) else 0.0

    # Anti-cheat / DRM
    ac = detect_anticheat_drm(result)
    features["anticheat.battleye"] = 1.0 if ac["battleye"] else 0.0
    features["anticheat.eac"] = 1.0 if ac["eac"] else 0.0
    features["anticheat.vanguard"] = 1.0 if ac["vanguard"] else 0.0
    features["anticheat.punkbuster"] = 1.0 if ac["punkbuster"] else 0.0
    features["anticheat.denuvo"] = 1.0 if ac["denuvo"] else 0.0
    features["anticheat.vmprotect"] = 1.0 if ac["vmprotect"] else 0.0
    features["anticheat.themida"] = 1.0 if ac["themida"] else 0.0
    features["anticheat.widevine"] = 1.0 if ac["widevine"] else 0.0

    # Text editor detection
    features["heuristics.is_text_editor"] = 1.0 if detect_text_editor(result) else 0.0

    # Installer detection (cross-module call)
    from app.configurator.ai.pe_analyzer import installers
    installer_type = installers.detect_installer(result)
    features["heuristics.is_installer"] = 1.0 if installer_type != "none" else 0.0
