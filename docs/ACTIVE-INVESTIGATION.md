# ACTIVE INVESTIGATION — живое состояние (читать ПЕРВЫМ после compaction)

> Этот файл = единственный источник «где мы ПРЯМО СЕЙЧАС». Обновляется на КАЖДУЮ
> находку (включая опровержения), не только на правку кода. Список DISPROVED —
> ОБЯЗАТЕЛЕН: НЕ перепроверять заново то, что уже опровергнуто трассой.
> Координатор (Claude) держит его свежим, читая сессию Codex. Stale = хуже чем пусто.

Last update: 2026-05-23 09:10 (Claude, по сессии Codex)

## Текущий баг
Чёрные иконки тулбара Notepad++ (и вероятно folder/shell/tab — общий путь).
Цель: найти ТОЧКУ обнуления цвета и починить в правильном слое (HyperBridge ИЛИ
host cross-arch), без costылей в comctl32/user32.

## CONFIRMED (доказано, опираться)
- Bootstrap чист: gdi_init-order, NtMapViewOfSection ARM64 ABI, KUSER-адрес — DONE.
- Toolbar source DIB чёрный ДО ImageList (`add_dib_bits_raw colorful=0`, 32/32).
- Тулбар грузит иконки как ICON IDI_* (201..247, set2 301..347, dark 251..297),
  PNG-in-ICO RGBA, через LoadImage(IMAGE_ICON)+ImageList_AddIcon. НЕ RT_BITMAP.
- Probe (188 иконок): сырые PNG-байты ЦВЕТНЫЕ host-декодом (`RAW_PNG colorful=72`),
  но `LoadImageW(IMAGE_ICON)`→`hbmColor` ВЕСЬ ЧЁРНЫЙ.
- **LoadImageW = NATIVE-DIRECT import в HyperBridge** → исполняется НА НАТИВНОМ
  aarch64 user32, НЕ через транслятор. (ключевой факт окружения!)
- Нативная трасса: `load_png_after_read` И `frame_source_dib` уже ЦВЕТНЫЕ →
  PNG inflate/unfilter РАБОТАЕТ корректно.

## DISPROVED — НЕ перепроверять (опровергнуто трассой)
- ❌ present/flush / comctl32_v6 draw / alpha-mask / StretchDIBits-в-hdcImage — НЕ виноваты
  (probe: чёрный уже на add boundary).
- ❌ HyperBridge opcode в PNG-декоде (SHLD/SHRD/RCL/RCR гипотеза) — НЕВЕРНО:
  load_png_after_read цветной, и LoadImage всё равно native-direct (не транслятор).
- ❌ RT_BITMAP 0x05ed путь — это legacy TB_STANDARD, не дефолт.
- ❌ marlett_center — НЕ баг (артефакт захвата нативного macOS-titlebar → SKIP).

## CURRENT BOUNDARY (где сейчас ищем)
Цвет ЖИВ после PNG-декода (`frame_source_dib` цветной), но probe (x64 гость) читает
hbmColor ЧЁРНЫМ через GetIconInfo/GetDIBits. Потеря МЕЖДУ нативным `frame->color` и
битмапом, который видит x64-гость. Кандидаты:
  (a) `StretchDIBits` (стадия `frame_color_after_stretch`), либо
  (b) cross-arch возврат бит битмапа: нативный aarch64-продюсер → x64-гость
      (GetIconInfo/GetDIBits через границу). Это класс KUSER/NtMapViewOfSection.

## NEXT STEP (что делает Codex сейчас)
Трасса `frame->color` тем же GetDIBits, что и probe, ВНУТРИ нативного
`create_icon_frame` — чтобы разделить StretchDIBits-producer от cross-arch возврата.

## CROSS-ELEMENT (после фикса — критерий «не подгонка»)
По NPP-RENDER-PATH-ATLAS.md folder/shell/tab иконки идут тем же icon-путём → один
корневой фикс обязан перекрасить их разом. Проверить probe + on-screen после фикса.

## Опорные артефакты
- reports/phase-h/MILESTONE/ICON-LOAD-TINY-PROBE-20260523.md (probe verdict)
- reports/phase-h/MILESTONE/TOOLBAR-BLACK-ONSCREEN-PINPOINT-20260523.md (boundary)
- reports/engine-audit/NPP-RENDER-PATH-ATLAS.md (cross-element)
- reports/engine-audit/CROSS-ARCH-ABI-BOOTSTRAP-MAP.md (cross-arch класс)
- docs/ENGINE-CHANGE-JOURNAL.md (история правок)
