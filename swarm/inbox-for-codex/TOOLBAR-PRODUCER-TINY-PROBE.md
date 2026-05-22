# Toolbar producer — tiny probe вместо full smoke (root boundary готов)

Confidence: high (root boundary доказан трассой Codex 2026-05-23 05:06).
Symptom: toolbar-иконки чёрные on-screen; source DIB-биты уже чёрные ДО ImageList
(`add_dib_bits_raw colorful=0`, 32/32). Present/draw/alpha/StretchDIBits исключены.

## Root boundary (file:line)
- `dlls/comctl32/imagelist.c:285` — биты приходят из `GetDIBits(hdc, hbmImage, ...)`.
  Источник = `hbmImage`, создаёт сам Notepad++ (LoadImage/LoadBitmap/CreateDIBSection).
- Producer-вход host-стороны: `dlls/win32u/dib.c:887` NtGdiSetDIBitsToDeviceInternal,
  `:658` set_dib_bits, `:1471` NtGdiCreateDIBSection.

## Fix scope / next step (НЕ full smoke)
Сделать tiny x64 probe `reports/phase-h/MILESTONE/smoke-tools/bitmap_load_probe.c`:
EnumResourceNamesW(RT_BITMAP) по notepad++.exe (LOAD_LIBRARY_AS_DATAFILE) → LoadImageW →
GetObjectW → GetDIBits 32bpp → count colorful/nonwhite/black, stats до/после StretchDIBits.
Запуск секунды, без desktop/readiness. Полный бриф:
`reports/agent-prompts/CODEX-TINY-BITMAP-PROBE-NOT-FULL-SMOKE.txt`.

## Decision (слой за один прогон)
- resource_bits цветные + readback чёрный → host win32u DIB producer/readback фикс.
- resource_bits уже чёрные → guest/HyperBridge memory-copy семья (как REP MOVS), весь стек.
- probe цветной, окно чёрное → root позже в comctl32, не BITMAP_Load.
- тулбар = PNG/RCDATA (не RT_BITMAP) → producer другой (Gdiplus/декодер), перенацелить probe.

Не хардкодить resource 0x05ed (догадка). Не запускать /review пока probe бежит.
folder_icons/menu — ОТДЕЛЬНЫЙ корень (см. reports/engine-audit/MENU-COMMAND-DISPATCH-AUDIT.md).
