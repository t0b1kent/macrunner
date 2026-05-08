#!/bin/bash
# MacRunner — запуск Wine + Windows .exe через Rosetta 2
#
# Wine LOADER скомпилирован arm64-нативно (быстрый).
# x86/x86_64 Windows-бинарник внутри запускается через Rosetta 2
# (аппаратная трансляция в M-чипе).
# Это даёт ~80-90% от нативной x86 Windows скорости.
#
# Usage: ./scripts/run-with-rosetta.sh path/to/program.exe [args...]
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE="$PROJECT_ROOT/engine/wine/dist/bin/wine"

if [ ! -x "$WINE" ]; then
    echo "❌ Wine не собран: $WINE"
    echo "   Запусти ./scripts/build-wine.sh"
    exit 1
fi

# Проверка Rosetta 2 (нужна для эмуляции x86 внутри процесса с macOS Sonoma+)
if ! /usr/bin/pgrep -q oahd; then
    echo "⚠️  Rosetta 2 daemon (oahd) не запущен."
    echo "   Установка: softwareupdate --install-rosetta --agree-to-license"
    exit 1
fi

# Прелоадер для Rosetta — Apple даёт системное API с Sonoma:
#   ROSETTA_ADVERTISE_AVX=1 — публикует AVX через Rosetta
#   ROSETTA_DEBUGSERVER_PORT — для дебага x86 внутри
export ROSETTA_ADVERTISE_AVX=1

# Изолированная "бутылка" для теста
PREFIX="$HOME/Library/Application Support/MacRunner/bottles/default"
mkdir -p "$PREFIX"
export WINEPREFIX="$PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"

# DYLD path для libvulkan / libMoltenVK (Homebrew)
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:${DYLD_FALLBACK_LIBRARY_PATH:-/usr/local/lib:/usr/lib}"

# WoW64 mode — позволяет x86_64 Wine запускать i386 .exe через Rosetta
# (а не только нативные x86_64 .exe)
export WINEARCH="${WINEARCH:-wow64}"

echo "🍷 Wine: $($WINE --version)"
echo "🍾 Bottle: $WINEPREFIX"
echo "🚀 Запускаю: $@"
echo ""

exec "$WINE" "$@"
