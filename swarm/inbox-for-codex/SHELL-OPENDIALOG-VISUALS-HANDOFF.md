# Open-dialog shell visuals: namespace pane пустая + иконки списка отсутствуют

Статус: active bug (installer E2E acceptance). Cline готовит глубокую карту
reports/engine-audit/SHELL-OPENDIALOG-VISUALS-MAP.md (file:line + fork-vs-stock +
«наш баг vs Wine-gap»). Бери ПОСЛЕ карты Cline и текущего JIT/installer.

## Что чинить (по приоритету, после карты Cline)
1. ИКОНКИ списка (вероятно чинибельно): shell32/shlview.c Shell_GetImageLists /
   I_IMAGECALLBACK / LVN_GETDISPINFO / SHMapPIDLToSystemImageListIndex; iconcache.c.
   Трасса: валиден ли index, не пуст ли system imagelist, вызван ли draw. Связь с уже
   чинёным icon/PNG-декодом — проверить. Фикс в правильном слое.
2. NAMESPACE-панель (Favorites/Desktop tree): сначала ПОДТВЕРДИ по карте Cline —
   это Wine-gap (INameSpaceTreeControl2 не реализован и в стоке) или наша регрессия.
   Если Wine-gap → НЕ городить — отдельное решение/низкий приоритет (не блокер для
   функционального E2E; диалог работает, файлы видны). Если регрессия → чинить.

## Протокол приёмки
- Verify ТОЛЬКО на реальном продуктовом Open-диалоге (через MacRunner.app), не прокси:
  иконки строк видны на экране; namespace-панель либо с записями (если чинибельно),
  либо явно классифицирована как Wine-gap.
- Не помечать installer E2E complete, пока иконки списка не появятся onscreen (namespace
  pane — по классификации Cline).
- Обнови ACTIVE-INVESTIGATION + ENGINE-CHANGE-JOURNAL.

## После этого
Вернуться к freeze (P0) на свежем РЕАЛЬНОМ ручном репро пользователя (карта wake/drain
готова: EVENT-WAKE-DRAIN-PATH-MAP.md — логика=сток, искать runtime/race/thread-liveness).
