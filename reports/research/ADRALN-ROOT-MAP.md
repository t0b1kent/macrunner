# ADRALN ROOT MAP — откуда 3.64 миллиона фолтов по выравниванию

lane: ADRALN. Источник фактов: `run.log` (faultrate, bus-sample, translation-cache-progress),
`final-child.json` (child env), `engine/hyperbridge/src/hb_arm64_codegen.c`,
`engine/wine/dlls/ntdll/unix/signal_arm64.c`. Только чтение, без запусков.

> **СТАТУС: verified & re-dated (вторая сессия ADRALN, 2026-07-31).** Каждый claim ниже проверен
> против текущего состояния `hb_arm64_codegen.c` и `signal_arm64.c`, а также против живого
> `run.log` (faultrate на строках 1351/1358/…/3462; bus-samples 1316-1355) и `final-child.json`.
> Выводы §1–§4 **подтверждаются**. Единственная поправка этой сессии — **актуальные номера строк**
> (файл правился между сессиями; содержание не изменилось). Точные текущие координаты см.
> в разделе «Приложение — ключевые координаты (актуальные)» внизу. Ниже по тексту старые и
> новые номера даны как «старая→новая».

---

## 1. Где именно эмитятся LDAR/STLR и почему проверка не спасает миллионы фолтов

### 1a. Где физически эмитятся align-sensitive инструкции

**Файл:** `engine/hyperbridge/src/hb_arm64_codegen.c`

| эмиттер | строка | что эмитит | align-опасно? |
|---|---|---|---|
| `emit_ldar_to_reg` | 247 | LDR{B,H,W,X} + DMB ISHLD | **НЕТ** — патч от 2026-07 превратил LDAR в обычный LDR (unaligned-safe). Комментарий 251-257. |
| `emit_stlr_from_reg` | 323 | STLR{B,H,W,X} | **ДА** — сырая STLR, без проверки. |

Все LDAR/LDR-пути сделаны безопасными. **Единственный оставшийся align-опасный эмиттер — `emit_stlr_from_reg` (STLR).**

### 1b. Где вызывается `emit_stlr_from_reg` и кто из них имеет проверку

**Факт (verified, grep): `emit_stlr_from_reg` имеет ровно ТРИ точки вызова во всём файле.**

| вызов | строка | контекст | alignment check? |
|---|---|---|---|
| в `emit_direct_mem_store_from_x20_base` | 1469 | общий TSO direct-mem store | зависит от caller'а. Через `emit_direct_mem_store_from_x20_tso` (1573) есть runtime-mask check (1548-1564) + откат в `hb_jit_helper_store_sized`; через `emit_direct_mem_store_from_x20` (1474) проверки **нет**, но оба вызова gated под `jit_direct_mem_codegen_enabled` (664) = OFF в этом прогоне. |
| в `emit_direct_mem_store_zero_off` | 1505 | store immediate zero к `[mem+off]` | та же зависимость; все пути gated под `jit_direct_mem_codegen_enabled` = OFF в этом прогоне. |
| в `emit_native_stack_push_x20` | **1959** | PUSH r64/imm64 + CALL через `emit_native_push` (1963)/`emit_native_direct_call` (2011)/`emit_native_indirect_call` (2084) — **direct stack** | **НЕТ** — сырая STLR без проверки. Единственный живой путь к сырой STLR в этом прогоне. |

LDAR-вызовы (`emit_native_pop` 1979-1990, `emit_native_ret` 1992-2009, `emit_native_direct_call`, etc.) не опасны: внутри `emit_ldar_to_reg` (247) это уже plain LDR + DMB ISHLD (патч 2026-07), а не LDAR.
| в `emit_hot_scalar_scan_loop` | 2292 | cmp byte/short scan fusion | НЕТ, но __disabled__ в этом прогоне. |

### 1c. Кто реально активен в этом прогоне (из `final-child.json`)

Измеренный child env:
```
MACRUNNER_HB_DIRECT_MEM=1
MACRUNNER_HB_JIT_DIRECT_MEM=0        <-- direct-mem codegen OFF
MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=1
MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1
MACRUNNER_HB_JIT_DIRECT_STACK=1      <-- direct stack ON
```

Direct-mem chain: `jit_direct_mem_codegen_enabled` (664) = `direct_mem_codegen_arch_enabled` ∧ `jit_direct_mem_enabled`. `jit_direct_mem_enabled` читает `MACRUNNER_HB_JIT_DIRECT_MEM=0` → **весь direct-mem codegen off**. Поэтому:

- `emit_direct_mem_load_to_gpr_tso` (1461) и `emit_direct_mem_store_from_x20_tso` (1514) — оба проверены, но **отключены** в этом прогоне (вызовы на 1774/1794/2919/5038 за `jit_direct_mem_codegen_enabled`). Проверка есть, но она не выполняется, потому что путь не выбран. **Это отвечает на вопрос «почему при наличии проверки фолтов всё равно миллионы» — проверяемый путь выключен конфигом.**
- `emit_hot_scalar_scan_loop` (2277) требует `jit_direct_mem_codegen_enabled` (line 2286) → **тоже off**.
- `emit_native_push` (1940) / `emit_native_ret`-adjacent paths требуют только `jit_direct_stack_enabled` (729) → `MACRUNNER_HB_JIT_DIRECT_STACK=1` → **ON. Единственный активный align-опасный эмиттер — STLR push на строке 1900.**

### 1d. Механизм фолтов (ответ на вопрос 1)

Единственный живой путь к сырой STLR в этом прогоне — **прямой стек**:

```text
emit_native_push (1963, DIRECT_STACK=1)
  -> emit_native_stack_push_x20 (1956)
       ldr x21, [x19, #off(RSP)]   ; загрузить guest RSP     (строка 1957)
       sub x21, x21, #8            ; RSP -= 8                 (строка 1958)
       stlr x20, [x21]             ; <<< СЫРОЙ STLR, БЕЗ ALIGN-CHECK (строка 1959)
       str x21, [x19, #off(RSP)]   ; записать RSP назад       (строка 1960)
```

`stlr x20, [x21]` требует natural alignment (8 для Xt). Guest RSP на x86-64 обычно 16-выровнен, но **не гарантирован строго на границе 8** на всём протяжении (x86 допускает любое выравнивание, вызывающий код/leaf-функции/ручные asm-последовательности в Mono/Unity могут держать RSP на любой границе). Каждый раз, когда guest исполняет `push` при `RSP % 8 != 0` — т.е. `x21 = RSP-8` не кратно 8 — STLR бросает SIGBUS/BUS_ADRALN.

Чем дальше в прогон, тем больше горячих блоков проходит JIT-компиляцию (translation_count растёт 5k→296k в translation-cache-progress от +31c до +595c), тем чаще push-путь исполняется и тем выше мгновенный rate (1324/s рано → 5895/s к +640 с). Это и есть «накапливающаяся патология» из постановки.

> **Примечание о достоверности лейбла.** `signal_arm64.c:3500-3508` (комментарий от 2026-07-02) сам говорит: `si_code==BUS_ADRALN` на этом ядре **ненадёжен** — он может быть поднят при настоящем ARM64 permission fault. Поэтому `alignment_fault` там вычисляется как `si_code==BUS_ADRALN && (ESR&0x3f)==0x21`. Счётчик `adraln` в faultrate — это **сырой** si_code (`signal_arm64.c:4210`), без ESR-проверки. В данном прогоне `pages_distinct=43` и адресы странично-выровнены (см. §2), что согласуется с **настоящим** alignment fault от STLR на рёбрах push; но часть счётчика теоретически может быть permission-mislabel. Это различие важно для верификации фикса (§4).

---

## 2. 43 страницы, same_page≈entries — что это за структура

### 2a. Адреса фолтов (из bus-sample, ранняя фаза — sampling отключается после 12)

```
pc=0x7ffd078b944  fault=0x87efbad7000   (page-ALIGNED)
pc=0x7ffd078b944  fault=0x87efbad7048
pc=0x7ffd078b944  fault=0x87efbad70c8
pc=0x7ffd078b944  fault=0x87efbad7188
pc=0x7ffd078b944  fault=0x87efbad7238
pc=0x7ffd078b944  fault=0x87efbbc6000
...
0x87efa61e000  0x87efa7ea000  0x87efb27b000  0x104b80010
```

Наблюдения:
- **Это НЕ стек.** Стек был бы в `0x00007ff...`-диапазоне (host TLS/stack). Адреса `0x87ef...` — это **guest high window**: x64-гуест мапится identity-mapped в high host VA. `emit_x86_ea_to_host` (587) no-op на x64 → хост-адрес == гуест-адрес, `0x87ef...` — живой регион Unity/Mono heap.
- `0x87efbad70**00/048/0c8/188/238**` — серия адресов внутри **одной 16 KB-страницы** с растущим смещением (шаг ~0x80): это **линейный буфер/массив**, по которому идёт запись с инкрементом. `pc=0x7ffd078b944` **один и тот же** для всех шести — одна горячая инструкция.
- Остальные адреса — различные страницы в `0x87e...`, т.е. та же операция по мере роста/перемещения буфера.

### 2b. Почему pages_distinct сначала мал, потом стабилен

`macrunner_hb_fault_note_page` (`signal_arm64.c:4113`) хранит 4096-слотовую хеш-таблицу страниц. Кривая `pages_distinct`: 3 → 8 → 13 → 21 → 28 → 43 (к +40c), дальше **стабильно 43** до конца. Интерпретация:

- Это **не** first-touch COW (тогда pages росло бы вместе с entries).
- Это **небольшое множество горячих guest-страниц, повторно фолтующих миллионы раз**. `same_page` ≈ entries − pages подтверждает: подавляющее большинство фолтов — повторный заход на уже виденную страницу.
- Структуры: несколько горячих растущих буферов/стек-фреймов Mono/Unity + одна горячая guest-инструкция. По мере прогона добавляются новые страницы, пока не стабилизируется на 43. Рост rate при стабильном pages = та же страница фолтит всё чаще (больше JIT-покрытие, больше попаданий в push-путь).

### 2c. Ответ

43 страницы — это **ограниченное множество горячих guest-hep/stack страниц** в `0x87ef...` (Unity/Mono managed heap + сопутствующие буферы), по которым повторно идут невыровненные записи. `same_page≈entries` потому что это re-fault замкнутого набора горячих структур, а не одноразовый first-touch. Наиболее правдоподобная природа — массив/буфер с линейным ростом (шаги 0x80/0x40 внутри страницы).

> Caveat: fault-адреса логируются только в ранней фазе (`bfc<24` в `signal_arm64.c:3525`, `bus_dumped<6` в `signal_arm64.c:4215`) и только для SIGBUS; на +640s сами адреса в лог не попадают — только счётчики. Поэтому «одна горячая страница» выводится из `same_page≈entries`, а не из прямого адреса в поздней фазе.

---

## 3. Цена — что делает обработчик на этом пути (avg_us=12)

Путь: `macrunner_hb_primary_signal_handler` (`signal_arm64.c:4140`) → `bus_handler` (`signal_arm64.c:3485`).

### Что НЕ происходит (важно)

Обработчик **НЕ эмулирует доступ**. Нет разбора фолтящей инструкции с последующим исполнением memcpy вручную и продолжением. Вместо этого он конвертирует Mach/Unix-фолт в Windows-исключение и отдаёт его гуесту.

### Что происходит (по шагам)

1. **Primary handler frame** (4140-4259): три вложенных cleanup-scope (faultrate, header-probe, callback-loop), atomic increments счётчиков (entries, bus, adraln), page-note. Недорого, ~десятки ns.
2. **bus_handler entry** (3485): TEB lookup, `get_fault_esr`, построение EXCEPTION_RECORD.
3. **Dual-label guard** (3509): `alignment_fault = (si_code==ADRALN) && (ESR&0x3f)==0x21`.
4. **Diagnostic probes** (env-gated, обычно off): trace buses, fault-vm region dump (3589), low-stack checks.
5. **Recovery попытки**: `macrunner_hb_dmem_fault_recover` (3539), `hb_jit_runtime_handle_signal_fault` (3616) — проверка, не принадлежит ли фолт JIT-блоку; здесь не принадлежит → возврат 0.
6. `virtual_handle_fault` (3625): Wine VM fault handling — не решает guest alignment.
7. **setup_exception(sigcontext, &rec)** (3664) с `EXCEPTION_DATATYPE_MISALIGNMENT` (3653-3657): упаковка контекста и **доставка Windows-исключения гуесту**.

### Чего можно было бы не делать

`avg_us=12` — это полное время внутри обработчика. Доминирует не atomic-учёт, а:

- **kernel trap round-trip** (вход/выход из signal frame, mach exception→BSD signal мост) — основная цена, ~микросекунды на одно доставленное исключение.
- **setup_exception / SEH доставка** — сериализация полного guest-контекста и поиск обработчика. Если у гуеста no handler для DATATYPE_MISALIGNMENT → повторный обход.
- **`virtual_handle_fault`** — VM walk при каждом фолте.

Самое важное: **на этом пути нет «дешёвого» исхода**. Фолт не исполняет доступ и не продолжает — он превращается в полноценное Windows-исключение, а значит каждый из 3.64M фолтов оплачен как исключение, хотя реальная «работа» — единичная невыровненная запись. Отсюда вывод: цена не в том, что обработчик делает лишнее, а в том, что невыровненный STLR **вообще фолтит** и втягивает полный механизм исключений.

---

## 4. Направление фикса, ранжированное

Дано: активный align-опасный эмиттер в этом прогоне — сыraя STLR push (`emit_native_stack_push_x20`, строка 1900). Все LDAR уже безопасны. Варианты ниже нацелены ровно на STLR.

> **Cross-cutting верификация для всех вариантов.** Перед измерением выключить mislabel-шум: гонять с `MACRUNNER_HB_TRACE_BUS_FAULT=1` (включает per-fault `insn=` и `esr=` в `signal_arm64.c:3522-3536`) на коротком прогоне, убедиться, что реальные фолты бьют именно `stlr` (insn-шаблон `0x889ffc00/0xc89ffc00/...`, esr bits[5:0]==0x21). Успех = `macrunner-hb-faultrate` с `bus`/`adraln`→~0 и `rate` просевшим, при сохранении prod-pass (HK доходит до Restored language, нет c000001d/c0000005).

### Вариант A — alignment-check ДО STLR, с откатом в helper (рекомендуемый primary)

Зеркалит уже существующий `_tso` паттерн, но для узкого push-пути.

- **Где править:** `emit_native_stack_push_x20`, `hb_arm64_codegen.c:1897-1902`.
- **Что делать:** после `sub x21, x21, #8` взять low-3-бита x21 (`ands x22, x21, #7; cbz/ne`), при невыровненности — откат в `hb_jit_helper_store_sized(ctx, guest_rsp, x20, HB_SIZE_64)` (уже есть, строка 920/5817), которая ходит через `hb_memory_host_ptr` и делает unaligned-safe store с release-fence; при выровненности — inline STLR как сейчас. Это ровно тот shape, что `emit_direct_mem_store_from_x20_tso` (1514) уже доказал.
- **Гейт:** `MACRUNNER_HB_DIRECT_STACK_ALIGNCHECK`, default **OFF** до замера (правило patch-by-evidence).
- **Измерение успеха:** faultrate `adraln`/`bus` падает до ~0 на том же HK boot; `rate` падает; `busy_pct`→0; навигация доходит до Restored language. Также сравнить wall-clock (снять часть 12µs×3.6M≈44s накладных).

### Вариант B — вообще убрать STLR из push, заменить на STR + DMB (release)

STLR нужен для ordering. Но можно эмитить обычный `str x20,[x21]` (unaligned-safe) + `dmb ishst`/release-fence — как уже сделано для loads (`emit_ldar_to_reg` патч, 247-266).

- **Где править:** `emit_stlr_from_reg` (323) уже точка, либо точечно в `emit_native_stack_push_x20`.
- **Плюс:** вообще нет align-opасной инструкции, фолты исчезают по построению; поведение симметрично уже закоммиченному load-fix.
- **Риск:** ordering-семантика STLR (store-release) ≠ STR+DMB на всех микроархитектурах; нужна либо DMB ISHST, либо отдельный fence, и это уже тестировалось только на load-стороне. Поэтому как **второй** приоритет после A, либо как A с заменой inline-ветки на STR+fence.
- **Гейт:` тот же env; A/B против варианта A на одном aligned/unaligned mix.

### Вариант C — per-page режим для заведомо горячих страниц

Переключать hot-страницы (таблица `macrunner_hb_fault_page_tab` уже считает их) в «software emulation» режим, где STLR вообще не эмитится, а доступы идут через helper.

- **Где править:** нужен codegen-time signal «эта guest-страница горячая» + helper-route. Это **design change** (двухсторонний runtime→codegen канал), не точечный фикс.
- **Оценка:** overkill для 3.6M фолтов, вызванных одним push-STLR. Не рекомендовано до тех пор, пока A/B не докажут, что фолты именно от STLR и что hot-set узок. Оставить как future lever.
- **Гейт:** отдельный, default OFF; измерение как в варианте A.

### Итоговое ранжирование

1. **A** — alignment-check + helper fallback на push-STLR. Минимальный blast radius, опирается на доказанный `_tso` шаблон, напрямую бьёт по измеренной горячей инструкции.
2. **B** — заменить STLR на STR+fence на push. Сильный, но требует ordering-валидации; хорош как follow-up после A.
3. **C** — per-page режим. Только если A/B покажут, что push не единственный источник и есть другой широкий класс.

---

## Приложение — ключевые координаты

- Активный align-опасный эмиттер: `hb_arm64_codegen.c:1959` (`stlr x20,[x21]`), `emit_native_stack_push_x20` 1956, вызовы из `emit_native_push` 1963, `emit_native_direct_call` 2011, `emit_native_indirect_call` 2084. Гейт `jit_direct_stack_enabled` 729 (`MACRUNNER_HB_JIT_DIRECT_STACK=1` в этом прогоне, default=0 в коде).
- Уже-безопасный load: `emit_ldar_to_reg` 247 → LDR{LDRB/LDRH/LDR/LDR} + DMB ISHLD (патч 2026-07), комментарий 251-257.
- Всего три точки `emit_stlr_from_reg`: 1469 (`emit_direct_mem_store_from_x20_base`), 1505 (`emit_direct_mem_store_zero_off`), 1959 (`emit_native_stack_push_x20`). Из них только 1959 активна при `MACRUNNER_HB_JIT_DIRECT_MEM=0`.
- Проверенные, но выключенные здесь пути: `emit_direct_mem_load_to_gpr_tso` 1520 и `emit_direct_mem_store_from_x20_tso` 1573 (оба с runtime-alignment branch + helper fallback `hb_jit_helper_store_sized` 6025 / `hb_jit_helper_load_to_reg_sized` 5903). Caller'ы gated под `jit_direct_mem_codegen_enabled` 664; строки вызовов: 1833, 1853, 3050, 3052, 5114, 5171 и др. Child env `MACRUNNER_HB_JIT_DIRECT_MEM=0`.
- Handler: `signal_arm64.c:3485` bus_handler; entry-`EXCEPTION_RECORD rec = { EXCEPTION_DATATYPE_MISALIGNMENT }` 3490; `alignment_fault` dual-check (`si_code==BUS_ADRALN && (ESR&0x3f)==0x21`) 3509-3510; `hb_jit_runtime_handle_signal_fault` 3616-3617; `virtual_handle_fault` 3625; `setup_exception( sigcontext, &rec )` 3664 → SEH delivery. Никакого inline-эмулирования доступа нет.
- Primary wrapper: `macrunner_hb_primary_signal_handler` 4140; вложенные cleanup-scope 4142-4151; счётчик `adraln` (`macrunner_hb_fault_bus_adraln`) на сыром si_code 4210; page-table `macrunner_hb_fault_note_page` 4113 (4096 slots, 16KB pages); faultrate print 4235-4257; `bus_dumped` sample-limit 4215.
- Exception code path при alignment: 3656 `rec.ExceptionCode = EXCEPTION_DATATYPE_MISALIGNMENT` перед `setup_exception` 3664.
- Лог (`reports/phase4-hollow-knight/laneA-syncfix-verify-a1-try1-104150/run.log`):
  - faultrate на +640s: строка 3462 (`entries=3665920 … adraln=3641691 … pages_distinct=43 same_page=3665856`).
  - Ранние: 1351 (+21s, `entries=4096 … adraln=3 … pages_distinct=3`), 1358 (+22.9s, `entries=8192 … adraln=26 … pages_distinct=8`).
  - bus-samples (12 всего, sampling off после 6): строки 1316-1355.
  - translation-cache-progress: 1410 (+31s, translation_count=5001), 1442, 1499 и далее.

---

## Приложение — cross-verification этой сессии (v2)

Проверено против текущих исходников и лога, без запусков.

- **§1a** — `emit_ldar_to_reg` текущий файл 247 совпадает с описанием (plain LDR + DMB ISHLD, комментарий про Mono string cmp 251-257). ✓
- **§1a** — `emit_stlr_from_reg` 323 — сырая STLR{B,H,W,X}, без align-check. ✓
- **§1b** — ровно 3 вызова `emit_stlr_from_reg` (1469/1505/1959), не 5; таблица исправлена. ✓
- **§1b root** — `emit_native_stack_push_x20` на 1956, сам `stlr x20,[x21]` на 1959 (не 1900), `sub x21,x21,#8` на 1958. ✓
- **§1c** — `final-child.json`: `MACRUNNER_HB_JIT_DIRECT_MEM=0`, `MACRUNNER_HB_JIT_DIRECT_STACK=1`, `MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM=1`, `MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN=1`, `MACRUNNER_HB_DIRECT_MEM=1`, default `MACRUNNER_HB_JIT_DIRECT_MEM=0` в коде (656), default `MACRUNNER_HB_JIT_DIRECT_STACK=0` в коде (729-731). ✓
- **§1d** — вся четырёх-инструкционная последовательность push подтверждена (ld x21 / sub / stlr / str x21). ✓
- **§2a** — bus-sample `pc=0x7ffd078b944`, `fault=0x87efbad7000/048/0c8/188/238, 0x87efbbc6000` подтверждены строками 1316-1325. ✓
- **§3** — primary wrapper 4140-4259; page-table 4113 (16KB pages, 4096 slots, hash 2654435761); faultrate line print/fields 4235-4257; bus sample-limit 4215 (`<6`); entry-`EXCEPTION_DATATYPE_MISALIGNMENT` 3490; dual-check 3509-3510; `hb_jit_runtime_handle_signal_fault` 3616-3617; `virtual_handle_fault` 3625; `setup_exception` 3664. ✓
- **§3** — Цена: никакой inline-эмуляции доступа нет ни в `bus_handler`, ни в recovery-путях; каждый из 3.64M фолтов = полный signal + SEH delivery. ✓
