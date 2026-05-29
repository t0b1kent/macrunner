# MEGA MASTER BRIEF — x64 OPCODE COVERAGE: дожать HyperBridge x86_64 до запуска DXMT-клиентов

**Date:** 2026-05-29
**Agent:** Claude Opus (Thinking) — дефолт. Codex gpt-5.5 xhigh — ок. НЕ Flash.
**Working copy:** `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` (canonical).
**Lane:** HyperBridge x86_64 (x64) CPU-движок. ЭТО ГЛАВНЫЙ ПУТЬ К ПРОДУКТУ (игры x64).
**Граница:** НЕ трогать `engine/graphics/**` (Kimi), golden x64 snapshot (read-only oracle).
Kill ТОЛЬКО scoped по prefix. Не глобальный `pkill -9 wine` (убьёшь параллельный Wine Kimi).

---

## ПОЧЕМУ ЭТО ПРИОРИТЕТ #1 (стратегия — прочти, чтобы не уйти в сторону)
Продукт = запускать Windows-**игры** на Apple Silicon. Игры = **x64**. Видеоядро
(DXMT→Metal) нужно x64-играм. Значит критический путь = **довести x64 CPU-движок до
того, чтобы x86_64 DXMT-клиенты запускались без падений**, тогда Kimi даст пиксель, и
это первый игровой кадр.

**32-бит ПРИПАРКОВАН** (бриф `CONTINUE-PE32-qsi-class102-hotspin` — на потом, для полноты).
НЕ переключайся на 32-бит. WOW64/PEB32/xtajit-работа на x64 **не нужна** (x64 PE идёт
напрямую, без WOW64-thunk'ов). Твой фокус — чистый x64 decode→lift→interp.

**ARM64EC ntdll — НЕ твоя задача сейчас.** Это отдельный архитектурный блокер (x86_64
ntdll не экспортит `__wine_unix_call_dispatcher_arm64ec`), по нему идёт внешний research
(ChatGPT/Gemini). НЕ ныряй в пересборку Wine с ARM64EC вслепую — утопишь недели. Если
упрёшься в ntdll `EXEC_FAULT` (ARM64-инструкции в x86_64 ntdll `.text`) — это ОТДЕЛЬНЫЙ
блокер, эскалируй, не чини здесь. Твоя цель — **UNSUPPORTED_OPCODE coverage**, не ntdll.

---

## ЗАДАЧА: систематическая кампания покрытия x64-опкодов
Цель не «один опкод», а **прогнать DXMT тест-клиенты и закрыть КАЖДЫЙ `UNSUPPORTED_OPCODE`,
пока клиент не отрабатывает чисто** (до present/выхода без HB-краша). Это итеративный цикл.

### Известная точка входа (от Kimi, evidence)
- `dx11_clear_present_x64.exe`: CRT init падает на `pc=0x140001cc0` →
  `HB_ERR_UNSUPPORTED_OPCODE` (`blocks=1c steps=74`). Патч `call r12/r13/r15`→NOP не помог
  (значит дело не в indirect call — ищи реальный неподдержанный опкод, скорее SSE/AVX или
  редкий x64-энкод).
- Корпус тест-клиентов лежит в worktree Kimi:
  `/Volumes/MacOS 1/MacRunner-dxmt-truth-gate-20260527/engine/graphics/dist/tests/`
  (`*_x64*.exe`, `clear-*`, `hello-triangle-*`). Скопируй нужный exe к себе в
  `artifacts/phase-h/` для прогона (НЕ правь дерево Kimi).

### Где живёт движок (file:line, твоя зона правок)
- `engine/hyperbridge/src/hb_decode_x64.c` — декодер x64 (тут эмитится UNSUPPORTED_OPCODE).
- `engine/hyperbridge/src/hb_lift_x64.c` — лифтинг x64 → IR.
- `engine/hyperbridge/src/hb_interpreter.c` — интерпретация IR.
- `engine/hyperbridge/include/hb_result.h:15` — `HB_ERR_UNSUPPORTED_OPCODE = -5`.
- `engine/hyperbridge/tests/hb_test_runner.c` — тесты (см. примеры `hb_decode_x64(...)
  == HB_ERR_UNSUPPORTED_OPCODE` на строках ~3440/3549 — паттерн для новых тестов).

## ЦИКЛ (повторять до чистого прогона клиента)
1. **Прогон + поймать опкод.** Запусти клиент под движком, поймай
   `pc=<addr>` + `UNSUPPORTED_OPCODE`. Включи минимальный fault-trace
   (`MACRUNNER_HB_TRACE_FAULTS=1` или эквивалент), НЕ широкий all-trace (шумно).
2. **Достань БАЙТЫ инструкции** на `pc` из exe (objdump/llvm-objdump по offset, или
   hexdump секции `.text` по RVA). Раскодируй вручную (или прогони
   `hb_decode_x64(bytes, ...)` в unit-харнессе) — пойми ТОЧНО какая инструкция/энкод
   не поддержан (mnemonic, prefix, ModRM, VEX/REX).
3. **x64-ORACLE (обязательно по AGENTS):** глянь, **есть ли этот опкод уже в x86-пути**
   (`hb_decode_x86.c`/`hb_lift_x86.c`). Часто да — тогда **зеркаль рабочую реализацию**,
   не изобретай. Golden x64 snapshot — read-only, только смотреть.
4. **FAMILY-AUDIT (по AGENTS, обязательно):** не добавляй один энкод — закрой СЕМЕЙСТВО.
   Чеклист на КАЖДЫЙ опкод:
   - [ ] decode: все варианты ModRM/REX/VEX/operand-size этого опкода
   - [ ] IR: добавлен/смаплен корректно
   - [ ] lift: x64 → IR покрывает все операнды/режимы адресации
   - [ ] interp: семантика верна (флаги EFLAGS, ширина, знаковость, SSE/AVX-lane'ы)
   - [ ] tests: добавлены в `hb_test_runner.c` (decode + поведение, включая edge-cases)
   - [ ] Audit completed: yes
5. **ОДИН опкод/семейство за коммит.** Family-audit чеклист в commit message
   (формат как в `6b7886b`: Family/Members/Trigger/Coverage/Audit).
6. **Верификация:** `hb_test_runner` (все зелёные) + `verify-build-freshness` +
   повторный прогон клиента (должен пройти ДАЛЬШЕ прежнего pc). Зафиксируй новый pc.
7. **Назад к шагу 1** на следующий UNSUPPORTED_OPCODE, пока клиент не отработает чисто.

## DEFINITION OF DONE
- `dx11_clear_present_x64.exe` (минимум) и в идеале `hello-triangle` проходят CRT-init и
  D3D11-инициализацию **без `HB_ERR_UNSUPPORTED_OPCODE`** до точки present.
- Все новые опкоды покрыты тестами в `hb_test_runner.c` (зелёные).
- DONE — это «клиент дошёл до present-вызовов без HB-краша», НЕ «процесс жив», НЕ «трасса
  прошла». (Урок PE32-лейна: длина лога ≠ успех.)
- Если клиент упёрся в ntdll `EXEC_FAULT` (ARM64EC) — это ДРУГОЙ блокер: задокументируй
  pc + что осталось, эскалируй. Опкод-кампанию по своей части считаем закрытой.

## ДИСЦИПЛИНА (жёстко)
- **Evidence-first.** Сначала БАЙТЫ инструкции на faulting pc, потом фикс. Никаких
  «наверное это SSE» без дизасма. (Прошлые сессии тонули в гипотезах-без-трассы.)
- **x64 golden = working oracle (read-only).** Зеркаль x86-путь/golden, не изобретай.
- **Family-audit на каждый опкод** — иначе следующий энкод того же семейства = новый краш.
- **Один фикс за коммит**, чеклист в сообщении, тесты обязательны.
- **Scoped kill только:** `WINEPREFIX=$PREFIX wineserver -k`. Глобальный `pkill -9 wine`
  ЗАПРЕЩЁН — Kimi гоняет Wine параллельно.
- НЕ трогать `engine/graphics/**` (Kimi), дерево Kimi worktree, golden snapshot.
- Обновляй `docs/ACTIVE-INVESTIGATION.md` на каждый опкод (переживает compaction).
- `docs/ENGINE-CHANGE-JOURNAL.md` (append) на каждую правку движка.

## OUTPUT
- Коммиты в canonical с family-audit чеклистами (по одному опкоду/семейству).
- `hb_test_runner` зелёный, новые тесты на каждый опкод.
- `ACTIVE-INVESTIGATION.md` + `ENGINE-CHANGE-JOURNAL.md` обновлены.
- Финал: список закрытых опкодов + до какой точки дошёл клиент (pc / present / ntdll-блок).

## ESCALATION
- ntdll `EXEC_FAULT` / ARM64EC missing exports → ОТДЕЛЬНЫЙ лейн (внешний research идёт),
  НЕ чини, эскалируй оператору.
- Нужна правка golden x64 snapshot → НЕ правь, эскалируй.
- Если опкод требует чего-то в `engine/graphics` → это Kimi, эскалируй, не лезь.

---
## ПАРАЛЛЕЛЬНЫЕ ЛЕЙНЫ (контекст, не твоя работа)
- **Kimi:** доказывает пиксель native-ARM64 клиентом + чинит off-screen окно (его бриф
  `KIMI-TASK-native-pixel-decouple-...` в его worktree). Когда ты закроешь x64-опкоды,
  его x86_64-клиенты поедут → пиксель.
- **32-бит PE32:** припаркован (`CONTINUE-PE32-qsi-class102-hotspin`). На потом.
- **ARM64EC ntdll:** внешний research (ChatGPT/Gemini) → потом отдельное решение.
