# X86 32-bit Coverage Atlas for KeePass (HyperBridge)

Дата: 2026-05-22  
Промпт: `reports/agent-prompts/CLINE-START-X86-AUDIT-FRESH.txt`

## Краткий вывод

`hb_decode_x86.c` в текущем состоянии **не «почти пустой»**, а покрывает базовый integer/control-flow слой (MOV/ALU/Jcc/CALL/RET/stack/bitwise/shifts subset).  
Но для KeePass как 32-bit GUI app всё равно есть крупные блокеры:

1. **Нет SIMD/SSE/SSE2/MMX в x86 decode/lift path** (фактически отсутствует весь нужный GUI/blit/modern-CRT слой).
2. **Нет string-ops семейства (CMPS/LODS/MOVS/STOS/SCAS) для x86-пути**.
3. **Нет x87 FPU** (legacy/crypto/math risk).
4. x86 lifter (`hb_lift_x86.c`) покрывает только базовые опкоды; остальное уходит в unsupported IR.

Итог: для «KeePass стартует и рисует» нужен staged roadmap: сначала loader/ABI/start-path, затем SIMD/GUI.

---

## 1) Что реально декодирует `hb_decode_x86.c`

По `hb_decode_x86.c` подтверждены decode-ветки для:

- **Data/ALU base**: `MOV`, `LEA`, `ADD`, `SUB`, `CMP`, `TEST`, `AND`, `OR`, `XOR`, `INC`, `DEC`, `NOT`, `NEG`, `SHL/SHR/SAR`.
- **Stack/control-flow**: `PUSH`, `POP`, `CALL`, `RET`, `JMP`, `Jcc` (short/near), `NOP`.
- **Conditionals**: `SETcc`, `CMOVcc`.
- **ModRM groups**: частичное покрытие `0x80/81/83`, `0xC0/C1/D1/D3`, `0xF6/F7`, `0xFF` (subset ext).

При этом в `0x0F` блоке после `Jcc/SETcc/CMOVcc` — основной массив опкодов возвращает `HB_ERR_UNSUPPORTED_OPCODE`.

---

## 1b) ПРОВЕРЕНО (дополнено): integer-core ПРОБЕЛЫ — критичнее, чем «скелет»

Прямая проверка `hb_decode_x86.c` (grep по `HB_INS_*`) — отсутствуют даже базовые
integer-инструкции, которые НЕ опциональны:

| Инструкция | x86 декодирует? | Почему критично |
|---|---|---|
| **MOVZX / MOVSX** (0F B6/B7/BE/BF) | **НЕТ (0)** | Вездесущи (zero/sign-extend). Без них реальный код падает почти сразу. |
| **MUL / IMUL / DIV / IDIV** | **НЕТ (0)** | Любая арифметика. **Crypto KeePass (AES/SHA/bignum) фундаментально требует.** |
| **ADC / SBB** | **НЕТ (0)** | Многословная/carry арифметика (bignum в crypto). |
| **ROL / ROR / RCL / RCR** | **НЕТ (0)** | Ротации — **crypto использует МАССОВО** (SHA/AES/ChaCha). |
| **SHLD / SHRD** | **НЕТ (0)** | Double-precision сдвиги (тоже crypto/optimized). |
| **BT / BTS / BTR / BTC / BSF / BSR / BSWAP** | **НЕТ (0)** | Bit-test/scan/byte-swap — частые в runtime/crypto. |

ВЫВОД: x86 path — НЕ «скелет, готовый к простому коду», а **неполное integer-ядро**.
До string/SIMD надо ЗАКРЫТЬ integer-core (особенно MOVZX/MOVSX — иначе ничего не
поедет, и MUL/DIV/ADC/SBB/ROL — иначе crypto KeePass нерабочий). Это сдвигает
приоритет: **Фаза A0 (новая, P0): integer-core (MOVZX/MOVSX, MUL/DIV, ADC/SBB,
ротации, BT*) ПЕРЕД string/SIMD.**

Замечание по объёму: x86-декодер (861 строка) на порядок меньше x64 — это не «почти
готово», а ранняя стадия. Реалистично x86-путь = крупная отдельная задача (как когда-
то поднимали x64), не доводка.

---

## 2) Сравнение x64 vs x86 (ключевые классы)

| Класс инструкций | x64 умеет? | x86 умеет? | Нужно для KeePass старт/GUI? | Приоритет |
|---|---|---|---|---|
| Базовый integer + control-flow (MOV/ALU/Jcc/CALL/RET/PUSH/POP) | Да | Да (существенно) | Старт: Да | P0 |
| SETcc/CMOVcc | Да | Да | Старт: Да | P0 |
| SIMD/SSE/SSE2/MMX (`MOVDQU/MOVDQA`, `PACK/PUNPCK/PADD/PMUL/...`) | Да (частично по x64 аудиту) | **Нет (x86 path)** | GUI: Да, критично | **P0-P1** |
| x87 FPU (`FLD/FST/FADD/...`) | По текущим аудитам не ключевой x64-path; полного покрытия нет | **Нет** | Старт/crypto/math: Да | P1 |
| String ops (`CMPS/LODS/SCAS/STOS/MOVS`) | Частично в x64 (`SCAS/STOS`, MOVS missing в отдельном аудите) | **Практически нет** | Старт/runtime: Да | **P0** |
| Atomics extended (`CMPXCHG16B`, fences, `PAUSE`) | Частично/ограниченно | Нет | Старт (threading/COM): важно | P1 |

---

## 3) 32-бит специфика и её статус

### Адресация 32-bit ModRM/SIB
- В `hb_decode_x86.c` есть полноценный парсинг ModRM/SIB для 32-bit effective address (base/index/scale/disp), без RIP-relative (и это корректно для 32-bit ISA).

### Сегменты FS/GS (TEB32, SEH fs:[0])
- Интерпретатор учитывает segment-bases при resolve memory (`op->mem.segment == 0x64/0x65` -> `fs_base/gs_base`).
- Но в x86 decode критично наличие корректного capture segment-prefix в опернде для всех нужных форм; это must-test для SEH/TEB путей.

### Stack/calling conventions
- Есть `hb_abi_x86.c` для `cdecl` и `stdcall` (stack-args через `esp`, push return sentinel).
- `thiscall` отдельным ABI-хелпером явно не обнаружен (риск для C++ Win32 paths).

### SEH
- Непрямо опирается на корректную `fs:[0]` адресацию + исключения/runtime; decode/exec база есть, но end-to-end корректность для SEH должна подтверждаться интеграционно.

---

## 4) Классификация по важности для KeePass

### Критично-для-старта (loader/ABI/runtime)
1. Надёжный integer/control-flow coverage + flags behavior (уже частично есть).
2. String-op family для CRT (`MOVS/CMPS/LODS/STOS/SCAS`) — сейчас дыры.
3. FS/GS segment-prefix correctness (TEB/SEH).
4. ABI completeness: минимум `cdecl/stdcall` уже есть; проверить `thiscall`-relevant paths.

### Критично-для-GUI (render/blit)
1. SSE/SSE2/MMX decode+lifter+exec для x86-пути (сейчас практически отсутствует).
2. В первую очередь move+pack+unpack+arith families (`MOVDQU/MOVDQA`, `PACK*`, `PUNPCK*`, `PADD*`, `PMUL*`, `PSHUF*`).

### Реже-нужно (но важно для совместимости)
1. x87 FPU.
2. Extended atomics/fences.

---

## Дорожная карта: что добавить в x86-декодер, чтобы KeePass запустился и рисовал

### Фаза A — «запуск процесса/логика» (P0)
1. Закрыть string-op baseline в x86 (`MOVS/CMPS/LODS/STOS/SCAS + REP semantics`).
2. Подтвердить/дополнить segment-prefix decode (`FS/GS`) для TEB/SEH-critical memory forms.
3. Довести lifter parity для уже декодируемых опкодов (чтобы меньше уходило в unsupported).

### Фаза B — «GUI и стабильная отрисовка» (P0-P1)
1. Добавить x86 SSE/SSE2 move+shuffle+pack/unpack+arith ядро.
2. Начать с часто встречаемых CRT/GUI путей (`MOVDQU/MOVDQA`, `PUNPCK*`, `PACK*`, `PADD*`, `PMUL*`, `PSHUF*`).

### Фаза C — «совместимость/долг» (P1-P2)
1. x87 FPU baseline.
2. Расширенные atomics/sync и прочие редкие ISA families.

---

## Практический итог для KeePass

Текущий x86 path уже имеет «скелет» для простых integer/control-flow блоков, но **для реального KeePass запуска с GUI этого недостаточно**. Главные блокеры — отсутствие x86 SIMD и неполный string/runtime слой.  
Рекомендуемый порядок: **startup/ABI/string/FS/GS first**, затем **SIMD for GUI render paths**.
