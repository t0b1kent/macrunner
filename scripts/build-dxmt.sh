#!/bin/bash
# MacRunner — сборка DXMT (DirectX 10/11 → Metal)
# DXMT — open source аналог Apple D3DMetal
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DXMT_SRC="$PROJECT_ROOT/engine/dxmt"

if [ ! -d "$DXMT_SRC" ]; then
    echo "❌ DXMT source не найден: $DXMT_SRC"
    echo "   Запусти ./scripts/clone-sources.sh"
    exit 1
fi

# DXMT использует Meson + mingw-w64 + LLVM 15+
for tool in meson ninja x86_64-w64-mingw32-gcc; do
    if ! command -v "$tool" &> /dev/null; then
        echo "❌ Не установлен: $tool"
        echo "   brew install meson ninja mingw-w64"
        exit 1
    fi
done

# LLVM проверка (DXMT нужен LLVM 15+ для airconv shader translator)
if ! command -v llvm-config &> /dev/null; then
    echo "⚠️  llvm-config не найден. Устанавливаю LLVM..."
    brew install llvm@15 2>&1 | tail -3
    export PATH="/opt/homebrew/opt/llvm@15/bin:$PATH"
fi

echo "🔧 MacRunner: сборка DXMT"
echo "📁 Source: $DXMT_SRC"
echo "📦 LLVM: $(llvm-config --version 2>/dev/null || echo 'not found')"
echo ""

cd "$DXMT_SRC"

# Способ 1: package-release.sh (если есть)
if [ -f "package-release.sh" ]; then
    echo "🔨 DXMT package-release.sh..."
    ./package-release.sh master /tmp/dxmt-build --no-package 2>&1 | tail -20
# Способ 2: meson напрямую
elif [ -f "build-win64.txt" ] || [ -f "build.win64.cross.txt" ]; then
    CROSSFILE=$(ls build*.txt | head -1)
    BUILD_DIR="$DXMT_SRC/build-win64"
    rm -rf "$BUILD_DIR"
    meson setup "$BUILD_DIR" --cross-file "$CROSSFILE" --buildtype release 2>&1 | tail -10
    ninja -C "$BUILD_DIR" 2>&1 | tail -10
else
    echo "ℹ️  Доступные файлы:"
    ls
    echo "❌ Нужно изучить структуру DXMT и адаптировать скрипт"
    exit 1
fi

echo ""
echo "✅ DXMT собран"
