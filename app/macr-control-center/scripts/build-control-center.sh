#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
echo "Building MacRunner Control Center..."
swift build -c release
echo "Build complete."
