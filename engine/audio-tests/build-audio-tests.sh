#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_DIR="$ROOT/engine/audio-tests"
OUT_DIR_ARM64="$SRC_DIR/out/arm64"
OUT_DIR_X64="$SRC_DIR/out/x64"
FIXTURE_DIR_ARM64="$ROOT/fixtures/arm64"
FIXTURE_DIR_X64="$ROOT/fixtures/x64"
TOOLCHAIN_DIR="$ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin"
CC_ARM64="$TOOLCHAIN_DIR/aarch64-w64-mingw32-clang"
CC_X64="$TOOLCHAIN_DIR/x86_64-w64-mingw32-clang"
ALIGN_FLAGS="-Wl,--section-alignment,0x10000 -Wl,--file-alignment,0x10000"
CFLAGS="-Os -s"

ARM64_TARGETS=(
  "tone_winmm.c:tone_winmm.exe:-lwinmm"
  "render_mmdevapi.c:render_mmdevapi.exe:-lwinmm -lole32 -luuid"
  "mix_dsound.c:mix_dsound.exe:-ldsound -lole32 -luuid"
  "voice_xaudio2.c:voice_xaudio2.exe:-lole32 -luuid"
  "poll_dinput.c:poll_dinput.exe:-ldinput8 -lole32 -luuid -luser32"
  "poll_xinput.c:poll_xinput.exe:"
)

X64_TARGETS=(
  "mix_dsound.c:mix_dsound.exe:dsound_buffer_mix_x64.exe:-ldsound -lole32 -luuid"
  "voice_xaudio2.c:voice_xaudio2.exe:xaudio2_source_voice_x64.exe:-lole32 -luuid"
)

if [ ! -x "$CC_ARM64" ]; then
  echo "compiler missing: $CC_ARM64" >&2
  exit 1
fi

if [ ! -x "$CC_X64" ]; then
  echo "compiler missing: $CC_X64" >&2
  exit 1
fi

mkdir -p "$OUT_DIR_ARM64" "$OUT_DIR_X64" "$FIXTURE_DIR_ARM64" "$FIXTURE_DIR_X64"

build_arm64() {
  local src="$1" out="$2" libs="$3"
  local src_path="$SRC_DIR/$src"
  local out_path="$OUT_DIR_ARM64/$out"
  local fixture_path="$FIXTURE_DIR_ARM64/${out%.exe}_arm64.exe"

  if [ ! -f "$src_path" ]; then
    echo "missing source: $src_path" >&2
    return 1
  fi

  "$CC_ARM64" $CFLAGS $ALIGN_FLAGS -I"$SRC_DIR" -Wl,-subsystem,console "$src_path" -o "$out_path" $libs
  cp -f "$out_path" "$fixture_path"
  echo "built $out_path"
  echo "  fixture: $fixture_path"
}

build_x64() {
  local src="$1" out="$2" fixture="$3" libs="$4"
  local src_path="$SRC_DIR/$src"
  local out_path="$OUT_DIR_X64/$out"
  local fixture_path="$FIXTURE_DIR_X64/$fixture"

  if [ ! -f "$src_path" ]; then
    echo "missing source: $src_path" >&2
    return 1
  fi

  "$CC_X64" $CFLAGS $ALIGN_FLAGS -I"$SRC_DIR" -Wl,-subsystem,console "$src_path" -o "$out_path" $libs
  cp -f "$out_path" "$fixture_path"
  echo "built $out_path"
  echo "  fixture: $fixture_path"
}

for target in "${ARM64_TARGETS[@]}"; do
  IFS=":" read -r source exe libs <<< "$target"
  build_arm64 "$source" "$exe" "$libs"
  echo
done

for target in "${X64_TARGETS[@]}"; do
  IFS=":" read -r source exe fixture libs <<< "$target"
  build_x64 "$source" "$exe" "$fixture" "$libs"
  echo
done

echo "Audio test fixtures ready in $OUT_DIR_ARM64, $OUT_DIR_X64, $FIXTURE_DIR_ARM64 and $FIXTURE_DIR_X64"
