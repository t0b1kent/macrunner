#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

DXVK_SRC="$PROJECT_ROOT/engine/dxvk"
require_file "$DXVK_SRC/meson.build"
ensure_build_tools
ensure_llvm_mingw
require_cmd glslangValidator

mkdir -p "$GRAPHICS_BUILD/cross" "$GRAPHICS_DIST/dxvk" "$GRAPHICS_DIST/manifest"

echo "MacRunner graphics: DXVK Windows DLL build"
echo "source: $DXVK_SRC"

ensure_dxvk_arm64_patch() {
  local patch="$GRAPHICS_ROOT/vendor-patches/dxvk/0001-macr-arm64-windows-portability.patch"
  require_file "$patch"
  if ! grep -q "lib-arm64" "$DXVK_SRC/meson.build"; then
    git -C "$DXVK_SRC" apply "$patch"
  fi
}

prepare_arm64_import_libs() {
  local dst="$DXVK_SRC/lib-arm64"
  mkdir -p "$dst"
  if [[ -f "$DXVK_SRC/lib/d3dcompiler_43.lib" ]]; then
    cp -f "$DXVK_SRC/lib/d3dcompiler_43.lib" "$dst/d3dcompiler_43.lib"
  fi
  if [[ -f "$DXVK_SRC/lib/libd3dcompiler_43.def" ]]; then
    cp -f "$DXVK_SRC/lib/libd3dcompiler_43.def" "$dst/libd3dcompiler_43.def"
  fi
  cat > "$dst/vulkan-1.def" <<'DEF'
LIBRARY vulkan-1.dll
EXPORTS
vkGetInstanceProcAddr
DEF
  "$TOOLCHAIN_BIN/aarch64-w64-mingw32-dlltool" -d "$dst/vulkan-1.def" -l "$dst/vulkan-1.lib" -D vulkan-1.dll
}

build_arch() {
  local arch="$1"
  local cross="$GRAPHICS_BUILD/cross/$arch-windows-clang.meson"
  local build="$GRAPHICS_BUILD/dxvk-$arch"
  local install="$GRAPHICS_BUILD/dxvk-install-$arch"
  local out="$GRAPHICS_DIST/dxvk/$arch-windows"

  write_windows_cross_file "$arch" "$cross" no
  rm -rf "$build" "$install"
  "${MESON[@]}" setup "$build" "$DXVK_SRC" \
    --cross-file "$cross" \
    --buildtype release \
    -Denable_tests=false \
    -Denable_d3d10=false \
    --prefix "$install" --bindir . --libdir .
  ninja -C "$build" -j"$JOBS" install
  copy_dlls "$install" "$out" d3d9.dll d3d11.dll dxgi.dll
}

ensure_dxvk_arm64_patch
prepare_arm64_import_libs
build_arch aarch64
build_arch x86_64
write_manifest "$GRAPHICS_DIST/manifest/dxvk.json" dxvk PASS "built DXVK d3d9/d3d11/dxgi for aarch64-windows and x86_64-windows; d3d10 deferred"

echo "DXVK PASS: $GRAPHICS_DIST/dxvk"
