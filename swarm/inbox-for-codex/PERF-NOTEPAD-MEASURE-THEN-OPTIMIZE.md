# PERF: «чтобы Notepad летал» — ИЗМЕРИТЬ доминанту, потом оптимизировать (одну)

## Статус: freeze ОТЛОЖЕН, perf АКТИВЕН
Freeze (race в real-event delivery) пользователь вручную НЕ смог поймать сейчас → не
закрыт, но deprioritized. Заметка RECURRING-HANG остаётся; если freeze всплывёт во время
perf-тестов — лови по той инструкции. СЕЙЧАС: performance (NPP это GDI/CPU, тормоза =
накладные трансляции HyperBridge, НЕ графика).

## Готовая карта (Cline, read-only) — опирайся на неё
reports/engine-audit/HYPERBRIDGE-PERF-SURFACE-MAP.md — кандидаты издержек с file:line +
колонка «как измерить» + порядок замеров. Это КАНДИДАТЫ, не диагноз.

## ДИСЦИПЛИНА: сначала измерить, потом чинить (не угадывать)
НЕ оптимизируй по интуиции. Профилируй на реальных NPP-сценариях (открытие меню, открытие
диалога, набор текста), найди ДОМИНИРУЮЩИЙ компонент, оптимизируй ЕГО ОДИН, переизмерь.

## Порядок (из карты Cline)
1. Phase-split профиль (без оптимизаций) на menu/dialog/typing: доля времени по backend
   (interpreter vs JIT/AOT, hb_runtime.c:266-284) и по фазам dispatcher-loop
   (find_block, cache-hit exec, compile-on-miss, helper-calls). Это назовёт доминанту.
2. Cache: cold vs warm start; hit/miss + latency для in-memory block cache и persistent
   AOT cache (hb_aot_cache.c lookup O(N) + put/fsync/rename).
3. Helper-boundary профиль: топ helper-групп по count+time внутри JIT-блоков
   (hb_arm64_codegen.c emit_call_helper) — доминируют ли call-границы на коротких UI-блоках.
4. Opcode fallback: счётчик unsupported_opcode_hits по family+callsite; коррелируют ли
   частые fallback с NPP-действиями (interpreter медленнее).
5. Syscall boundary: ПРИМЕЧАНИЕ — Cline не нашёл файл, но он ЕСТЬ:
   dlls/ntdll/unix/macrunner_hb.c (call_direct_native_target / import thunk). Поставь
   per-call latency + callsite histogram там.
6. present/flush (win32u→winemac): лёгкая трасса частоты/latency, без углубления.

## Сильные кандидаты (ОЖИДАЙ, но ПОДТВЕРДИ замером)
- find_block O(N) линейный (hb_runtime.c:75) на каждом переходе блоков → hash-индекс.
- mprotect RW↔RX churn на мелких блоках (hb_jit.c:86-109) → реже переключать/batch.
- helper-call на каждую IR-операцию вместо inline → inline top-N горячих helper-path.
- доля interpreter в горячем пути → JIT-покрытие/закрыть hot fallback опкоды.
Любой из них может оказаться НЕ доминантой — решает п.1 профиль.

## Цель и verify
NPP ощущается отзывчиво: меню/окна открываются быстро, ввод без лагов (субъективно «летает»)
+ объективное снижение времени в доминирующем компоненте (до/после метрика). Корректность
не ломать, стоковый рендер не трогать, иконки/диалоги остаются рабочими (регресс-чек).
Обновляй ACTIVE-INVESTIGATION: текущая задача = perf, метрики до/после.
