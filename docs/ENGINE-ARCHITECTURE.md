# Архитектура движка

## Слои сверху вниз

```
┌──────────────────────────────────────────────────────┐
│ Windows .exe (любой архитектуры x86 / x86_64 / ARM)  │
└──────────────────────────────────────────────────────┘
                         ↓
┌──────────────────────────────────────────────────────┐
│ AI автоконфигуратор: профиль программы               │
│  • детект бинарника (хеш, метаданные, импорты)       │
│  • подбор Wine settings + DLL overrides              │
│  • парсинг логов и автофиксы (через Claude API)      │
└──────────────────────────────────────────────────────┘
                         ↓
┌──────────────────────────────────────────────────────┐
│ Наш Wine форк (LGPL 2.1)                             │
│  • базируется на Wine HQ vanilla                     │
│  • наши патчи для проблемных программ                │
│  • нативный ARM64 для самого Wine                    │
└──────────────────────────────────────────────────────┘
        ↓                ↓                ↓
┌─────────────┐  ┌─────────────┐  ┌──────────────────┐
│ Rosetta 2   │  │ Графика     │  │ CoreAudio        │
│ (x86 → ARM) │  │ (см. ниже)  │  │ Networking       │
│ системная   │  │             │  │ FileSystem       │
└─────────────┘  └─────────────┘  └──────────────────┘
                         ↓
              ┌──────────────────────┐
              │ Графический роутер   │
              │ — DX 11/12 → DXMT    │
              │ — DX  9/10 → DXVK    │
              │ — DX 12   → VKD3D    │
              └──────────────────────┘
                ↓                ↓
        ┌──────────────┐  ┌─────────────────┐
        │ DXMT         │  │ DXVK / VKD3D    │
        │ DX → Metal   │  │ DX → Vulkan     │
        │ напрямую     │  │                 │
        └──────────────┘  └─────────────────┘
                                  ↓
                          ┌──────────────┐
                          │ MoltenVK     │
                          │ Vulkan→Metal │
                          └──────────────┘
                                  ↓
              ┌──────────────────────────┐
              │ Apple Metal (нативный)   │
              └──────────────────────────┘
```

## Маршрутизация графики (важное преимущество над GPTK)

GPTK имеет только **один путь**: DX → D3DMetal → Metal.

У нас **два пути с автовыбором:**

1. **DXMT** для современного DirectX 11/12 (быстрее)
2. **DXVK + MoltenVK** для legacy DirectX и когда DXMT не справляется (универсальнее)

Это даёт **более широкое покрытие** программ из коробки.

## Rosetta 2 интеграция

Apple с macOS Sonoma (14) разрешила Rosetta для эмуляции внутри Wine процессов.

Доступ через системное API:
```c
// Концепт
#include <mach-o/loader.h>

// Помечаем процесс для Rosetta трансляции
posix_spawnattr_setflags(&attr, POSIX_SPAWN_OSX_USE_ROSETTA);
```

Wine компилируется как **ARM64-нативный**. Только Windows-бинарник идёт через Rosetta. Это даёт ~80-90% от нативной x86 Windows скорости.

## AI автоконфигуратор — наш дифференциатор

### Что делает

1. **Анализ exe**:
   - Парсинг PE-заголовка (импортируемые DLL, версия Windows, архитектура)
   - Извлечение иконки и метаданных
   - Хеш для матчинга с базой профилей

2. **Подбор профиля**:
   - Если в базе есть профиль — применяем
   - Если нет — генерируем дефолтный + AI рекомендации

3. **Парсинг логов Wine**:
   - При сбое программы парсим Wine debug output
   - Скармливаем Claude API с промптом "что не так и как починить"
   - Применяем фикс автоматически (DLL override, регистр, патч)

4. **Обучение базы**:
   - Успешные конфиги сохраняются
   - Community-driven профили (как ProtonDB)

### API скетч

```swift
// app/Sources/Engine/Configurator.swift
struct ProgramProfile {
    let id: String
    let name: String
    let requiredDlls: [String]
    let wineSettings: WineSettings
    let patches: [Patch]
}

actor Configurator {
    func analyze(exe: URL) async -> ProgramProfile {
        // 1. Hash + metadata
        // 2. Lookup in profiles DB
        // 3. Fallback: AI generation via Claude API
    }

    func diagnoseFailure(logs: String, profile: ProgramProfile) async -> [Fix] {
        // Send logs to Claude → get fixes
    }
}
```

## План интеграции по неделям

### Неделя 1: Wine baseline
- Build Wine HQ под ARM64
- Запуск notepad.exe
- Записать что не работает

### Неделя 2-3: DXMT
- Build DXMT
- Линковка с Wine
- Тест на DirectX 11 простой программе

### Неделя 4-5: DXVK + MoltenVK
- Build MoltenVK
- Build DXVK
- Графический роутер: автовыбор DXMT vs DXVK

### Неделя 6: Rosetta 2
- Системное API integration
- Тест: x86 .exe запускается на ARM Wine

### Неделя 7-8: Real software test
- 1С полный цикл
- AutoCAD LT
- Простая игра

### Неделя 9-12: AI configurator
- PE parser
- Profile DB schema
- Claude API integration для диагностики

### Неделя 13+: BIM атака
- Civil 3D, Revit конкретные проблемы
- Wine патчи под них
