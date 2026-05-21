# ICON LOWER-LAYER AUDIT (rescued from auditor session)

Дата: 2026-05-22
Источник: анализ агента-аудитора (Cline) — выводы сделаны, но его терминал завис на
heredoc-записи, поэтому отчёт спасён на диск вручную (Claude) из его сессии + сверка
с кодом. READ-ONLY, движок не правился.

## Выводы аудитора (confidence)
- **Data-flow reconstruction (resource→DIB→blit→surface): HIGH** — путь восстановлен.
- **Классификация наших патчей в win32u: HIGH** — diff vs upstream показывает, что
  патчи в win32u-файлах = **в основном трассировка/диагностика + ОДИН ключевой
  функциональный icon-fix** (остальное не меняет рендер). То есть win32u-патчи
  большей частью НЕ виноваты (как и предполагали — band-aids/трасса).
- **virtual.c как корень: MEDIUM** — «strongly plausible, но требует runtime-
  корреляции, чтобы доказать точную падающую транзакцию».

## Открытый домен корня (главный подозреваемый)
**`ntdll/unix/virtual.c` — memory mapping / protection state для guest memory READS.**
Гостевые x64-страницы помечены `VPROT_MACRUNNER_X64_GUEST` (0x1000). На пути mprotect
для guest-памяти снимается PROT_EXEC (`unix_prot &= ~PROT_EXEC`, ~строка 1962) — это
изоляция исполнения (корректно). НО гипотеза аудитора: взаимодействие protection-
state при ЧТЕНИИ guest-памяти (где лежит DIB-буфер иконки) может возвращать нули/
неверные данные при определённых переходах protection (это и есть «данные нулевые
ДО SIMD» из runtime-вердикта Codex 03:58).

## Связь с runtime-находкой Codex
- Codex (03:58): vector-memory load/store ТОЧНЫ (186/232 ok), данные иконки УЖЕ нули
  ДО SIMD → корень в GDI/DIB слое или памяти.
- Аудитор (static): win32u-патчи в основном безобидны; подозрение — virtual.c guest-
  memory read path.
- ВМЕСТЕ: проверить, попадает ли DIB-буфер иконки в view с `VPROT_MACRUNNER_X64_GUEST`,
  и возвращает ли чтение этого региона нули (а не реальные пиксели), особенно после
  смены protection (mprotect/COW/fault handler).

## Рекомендация Codex (runtime-корреляция)
1. В data-trace добавь: для адреса DIB-source-буфера — какой это view, есть ли
   VPROT_MACRUNNER_X64_GUEST, и совпадает ли прочитанное гостем с тем, что реально
   в host-памяти по тому же адресу.
2. Если гость читает нули, а в host-памяти данные есть → баг в guest-memory read/
   protection (virtual.c). Если данные нули и в host → проблема ещё раньше (fill не
   записал / resource decode).
3. ОДИН ключевой функциональный win32u icon-fix (из наших патчей) — найди его в diff
   vs upstream: возможно он и есть нужный (не выкидывай его при выравнивании к стоку).

## Что НЕ виновато (по аудиту)
- SIMD-арифметика (добавлена, не исполняется в blit).
- vector-memory ops MOVDQU/MOVDQA/PUNPCK (load/store доказанно точны).
- Большинство win32u-патчей (трасса/диагностика).
