#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$PROJECT_ROOT/engine/graphics/build-support/common.sh"

MVK_SRC="$PROJECT_ROOT/engine/moltenvk"
DIST_LIB="$GRAPHICS_DIST/lib"
mkdir -p "$DIST_LIB" "$GRAPHICS_DIST/manifest"

echo "MacRunner graphics: MoltenVK native arm64 artifact"
echo "source: $MVK_SRC"

if [[ "${MACRUNNER_MOLTENVK_FROM_SOURCE:-0}" == "1" ]]; then
  require_file "$MVK_SRC/Makefile"
  make -C "$MVK_SRC" macos -j"$JOBS"
fi

candidate=""
while IFS= read -r path; do
  candidate="$path"
  break
done < <(find "$MVK_SRC" -maxdepth 8 -name libMoltenVK.dylib -type f 2>/dev/null | sort)

if [[ -z "$candidate" ]]; then
  if command -v brew >/dev/null 2>&1 && [[ -f "$(brew --prefix molten-vk 2>/dev/null)/lib/libMoltenVK.dylib" ]]; then
    candidate="$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"
  elif [[ -f /opt/homebrew/lib/libMoltenVK.dylib ]]; then
    candidate="/opt/homebrew/lib/libMoltenVK.dylib"
  fi
fi

if [[ -z "$candidate" ]]; then
  echo "MoltenVK dylib not found; install molten-vk or set MACRUNNER_MOLTENVK_FROM_SOURCE=1" >&2
  exit 1
fi

cp -f "$candidate" "$DIST_LIB/libMoltenVK.dylib"
file "$DIST_LIB/libMoltenVK.dylib"
otool -L "$DIST_LIB/libMoltenVK.dylib" | sed -n '1,8p'
write_manifest "$GRAPHICS_DIST/manifest/moltenvk.json" moltenvk PASS "copied native arm64 dylib from $candidate"

echo "MoltenVK PASS: $DIST_LIB/libMoltenVK.dylib"
