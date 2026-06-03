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

prepare_glslang_tool() {
  local source_bin tool_dir tool_bin glslang_lib default_limits_lib
  source_bin="${GLSLANG_BIN:-}"
  if [[ -z "$source_bin" ]]; then
    source_bin="$(command -v glslang || command -v glslangValidator || true)"
  fi
  require_file "$source_bin"

  tool_dir="$GRAPHICS_BUILD/tools/glslang"
  tool_bin="$tool_dir/glslang"
  glslang_lib="/opt/homebrew/opt/glslang/lib/libglslang.16.dylib"
  default_limits_lib="/opt/homebrew/opt/glslang/lib/libglslang-default-resource-limits.16.dylib"
  require_file "$glslang_lib"
  require_file "$default_limits_lib"

  mkdir -p "$tool_dir"
  cp -f "$source_bin" "$tool_bin"
  chmod +w "$tool_bin"
  install_name_tool -change @rpath/libglslang.16.dylib "$glslang_lib" "$tool_bin" || true
  install_name_tool -change @rpath/libglslang-default-resource-limits.16.dylib "$default_limits_lib" "$tool_bin" || true
  codesign -s - -f "$tool_bin" >/dev/null 2>&1 || true

  "$tool_bin" --version >/dev/null
  export PATH="$tool_dir:$PATH"
}

prepare_glslang_tool

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
