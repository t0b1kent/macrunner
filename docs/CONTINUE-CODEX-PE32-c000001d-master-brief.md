# CONTINUATION BRIEF — продолжить работу Codex CLI: PE32 c000001d (Antigravity 2.0)

**Date:** 2026-05-28
**Назначение:** заменить временно простаивающий Codex CLI (закончились токены).
**Где запускать:** Antigravity 2.0, модель — см. раздел «Выбор модели».
**Working copy:** `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` (canonical).
**Граница:** PE32/WOW64/HyperBridge lane Codex'а. НЕ трогать графику (`engine/graphics/**`,
DXMT) — у Kimi свой worktree на внешнем диске. Не глушить wine глобально (см. AGENTS).

## Выбор модели (честно)
- **Claude Opus 4.6 (Thinking)** — дефолт для глубокого engine-debugging (мой выбор).
- **Claude Sonnet 4.6 (Thinking)** / **Gemini 3.1 Pro (High)** — норм альтернативы.
- **Gemini 3.5 Flash (High)** — только на bounded суб-таск (один трейс/одна классификация),
  не на весь движок. Если используешь Flash — НЕ редактируй движок без подтверждения трассой.
- **GPT-OSS 120B** — экспериментально, не основной.

## КОНТЕКСТ (читать ПЕРВЫМ)
1. `docs/ACTIVE-INVESTIGATION.md` — живое состояние. Последний апдейт 2026-05-28 18:59,
   секции «Current Task» + Updates 13:38, 13:53, 18:07. Текущий блокер ИМЕННО там.
2. `AGENTS.md` — обязательные правила (engine memory journal, x64 oracle для PE32 = read-
   only, prefix-scoped Wine kill — не глобальный pkill, family audit для опкодов).
3. Краткая суть на 2026-05-28:
   - PE32 Notepad++ через HyperBridge x86 + xtajit. Большие фиксы прошли (x87 family,
     SSE convert, MOV operand16, RET family, TEB32 mirror, WOW64 syscall boundary).
   - PC=0/null-PC уже классифицирован как branch на x64 guest target → пофикшено через
     зеркало рабочего x64 route (signal_arm64.c:759 + macrunner_hb.c).
   - **ТЕКУЩИЙ блокер:** post-vprot/post-route → процесс жив, нет CG-window. lldb ловит
     thread #2 в `_sigtramp` с `x13=0xc000001d` (`STATUS_ILLEGAL_INSTRUCTION`),
     неизвестный native target ~`0x438b6880b19f`. Codex дошёл до identification узких
     xtajit-knob'ов (`MACRUNNER_XTAJIT_TRACE_ALL_SIMULATE/_SYSCALLS/_STACK` в
     `engine/wine/dlls/xtajit/cpu.c`) и low-overhead callback/signal trace (18:07 update).
4. Параллельный блокер (НЕ твой): Kimi эскалировал ARM64EC ntdll (для x64 PE через
   HyperBridge, графика). Это **отдельная** инженерная работа, не твой текущий трейс.

## ЗАДАЧА (продолжение Codex'а)
Доделать классификацию `c000001d`/неизвестного native target в PE32:
1. **Узкая трасса (не широкая):** включи `MACRUNNER_XTAJIT_TRACE_SYSCALLS` (или
   `_STACK`, не `_ALL_SIMULATE` — это слишком шумно, Codex уже отметил). Получи
   `publish-exception-context`: guest EIP, status, байты инструкции на момент c000001d.
2. **Классифицируй:** это (a) return/BOP-target corruption → зеркаль рабочий x64 route
   (x64-oracle, см. AGENTS), или (b) реально неподдерживаемая PE32 инструкция → family
   audit per AGENTS (decode→IR→lift→interp→tests).
3. **Один фикс за раз**, family-audit чеклист в коммит-сообщении, верификация
   `hb_test_runner` + `verify-build-freshness` + bounded scoped run.
4. **Verify**: реальное окно Notepad++ x86 на экране (capture, не «процесс жив»).
5. Обнови `ACTIVE-INVESTIGATION.md` на каждый шаг (Codex это требует, чтобы пережить
   compaction).

## ДИСЦИПЛИНА (важно — урок прошлых сессий)
- **x64 golden = working oracle (read-only).** Для любого x86-бага сперва глянь, как
  это решено в x64-пути, и зеркаль. См. AGENTS секция «x64 golden = WORKING ORACLE».
- **Kill только scoped по prefix.** Никогда `pkill -9 wine` — глобально. Используй
  `WINEPREFIX=$PREFIX wineserver -k`. Kimi гоняет Wine параллельно — не убей его.
- **Evidence-first, не догадки.** В прошлые сессии гипотезы без трассы (env_size,
  label-whitelist) уводили в тупик. Сначала факт из стека/лога, потом фикс.
- **Не глуши `pkill -f notepad`/`pkill -f dx11`** глобально — задеть Kimi.
- Не лезть в `engine/graphics/**` (Kimi) и в golden x64 snapshot (read-only).

## ПОСЛЕДНИЕ АРТЕФАКТЫ Codex'а (как точка входа)
- `reports/phase-h/npp-x86-after-vprot-fix-20260528-133052` — post-vprot run, c000001d
  в `_sigtramp`.
- `reports/phase-h/npp-x86-xtajit-trace-all-20260528-133941` — слишком шумный all-trace
  (отрицательный результат, не повторяй).
- ACTIVE-INVESTIGATION updates 13:38, 13:53, 18:07.

## OUTPUT
- Коммиты в `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` с family-audit чеклистами.
- Обновления `docs/ACTIVE-INVESTIGATION.md` (живой статус, как Codex это вёл).
- `docs/ENGINE-CHANGE-JOURNAL.md` (append) на каждую правку движка.
- **Verified = реальное окно Notepad++ x86 на экране (CG-capture), не proxy/process-alive.**

## ESCALATION
- Если упрёшься в **ARM64EC ntdll missing exports** (см. Kimi-эскалация про
  `__wine_unix_call_dispatcher_arm64ec`) — это **параллельный лейн**, Gemini Flash на
  research; ты на это не отвлекайся, продолжай PE32 c000001d.
- Если нашёл что-то, что требует **golden x64 правки** — НЕ правь golden, эскалируй мне.
