#!/bin/bash
# MacRunner — сборка DXVK (DirectX 9/10/11 → Vulkan)
# Использует Meson + mingw-w64 cross-compile
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DXVK_SRC="$PROJECT_ROOT/engine/dxvk"

if [ ! -d "$DXVK_SRC" ]; then
    echo "❌ DXVK source не найден: $DXVK_SRC"
    exit 1
fi

# DXVK требует meson + ninja + mingw-w64
for tool in meson ninja x86_64-w64-mingw32-gcc; do
    if ! command -v "$tool" &> /dev/null; then
        echo "❌ Не установлен: $tool"
        echo "   Запусти: brew install meson ninja mingw-w64"
        exit 1
    fi
done

echo "🔧 MacRunner: сборка DXVK"
echo "📁 Source: $DXVK_SRC"
echo ""

cd "$DXVK_SRC"

# Сборка для x86_64 (под Windows-target внутри Wine)
if [ -f "package-release.sh" ]; then
    echo "🔨 Использую DXVK package-release.sh..."
    ./package-release.sh master /tmp/dxvk-build --no-package 2>&1 | tail -20
elif [ -f "build-win64.txt" ]; then
    BUILD_DIR="$DXVK_SRC/build-win64"
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" --cross-file build-win64.txt --buildtype release 2>&1 | tail -10
    cd "$BUILD_DIR"
    ninja 2>&1 | tail -10
else
    echo "❌ Не нашёл build instruction в DXVK"
    ls
    exit 1
fi

echo ""
echo "✅ DXVK собран"
