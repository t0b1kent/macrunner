#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: install-graphics.sh <wine-prefix> <dxmt|dxvk|vkd3d|auto|wined3d>" >&2
  exit 2
fi

PREFIX="$1"
BACKEND="$2"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SYSTEM32="$PREFIX/drive_c/windows/system32"
SYSWOW64="$PREFIX/drive_c/windows/syswow64"
mkdir -p "$SYSTEM32" "$SYSWOW64"

write_env() {
  local backend="$1"
  local overrides="$2"
  cat > "$PREFIX/macr-graphics.env" <<ENV
MACRUNNER_GRAPHICS_BACKEND=$backend
WINEDLLOVERRIDES=$overrides
ENV
  printf '%s\n' "$backend" > "$PREFIX/macr-graphics-backend"
}

copy_backend() {
  local src="$1"
  shift
  local dll
  for dll in "$@"; do
    if [[ ! -f "$src/$dll" ]]; then
      echo "missing backend dll: $src/$dll" >&2
      exit 1
    fi
    cp -f "$src/$dll" "$SYSTEM32/$dll"
    file "$SYSTEM32/$dll"
  done
}

case "$BACKEND" in
  auto|dxmt)
    copy_backend "$DIST_ROOT/dxmt/aarch64-windows" d3d10core.dll d3d11.dll dxgi.dll winemetal.dll
    write_env dxmt "d3d10core,d3d11,dxgi,winemetal=n,b"
    ;;
  dxvk)
    copy_backend "$DIST_ROOT/dxvk/aarch64-windows" d3d9.dll d3d11.dll dxgi.dll
    write_env dxvk "d3d9,d3d11,dxgi=n,b"
    ;;
  vkd3d)
    copy_backend "$DIST_ROOT/vkd3d/aarch64-windows" d3d12.dll d3d12core.dll
    write_env vkd3d "d3d12,d3d12core=n,b"
    ;;
  wined3d)
    write_env wined3d ""
    ;;
  *)
    echo "unknown backend: $BACKEND" >&2
    exit 2
    ;;
esac

echo "graphics backend installed: $(cat "$PREFIX/macr-graphics-backend")"
