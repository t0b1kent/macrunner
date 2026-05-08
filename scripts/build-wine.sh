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

# Гарантия что используем Homebrew toolchain
# bison и flex в Homebrew keg-only — нужно добавить их явно ПЕРЕД системным PATH
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:/opt/homebrew/bin:/opt/homebrew/sbin:$PATH"

# Wine на macOS требует чтобы CC указывал на правильный clang
export CC="clang"
export CXX="clang++"

# Флаги
export CFLAGS="-O2 -arch arm64 -mmacosx-version-min=14.0"
export LDFLAGS="-arch arm64 -mmacosx-version-min=14.0"

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
    "$WINE_SRC/configure" \
        --prefix="$WINE_INSTALL" \
        --enable-archs=x86_64,i386 \
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
