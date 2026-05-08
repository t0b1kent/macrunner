#!/bin/bash
# MacRunner — оркестратор сборки всего движка
# Порядок: deps → sources → MoltenVK → Wine → DXVK → DXMT
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "🚀 MacRunner: полная сборка движка"
echo ""
echo "Порядок:"
echo "  1. install-deps    (Homebrew)"
echo "  2. clone-sources   (Wine, DXMT, DXVK, MoltenVK)"
echo "  3. build-moltenvk  (Vulkan → Metal)"
echo "  4. build-wine      (Wine 11 + CrossOver Mac patches)"
echo "  5. build-dxvk      (DX9/10/11 → Vulkan)"
echo "  6. build-dxmt      (DX10/11 → Metal напрямую)"
echo ""
read -p "Продолжить? (y/n) " -n 1 -r
echo ""
[[ ! $REPLY =~ ^[Yy]$ ]] && exit 0

"$SCRIPT_DIR/install-deps.sh"
"$SCRIPT_DIR/clone-sources.sh"
"$SCRIPT_DIR/build-moltenvk.sh"
"$SCRIPT_DIR/build-wine.sh"
"$SCRIPT_DIR/build-dxvk.sh"
"$SCRIPT_DIR/build-dxmt.sh"

echo ""
echo "🎉 Сборка движка завершена!"
echo ""
echo "Тест:"
echo "  ./engine/wine/dist/bin/wine --version"
echo "  ./engine/wine/dist/bin/wine notepad.exe"
