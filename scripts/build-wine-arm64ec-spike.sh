#!/bin/bash
# Experimental pure-ARM64 Wine build for MacRunner Phase G sanity checks.
# Does not replace the production ARM64X dist.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export MACRUNNER_ROOT="$PROJECT_ROOT"
. "$PROJECT_ROOT/config/env.sh"

WINE_SRC="$PROJECT_ROOT/engine/wine"
: "${WINE_BUILD:=$PROJECT_ROOT/engine/wine/build-arm64ec-spike}"
: "${WINE_INSTALL:=$PROJECT_ROOT/engine/wine/dist-arm64ec-spike}"

if [ ! -d "$WINE_SRC" ]; then
    echo "Wine source missing: $WINE_SRC" >&2
    exit 1
fi

if [ -n "${MACRUNNER_WINE_CCACHE_PREFIX:-}" ]; then
    echo "ccache enabled: $CCACHE_DIR"
fi
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=14.0 -I/opt/homebrew/include"
export CPPFLAGS="-I/opt/homebrew/include"
export LDFLAGS="-arch arm64 -mmacosx-version-min=14.0 -L/opt/homebrew/lib"
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig:${PKG_CONFIG_PATH:-}"

mkdir -p "$WINE_BUILD" "$WINE_INSTALL" "$MACRUNNER_REPORTS_ROOT/build"
cd "$WINE_BUILD"

if [ -f Makefile ] && command -v pkg-config >/dev/null 2>&1; then
    pkg_glib_cflags=$("${PKG_CONFIG:-pkg-config}" --cflags glib-2.0 2>/dev/null || true)
    pkg_glib_include=$(printf '%s\n' "$pkg_glib_cflags" | tr ' ' '\n' | awk '$0 ~ /^-I/ && $0 ~ /\/glib\/[0-9]/ {sub(/^-I/, "", $0); print; exit}')
    if [ -n "$pkg_glib_include" ] && ! grep -Fq -- "$pkg_glib_include" Makefile; then
        echo "♻️ Обнаружен изменившийся путь glib от pkg-config, пересоздаю конфиг..."
        rm -f Makefile config.status
    fi
fi

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
