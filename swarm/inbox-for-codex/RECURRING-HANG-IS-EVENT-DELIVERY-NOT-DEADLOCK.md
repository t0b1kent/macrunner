# Повторяющийся hang = event-delivery/wakeup, НЕ deadlock (verified, 4 сэмпла)

## Контекст
Class-registration фикс работает: File-меню открывается ✓, Open-диалог + иконки папок ✓.
ОТДЕЛЬНАЯ проблема: NPP периодически становится unresponsive после нескольких действий
(в этот раз — до клика по биноклю; ранее — на new-tab). Работает (иконки/меню/диалог/
ввод), потом «висит».

## Verified (Claude, 4 независимых сэмпла живого процесса)
sample-npp-hang{,2,3,4}-*.txt — КАЖДЫЙ раз ОДНО И ТО ЖЕ:
- main-thread: `__wine_main → CFRunLoopRun → run_cocoa_app (winemac.so) →
  -[NSApplication run] → nextEventMatchingMask → mach_msg` — ждёт macOS-событие.
- rpcrt4 x64-thread: штатный `NtWaitForMultipleObjects → server_select` (idle RPC).
- остальные — idle workqueue.
НЕТ: CPU-spin, mutex-deadlock, застрявшего server-вызова. Приложение полностью PARKED
в нормальном GUI-ожидании.

ВЫВОД: это НЕ код-дедлок (его было бы видно в стеке). Это класс **доставки/пробуждения
UI-событий**: app паркуется и не просыпается на ввод. Stack-sampling исчерпан — он не
локализует это (parked == и hung, и normal-idle выглядят одинаково).

## Как локализовать (нужен event-trace, не сэмпл)
1. winemac.drv event source: когда app в «hung» состоянии и пользователь кликает —
   ПРИХОДИТ ли macdrv mouse/key event в Wine? Трасса на macdrv event delivery +
   CFRunLoop source wakeup. Если событие НЕ приходит → event-source/focus заснул.
2. Фокус/key-window: теряет ли NPP-окно key/active статус (тогда macOS перестаёт слать
   ему события). Проверить window activation path в winemac.drv.
3. Быстрый разделитель (попросить пользователя): когда «зависло» — кликнуть в другое
   приложение и обратно в NPP (или по title bar). Если ПРОСЫПАЕТСЯ → это focus/event-
   source wakeup, а не настоящий freeze.

## Приоритет
После закрытия Find-половины. Это последний нюанс интерактивности; iconы/меню/диалоги
уже работают. Не code-deadlock — не ищи mutex; ищи event-delivery/wakeup в winemac.drv.

## UPDATE 2026-05-23 20:07 (Claude) — консолидация + ПОПРАВКА
Class-registration фикс закрыл меню/Find/Open (verified). Остался ТОЛЬКО recurring freeze.
Новые verified-факты (важно — корректируют направление):
- **Воспроизводится и через ПРОДУКТОВЫЙ путь** (запуск NPP из MacRunner.app), не только dev-скрипт.
- **5+ сэмплов: ВСЕ wine-процессы на 0.0% CPU** (npp+wineserver+services+explorer) — чистый блок.
- **ПОПРАВКА (user-verified): переключение фокуса/приложения НЕ будит окно. Только force-quit.**
  → Значит это НЕ «окно заснуло в ожидании фокуса». `set frontmost to true` / focus-toggle
  это НЕ починит — не трать на это циклы (этот угол отработан и отрицателен).
- Стек всегда parked: main в winemac `nextEvent→mach_msg`, rpcrt4 в server_select. Ни spin,
  ни mutex-frame не видно.

Это true freeze при 0% CPU, не пробуждаемый фокусом. Stack-sampling и focus-toggle
исчерпаны — НЕ повторять. Декисив — ТРАССА, а не сэмпл:
1. Инструментируй winemac.drv event delivery: когда окно «заморожено» и приходит реальный
   клик/событие от macOS — доходит ли macdrv event до Wine queue, просыпается ли CFRunLoop
   source, что происходит в обработчике события (доходит ли до SendMessage в Wine-окно и
   возвращается ли). Лог на каждый этап.
2. Кандидат: главный поток просыпается на событие, заходит в Wine-обработку (SendMessage/
   server round-trip во время event handling) и не возвращается, а сэмпл ловит его обратно
   в nextEvent между попытками — проверь, делает ли main thread server-call при обработке
   события в замороженном состоянии (трасса входа/выхода, не сэмпл).
Не focus, не spin, не mutex-frame. Нужна именно event-path трасса в момент freeze.
