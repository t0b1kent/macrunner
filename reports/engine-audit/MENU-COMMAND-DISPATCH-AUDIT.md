# Menu / command dispatch audit (folder_icons blocker)

Date: 2026-05-23
Автор: Claude (координатор). Cline отчитался «закончил», но файл не записал (снова
shell-quoting hang) — аудит выполнен вручную, read-only по движку.

## Краткий вывод
- Вся цепочка меню/команд/диалога — **ЧИСТЫЙ СТОК Wine** (нет MACRUNNER-маркеров в
  `win32u/menu.c`, `win32u/message.c`, `user32/menu.c`, `user32/dialog.c`,
  `comdlg32/filedlg.c`, `ntdll/unix/server.c`). Значит дефект НЕ в логике dispatch,
  а в нижнем слое доставки/представления.
- Хелпер шлёт команды через **cross-process `PostMessageA`** (не SendMessage) + синтетику
  `keybd_event` (`reports/phase-h/MILESTONE/smoke-tools/npp_open_saveas_helper.c`).
- **Сильнейшая гипотеза (объединяет с toolbar-black):** диалог/popup, возможно, СОЗДАЁТСЯ,
  но его window-surface НЕ презентуется на экран — тот же present/flush-класс, что
  toolbar-black S1. Тогда «dialog not detected» = артефакт детекции невидимого окна,
  а корень общий с toolbar-black. Это надо проверить ПЕРВЫМ (одна трасса разделит).

## Цепочки по file:line (engine/wine)

| звено | файл:строка | что делает | риск/гипотеза | что Codex проверит |
|---|---|---|---|---|
| Хелпер: cross-process команда | `reports/phase-h/MILESTONE/smoke-tools/npp_open_saveas_helper.c:161,166,182,187` | `PostMessageA(fm.main, NPPM_MENUCOMMAND/WM_COMMAND, ...)` в чужой процесс npp | Posted-сообщение должно пройти через wineserver в очередь потока npp; если не доставлено/не выбрано GetMessage — команда не исполняется | Трасса: доходит ли posted WM_COMMAND/NPPM до wndproc npp (или до comdlg32 entry) |
| Хелпер: синтетика ввода | `npp_open_saveas_helper.c:100-120,154-157` | `keybd_event` (Alt/Ctrl+O) + `AttachThreadInput`+`SetForegroundWindow` | Синтетика зависит от macOS foreground/focus и input-desktop; ранее «System Events keystroke denied» | Это путь B-риска (среда), НЕ полагаться на него для verdict; опираться на PostMessage-путь |
| Меню-бар: вход по Alt/мыши | `dlls/win32u/menu.c:1712,1772-1802` | `VK_MENU` mask, `WM_INITMENU/WM_INITMENUPOPUP` send | Если synthetic Alt не дошёл — меню-бар не входит в трекинг (среда, B) | Сначала проверить, входит ли поток в track вообще |
| popup create | `dlls/win32u/menu.c:3063 show_popup`, `:3325 init_popup`, `:4088 track_menu`, `:4498 NtUserTrackPopupMenuEx` | popup-окно создаётся ДО modal-loop трекинга | popup-окно может создаваться, но не быть видимым на экране (present) | Проверить: создаётся ли popup HWND и виден ли он on-screen |
| WM_COMMAND/NPPM → диалог | `dlls/comdlg32/filedlg.c:392,398 DialogBoxIndirectParamW/A`, `:852 CreateTemplateDialog` | GetOpenFileName → создаёт модальный file-диалог | comdlg32 сток; если entry достигается, а окна нет → present/surface | Поставить трассу на входе в `GetOpenFileNameW`/`DialogBoxIndirectParamW` |
| dialog window create + modal loop | `dlls/user32/dialog.c:444 DIALOG_CreateIndirect`, `:852/:871` (modal) | `NtUserCreateWindowEx` + собственный modal message-loop | Окно может создаться, но surface не презентуется (S1) ИЛИ modal-loop не качает сообщения | Сравнить: HWND валиден? IsWindowVisible? пиксели на экране? |
| present/flush surface (S1, общий с toolbar) | `dlls/win32u/` surface flush → `winemac.drv` | вывод нарисованного на видимую поверхность macOS | Если popup/dialog HWND создан, но не виден — тот же present-баг, что toolbar-black | Снять: создан HWND vs виден on-screen. Если создан-но-невидим = объединить с toolbar-black |

## A vs B — признаки (различить реальный сбой и артефакт детекции)
- **Сценарий A (реальный сбой dispatch/создания):** posted WM_COMMAND/NPPM НЕ доходит до
  wndproc npp, ИЛИ `GetOpenFileNameW`/`DialogBoxIndirectParamW` НЕ вызывается. Тогда корень
  в cross-process доставке (wineserver/queue) или в самой команде.
- **Сценарий B / B' (артефакт детекции):** `GetOpenFileNameW` ВЫЗЫВАЕТСЯ и HWND диалога
  СОЗДАЁТСЯ (валидный, IsWindow=TRUE), но окно НЕ видно на экране (present/surface) ИЛИ
  харнесс просто не находит его по заголовку/региону. Урок marlett: не верить «not detected»
  без проверки IsWindowVisible + реального пикселя. B' (создан-но-невидим) = ТОТ ЖЕ present-
  баг, что toolbar-black, и тогда оба блокера чинятся одним фиксом в host present/flush.
- Синтетика `keybd_event` (Alt/Ctrl+O) — всегда B-риск среды (focus/permission); НЕ
  использовать как единственный критерий, опираться на PostMessage-путь.

## Следующий шаг для Codex (по приоритету)
1. **P0 — разделить A/B одной трассой:** поставить временную трассу на
   `comdlg32/filedlg.c` вход `GetOpenFileNameW` и на `user32/dialog.c:444
   DIALOG_CreateIndirect` (HWND ret + IsWindowVisible). Послать NPPM_MENUCOMMAND
   PostMessage напрямую (без синтетики). Это сразу скажет: команда не дошла (A) /
   диалог создан, но невидим (B' = present-баг) / не создан (A в comdlg32).
2. **P0 — если B' (создан-невидим):** это ТОТ ЖЕ surface-present класс, что toolbar-black
   → чинить общий present/flush в host (win32u/winemac.drv), оба блокера закрываются вместе.
3. **P1 — если A (posted не доходит):** проверить cross-process post path
   (`win32u/message.c`, `ntdll/unix/server.c`) — доставку posted-сообщения в очередь
   чужого процесса на ARM64-хост форке.
4. Синтетику ввода (keybd_event/Alt) НЕ чинить как приоритет — это среда (macOS focus/
   permission), решается отдельно от рендер/dispatch ядра.

## Принцип
Verified = реальное окно/пиксель, не «detected». Цепочка стоковая — фикс в нижнем слое
(host present/flush ИЛИ cross-process delivery), НЕ патч menu/comdlg32. Если B' подтвердится
— объединить с toolbar-black (один present-фикс на два блокера).
