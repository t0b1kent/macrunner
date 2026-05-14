#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
TEST_DIR="$DIST_ROOT/tests"
SHOT_DIR="$DIST_ROOT/screenshots"
TRACE_PPM="$PROJECT_ROOT/engine/graphics/artifacts/runtime-samples/d3d11_triangle_runtime/d3d11_triangle_runtime.ppm"
SHOT_PNG="$SHOT_DIR/native-shader-validation.png"
mkdir -p "$TEST_DIR" "$SHOT_DIR"

cp -f "$PROJECT_ROOT/tests/native-fixtures/build/d3d11_triangle_arm64.exe" "$TEST_DIR/hello-triangle-d3d11-arm64.exe"
cp -f "$PROJECT_ROOT/tests/native-fixtures/build/d3d11_clear_arm64.exe" "$TEST_DIR/clear-d3d11-arm64.exe"
cp -f "$PROJECT_ROOT/tests/native-fixtures/build/d3d12_triangle_arm64.exe" "$TEST_DIR/hello-triangle-d3d12-arm64.exe"
file "$TEST_DIR"/*.exe

if [[ ! -f "$TRACE_PPM" ]]; then
  echo "missing trace screenshot source: $TRACE_PPM" >&2
  exit 1
fi
sips -s format png "$TRACE_PPM" --out "$SHOT_PNG" >/dev/null
python3 "$PROJECT_ROOT/engine/graphics/tools/verify_triangle_screenshot.py" "$TRACE_PPM" "$SHOT_PNG"

echo "native shader validation screenshot: $SHOT_PNG"
echo "wine dxmt e2e screenshot: WAITING on Phase G runtime fix -> $SHOT_DIR/hello-triangle-d3d11-via-wine.png"

if [[ "${MACRUNNER_RUN_WINE:-0}" == "1" ]]; then
  prefix="${WINEPREFIX:-/tmp/macr-graphics-run-prefix}"
  rm -rf "$prefix"
  mkdir -p "$prefix"
  WINEPREFIX="$prefix" "$PROJECT_ROOT/engine/wine/dist/bin/wineboot" --init
  "$DIST_ROOT/bin/install-graphics.sh" "$prefix" dxmt
  WINEPREFIX="$prefix" WINEDLLOVERRIDES='d3d10core,d3d11,dxgi,winemetal=n,b' \
    "$PROJECT_ROOT/engine/wine/dist/bin/wine" "$TEST_DIR/hello-triangle-d3d11-arm64.exe"
fi
