#!/usr/bin/env python3
import struct
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: zero_pe_entry.py <pe-dll>")

    path = Path(sys.argv[1])
    image = bytearray(path.read_bytes())
    if len(image) < 0x40 or image[:2] != b"MZ":
        raise SystemExit("not a DOS/PE image")
    pe_offset = struct.unpack_from("<I", image, 0x3C)[0]
    if pe_offset + 24 > len(image) or image[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise SystemExit("invalid PE signature")
    optional_size = struct.unpack_from("<H", image, pe_offset + 20)[0]
    optional_offset = pe_offset + 24
    if optional_size < 20 or optional_offset + optional_size > len(image):
        raise SystemExit("invalid PE optional header")
    if struct.unpack_from("<H", image, optional_offset)[0] not in (0x10B, 0x20B):
        raise SystemExit("unsupported PE optional-header magic")

    struct.pack_into("<I", image, optional_offset + 16, 0)
    path.write_bytes(image)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
