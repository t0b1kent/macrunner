# MacRunner

Native Windows app & game runner for Apple Silicon Macs.
Конкурент Crossover с фокусом на BIM/CAD (Civil 3D, Revit, Navisworks) и СНГ-софт (1С).

## Цель

Запускать Windows-программы и игры на Mac **нативнее, быстрее и удобнее** чем Crossover, Mythic и Whisky.

## Стек

- **Wine** (форк Wine HQ) — Windows API → macOS
- **DXMT** — DirectX 10/11 → Metal
- **DXVK + MoltenVK** — DirectX 9 / Vulkan → Metal
- **Rosetta 2** — x86 → ARM64 (через системное API)
- **SwiftUI** — нативный Mac UI

## Структура проекта

- `app/` — SwiftUI приложение
- `wine-fork/` — наш форк Wine
- `profiles/` — JSON профили программ
- `scripts/` — установка зависимостей, билды
- `docs/` — документация
- `tests/` — тестовые сценарии для популярных программ

## Документы по проекту

См. Obsidian vault: `~/Documents/MacRunner/`

## Системные требования

- macOS 14 (Sonoma) или новее
- Apple Silicon (M1/M2/M3/M4)
- Xcode Command Line Tools
- Homebrew

## Quick Start

```bash
./scripts/install-deps.sh
./scripts/setup-wine.sh
```
