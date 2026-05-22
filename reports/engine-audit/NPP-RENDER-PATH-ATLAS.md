# NPP Render Path Atlas (bounded, READ-ONLY)

Дата: 2026-05-23  
Режим: bounded atlas по 6 элементам из prompt, без engine-правок.

## Таблица: элемент → источник → путь Wine → примитив → fork-touch → probe

| Элемент | Источник данных | Путь Wine (file:line) | Критичный примитив для HyperBridge | Форк трогал? | Точка probe (вместо тяжёлого smoke) |
|---|---|---|---|---|---|
| 1) Toolbar icons (producer DIB) | Notepad++ modern toolbar: `ICON` (`IDI_*`) → `ImageList_Create(ILC_COLOR32\|ILC_MASK)` + `ImageList_AddIcon` (см. `NPP-TOOLBAR-RESOURCE-FINDINGS.md`) | `comctl32/imagelist.c:285` (`GetDIBits` в add boundary), draw path `:1572`, mask branch `:1691-1713`, final blit `:1765-1777` | `GetDIBits`, `BitBlt(SRCAND/SRCPAINT/SRCCOPY)`, temp DIB compose | **Да**: журнал 2026-05-23 05:06 (diagnostic boundary в `imagelist.c`) | P1: boundary dump «add-time DIB bits» vs stored slot; P2: post-composite temp bitmap before final `BitBlt` |
| 2) Folder/file icons (Open/Save dialog) | Shell icon path (`SHGetFileInfoW`/sys imagelist), ICO resources (часто 4/8bpp indexed) | `shell32/brsfolder.c:182,285,353` → `shell32/shell32_main.c:154` → `shell32/iconcache.c:504,515` → `user32/exticon.c:249` → `user32/cursoricon.c:1250,982,768,879,929` → `comctl32/imagelist.c:2772,2819,1572`; fallback `cursoricon.c:2233,2454,2192` | `StretchDIBits` (indexed→32bpp color fill), `SetDIBits`/DIB fill family, `GetDIBits` + imagelist blit chain | **Да**: в журнале есть серия по `cursoricon.c`/imagelist; baseline затем откатывался к stock для локализации | P1: return/value+pixel probe в `cursoricon.c:879` до/после color-plane fill; P2: imagelist stored-slot colorful/nonzero before draw |
| 3) Scrollbar (thumb/arrows/selected track) | Классические user32/win32u GDI pattern/ROP draw (не icon resource) | Подтверждённые точки из журнала: `win32u/dibdrv/primitives.c:873` (pattern path), `user32/scroll.c:148` (selected-page track paint) | `PatBlt/PATCOPY`, pattern copy/ROP на 32bpp target | **Да**: журнал 2026-05-21 17:46 (`scroll.c:148` PATCOPY fix attempt) + revert/experiments around `primitives.c:873` | P1: probe на selected-track branch (`user32/scroll.c`), фиксировать ROP и цвета до/после; P2: dibdrv pattern row copy sanity |
| 4) Tab control | Owner-draw (`TCS_OWNERDRAWFIXED`), close/dot glyphs через `ImageList_Draw(...ILD_TRANSPARENT)` | `comctl32/tab.c:1548` (tab paint), owner-draw send около `:1707`; `comctl32/imagelist.c:1572`, transparent mask branch `:1691-1713`, final blit `:1765-1777`; add-masked path `:780-839` (`NOTSRCAND` around `:827`) | `BitBlt(SRCAND/SRCPAINT/SRCCOPY)`, mask-based compose, temporary black-filled bitmap risk | **Да**: журнал фиксирует много итераций вокруг transparent-mask path `imagelist.c` | P1: probe temp-bitmap init color + mask branch taken/not; P2: destination pixels after final blit for tab-close rect |
| 5) Status bar / bottom bands | **TODO (bounded):** точный source map не подтверждён в предоставленных файлах | **TODO:** нет верифицированных file:line в текущем наборе READ-ONLY источников | Вероятно общие GDI fill/blit primitive family, но без подтверждённых line-map | Не подтверждено | P1: сначала собрать line-map отдельно (status class paint entry + final blit), затем ставить probes |
| 6) Menu popup + dialog create | Меню/диалог dispatch path (stock) | **Ссылкой на готовый аудит (без дублирования):** `reports/engine-audit/MENU-COMMAND-DISPATCH-AUDIT.md` с ключевыми точками `win32u/menu.c:3063,3325,4088,4498`, `comdlg32/filedlg.c:392,398,852`, `user32/dialog.c:444,852,871` | Message dispatch + dialog create/present boundary; не icon decode | По аудиту: path stock, MACRUNNER markers в этих узлах не выявлены | P1: A/B split probe — вход в `GetOpenFileNameW` + `DIALOG_CreateIndirect` (HWND/visible) |

## Общие примитивы (семейные узлы, чинят сразу несколько элементов)

1. **DIB ingest/extract family**: `GetDIBits`, `SetDIBits`, `StretchDIBits`  
   - Общие для toolbar/folder-file/icon pipelines.  
2. **Mask/ROP blit family**: `BitBlt` с `SRCAND/SRCPAINT/SRCCOPY`, плюс `PATCOPY`/pattern path  
   - Общие для toolbar/tab/scrollbar.  
3. **ImageList internal compose family** (`comctl32/imagelist.c`)  
   - Одна ошибка на add/draw boundary даёт мульти-симптомы: toolbar + tab + shell list/tree icons.  
4. **Present/visibility boundary family** (menu/dialog audit A vs B')  
   - Если HWND создан, но не виден, это общий host present/flush класс, а не конкретный widget.

## Что probe проверит вместо трассы (приоритет)

1. **P0: Source-boundary vs present-boundary split**  
   - Toolbar/folder path: colorful/nonzero ли пиксели уже на add/store boundary (`GetDIBits`/slot).  
   - Menu/dialog path: вызывается ли `GetOpenFileNameW` и создаётся ли HWND (`DIALOG_CreateIndirect`) + visible.

2. **P1: ImageList transparent-mask branch integrity**  
   - Для tab/toolbar: taken-branch, temp bitmap init, post-mask composite, финальный dst sample.

3. **P1: Indexed icon decode fill integrity**  
   - Для folder/file: `cursoricon.c:879` (indexed→32bpp) return/pixel sanity до вставки в imagelist.

4. **P2: Scrollbar pattern path sanity**  
   - Зафиксировать selected-track branch и фактический ROP/цвет до и после `PATCOPY`/pattern copy.

## Bounded TODO (осознанно оставлено)

- **Status bar / bottom bands**: в текущем предоставленном наборе источников нет подтверждённого точного line-map уровня, сопоставимого с пунктами 1/2/4/6.  
- Следующий bounded шаг: отдельный read-only mini-audit только по status/bands (paint entry → final blit), затем дописать atlas line-map.
