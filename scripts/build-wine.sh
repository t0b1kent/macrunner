#!/bin/bash
# MacRunner — сборка Wine под ARM64 macOS
#
# Этот скрипт собирает чистый Wine HQ. На M-серии Mac есть
# известные сложности — будем итеративно их фиксить.
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

# Toolchain layout:
# - host arm64 build: Apple clang (/usr/bin/clang)
# - aarch64 PE: llvm-mingw aarch64-w64-mingw32-clang
# - x86_64 / i386 PE: Homebrew mingw-w64
# llvm-mingw имеет свой `clang` который нацелен на Windows — кладём его bin
# В КОНЕЦ PATH чтобы не затенял Apple clang. wrappers (aarch64-w64-mingw32-*)
# всё равно попадают в PATH через эту директорию.
LLVM_MINGW="$PROJECT_ROOT/engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin"
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin:/usr/local/bin:$PATH:$LLVM_MINGW"

# Apple clang явно — иначе configure возьмёт Windows-target clang и не сможет
# собрать host loader.
export CC="/usr/bin/clang"
export CXX="/usr/bin/clang++"

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

if [ ! -f "Makefile" ]; then
    echo "⚙️  Запускаю configure..."
    # aarch64 PE — для host системных сервисов (wineboot, services, conhost, ...)
    # x86_64 / i386 PE — для Windows-программ (через Rosetta 2 на M-чипе)
    "$WINE_SRC/configure" \
        --prefix="$WINE_INSTALL" \
        --enable-archs=aarch64,x86_64,i386 \
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
