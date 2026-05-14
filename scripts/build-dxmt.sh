#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

DXMT_SRC="$PROJECT_ROOT/engine/dxmt"
WINE_DIST="$PROJECT_ROOT/engine/wine/dist"
LLVM15="${LLVM15_ROOT:-/opt/homebrew/opt/llvm@15}"
require_file "$DXMT_SRC/meson.build"
require_file "$WINE_DIST"
require_file "$LLVM15/bin/llvm-config"
ensure_build_tools
ensure_llvm_mingw
require_cmd xcrun
xcrun -sdk macosx --find metal >/dev/null

mkdir -p "$GRAPHICS_BUILD/cross" "$GRAPHICS_DIST/dxmt" "$GRAPHICS_DIST/manifest"

echo "MacRunner graphics: DXMT Metal backend build"
echo "source: $DXMT_SRC"
echo "llvm: $($LLVM15/bin/llvm-config --version)"

ensure_dxmt_homebrew_patch() {
  local patch="$GRAPHICS_ROOT/vendor-patches/dxmt/0001-macr-apple-silicon-llvm15-link.patch"
  require_file "$patch"
  if ! grep -q "/opt/homebrew/opt/zstd/lib/libzstd.a" "$DXMT_SRC/src/airconv/darwin/meson.build"; then
    git -C "$DXMT_SRC" apply "$patch"
  fi
}

git -C "$DXMT_SRC" submodule update --init --recursive
ensure_dxmt_homebrew_patch

build_arm64_full() {
  local arch="aarch64"
  local cross="$GRAPHICS_BUILD/cross/$arch-windows-clang.meson"
  local build="$GRAPHICS_BUILD/dxmt-$arch"
  local install="$GRAPHICS_BUILD/dxmt-install-$arch"
  write_windows_cross_file "$arch" "$cross" no
  rm -rf "$build" "$install"
  "${MESON[@]}" setup "$build" "$DXMT_SRC" \
    --cross-file "$cross" \
    --buildtype release \
    -Dnative_llvm_path="$LLVM15" \
    -Dwine_install_path="$WINE_DIST" \
    --prefix "$install" --bindir . --libdir .
  ninja -C "$build" -j"$JOBS" install
  copy_dlls "$install/aarch64-windows" "$GRAPHICS_DIST/dxmt/aarch64-windows" \
    d3d10core.dll d3d11.dll dxgi.dll winemetal.dll
  mkdir -p "$GRAPHICS_DIST/dxmt/aarch64-unix"
  require_file "$install/aarch64-unix/winemetal.so"
  cp -f "$install/aarch64-unix/winemetal.so" "$GRAPHICS_DIST/dxmt/aarch64-unix/winemetal.so"
  file "$GRAPHICS_DIST/dxmt/aarch64-unix/winemetal.so"
}

build_x86_64_pe_only() {
  local arch="x86_64"
  local cross="$GRAPHICS_BUILD/cross/$arch-windows-clang.meson"
  local build="$GRAPHICS_BUILD/dxmt-$arch"
  write_windows_cross_file "$arch" "$cross" no
  rm -rf "$build"
  "${MESON[@]}" setup "$build" "$DXMT_SRC" \
    --cross-file "$cross" \
    --buildtype release \
    -Dnative_llvm_path="$LLVM15" \
    -Dwine_install_path="$WINE_DIST" \
    --prefix "$GRAPHICS_BUILD/dxmt-install-$arch" --bindir . --libdir .
  ninja -C "$build" -j"$JOBS" \
    src/d3d10/d3d10core.dll \
    src/d3d11/d3d11.dll \
    src/dxgi/dxgi.dll \
    src/winemetal/winemetal.dll
  mkdir -p "$GRAPHICS_DIST/dxmt/x86_64-windows"
  cp -f "$build/src/d3d10/d3d10core.dll" "$GRAPHICS_DIST/dxmt/x86_64-windows/d3d10core.dll"
  cp -f "$build/src/d3d11/d3d11.dll" "$GRAPHICS_DIST/dxmt/x86_64-windows/d3d11.dll"
  cp -f "$build/src/dxgi/dxgi.dll" "$GRAPHICS_DIST/dxmt/x86_64-windows/dxgi.dll"
  cp -f "$build/src/winemetal/winemetal.dll" "$GRAPHICS_DIST/dxmt/x86_64-windows/winemetal.dll"
  file "$GRAPHICS_DIST/dxmt/x86_64-windows/"*.dll
}

build_arm64_full
build_x86_64_pe_only
metal_count="$(strings "$GRAPHICS_DIST/dxmt/aarch64-windows/d3d11.dll" | grep -c Metal || true)"
echo "DXMT d3d11 Metal strings: $metal_count"
if [[ "$metal_count" -lt 5 ]]; then
  echo "DXMT d3d11.dll does not look like a Metal-backed artifact" >&2
  exit 1
fi
write_manifest "$GRAPHICS_DIST/manifest/dxmt.json" dxmt PASS "built DXMT arm64 Windows DLLs + winemetal.so and x86_64 PE DLLs; x86_64 unix link waits for Phase G Wine unix libs"

echo "DXMT PASS: $GRAPHICS_DIST/dxmt"
