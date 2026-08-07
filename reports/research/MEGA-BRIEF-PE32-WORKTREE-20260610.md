# MEGA-BRIEF — PE32/i386 (изолированный worktree). 2026-06-10

## 🏠 ТВОЙ НОВЫЙ ДОМ — ОТДЕЛЬНОЕ ДЕРЕВО (причина: вы с Lane A ломали друг друга)
Работаешь ТОЛЬКО в: **`/Users/timurtoby/Documents/MacRunner/Main/MacRunner-pe32`**
- Это git worktree, ветка **`pe32-lane`** (от main@66c9fa5). У тебя СВОИ копии
  `engine/wine/build-arm64ec-spike` и `engine/wine/dist-arm64ec-spike` — собирай и деплой
  В НИХ. Lane A больше НЕ перезапишет твой dist (его «c0000026 dist regression» у тебя
  больше не появится сам по себе), и ты не перезапишешь его.
- **КАНОНИЧЕСКОЕ дерево `Main/MacRunner` НЕ ТРОГАТЬ ВООБЩЕ** (там живёт Lane A).
- Коммить на ветку `pe32-lane` ИМЕНОВАННЫМИ файлами (`git add <file>`, никогда `-A`).
  Координатор мержит pe32-lane → main. Конфликтов не бойся — мерж не твоя забота.

## ШАГ 0 — восстанови свой бейзлайн в чистом дереве
1. `./scripts/disk-guard.sh`.
2. Пересобери СВОИ движковые бинарники из закоммиченного HEAD (66c9fa5): ntdll, xtajit,
   wow64 (`make -j8 dlls/ntdll/ntdll.so`, `dlls/xtajit/...`, `dlls/wow64/...` в СВОЁМ build),
   задеплой в СВОЙ dist + codesign.
3. Прогон-проверка: Diablo/hl.exe должен снова дойти до твоего фронтира
   (первый win32u syscall `svc=0x147a`). Это твой verified-baseline — зафиксируй heartbeat.

## ТЕКУЩИЙ ФРОНТИР (из твоего же PE32-PROGRESS, run-20260610-041803)
i386 доходит до ПЕРВОГО win32u (NtUser/NtGdi) syscall `svc=0x147a` из 32-bit win32u.dll
@0x7ab80000 → нативный handler падает c0000005 deref **0x201094** = сырой 32-битный guest-ptr
НЕ ребейзнут к host (guest32_base=0x3_00000000 → ждали 0x300201094). Плюс **x18(host TEB)=0**
в момент фолта. Разделение работ:
- **ТВОЁ (чини сам):** ребейз guest-ptr на границе win32u-syscall — аргументы 32-бит syscall'а
  должны транслироваться guest→host (0x201094 → 0x300201094) в твоём wow64/xtajit слое ДО
  передачи нативному handler'у. Это классическая WOW64-марш аллинг-дыра — найди, какой
  параметр класса NtUser не проходит thunk, и почини СЕМЬЁЙ (bulk: весь win32u-thunk класс
  с указателями, не один syscall).
- **НЕ ТВОЁ (уже записано в PE32-NEEDS, Lane A чинит в каноническом дереве):** потеря x18
  (host TEB) ВНУТРИ нативного вызова (init_user) — корень в signal_arm64.c/macrunner_hb.c.
  НЕ редактируй эти файлы даже в своём worktree (Lane A их активно переписывает — мерж
  станет адом, как было с interpreter.c). Уточняй NEEDS evidence'ом и иди дальше.
- Warm-prefix repro на Diablo.exe (твой же NEXT) — назови точный callee init_user и точку
  x18-потери: это ускорит Lane A.

## ДАЛЬШЕ ПО ПРОГРАММЕ (resume MEGA-PROGRAM-i386-to-real-games-20260608.md)
teb32/WowTebOffset drift → rpcss/warm-prefix (0x6ba) → win32u-марш аллинг (текущее) →
GUI app-body → **ОКНО Diablo/Notepad++ x86 (CG-capture)**. Bulk-first по i386-опкодам
(матрица HB-X86-32 + capstone). Автономно: блокер → корень → rebuild → rerun → следующий.

## ДИСЦИПЛИНА (всё в ТВОЁМ worktree)
- ctx (ctx_execute javascript) на логи, НЕ сырой cat/tail (хук блокирует).
- Прогоны: `MACRUNNER_RUN_DIR=$RUNDIR scripts/mr-run.sh …` под timeout (авто-триаж);
  смотри LADDER_RUNG в triage-summary; scoped `wineserver -k` ТОЛЬКО свои префиксы.
- Тяжёлые wine-прогоны НЕ одновременно с HK-прогонами Lane A (CPU-контеншн) — короткие
  пробы ок, длинные циклы координируй.
- Heartbeat одной строкой КАЖДЫЙ шаг в `reports/research/PE32-PROGRESS.md` (своего дерева).
- disk-guard перед длинными циклами; орфан-префиксы чисти; snapshots rolling max 3.
- Verdict = окно на экране (CG-capture) / pasted evidence, не «process alive».
