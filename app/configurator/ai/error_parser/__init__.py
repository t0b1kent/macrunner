"""MacRunner AI Configurator — Error Parser (Phase A.3).

Parse Wine stderr logs, match error signatures to known solutions, and
suggest profile overrides.

Architecture:
- PatternDB: YAML/JSON-backed regex pattern database built from real logs
- LogParser: line-by-line ingestion with multi-line stack trace support
- SuggestionEngine: maps matched errors → profile overrides with confidence
"""

from __future__ import annotations

import copy
import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence


@dataclass(frozen=True)
class WineError:
    """A single extracted error from a Wine log."""

    category: str  # e.g. "err", "fixme", "warn", "fault", "signal"
    subcategory: str  # e.g. "virtual", "module", "file", "keyboard"
    message: str  # full matched line(s)
    matched_pattern_id: str  # reference into PatternDB
    severity: str  # "critical", "high", "medium", "low", "info"
    raw_lines: tuple[str, ...] = ()  # context lines for multi-line traces


@dataclass(frozen=True)
class Suggestion:
    """A profile tweak suggested to resolve a matched error."""

    field: str  # dot-path, e.g. "env.MACRUNNER_SAFE_MODE", "graphics.api"
    value: Any
    confidence: float  # 0.0–1.0
    reason: str  # human-readable why
    auto_apply: bool  # if True, can be applied without confirmation
    requires_confirmation: bool  # inverse of auto_apply for clarity
    source_pattern_id: str  # which pattern produced this

    def __post_init__(self) -> None:
        object.__setattr__(
            self, "requires_confirmation", not self.auto_apply
        )


@dataclass
class ParsedLog:
    """Result of parsing a complete stderr log."""

    errors: list[WineError] = field(default_factory=list)
    suggestions: list[Suggestion] = field(default_factory=list)
    summary: dict[str, int] = field(default_factory=dict)


class PatternDB:
    """In-memory regex pattern database with built-in real-evidence patterns.

    Patterns are tuples of (pattern_id, regex, category, severity, suggestion_builder).
    suggestion_builder is a callable that receives the Match object and returns
    a list of Suggestion instances.
    """

    # -----------------------------------------------------------------------
    # External pattern loading
    # -----------------------------------------------------------------------
    _EXTERNAL_PATTERN_DIR = (
        Path(__file__).resolve().parent / "patterns"
    )

    def __init__(self, extra_patterns: Sequence[dict[str, Any]] | None = None) -> None:
        self._patterns: list[tuple[str, re.Pattern[str], str, str, Any]] = []
        self._load_builtin()
        self._load_external_dir(self._EXTERNAL_PATTERN_DIR)
        if extra_patterns:
            for p in extra_patterns:
                self._register(
                    p["id"],
                    p["regex"],
                    p.get("category", "unknown"),
                    p.get("severity", "medium"),
                    p.get("suggestions", []),
                )

    @classmethod
    def _load_external_file(cls, path: Path) -> list[dict[str, Any]]:
        """Load patterns from a single JSON or YAML file."""
        text = path.read_text(encoding="utf-8")
        if path.suffix in {".yaml", ".yml"}:
            try:
                import yaml
            except ImportError as exc:  # pragma: no cover
                raise ImportError(
                    "YAML pattern files require PyYAML; install with: pip install pyyaml"
                ) from exc
            data = yaml.safe_load(text)
        else:
            data = json.loads(text)
        if not isinstance(data, list):
            raise ValueError(f"{path}: pattern file must be a list of objects")
        return data

    def _load_external_dir(self, directory: Path) -> None:
        """Load all .json / .yaml / .yml files from a directory."""
        if not directory.exists():
            return
        for path in sorted(directory.glob("*.json")) + sorted(directory.glob("*.yaml")) + sorted(directory.glob("*.yml")):
            try:
                patterns = self._load_external_file(path)
                for p in patterns:
                    self._register(
                        p["id"],
                        re.compile(p["regex"]),
                        p.get("category", "unknown"),
                        p.get("severity", "medium"),
                        p.get("suggestions", []),
                    )
            except Exception as exc:  # pragma: no cover
                # External patterns are best-effort; don't crash on malformed files
                pass

    def _load_builtin(self) -> None:
        # ——— Critical: HyperBridge unsupported opcode / runtime fail ———
        self._register(
            "hb_unsupported_opcode",
            re.compile(
                r"(?:err:module:macrunner_hb_run_x64.*?UNSUPPORTED_OPCODE|"
                r"macrunner-hb-runtime-fail:.*?UNSUPPORTED_OPCODE|"
                r"macrunner-hb-fault:.*?ir_op=UNSUPPORTED)"
            ),
            "fault",
            "critical",
            [
                {
                    "field": "env.MACRUNNER_HB_X64_LOADER",
                    "value": "0",
                    "confidence": 0.75,
                    "reason": "HyperBridge encountered unsupported x86 opcode; disable HB x64 loader and fall back to Rosetta",
                    "auto_apply": False,
                },
                {
                    "field": "default_lane",
                    "value": "x86_64-rosetta",
                    "confidence": 0.80,
                    "reason": "HyperBridge cannot translate this binary; use Rosetta lane",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Critical: HyperBridge thread callback failed ———
        self._register(
            "hb_thread_callback_failed",
            re.compile(
                r"err:module:macrunner_hb_BaseThreadInitThunk MacRunner HyperBridge thread callback failed"
            ),
            "err",
            "critical",
            [
                {
                    "field": "env.MACRUNNER_HB_STACK_RESERVE",
                    "value": "large",
                    "confidence": 0.60,
                    "reason": "Thread init thunk failed; increase HyperBridge stack reserve",
                    "auto_apply": False,
                },
                {
                    "field": "default_lane",
                    "value": "x86_64-rosetta",
                    "confidence": 0.70,
                    "reason": "HB thread callback unstable; fallback to Rosetta",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Critical: SIGSEGV in HyperBridge ———
        self._register(
            "hb_sigsegv",
            re.compile(
                r"macrunner-hb-(?:primary-entry|signal-chain):.*?sig=11\b"
            ),
            "signal",
            "critical",
            [
                {
                    "field": "env.MACRUNNER_SAFE_MODE",
                    "value": "1",
                    "confidence": 0.70,
                    "reason": "SIGSEGV in HyperBridge; enable safe mode to disable risky optimizations",
                    "auto_apply": False,
                },
                {
                    "field": "env.MACRUNNER_HB_STACK_RESERVE",
                    "value": "large",
                    "confidence": 0.55,
                    "reason": "SIGSEGV may indicate stack overflow; increase stack reserve",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: SIGILL in HyperBridge ———
        self._register(
            "hb_sigill",
            re.compile(
                r"macrunner-hb-(?:primary-entry|signal-chain):.*?sig=4\b"
            ),
            "signal",
            "high",
            [
                {
                    "field": "env.MACRUNNER_HB_X64_LOADER",
                    "value": "0",
                    "confidence": 0.80,
                    "reason": "SIGILL indicates illegal instruction translation; disable HB x64 loader",
                    "auto_apply": False,
                },
                {
                    "field": "default_lane",
                    "value": "x86_64-rosetta",
                    "confidence": 0.85,
                    "reason": "Illegal instruction in HB; Rosetta is more reliable for this binary",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: bus error in HyperBridge ———
        self._register(
            "hb_bus_error",
            re.compile(
                r"err:seh:bus_handler macrunner-hb-signal-entry: kind=bus"
            ),
            "signal",
            "high",
            [
                {
                    "field": "env.MACRUNNER_SAFE_MODE",
                    "value": "1",
                    "confidence": 0.65,
                    "reason": "Bus error in HyperBridge; safe mode reduces memory alignment risks",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: mprotect_exec failed ———
        self._register(
            "mprotect_exec_failed",
            re.compile(
                r"err:virtual:mprotect_exec mprotect_exec failed Invalid argument"
            ),
            "err",
            "high",
            [
                {
                    "field": "bottle_policy.isolation",
                    "value": "dedicated",
                    "confidence": 0.70,
                    "reason": "mprotect_exec failed on shared bottle; dedicated isolation may help",
                    "auto_apply": False,
                },
                {
                    "field": "performance.fast_io",
                    "value": False,
                    "confidence": 0.55,
                    "reason": "Fast I/O may interfere with memory protection; disable as precaution",
                    "auto_apply": True,
                },
            ],
        )

        # ——— High: ZwLoadDriver winebth failed ———
        self._register(
            "zwload_driver_winebth",
            re.compile(
                r"err:ntoskrnl:ZwLoadDriver failed to create driver .*?winebth.*?c00000e5"
            ),
            "err",
            "high",
            [
                {
                    "field": "wine_settings.bluetooth",
                    "value": "disabled",
                    "confidence": 0.85,
                    "reason": "Wine Bluetooth driver fails to load; disable to avoid startup crash",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: keyboard locale not supported ———
        self._register(
            "keyboard_locale_unsupported",
            re.compile(
                r"fixme:keyboard:NtUserActivateKeyboardLayout Changing user locale is not supported"
            ),
            "fixme",
            "medium",
            [
                {
                    "field": "wine_settings.locale",
                    "value": "en_US.UTF-8",
                    "confidence": 0.90,
                    "reason": "Locale switching unsupported; force US UTF-8",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: ARM64 get_core_id_regs stub ———
        self._register(
            "arm64_core_id_stub",
            re.compile(
                r"fixme:ntdll:get_core_id_regs_arm64 stub"
            ),
            "fixme",
            "medium",
            [
                {
                    "field": "performance.threading",
                    "value": "single",
                    "confidence": 0.60,
                    "reason": "Core ID detection stubbed; single-threading avoids thread-affinity issues",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: winebth service auto-start failed ———
        self._register(
            "winebth_autostart_failed",
            re.compile(
                r"fixme:service:scmdatabase_autostart_services Auto-start service L\"winebth\" failed to start: 1359"
            ),
            "fixme",
            "medium",
            [
                {
                    "field": "wine_settings.bluetooth",
                    "value": "disabled",
                    "confidence": 0.85,
                    "reason": "winebth service auto-start fails; disable Bluetooth in Wine",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: wineusb unhandled query ———
        self._register(
            "wineusb_unhandled_query",
            re.compile(
                r"fixme:wineusb:query_id Unhandled ID query type 0x5"
            ),
            "fixme",
            "low",
            [
                {
                    "field": "wine_settings.usb_redirect",
                    "value": "disabled",
                    "confidence": 0.50,
                    "reason": "Unhandled USB query; disable USB redirect",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: missing network DLLs in drivers path ———
        self._register(
            "missing_network_dll",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\drivers\\(?:ws2_32|nsi|iphlpapi|dnsapi)\.dll\" not found \(c0000034\)"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_NET_BACKEND",
                    "value": "nw",
                    "confidence": 0.65,
                    "reason": "Network stack DLLs missing; use native NW.framework backend",
                    "auto_apply": False,
                },
                {
                    "field": "required_dlls",
                    "value": ("ws2_32", "nsi", "iphlpapi", "dnsapi"),
                    "confidence": 0.55,
                    "reason": "Add missing network DLLs to required list (winetricks may help)",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: missing common DLLs in app dir ———
        self._register(
            "missing_common_dll",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\(?:mpr\.dll|dwmapi\.dll|dbghelp\.dll|cryptbase\.dll|bcrypt\.dll|WINTRUST\.dll|WININET\.dll|VERSION\.dll|UxTheme\.dll|SensApi\.dll|CRYPT32\.dll)\" not found \(c0000034\)"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": (
                        "mpr",
                        "dwmapi",
                        "dbghelp",
                        "cryptbase",
                        "bcrypt",
                        "wintrust",
                        "wininet",
                        "version",
                        "uxtheme",
                        "sensapi",
                        "crypt32",
                    ),
                    "confidence": 0.60,
                    "reason": "Common Windows DLLs missing; install via winetricks or bundled redist",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: FreeType not found ———
        self._register(
            "freetype_missing",
            re.compile(
                r"Wine cannot find the FreeType font library"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.font_smoothing",
                    "value": "disabled",
                    "confidence": 0.70,
                    "reason": "FreeType missing; disable font smoothing to avoid rendering crashes",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Low: DOSATTRIB extended attribute failure ———
        self._register(
            "dosattrib_ea_failed",
            re.compile(
                r"warn:file:fd_set_file_info Failed to set extended attribute user\.DOSATTRIB\. errno 93"
            ),
            "warn",
            "low",
            [
                {
                    "field": "bottle_policy.isolation",
                    "value": "dedicated",
                    "confidence": 0.50,
                    "reason": "Extended attribute failure on shared bottle; dedicated may reduce conflicts",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Low: NtCreateFile STATUS_OBJECT_NAME_COLLISION (c0000035) ———
        self._register(
            "name_collision_system32",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\windows\\system32\" not found \(c0000035\)"
            ),
            "warn",
            "low",
            [
                {
                    "field": "wine_settings.init_prefix",
                    "value": "true",
                    "confidence": 0.45,
                    "reason": "system32 name collision; ensure prefix is fully initialized",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Low: manifest files not found ———
        self._register(
            "manifest_not_found",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\.(?:exe|dll)\.manifest\" not found \(c0000034\)"
            ),
            "warn",
            "low",
            [
                {
                    "field": "wine_settings.manifest",
                    "value": "embedded",
                    "confidence": 0.40,
                    "reason": "Manifest file missing; prefer embedded manifests",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Info: HyperBridge gate env check ———
        self._register(
            "hb_gate_env_unset",
            re.compile(
                r"macrunner_hb_x64_main_requested MacRunner HyperBridge gate current=aa64 has_env=0 value=\(unset\) enabled=0"
            ),
            "trace",
            "info",
            [
                {
                    "field": "env.MACRUNNER_HB_X64_LOADER",
                    "value": "1",
                    "confidence": 0.95,
                    "reason": "HB x64 loader gate disabled because env var unset; enable for x86_64 main execution",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Info: HyperBridge PE call stack bounds ———
        self._register(
            "hb_stack_bounds_warning",
            re.compile(
                r"macrunner_hb_call_arm64_pe_import12 MacRunner HyperBridge PE call stack bounds"
            ),
            "trace",
            "info",
            [
                {
                    "field": "env.MACRUNNER_HB_STACK_RESERVE",
                    "value": "large",
                    "confidence": 0.40,
                    "reason": "PE call stack bounds tight; consider larger stack reserve",
                    "auto_apply": False,
                },
            ],
        )

        # =====================================================================
        # Phase A.3 expanded patterns (Session 5) — 32 new patterns
        # =====================================================================

        # ——— Critical: D3D11/D3D12 device creation failed ———
        self._register(
            "d3d_device_creation_failed",
            re.compile(
                r"err:d3d(?:11|12):.*?(?:CreateDevice|D3D11CreateDevice|D3D12CreateDevice).*?failed|"
                r"err:dxgi:.*?(?:CreateSwapChain|MakeAssociation).*?failed"
            ),
            "err",
            "critical",
            [
                {
                    "field": "graphics.preferred_backend",
                    "value": "dxvk-moltenvk",
                    "confidence": 0.75,
                    "reason": "D3D device creation failed; fallback to DXVK+MoltenVK",
                    "auto_apply": False,
                },
                {
                    "field": "env.MACRUNNER_GFX_BACKEND",
                    "value": "dxvk",
                    "confidence": 0.70,
                    "reason": "Force DXVK backend when native D3D fails",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: D3D shader compilation failed ———
        self._register(
            "d3d_shader_compile_failed",
            re.compile(
                r"err:d3d(?:11|12):.*?shader compilation failed|"
                r"err:d3d:.*?compile_shader.*?(?:error|failed)"
            ),
            "err",
            "high",
            [
                {
                    "field": "graphics.shader_cache",
                    "value": False,
                    "confidence": 0.65,
                    "reason": "Shader compilation failing; disable cache to force recompile",
                    "auto_apply": True,
                },
                {
                    "field": "env.MACRUNNER_SHADER_CACHE_ROOT",
                    "value": "",
                    "confidence": 0.55,
                    "reason": "Clear shader cache path to force fresh compilation",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: DXGI adapter enumeration failed ———
        self._register(
            "dxgi_adapter_enum_failed",
            re.compile(
                r"err:dxgi:.*?EnumAdapters|err:dxgi:.*?adapter not found|"
                r"err:dxgi:.*?CreateDXGIFactory.*?failed"
            ),
            "err",
            "high",
            [
                {
                    "field": "graphics.preferred_backend",
                    "value": "d3dmetal",
                    "confidence": 0.70,
                    "reason": "DXGI adapter enumeration failed; try D3DMetal backend",
                    "auto_apply": False,
                },
                {
                    "field": "env.MACRUNNER_GFX_BACKEND",
                    "value": "d3dmetal",
                    "confidence": 0.65,
                    "reason": "Force D3DMetal when DXGI factory fails",
                    "auto_apply": False,
                },
            ],
        )

        # ——— High: fixme dxgi present ———
        self._register(
            "dxgi_present_fixme",
            re.compile(
                r"fixme:dxgi:.*?Present.*?(?:not implemented|stub|ignored)"
            ),
            "fixme",
            "high",
            [
                {
                    "field": "env.MACRUNNER_GFX_PRESENT_MODE",
                    "value": "mailbox",
                    "confidence": 0.60,
                    "reason": "DXGI Present path incomplete; use mailbox present mode",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: Vulkan loader not found ———
        self._register(
            "vulkan_loader_missing",
            re.compile(
                r"err:vulkan:.*?vkCreateInstance.*?failed|"
                r"err:vulkan:.*?could not load vulkan\.dll|"
                r"err:vulkan:.*?loader not found"
            ),
            "err",
            "medium",
            [
                {
                    "field": "graphics.api",
                    "value": "d3d11",
                    "confidence": 0.70,
                    "reason": "Vulkan loader missing; fallback to D3D11",
                    "auto_apply": False,
                },
                {
                    "field": "required_dlls",
                    "value": ("vulkan-1",),
                    "confidence": 0.50,
                    "reason": "Install Vulkan loader via winetricks or MoltenVK bundle",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: mmdevapi audio init failed ———
        self._register(
            "mmdevapi_init_failed",
            re.compile(
                r"err:mmdevapi:.*?AudioClient.*?Initialize.*?failed|"
                r"err:mmdevapi:.*?CreateDevice.*?failed|"
                r"err:mmdevapi:.*?CoCreateInstance.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.audio_backend",
                    "value": "coreaudio",
                    "confidence": 0.75,
                    "reason": "MMDevAPI init failed; force CoreAudio backend on macOS",
                    "auto_apply": True,
                },
                {
                    "field": "env.MACRUNNER_AUDIO_BACKEND",
                    "value": "coreaudio",
                    "confidence": 0.70,
                    "reason": "Set CoreAudio as audio backend",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: ALSA audio errors ———
        self._register(
            "alsa_audio_error",
            re.compile(
                r"err:alsa:.*?snd_pcm_open|err:alsa:.*?device.*?busy|"
                r"err:alsa:.*?cannot open|warn:alsa:.*?no soundcards found"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.audio_backend",
                    "value": "coreaudio",
                    "confidence": 0.85,
                    "reason": "ALSA unavailable on macOS; force CoreAudio",
                    "auto_apply": True,
                },
                {
                    "field": "env.MACRUNNER_AUDIO_BACKEND",
                    "value": "coreaudio",
                    "confidence": 0.85,
                    "reason": "Disable ALSA, use CoreAudio",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: coreaudio stream format mismatch ———
        self._register(
            "coreaudio_format_mismatch",
            re.compile(
                r"err:coreaudio:.*?stream format mismatch|"
                r"err:coreaudio:.*?invalid sample rate|"
                r"err:coreaudio:.*?AudioUnitRender.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.audio_sample_rate",
                    "value": 48000,
                    "confidence": 0.65,
                    "reason": "Audio format mismatch; force 48kHz sample rate",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: fixme audio channel mapping ———
        self._register(
            "audio_channel_mapping_fixme",
            re.compile(
                r"fixme:audio:.*?channel mapping.*?(?:not supported|stub)|"
                r"fixme:mmdevapi:.*?GetChannelCount"
            ),
            "fixme",
            "low",
            [
                {
                    "field": "wine_settings.audio_channels",
                    "value": "stereo",
                    "confidence": 0.55,
                    "reason": "Channel mapping not supported; force stereo",
                    "auto_apply": True,
                },
            ],
        )

        # ——— High: registry access denied ———
        self._register(
            "registry_access_denied",
            re.compile(
                r"err:reg:.*?access denied|err:advapi32:.*?RegOpenKey.*?failed|"
                r"err:reg:.*?cannot create key"
            ),
            "err",
            "high",
            [
                {
                    "field": "bottle_policy.isolation",
                    "value": "dedicated",
                    "confidence": 0.70,
                    "reason": "Registry access denied on shared bottle; dedicated isolation required",
                    "auto_apply": False,
                },
                {
                    "field": "wine_settings.registry",
                    "value": "user",
                    "confidence": 0.60,
                    "reason": "Force user-only registry writes",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: registry key not found ———
        self._register(
            "registry_key_not_found",
            re.compile(
                r"err:reg:.*?key not found|err:advapi32:.*?RegQueryValue.*?not found|"
                r"warn:reg:.*?creating key"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.registry",
                    "value": "create_missing",
                    "confidence": 0.50,
                    "reason": "Auto-create missing registry keys",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: missing MSXML / VC runtime ———
        self._register(
            "missing_msxml_vcredist",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\(?:msxml[3-6]|vcruntime140|msvcp140|msvcr140|"
                r"api-ms-win-crt-runtime|api-ms-win-crt-heap|api-ms-win-crt-stdio|"
                r"api-ms-win-crt-string)\.dll\" not found \(c0000034\)"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("vcrun2019", "vcrun2022", "msxml6"),
                    "confidence": 0.75,
                    "reason": "MSXML / VC runtime missing; install via winetricks",
                    "auto_apply": False,
                },
                {
                    "field": "wine_settings.vcrun_install",
                    "value": True,
                    "confidence": 0.70,
                    "reason": "Auto-install VC runtime redistributables",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: missing .NET Framework ———
        self._register(
            "missing_dotnet",
            re.compile(
                r"err:mscoree:.*?CLR initialization failed|"
                r"err:mscoree:.*?cannot load .NET|"
                r"warn:file:NtCreateFile L\".*?\\(?:clr|mscorlib|System\.dll)\" not found"
            ),
            "err",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("dotnet48", "dotnetcore3"),
                    "confidence": 0.70,
                    "reason": ".NET Framework missing; install via winetricks",
                    "auto_apply": False,
                },
                {
                    "field": "wine_settings.mono",
                    "value": True,
                    "confidence": 0.60,
                    "reason": "Enable Wine Mono as .NET fallback",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: missing DirectX redist DLLs ———
        self._register(
            "missing_dx_redist",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\(?:d3dcompiler_4[3-7]|d3dx9_.*|"
                r"xinput1_[3-4])\.dll\" not found \(c0000034\)"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("d3dx9", "d3dx10", "d3dx11", "xinput"),
                    "confidence": 0.70,
                    "reason": "DirectX redistributables missing; install via winetricks",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: missing OpenAL / XAudio ———
        self._register(
            "missing_openal_xaudio",
            re.compile(
                r"warn:file:NtCreateFile L\".*?\\(?:OpenAL32|XAudio2_8|XAudio2_9)\.dll\" not found"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("openal", "xaudio29"),
                    "confidence": 0.65,
                    "reason": "Audio middleware DLLs missing; install via winetricks",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: missing Media Foundation ———
        self._register(
            "missing_media_foundation",
            re.compile(
                r"err:mf:.*?MFCreateMediaSession|err:mfplat:.*?MFStartup|"
                r"warn:file:NtCreateFile L\".*?\\(?:mf|mfplat|mfreadwrite)\.dll\" not found"
            ),
            "err",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("mf", "mfplat", "mfreadwrite"),
                    "confidence": 0.65,
                    "reason": "Media Foundation missing; install via winetricks",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: GDI font / text metrics error ———
        self._register(
            "gdi_font_metrics_error",
            re.compile(
                r"err:gdi:.*?GetTextMetrics|err:gdi:.*?font.*?(?:not found|invalid)|"
                r"err:gdi:.*?CreateFontIndirect.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.font_smoothing",
                    "value": "disabled",
                    "confidence": 0.65,
                    "reason": "GDI font metrics failing; disable font smoothing",
                    "auto_apply": True,
                },
                {
                    "field": "wine_settings.locale",
                    "value": "en_US.UTF-8",
                    "confidence": 0.60,
                    "reason": "Font locale mismatch; force US UTF-8",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: shell32 path resolution error ———
        self._register(
            "shell32_path_error",
            re.compile(
                r"err:shell:.*?SHGetFolderPath|err:shell:.*?PathResolve|"
                r"err:shell:.*?CSIDL.*?(?:not found|invalid)"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.init_prefix",
                    "value": True,
                    "confidence": 0.55,
                    "reason": "Shell path resolution failing; ensure prefix is initialized",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: user32 window creation failed ———
        self._register(
            "user32_window_fail",
            re.compile(
                r"err:user32:.*?CreateWindowEx.*?failed|err:win:.*?window.*?(?:create|register).*?failed|"
                r"err:user32:.*?RegisterClass.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_DISABLE_OVERLAYS",
                    "value": "1",
                    "confidence": 0.60,
                    "reason": "Window creation failing; disable overlays that may conflict",
                    "auto_apply": False,
                },
                {
                    "field": "wine_settings.desktop",
                    "value": "virtual",
                    "confidence": 0.55,
                    "reason": "Use virtual desktop to isolate window creation",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: Wine prefix needs initialization ———
        self._register(
            "wine_prefix_not_initialized",
            re.compile(
                r"wine:.*?Wine prefix.*?not initialized|"
                r"wine:.*?configuration.*?L\".*?wineprefix\"|"
                r"err:setupapi:.*?CreateSetupQueue.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.init_prefix",
                    "value": True,
                    "confidence": 0.80,
                    "reason": "Wine prefix not initialized; trigger wineboot",
                    "auto_apply": False,
                },
                {
                    "field": "bottle_policy.isolation",
                    "value": "dedicated",
                    "confidence": 0.60,
                    "reason": "Dedicated bottle ensures clean prefix",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: Wine version mismatch / old prefix ———
        self._register(
            "wine_version_mismatch",
            re.compile(
                r"wine:.*?Wine build.*?different version|"
                r"wine:.*?prefix.*?was used by different Wine version|"
                r"warn:ntdll:.*?mismatched Wine version"
            ),
            "warn",
            "medium",
            [
                {
                    "field": "wine_settings.prefix_update",
                    "value": True,
                    "confidence": 0.75,
                    "reason": "Wine version mismatch; update prefix with new wineboot",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: heap allocation failed ———
        self._register(
            "heap_alloc_failed",
            re.compile(
                r"err:heap:.*?HeapAlloc.*?failed|err:heap:.*?out of memory|"
                r"err:ntdll:.*?RtlAllocateHeap.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_HB_MEMORY_LIMIT",
                    "value": "unlimited",
                    "confidence": 0.60,
                    "reason": "Heap allocation failing; remove artificial memory limits",
                    "auto_apply": False,
                },
                {
                    "field": "performance.fast_io",
                    "value": False,
                    "confidence": 0.50,
                    "reason": "Fast I/O may consume extra heap; disable as precaution",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Medium: virtual memory exhausted ———
        self._register(
            "virtual_memory_exhausted",
            re.compile(
                r"err:virtual:.*?out of memory|err:virtual:.*?mmap.*?failed|"
                r"err:virtual:.*?could not allocate memory"
            ),
            "err",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_HB_MEMORY_LIMIT",
                    "value": "unlimited",
                    "confidence": 0.65,
                    "reason": "Virtual memory exhausted; remove memory limits",
                    "auto_apply": False,
                },
                {
                    "field": "default_lane",
                    "value": "x86_64-rosetta",
                    "confidence": 0.55,
                    "reason": "Rosetta may have better memory handling for this app",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: ntdll process/thread creation failed ———
        self._register(
            "ntdll_process_thread_fail",
            re.compile(
                r"err:ntdll:.*?NtCreateThread.*?failed|err:ntdll:.*?NtCreateProcess.*?failed|"
                r"err:ntdll:.*?RtlCreateUserThread.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "performance.threading",
                    "value": "single",
                    "confidence": 0.60,
                    "reason": "Thread creation failing; try single-threaded mode",
                    "auto_apply": False,
                },
                {
                    "field": "env.MACRUNNER_HB_STACK_RESERVE",
                    "value": "large",
                    "confidence": 0.55,
                    "reason": "Thread creation may fail due to insufficient stack reserve",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: kernel32 DLL load failure ———
        self._register(
            "kernel32_dll_load_fail",
            re.compile(
                r"err:kernel32:.*?LoadLibrary.*?failed|err:kernel32:.*?GetProcAddress.*?failed|"
                r"err:kernel32:.*?DllMain.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "required_dlls",
                    "value": ("kernelbase", "api-ms-win-core-libraryloader"),
                    "confidence": 0.60,
                    "reason": "Kernel32 DLL dependency missing; install via winetricks",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: window metrics / registry corruption ———
        self._register(
            "window_metrics_corrupt",
            re.compile(
                r"err:win:.*?WindowMetrics.*?corrupt|err:reg:.*?WindowMetrics|"
                r"err:user32:.*?SPI_GETNONCLIENTMETRICS.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "wine_settings.registry",
                    "value": "reset_window_metrics",
                    "confidence": 0.70,
                    "reason": "WindowMetrics registry corruption detected; reset wine registry",
                    "auto_apply": False,
                },
                {
                    "field": "wine_settings.desktop",
                    "value": "virtual",
                    "confidence": 0.55,
                    "reason": "Use virtual desktop to avoid metric-dependent window sizing",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: TLS / SSL handshake failure ———
        self._register(
            "tls_ssl_handshake_fail",
            re.compile(
                r"err:secur32:.*?InitializeSecurityContext.*?failed|"
                r"err:wininet:.*?SSL.*?failed|err:schannel:.*?handshake.*?failed|"
                r"err:secur32:.*?SEC_E_.*?"
            ),
            "err",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_NET_TLS_VERSION",
                    "value": "1.2",
                    "confidence": 0.65,
                    "reason": "TLS handshake failing; force TLS 1.2",
                    "auto_apply": True,
                },
                {
                    "field": "wine_settings.crypto",
                    "value": "builtin",
                    "confidence": 0.55,
                    "reason": "Use built-in crypto instead of native schannel",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Medium: WinSock init failure ———
        self._register(
            "winsock_init_fail",
            re.compile(
                r"err:winsock:.*?WSAStartup.*?failed|err:winsock:.*?socket.*?failed|"
                r"err:winsock:.*?getaddrinfo.*?failed"
            ),
            "err",
            "medium",
            [
                {
                    "field": "env.MACRUNNER_NET_BACKEND",
                    "value": "nw",
                    "confidence": 0.75,
                    "reason": "WinSock init failing; use native NW.framework backend",
                    "auto_apply": False,
                },
                {
                    "field": "required_dlls",
                    "value": ("ws2_32",),
                    "confidence": 0.60,
                    "reason": "Ensure ws2_32.dll is available",
                    "auto_apply": False,
                },
            ],
        )

        # ——— Low: fixme ntdll async I/O ———
        self._register(
            "ntdll_async_io_fixme",
            re.compile(
                r"fixme:ntdll:.*?NtCancelIoFile.*?(?:stub|not implemented)|"
                r"fixme:ntdll:.*?NtQueryDirectoryFile.*?(?:stub|not implemented)"
            ),
            "fixme",
            "low",
            [
                {
                    "field": "performance.fast_io",
                    "value": False,
                    "confidence": 0.50,
                    "reason": "Async I/O stubbed; disable fast I/O to avoid reliance",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Low: fixme kernel32 file operations ———
        self._register(
            "kernel32_file_fixme",
            re.compile(
                r"fixme:kernel32:.*?FindFirstFileExW.*?(?:stub|ignored)|"
                r"fixme:kernel32:.*?GetFileInformationByHandleEx.*?(?:stub|ignored)"
            ),
            "fixme",
            "low",
            [
                {
                    "field": "performance.fast_io",
                    "value": False,
                    "confidence": 0.45,
                    "reason": "Extended file APIs stubbed; disable fast I/O",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Low: fixme explorer desktop integration ———
        self._register(
            "explorer_desktop_fixme",
            re.compile(
                r"fixme:explorer:.*?desktop.*?(?:stub|not implemented)|"
                r"fixme:shell:.*?SHCreateDefaultExtractIcon.*?(?:stub|not implemented)"
            ),
            "fixme",
            "low",
            [
                {
                    "field": "wine_settings.desktop",
                    "value": "none",
                    "confidence": 0.45,
                    "reason": "Explorer desktop integration stubbed; disable desktop",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Low: warn ole32 COM marshalling ———
        self._register(
            "ole32_com_warn",
            re.compile(
                r"warn:ole:.*?CoCreateInstance.*?failed|warn:ole:.*?marshal.*?(?:failed|not found)|"
                r"err:ole:.*?apartment threading"
            ),
            "warn",
            "low",
            [
                {
                    "field": "wine_settings.com",
                    "value": "builtin",
                    "confidence": 0.50,
                    "reason": "COM marshalling issues; use built-in COM implementation",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Info: Wine debug channel spew ———
        self._register(
            "wine_debug_spew",
            re.compile(
                r"trace:.*?:.*?enter|trace:.*?:.*?leave"
            ),
            "trace",
            "info",
            [
                {
                    "field": "env.WINEDEBUG",
                    "value": "-all",
                    "confidence": 0.40,
                    "reason": "Excessive Wine debug trace; disable to reduce log noise",
                    "auto_apply": True,
                },
            ],
        )

        # ——— Info: MacRunner HyperBridge import thunk trace (not error) ———
        self._register(
            "hb_import_thunk_trace",
            re.compile(
                r"trace:module:macrunner_hb_call_import_thunk MacRunner HyperBridge native import call"
            ),
            "trace",
            "info",
            [
                {
                    "field": "env.MACRUNNER_HB_TRACE_IMPORTS",
                    "value": "0",
                    "confidence": 0.35,
                    "reason": "Import thunk tracing is informational only; disable to reduce noise",
                    "auto_apply": True,
                },
            ],
        )

    def _register(
        self,
        pattern_id: str,
        regex: re.Pattern[str],
        category: str,
        severity: str,
        suggestions: Sequence[dict[str, Any]],
    ) -> None:
        self._patterns.append((pattern_id, regex, category, severity, suggestions))

    def match(self, line: str) -> tuple[WineError, list[Suggestion]] | None:
        """Try to match a single log line against all patterns.

        Returns (WineError, [Suggestion, ...]) on first match, else None.
        """
        for pid, regex, category, severity, sugg_defs in self._patterns:
            m = regex.search(line)
            if m:
                err = WineError(
                    category=category,
                    subcategory=pid,
                    message=line.strip(),
                    matched_pattern_id=pid,
                    severity=severity,
                )
                suggestions: list[Suggestion] = []
                for sd in sugg_defs:
                    suggestions.append(
                        Suggestion(
                            field=sd["field"],
                            value=sd["value"],
                            confidence=float(sd["confidence"]),
                            reason=sd["reason"],
                            auto_apply=bool(sd.get("auto_apply", False)),
                            requires_confirmation=not bool(sd.get("auto_apply", False)),
                            source_pattern_id=pid,
                        )
                    )
                return err, suggestions
        return None

    def match_all(self, lines: Sequence[str]) -> list[tuple[WineError, list[Suggestion]]]:
        """Match every line, return all hits."""
        results: list[tuple[WineError, list[Suggestion]]] = []
        for line in lines:
            hit = self.match(line)
            if hit:
                results.append(hit)
        return results


class LogParser:
    """Ingest Wine stderr logs and produce ParsedLog."""

    def __init__(self, pattern_db: PatternDB | None = None) -> None:
        self.db = pattern_db or PatternDB()

    def parse_lines(self, lines: Sequence[str]) -> ParsedLog:
        """Parse a sequence of log lines."""
        parsed = ParsedLog()
        seen_patterns: set[str] = set()

        for line in lines:
            line = line.rstrip("\n")
            if not line:
                continue
            hit = self.db.match(line)
            if hit:
                err, suggestions = hit
                # De-duplicate by pattern_id to avoid spam from repeated lines
                key = err.matched_pattern_id
                if key in seen_patterns:
                    continue
                seen_patterns.add(key)
                parsed.errors.append(err)
                parsed.suggestions.extend(suggestions)

        # Build summary
        parsed.summary = {
            "total_lines": len(lines),
            "errors_found": len(parsed.errors),
            "suggestions": len(parsed.suggestions),
            "by_severity": {},
        }
        for err in parsed.errors:
            parsed.summary["by_severity"][err.severity] = (
                parsed.summary["by_severity"].get(err.severity, 0) + 1
            )

        return parsed

    def parse_text(self, text: str) -> ParsedLog:
        return self.parse_lines(text.splitlines())

    def parse_path(self, path: Path | str) -> ParsedLog:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        return self.parse_text(text)


# ---------------------------------------------------------------------------
# Suggestion engine: conflict resolution, scoring, materialization
# ---------------------------------------------------------------------------

@dataclass
class ResolvedSuggestions:
    """Final resolved suggestions after conflict resolution and scoring."""

    suggestions: list[Suggestion]
    conflicts: list[tuple[str, list[Suggestion]]]
    auto_apply: list[Suggestion]
    manual: list[Suggestion]


def resolve_suggestions(
    suggestions: Sequence[Suggestion],
    min_confidence: float = 0.50,
) -> ResolvedSuggestions:
    """Resolve conflicts and partition into auto_apply vs manual.

    Rules:
    - Drop suggestions below min_confidence.
    - Group by field; if multiple suggestions target same field,
      keep only the highest-confidence one (report conflict).
    - Partition: auto_apply (confidence ≥ 0.85 AND field is low-risk)
      vs manual (everything else).
    """
    HIGH_RISK_FIELDS = {
        "graphics.api",
        "graphics.preferred_backend",
        "graphics.fallback_backend",
        "anti_cheat.mode",
        "anti_cheat.online_supported",
        "default_lane",
        "architecture",
    }

    filtered = [s for s in suggestions if s.confidence >= min_confidence]

    # Group by field
    by_field: dict[str, list[Suggestion]] = {}
    for s in filtered:
        by_field.setdefault(s.field, []).append(s)

    resolved: list[Suggestion] = []
    conflicts: list[tuple[str, list[Suggestion]]] = []

    for field, group in by_field.items():
        if len(group) > 1:
            conflicts.append((field, group))
        best = max(group, key=lambda s: s.confidence)
        resolved.append(best)

    auto_apply = [
        s
        for s in resolved
        if s.auto_apply
        and s.confidence >= 0.85
        and s.field not in HIGH_RISK_FIELDS
    ]
    manual = [s for s in resolved if s not in auto_apply]

    return ResolvedSuggestions(
        suggestions=resolved,
        conflicts=conflicts,
        auto_apply=auto_apply,
        manual=manual,
    )


def apply_suggestions_to_profile(
    profile: Mapping[str, Any], suggestions: Sequence[Suggestion]
) -> dict[str, Any]:
    """Apply suggestions to a profile dict (deep-merge style).

    Returns a new dict; original is untouched.
    """
    out = copy.deepcopy(dict(profile))
    for s in suggestions:
        parts = s.field.split(".")
        node = out
        for part in parts[:-1]:
            if part not in node:
                node[part] = {}
            node = node[part]
        node[parts[-1]] = s.value
    return out


# ---------------------------------------------------------------------------
# Smoke-test integration helper
# ---------------------------------------------------------------------------


def analyze_smoke_stderr(
    stderr_text: str, pattern_db: PatternDB | None = None
) -> dict[str, Any]:
    """Analyze smoke-test stderr and return structured report.

    Returns a dict compatible with SmokeReport format.
    """
    parser = LogParser(pattern_db)
    parsed = parser.parse_text(stderr_text)
    resolved = resolve_suggestions(parsed.suggestions)

    return {
        "errors_found": [e.matched_pattern_id for e in parsed.errors],
        "error_summary": parsed.summary,
        "suggestions_total": len(parsed.suggestions),
        "suggestions_resolved": len(resolved.suggestions),
        "auto_apply": [
            {"field": s.field, "value": s.value, "reason": s.reason}
            for s in resolved.auto_apply
        ],
        "manual": [
            {"field": s.field, "value": s.value, "reason": s.reason, "confidence": s.confidence}
            for s in resolved.manual
        ],
        "conflicts": [
            {
                "field": field,
                "candidates": [
                    {"value": s.value, "confidence": s.confidence, "reason": s.reason}
                    for s in group
                ],
            }
            for field, group in resolved.conflicts
        ],
    }
