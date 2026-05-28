#!/bin/bash
# MacRunner — сборка Wine под ARM64 macOS
#
# Этот скрипт собирает чистый Wine HQ. На M-серии Mac есть
# известные сложности — будем итеративно их фиксить.
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$PROJECT_ROOT/config/env.sh"
WINE_SRC="$PROJECT_ROOT/engine/wine"
WINE_BUILD="$PROJECT_ROOT/engine/wine/build"
WINE_INSTALL="$PROJECT_ROOT/engine/wine/dist"

if [ ! -d "$WINE_SRC" ]; then
    echo "❌ Wine source не найден. Запусти ./scripts/clone-sources.sh"
    exit 1
fi

echo "🛠  MacRunner: сборка Wine под ARM64 macOS"
echo "📁 Source: $WINE_SRC"
echo "📁 Build:  $WINE_BUILD"
echo "📁 Install: $WINE_INSTALL"
echo ""

echo "🌉 Сборка HyperBridge runtime для Wine Unix-side entrypoint dispatch..."
make -C "$PROJECT_ROOT/engine/hyperbridge" all
echo ""

# Toolchain layout:
# - host arm64 build: Apple clang (/usr/bin/clang)
# - all PE targets: bundled llvm-mingw *-w64-mingw32-clang
#
# ARM64EC builds compile some x64 companion objects with $(x86_64_CC). Homebrew
# mingw-gcc cannot consume Wine's clang-style ARM64EC flags/assembler output, so
# target-prefixed compilers must resolve to llvm-mingw first. Host CC is still
# pinned to Apple clang below, so prepending llvm-mingw does not hijack host code.
LLVM_MINGW="$PROJECT_ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin"
export PATH="$LLVM_MINGW:/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/usr/local/bin:$PATH"

# Apple clang явно — иначе configure возьмёт Windows-target clang и не сможет
# собрать host loader.
if command -v ccache >/dev/null 2>&1 && [ "${MACRUNNER_USE_CCACHE:-1}" != "0" ]; then
    echo "⚡ ccache enabled: $CCACHE_DIR"
    CCACHE_PREFIX="ccache "
else
    CCACHE_PREFIX=""
fi

export CC="${CCACHE_PREFIX}/usr/bin/clang"
export CXX="${CCACHE_PREFIX}/usr/bin/clang++"

# Keep target compiler selection deterministic even if Homebrew mingw-w64 is
# installed earlier in a user's shell PATH.
export aarch64_CC="${CCACHE_PREFIX}$LLVM_MINGW/aarch64-w64-mingw32-clang"
export aarch64_CXX="${CCACHE_PREFIX}$LLVM_MINGW/aarch64-w64-mingw32-clang++"
export arm64ec_CC="${CCACHE_PREFIX}$LLVM_MINGW/arm64ec-w64-mingw32-clang"
export arm64ec_CXX="${CCACHE_PREFIX}$LLVM_MINGW/arm64ec-w64-mingw32-clang++"
export x86_64_CC="${CCACHE_PREFIX}$LLVM_MINGW/x86_64-w64-mingw32-clang"
export x86_64_CXX="${CCACHE_PREFIX}$LLVM_MINGW/x86_64-w64-mingw32-clang++"
export i386_CC="${CCACHE_PREFIX}$LLVM_MINGW/i686-w64-mingw32-clang"
export i386_CXX="${CCACHE_PREFIX}$LLVM_MINGW/i686-w64-mingw32-clang++"

# Флаги
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=14.0 -I/opt/homebrew/include"
export CPPFLAGS="-I/opt/homebrew/include"
# Homebrew /opt/homebrew/lib обязательно — иначе configure не находит libvulkan/libMoltenVK
export LDFLAGS="-arch arm64 -mmacosx-version-min=14.0 -L/opt/homebrew/lib"
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:/opt/homebrew/share/pkgconfig:${PKG_CONFIG_PATH:-}"

# Проверки зависимостей
for dep in autoconf automake bison flex pkg-config mingw-w64; do
    if ! brew list "$dep" &> /dev/null; then
        echo "❌ Не установлен: $dep"
        echo "   Запусти ./scripts/install-deps.sh"
        exit 1
    fi
done

mkdir -p "$WINE_BUILD" "$WINE_INSTALL"
cd "$WINE_BUILD"

if [ "${FORCE_RECONFIGURE:-0}" = "1" ]; then
    rm -f Makefile
fi

if [ ! -f "Makefile" ]; then
    echo "⚙️  Запускаю configure..."
    # aarch64 PE — для host системных сервисов (wineboot, services, conhost, ...)
    # arm64ec PE — для hybrid x64-on-arm64 bootstrap/dispatcher path
    # x86_64 / i386 PE — для Windows-программ и fallback lanes
    "$WINE_SRC/configure" \
        --prefix="$WINE_INSTALL" \
        --enable-archs=aarch64,arm64ec,x86_64,i386 \
        --disable-tests \
        --without-x \
        --without-alsa \
        --without-capi \
        --without-oss \
        --without-pulse \
        --with-coreaudio \
        2>&1 | tee configure.log
fi

echo ""
echo "🔨 Сборка (используется $(sysctl -n hw.ncpu) ядер)..."
# pipefail чтобы set -e ловил ошибки make сквозь pipe в tee
set -o pipefail
make -j$(sysctl -n hw.ncpu) 2>&1 | tee build.log
make_status=${PIPESTATUS[0]}
if [ "$make_status" != "0" ]; then
    echo ""
    echo "❌ make упал с кодом $make_status"
    echo "Последние ошибки:"
    grep -E "error:|Error [0-9]" build.log | tail -10
    exit "$make_status"
fi

echo ""
echo "📦 Установка в $WINE_INSTALL..."
make install 2>&1 | tee install.log
install_status=${PIPESTATUS[0]}
if [ "$install_status" != "0" ]; then
    echo "❌ make install упал с кодом $install_status"
    exit "$install_status"
fi

echo ""
echo "✅ Wine собран!"
echo "   Бинарник: $WINE_INSTALL/bin/wine"
echo ""
echo "Тест:"
echo "   $WINE_INSTALL/bin/wine --version"
echo "   $WINE_INSTALL/bin/wine notepad.exe"
