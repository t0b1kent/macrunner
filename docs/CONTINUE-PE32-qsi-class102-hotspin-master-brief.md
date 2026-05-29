# CONTINUATION BRIEF — PE32 Notepad++ x86: 100% CPU hot-spin на NtQuerySystemInformation class=102

**Date:** 2026-05-29
**Working copy:** `/Users/timurtoby/Documents/MacRunner/Main/MacRunner` (canonical).
**Модель:** Claude Opus (Thinking) — дефолт для engine-debug. Codex gpt-5.5 xhigh — ок.
НЕ Flash на движок.
**Граница:** PE32/WOW64/HyperBridge lane. НЕ трогать `engine/graphics/**` (Kimi),
golden x64 snapshot (read-only oracle). Kill ТОЛЬКО scoped по prefix.

## ГЛАВНОЕ ПРАВИЛО (урок прошлой сессии)
**Verdict = реальное окно Notepad++ x86 на экране (CG-capture). НЕ «процесс жив»,
НЕ «трасса 2.2M строк прошла».** Прошлый агент (Opus) принял длинный syscall-trace
за «launched» и ошибся — окна не было. НЕ останавливайся и НЕ рапортуй «готово»,
пока на экране нет окна.

## КОНТЕКСТ (читать ПЕРВЫМ)
1. `docs/ACTIVE-INVESTIGATION.md` — Update 2026-05-29 12:xx. Там полная картина.
2. `AGENTS.md` — обязательные правила (x64 oracle read-only, scoped wine kill
   `WINEPREFIX=$PREFIX wineserver -k` НЕ глобальный pkill, family-audit для опкодов,
   ENGINE-CHANGE-JOURNAL append на каждую правку).
3. Что уже решено (НЕ переделывать):
   - `c0000005` в `gdi32 get_gdi_client_ptr` ПОФИКШЕН — commit `6b7886b`.
     Root cause: 64-битный `win32u.so` не зеркалил `gdi_shared` в `peb32->
     GdiSharedHandleTable`. Фикс в `gdiobj.c:585-595` (`#else` ветка по
     `WowTebOffset`). Это правильно, подтверждено vs vanilla WineHQ.

## ТЕКУЩИЙ БЛОКЕР (это твоя задача)
После фикса `c0000005` процесс грузится дальше, но **зависает на 98-100% CPU**
(hot-spin, НЕ блок на сисколле). Лог замирает на:
```
macrunner-wow64: NtQuerySystemInformation enter class=102 ptr=... len=432 retlen=0
```
без `leave`. GUI не достигнут (`0` вызовов `NtUser*`/`NtGdi*`/`CreateWindow`).
Class 102 = `SystemModuleInformationEx` (семейство перечисления модулей лоадером).

### Репро (1 команда, чисто, без шумных флагов)
```
bash reports/phase-h/run_window_clean.sh > /tmp/npp.log 2>&1 &
# через ~20с: ps -o pcpu -p <notepad++ pid> покажет ~100%, лог замрёт на class=102
# убить scoped: WINEPREFIX=$PWD/bottles/generic-x86 \
#   engine/wine/dist-pure-arm64/bin/wineserver -k
```

## ЗАДАЧА (stepwise, evidence-first)
1. **Узкая трасса на точку спина** (НЕ `_ALL_SIMULATE` — шумно). Включи
   `MACRUNNER_XTAJIT_TRACE_SYSCALLS=1` + `MACRUNNER_XTAJIT_TRACE_STACK=1`
   (`engine/wine/dlls/xtajit/cpu.c`). Поймай **guest EIP + байты инструкции**
   в момент спина: что именно крутит гость после/вокруг QSI class=102.
2. **Классифицируй причину** (одна из):
   - (a) **WOW64-thunk для QSI class=102 отдаёт кривой 32-битный layout** →
     гость в бесконечном retry/итерации по битому списку модулей. Чини thunk
     (зеркаль рабочий x64-путь: как class=102 возвращается на x64, read-only).
     Смотри `engine/wine/dlls/wow64/` + где у нас обрабатывается
     NtQuerySystemInformation для 32-бит.
   - (b) **QSI class=102 не реализован/возвращает garbage success** → добавь
     корректную обработку class=102 для WOW64.
   - (c) **JIT/interp застрял на опкоде** в цикле перечисления → family-audit
     per AGENTS (decode→IR→lift→interp→tests), один опкод/семейство за раз.
3. **x64-oracle:** прежде чем чинить — глянь как class=102 отрабатывает на рабочем
   x64-пути (golden read-only) и зеркаль. Не изобретай.
4. **Один фикс за раз.** Family-audit чеклист в commit message.
   Верификация: `hb_test_runner` + `verify-build-freshness` + повтор
   `run_window_clean.sh`.
5. **VERIFY ОКНОМ:** после фикса — `screencapture` реального окна Notepad++ x86,
   сохрани в `reports/phase-h/npp-x86-window-<date>.png`. Если окна нет — это
   следующий блокер, продолжай (НЕ останавливайся).

## ДИСЦИПЛИНА
- **Evidence-first.** Сначала факт из трассы (guest EIP/байты/layout), потом фикс.
  Никаких гипотез-без-трассы (прошлые сессии так уходили в тупик).
- **x64 golden = read-only working oracle.** Зеркаль, не правь golden.
- **Scoped kill только:** `WINEPREFIX=$PWD/bottles/generic-x86 wineserver -k`.
  Kimi гоняет Wine параллельно — глобальный `pkill -9 wine` его убьёт. Запрещено.
- НЕ трогать `engine/graphics/**` (Kimi).
- Обновляй `ACTIVE-INVESTIGATION.md` на каждый шаг (переживает compaction).
- `ENGINE-CHANGE-JOURNAL.md` (append) на каждую правку движка.

## OUTPUT
- Коммиты в canonical working copy с family-audit чеклистами.
- `ACTIVE-INVESTIGATION.md` + `ENGINE-CHANGE-JOURNAL.md` обновлены.
- **DONE = окно Notepad++ x86 на экране (CG-capture). Не process-alive, не trace.**
  НЕ останавливайся пока 32-бит реально не запустился с окном.

## ESCALATION
- ARM64EC ntdll exports (`__wine_unix_call_dispatcher_arm64ec`) — это ПАРАЛЛЕЛЬНЫЙ
  лейн (Kimi/графика, x64 PE). НЕ твоя задача, не отвлекайся.
- Нужна правка golden x64 — НЕ правь, эскалируй оператору.
