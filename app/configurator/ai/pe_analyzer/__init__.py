"""MacRunner PE Analyzer — deep binary inspection via lief."""

from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import lief


# ---------------------------------------------------------------------------
# PE constants
# ---------------------------------------------------------------------------

IMAGE_SUBSYSTEM_WINDOWS_GUI = 2
IMAGE_SUBSYSTEM_WINDOWS_CUI = 3
IMAGE_SUBSYSTEM_EFI_APPLICATION = 10
IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER = 11
IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER = 12
IMAGE_SUBSYSTEM_EFI_ROM = 13
IMAGE_SUBSYSTEM_XBOX = 14

# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class SectionFeatures:
    name: str
    virtual_size: int
    raw_size: int
    entropy: float
    characteristics: int
    is_executable: bool
    is_writable: bool
    is_readable: bool


@dataclass(frozen=True)
class ImportFeatures:
    dll_name: str
    entries_count: int
    entries: tuple[str, ...] = field(default_factory=tuple)


@dataclass(frozen=True)
class ResourceFeatures:
    has_manifest: bool
    has_version_info: bool
    icon_count: int
    string_table_count: int
    language_id: int | None
    company_name: str | None
    product_name: str | None
    file_version: str | None


@dataclass(frozen=True)
class SignatureFeatures:
    has_signature: bool
    signature_valid: bool
    certificate_count: int
    has_countersignature: bool


@dataclass(frozen=True)
class PEAnalysisResult:
    """Complete PE analysis output."""
    path: str
    sha256: str
    arch: str
    subsystem: str
    image_base: int
    entry_point: int
    timestamp: int
    checksum: int
    number_of_sections: int
    dll_characteristics: int
    sections: tuple[SectionFeatures, ...]
    imports: tuple[ImportFeatures, ...]
    exports_count: int
    has_forwarded_exports: bool
    has_reexports: bool
    avg_export_name_length: float
    resources: ResourceFeatures
    signatures: SignatureFeatures
    has_tls: bool
    has_exception_dir: bool
    has_debug_dir: bool
    has_load_config: bool
    is_pe32_plus: bool

    def to_dict(self) -> dict[str, Any]:
        """Serialize to plain dict for JSON/storage."""
        from dataclasses import asdict
        return asdict(self)


# ---------------------------------------------------------------------------
# Core analyzer
# ---------------------------------------------------------------------------

def _compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def _analyze_sections(binary: lief.PE.Binary) -> tuple[SectionFeatures, ...]:
    secs: list[SectionFeatures] = []
    for sec in binary.sections:
        chars = sec.characteristics
        is_exec = bool(chars & 0x20000000)
        is_write = bool(chars & 0x80000000)
        is_read = bool(chars & 0x40000000)
        secs.append(SectionFeatures(
            name=sec.name,
            virtual_size=sec.virtual_size,
            raw_size=sec.size,
            entropy=sec.entropy,
            characteristics=chars,
            is_executable=is_exec,
            is_writable=is_write,
            is_readable=is_read,
        ))
    return tuple(secs)


def _analyze_imports(binary: lief.PE.Binary) -> tuple[ImportFeatures, ...]:
    imps: list[ImportFeatures] = []
    for imp in binary.imports:
        entries = tuple(e.name for e in imp.entries if e.name)
        imps.append(ImportFeatures(
            dll_name=imp.name,
            entries_count=len(imp.entries),
            entries=entries,
        ))
    return tuple(imps)


def _analyze_exports(binary: lief.PE.Binary) -> tuple[int, bool, bool, float]:
    exp = binary.exported_functions
    count = len(exp)
    has_fwd = any(bool(getattr(e, "forward_information", None)) for e in exp)
    has_re = any(bool(getattr(e, "reexport", None)) for e in exp)
    avg_len = 0.0
    if count:
        lengths = [len(e.name or "") for e in exp]
        avg_len = sum(lengths) / count
    return count, has_fwd, has_re, avg_len


def _analyze_resources(binary: lief.PE.Binary) -> ResourceFeatures:
    has_manifest = False
    has_version = False
    icon_count = 0
    string_count = 0
    lang_id: int | None = None
    company_name: str | None = None
    product_name: str | None = None
    file_version: str | None = None

    if not binary.has_resources:
        return ResourceFeatures(False, False, 0, 0, None, None, None, None)

    resources = binary.resources
    # Manifest detection
    if resources and hasattr(resources, 'types') and resources.types:
        for rtype in resources.types:
            if rtype.id == 24:  # RT_MANIFEST
                has_manifest = True
            if rtype.id == 3:   # RT_ICON
                icon_count += len(rtype.childs) if hasattr(rtype, 'childs') else 0
            if rtype.id == 6:   # RT_STRING
                string_count += len(rtype.childs) if hasattr(rtype, 'childs') else 0
            if rtype.id == 16:  # RT_VERSION
                has_version = True
                # Try to extract version info strings
                for child in (rtype.childs if hasattr(rtype, 'childs') else []):
                    for lang in (child.childs if hasattr(child, 'childs') else []):
                        try:
                            content = lang.content
                            if content:
                                if hasattr(content, 'key') and content.key == "StringFileInfo":
                                    for pair in (content.childs if hasattr(content, 'childs') else []):
                                        for s in (pair.childs if hasattr(pair, 'childs') else []):
                                            if hasattr(s, 'key') and hasattr(s, 'value'):
                                                key = s.key
                                                val = s.value
                                                if key == "CompanyName":
                                                    company_name = val
                                                elif key == "ProductName":
                                                    product_name = val
                                                elif key == "FileVersion":
                                                    file_version = val
                        except Exception:
                            pass

    return ResourceFeatures(
        has_manifest=has_manifest,
        has_version_info=has_version,
        icon_count=icon_count,
        string_table_count=string_count,
        language_id=lang_id,
        company_name=company_name,
        product_name=product_name,
        file_version=file_version,
    )


def _analyze_signatures(binary: lief.PE.Binary) -> SignatureFeatures:
    has_sig = binary.has_signatures
    if not has_sig:
        return SignatureFeatures(False, False, 0, False)
    certs = 0
    valid = False
    has_counter = False
    try:
        for sig in binary.signatures:
            if sig.is_valid:
                valid = True
            certs += len(sig.certificates) if hasattr(sig, 'certificates') else 0
            # countersignature detection is lib-specific; lief may not expose directly
    except Exception:
        pass
    return SignatureFeatures(has_sig, valid, certs, has_counter)


def _get_subsystem_name(subsystem: int | Any) -> str:
    val = int(subsystem)
    mapping = {
        1: "native",
        2: "gui",
        3: "console",
        9: "wince_gui",
        10: "efi_app",
        11: "efi_boot_driver",
        12: "efi_runtime_driver",
        13: "efi_rom",
        14: "xbox",
        16: "windows_boot",
    }
    return mapping.get(val, f"id={val}")


def _get_arch_name(machine: int | Any) -> str:
    val = int(machine)
    mapping = {
        0x014C: "i386",
        0x8664: "x86_64",
        0xAA64: "arm64",
        0xA641: "arm64ec",
        0xA64E: "arm64x",
    }
    return mapping.get(val, f"unknown(0x{val:04X})")


def analyze_pe(path: Path | str) -> PEAnalysisResult:
    """Analyze a PE binary and return structured features."""
    p = Path(path)
    data = p.read_bytes()
    sha = hashlib.sha256(data).hexdigest()

    binary = lief.parse(str(p))
    if binary is None or not isinstance(binary, lief.PE.Binary):
        raise ValueError(f"{p}: not a valid PE binary")

    header = binary.header
    optional = binary.optional_header

    arch = _get_arch_name(header.machine)
    subsystem = _get_subsystem_name(optional.subsystem)

    sections = _analyze_sections(binary)
    imports = _analyze_imports(binary)
    exports_count, has_fwd, has_re, avg_exp_len = _analyze_exports(binary)
    resources = _analyze_resources(binary)
    signatures = _analyze_signatures(binary)

    # Data directories
    has_tls = binary.has_tls
    has_exception = False
    has_debug = False
    has_load_config = False
    try:
        if binary.data_directories:
            dd = list(binary.data_directories)
            if len(dd) > 3:
                has_exception = dd[3].size > 0
            if len(dd) > 6:
                has_debug = dd[6].size > 0
            if len(dd) > 10:
                has_load_config = dd[10].size > 0
    except Exception:
        pass

    return PEAnalysisResult(
        path=str(p),
        sha256=sha,
        arch=arch,
        subsystem=subsystem,
        image_base=optional.imagebase,
        entry_point=optional.addressof_entrypoint,
        timestamp=header.time_date_stamps,
        checksum=optional.checksum,
        number_of_sections=header.numberof_sections,
        dll_characteristics=optional.dll_characteristics,
        sections=sections,
        imports=imports,
        exports_count=exports_count,
        has_forwarded_exports=has_fwd,
        has_reexports=has_re,
        avg_export_name_length=avg_exp_len,
        resources=resources,
        signatures=signatures,
        has_tls=has_tls,
        has_exception_dir=has_exception,
        has_debug_dir=has_debug,
        has_load_config=has_load_config,
        is_pe32_plus=(optional.magic == lief.PE.PE_TYPE.PE32_PLUS),
    )
