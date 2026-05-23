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
