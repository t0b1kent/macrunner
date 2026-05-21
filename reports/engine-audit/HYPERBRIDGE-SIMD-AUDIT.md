# HYPERBRIDGE SIMD AUDIT (family-audit, READ-ONLY)

Дата: 2026-05-21

Область аудита: `engine/hyperbridge` (decode → lift → interpreter), без правок `.c`.

## Краткий вывод

- Ветвь x64 содержит рабочую цепочку для части SSE/MMX-инструкций: `PMOVMSKB`, `PUNPCK*`, `PACKSSWB/PACKUSWB/PACKSSDW`, `PSHUF*`, `PSRL*/PSRA*/PSLL*`, `PADD*`, `PSUB*`, `MOVD`.
- Критичный набор инструкций, запрошенный как подозреваемый для icon/blit-проблем, **вообще отсутствует в декодировании/IR**: `PMULLW`, `PMADDWD`, `PADDUSB`, `PAVGB`, `MOVDQA`, `MOVDQU`, `PSHUFB`.
- `hb_decode_x86.c` практически без SIMD-покрытия (в `0x0F` реализованы в основном `Jcc/SETcc/CMOVcc`, остальное `HB_ERR_UNSUPPORTED_OPCODE`).
- `hb_jit.c` — только JIT-buffer инфраструктура, без SIMD lowering/codegen.

## Таблица аудита

| Инструкция | декодируется? | наша трансляция | sse2neon/SDM эталон | вердикт | риск | комментарий |
|---|---|---|---|---|---|---|
| PMOVMSKB | Да (x64) | `HB_INS_PMOVMSKB -> HB_IR_PMOVMSKB -> interpreter(mask from byte MSB)` | SDM: собрать старшие биты 16 байт в r32 | OK | Средний | Реализация в `hb_interpreter.c` соответствует базовой семантике. |
| PUNPCKLBW/PUNPCKHBW (+LWD/LDQ/LQDQ/HWD/HDQ/HQDQ) | Да (x64) | `HB_INS_PUNPCK* -> HB_IR_PUNPCK(target lane/high)` | SDM: interleave low/high lanes | OK | Средний | Логика interleave есть; ориентирована на 128-bit XMM. |
| PACKSSWB | Да (x64) | `HB_INS_PACKSSWB -> HB_IR_PACKSSWB -> clamp int16->int8 signed` | SDM: signed saturation to [-128..127] | OK | Средний | Saturation реализован явно. |
| PACKUSWB | Да (x64) | `HB_INS_PACKUSWB -> HB_IR_PACKUSWB -> clamp int16->[0..255]` | SDM: unsigned saturation | OK | Низкий/Средний | Для icon-path это важная операция; текущая логика корректная по диапазону. |
| PACKSSDW | Да (x64) | `HB_INS_PACKSSDW -> HB_IR_PACKSSDW -> clamp int32->int16 signed` | SDM: signed saturation to [-32768..32767] | OK | Средний | Saturation присутствует. |
| PSHUFD | Да (x64) | `HB_INS_PSHUFD -> HB_IR_PSHUF(target=4)` | SDM: dword shuffle by imm8 | OK | Средний | Перестановка 4x32 lane реализована. |
| PSHUFLW | Да (x64) | `HB_INS_PSHUFLW -> HB_IR_PSHUF(target=2)` | SDM: shuffle low 4 words only | OK | Средний | Низ половины шифтуется, остальное сохраняется. |
| PSHUFHW | Да (x64) | `HB_INS_PSHUFHW -> HB_IR_PSHUF(target=0x102)` | SDM: shuffle high 4 words only | OK | Средний | Верхняя половина шифтуется. |
| PSRLW/PSRLD | Да (x64) | `HB_INS_PSRL* -> HB_IR_PSRL(target lane)` | SDM: logical right shift with zero on over-shift | OK | Средний | `count>=lane_bits => 0` соблюдается. |
| PSRAW/PSRAD | Да (x64) | `HB_INS_PSRA* -> HB_IR_PSRA(target lane)` | SDM: arithmetic right shift | OK | Средний | sign-extend при больших count реализован. |
| PSLLW/PSLLD | Да (x64) | `HB_INS_PSLL* -> HB_IR_PSLL(target lane)` | SDM: logical left shift with zero on over-shift | OK | Средний | Семантика over-shift корректная. |
| PSRLQ/PSLLQ | Да (x64) | `HB_INS_PSRLQ/PSLLQ -> HB_IR_*Q` | SDM: 64-bit lane shifts | OK | Средний | `count>63 => 0` реализовано. |
| PSRLDQ/PSLLDQ | Да (x64) | `HB_INS_PSRLDQ/PSLLDQ -> HB_IR_*DQ` | SDM: whole-register byte shifts | OK | Средний | byte-shift через `memmove`, `count>=16 => zero`. |
| PADDB/PADDW/PADDD/PADDQ | Да (x64) | `HB_INS_PADD* -> HB_IR_PADD(target lane)` | SDM: wraparound add per lane | OK | Средний | Wraparound сохраняется по lane-ширине. |
| PSUBB/PSUBW/PSUBD/PSUBQ | Да (x64) | `HB_INS_PSUB* -> HB_IR_PSUB(target lane)` | SDM: wraparound sub per lane | OK | Средний | Реализовано без saturation (как и должно быть для PSUB*). |
| MOVD | Да (x64) | `HB_INS_MOVD -> HB_IR_MOVD` | SDM: scalar move between GPR/XMM/mem | Частично OK | Средний | Основные случаи есть; нюансы legacy/MMX-path не покрыты этим аудитом. |
| PMULLW | Нет | Нет decode/IR/lift/interpreter | SDM/SSE2: mullo signed 16-bit lanes | **ОТСУТСТВУЕТ** | **Высокий** | Часто участвует в пиксельных/коэфф. операциях; отсутствие может ломать SIMD-пути. |
| PMADDWD | Нет | Нет decode/IR/lift/interpreter | SDM/SSE2: pair mul-add 16-bit -> 32-bit | **ОТСУТСТВУЕТ** | **Высокий** | Критично для фильтров/скейлинга/конверсий. |
| PADDUSB | Нет | Нет decode/IR/lift/interpreter | SDM/MMX/SSE2: unsigned saturating add bytes | **ОТСУТСТВУЕТ** | **Высокий** | Прямая связь с обработкой цветовых каналов (clamp/sat). |
| PAVGB | Нет | Нет decode/IR/lift/interpreter | SDM/MMX/SSE2: average unsigned bytes | **ОТСУТСТВУЕТ** | **Высокий** | Часто в blit/blend. |
| MOVDQA | Нет | Нет decode/IR/lift/interpreter | SDM/SSE2: aligned 128-bit move | **ОТСУТСТВУЕТ** | Средний/Высокий | Отсутствие ломает распространённые XMM code paths. |
| MOVDQU | Нет | Нет decode/IR/lift/interpreter | SDM/SSE2: unaligned 128-bit move | **ОТСУТСТВУЕТ** | **Высокий** | Особенно критично для unaligned UI buffers/icons. |
| PSHUFB | Нет | Нет decode/IR/lift/interpreter | SSSE3 byte-wise shuffle with mask | **ОТСУТСТВУЕТ** | Высокий | Не SSE2, но часто используется современными optimized-путями. |

## Наблюдения по backend-цепочке

1. **x64 decode/lift/interpreter связка присутствует** для ограниченного SSE2-поднабора.
2. **x86 decode (`hb_decode_x86.c`) SIMD почти не поддерживает**: в блоке `0x0F` реализованы `Jcc/SETcc/CMOVcc`, прочее уходит в `HB_ERR_UNSUPPORTED_OPCODE`.
3. **`hb_jit.c` не содержит SIMD lowering** — это инфраструктура памяти JIT-буфера, не кодоген инструкций.

## Кандидаты №1 для icon/blit black-pixel bug (для Codex)

1. **PADDUSB — отсутствует (High risk).**
   - Если гостевой путь ожидает saturating add, замена/деоптимизация может дать clipping/черные блоки.
2. **MOVDQU — отсутствует (High risk).**
   - Для unaligned буферов иконок/toolbar может приводить к fallback/faulty path.
3. **PMULLW/PMADDWD — отсутствуют (High risk).**
   - Часто применяются в цветовых/коэффициентных стадиях рендера.
4. **PAVGB — отсутствует (High risk).**
   - Усреднение каналов при blend/scaling; отсутствие ломает quality path.

## Статус ограничений

- READ-ONLY по движку соблюдён: `.c/.h` не изменялись.
- Сформирован только отчёт: `reports/engine-audit/HYPERBRIDGE-SIMD-AUDIT.md`.
