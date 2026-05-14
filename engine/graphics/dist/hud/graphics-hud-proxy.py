#!/usr/bin/env python3
import argparse
import json
import re
import sys
from datetime import datetime, timezone

FPS_RE = re.compile(r"(?P<fps>\d+(?:\.\d+)?)\s*fps", re.IGNORECASE)
FRAME_RE = re.compile(r"frame(?:=|\s+)(?P<frame>\d+)", re.IGNORECASE)

def parse_line(line, seq):
    fps = FPS_RE.search(line)
    frame = FRAME_RE.search(line)
    return {
        "seq": seq,
        "ts": datetime.now(timezone.utc).isoformat(),
        "backend": "dxvk-hud",
        "fps": float(fps.group("fps")) if fps else None,
        "frame": int(frame.group("frame")) if frame else seq,
        "raw": line.rstrip("\n"),
    }

def synthetic_lines(count):
    for i in range(count):
        yield f"frame={i} {59.5 + (i % 7) * 0.2:.1f} fps dxvk-hud backend=dxmt\n"

def main():
    ap = argparse.ArgumentParser(description="Convert DXVK/HUD text stream to MacRunner JSONL HUD events")
    ap.add_argument("--input", default="-", help="input log path or - for stdin")
    ap.add_argument("--output", required=True, help="output JSONL path")
    ap.add_argument("--synthetic", type=int, default=0, help="emit deterministic debug HUD events")
    args = ap.parse_args()

    if args.synthetic:
        lines = synthetic_lines(args.synthetic)
    elif args.input == "-":
        lines = sys.stdin
    else:
        lines = open(args.input, "r", encoding="utf-8", errors="replace")

    count = 0
    with open(args.output, "w", encoding="utf-8") as out:
        for count, line in enumerate(lines, 1):
            out.write(json.dumps(parse_line(line, count), sort_keys=True) + "\n")
    print(f"hud events: {count}")
    if count < 20:
        raise SystemExit("hud event gate requires >=20 events")

if __name__ == "__main__":
    main()
