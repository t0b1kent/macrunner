# Graphics Router — наш дифференциатор vs GPTK

## Проблема
GPTK имеет **один путь**: `DX → D3DMetal → Metal`. Если D3DMetal не справляется с конкретной игрой/программой — вариантов нет.

## Решение
Автоматический роутер, который выбирает между **DXMT** и **DXVK** на основе версии DirectX и success/failure history.

## Маршруты

| DX версия | Первичный | Fallback | Комментарий |
|-----------|-----------|----------|-------------|
| DX 7 / 8 | WineD3D | — | через OpenGL |
| **DX 9** | **DXVK** | WineD3D | DXMT не поддерживает |
| DX 10 | DXMT | DXVK | один слой быстрее |
| **DX 11** | **DXMT** | DXVK | оптимальный путь |
| **DX 12** | **DXMT** | VKD3D-Proton | DX12 → Metal напрямую |
| Vulkan | MoltenVK | — | нативный путь |

## Алгоритм автовыбора

```
1. PE-парсер читает .exe → находит импорты d3d{9,10,11,12}.dll
2. Lookup в profile DB по hash:
   - если есть профиль → использовать выбор из него
   - если нет → дефолтный маршрут из таблицы
3. Запуск программы с первичным маршрутом
4. AI парсер логов следит за фейлами рендера:
   - если crash в Metal → переключиться на fallback
   - сохранить в профиль "DXVK works for this app"
5. Community-driven база (как ProtonDB)
```

## Реализация: переключатель DLL

В Wine bottle через `WINEDLLOVERRIDES` указываем какие DLL подменять:

```bash
# DXMT путь (DX 11)
WINEDLLOVERRIDES="d3d11=n;dxgi=n" wine app.exe

# DXVK путь (DX 9)
WINEDLLOVERRIDES="d3d9=n,b" wine app.exe

# Гибрид: DX 11 через DXMT, DX 9 через DXVK fallback
WINEDLLOVERRIDES="d3d11=n;d3d9=n,b" wine app.exe
```

`n` = native (наша подменённая DLL), `b` = builtin (Wine fallback).

## DLL установка в bottle

```
bottle/drive_c/windows/system32/
├── d3d9.dll      ← DXVK
├── d3d10.dll     ← DXVK
├── d3d11.dll     ← DXMT (приоритет) или DXVK
├── d3d12.dll     ← DXMT (приоритет) или vkd3d-proton
├── dxgi.dll      ← DXMT (или DXVK при fallback)
└── nvapi.dll     ← заглушка (DXVK ships)
```

## Преимущество над Crossover

- Crossover использует **только** DXMT (через интеграцию с D3DMetal-like flow)
- У нас выбор: DXMT для скорости, DXVK для совместимости
- AI следит и переключает автоматически — пользователь не думает
- Community-driven база растёт быстрее (open source модель)
