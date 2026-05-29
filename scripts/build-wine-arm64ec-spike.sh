#!/bin/bash
# Experimental pure-ARM64 Wine build for MacRunner Phase G sanity checks.
# Does not replace the production ARM64X dist.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$PROJECT_ROOT/config/env.sh"

WINE_SRC="$PROJECT_ROOT/engine/wine"
WINE_BUILD="$PROJECT_ROOT/engine/wine/build-arm64ec-spike"
WINE_INSTALL="$PROJECT_ROOT/engine/wine/dist-arm64ec-spike"
LLVM_MINGW="$PROJECT_ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin"

if [ ! -d "$WINE_SRC" ]; then
    echo "Wine source missing: $WINE_SRC" >&2
    exit 1
fi

export PATH="$LLVM_MINGW:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/usr/local/bin:$PATH"
if command -v ccache >/dev/null 2>&1 && [ "${MACRUNNER_USE_CCACHE:-1}" != "0" ]; then
    echo "ccache enabled: $CCACHE_DIR"
    CCACHE_PREFIX="ccache "
else
    CCACHE_PREFIX=""
fi
export CC="${CCACHE_PREFIX}/usr/bin/clang"
export CXX="${CCACHE_PREFIX}/usr/bin/clang++"
export aarch64_CC="${CCACHE_PREFIX}$LLVM_MINGW/aarch64-w64-mingw32-clang"
export aarch64_CXX="${CCACHE_PREFIX}$LLVM_MINGW/aarch64-w64-mingw32-clang++"
export x86_64_CC="${CCACHE_PREFIX}$LLVM_MINGW/x86_64-w64-mingw32-clang"
export x86_64_CXX="${CCACHE_PREFIX}$LLVM_MINGW/x86_64-w64-mingw32-clang++"
export i386_CC="${CCACHE_PREFIX}$LLVM_MINGW/i686-w64-mingw32-clang"
export i386_CXX="${CCACHE_PREFIX}$LLVM_MINGW/i686-w64-mingw32-clang++"
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=14.0 -I/opt/homebrew/include"
export CPPFLAGS="-I/opt/homebrew/include"
export LDFLAGS="-arch arm64 -mmacosx-version-min=14.0 -L/opt/homebrew/lib"
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig:${PKG_CONFIG_PATH:-}"

mkdir -p "$WINE_BUILD" "$WINE_INSTALL" "$MACRUNNER_REPORTS_ROOT/build"
cd "$WINE_BUILD"

if [ "${FORCE_RECONFIGURE:-0}" = "1" ]; then rm -f Makefile; fi

if [ ! -f Makefile ]; then
    "$WINE_SRC/configure" \
        --prefix="$WINE_INSTALL" \
        --enable-archs=arm64ec,aarch64,i386,x86_64 \
        --disable-tests \
        --without-x \
        --without-alsa \
        --without-capi \
        --without-oss \
        --without-pulse \
        --with-coreaudio \
        2>&1 | tee configure.log
fi

make -j"$(sysctl -n hw.ncpu)" 2>&1 | tee build.log
make install 2>&1 | tee install.log

echo "arm64ec spike installed: $WINE_INSTALL"
