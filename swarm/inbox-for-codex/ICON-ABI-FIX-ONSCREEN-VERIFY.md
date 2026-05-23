# Icon ABI family-fix — ОБЯЗАТЕЛЬНЫЙ on-screen verify (не закрыто без этого)

Confidence корня: high. Но статус = НЕ закрыто, пока нет реального окна.

## Что сделано (probe-level, доказано)
Root найден: GDI blit syscall family со stack-args на ARM64 требует
MACRUNNER_ARM64_MS_SYSCALL_ABI (иначе rop=0x0 → copy_rect чёрный). Покрыто:
NtGdiStretchDIBitsInternal/SetDIBitsToDeviceInternal, NtGdiBitBlt/StretchBlt,
NtGdiAlphaBlend/MaskBlt/TransparentBlt/PlgBlt. Tiny+cross-element probe: toolbar/
folder/shell/tab colorful=98, allblack=0. verify-build-freshness PASS.

## Почему ещё НЕ закрыто
Доказательство = probe + freshness ТОЛЬКО. On-screen визуал не прогонялся.
Правило проекта (AGENTS): verified = РЕАЛЬНОЕ окно на экране, НЕ probe/CG/прокси.
Нас уже жгло false-verified-by-proxy — поэтому без живого окна не объявляем зелёным.

## ЗАДАЧА: bounded on-screen visual gate
1. Один прогон реального Notepad++ через visual-gate (окно-кроп, не full-screen,
   VG_PROBE_TIMEOUT с запасом — окно появляется поздно).
2. Снять и проверить НА ЭКРАНЕ: toolbar-иконки цветные (не чёрные/серые), и если
   доступно — folder-иконки в Open/Save (по dispatch-аудиту диалог мог не открываться;
   если не открылся — это ОТДЕЛЬНЫЙ известный блокер, не регресс этого фикса).
3. Сверить активную DLL: тулбар рисует загруженный из WinSxS COMCTL32 (v6). Фикс в
   win32u syscall-слое — DLL-агностичен (v6 зовёт те же NtGdi*), но ПОДТВЕРДИ, что
   на экране именно цветные иконки, а не «probe зелёный / окно серое».
   ПРИМЕЧАНИЕ из ACTIVE-INVESTIGATION: comctl32_v6 без отдельного build target
   (__WINE_COMCTL32_VERSION=5), artifact синхронизирован по timestamp — это watch-item,
   если на экране иконки всё ещё не цветные при зелёном probe.

## Решение
- Иконки цветные НА ЭКРАНЕ → закрыть icon-баг: перенести итог в ENGINE-CHANGE-JOURNAL
  (ROOT-FIX, syscall-ABI family), очистить ACTIVE-INVESTIGATION под следующий пункт,
  обновить cross-element статус. Это первый по-настоящему зелёный визуальный элемент.
- Probe зелёный, но окно НЕ цветное → НЕ закрывать; новая граница = present/draw слой
  активной DLL (вернуться с этим фактом, записать в ACTIVE-INVESTIGATION DISPROVED/boundary).

Принцип: фикс в host-слое (win32u syscall ABI) — корректно. Трассы убраны (Codex сделал).
/review/full smoke — один раз, для on-screen proof.
