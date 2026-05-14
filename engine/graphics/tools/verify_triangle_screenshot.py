#!/usr/bin/env python3
from __future__ import annotations
import sys
from pathlib import Path

def read_ppm(path: Path):
    data = path.read_bytes()
    if not data.startswith(b'P6\n'):
        raise SystemExit(f'{path}: not a raw P6 PPM')
    offset = 3
    tokens = []
    while len(tokens) < 3:
        while data[offset:offset+1].isspace():
            offset += 1
        if data[offset:offset+1] == b'#':
            offset = data.index(b'\n', offset) + 1
            continue
        end = offset
        while not data[end:end+1].isspace():
            end += 1
        tokens.append(data[offset:end])
        offset = end
    while data[offset:offset+1].isspace():
        offset += 1
    width, height, maxval = map(int, tokens)
    pixels = data[offset:]
    if maxval != 255 or len(pixels) != width * height * 3:
        raise SystemExit(f'{path}: invalid PPM payload')
    return width, height, pixels

def main() -> int:
    ppm = Path(sys.argv[1])
    png = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    width, height, pixels = read_ppm(ppm)
    unique = {pixels[i:i+3] for i in range(0, len(pixels), 3)}
    print(f'ppm: {ppm}')
    print(f'size: {width}x{height}')
    print(f'unique_pixels: {len(unique)}')
    if width < 32 or height < 32 or len(unique) < 8:
        return 1
    if png:
        if not png.exists():
            print(f'missing png: {png}')
            return 1
        print(f'png: {png}')
        print(f'png_bytes: {png.stat().st_size}')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
