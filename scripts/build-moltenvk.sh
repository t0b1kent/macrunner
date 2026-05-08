#!/bin/bash
# MacRunner — сборка MoltenVK (Vulkan → Metal)
# Используем CrossOver-проверенную версию из crossover-source
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MVK_SRC="$PROJECT_ROOT/engine/moltenvk"

if [ ! -d "$MVK_SRC" ]; then
    echo "❌ MoltenVK source не найден: $MVK_SRC"
    echo "   Запусти ./scripts/clone-sources.sh"
    exit 1
fi

echo "🔧 MacRunner: сборка MoltenVK"
echo "📁 Source: $MVK_SRC"
echo ""

cd "$MVK_SRC"

# CrossOver MoltenVK имеет MoltenVK подкаталог как реальный Khronos source
if [ -d "MoltenVK" ]; then
    cd MoltenVK
fi

# fetch external dependencies if needed
if [ -f "fetchDependencies" ] && [ ! -d "External" ]; then
    echo "📥 Загружаю зависимости MoltenVK..."
    ./fetchDependencies --macos 2>&1 | tail -5
fi

# Сборка
echo "🔨 Сборка MoltenVK для ARM64 macOS..."
if [ -f "Makefile" ]; then
    make macos -j$(sysctl -n hw.ncpu) 2>&1 | tail -10
elif [ -d "MoltenVK.xcodeproj" ]; then
    xcodebuild -project MoltenVK.xcodeproj \
        -scheme "MoltenVK Package" \
        -configuration Release \
        -arch arm64 \
        ARCHS=arm64 \
        2>&1 | tail -10
else
    echo "❌ Не нашёл Makefile или xcodeproj"
    exit 1
fi

echo ""
echo "✅ MoltenVK собран"
echo "   Артефакты: $MVK_SRC/Package/Release/MoltenVK/"
