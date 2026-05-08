#!/bin/bash
# Подписывает Wine бинарники с нужными entitlements для запуска на Apple Silicon
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WINE_DIST="$PROJECT_ROOT/engine/wine/dist"
ENTITLEMENTS="$PROJECT_ROOT/scripts/wine-entitlements.plist"

if [ ! -d "$WINE_DIST" ]; then
    echo "❌ Wine dist не найден"
    exit 1
fi

echo "✍️  Подписываю Wine бинарники с нужными entitlements..."

# Подписываем все исполняемые в bin/
find "$WINE_DIST/bin" -type f -perm +111 | while read bin; do
    if file "$bin" | grep -q "Mach-O"; then
        codesign --force --sign - --entitlements "$ENTITLEMENTS" --options runtime "$bin" 2>&1 | grep -v "replacing existing signature" || true
    fi
done

# Подписываем все .dylib и .so в lib/
find "$WINE_DIST/lib" -type f \( -name "*.dylib" -o -name "*.so" \) | while read lib; do
    codesign --force --sign - --entitlements "$ENTITLEMENTS" --options runtime "$lib" 2>/dev/null || \
    codesign --force --sign - --options runtime "$lib" 2>/dev/null || true
done

echo "✅ Подписано"
