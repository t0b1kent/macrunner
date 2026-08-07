# DBTACADEMIC LANE — академическая литература по динамической двоичной трансляции

Дата начала: 2026-07-31 (сессия YYYY-MM-DD). Лейн **только читает** (веб + статьи).
Ничего не запускаем, не правим, не коммитим.

Каждая находка помечается **[SRC]** (источник, ссылка), **[CODE]** (чтение кода,
файл/символ) или **[ГИПОТЕЗА]** (вывод/догадка).

## Привязка к нашей данности (не переоткрываем)

Наши три боли, на которые литература должна дать ответы:

1. **Сцепление безопасно.** У нас один слот, покрытие 7.4 %, фолт внутри цепочки
   отбрасывает легитимно завершённые блоки (снапшот на dispatch, не на блок).
   avg_chain=1.0000. Вопросы к литературе: прямое связывание (два слота на
   блок? откат? владение регистрами в цепочке), atomic patch/unpatch, SMC при
   связывании.
2. **Восстановление состояния при исключениях.** 3.67M BUS_ADRALN за прогон —
   откат это ГОРЯЧИЙ путь. Вопросы: precise state в DBT (классика), checkpoint
   granularity (Bala/Transmeta), incremental state update, якоря/харбор-регистры.
3. **Region-based трансляция.** Круг через диспетчер 416 нс: вопросы — как
   литература (UQDBT, HDTrans, DIABLO, LLBT) выбирает регион (trace/superblock/
   tree), критерий host/retarget, издержки перкомпиляции, shared code cache.

Планка контекста: Rosetta 71 % нативной, box64 57 % (7-zip). [SRC: из брифа]

## Канонический список работ к разбору (пополняется)

- [ ] Hewlett-Packard **Dynamo** (Bala, Duesterwald, Banerji, PLDI 2000)
- [ ] **DynamoRIO** overview paper (Bruening, 2004) — исключения/сигналы
- [ ] **UQDBT** (Ung, Cifuentes) — постоянный транслятор, двоичная спецификация
- [ ] **HDTrans** (Sridhar et al., Iowa State) — trace formation, инкрементальная специализация
- [ ] **LLBT** (LLVM-based static binary translation, Shen et al.) — флаги на SSA
- [ ] **QEMU / Tiny Code Generator** (Bellard, USENIX ATC 2005)
- [ ] **Valgrind** (Nethercote, Seward, PLDI 2007) — shadow/jit контекст
- [ ] **DIABLO** (Van Put et al.) — ретаргетируемая линковка/оверлеи
- [ ] **PQEMU / hardware-accelerated QEMU** (Hong et al., USENIX ATC 2012)
- [ ] **CrossBit** (Wu et al.) — инкрементальная трансляция, xc
- [ ] **Virtium / hybrid translation** если попадётся
- [ ] **Box86/64, FEX, Unicorn** — промышленные, но с научными публикациями
- [ ] **Apple Patent US 8,352,797** — execution state register snapshots (Janbe et al.)

## Чек-лист покрытия (в конце отчёта)

- прямое связывание: структура слотов, патч/анпатч, atomicity
- trace vs block vs region: критерии, overhead crossings
- восстановление состояния: полный снапшот? инкрементальный?
- флаги ленивые (наша feature уже есть — литература по верификации)
- SMC / инвалидация кода / code cache management (LRU? region-evict?)
- многопоточная компиляция (hot thread vs translator thread)

---

<!-- наполняется по итерациям -->
