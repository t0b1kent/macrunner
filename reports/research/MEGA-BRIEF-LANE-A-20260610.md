# MEGA-BRIEF — LANE A (x64 → окно Hollow Knight). 2026-06-10

## 🏠 ТЕРРИТОРИЯ
Канон `Main/MacRunner` теперь ТВОЙ по движку: PE32 переехал в отдельный worktree
(`Main/MacRunner-pe32`, ветка pe32-lane, со СВОИМИ build+dist) — больше никто не
перезаписывает твой dist и не правит твои файлы.
- ТВОЁ: `macrunner_hb.c`, `signal_arm64.c`, `engine/wine/dlls/xtajit64/**`, `hb_*`
  (hyperbridge), ntdll unix x64-пути (loader.c/virtual.c/thread.c для x64-фронта).
- НЕ ТВОЁ: `engine/graphics|dxmt|vkd3d` (Lane D, у него там грязные правки — не трогать),
  `dlls/xtajit` (32-бит), `wow64*` (PE32), worktree MacRunner-pe32.

## 🎯 МИССИЯ — лестница до окна (объективный спидометр)
Триаж на каждом прогоне печатает `LADDER_RUNG: N (имя)  BEST: 8 (gfxdevice)`.
Текущее: ~6 (mono-init). Путь: **6 → 7 (create-window) → 8 (gfxdevice) → 9 (CreateDXGIFactory)
→ 10 (D3D11CreateDevice) → 11 (swapchain) → 12 (present) → 13 (window-visible = CG-capture)**.
Графика ДОКАЗАНА Lane D (82/82 HK-шейдеров рендерятся через DXMT→Metal) — после ступени 8
стены должны быть тонкими. Вердикт = пиксели, не «process alive».

## ТЕКУЩИЙ БЛОКЕР (ступень 6)
`GENERIC_INVALID_DISPOSITION_SEH` = c0000026-семейство — ТО ЖЕ, что 06-07 закрывал bulk-CFI
(коммит 50c1eea). Логично: ntdll/xtajit64 пересобраны из нового дерева → часть thunks без
unwind-метаданных. **Примени тот же bulk-CFI рецепт ко ВСЕМ host-call thunks текущей сборки**
(семьёй, не по одному — audit всех глобальных ASM-входов как 06-07), плюс tagged-PC
нормализация (`pc & ~1`) перед RtlLookupFunctionEntry/RtlVirtualUnwind2.

## 🛡 ПРАВИЛА, РОЖДЁННЫЕ РЕГРЕССОМ 06-10 (нарушение = повтор катастрофы)
1. **LADDER_REGRESSION в триаже = СТОП-сигнал**: сначала восстанови/проверь последний
   verified-forward бейзлайн, потом итерируй. Не наматывай вариации на регрессе.
2. **`scripts/milestone-dist-snapshot.sh <rung>` ОБЯЗАТЕЛЕН при каждом новом best**
   (триаж сам напомнит) — git хранит исходники, НЕ задеплоенную комбинацию; именно так
   потеряли GfxDevice-dist 06-07. Вернёшься на 8 — снапшот немедленно.
3. **Коммить verified-forward сразу** (именованные файлы) — милстоун не должен жить
   незакоммиченным/незаснапшоченным. Деплой поверх dist без снапшота предыдущего — нельзя.
4. Меняй ПО ОДНОМУ; после каждой сборки — прогон + LADDER_RUNG; регресс-чек миль стоуна.

## 🤝 NEEDS ОТ PE32 (вторая выгода твоих же фиксов — закрой попутно)
Из PE32-NEEDS (evidence в PE32-PROGRESS, run-20260610-041803): **x18 (host TEB) = 0 внутри
нативного win32u-вызова** (init_user) — потеря x18 в syscall/callback-пути signal_arm64
(native `__wine_syscall_dispatcher` x18-restore для bridge-context syscalls ИЛИ callback без
`prepare_arm64_pe_call`). Ты всё равно в этой зоне (CFI/dispatcher) — почини x18-restore
семьёй: разблокирует и PE32-фронт (i386 win32u), и потенциально твои же callback-пути.

## ДИСЦИПЛИНА
ctx на логи (хук блокирует сырое чтение); прогоны `MACRUNNER_RUN_DIR=$RUNDIR scripts/mr-run.sh`
под timeout (авто-триаж); scoped `wineserver -k`; disk-guard перед циклами; heartbeat одной
строкой каждый шаг в `LANE-A-PROGRESS.md`; орфан-префиксы чисти. Автономно: блокер → корень →
rebuild → rerun → следующий; стоп только окно (rung 13, CG-capture) или операторский STOP.
