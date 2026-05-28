#!/usr/bin/env python3
"""PE/COFF parsing helpers for MacRunner."""

from __future__ import annotations

import struct
from enum import Enum
from pathlib import Path


IMAGE_FILE_MACHINE_I386 = 0x014C
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_ARM64 = 0xAA64
IMAGE_FILE_MACHINE_ARM64EC = 0xA641
IMAGE_FILE_MACHINE_ARM64X = 0xA64E


class PEMachine(str, Enum):
    X86 = "x86"
    X86_64 = "x86_64"
    ARM64 = "arm64"
    ARM64EC = "arm64ec"
    ARM64X = "arm64x"
    UNKNOWN = "unknown"


class PEFormatError(Exception):
    """Raised when a file is not a valid PE image."""


_MACHINE_MAP = {
    IMAGE_FILE_MACHINE_I386: PEMachine.X86,
    IMAGE_FILE_MACHINE_AMD64: PEMachine.X86_64,
    IMAGE_FILE_MACHINE_ARM64: PEMachine.ARM64,
    IMAGE_FILE_MACHINE_ARM64EC: PEMachine.ARM64EC,
    IMAGE_FILE_MACHINE_ARM64X: PEMachine.ARM64X,
}


def _read_u32(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 4 > len(data):
        raise PEFormatError("PE header truncated")
    return struct.unpack_from("<I", data, offset)[0]


def _read_u16(data: bytes, offset: int) -> int:
    if offset < 0 or offset + 2 > len(data):
        raise PEFormatError("PE header truncated")
    return struct.unpack_from("<H", data, offset)[0]


def detect_pe_machine(path: str | Path) -> PEMachine:
    """Detect the PE machine type from a file path."""

    file_path = Path(path)
    try:
        data = file_path.read_bytes()
    except FileNotFoundError:
        raise
    except OSError as exc:
        raise PEFormatError(f"{file_path}: unable to read file: {exc}") from exc

    if len(data) < 0x40:
        raise PEFormatError(f"{file_path}: file too small for PE header")
    if data[:2] != b"MZ":
        raise PEFormatError(f"{file_path}: missing MZ signature")

    e_lfanew = _read_u32(data, 0x3C)
    pe_sig_off = e_lfanew
    coff_off = pe_sig_off + 4
    if pe_sig_off < 0 or coff_off + 20 > len(data):
        raise PEFormatError(f"{file_path}: PE header outside file")
    if data[pe_sig_off:pe_sig_off + 4] != b"PE\x00\x00":
        raise PEFormatError(f"{file_path}: missing PE signature")

    machine = _read_u16(data, coff_off)
    return _MACHINE_MAP.get(machine, PEMachine.UNKNOWN)

