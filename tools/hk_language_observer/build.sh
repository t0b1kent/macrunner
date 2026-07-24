#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
. "$ROOT/config/env.sh"
BUILD_DIR=${1:-"$ROOT/reports/phase4-hollow-knight/first-run-language-static-20260718/build"}
DIST_DIR=${2:-"$ROOT/engine/wine/dist-arm64ec-spike/lib/wine/x86_64-windows"}
CC="$ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/x86_64-w64-mingw32-clang"
SOURCE="$ROOT/tools/hk_language_observer/hk_language_observer.c"
NAME=mono-profiler-hk_language.dll

mkdir -p "$BUILD_DIR" "$DIST_DIR"
export SOURCE_DATE_EPOCH=0
"$CC" -std=c11 -O2 -Wall -Wextra -Werror -fno-ident -fno-builtin \
    -fno-stack-protector -nostdlib -shared \
    -Wl,--entry,DllMain,--no-insert-timestamp,--dynamicbase,--nxcompat \
    -o "$BUILD_DIR/$NAME" "$SOURCE" -lkernel32 -luser32
python3 "$ROOT/tools/hk_language_observer/zero_pe_entry.py" "$BUILD_DIR/$NAME"
cp "$BUILD_DIR/$NAME" "$DIST_DIR/$NAME"
cmp "$BUILD_DIR/$NAME" "$DIST_DIR/$NAME"
shasum -a 256 "$BUILD_DIR/$NAME" "$DIST_DIR/$NAME"
