#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=".build/release"
OUT="dist/MacRunnerControlCenter"
echo "Packaging MacRunner Control Center..."
rm -rf "$OUT"
mkdir -p "$OUT"
cp -R "$BUILD/MacRunnerControlCenter" "$OUT/" 2>/dev/null || true
cp README.md "$OUT/" 2>/dev/null || true
cp -R docs "$OUT/" 2>/dev/null || true
echo "Package created at: $(pwd)/$OUT"
