#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
source "$PROJECT_ROOT/config/env.sh"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

ARCH="${1:-x86_64}"
BUILD_DIR="$GRAPHICS_BUILD/dxmt-${ARCH}-hk-present"
CROSS_FILE="$GRAPHICS_BUILD/cross/dxmt-${ARCH}-hk-present.ini"
NATIVE_LLVM_PATH="${NATIVE_LLVM_PATH:-/opt/homebrew/opt/llvm@15}"
DXMT_SMOKE_BIND_MODE="${DXMT_SMOKE_BIND_MODE:-native}"
WINE_DIST="$PROJECT_ROOT/engine/wine/dist"
WINE="$WINE_DIST/bin/wine"
WINESERVER="$WINE_DIST/bin/wineserver"
PREFIX="${DXMT_HK_PRESENT_PREFIX:-$PROJECT_ROOT/artifacts/dxmt-hk-present-prefix}"
PREFIX_SYSTEM32="$PREFIX/drive_c/windows/system32"
APP_DIR="$PREFIX/drive_c/dxmt-hk-present"
OVERLAY_DIR="$PREFIX/dxmt-builtin-overlay"
LOG_DIR="$PROJECT_ROOT/artifacts/dxmt-hk-present-logs"
LOG="$LOG_DIR/hk-zero-latency-present-${ARCH}.log"
TIMEOUT_SECONDS="${HK_PRESENT_TIMEOUT_SECONDS:-90}"

case "$ARCH" in
  aarch64|arm64) MACHINE_DIR="aarch64-windows"; UNIX_DIR="aarch64-unix" ;;
  x86_64|amd64) MACHINE_DIR="x86_64-windows"; UNIX_DIR="x86_64-unix" ;;
  *) echo "unsupported HK present arch: $ARCH" >&2; exit 21 ;;
esac

case "$DXMT_SMOKE_BIND_MODE" in
  native)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-false}"
    DXMT_DLL_OVERRIDES="${DXMT_DLL_OVERRIDES:-d3d11,dxgi,winemetal=n}"
    ;;
  mixed)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-false}"
    DXMT_DLL_OVERRIDES="${DXMT_DLL_OVERRIDES:-d3d11,dxgi=n;winemetal=b,n}"
    ;;
  builtin)
    WINE_BUILTIN_DLL="${WINE_BUILTIN_DLL:-true}"
    DXMT_DLL_OVERRIDES="${DXMT_DLL_OVERRIDES:-d3d11,dxgi,winemetal=b,n}"
    ;;
  *)
    echo "unsupported DXMT_SMOKE_BIND_MODE: $DXMT_SMOKE_BIND_MODE" >&2
    exit 24
    ;;
esac

OVERLAY_MACHINE_DIR="$OVERLAY_DIR/$MACHINE_DIR"
OVERLAY_UNIX_DIR="$OVERLAY_DIR/$UNIX_DIR"
WINE_MACHINE_DIR="$WINE_DIST/lib/wine/$MACHINE_DIR"
WINE_UNIX_DIR="$WINE_DIST/lib/wine/$UNIX_DIR"

ensure_build_tools
ensure_llvm_mingw
write_windows_cross_file "$ARCH" "$CROSS_FILE"

if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
  "${MESON[@]}" setup "$BUILD_DIR" "$PROJECT_ROOT/engine/dxmt" \
    --cross-file "$CROSS_FILE" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$WINE_DIST"
else
  "${MESON[@]}" configure "$BUILD_DIR" \
    -Denable_tests=true \
    -Dwine_builtin_dll="$WINE_BUILTIN_DLL" \
    -Dnative_llvm_path="$NATIVE_LLVM_PATH" \
    -Dwine_install_path="$WINE_DIST"
fi

ninja -C "$BUILD_DIR" tests/dx11/dx11_hk_present_probe.exe src/winemetal/unix/winemetal.so
if [[ "$WINE_BUILTIN_DLL" == "true" ]]; then
  ninja -C "$BUILD_DIR" src/d3d11/d3d11.dll.postproc src/dxgi/dxgi.dll.postproc src/winemetal/winemetal.dll.postproc
else
  rm -f "$BUILD_DIR/src/d3d11/d3d11.dll" "$BUILD_DIR/src/dxgi/dxgi.dll" "$BUILD_DIR/src/winemetal/winemetal.dll"
  ninja -C "$BUILD_DIR" src/d3d11/d3d11.dll src/dxgi/dxgi.dll src/winemetal/winemetal.dll
fi

EXE="$BUILD_DIR/tests/dx11/dx11_hk_present_probe.exe"
D3D11_DLL="$BUILD_DIR/src/d3d11/d3d11.dll"
DXGI_DLL="$BUILD_DIR/src/dxgi/dxgi.dll"
WINEMETAL_DLL="$BUILD_DIR/src/winemetal/winemetal.dll"
WINEMETAL_SO="$BUILD_DIR/src/winemetal/unix/winemetal.so"

require_file "$WINE"
require_file "$WINESERVER"
require_file "$EXE"
require_file "$D3D11_DLL"
require_file "$DXGI_DLL"
require_file "$WINEMETAL_DLL"
require_file "$WINEMETAL_SO"
require_file "$WINE_MACHINE_DIR/kernel32.dll"
require_file "$WINE_UNIX_DIR/ntdll.so"

mkdir -p "$PREFIX" "$PREFIX_SYSTEM32" "$APP_DIR" "$OVERLAY_MACHINE_DIR" "$OVERLAY_UNIX_DIR" "$LOG_DIR"
WINEPREFIX="$PREFIX" "$WINESERVER" -k >/dev/null 2>&1 || true

find "$OVERLAY_MACHINE_DIR" -mindepth 1 -maxdepth 1 -exec rm -f {} +
find "$OVERLAY_UNIX_DIR" -mindepth 1 -maxdepth 1 -exec rm -f {} +
find "$WINE_MACHINE_DIR" -mindepth 1 -maxdepth 1 -exec sh -c 'ln -s "$1" "$2/$(basename "$1")"' sh {} "$OVERLAY_MACHINE_DIR" \;
find "$WINE_UNIX_DIR" -mindepth 1 -maxdepth 1 -exec sh -c 'ln -s "$1" "$2/$(basename "$1")"' sh {} "$OVERLAY_UNIX_DIR" \;
rm -f "$OVERLAY_MACHINE_DIR/d3d11.dll" "$OVERLAY_MACHINE_DIR/dxgi.dll" "$OVERLAY_MACHINE_DIR/winemetal.dll" "$OVERLAY_UNIX_DIR/winemetal.so"

cp -f "$EXE" "$APP_DIR/dx11_hk_present_probe.exe"
cp -f "$D3D11_DLL" "$APP_DIR/d3d11.dll"
cp -f "$DXGI_DLL" "$APP_DIR/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$APP_DIR/winemetal.dll"
cp -f "$WINEMETAL_SO" "$APP_DIR/winemetal.so"
cp -f "$WINEMETAL_SO" "$APP_DIR/winemetal.dll.so"
cp -f "$D3D11_DLL" "$PREFIX_SYSTEM32/d3d11.dll"
cp -f "$DXGI_DLL" "$PREFIX_SYSTEM32/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$PREFIX_SYSTEM32/winemetal.dll"
cp -f "$WINEMETAL_SO" "$PREFIX_SYSTEM32/winemetal.so"
cp -f "$WINEMETAL_SO" "$PREFIX_SYSTEM32/winemetal.dll.so"
cp -f "$D3D11_DLL" "$OVERLAY_MACHINE_DIR/d3d11.dll"
cp -f "$DXGI_DLL" "$OVERLAY_MACHINE_DIR/dxgi.dll"
cp -f "$WINEMETAL_DLL" "$OVERLAY_MACHINE_DIR/winemetal.dll"
cp -f "$WINEMETAL_SO" "$OVERLAY_UNIX_DIR/winemetal.so"
cp -f "$WINEMETAL_SO" "$OVERLAY_MACHINE_DIR/winemetal.so"
cp -f "$WINEMETAL_SO" "$OVERLAY_MACHINE_DIR/winemetal.dll.so"

echo "probe=$EXE"
echo "prefix=$PREFIX"
echo "app_dir=$APP_DIR"
echo "log=$LOG"
echo "arch=$ARCH"
echo "bind_mode=$DXMT_SMOKE_BIND_MODE"
echo "overrides=$DXMT_DLL_OVERRIDES"

set +e
(
  env -i \
    HOME="$HOME" \
    USER="${USER:-}" \
    LOGNAME="${LOGNAME:-${USER:-}}" \
    PATH="$PATH" \
    TMPDIR="${TMPDIR:-/tmp}" \
    MACRUNNER_DXMT_ROOT="$OVERLAY_DIR" \
    WINEDLLOVERRIDES="$DXMT_DLL_OVERRIDES" \
    WINEDLLDIR0="$OVERLAY_DIR" \
    WINEDLLPATH="$OVERLAY_MACHINE_DIR:$OVERLAY_UNIX_DIR:$WINE_MACHINE_DIR:$WINE_UNIX_DIR" \
    WINESYSTEMDLLPATH="$OVERLAY_MACHINE_DIR" \
    WINEDEBUG="${WINEDEBUG_SMOKE:--all,+loaddll}" \
    DXMT_HK_PRESENT_VISIBLE_WINDOW="${DXMT_HK_PRESENT_VISIBLE_WINDOW:-0}" \
    "$PROJECT_ROOT/scripts/mr-run.sh" "$WINE_DIST" "$APP_DIR/dx11_hk_present_probe.exe" "$TIMEOUT_SECONDS"
) >"$LOG" 2>&1
rc=$?
set -e

grep -E "HKPresentProbe|D3D11CreateDevice|CreateSwapChainForHwnd|Present|Readback|pixel0_bgra|pixel_readback|result=|err:module|not found|failed|FAIL" "$LOG" | tail -240 || true
WINEPREFIX="$PREFIX" "$WINESERVER" -k >/dev/null 2>&1 || true

echo "exit_code=$rc"
exit "$rc"
