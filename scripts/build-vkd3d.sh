#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

if [[ -d "$PROJECT_ROOT/engine/vkd3d-proton" ]]; then
  VKD3D_SRC="$PROJECT_ROOT/engine/vkd3d-proton"
else
  VKD3D_SRC="$PROJECT_ROOT/engine/vkd3d"
fi

require_file "$VKD3D_SRC/meson.build"
ensure_build_tools
ensure_llvm_mingw

mkdir -p "$GRAPHICS_BUILD/cross" "$GRAPHICS_DIST/vkd3d" "$GRAPHICS_DIST/manifest"

echo "MacRunner graphics: vkd3d-proton D3D12 build"
echo "source: $VKD3D_SRC"

git -C "$VKD3D_SRC" submodule update --init --recursive

build_arch() {
  local arch="$1"
  local cross="$GRAPHICS_BUILD/cross/$arch-vkd3d.meson"
  local build="$GRAPHICS_BUILD/vkd3d-$arch"
  local install="$GRAPHICS_BUILD/vkd3d-install-$arch"
  local out="$GRAPHICS_DIST/vkd3d/$arch-windows"
  write_windows_cross_file "$arch" "$cross" yes
  rm -rf "$build" "$install"
  "${MESON[@]}" setup "$build" "$VKD3D_SRC" \
    --cross-file "$cross" \
    --buildtype release \
    -Denable_tests=false \
    -Denable_extras=false \
    --prefix "$install" --bindir . --libdir .
  ninja -C "$build" -j"$JOBS" install
  copy_dlls "$install" "$out" d3d12.dll d3d12core.dll
}

build_arch aarch64
build_arch x86_64
write_manifest "$GRAPHICS_DIST/manifest/vkd3d.json" vkd3d PASS "built vkd3d-proton d3d12/d3d12core for aarch64-windows and x86_64-windows"

echo "vkd3d PASS: $GRAPHICS_DIST/vkd3d"
