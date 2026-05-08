#!/bin/bash
# MacRunner — установка зависимостей для разработки
set -e

echo "🚀 MacRunner: установка зависимостей"
echo ""

# Проверка Homebrew
if ! command -v brew &> /dev/null; then
    echo "❌ Homebrew не установлен. Установи: https://brew.sh"
    exit 1
fi

echo "✅ Homebrew найден"

# Проверка ARM64
if [ "$(uname -m)" != "arm64" ]; then
    echo "⚠️  Не Apple Silicon. Проект оптимизирован под M-серию."
fi

# Проверка macOS версии
macos_major=$(sw_vers -productVersion | cut -d. -f1)
if [ "$macos_major" -lt 14 ]; then
    echo "❌ Требуется macOS 14 (Sonoma) или новее. У тебя: $(sw_vers -productVersion)"
    exit 1
fi

echo "✅ macOS $(sw_vers -productVersion)"

echo ""
echo "📦 Устанавливаю зависимости через Homebrew..."

# Wine deps (для сборки своего Wine)
brew install --quiet \
    autoconf \
    automake \
    bison \
    flex \
    gcc \
    gnutls \
    libtool \
    mingw-w64 \
    pkg-config \
    || true

# Графические зависимости
brew install --quiet \
    molten-vk \
    vulkan-headers \
    vulkan-loader \
    || true

# Полезные утилиты
brew install --quiet \
    cmake \
    ninja \
    git-lfs \
    || true

# Reference Wine и Whisky — опционально, ставь вручную если нужно для сравнения:
#   brew install --cask wine-stable
#   brew install --cask whisky
# Нам они не нужны для нашей сборки.

echo ""
echo "✅ Готово! Следующий шаг:"
echo "   ./scripts/setup-wine.sh"
