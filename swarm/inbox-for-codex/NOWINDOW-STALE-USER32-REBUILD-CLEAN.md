# No-window = битый x86_64 user32.dll (НЕ иконки, НЕ regress ABI-фикса)

## Диагноз (Claude, по прогону npp-x64-20260523-102306)
Окно НЕ появилось, потому что app аварийно завершился:
```
wine: Call ... to unimplemented function user32.dll.BroadcastSystemMessageW, aborting
wine: Call ... to unimplemented function shell32.dll.SHGetFolderPathW, aborting
```
- Обе функции ЕСТЬ в спеках (user32.spec:243 stdcall BroadcastSystemMessageW;
  shell32.spec:385 SHGetFolderPathW) — НЕ стабы. Источник чист.
- Установленные DLL рассинхронены по арке:
  - dist-pure-arm64/lib/wine/x86_64-windows/user32.dll = **09:02** (СТАРАЯ трассировочная
    сборка, которую ставил пока думал что LoadImage через транслятор).
  - .../aarch64-windows/user32.dll = 10:01 (пересобрал после).
- x64-приложение грузит x86_64 (PE) user32.dll = СТАРЫЙ/битый → missing export → abort.
ВЫВОД: это build/install регрессия от итеративных пересборок, НЕ icon ABI-фикс,
НЕ доказательство серых иконок. Icon-фикс не трогать.

## ОБНОВЛЕНО: verify-build-freshness теперь ловит рассинхрон арок
Добавлена авто-проверка `arch_pair_*`. Текущий прогон уже FAIL:
- `arch_pair_user32 skew 3524s` (x86_64 stale) ← причина no-window.
- `arch_pair_gdi32 skew 8314s` (x86_64 stale) ← ЛАТЕНТНАЯ мина, тоже переустанови.
Чистый PASS гейта = обе арки согласованы. Гони гейт ПОСЛЕ install, до запуска окна.

## ЗАДАЧА: чистый rebuild+install, поднять окно, потом crop
1. Чистая пересборка user32, **gdi32**, shell32 — ОБЕ арки (aarch64-windows И
   x86_64-windows) из текущего исходника, БЕЗ остаточных trace-хуков. make install в
   dist-pure-arm64. (gdi32 добавлен — гейт показал его x86_64 stale.)
2. Подтвердить экспорты в установленном x86_64 user32.dll (winedump exports / nm /
   spec-check): BroadcastSystemMessageW, плюс что иконочные NtGdi* fixes на месте.
   shell32: SHGetFolderPathW присутствует.
3. verify-build-freshness.sh PASS; проверить что нет рассинхрона timestamp между арками.
4. Перезапустить bounded прогон → подтвердить, что процесс НЕ абортит и CGWindow окна
   создаётся (окно появляется поздно ~80s — VG_PROBE_TIMEOUT с запасом).
5. Только после видимого окна — один on-screen toolbar crop: иконки цветные?

## Решение
- Окно поднялось + иконки цветные НА ЭКРАНЕ → закрыть icon-баг (ROOT-FIX в журнал),
  очистить ACTIVE-INVESTIGATION, cross-element статус.
- Окно поднялось, иконки серые/чёрные → новая граница present/draw активной DLL
  (записать в ACTIVE-INVESTIGATION, НЕ закрывать).
- Окно всё ещё не поднимается после чистой сборки → отдельный pre-visual bootstrap
  blocker, диагностировать (не путать с icon-фиксом).

Принцип: verified = реальное окно. Icon ABI-фикс корня сохранить. Обнови
ACTIVE-INVESTIGATION этим диагнозом (стейл x86_64 user32) и результатом.
