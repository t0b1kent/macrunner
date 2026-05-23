# HYPERBRIDGE PERF SURFACE MAP (for Codex measurement)

## Краткий вывод
По коду основные кандидаты издержек — это граница `JIT↔interpreter`/dispatch в runtime, поведение translation/AOT cache на cold/warm пути, и частые helper-call границы внутри JIT-блоков. Это **гипотезы-кандидаты**, а не диагноз: узкое место должен подтвердить Codex измерением на NPP-сценариях.

## Карта perf-поверхности (кандидаты + точки замера)

| Компонент | Файл:строка | Что делает | Почему может быть дорого | Как Codex измерит (где поставить счётчик/таймер/трассу) | Гипотеза оптимизации |
|---|---|---|---|---|---|
| Backend routing (JIT vs interpreter) | `engine/hyperbridge/src/hb_runtime.c:266-284` | Выбор backend в `hb_runtime_run` (`HB_BACKEND_INTERP` vs `HB_BACKEND_JIT/AOT`) | Разный steady-state cost путей; важна доля времени в backend на реальном UI loop | Счётчики входов по backend + wall-time per-run вокруг `hb_interpreter_run` и `hb_jit_runtime_run`; отдельно cold/warm | Уменьшение доли interpreter на горячих участках после замера/покрытия |
| Runtime dispatcher loop | `engine/hyperbridge/src/hb_runtime.c:120-263` | Основной цикл исполнения JIT runtime: поиск блока, compile/cache-hit, переходы | Частый re-entry в loop, branch-heavy path, fault/limit checks в каждом витке | Таймеры фаз внутри цикла: `find_block`, cache-hit exec, compile path, post-block control-flow; счётчик итераций loop на guest block | Chaining/superblock после подтверждения high dispatcher overhead |
| Поиск блока по адресу | `engine/hyperbridge/src/hb_runtime.c:75-81` + вызовы `132`, `230` | Линейный `find_block` по `cfg->block_count` | O(N) lookup на каждый переход между блоками | Счётчик вызовов `find_block`, гистограмма `cfg->block_count`, таймер суммарного времени lookup | Индекс guest_addr→block (hash/map) при подтверждении затрат |
| In-memory block cache hit/miss | `engine/hyperbridge/src/hb_runtime.c:19-27`, `30-51`, `147-154`, `199-206` | Probe/insert в fixed-size `HB_BLOCK_CACHE_SIZE` + exec cached code | Miss-rate/пробинг и ограничение ёмкости могут давать лишние compile/dispatch | Метрики: hit/miss, средняя длина probe, число `cache full` ситуаций, latency hit-path vs miss-path | Тюнинг размера/eviction/chaining только после метрик |
| JIT compile-on-miss path | `engine/hyperbridge/src/hb_runtime.c:155-197` | Создание codegen buffer, codegen блока, копирование в JIT buffer, commit RX | Miss-path включает allocation + codegen + memcpy + memory-protection переключения | Разбить miss-path на таймеры: `codegen`, `jit_buffer_make_writable`, `memcpy`, `jit_buffer_commit`; отдельно first-hit vs later | Снижение стоимости cold compile и batched commit по данным |
| JIT memory protection boundary | `engine/hyperbridge/src/hb_jit.c:86-98`, `100-109` | `mprotect` RW↔RX и verify перед exec | Частые `mprotect`/verify могут быть заметны на мелких блоках | Счётчик/таймер вызовов `hb_jit_buffer_make_writable` и `hb_jit_buffer_commit`; корреляция с block size | Реже переключать protection (если метрики покажут), page-batching |
| JIT helper-call boundary (внутриблочный syscall-like переход в C helpers) | `engine/hyperbridge/src/hb_arm64_codegen.c` (множественные `emit_call_helper(...)`, см. вызовы `hb_jit_helper_exec_*`, `hb_jit_helper_load/store_*`) | Для многих IR операций генерируется call в C helper, а не inline ARM64 | Call boundary на каждую инструкцию/операнд может доминировать на коротких UI-блоках | Добавить счётчики helper-call по типам (`binop`, `cmp/test`, `mov/extend`, `load/store`, flags consumers), и таймер суммарно в helper-группах | Inline наиболее частых helper-path после top-N профиля |
| Persistent translation cache lookup | `engine/hyperbridge/src/hb_aot_cache.c:118-174`, `234-266` | `load_entries()` + линейный scan в `hb_cache_get` | O(N) lookup и аллокации/копирование при каждом get | Таймеры: open/read header, load_entries total, entries scanned, hit/miss latency; отдельно cold/warm startup | Индексация/новый формат только после подтверждённой lookup-стоимости |
| Persistent translation cache put/flush | `engine/hyperbridge/src/hb_aot_cache.c:176-205`, `272-307` | Перезапись через temp+fsync+rename на каждом put | Потенциально дорогой I/O путь на прогреве | Метрики: puts count, bytes written, fsync/rename latency, warmup wall-time с cache enabled | Батчинг/append-index формат после замера write overhead |
| Interpreter path (fallback/coverage pressure) | `engine/hyperbridge/src/hb_runtime.c:270-275`, `engine/hyperbridge/src/hb_interpreter.c` (`hb_interpreter_run`) | Полное исполнение IR в interpreter | Если горячие опкоды/паттерны остаются вне эффективного JIT path, UI может идти через более тяжёлый loop | Счётчик доли времени interpreter vs JIT на NPP flow; per-op histogram в interpreter (без глубокого intrusive trace) | Закрывать именно top-hot fallback классы опкодов |
| Syscall/unixlib boundary (guest→host) | `dlls/ntdll/unix/macrunner_hb.c` (по prompt) — **не найден в текущем дереве чтением** | Точка перехода из guest к host thunk/dispatcher/signal path | Потенциальный fixed overhead на каждый host call | TODO для Codex: найти актуальный файл-замену boundary (в `dlls/ntdll/unix/**`) и поставить per-call latency + callsite histogram на direct/import thunk path | Снижение per-call boundary overhead только после замера реального call mix |
| present/flush кандидат (легко, без углубления) | `win32u -> winemac` path (вне глубокой ревизии этой карты) | UI present/flush цикл | Может добавлять заметный frame-latency в интерактиве | Лёгкая трасса timestamps на вход/выход present/flush + частота вызовов в menu/dialog typing сценариях | Коалесцинг/уменьшение частоты вызовов при подтверждении |

## Непокрытые опкоды в горячем пути (кандидаты, не диагноз)

Источник: `reports/engine-audit/HYPERBRIDGE-OPCODE-COVERAGE-ATLAS.md`.

- Использовать atlas как **список кандидатов fallback→interpreter** для GDI/CRT/event-loop трасс, а не как доказательство bottleneck.
- Для Codex: добавить счётчик `unsupported_opcode_hits` с разрезом по opcode family и callsite (guest PC/module), затем сопоставить с NPP сценариями (меню/диалог/набор текста).
- Если семейство часто встречается в этих сценариях и коррелирует с latency spikes — только тогда поднимать в приоритет оптимизации.

## Что уже известно из bench/cache отчётов (без дублирования)

- `reports/HYPERBRIDGE-BENCH.md`: есть базовый micro-bench (`decode/lift/run/total`), но не даёт фазовой картины runtime hot path для NPP.
- `reports/HYPERBRIDGE-TRANSLATION-CACHE.md`: зафиксированы `cold_cache_status: miss`, `warm_cache_status: hit` (и `entries: 0` на снимке).
- `reports/HYPERBRIDGE-LAZY-FLAGS-BENCH.md`: формульные lazy-flags замеры есть; это не замена runtime/JIT helper-boundary профилю в app-flow.
- `reports/HYPERBRIDGE-FLAGS-BASELINE.md`: описывает модель/маршрутизацию флагов (producer/consumer), полезно для выбора точек замера вокруг flag-related helpers.

## Порядок замеров для Codex (NPP-сценарий)

1. **E2E phase split (без оптимизаций):** на сценариях `open menu`, `open dialog`, `typing` снять долю времени по backend (`interpreter`/`JIT`) и по фазам runtime loop (`find_block`, cache-hit exec, compile, helper calls, boundary calls).
2. **Cache behavior:** отдельно cold start vs warm start; снять hit/miss и latency для in-memory block cache и persistent translation cache (lookup/put/fsync).
3. **Helper-boundary profile:** топ helper-групп по count+time внутри JIT-блоков; подтвердить, какие из них реально горячие в NPP input/render loop.
4. **Opcode fallback map:** `unsupported_opcode_hits` + opcode family histogram, связанный с конкретными NPP действиями.
5. **Syscall/unixlib boundary TODO:** после нахождения актуального boundary-файла в `dlls/ntdll/unix/**` добавить per-call cost и call mix.
6. **present/flush light trace:** только частота и latency, без глубокого dive.

## TODO / ограничение текущего прохода

- По пути из prompt `dlls/ntdll/unix/macrunner_hb.c` в текущем дереве чтением совпадений не найдено; boundary-секцию оставил как измерительный TODO для Codex с требованием найти актуальную реализацию thunk/dispatcher/signal path.
