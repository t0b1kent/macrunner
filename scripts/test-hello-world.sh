#!/bin/bash
# MacRunner — Hello World тест: запуск notepad.exe через Wine
set -e

echo "🧪 MacRunner: Hello World тест"
echo ""

# Проверка Wine
if ! command -v wine &> /dev/null; then
    echo "❌ Wine не найден. Запусти ./scripts/install-deps.sh"
    exit 1
fi

echo "✅ Wine версия: $(wine --version)"
echo ""

# Создаём изолированный prefix
PREFIX_DIR="$HOME/Library/Application Support/MacRunner/test-bottle"
mkdir -p "$PREFIX_DIR"
export WINEPREFIX="$PREFIX_DIR"
export WINEDEBUG=-all

echo "🍾 Bottle: $PREFIX_DIR"
echo ""

# Инициализация Wine prefix (если ещё не создан)
if [ ! -d "$PREFIX_DIR/drive_c" ]; then
    echo "⏳ Инициализация Wine prefix (займёт минуту)..."
    wineboot -i 2>&1 | tail -5
fi

echo ""
echo "🚀 Запускаю notepad.exe..."
wine notepad.exe &

echo ""
echo "✅ Если открылся блокнот — Wine работает!"
echo "   Закрой окно блокнота для выхода из теста."
