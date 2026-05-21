# HyperBridge memcpy/copy-op audit (outside proven SSE2 MOVDQU/MOVDQA)

Дата: 2026-05-22  
Контекст: `reports/agent-prompts/CLINE-HYPERBRIDGE-MEMCOPY-AUDIT.txt`

## Краткий вывод

Наиболее вероятные корни «обнуления при копировании иконки» в текущем HyperBridge:

1. **REP MOVS (MOVSB/MOVSW/MOVSD/MOVSQ) отсутствует полностью** (decode/lift/IR/exec). Это **топ-1 кандидат** для путей memcpy/memmove.
2. **AVX/VEX (YMM/VMOV") отсутствует полностью**; VEX-префиксы не декодируются, что ломает современные AVX memcpy-paths.
3. **Non-temporal stores/moves (MOVNT*) отсутствуют**.

При этом:
- `STOS` реализован (decode/lift/interpreter) и выглядит семантически корректно по DF/RCX/RDI.
- 128-бит legacy SSE moves (`MOVUPS/MOVAPS/MOVDQU/MOVDQA`, плюс scalar `MOVSS/MOVSD`) заведены через `HB_INS_SSE_MOV` и в интерпретаторе копируют нужный размер (включая partial-lane для scalar).

---

## Таблица аудита

| Инструкция/семейство | Декод? | Трансляция (lift/IR/exec) | Эталон / сверка | Вердикт | Риск | Комментарий |
|---|---|---|---|---|---|---|
| **REP MOVS** (`A4/A5` + `F3/F2`) | **Нет** (`hb_decode_x64.c`: нет веток `opcode==0xA4/0xA5`, нет `HB_INS_MOVS` в enum) | Нет (нет `HB_INS_MOVS`, нет `HB_IR_*` для MOVS) | Intel SDM string move (RCX/RSI/RDI/DF) | **ОТСУТСТВУЕТ** | **КРИТИЧЕСКИЙ** | Классический memcpy путь. Отсутствие = прямой кандидат на порчу/обнуление буферов при копировании. |
| **REP STOS** (`AA/AB` + `F3/F2`) | Да (`hb_decode_x64.c:810+`) | Да: `HB_INS_STOS -> HB_IR_STOS` (`hb_lift_x64.c:738+`) -> loop в `hb_interpreter.c:2020+` | Intel SDM STOS/REP/DF | **OK** | Средний | Учитывает `RCX`, `RDI`, `DF` (`rflags bit10`), шаг = size, обновляет RCX/RDI; лимит итераций есть. |
| **AVX/AVX2 256-bit moves** (`VEX` `VMOVDQU/VMOVDQA/VMOVUPS/VMOVAPS`) | **Нет** (нет VEX decode; CPUID/XGETBV AVX намеренно скрыт) | Нет | Intel SDM VEX/AVX | **ОТСУТСТВУЕТ** | **КРИТИЧЕСКИЙ** | `HB_IR_CPUID` выставляет SSE2-baseline и явно не рекламирует AVX (`hb_interpreter.c:1876+`, `1906`). Но если код всё же дойдёт до VEX-байтов — декодер их не понимает. |
| **128-bit float/int moves** (`MOVUPS/MOVAPS/MOVUPD/MOVAPD`, `MOVDQU/MOVDQA`) | Да частично: через `HB_INS_SSE_MOV` для `0F 10/11/28/29/6F/7F` (`hb_decode_x64.c:1570+`) | Да: lift в `LOAD/STORE/MOV` (`hb_lift_x64.c:104+`), exec в `HB_IR_LOAD/STORE/MOV` с XMM размером | Intel SDM legacy SSE moves | **OK** | Низкий | Для scalar `MOVSS/MOVSD` (`F3/F2 0F 10/11`) размер 4/8, верх XMM сохраняется (partial copy), что корректно. |
| **Non-temporal** (`MOVNTDQ/MOVNTPS/MOVNTI`) | **Нет** (нет опкодов/enum веток) | Нет | Intel SDM streaming stores | **ОТСУТСТВУЕТ** | Высокий | Современные memcpy/memset/graphics пути могут использовать streaming stores. |
| **LDDQU** (`F2 0F F0`) | **Нет** (нет decode-ветки) | Нет | Intel SDM SSE3 | **ОТСУТСТВУЕТ** | Средний | Может встречаться в unaligned copy на некоторых CRT/SIMD helper путях. |
| **MOVSD/MOVSS scalar** (`F2/F3 0F 10/11`) | Да (внутри `HB_INS_SSE_MOV`, scalar move_size 8/4) | Да (через XMM partial load/store/mov) | Intel SDM scalar lane semantics | **OK** | Низкий | Реализация учитывает low-lane copy без затирания остальных битов XMM. |

---

## Доказательства по коду (ключевые точки)

- `HB_INS_SCAS` decode: `hb_decode_x64.c:800` (`0xAE/0xAF`) — как reference string-op c REP mode в `imm`.
- `HB_INS_STOS` decode: `hb_decode_x64.c:810` (`0xAA/0xAB`) — REP mode переносится в `op2 imm`.
- `HB_INS_SSE_MOV` decode: `hb_decode_x64.c:1570-1591` для `0F 10/11/28/29/6F/7F` + scalar case `F2/F3`.
- Lift:
  - `HB_INS_SSE_MOV -> LOAD/STORE/MOV`: `hb_lift_x64.c:104-114`.
  - `HB_INS_STOS -> HB_IR_STOS`: `hb_lift_x64.c:738-745`.
- Exec:
  - XMM move semantics: `hb_interpreter.c:1229+` (`HB_IR_MOV`), `1611+` (`HB_IR_LOAD`), `1634+` (`HB_IR_STORE`).
  - `HB_IR_STOS`: `hb_interpreter.c:2020+`.
  - `HB_IR_SCAS`: `hb_interpreter.c:1985+`.
  - AVX intentionally not advertised: `hb_interpreter.c:1876+`, `1906`.
- Enum surface: `hb_decoder.h` содержит `HB_INS_SCAS`, `HB_INS_STOS`, `HB_INS_SSE_MOV`, но **нет** `HB_INS_MOVS`/AVX/MOVNT/LDDQU.

---

## Top candidates для Codex (приоритет фикса)

1. **REP MOVS family (A4/A5 + REP)** — добавить decode/lift/IR/exec с корректной семантикой `RCX/RSI/RDI/DF`, включая `MOVSB/W/D/Q`.
2. **VEX prefix + базовые VMOV* (YMM/XMM)** — хотя бы безопасный/детерминированный путь (или controlled trap), чтобы не получать silent corruption.
3. **MOVNT* + LDDQU** — закрыть частые memcpy/blit специализированные ветки.

---

## Итог

С учётом уже доказанной корректности SSE2 MOVDQU/MOVDQA, наиболее вероятный следующий корень проблемы на ImageList boundary — **пропущенные copy-инструкции вне текущего покрытия**, в первую очередь **REP MOVS** (критично) и затем **AVX/VEX path** (критично, если активируется).
