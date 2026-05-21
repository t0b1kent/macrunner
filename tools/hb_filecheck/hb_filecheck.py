#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
from pathlib import Path


def load(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    raise SystemExit(1)


def check_order(text: str, expr: str) -> None:
    parts = [p.strip() for p in expr.split(">>") if p.strip()]
    pos = -1
    for p in parts:
        nxt = text.find(p, pos + 1)
        if nxt < 0:
            fail(f"CHECK-ORDER missing part: {p}")
        pos = nxt


def run(input_text: str, check_text: str) -> None:
    for raw in check_text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("CHECK:"):
            pat = line[len("CHECK:"):].strip()
            if pat not in input_text:
                fail(f"CHECK missing: {pat}")
        elif line.startswith("CHECK-NOT:"):
            pat = line[len("CHECK-NOT:"):].strip()
            if pat in input_text:
                fail(f"CHECK-NOT violated: {pat}")
        elif line.startswith("CHECK-ORDER:"):
            expr = line[len("CHECK-ORDER:"):].strip()
            check_order(input_text, expr)
        elif line.startswith("CHECK-COUNT:"):
            body = line[len("CHECK-COUNT:"):].strip()
            if "::" not in body:
                fail("CHECK-COUNT requires '<N> :: <substring>'")
            n_str, pat = [x.strip() for x in body.split("::", 1)]
            n = int(n_str)
            c = input_text.count(pat)
            if c != n:
                fail(f"CHECK-COUNT mismatch for '{pat}': expected {n}, got {c}")
        elif line.startswith("CHECK-REGEX:"):
            pat = line[len("CHECK-REGEX:"):].strip()
            if not re.search(pat, input_text, flags=re.MULTILINE):
                fail(f"CHECK-REGEX no match: {pat}")
        else:
            fail(f"Unknown directive: {line}")
    print("PASS")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--check", required=True)
    args = ap.parse_args()
    run(load(Path(args.input)), load(Path(args.check)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
