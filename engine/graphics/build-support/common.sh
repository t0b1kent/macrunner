#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${PROJECT_ROOT:-}" ]]; then
  PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
fi

GRAPHICS_ROOT="$PROJECT_ROOT/engine/graphics"
GRAPHICS_BUILD="$GRAPHICS_ROOT/build"
GRAPHICS_DIST="$GRAPHICS_ROOT/dist"
TOOLCHAIN_ROOT="$PROJECT_ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal"
TOOLCHAIN_BIN="$TOOLCHAIN_ROOT/bin"
MESON=(python3 -m mesonbuild.mesonmain)
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

require_file() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "missing: $path" >&2
    exit 1
  fi
}

require_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "missing command: $cmd" >&2
    exit 1
  fi
}

require_meson() {
  python3 - <<'PY'
import importlib.util, sys
sys.exit(0 if importlib.util.find_spec('mesonbuild') else 1)
PY
}

ensure_build_tools() {
  require_cmd ninja
  require_meson || {
    echo "missing Python mesonbuild module; install with: python3 -m pip install --user meson" >&2
    exit 1
  }
}

ensure_llvm_mingw() {
  require_file "$TOOLCHAIN_BIN/aarch64-w64-mingw32-clang"
  require_file "$TOOLCHAIN_BIN/x86_64-w64-mingw32-clang"
}

triplet_for_arch() {
  case "$1" in
    aarch64) echo "aarch64-w64-mingw32" ;;
    x86_64) echo "x86_64-w64-mingw32" ;;
    *) echo "unsupported arch: $1" >&2; exit 1 ;;
  esac
}

cpu_family_for_arch() {
  case "$1" in
    aarch64) echo "aarch64" ;;
    x86_64) echo "x86_64" ;;
    *) echo "unsupported arch: $1" >&2; exit 1 ;;
  esac
}

write_windows_cross_file() {
  local arch="$1"
  local out="$2"
  local include_widl_fallback="${3:-no}"
  local triplet cpu_family
  triplet="$(triplet_for_arch "$arch")"
  cpu_family="$(cpu_family_for_arch "$arch")"
  mkdir -p "$(dirname "$out")"
  cat > "$out" <<CROSS
[binaries]
c = '$TOOLCHAIN_BIN/$triplet-clang'
cpp = '$TOOLCHAIN_BIN/$triplet-clang++'
ar = '$TOOLCHAIN_BIN/$triplet-ar'
strip = '$TOOLCHAIN_BIN/$triplet-strip'
windres = '$TOOLCHAIN_BIN/$triplet-windres'
CROSS
  if [[ "$include_widl_fallback" == "yes" ]]; then
    cat >> "$out" <<CROSS
widl-mingw-tools-fallback = '$TOOLCHAIN_BIN/$triplet-widl'
CROSS
  else
    cat >> "$out" <<CROSS
widl = '$TOOLCHAIN_BIN/$triplet-widl'
CROSS
  fi
  cat >> "$out" <<CROSS

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = '$cpu_family'
cpu = '$cpu_family'
endian = 'little'
CROSS
}

copy_dlls() {
  local src="$1"
  local dst="$2"
  shift 2
  mkdir -p "$dst"
  local dll
  for dll in "$@"; do
    require_file "$src/$dll"
    cp -f "$src/$dll" "$dst/$dll"
    file "$dst/$dll"
  done
}

write_manifest() {
  local path="$1"
  local component="$2"
  local status="$3"
  local details="$4"
  mkdir -p "$(dirname "$path")"
  python3 - "$path" "$component" "$status" "$details" <<'PY'
import datetime, json, sys
path, component, status, details = sys.argv[1:]
with open(path, 'w', encoding='utf-8') as f:
    json.dump({
        'component': component,
        'status': status,
        'details': details,
        'generated_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }, f, indent=2, sort_keys=True)
    f.write('\n')
PY
}
