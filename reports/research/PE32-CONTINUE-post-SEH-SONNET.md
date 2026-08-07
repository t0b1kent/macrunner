# PE32 — CONTINUE (post-SEH) — for a Sonnet terminal

Ты — **Lane PE32** (весь 32-битный фронт). Многомесячная автономная миссия, НЕ один проход. Repo root:
/Users/timurtoby/Documents/MacRunner/Main/MacRunner.

## ПРОЧИТАЙ (resume)
- reports/research/PE32-PROGRESS.md — твой чекпоинт (продолжай с конца)
- docs/CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md — твоя мега-программа (Phases 0–6)
- reports/research/LANE-C-API-GAPS.md — доказательство, что i386 уже доходит до BTCpu
- reports/research/MILESTONE-20260607-SEH-passed-graphics-critical-path.md — что изменилось

## 🔥 ГЛАВНОЕ ИЗМЕНЕНИЕ: твой блокер снят Lane A
Раньше i386-приложения доходили до BTCpu и умирали на `c0000026` (SEH host-boundary). **Lane A этот
SEH пробил** (bulk CFI на host-thunks). Значит твой 32-битный путь теперь должен идти ДАЛЬШE. Твоя
работа — гнать его вперёд, НЕ трогая фикс Lane A (ты на нём едешь).

## ПЕРВЫЙ ШАГ: перепрогон i386 на приложениях, что РЕАЛЬНО доходят до BTCpu
НЕ notepad++ (у него был форс-x64 в раннере). Бери те, что уже достигали BTCpu (из LANE-C-API-GAPS):
- Diablo i386: `/Users/timurtoby/Documents/MacRunner/Main/Diablo.Hellfire-Rutracker/extracted/Diablo.exe`
- Terraria i386: `/Users/timurtoby/Documents/MacRunner/Main/Terraria_v1.4.5.6_(89298)_Win_[GOG]/extracted/Terraria.exe`
Запускай с авто-триажем (новая вшивка): 
`MACRUNNER_RUN_DIR="$RUNDIR" scripts/mr-run.sh <dist> <exe> <tmo> > "$RUNDIR/run.log" 2>&1`
→ сам получишь $RUNDIR/triage-summary.txt. Смотри: проходит ли теперь `c0000026`, докуда дошёл EIP/BTCpu.

## ЦЕПЬ (mega-program): Phase 1 → 2 → 3
1. **Phase 1** — i386 CPU исполняет под BTCpuSimulate за SEH-границей (раньше тут умирал). Чини
   guest32↔host граничные проблемы (TEB32, fs:[..], NtContinue/NtRaiseException marshalling) если всплывут.
2. **Phase 2** — реальное окно 32-битного приложения (Notepad++ x86 / Diablo). CG-capture = вердикт.
3. **Phase 3** — i386 ISA bulk. **КООРДИНАЦИЯ:** Gemini параллельно строит ОФФЛАЙН ISA-coverage-инструмент
   (capstone-diff, матрица дыр) в `tools/` — НЕ дублируй его анализ; ты ИМПЛЕМЕНТИШЬ фиксы декода/lift/
   interp в xtajit по его матрице, когда она появится.

## ВЛАДЕНИЕ (жёстко — другие лайны живые)
- Твоё: `engine/wine/dlls/xtajit/**` (32-бит, НЕ xtajit64), `wow64`/`wow64cpu`/`wow64win`, deploy-скрипты.
- НИКОГДА: `macrunner_hb.c` / `signal_arm64.c` (Lane A АКТИВНО их правит — коллизия), `xtajit64` (Lane A),
  `engine/dxmt|graphics` (Lane D = Sonnet, активен), `server/mapping.c` (loader-фикс запаркован — не нужен
  для Diablo/Terraria, они и так доходят до BTCpu).
- Если упрёшься в чужой файл → запиши в `reports/research/PE32-NEEDS.md` и продолжай свои части.

## ДИСЦИПЛИНА
- ctx через `ctx_execute` language=javascript (НЕ ctx_batch_execute — падает /bin/zsh ENOENT). Сырьём
  большие логи не читай. Heartbeat КАЖДЫЙ шаг в reports/research/PE32-PROGRESS.md.
- Scoped kill: `WINEPREFIX=$PWD/bottles/generic-x86 wineserver -k`, НИКОГДА global pkill.
- Коммить только свои файлы (не -A).

В КОНЦЕ турна строкой с КОЛОНКИ 0: `LOOP-STATUS: GOAL` (32-бит окно на экране) /
`LOOP-STATUS: BLOCKED <причина>` / `LOOP-STATUS: CONTINUE`.
