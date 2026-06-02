#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

ARCH="${1:-aarch64}"
BUILD_DIR="$GRAPHICS_BUILD/dxmt-${ARCH}-tests"
CROSS_FILE="$GRAPHICS_BUILD/cross/dxmt-${ARCH}-tests.ini"
NATIVE_LLVM_PATH="${NATIVE_LLVM_PATH:-/opt/homebrew/opt/llvm@15}"
WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-false}"

ensure_build_tools
ensure_llvm_mingw
write_windows_cross_file "$ARCH" "$CROSS_FILE"

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
  "${MESON[@]}" setup "$BUILD_DIR" "$PROJECT_ROOT/engine/dxmt" \
    --cross-file "$CROSS_FILE" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$PROJECT_ROOT/engine/wine/dist"
else
  "${MESON[@]}" configure "$BUILD_DIR" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$PROJECT_ROOT/engine/wine/dist"
fi

ninja -C "$BUILD_DIR" tests/dx11/dx11_headless_smoke.exe src/d3d11/d3d11.dll src/dxgi/dxgi.dll src/winemetal/winemetal.dll

echo "built=$BUILD_DIR/tests/dx11/dx11_headless_smoke.exe"
echo "dlls=$BUILD_DIR/src/d3d11/d3d11.dll,$BUILD_DIR/src/dxgi/dxgi.dll,$BUILD_DIR/src/winemetal/winemetal.dll"
echo "run with a scoped Wine prefix and DLL overrides: d3d11,dxgi,winemetal=n,b"
