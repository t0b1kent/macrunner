#!/bin/bash
# MacRunner — клонирование всех исходников движка
# Wine + DXMT + DXVK + MoltenVK
set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE_DIR="$PROJECT_ROOT/engine"
mkdir -p "$ENGINE_DIR"
cd "$ENGINE_DIR"

echo "🔧 MacRunner: клонирование исходников движка"
echo "📁 Engine root: $ENGINE_DIR"
echo ""

# Wine HQ — последняя стабильная (LGPL)
if [ ! -d "wine" ]; then
    echo "⏳ Клон Wine HQ (это займёт 5-15 мин, ~1.5GB)..."
    git clone --depth 1 https://gitlab.winehq.org/wine/wine.git
    echo "✅ Wine склонирован"
else
    echo "✅ Wine уже есть, обновляю..."
    (cd wine && git pull --ff-only || true)
fi
echo ""

# DXMT — DirectX 10/11 → Metal (open source аналог D3DMetal)
if [ ! -d "dxmt" ]; then
    echo "⏳ Клон DXMT..."
    git clone --depth 1 https://github.com/3Shain/dxmt.git
    echo "✅ DXMT склонирован"
else
    echo "✅ DXMT уже есть, обновляю..."
    (cd dxmt && git pull --ff-only || true)
fi
echo ""

# DXVK — DirectX 9/10/11 → Vulkan (для legacy/fallback)
if [ ! -d "dxvk" ]; then
    echo "⏳ Клон DXVK..."
    git clone --depth 1 https://github.com/doitsujin/dxvk.git
    echo "✅ DXVK склонирован"
else
    echo "✅ DXVK уже есть, обновляю..."
    (cd dxvk && git pull --ff-only || true)
fi
echo ""

# MoltenVK — Vulkan → Metal
if [ ! -d "MoltenVK" ]; then
    echo "⏳ Клон MoltenVK..."
    git clone --depth 1 https://github.com/KhronosGroup/MoltenVK.git
    echo "✅ MoltenVK склонирован"
else
    echo "✅ MoltenVK уже есть, обновляю..."
    (cd MoltenVK && git pull --ff-only || true)
fi
echo ""

# VKD3D-Proton — DirectX 12 → Vulkan (опционально, для DX12 игр)
if [ ! -d "vkd3d-proton" ]; then
    echo "⏳ Клон VKD3D-Proton (DirectX 12 → Vulkan)..."
    git clone --depth 1 https://github.com/HansKristian-Work/vkd3d-proton.git
    echo "✅ VKD3D-Proton склонирован"
else
    echo "✅ VKD3D-Proton уже есть, обновляю..."
    (cd vkd3d-proton && git pull --ff-only || true)
fi
echo ""

echo "📊 Размер всех исходников:"
du -sh "$ENGINE_DIR"/* 2>/dev/null | sort -h
echo ""
echo "✅ Все исходники получены."
echo "Следующий шаг: ./scripts/build-wine.sh"
