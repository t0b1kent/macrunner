# LANE-EXIT5-RESEARCH — интернет-разведка отказа exit=5 (БЕЗ ПРОГОНОВ)

Дата: 2026-08-02. Метод: 5 параллельных web-research углов + локальные golden-логи. Прогонов не было.
Источник сырых отчётов: 5 субагентов (swarm), консолидация ниже.

---

## ГЛАВНЫЕ ВЫВОДЫ (TL;DR)

1. **exit=5 = усечённый до 8 бит NTSTATUS 0xC0000005 (STATUS_ACCESS_VIOLATION).** Доказано в нашем
   же дереве: unhandled exception → `NtTerminateProcess(self, rec->ExceptionCode)`
   (`engine/wine/dlls/ntdll/unix/thread.c:1870`) → `exit_process(status)` → POSIX `wait()` отдаёт
   младший байт: `0xC0000005 & 0xff = 0x05`. Это НЕ код Unity, НЕ ERROR_ACCESS_DENIED, НЕ синтез Wine
   (grep `exit( 5 )` по engine/wine = ноль). Комментарий в `macrunner_hb.c:16467-16473` уже знал про
   «exit(5)» как AV-транкейт.
2. **Сигнатура `pc == lr == fault-addr` внутри собственного стека + info0=0x8 = порченый saved-LR →
   `ret` в данные стека → NX instruction abort.** Apple: «если pc == exception address — это невалидный
   instruction fetch» от плохого указателя/возврата. info0=0x8 = `EXCEPTION_EXECUTE_FAULT` (winnt.h:2050),
   синтезирован нашим хендлером из ESR EC=0x20/0x21 (Instruction Abort), НЕ mach-код.
3. **PAC и BTI ОПРОВЕРГНУТЫ.** Наш процесс — `Mach-O arm64` (не arm64e): ядро выключает PAC в SCTLR_EL1,
   `pacibsp` в `_OSAtomicAdd32Barrier` (arm64e dyld cache) исполняется как NOP — это шум, не сигнал
   (измерено: lelegard/arm-cpusysregs). PAC-фолт дал бы poisoned-адрес или SIGTRAP/EC=0x1C — у нас чистый
   page-aligned стековый адрес + Instruction Abort. BTI: на M1 нет железа; BTI-фолт = SIGILL, не SEGV.
4. **«Частичная загрузка сцены» ОПРОВЕРГНУТА локальным golden-логом.** Прогон 28.07 (меню на экране,
   47240 объектов) содержит БАЙТ-В-БАЙТ ту же сигнатуру: sweep `49 / 4274`, `Couldn't find GameCameras`,
   `Couldn't find a UIManager` ×11 — всё это ДО соответствующих `manager-created … Awake`. 4274 —
   нормальное pre-activation состояние, а не недогруз. Ни одной asset-I/O ошибки в логах (Unity такие
   вещи логирует: «corrupted!», «Failed to read past end of stream», «Failed to decompress» — ноль
   совпадений). Падающий прогон умирает ВНУТРИ boot-scene корутины `StartManager.<Start>d__25`, ДО
   `IL_02a7` (`Didn't need to wait for PlayerPrefs load.`) — т.е. причинность «краш → нет второго
   прохода», а не «частичная сцена → краш». «Making UI menu lean.» не появлялось даже в golden.
5. **GC-перекос 72× = CPU-bound `MarkObjects` в транслированном коде, а не stop-world/сигналы.**
   HK = Unity 6000.0.61f1 + Mono + **Boehm** (не sgen; на Windows stop-world через
   SuspendThread/GetThreadContext, сигналов нет). Декомпозиция уже в наших логах:
   `MarkObjects` 8832 мс против 52.85 мс у оракула (~167×), остальные bucket'ы ~50-100×; перекос
   зависит от JIT memory-path конфигурации (25.9 с с direct-mem off ↔ 9.2 с on). Это throughput-
   симптом JIT, временно ОТРЕЗАН от смерти (run6 жил 5628 с после GC; текущий — ~170 с UIManager
   после unload). Не механизм отказа.
6. **Upstream (FEX) имеет открытые баги ровно нашего класса** — и это лучший источник рабочих
   гипотез и готовых приёмов (ниже).

---

## ANGLE 1 — код 5 как таковой

Искал: Unity exit codes, wine exit code 5, NTSTATUS→unix exit truncation, ExitProcess paths.

- exit=5 = `0xC0000005 & 0xff`. Цепочка byte-exact в нашем форке (thread.c:1870 → 1690/1709 →
  server.c:1589-1593; `get_unix_exit_code()` thread.c:112-117 не маскирует статус).
- Локальные probe-отчёты уже вязали 0xC0000005 с этим тайтлом:
  `reports/phase4-hollow-knight/RUN-X64-EXIT-TRACE-RESULT.md` (RtlExitUserThread(0xc0000005)),
  `EXIT-ORIGIN-PROBE-RESULT.md` (native AV в `run_jit_block_with_signal_guard+832`). Июльский прогон
  был write-AV (info[0]=1) на мусорный адрес; текущий — execute-AV в собственный стек. Разные
  манифестации одного класса «порченый указатель/адрес возврата из JIT».
- Отличие: июль = самоубийство одного потока + вис до timeout(124); текущий = выход ВСЕГО процесса →
  либо умер последний поток, либо сработал unhandled-exception terminator. Различимо (P1/P2 ниже).
- Отсутствие macOS .ips — ожидаемо: Wine конвертирует фолт в Windows exception и выходит чистым
  `exit()`. Не аргумент против AV.
- Документированных Unity exit codes не существует публично (честно: источников нет). Unity-игры при
  фатале падают с 0xC0000005 и на настоящей Windows (множество источников).

Опровергнуто: «5 = ERROR_ACCESS_DENIED от Unity», «осмысленный Mono/GC exit code», «нет .ips = не
падал», «ExceptionAddress=0 = прыжок на null».

## ANGLE 2 — сигнатура pc==lr, execute в собственном стеке, pacibsp рядом

Искал: PAC failure manifestation, BTI на macOS, NX execute fault, mach exception encodings.

- info0=0x8 = наш `EXCEPTION_EXECUTE_FAULT` из ESR top-nibble 0x8 → EC 0x20/0x21 = Instruction Abort.
  Подлинный NX-exec даёт IFSC 0x0C–0x0F (permission fault). Предсказание: полный ESR должен быть
  `0x8200000f`-подобным.
- pc==lr конкретно = порченый RETURN ADDRESS: `ret` берёт x30; плохой `blr` оставил бы lr в коде.
  Т.е. значение в saved-LR-слоте кадра (или сам lr) заменено стековым адресом, потом `ret` его
  съел. Согласуется с event-A находкой LANE-EXIT5: `[sp-8]=0x1119cdbe8` — стек-адрес, записанный
  в слот кадра; значение page-aligned (`…e400`) → похоже на sp/fp-swap в LR-слот, не heap spray.
- PAC — опровергнут троекратно (arm64 non-e задача → PAC NOP-ится; PAC-фолт = poisoned адрес или
  SIGTRAP EC=0x1C; `pacibsp` у `_OSAtomicAdd32Barrier` — NOP-пролог arm64e кеша). Информационное
  содержание `_OSAtomicAdd32Barrier`: поток был в unfair-lock atomic slow path — консистентно с
  коррупцией от конкурентного локера.
- BTI — опровергнут (M1 без BTI; BTI-фолт = SIGILL Branch Target Exception).
- Источники: Apple «Investigating memory access crashes»; lelegard/arm-cpusysregs «arm64e on macOS»
  (измерено); bun#30281 / claude-code#19068 (FPAC); HackTricks stack-shellcode arm64 (NX hardware).

Выжившая гипотеза: (c)+(d) — коррупция saved-LR → ret в NX-стек; race (watchpoint подавляет,
0/2 воспроизведений с WP).

## ANGLE 3 — апстрим (Hangover / FEX / box64 / ARM64EC)

Искал: трекеры FEX/box64/hangover, SMC-гонки, Mono-under-emulator, JIT block cache eviction.

Ключевые находки (все с URL):
- **FEX#5328** (github.com/FEX-Emu/FEX/issues/5328): WoW под Winlator+Wine arm64ec → `NoExec
  instruction in entry block`, первый AV, затем ШТОРМ C0000005. Причина: регион MEM_MAPPED
  PAGE_EXECUTE_READ, но трекер executable-регионов FEX потерял его — **много NotifyUnmapViewOfSection
  без парного NtMapViewOfSection**. Класс: address-space reuse + протухший code-tracking → wild
  execute по переиспользованному адресу. Метод диагностики прямо переносим.
- **FEX#5000**: FEX специально собирается детектить «RIP внутри стека потока» телеметрией — признанный
  upstream феномен, связан с SMC. Приём: логировать (rate-limited) любой block lookup с guest RIP в
  диапазоне чьего-либо стека — поймаем сам переход в стек, сейчас невидимый.
- **FEX-2509 blog**: Mono JIT = хроническая боль (SMC-инвалидация, стуттеры); FEX шипит MonoHacks
  (default true): **hook-based SMC + меньшие JIT-блоки при детекте Mono**, «less likely to crash
  when TSO memory model emulation is disabled». У нас ничего из трёх (mono-detect / small blocks /
  hook SMC) нет — вывод из описания reverify в AGENTS.md, проверить grep'ом `hb_runtime.c` за 5 минут.
- **FEX#5404** (open): Batman Arkham Origins — **SMC SIGSEGV loop при multiblock; с multiblock off
  игра продолжается**. Наш boundary-trace заканчивается в `hb_jit_helper_exec_two_block_loop` —
  у нас есть fused two-block путь. Если Mono патчит call site внутри ВТОРОГО блока fused-пары,
  entry-reverify по адресу первого блока этого не поймает.
- **FEX-2501 blog**: block-level invalidation недостаточна, когда SMC-запись бьёт в текущий
  исполняемый блок — FEX реконструирует контекст на модифицирующей инструкции. Mono magic trampoline
  патчит call site (mono-project mini-porting doc) = запись в уже транслированный, возможно
  исполняемый код. Reverify-on-entry не спасает поток, уже находящийся в блоке, и не спасает
  ДРУГОЙ поток, исполняющий тот же блок.
- **box64 README**: mono/Unity3D под dynarec = постоянный «нормальный» segfault-трафик от mprotect-
  SMC; **box64#1570**: random freezes чинятся `BOX64_DYNAREC_STRONGMEM=1` (memory ordering).
  Подавление нашего отказа watchpoint'ом (пертурбация тайминга) консистентно с этим классом.
- **FEX wiki: Hollow Knight = Playable**; PortMaster гоняет Windows-билд HK через wine64+box64.
  Тайтл эмулируем — отказ специфичен для нашего стека, не для жанра.
- Два наших недействительных бисект-вердикта СОГЛАСУЮТСЯ с upstream-картиной: все аналоги — race/
  tracking-баги с timing-dependent манифестацией, не детерминированные логические регрессии.

## ANGLE 4 — Unity partial scene load (4274 vs 47392)

Искал: UIManager/GameCameras сообщения, Loaded Objects semantics, asset read failures, HK modding.

- **F1 (локально, решающе)**: golden run.log 28.07 (checkpoint `20260728-MAIN-MENU-REACHED`) содержит
  идентичные 49/4274, GameCameras, UIManager ×11 — и потом доходит до 66/47240 + меню на экране.
  Каждое «Couldn't find» предшествует своему `manager-created … Awake`. Объектов нет ПОТОМУ ЧТО их
  ещё не создали — boot-scene поллит синглтоны до активации меню.
- **F2 (локально, решающе)**: падающий прогон (laneA-SPREAD1-try1) умирает до `IL_02a7`
  (`Didn't need to wait for PlayerPrefs load.`) — внутри boot-корутины. Строки `Performing automatic
  level start.`, второй sweep 66/47240 — отсутствуют (game Debug.Log = валидный негатив).
- **F3**: ноль asset-I/O ошибок в обоих логах; Unity логирует все классы таких ошибок. HK шипит сцены
  loose serialized files, не bundles — load/unload циклится нормально.
- `Couldn't find …` — собственные guard'ы HK (CSDN туториалы воспроизводят дословно);
  `FindObjectOfType` возвращает только active объекты, полл per-frame → ~12 строк за 0.7 с на 13 Гц.
- Почему на Windows-бейзлайне сообщений нет (инференс): там взята ветка `Loaded saved language code`
  (PlayerPrefs GameLangSet) → language-select UI пропущен → никто не поллит синглтоны. Наши прогоны
  с свежим prefix берут `Loaded system language` fallback → select UI показан → его компоненты поллят.
- `Loaded Objects now:` = живые нативные объекты после sweep, НЕ «сколько загрузилось».

Опровергнуто: частичная загрузка / corrupt read / LZ4 / mmap / serialization mismatch; «синглтоны не
инстанцировались из-за missing scripts»; «exit=5 — следствие битой сцены»; «Making UI menu lean. —
следующий майлстоун» (не появлялось даже в golden).

## ANGLE 5 — GC 4894 мс vs 68 мс

Искал: Boehm vs sgen, stop-world механика, SuspendThread, stack scanning, bucket breakdown.

- HK = Unity 6000.0.61f1 + Mono + Boehm (Unity 6 docs: Boehm-Demers-Weiser, incremental default).
  sgen-линия (SIGUSR1/SIGUSR2) НЕПРИМЕНИМА — Boehm на Windows останавливает мир через
  SuspendThread/GetThreadContext (bdwgc win32_threads.c прочитан по исходнику).
- Перекос уже декомпозирован нашими логами: MarkObjects ~167× (8832 vs 52.85 мс), остальное ~50-100×;
  сумма bucket'ов ≈ Total → места для скрытого suspend-ожидания почти нет (остаток ≤~200 мс в
  FindLiveObjects не исключён, догадка про bucket assignment).
- MarkObjects-доминирование нормально даже нативно (968/975 мс Windows-пример); это pointer-chasing +
  native↔managed boundary — ровно то, что максимально наказывает memory path транслятора.
  Конфиг-чувствительность (25.9 с ↔ 9.2 с от direct-mem family) = throughput-связка, не timing.
- Boehm WARN'ов (`stack pointer out of range, pushing everything`, `SuspendThread loop failed`) в
  логах ноль (осторожно: Unity может роутить GC-warning'и мимо нашего захвата — слабое отрицание).
- Перекос и смерть временно разрезаны: run6 — 5628 с после GC жив; текущий — ~170 с UIManager после
  unload. GC-перекос — перфоманс-баг JIT memory path, НЕ механизм exit=5. Общий слой, разные механизмы.

Опровергнуто: sgen-сигналы; «heap huge/asset thrash» (heap меньше оракула); «гигантский wrong stack
scan» (1 МБ/поток = sub-ms); «SuspendThread retry loop жжёт секунды» (как доминирующий член).

---

## ОПРОВЕРГНУТОЕ (сводно)

| Гипотеза | Чем опровергнута |
|---|---|
| exit=5 = Unity/Win32 код (ERROR_ACCESS_DENIED и т.п.) | Цепочка NTSTATUS→exit truncation в нашем дереве + RtlExitUserThread(0xc0000005) в probe-отчёте |
| Нет .ips = не падал | Wine конвертирует фолт в Win-exception, выходит чистым exit() |
| PAC failure (pacibsp рядом) | Процесс arm64 non-e → PAC NOP (измерено, lelegard); PAC-фолт = poisoned-адрес/SIGTRAP EC=0x1C |
| BTI | M1 без BTI; BTI-фолт = SIGILL, не Instruction Abort |
| info0=0x8 = mach/ESR-IFSC код | Это EXCEPTION_EXECUTE_FAULT (winnt.h:2050) из ESR EC=0x20/0x21 |
| Частичная сцена / asset I/O / decompress | Golden-лог: те же 4274 + те же сообщения, потом меню; ноль error-строк |
| UIManager×12 = объекты не создались | В golden каждое «Couldn't find» предшествует своему Awake |
| exit=5 следствие битой сцены | Смерть внутри boot-корутины до IL_02a7 — сцена не активировалась вообще |
| sgen-сигналы / suspend-loop как GC-перекос | Boehm, не sgen; bucket'ы сходятся к Total без suspend-окна |
| GC-перекос = механизм смерти | 5628 с и ~170 с жизни после GC в двух прогонах |
| «HK/Unity-Mono неэмулируем в принципе» | FEX wiki Playable; PortMaster wine64+box64 |
| Коммит-атрибутация (2 бисекта мимо) | Все upstream-аналоги — timing-race класс; бисект-исход подтверждён, не опровергнут |

---

## ПРОВЕРЯЕМЫЕ ПРЕДСКАЗАНИЯ (что смотреть в следующем прогоне — по приоритету)

**T1. Доказать exit=5 ≡ 0xC0000005 для текущего прогона (один прогон, ноль альтернатив).**
`MACRUNNER_TRACE_PROCESS_EXIT=1` (гейт есть, thread.c:1711-1715). Строка
`macrunner-process-exit: stage=exit_process … status=0xc0000005` → цепочка доказана, вопрос сводится
к «кто эмитит AV». Если `status=0x5` чистый — версия ExitProcess(5) жива (маловероятно).

**T2. Кто терминирует процесс.** Тот же трейс: выход через exit_process из exit_thread (последний
поток) vs unhandled-exception terminator (thread.c:1870). WINEDEBUG=-all глушит ERR_ — смотреть наш
fprintf-трейс или +seh.

**T3. Полный ESR в `macrunner-hb-signal-entry`.** Предсказание: esr=0x8200000f-класс (IFSC 0x0C–0x0F,
permission fault на mapped NX-странице). Если IFSC 0x04–0x07 (translation fault) — адрес немапнут,
предпосылка «свой стек» ломается, открываются другие углы.

**T4. `macrunner-hb-fault-frame` (уже есть, signal_arm64.c:3305).** Предсказание для порченого
saved-LR: одно из слов около sp равно fault-адресу (протухшая копия LR в кадре), соседи содержат
page-aligned стек-адрес (…e400). Если копии fault-адреса в кадре НЕТ — коррупция в lr-регистре
(путь context-restore: x18 sigreturn / hb_run_guest_return — территория architectural bug #2).

**T5. FEX#5000-телеметрия: логировать block lookup с guest RIP внутри любого thread-stack диапазона.**
Одно срабатывание до краша = пойман сам переход в стек (сейчас невидим). Дамп последней пары
guest call/ret + cache entry для return address.

**T6. FEX#5328-метод: трейс NtMapViewOfSection/NtUnmapViewOfSection для диапазона fault-адреса.**
Если диапазон был mapped executable → unmapped → remapped как стек потока → подтверждён класс
«stale code-tracking на переиспользованном адресе».

**T7. H3 (inline SMC): логировать guest-записи в RWX-страницы, где target ВНУТРИ guest-диапазона
существующего cached-блока (не по entry).** Mono call-site patch = 5-байтный rel32 rewrite. Такие
записи при активных исполнителях блока → подтверждено; направление фикса = FEX MonoHacks (меньшие
блоки + hook-patching при детекте Mono). Проверить grep'ом `mono` в hb_runtime.c — есть ли у нас
вообще mono-detect.

**T8. A/B двухблочный helper.** Если есть knob выключить `hb_jit_helper_exec_two_block_loop` /
multiblock — FEX#5404 предсказывает: краш уйдёт или переедет в обычный dispatch. Сигнатура без
изменений при выключенном fusion → H4 мёртв.

**T9. TSO/memory-ordering.** Если есть strongmem/barrier-флаг — прогон с максимальными барьерами:
отказ исчезает/переезжает → ordering race (box64 STRONGMEM прецедент; FEX вяжет hardware TSO на
Apple Silicon не зря). Дискриминатор с T6: подавление ЛЮБЫМ watchpoint/замедлением = timing race;
подавление только конкретными адресами = data-dependent.

**T10. Armed-watchpoint план из LANE-EXIT5 (слот 0x1119cdbe8) — правильный дискриминатор, продолжать.**
PC писателя в watchpoint-hit назовёт подсистему. Page-aligned записываемое значение (…e400) указывает
на sp/fp→LR-swap класс бага, не heap spray.

**T11. GC-перекос (перфоманс, отдельно от exit=5):** sample(1) процесса во время `Total:` окна —
H1 предсказывает поток UnloadUnusedAssets на 100% CPU в транслированном коде (mem_read-heavy блоки);
сигнальная гипотеза предсказала бы блокировку в NtSuspendThread. Grep stderr на Boehm WARN'и
(`pushing everything`, `SuspendThread loop failed`) — любой hit = реальный suspend-context баг
(чинить независимо от тайминга).

**T12. Маркер точки смерти в boot-корутине:** наличие/отсутствие `Didn't need to wait for PlayerPrefs
load.` в следующем прогоне делит границу: есть строка, но нет `Performing automatic level start.` →
новая узкая граница mid-activation; нет строки → умерли в boot-poll'ах, как SPREAD1.

---

## ДОГАДКИ (явно помечены, источников нет)

- Конкретный механизм «стек-адрес в LR-слоте» — вывод из сигнатуры, не из источника.
- Связь UIManager-ретрая 13 Гц с моментом смерти — догадка; T12/T4 дают дешёвую проверку.
- Отсутствие у нашего JIT mono-detect/small-blocks/hook-SMC — инференс из AGENTS.md, проверить кодом.
- Привязка GC-перекоса к той же первопричине, что краш — не подтверждена; скорее общий слой (JIT
  memory path), разные механизмы.
- Bucket assignment Boehm GC.Collect внутри Unity-разбивки — источника нет.

## Чего не хватило / не найдено

- Публичной таблицы Unity exit codes не существует (искали, нет).
- WineHQ GitLab MR 5301 (NtSuspendThread waits) — за anti-bot стеной, только сниппет.
- Hangover tracker: Unity/Mono scene-load багов этого класса нет (Hangover делегирует FEX/box64).

---

# 2026-08-02 21:49 — EXIT5-RESEARCH, раунд 2 (6 направлений параллельно, swarm)

Источник: 6 исследовательских субагентов (веб + наши логи), без прогонов. Ниже — консолидация
с ссылками. Полные отчёты агентов не сохранены на диск (жили в контексте сессии); ключевые URL
и выводы перенесены сюда.

## Направление 1 — код 5 как таковой

**Вывод: exit=5 = усечение 0xC0000005 (STATUS_ACCESS_VIOLATION) до 8 бит. Это guest-side
unhandled AV, а не код Unity и не код Wine.**

- У Unity НЕТ документированной таблицы exit codes; единственный легальный путь — `Application.Quit(int)`, дефолт 0. Источник: https://docs.unity3d.com/2021.3/Documentation/ScriptReference/Application.Quit.html
- Wine пробрасывает guest DWORD exit status в Unix `exit()`, ядро режет до 8 бит. Каноническое доказательство механизма: `0xC0000135 & 0xFF = 53` — задокументировано в Wine bug 51645 (https://bugs.winehq.org/show_bug.cgi?id=51645). Значит `0xC0000005 & 0xFF = 5`.
- Windows-процесс, убитый unhandled exception, завершается с кодом исключения как exit status (https://stackoverflow.com/questions/65658940).
- `ExceptionInformation[0]==8` в AV = EXECUTE violation (попытка исполнения неисполняемой страницы) — https://learn.microsoft.com/en-us/windows/win32/memory/data-execution-prevention. Совпадает с нашим `info0=0x8`.
- «Отсутствие macOS crash report» объясняется: fault живёт внутри guest-exception машинерии Wine, процесс выходит «добровольно» с кодом исключения.
- В нашем дереве цепочка уже подтверждена byte-exact: thread.c:1870 → 1709/1719 → get_unix_exit_code (thread.c:112-117); macrunner_hb.c:16473 уже содержит комментарий «-> exit(5) shortly after Unity input-init».

ОПРОВЕРГНУТО: «5 = Unity-статус» (таблицы нет), «5 = внутренний код Wine» (нет evidence, Wine пробрасывает guest DWORD), «5 = ERROR_ACCESS_DENIED / чистый Application.Quit(5)» (неправдоподобно mid-load).

## Направление 2 — сигнатура pc==lr, exec в собственном стеке, pacibsp рядом

**Вывод: PAC и BTI опровергнуты. Стоящая модель: ret/br x30 с lr, указывающим в собственный
стек (NX/XN exec fault), т.е. разрушенный return path / разбитый кадр. pc==lr возможно ТОЛЬКО
после ret-подобного перехода (blr поставил бы lr = код после blr и сломал бы равенство).**

- PAC опровергнут дважды: (а) по сигнатуре — poison-mode даёт data abort с pc≠far (https://srd.cx/possible-pointer-authentication-failure-data-abort/), FPAC-mode даёт EC=0x1C SIGILL с pc = самой инструкции auti* в text; (б) по архитектуре — наши бинарники arm64, не arm64e, PAC-инструкции исполняются как NOP (https://github.com/lelegard/arm-cpusysregs/blob/main/docs/arm64e-on-macos.md). XNU имеет отдельный путь ESR_EC_PAC_FAIL (https://fergofrog.com/code/codebrowser/xnu/osfmk/arm64/sleh.c.html).
- BTI опровергнут: в userland macOS не enforced, bti = NOP, br на non-landing-pad не фолтит (https://discourse.llvm.org/t/apple-arm-chips-and-bti-instruction-is-it-currently-ignored-for-user-apps-and-why/91346).
- NX-exec из стека: XNU даёт EXC_BAD_ACCESS / KERN_PROTECTION_FAILURE, code[1]=pc=far=адрес в стеке, ESR≈0x8200000F. Ровно наша сигнатура.
- `_OSAtomicAdd32Barrier` + pacibsp по pc=0x18418770c — почти наверняка АРТЕФАКТ сломанного анвайнда (ГИПОТЕЗА, обоснованная): весь dyld cache arm64e, pacibsp в прологе каждой функции; return-address, равный входу функции (offset 0), не может быть результатом `bl` (тот возвращает entry+4). Это stored pointer / мусор в разбитом стеке, не call site. Информационное содержание низкое.

## Направление 3 — апстрим (FEX / box64 / Hangover)

**ГЛАВНОЕ ПОПАДАНИЕ раунда: FEX-2509 содержит Mono-специфичные фиксы ровно нашего класса.**

- FEX-2509 (https://fex-emu.com/FEX-2509/): «Mono's JIT … self-modifying code … less likely to crash». Конкретно:
  - PR/commit aa979e4 «Implement mono specific hacks»: FEX ПЕРЕХВАТЫВАЕТ запись Mono trampoline backpatcher — пишет сам под CodeInvalidationMutex и СИНХРОННО зовёт InvalidateGuestCodeRange на пропатченные байты. Это write-side закрытие окна, которое наш read-side FNV-reverify оставляет открытым: Mono пропатчил trampoline, другой поток продолжает исполнять stale-трансляцию до следующего cache hit.
  - commit bd0cae9 «Force acq/rel semantics for known Unity ringbuffer offsets»: Unity SPSC ringbuffer (offsets 0x80/0x84/0xC0/0xC4, стабильны с 2015), plain mov на них принудительно делаются acquire/release. Это memory-ordering race — timing-sensitive, ПОДАВЛЯЕТСЯ watchpoint'ом. Прямой шаблон для аудита нашего JIT: все guest LDR/STR без барьеров на weak-ordered Apple Silicon?
  - 6ae9458 tailcall handling с mono hacks; 4f84c28 cooperative suspend при выходе из JIT (GC safepoint).
- Hollow Knight «Playable» на FEX (https://wiki.fex-emu.com/index.php/Hollow_Knight) — stacked-JIT Unity/Mono HK принципиально runnable, наш отказ = handling gap, не неосуществимость.
- box64: write-protect translated pages + SIGSEGV-based invalidation; BOX64_DYNAREC_STRONGMEM «needed by some games, like RimWorld» (Unity) — ещё одно подтверждение, что Unity игры стрессуют memory model эмуляции (https://github.com/ptitSeb/box64/blob/main/docs/CHANGELOG.md).
- WineHQ forum t=37530: 64-bit Unity игры падают с `virtual_setup_exception stack overflow` под Wine на Apple Silicon даже через Rosetta, под CrossOver работают — Unity/Mono stack-hungry и чувствителен к Wine stack handling на этой платформе.
- Hangover: Unity/Mono-специфичных issues нет; с 10.11 делегирует FEX+box64 (https://www.phoronix.com/news/Hangover-10.11-Released).

## Направление 4 — UIManager/GameCameras / частичная сцена (4274 vs 47392)

- Строки — собственные синглтон-геттеры HK (Assembly-CSharp), паттерн FindObjectOfType → null → Debug.LogError. Источники декомпилов: http://www.shjibing021.com/smzs/165320.html (GameCameras), https://blog.csdn.net/dangoxiba/article/details/143498240 (UIManager). Спам на 13 Гц = кто-то поллит синглтон каждый кадр. Королларий: managed-код в этот момент ЖИВ (polling/FindObjectOfType/Debug.LogError исполняются).
- «Unloading N unused Assets / Loaded Objects now» печатает Resources.UnloadUnusedAssets — Unity зовёт её автоматически на каждой смене сцены (https://discussions.unity.com/t/about-log-analysis/918217).
- Unity НЕ молчит при битых asset/serialization reads («Failed to load … corrupted», «level0 is corrupted», «referenced script … is missing»). Отсутствие таких строк в наших логах делает «тихий отказ чтения resources.assets» маловероятным как первичный механизм.
- 91% дефицит объектов (4274/47392) НЕ может быть missing-script (unbound scripts оставляют GameObject посчитанным) — объекты меню реально никогда не инстанцировались.
- НО: наш собственный golden-лог (reports/phase4-hollow-knight/checkpoints/20260728-MAIN-MENU-REACHED/evidence/run.log) показывает ТОТ ЖЕ проход 49/4274 с теми же GameCameras/UIManager строками, а затем ВТОРОЙ проход «Restored language code» + 47240 и меню на экране. Т.е. 4274 само по себе не фатально — фатально то, что второго прохода не случилось. Напряжение с UIREPRO: у Windows-oracle UIManager встречается 0 раз и есть «Performing automatic level start», отсутствующее в 341/341 наших прогонов — расхождение не закрыто.
- BepInEx issue #1158 (Silksong): тот же спам GameCameras при сломанном managed-слое (IL Compile Error) — корреляция этого error-семейства со сломанным managed execution, не с asset corruption.

## Направление 5 — GC 4894 ms vs 68 ms

- Unity 6000 Mono = Boehm GC, incremental по умолчанию (https://docs.unity3d.com/6000.1/Documentation/Manual/performance-incremental-garbage-collection.html); scene load триггерит полную stop-the-world сборку.
- bdwgc в DLL-режиме регистрирует ВСЕ потоки через DllMain — «foreign threads не зарегистрированы» опровергнуто для этого сценария (https://raw.githubusercontent.com/bdwgc/bdwgc/master/win32_threads.c).
- Кандидаты на 72×: (а) GC_suspend retry-loop: SuspendThread+GetThreadContext с retry до 1M раз при неудаче GetThreadContext («SuspendThread loop failed» реально бывает в Unity играх, https://github.com/IntelSDM/7DTD/issues/7) — под нашим рантаймом context capture потока, подвешенного mid-translation, может флапать, каждый retry = 2+ wineserver round-trip × ~50-100 потоков; (б) GetWriteWatch на macOS — software fallback (нет userfaultfd), известен как «much slower» (Wine bug 37669); (в) stale Rsp при захвате → bdwgc WARN «pushing everything» → консервативное сканирование всего стека каждого потока.
- GC-перекос уже ранее декуплирован от exit=5 по времени (прогоны жили 170с+ после GC) — это throughput-баг JIT memory path, не механизм смерти. Но класс механизма (suspension/context race) может быть ОБЩИМ с частичной сценой (ГИПОТЕЗА).

## Направление 6 — наши логи (самое важное)

**Конкретный root cause уже реконструирован координатором 01.08 21:45 по адресам+дизасму
(prompts/lane-uimanager-exit5.md § НАЙДЕНО), фикс НЕ применён:**

- +670.926s, tid=003c: SEH-анвайндер увидел кадр frame_pc=frame_lr=0x107092D40, отверг его с
  reason=pc-not-code, stop-unwind, через 2.5с exit=5.
- 0x107092D40 = host ntdll.so base 0x10701c000 + 0x76d40 = легитимный return address внутри
  `_macrunner_hb_call_direct_native_target + 0x8e0` (macrunner_hb.c:35013), сразу после
  `bl ___wine_unix_call_dispatcher`.
- Механизм отказа: macrunner_hb_is_plausible_recovered_pc (signal_arm64.c:167-210) →
  macrunner_hb_find_pc_section (:212-248) резолвит модули ТОЛЬКО через LdrFindEntryForAddress
  (PE loader list); host Mach-O ntdll.so не PE-модуль → FALSE → pc-not-code →
  macrunner_hb_stop_unwind_at_boundary (:250) → unhandled → NtTerminateProcess(0xC0000005) →
  POSIX truncation → 5.
- Вторично: bad-prev-sp сравнивает host sp с GUEST Tib.StackLimit/StackBase (signal_arm64.c:437-448) — сравнение двух разных стеков.
- Прецедент того же класса уже чинился: 21c25562 (2026-07-01) — unwind_made_no_progress false positive → та же цепочка recover→stop-unwind→exit=5.

## Сводный список ОПРОВЕРГНУТОГО (все раунды)

- PAC-отказ — тройное опровержение (сигнатура, архитектура процесса, dyld-cache пролог = шум).
- BTI — не enforced в userland macOS; M1 без BTI.
- exit=5 как Unity/Win32/Wine-код — это усечённый 0xC0000005.
- Тихий asset-I/O отказ (resources.assets truncation) — Unity такое громко логирует, строк нет; golden-лог проходит через те же 4274 в меню.
- Foreign-thread registration (mono_thread_attach класс) — bdwgc регистрирует все потоки через DllMain.
- sgen-гипотезы — HK = Unity 6000.0.61f1 Mono Boehm, не sgen.
- Null import call — probe не срабатывал; ExceptionAddress=0 = наше недозаполненное поле.
- Commit-attribution — 53-коммитная бисекция: отказ на каждой buildable точке, оба вердикта недействительны (timing-sensitive).

## ПРОВЕРЯЕМЫЕ ПРЕДСКАЗАНИЯ для следующего прогона (ранжировано по дешевизне)

1. **Без пересборки**: MACRUNNER_TRACE_PROCESS_EXIT=1 → ожидаем status=0xc0000005 (докажет exit=5 ≡ truncated AV для этого прогона). И grep лога на `macrunner-hb-native-dispatch-boundary-reject … reason=pc-not-code` за ~2.5с до exit=5 с frame_pc внутри host ntdll.so — если строки НЕТ, цепочка 21:45 не генерализуется и race-гипотеза (watchpoint) снова главная.
2. **Фикс из lane-uimanager-exit5.md**: научить recovery-предикат принимать PC внутри host unix ntdll.so mapping (не через LdrFindEntryForAddress). Предсказание: прогон переживает +670.9с и печатает второй «Restored language code» + «Loaded Objects now: ~47240».
3. **Player.log между «Restored language code 'EN'» и первым «Couldn't find»**: искать Exception/(Filename: Line:)/is missing/corrupted/Failed to load — единственная самая дискриминирующая проверка ветки A (boot coroutine умерла) vs B (десериализация) vs C (AsyncOperation никогда не завершается).
4. **Census потоков в фазе 13 Гц** (lldb): живы ли Unity Job worker threads. Мёртвые/заблокированные workers → соответствует FEX Unity-ringbuffer ordering race (bd0cae9).
5. **Барьерный аудит JIT**: испускаем ли мы ordered доступы (LDAR/STLR/DMB) для guest memory ops. Эксперимент по шаблону FEX: форсировать acq/rel на mov r/m с disp 0x80/0x84/0xC0/0xC4 — если отказ сдвинется/исчезнет, класс гонки подтверждён.
6. **SMC write-side check**: инструментировать store Mono backpatcher (адрес, который наш hash-reverify чаще всего evict'ит) — исполнял ли кто-то stale блок между guest-записью и нашим next-hit reverify.
7. **info0=0x8 в нашем логгере**: ни mach code[0]=8 (KERN_NO_ACCESS), ни esr IFSC=0x08 не совпадают с plain NX-exec (ожидается 2 / 0x0F) — перепроверить декодировку поля, самая дешёвая проверка.
8. **file на wine/ntdll бинарниках ⇒ arm64 (не arm64e)** — формально добить PAC-гипотезу in-process.
09:50 · EXIT5-RESEARCH-3 второй проход: 6 субагентов, дельта дописана в reports/lanes/EXIT5RESEARCH-PROGRESS.md (694 строки) · PAC закрыт; RET zero-guard=1 вызов эмиттера; GC_DISABLE_INCREMENTAL=1 решающий A/B; box64#3577 per-thread dispatch cache — приоритет скорости · следующий: прогон с TRACE_BRANCHES+macrunner-ww когда слот освободится
