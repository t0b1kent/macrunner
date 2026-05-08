#!/bin/bash
# MacRunner — подписать все Wine бинари нашим dev-сертификатом
#
# Использование:
#   1) Получить cert: Xcode → Accounts → "+" Apple ID
#   2) Проверить: security find-identity -v -p codesigning
#   3) Запустить с identity name:
#        ./scripts/sign-engine.sh "Apple Development: Your Name (TEAMID)"
#      или вообще без аргументов — попробуем подобрать первый Apple Development cert
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$PROJECT_ROOT/engine/wine/dist"
ENT="$PROJECT_ROOT/wine-fork/wine.entitlements"

# Если идентичность не передали — пробуем найти Apple Development cert автоматически
IDENTITY="${1:-}"
if [ -z "$IDENTITY" ]; then
    IDENTITY=$(security find-identity -v -p codesigning 2>/dev/null | grep -m1 "Apple Development" | awk -F'"' '{print $2}')
    if [ -z "$IDENTITY" ]; then
        echo "❌ Не нашёл Apple Development cert."
        echo "   Открой Xcode → Settings → Accounts → '+' Apple ID и войди."
        echo "   Потом: security find-identity -v -p codesigning"
        exit 1
    fi
    echo "🔑 Использую: $IDENTITY"
fi

# Создаём entitlements если нет
if [ ! -f "$ENT" ]; then
    mkdir -p "$(dirname "$ENT")"
    cat > "$ENT" << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key><true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
    <key>com.apple.security.cs.allow-dyld-environment-variables</key><true/>
    <key>com.apple.security.cs.disable-library-validation</key><true/>
    <key>com.apple.security.cs.disable-executable-page-protection</key><true/>
    <key>com.apple.security.get-task-allow</key><true/>
</dict>
</plist>
EOF
fi

echo "🔏 Подписываю Wine бинари..."
echo ""

# CrossOver source ставит identifier `com.codeweavers.CrossOver.wineloader`
# в loader/wine_info.plist.in — Apple привязал его к team 27GN9XE9CP.
# Когда мы подписываем нашим сертификатом, ASP видит mismatch и блокирует.
# Решение: переопределить identifier на наш собственный через --identifier.
MR_BIN_ID="app.macrunner.wine"
MR_LOADER_ID="app.macrunner.wineloader"
MR_LIB_PREFIX="app.macrunner.lib"

SIGNED=0

# Top-level bin/* — даём bin-уровневый identifier
for f in "$DIST/bin/"*; do
    [ -f "$f" ] || continue
    if file "$f" | grep -q "Mach-O.*executable"; then
        name=$(basename "$f")
        codesign --force --sign "$IDENTITY" \
            --identifier "$MR_BIN_ID.$name" \
            --entitlements "$ENT" --options runtime "$f" 2>&1 | tail -1
        SIGNED=$((SIGNED + 1))
    fi
done

# Secondary loader (lib/wine/aarch64-unix/wine) — наш identifier
codesign --force --sign "$IDENTITY" \
    --identifier "$MR_LOADER_ID" \
    --entitlements "$ENT" --options runtime \
    "$DIST/lib/wine/aarch64-unix/wine" 2>&1 | tail -1
SIGNED=$((SIGNED + 1))

# Все .so в aarch64-unix
for f in "$DIST/lib/wine/aarch64-unix/"*.so; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .so)
    codesign --force --sign "$IDENTITY" \
        --identifier "$MR_LIB_PREFIX.$name" \
        --entitlements "$ENT" --options runtime "$f" 2>/dev/null
    SIGNED=$((SIGNED + 1))
done

echo ""
echo "✅ Подписано $SIGNED файлов"
echo ""
echo "Тест:"
echo "   $DIST/bin/wine wineboot --init"
echo "   $DIST/bin/wine notepad.exe"
