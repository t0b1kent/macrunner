# ROSETTA-DEEP LANE — как устроены Rosetta 2, Prism, FEX, box64 и почему они быстрые

Дата начала: 2026-07-31. Лейн **только читает** (веб + исходники FEX/box64/QEMU).
Ничего не запускаем и не правим.

Каждая находка помечается **[SRC]** (подтверждено источником, ссылка приведена),
**[CODE]** (подтверждено чтением кода, файл/символ указан) или
**[ГИПОТЕЗА]** (вывод/догадка, не подтверждена).

## Наша исходная точка (данность, не переоткрываем)

- Каждый гостевой базовый блок делает полный круг через C-диспетчер: `avg_chain = 1.0000`
  на 205.8 млн диспатчей.
- Цена круга: ~350–500 хостовых инструкций + ~1.5 КБ трафика памяти против блока в 34
  ARM64-инструкции (~23 — работа гостя). **416 нс/блок**, 205.8 млн блоков за 85.6 с.
- Транслированный код = 3–6 % выборок профиля, обвязка ~54 %.
- Сцепление: дефект генеральный, `reason=runtime` → `reason=import-thunk` при отказе от
  проблемного края (BLOCK-CHAIN-DIFFERENTIAL-20260731). Механизм: снапшот сделан один раз
  на весь dispatch, а цепочка исполняет 2–6 блоков → откат при фолте отбрасывает легитимно
  завершённые блоки (CHAIN-SNAPSHOT-GRANULARITY-20260731).
- Фолты: 3 665 920 за прогон, 99.3 % BUS_ADRALN (1324→5895/с) — откат это ГОРЯЧИЙ путь,
  не исключение.
- Аппаратного TSO нет (приватный entitlement), `FEAT_AFP=0`, `FEAT_FlagM=1`.
- SMC: 16.7 млн перепроверок ради 2364 находок (FNV-хэш гостевых байт на каждый cache hit).

## Ключевые вопросы (чек-лист покрытия в конце документа)

1. Сцепление блоков: состояние при фолте внутри цепочки; владение регистрами; откат
   частично исполненного блока.
2. Стоимость входа в блок (инструкций обвязки, цифры).
3. AOT против JIT: что даёт Rosetta преимущество на СТАРТЕ.
4. SMC: как решают без 16.7M перепроверок.
5. Чего мы НЕ делаем вообще.

---

<!-- разделы наполняются по итерациям -->

## 1. Rosetta 2 (Apple)

Главные источники: Koh M. Nakagawa, Project Champollion part1/part2 [SRC];
dougallj «Why is Rosetta 2 fast?» [SRC]; Wikipedia/Rosetta [SRC].

### AOT-конвейер: когда, где, что кэшируется [SRC]

- AOT-файл (`.aot`) — обычный Mach-O arm64, лежит в
  `/var/db/oah/<sha256(путь+контент)>/<sha256>/имя.aot` (Champollion part1).
  Демон `oahd` при `exec` проверяет наличие `.aot`; нет — форкает `oahd-helper`,
  который транслирует весь `__TEXT` x86-64 заранее, пишет `*.aot.in_progress`,
  переименовывает атомарно (защита от гонки повторного запуска).
  Часть сегментов `.aot` мапится с `PROT_READ|PROT_EXEC` (max `RWX`).
- Первый запуск платит полную трансляцию (десятки МБ кода — секунды), последующие
  идут сразу с диска. Хэш включает содержимое файла И путь — пересборка/переезд
  даёт новый кэш-ключ.
- Системные библиотеки не транслируются на машине вообще: в ОС вшит
  `/System/Library/dyld/aot_shared_cache` (~2.4 ГБ) — **предтранслированный дупликат
  x86-64 dyld shared cache** (Champollion part2; структура `AotCacheHeader`,
  магия `"AotCache"`, до 3 mapping-записей, metadata-сегмент с
  `CodeFragmentMetadata` на каждый image: offsets x64→arm64 кода, branch data,
  instruction map; подписан code signature, грузится через `shared_region_check_np`).
  То есть у Cisco Webex/Xcode.so и пр. нулевой JIT даже при первом старте.
- Межпроцессная передача: `runtime` отдаёт fd x86-64 бинаря в `oahd` через Mach IPC
  (`sys_fileport_makeport`/`sys_fileport_makefd`), `oahd` отвечает fd `.aot`
  (Champollion part2).

### Схема трансляции [SRC] (dougallj, Champollion)

- **~1:1 инструкция-к-инструкции**, каждая x86-инструкция транслируется ровно один
  раз; NOP'ы выбрасываются. Expansion factor для sqlite3 = **1.64x** по размеру
  кода. Две известные межинструкционные оптимизации: (1) dead flags elimination
  (не считать x86-флаги, если до перезаписи никто их не читает на всех путях);
  (2) prologue/epilogue combining — слияние цепочек `push/pop` в `stp/ldp` с
  отложенным обновлением SP (работает как x86 stack engine; на M1 парные
  load/store исполняются как одна микрооперация).
- **Каноничное состояние между КАЖДОЙ инструкцией**: все 16 GPR размещены
  фиксированно в x0–x15 (RAX→x0, RCX→x1, ..., R15→x15; XMM0–15→q0–15), SP гостя =
  x4. Это даёт precise exceptions, сэмплирующий профайлер и рабочий LLDB почти
  бесплатно — откат на границу инструкции не требует никаких снапшотов вообще.
  Обратная карта arm64-PC→x86-PC хранится в AOT (секция fragment list в
  `LC_AOT_METADATA`, cmd `0xcacaca01`): верхний уровень — двоичный поиск,
  нижний — bit-packed delta-записи по инструкциям. Карта x86→ARM для indirect
  branch — двухуровневая (binary search), результаты кэшируются в hash-map;
  red-black tree держит runtime.
- **Возвраты и RAS**: x86 `CALL`/`RET` переписываются в ARM `BL`/`RET` напрямую —
  хостовый return address stack предсказывает их идеально. Для валидации гостевого
  адреса возврата ведётся отдельный "Rosetta Return Stack" (виден в vmmap как
  отдельный 4K регион): при call кладутся (x86 return addr, translated target),
  при ret сверяются; промах → resolve через карту.
- **RIP-relative адресация** = `ADRP+ADD`. Раскладка памяти гарантирует досягаемость:
  `[x86 code][data][translated AOT][runtime]` — AOT сразу после оригинала, runtime
  следом; `ADRP` ±4 ГБ покрывает и данные оригинала (константы читаются прямо из
  исходного x86 __TEXT, endianness тот же), и runtime-routines (прямые `BL`).
- **Lazy binding импортов** — двойной: штатный `__la_symbol_ptr` чинит dyld, а
  секция `__stubs_sh` в AOT — своя пара (x86 addr, arm64 translated addr),
  заполняемая `resolve_x64_addr` при первом вызове; дальше прямой `br x22`
  (Champollion part1, §lazy binding).
- **Флаги**: `FEAT_FlagM` (CFINV/RMIF/SETF8/SETF16) + `FEAT_FlagM2` (AXFLAG/XAFLAG,
  формат которых «по странному совпадению» = x86 FP-флаги). Каноническая форма CF —
  инвертированная (subtract-with-borrow), CFINV после каждого ADD. PF/AF считает
  недокументированное расширение M1 (NZCV[27:26]), включаемое на потоке только под
  Rosetta; в Linux-VM его нет — там PF считается программно (5 инструкций
  через NEON `cnt`), что только усиливает ценность dead-flags оптимизации.
- **FP**: обработка NaN/tininess «до/после округления» через аппаратный режим
  (на M1 — нестандартный, предшественник FEAT_AFP). x87 80-bit — полная программная
  эмуляция (медленно, корректно).
- **TSO**: аппаратный режим на чипе (как у NVIDIA Denver/Carmel, Fujitsu A64fx);
  обычные ARM load/store получают x86-гарантии при включённом бите. Нам закрыт
  (приватный entitlement) — принимаем как данность, но фиксируем: у Rosetta цена
  модели памяти ≈ 0 инструкций.
- **SMC/JIT внутри гостя**: JIT-генерируемый гостевой код (JavaScript, Mono)
  идёт через `translate_indirect_branch` → red-black tree поиск перевода → при
  промахе `translate()` (полный декодер; ROSETTA_PRINT_IR показывает BB-структуру
  с preds и live flags `#nzcvpa`). Т.е. Rosetta не боится динамического кода:
  маппирование сделано ветвистым поиском по гостевому адресу, а не по хостовому.
  Явных следов «версионирования страниц» в публичных источниках нет —
  **[ГИПОТЕЗА]**: для JIT-регионов Rosetta полагается на invalidation через
  `pthread_jit_write_protect_np`-механику и внутренний tree-lookup, который
  повторно резолвит адрес после изменения; точный механизм не опубликован.

### Ответы Rosetta на наши вопросы

1. **Сцепление**: его у Rosetta НЕТ в нашем смысле — каждая инструкция валидная
   точка входа, состояние канонично на каждой границе [SRC]. «Цепочка» ей не нужна,
   потому что цена выхода после блока ≈ 0: ret через хостовый RAS, indirect через
   кэшированную hash-map. Замечание для нас: Rosetta решает задачу
   «состояние при фолте» отказом от блок-уровневого снапшота вовсе — состояние
   всегда консистентно, откатывать нечего.
2. **Стоимость входа в блок** [ГИПОТЕЗА, следует из 1:1]: у AOT-кода вход =
   0 инструкций обвязки (прямой fall-through внутри транслированной функции;
   между функциями — обычный `BL`). Для JIT-блока вход = обновление `__stubs_sh`-
   слота и `br` (~4–8 инструкций по Champollion).
3. **AOT-преимущество на старте**: предтрансляция всего `__TEXT` при первом запуске +
   дисковый кэш навсегда + предтранслированный 2.4 ГБ aot_shared_cache для системных
   библиотек → у типичного macOS-приложения JIT почти не вызывается. Прямой перенос
   «предтранслированного кэша» нам ограничен: игры несут свой код, но **статический
   wine/unityplayer DLL-набор кэшируем точно так же** [ГИПОТЕЗА].
4. **SMC**: см. выше; публично подтвержден только tree-based резолв с переводом при
   промахе [SRC].
5. **Приёмы, которых у нас нет вообще**: (a) прямая замена CALL/RET на BL/RET с
   отдельным validation-стеком (мы помещаем гостевой ret через общий диспетчер);
   (b) dead-flags elimination на уровне AOT-прохода по CFG; (c) prologue/epilogue
   combining в stp/ldp; (d) двойной fault-free дизайн — нет sigsetjmp/siglongjmp
   в горячем пути; (e) дисковый AOT-кэш с атомарным rename.

## 2. Prism (Microsoft, Windows 11 on ARM)

(пусто)

## 3. FEX-Emu

Главный источник: код, `FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp`
(FEX-2607, main @ 14 817 коммитов) [CODE] + `docs/SourceOutline.md` [SRC].

### Архитектурный скелет [CODE]

- «Splatter» codegen: генератор ARM64 по одному IR-опу за макрос, без isel.
  IR ≈ SSA, близка к ARM64. Фрагмент = набор BB (может быть целая функция).
  Движок: `Frontend.cpp` (декодер, multiblock CFG) → `OpcodeDispatcher.cpp`
  (x86→IR, «no-pf opt», «local-flags opt») → IR-пассы
  (`RedundantFlagCalculationElimination`, `RegisterAllocationPass`, валидация) →
  `JIT/*.cpp` (splatter-эмиттер) → `LookupCache` (L1/L2).
- **SRA = Static Register Allocation** (аналог Rosetta x0–x15): каждый гостевой
  рег и состояние PF/AF статически назначены фиксированным хост-регистрам на всю
  жизнь JIT-блока. `SignalDelegatorConfig.SRAGPRMapping/SRAFPRMapping` отдаёт
  карту `host_reg_idx → guest_reg_idx` сигнальному делегатору: при SIGSEGV в JIT
  обработчик *восстанавливает гостевой контекст, переписывая прерванный хост-PC
  на точку LoopTop* и перелив статики обратно в CpuStateFrame — никаких
  per-block снапшотов вообще. Это ответ FEX на наш главный блокер:
  **правильная точка отката = вершина диспетчер-цикла, а не граница блока**.
  (Dispatcher.cpp:2646–2690 `MakeSignalDelegatorConfig`; точки
  `AbsoluteLoopTopAddress`, `AbsoluteLoopTopAddressFillSRA`,
  `SignalHandlerReturnAddress`, `ThreadStopHandlerAddressSpillSRA` и т.д.)
- LoopTop диспетчера — руками выписанный ARM64, 16-байт выравнивание:
  1) `ldr RIP` из `CpuStateFrame.State.rip`;
  2) TF-flag → single step;
  3) **L2 lookup**: `Index = (rip & (VirtualMemSize-1)) >> 12` → `L2Pointer[page]`
     → `LookupCacheEntry` через **один LDP** (грузит сразу {host_code, guest_pc}
     парой); сверка guest_pc для алиасинга, нулевой host → `NoBlock` →
     `CTX->CompileBlock(RIP)` → прыжок в блок через `br`. L1-кэш (прямая
     маппированная, `stp {host, pc}` парой) обновляется при L2-hit — следующая
     итерация этого же PC попадает в 3–5 инструкций L1-touch.
  Итого цена промаха до существующего блока ≈ **~20 инструкций на блок**;
  без компиляции: ~12–18. На 34-инструкционный блок это **<60 %**, не 15–20×.

### Что важно для нашего chaining-блокера [CODE]

- FEX не делает snapshot-per-dispatch вовсе. «Снапшотом» служит сам факт, что
  внутри JIT все гостевые регистры — либо в статических хост-регах (SRA), либо
  уже записаны в `CpuStateFrame`. При host SIGSEGV управление улетает в
  системный обработчик, который по хост-PC классифицирует: в JIT — перелив SRA
  назад во фрейм, перепись PC на `AbsoluteLoopTopAddressFillSRA`, возврат.
  Ключевое: частично исполненный *guest-блок* НЕ откатывается; его побочные
  эффекты (записи в память) — уже валидная работа, а гостевой контекст на момент
  *начала фолтящей инструкции* восстанавливается из SRA-таблицы. Гранулярность
  отката — **одна гостевая инструкция**, не dispatch и не chain.
- `SignalHandlerReturnAddress` использует `hlt(0)` как легальную точку фолта
  (gdb ловит SIGTRAP, поэтому SIGILL через `hlt`, а для SIGSEGV — запись по
  нулевому указателю через `ldr x1, [0]`), то есть возврат из обработчика
  сигнала нарочно вызывает новый фолт, который диспетчер классифицирует.
- `SpillStaticRegs(TMP1)`/`FillStaticRegs()` — парные загрузки статиков при
  выходе в C-мир и обратно; цена ~16 ldp/stp. Это платится только на
  compile/sleep/stop/EC-переходах, НЕ на обычном диспетче.
- `ENTRY_FILL_SRA_SINGLE_INST_REG` — принудительный single-step для SMC:
  если хост заметил запись в исполняемый регион, следующий `LoopTop` зайдёт с
  флагом, скомпилирует РОВНО одну гостевую инструкцию, тем самым инвалидирует
  перезаписанный блок и даст новой записи «осесть». Inline-SMC, без FNV-хэша.
- `REG_CALLRET_SP` — аппаратная call/ret-подсказка: отдельный стек для
  guest CALL/RET (сохранение guest-адреса возврата), обслуживается в
  диспетчере и на EC-входе (возврат через `ldp/cmp/ret`, без ухода в C-код
  при попадании). Аналог Rosetta Return Stack, но проще.

### Block-linking = наше «сцепление», но правильно [CODE, JIT.cpp]

- Каждый блок завершается **jump-thunk**-ом (16 байт, в конце блока после выравнивания):
  `b .+8; br TMP1; …; dc64 HostCode=0; dc64 GuestRIP; dc64 CallerOffset`.
  На первом проходе `br TMP1` уводит в общий ExitFunctionLinker.
- `Arm64JITCore::ExitFunctionLink(Frame, Record)` вызывается один раз per edge:
  если TF — вернуться в LoopTop (без link), иначе `LookupCache->FindBlock(GuestRip)`,
  при промахе `CompileBlock`. Затем под `LookupCache->AcquireWriteLock` +
  `GuardSignalDeferringSection<shared_lock>(CodeInvalidationMutex)`:
  - если host-адрес цели укладывается в ±128 МБ (`IsInt26`): **атомарная перезапись
    call-site** одной инструкцией `b HostCode` (или `bl`, если перед thunk была
    метка-кандидат на CALL) + `ClearICache(4 байта)`. Через `AddBlockLink`
    регистрируется **делинкер** `DirectBlockDelinker`, который вернёт вызов назад
    на thunk при инвалидации цели.
  - если не помещается: пишет 8-байтный `Record->HostCode` (seq_cst) + `dc cvau /
    dsb ish`, а thunk остаётся «косвенным» (`ldr TMP1, [Record->HostCode]; br TMP1`),
    делинкер `IndirectBlockDelinker` восстанавливает первые 2 инструкции thunk-а.
- Прямой link получается **zero-overhead**: chained-ребро после линковки — одна
  инструкция `b HostCode` на блок. Цена перехода между блоками = **1 ARM64
  инструкция + i-cache**. Сравни с нашими ~350–500 инструкциями + 1.5 КБ трафика.
- Race-freedom достигается атомарной записью 4-байтной инструкции на выровненном
  адресе + code invalidation блокирует делинкеров пока линкующий поток пишет.
  Никаких sigsetjmp/siglongjmp в горячем пути.

### Как FEX отвечает на наш блокер «снапшот на dispatch vs на блок» [CODE, JIT.cpp]

- Снапшота нет вообще. Вместо него — **RIP reconstruction по JITCodeTail**:
  в конце каждого JIT-блока `JITCodeTail { RIP, GuestSize, SpinLockFutex,
  SingleInst, Size, NumberOfRIPEntries, OffsetToRIPEntries }` + таблица
  `vl64pair` пар «(дельта host-PC от предыдущей записи, дельта guest-RIP)».
  Одна запись на каждый *guest-opcode entry*. Host-PC фолтящей инструкции
  линейно свёртывается в конкретный guest RIP. «Фолт редок, а размер важен» —
  поэтому кодируется vl64.
- На фолте делегатор (SignalDelegator) по host-PC находит JIT-блок, по этой
  таблице восстанавливает точный гостевой RIP **момента фолта** (не начала
  блока, не начала диспетча), переливает SRA-статики в `CpuStateFrame`,
  переустанавливает PC в `AbsoluteLoopTopAddressFillSRA` и возвращается.
- Guest-работа, успевшая выполниться до фолтящей инструкции, **не откатывается**
  — её эффекты (записи в память) остаются, что и требуется для корректности.
  Гранулярность отката = **одна гостевая инструкция**, не dispatch и не chain.
  Это в точности закрывает наш дефект из CHAIN-SNAPSHOT-GRANULARITY.
- Дополнительно: `SpillSlots` (по нужде IR) вместо сохранения всего фрейма —
  `sub rsp, rsp, SpillSlots*MaxSpillSlotSize` на входе, и ничего на exit-link.
  `EmitEntryPoint` пишет только 8-байтный `InlineJITBlockHeader` ptr (2 инстр.)
  + опциональный TF-check; никакого `sigsetjmp`, никакого `memset(790)` fault-фрейма.

### LookupCache структуры и инвалидация [CODE, LookupCache.{h,cpp}]

- **L1** (thread-local): прямая маппированная таблица (pow2, 8K→1M записей),
  `LookupCacheEntry{host, guest}` = 16 Б, lookup `Index = rip & mask` — одна
  `ldp`, одна сверка `guest`. Динамически масштабируется раз в секунду
  по числу L2/L3-promote. Промахи L1 идут в L2.
- **L2** (thread-local): «таблица страниц»: `PagePointer[(rip>>12) & pages]`
  даёт указатель на массив 4096/16=512 записей на страницу гостя. Выделяется
  лениво по 8 КБ на страницу, общий лимит 128 МБ, при исчерпании — полный
  `ClearL2Cache` + перекомпиляция. Lookup — `ldr PagePointer[page]`, `cbz`,
  `LookupCacheEntry ldp`, сравнение guest: ~8 инструкций.
- **L3 / Shared** (процессный): `robin_map<uint64_t, BlockEntry>` + `WritePriorityMutex`.
  Чтение под shared_lock, запись редкая (при компиляции).
- **CodePages**: `map<page_idx, vector<guest_block_addr>>` — обратный индекс
  «какие guest-блоки пересекают эту страницу». `InvalidateRange(start,len)`:
  находит все страницы, для каждой — `Erase(block)` (вызывает все делинкеры
  входящих рёбер, заточенных на этот блок) + чистит L1/L2 через `InvalidateCache
  (Address)`. Это и есть механизм, которым FEX гасит **self-modifying code** без
  per-block FNV-хэша: запись в гостевую кодовую страницу ловится не hash-ем, а
  защитой страницы (или mprotect-hook) → InvalidateRange по `mprotect/madvise`
  + явный `ClearICache` после перезаписи.
- Отдельный `BlockLinks` `pmr::map` на monotonic_page_buffer_resource с
  «одним нажатием» сбрасывает все ребра, когда блок удалён (без поэлементного
  уничтожения).

### Итоги FEX по пяти вопросам

1. Сцепление: прямая `b` после однократного link + делинкеры на инвалидацию;
   внутри JIT нет какого-либо C-кода [CODE].
2. Стоимость входа: L1-hit ≈ **1 `ldp` + 1 `cmp` + 1 `br`** (3–5 инструкций);
   L2-hit ≈ 8; промах + compile — сотни, но редко [CODE].
3. AOT/JIT: FEX — чистый JIT, но большой code cache (128 МБ L2 + 16 МБ L1)
   и «CompileBlock += Size»-фрагментная компиляция; существует ЭКСПЕРИМЕНТАЛЬНЫЙ
   persistent code cache между запусками (упомянут в Readme) [SRC].
4. SMC: страница-гранулярная инвалидация через `InvalidateRange`, блок —
   `Erase` + делинкеры входящих рёбер. **Ноль** постоянного хэширования [CODE].
5. Чего у нас нет: JITCodeTail RIP-reconstruction таблица; делинкеры как
   функции на BlockLinkTag; L1/L2 разделение thread-local vs shared;
   CodeInvalidationMutex+GuardSignalDeferringSection вместо sigsetjmp;
   прямая `b`-link с atomic single-instruction patch.

## 4. box64

(пусто)

## 5. QEMU TCG (эталон-антипример)

(пусто)

## 6. Сравнительная таблица по пяти ключевым вопросам

(пусто)

## 7. Что применимо к нам, ранжировано по ожидаемому выигрышу

(пусто)

## Приложение. Чек-лист покрытия

| Вопрос | Rosetta | Prism | FEX | box64 | QEMU |
|---|---|---|---|---|---|
| 1. Сцепление + фолт | — | — | — | — | — |
| 2. Стоимость входа | — | — | — | — | — |
| 3. AOT/JIT старт | — | — | — | — | — |
| 4. SMC | — | — | — | — | — |
| 5. Новые приёмы | — | — | — | — | — |
