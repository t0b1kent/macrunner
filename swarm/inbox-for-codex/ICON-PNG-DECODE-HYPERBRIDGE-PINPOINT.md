# ICON PNG-decode — HyperBridge pinpoint (G-случай доказан)

Confidence: very high. Probe `reports/phase-h/MILESTONE/ICON-LOAD-TINY-PROBE-20260523.md`.

## Доказано (probe, 188 иконок)
- Источник = PNG-in-ICO, colorType=6 (RGBA), bitDepth=8. Host-декод PNG-байт ЦВЕТНОЙ
  (`RAW_PNG_ICON colorful=72 black=0`).
- `LoadImageW(IMAGE_ICON)` → `hbmColor` ВЕСЬ ЧЁРНЫЙ (`icon_color_bits colorful=0 black=256`).
- imagelist_after_add чёрный (унаследовано). Present/comctl32-draw/host НЕ виноваты.
- ВЫВОД: цвет теряется ВНУТРИ декода PNG-иконки под HyperBridge. Это transl­ator bug.

## Задача: pinpoint опкода (depth, твоя)
Найти ТОЧКУ, где colorful→black в пути декода PNG-иконки и назвать source/dest PC+opcode.
Путь: LoadImage(IMAGE_ICON) → детект PNG → PNG decode (inflate + unfilter) → 32bpp hbmColor.
Кандидатные файлы Wine: dlls/user32/cursoricon.c (PNG-ветка create_icon_from_resource),
dlls/windowscodecs/* (PNG decoder / zlib inflate), возможно dlls/user32 → windowscodecs COM.

ПОРЯДОК (follow the fault, НЕ гипотезу):
1. Сузь до конкретной функции декода, где выход чёрный при цветном входе (трасса до/после
   на буфере: вход compressed PNG / выход RGBA scanlines / финальный hbmColor fill).
2. Когда дойдёшь до горячего цикла декода — определи КОНКРЕТНЫЙ опкод, который даёт 0
   (single-step / PC+bytes / source-vs-dest).
3. ПОДСКАЗКА (но подтверди фактом, не добавляй по догадке — урок PACK): декод PNG/zlib —
   это shift/bit-тяжёлый код (inflate bit-reader, unfilter Paeth/Sub/Up/Average). По
   reports/engine-audit/HYPERBRIDGE-OPCODE-COVERAGE-ATLAS.md как MISSING помечены
   SHLD/SHRD, RCL/RCR — проверь их покрытие ПЕРВЫМИ среди подозреваемых. Но решает трасса.
4. Найдя семью — добей весь стек: decode→IR→lift→interp→jit→tests→oracle/FileCheck.

## Verify (дёшево, без full smoke)
- Перезапусти tiny probe (bitmap_load_probe.c) → icon_color_bits должен стать colorful>0.
- Затем cross-element (см. NPP-RENDER-PATH-ATLAS.md): тот же фикс обязан перекрасить
  folder-иконки и tab — это критерий «семейный фикс, не подгонка».
- Полный NPP smoke — ОДИН раз в конце для on-screen подтверждения.

## Принцип
Фикс в HyperBridge (опкод/семья), НЕ костыль в user32/windowscodecs/comctl32. Временные
трассы убрать после локализации. Журнал + git diff. /review не запускать пока probe/трасса бежит.
