# Toolbar producer — tiny probe вместо full smoke (root boundary готов)

Confidence: high (root boundary доказан трассой Codex 2026-05-23 05:06).
Symptom: toolbar-иконки чёрные on-screen; source DIB-биты уже чёрные ДО ImageList
(`add_dib_bits_raw colorful=0`, 32/32). Present/draw/alpha/StretchDIBits исключены.

## Root boundary (file:line)
- `dlls/comctl32/imagelist.c:285` — биты приходят из `GetDIBits(hdc, hbmImage, ...)`.
  Источник = `hbmImage`, создаёт сам Notepad++ (LoadImage/LoadBitmap/CreateDIBSection).
- Producer-вход host-стороны: `dlls/win32u/dib.c:887` NtGdiSetDIBitsToDeviceInternal,
  `:658` set_dib_bits, `:1471` NtGdiCreateDIBSection.

## Producer путь УТОЧНЁН (research Cline, NPP-TOOLBAR-RESOURCE-FINDINGS.md)
Модерн-тулбар NPP грузит иконки как ICON-ресурсы IDI_* (32bpp, ILC_COLOR32), НЕ RT_BITMAP:
  ImageList_Create(cx,cy,ILC_COLOR32|ILC_MASK) + LoadImage(IMAGE_ICON, IDI_*) +
  ImageList_AddIcon + TB_SETIMAGELIST.
Resource id: IDI_NEW_ICON=201 ... IDI_VIEW_DOCLIST_ICON=247 (set2 301-347, dark 251-297).
RT_BITMAP 0x05ed = legacy TB_STANDARD, НЕ дефолтный путь. БЕЙ ICON-путь первым.

## Fix scope / next step (НЕ full smoke)
tiny x64 probe `reports/phase-h/MILESTONE/smoke-tools/bitmap_load_probe.c`:
LoadLibraryExW(notepad++.exe, LOAD_LIBRARY_AS_DATAFILE) → LoadImageW(IMAGE_ICON, IDI_* 201..247)
→ GetIconInfo → GetDIBits(hbmColor) [stage icon_color_bits] → ImageList_Create(ILC_COLOR32|
ILC_MASK)+ImageList_AddIcon → GetDIBits [stage imagelist_after_add]. Секунды, без desktop.
Полный бриф: `reports/agent-prompts/CODEX-TINY-BITMAP-PROBE-NOT-FULL-SMOKE.txt`.

## Decision (слой за один прогон)
- icon_color_bits цветные + imagelist_after_add чёрный → host comctl32/win32u readback фикс.
- icon_color_bits уже чёрные → guest/HyperBridge memory-copy семья (как REP MOVS), весь стек.
- обе стадии цветные, окно чёрное → root позже (draw/themed v6/present).

Не хардкодить 0x05ed. Не запускать /review пока probe бежит.

## ПОСЛЕ ФИКСА: проверь cross-element (семейный каскад)
По reports/engine-audit/NPP-RENDER-PATH-ATLAS.md эти элементы сидят на ОБЩИХ примитивах:
- DIB ingest/extract (GetDIBits/SetDIBits/StretchDIBits) — toolbar + folder/file + icon.
- Mask/ROP blit (BitBlt SRCAND/SRCPAINT/SRCCOPY + PATCOPY) — toolbar + tab + scrollbar.
- ImageList compose (comctl32/imagelist.c) — toolbar + tab + shell list/tree icons.
Когда probe назовёт слой и ты починишь — НЕ останавливайся на toolbar: проверь, что тот же
фикс закрыл folder-иконки (cursoricon.c:879 indexed→32bpp) и tab (imagelist transparent-mask
branch). Если фикс в HyperBridge/host общего примитива — он обязан закрыть семью разом.
Это критерий «не подгонка»: один корень → несколько зелёных элементов одним фиксом.
folder_icons/menu — ОТДЕЛЬНЫЙ корень (см. reports/engine-audit/MENU-COMMAND-DISPATCH-AUDIT.md).
