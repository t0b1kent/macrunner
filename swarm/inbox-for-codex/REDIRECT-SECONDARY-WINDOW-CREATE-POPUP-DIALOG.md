# РЕДИРЕКТ по фактам ручной приёмки: ломается создание popup/dialog, НЕ общий dispatch

## Факты от пользователя (живые клики на реальном окне) — VERIFIED
- Обычные toolbar-кнопки РЕАГИРУЮТ → ввод и command-dispatch РАБОТАЮТ. Значит гипотеза
  «UI-ввод не доходит / message dispatch сломан» — СНЯТЬ как корень (буквы печатаются,
  кнопки откликаются).
- **Бинокль (= команда Find / Find in Files) → приложение ЗАКРЫВАЕТСЯ.** Это open диалога
  Find крашит/выходит процесс. Свежего macOS .ips НЕТ → это Wine-handled exit (смотри
  stderr твоего --hold прогона, не DiagnosticReports).
- **Подменю/меню НИГДЕ не открываются** = popup-меню окно не создаётся.

## Объединяющий вывод (сужение)
Простые команды работают; ломается ВСЁ, что создаёт ВТОРИЧНОЕ окно:
- popup-меню (submenu) — не появляется,
- модальный/модельный диалог (Find) — крашит app.
→ Корень в **создании secondary-window**: TrackPopupMenu (win32u/menu.c show_popup/
init_popup) и DIALOG_CreateIndirect / NtUserCreateWindowEx для popup/dialog классов.
Это ровно зона reports/engine-audit/MENU-COMMAND-DISPATCH-AUDIT.md, теперь подтверждённая
вживую (не синтетика).

## Куда смотреть (твой слой, по фактам — не общий message.c)
1. **Find-крах (по названию = команда Find):** найди обработчик команды Find в пути
   создания диалога (comdlg32 / NPP Find dialog → DIALOG_CreateIndirect →
   NtUserCreateWindowEx). Прочитай stderr --hold прогона на момент клика по биноклю —
   там Wine-exception/exit reason. Это назовёт точку краха создания окна.
2. **Submenu не открывается:** show_popup/init_popup/track_menu (win32u/menu.c) —
   создаётся ли popup HWND и виден ли он. Возможно тот же secondary-window-create корень.
3. Гипотеза: popup и dialog оба не презентуются/не создаются на macOS pop-up/dialog
   window path (winemac.drv create_window для WS_POPUP / overlapped dialog), либо краш
   в их инициализации. Один корень может закрыть оба.

## Verify
На ЖИВОМ окне: клик File → popup открылся; клик бинокль → Find-диалог открылся БЕЗ
закрытия app. Это input-class, не render; icon-баг закрыт, не трогать.
