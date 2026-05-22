# NPP Toolbar Resource Findings (READ-ONLY research)

Краткий вывод: в актуальном Notepad++ тулбар собирается в `HIMAGELIST` через `ImageList_Create(ILC_COLOR32|ILC_MASK)` + `ImageList_AddIcon`, где иконки грузятся как `ICON`-ресурсы (`IDI_*`) через `DPIManagerV2::loadIcon`/`LoadImage`-путь для icon. Это **не** `RT_BITMAP 0x05ed` как основной путь для modern toolbar. Режим `TB_STANDARD` отдельно использует legacy `BITMAP` (`IDR_*`) через `LoadImage(..., IMAGE_BITMAP, ..., LR_LOADMAP3DCOLORS | LR_LOADTRANSPARENT)` и `TB_ADDBITMAP`.

| Вопрос | Ответ | Ссылка |
|---|---|---|
| Как создаётся toolbar ImageList? | `IconList::init()` вызывает `ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, ...)`; затем `ToolBarIcons::create/reInit` добавляет иконки через `ImageList_AddIcon` (через `IconList::addIcon`). | `PowerEditor/src/WinControls/ImageListSet/ImageListSet.cpp` (`IconList::init`, `IconList::addIcon`, `ToolBarIcons::reInit`) https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/WinControls/ImageListSet/ImageListSet.cpp |
| Как toolbar получает этот ImageList? | `ToolBar` выставляет листы через `TB_SETIMAGELIST`/`TB_SETDISABLEDIMAGELIST` (`setDefaultImageList*`, `setDisableImageList*`). | `PowerEditor/src/WinControls/ToolBar/ToolBar.h` (методы `setDefaultImageList*`) и `ToolBar.cpp` (ветка `_state != TB_STANDARD`) https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/WinControls/ToolBar/ToolBar.h ; https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/WinControls/ToolBar/ToolBar.cpp |
| Из какого ресурса грузятся иконки (modern path)? | Из `ICON` ресурсов `IDI_*` (regular/filled + dark variants), объявленных в `.rc`; ID макросы в `resource.h` (`IDI_NEW_ICON`=201 ... `IDI_VIEW_DOCLIST_ICON`=247; set2 `301..347`; dark `251..297`; dark2 `351..397`). | `PowerEditor/src/Notepad_plus.rc` (строки `IDI_* ICON "icons/..."`) и `PowerEditor/src/resource.h` (define IDI_*) https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/Notepad_plus.rc ; https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/resource.h |
| Есть ли RT_BITMAP/0x05ed как основной путь? | Для modern toolbar — нет подтверждения. Legacy режим `TB_STANDARD` использует `BITMAP` ресурсы `IDR_*` + `TB_ADDBITMAP` (`LoadImage(...IMAGE_BITMAP...)`). | `ToolBar.cpp` (ветка `_state == TB_STANDARD`) + `Notepad_plus.rc` (`IDR_* BITMAP`) https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/WinControls/ToolBar/ToolBar.cpp ; https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/Notepad_plus.rc |
| Наборы иконок (standard / Fluent / filled): что влияет? | Состояние toolbar: `TB_SMALL/TB_LARGE` (regular), `TB_SMALL2/TB_LARGE2` (filled), `TB_STANDARD` (legacy BMP). Переключение sets идёт через `_tbIconSet` в GUIConfig ToolBar (`small/large/small2/large2/standard`). | `Parameters.cpp` (`_tbIconSet`, сериализация `ToolBar` GUIConfig) + `ToolBar.cpp` выбор листов по `_state`. https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/Parameters.cpp ; https://github.com/notepad-plus-plus/notepad-plus-plus/blob/master/PowerEditor/src/WinControls/ToolBar/ToolBar.cpp |
| Формат/bpp | ImageList создаётся как `ILC_COLOR32 | ILC_MASK` (32bpp + mask). Иконки грузятся как `.ico` (ресурс `ICON`), не PNG/RCDATA в этом пути. Для legacy standard: `BITMAP` + transparent map color flags. | `ImageListSet.cpp`, `Notepad_plus.rc`, `ToolBar.cpp` |

## Что воспроизвести в probe (для Codex)

1. **Основной (modern) путь, который нужно бить первым:**
   - Создать `HIMAGELIST` как `ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 0, extra)`.
   - Загружать toolbar-иконки как `ICON`-ресурсы `IDI_*` (например `IDI_NEW_ICON`, `IDI_OPEN_ICON`, и т.д.; для set2 — `IDI_*2`; для dark — `IDI_*_DM`, `IDI_*_DM2`).
   - Добавлять через `ImageList_AddIcon`.
   - Привязать к toolbar через `TB_SETIMAGELIST` и `TB_SETDISABLEDIMAGELIST`.

2. **Fallback/legacy path (отдельно):**
   - Режим `TB_STANDARD`: `LoadImage(..., IMAGE_BITMAP, ..., LR_LOADMAP3DCOLORS | LR_LOADTRANSPARENT)` + `TB_ADDBITMAP` по `IDR_*`.
   - Это и есть путь, где фигурируют BMP-ресурсы; не подтверждено, что это ваш текущий runtime default.

3. **Про дефолт набора:**
   - По коду подтверждён механизм переключения (`small/large/small2/large2/standard`) через GUIConfig.
   - Жёсткий «всегда default = X» по одному фрагменту не фиксируется; корректно считать default зависимым от сохранённого GUIConfig/темы.
