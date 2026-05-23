# НОВЫЙ баг (ручная приёмка): UI-ввод не доходит — меню не открывается, new-tab unresponsive

## Статус icon-бага: ЗАКРЫТ ✅ (не трогать)
Пользователь подтвердил на живом окне: toolbar-иконки ЦВЕТНЫЕ. Crop colorful=13304. Done.

## Новый баг (verified из сэмпла живого процесса)
На чистом интерактивном окне пользователь нашёл:
- меню File/Edit/… НЕ открываются по клику;
- «новая вкладка» → окно «зависает» (unresponsive).

Сэмпл живого NPP (`reports/phase-h/MILESTONE/sample-npp-hang-newtab-154227.txt`):
- ВСЕ потоки parked (по 2595/2595 сэмплов в одной точке) → НЕ CPU-spin, НЕ mutex-deadlock.
- main-thread: `__wine_main → CFRunLoopRun → run_cocoa_app (winemac.so) → -[NSApplication
  run] → nextEventMatchingMask → mach_msg` — ждёт следующее macOS-событие.
- rpcrt4 x64-thread: штатный `NtWaitForMultipleObjects → server_select` wait.
ВЫВОД (verified, без догадок о причине): приложение полностью idle/неотзывчиво к вводу.
main-thread (он же качает Wine-сообщения через winemac.drv) спит в ожидании события.
Значит UI-ввод (мышь/клава) НЕ доходит до Wine message queue, либо меню/tab не стартуют.
Это input/event-delivery, НЕ deadlock и НЕ render.

## Оба симптома — вероятно ОДИН корень
«Меню не открывается» + «new-tab unresponsive» = доставка UI-событий
macOS → winemac.drv → Wine message queue / обработка кликов в menubar. См. уже готовый
аудит `reports/engine-audit/MENU-COMMAND-DISPATCH-AUDIT.md` (TrackPopupMenu/win32u menu.c
+ message dispatch) — он теперь подтверждён ВЖИВУЮ на реальных кликах, не на синтетике.

## Куда смотреть (твой слой)
1. winemac.drv: доставка mouse/key событий из Cocoa в Wine message queue
   (macdrv events → __wine driver event → SendMessage/queue). Доходит ли click до окна?
2. Если событие доходит, но menubar не входит в track — путь NC hit-test / WM_NCLBUTTONDOWN
   / TrackPopupMenu (win32u/menu.c) на реальном клике.
3. Трасса на живом окне: приходит ли WM_LBUTTONDOWN/WM_NCLBUTTONDOWN в wndproc NPP при
   клике в меню — это разделит «событие не доставлено» vs «доставлено, но menu не открылся».

## Verify
Закрыть только когда на ЖИВОМ окне: клик по File открывает popup, new-tab создаёт вкладку
без зависания. verified = реальное окно/действие, не синтетика. Обнови ACTIVE-INVESTIGATION:
icon-баг closed, новый active bug = UI input/menu dispatch.
