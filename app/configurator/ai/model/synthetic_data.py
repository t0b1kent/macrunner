"""Balanced synthetic training data generator for ML model (Phase A.5 → A.7).

Target distribution (addressing class-imbalance bias discovered in Session 8):
  - Game (DX9/11/12, OpenGL, Vulkan, anti-cheat):        ~28%
  - Productivity (text editors, media players, archivers): ~18%
  - Business (Office-style, CAD, accounting):             ~18%
  - System utility (calc, hello, simple CLI):             ~12%
  - Framework apps (Qt, WPF, GTK, Electron, WinForms):     ~12%
  - CRT / runtime libs (vcredist, dotnet):                ~7%
  - ARM64 variants across all categories:                 included in counts above

Why: Session 8 validation showed 77.8% game bias (7 of 9 archetypes) causing
35.5% category accuracy on real fixtures. Balanced training data is a
necessary (not sufficient) condition for production sign-off.
"""

from __future__ import annotations

import copy
import random
from typing import Any


# ---------------------------------------------------------------------------
# Base archetypes — expanded from 9 → 25 to cover all target categories
# ---------------------------------------------------------------------------

_ARCHETYPES: list[dict[str, Any]] = [
    # ================================================================
    # CATEGORY: game  (target ~28% of total)
    # ================================================================
    {
        "name": "dx12_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 1, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 1, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 6, "import_count": 45, "export_count": 8,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "d3d12",
            "default_lane": "arm64-hyperbridge",
        },
    },
    {
        "name": "dx11_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 1, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 1, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 7, "import_count": 62, "export_count": 12,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "d3d11",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "dx9_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 1,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 5, "import_count": 28, "export_count": 4,
            "has_resources": 1, "has_signature": 0, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "game", "graphics.api": "d3d9",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "anticheat_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 1, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 1,
            "has_anticheat_signature": 1, "is_dotnet": 0, "is_installer": 0, "is_packed": 1,
            "framework_unity": 0, "framework_unreal": 1, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 8, "import_count": 55, "export_count": 15,
            "has_resources": 1, "has_signature": 1, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "d3d12",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "opengl_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 1, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 1, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 6, "import_count": 38, "export_count": 6,
            "has_resources": 1, "has_signature": 0, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "opengl",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "vulkan_game",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 1,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 1, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 7, "import_count": 42, "export_count": 10,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "vulkan",
            "default_lane": "arm64-hyperbridge",
        },
    },
    {
        "name": "arm64_game",
        "features": {
            "arch_x86_64": 0, "arch_arm64": 1, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 1, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 1, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 5, "import_count": 30, "export_count": 5,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "game", "graphics.api": "d3d12",
            "default_lane": "arm64-native",
        },
    },

    # ================================================================
    # CATEGORY: productivity  (target ~18% of total)
    # ================================================================
    {
        "name": "text_editor",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 1, "framework_gtk": 0,
            "is_text_editor": 1,
            "section_count": 5, "import_count": 22, "export_count": 3,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "media_player",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 1, "framework_qt": 0, "framework_gtk": 0,
            "section_count": 5, "import_count": 35, "export_count": 5,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "archive_tool",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 0,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 4, "import_count": 18, "export_count": 2,
            "has_resources": 0, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "arm64_productivity",
        "features": {
            "arch_x86_64": 0, "arch_arm64": 1, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 4, "import_count": 14, "export_count": 3,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "arm64-native",
        },
    },

    # ================================================================
    # CATEGORY: business  (target ~18% of total)
    # ================================================================
    {
        "name": "business_wpf_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 1,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 4, "import_count": 35, "export_count": 4,
            "has_resources": 1, "has_signature": 1, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "cad_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 1, "import_d3d9": 0,
            "import_opengl32": 1, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 1, "framework_gtk": 0,
            "section_count": 8, "import_count": 55, "export_count": 20,
            "has_resources": 1, "has_signature": 1, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "accounting_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 1, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "section_count": 4, "import_count": 40, "export_count": 5,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "arm64_business",
        "features": {
            "arch_x86_64": 0, "arch_arm64": 1, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 1, "framework_gtk": 0,
            "section_count": 5, "import_count": 28, "export_count": 6,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "arm64-native",
        },
    },

    # ================================================================
    # CATEGORY: system utility  (target ~12% of total)
    # ================================================================
    {
        "name": "simple_calc",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 4, "import_count": 12, "export_count": 2,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "hello_cli",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 0, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 0, "import_gdi32": 0,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 3, "import_count": 4, "export_count": 0,
            "has_resources": 0, "has_signature": 0, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "system_tool",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 0, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 0,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 4, "import_count": 20, "export_count": 3,
            "has_resources": 0, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "arm64_hello_cli",
        "features": {
            "arch_x86_64": 0, "arch_arm64": 1, "arch_i386": 0,
            "subsystem_windows_gui": 0, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 0, "import_gdi32": 0,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 3, "import_count": 5, "export_count": 0,
            "has_resources": 0, "has_signature": 0, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "arm64-native",
        },
    },

    # ================================================================
    # CATEGORY: framework apps  (target ~12% of total)
    # ================================================================
    {
        "name": "qt_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 1, "framework_gtk": 0,
            "section_count": 6, "import_count": 32, "export_count": 8,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "electron_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 1, "import_d3d9": 0,
            "import_opengl32": 1, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 0, "import_ws2_32": 1,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 1, "framework_qt": 0, "framework_gtk": 0,
            "section_count": 5, "import_count": 55, "export_count": 12,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "gtk_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 1,
            "section_count": 6, "import_count": 38, "export_count": 10,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "winforms_app",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 1,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 1, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "section_count": 4, "import_count": 30, "export_count": 5,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 1,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },

    # ================================================================
    # CATEGORY: CRT / runtime  (target ~7% of total)
    # ================================================================
    {
        "name": "vcredist_installer",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 1, "subsystem_windows_cui": 0, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 1, "import_gdi32": 0,
            "import_shell32": 1, "import_advapi32": 1, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 0, "is_installer": 1, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 3, "import_count": 10, "export_count": 1,
            "has_resources": 1, "has_signature": 1, "has_debug": 0, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "utility", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "dotnet_runtime_probe",
        "features": {
            "arch_x86_64": 1, "arch_arm64": 0, "arch_i386": 0,
            "subsystem_windows_gui": 0, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 0, "import_gdi32": 0,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 3, "import_count": 8, "export_count": 0,
            "has_resources": 0, "has_signature": 0, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "x86_64-rosetta",
        },
    },
    {
        "name": "arm64_dotnet_probe",
        "features": {
            "arch_x86_64": 0, "arch_arm64": 1, "arch_i386": 0,
            "subsystem_windows_gui": 0, "subsystem_windows_cui": 1, "subsystem_efi_app": 0,
            "import_d3d12": 0, "import_d3d11": 0, "import_d3d9": 0,
            "import_opengl32": 0, "import_vulkan": 0,
            "import_kernel32": 1, "import_user32": 0, "import_gdi32": 0,
            "import_shell32": 0, "import_advapi32": 0, "import_ws2_32": 0,
            "has_anticheat_signature": 0, "is_dotnet": 1, "is_installer": 0, "is_packed": 0,
            "framework_unity": 0, "framework_unreal": 0, "framework_wpf": 0,
            "framework_winforms": 0, "framework_electron": 0, "framework_qt": 0, "framework_gtk": 0,
            "is_text_editor": 0,
            "section_count": 3, "import_count": 9, "export_count": 0,
            "has_resources": 0, "has_signature": 0, "has_debug": 1, "entry_point_present": 1, "large_address_aware": 0,
        },
        "labels": {
            "category": "business", "graphics.api": "none",
            "default_lane": "arm64-native",
        },
    },
]


# ---------------------------------------------------------------------------
# Target class weights (stratified sampling)
# ---------------------------------------------------------------------------

# Map each archetype to its target group so we can enforce distribution
_GROUP_WEIGHTS: dict[str, float] = {
    # Games: 7 archetypes share ~28% → each gets ~4% weight
    "game": 0.28,
    # Productivity: 4 archetypes share ~18% → each gets ~4.5% weight
    "productivity": 0.18,
    # Business: 4 archetypes share ~18% → each gets ~4.5% weight
    "business": 0.18,
    # System utility: 4 archetypes share ~12% → each gets ~3% weight
    "system_utility": 0.12,
    # Framework: 4 archetypes share ~12% → each gets ~3% weight
    "framework": 0.12,
    # CRT/runtime: 3 archetypes share ~7% → each gets ~2.3% weight
    "crt_runtime": 0.07,
}


def _archetype_group(name: str) -> str:
    """Map archetype name to target distribution group."""
    if name.endswith("_game") or name in ("dx12_game", "dx11_game", "dx9_game", "anticheat_game", "opengl_game", "vulkan_game", "arm64_game"):
        return "game"
    if name in ("text_editor", "media_player", "archive_tool", "arm64_productivity"):
        return "productivity"
    if name in ("business_wpf_app", "cad_app", "accounting_app", "arm64_business"):
        return "business"
    if name in ("simple_calc", "hello_cli", "system_tool", "arm64_hello_cli"):
        return "system_utility"
    if name in ("qt_app", "electron_app", "gtk_app", "winforms_app"):
        return "framework"
    if name in ("vcredist_installer", "dotnet_runtime_probe", "arm64_dotnet_probe"):
        return "crt_runtime"
    return "game"  # fallback


def _inject_noise(features: dict[str, Any], rng: random.Random, intensity: float = 0.1) -> dict[str, Any]:
    out = copy.deepcopy(features)
    for key, value in list(out.items()):
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if rng.random() < intensity:
                noise = rng.gauss(0, abs(value) * intensity + 0.1)
                out[key] = max(0, value + noise)
        elif isinstance(value, bool):
            if rng.random() < intensity * 0.5:
                out[key] = not value
    return out


def _permute_features(features: dict[str, Any], rng: random.Random) -> dict[str, Any]:
    out = copy.deepcopy(features)
    import_flags = [k for k in out if k.startswith("import_")]
    if import_flags and rng.random() < 0.3:
        flag = rng.choice(import_flags)
        out[flag] = 1 if not out[flag] else 0
    if "section_count" in out:
        out["section_count"] = max(1, out["section_count"] + rng.randint(-2, 2))
    if "import_count" in out:
        out["import_count"] = max(0, out["import_count"] + rng.randint(-5, 5))
    if "export_count" in out:
        out["export_count"] = max(0, out["export_count"] + rng.randint(-3, 3))
    return out


def generate_synthetic_dataset(
    n_samples: int = 1000,
    seed: int | None = None,
    noise_intensity: float = 0.1,
) -> list[dict[str, Any]]:
    """Generate a stratified synthetic training dataset.

    Uses target class weights to avoid the game-majority bias that
    produced 35.5% category accuracy in Session 8.
    """
    rng = random.Random(seed)

    # Build per-archetype sampling weights using group quotas
    archetype_groups = {a["name"]: _archetype_group(a["name"]) for a in _ARCHETYPES}
    group_counts: dict[str, int] = {}
    for g in archetype_groups.values():
        group_counts[g] = group_counts.get(g, 0) + 1

    archetype_weights: dict[str, float] = {}
    for a in _ARCHETYPES:
        g = archetype_groups[a["name"]]
        archetype_weights[a["name"]] = _GROUP_WEIGHTS[g] / group_counts[g]

    # Normalise to probabilities
    total_w = sum(archetype_weights.values())
    probs = [archetype_weights[a["name"]] / total_w for a in _ARCHETYPES]

    samples: list[dict[str, Any]] = []
    archetype_names = [a["name"] for a in _ARCHETYPES]
    # Pre-compute index lookup
    name_to_idx = {a["name"]: i for i, a in enumerate(_ARCHETYPES)}

    for _ in range(n_samples):
        chosen_name = rng.choices(archetype_names, weights=probs, k=1)[0]
        archetype = _ARCHETYPES[name_to_idx[chosen_name]]
        features = _inject_noise(
            _permute_features(archetype["features"], rng),
            rng,
            intensity=noise_intensity,
        )
        labels = copy.deepcopy(archetype["labels"])
        samples.append(
            {
                "features": features,
                "labels": labels,
                "archetype": archetype["name"],
            }
        )

    return samples


def evaluate_predictor(
    predictor: Any,
    dataset: list[dict[str, Any]],
    target_fields: tuple[str, ...] = ("category", "graphics.api", "default_lane"),
) -> dict[str, float]:
    """Evaluate a predictor against a synthetic dataset.

    Returns per-field accuracy and overall accuracy.
    """
    correct: dict[str, int] = {field: 0 for field in target_fields}
    total = len(dataset)

    for sample in dataset:
        result = predictor.predict(sample["features"])
        predicted = predictor.predict_profile_dict(sample["features"])
        for field in target_fields:
            parts = field.split(".")
            node = predicted
            for part in parts[:-1]:
                node = node.get(part, {})
            pred_val = node.get(parts[-1]) if isinstance(node, dict) else None
            true_val = sample["labels"].get(field)
            if pred_val == true_val:
                correct[field] += 1

    return {
        "overall": sum(correct.values()) / (total * len(target_fields)),
        **{field: correct[field] / total for field in target_fields},
    }
