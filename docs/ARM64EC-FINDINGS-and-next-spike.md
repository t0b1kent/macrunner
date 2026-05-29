# ARM64EC — ПОДТВЕРЖДЁННАЯ ПРИЧИНА + план bounded-спайка

**Date:** 2026-05-29
**Status:** PART 1 СПАЙКА = GREEN (build feasibility доказана). Part 2 (wire cpu.c) — ждёт go.
**Research:** `reports/research/ARM64EC-NTDLL-RESEARCH-chatgpt-20260529.md` (ChatGPT web search).

## ⭐ SPIKE PART 1 РЕЗУЛЬТАТ — GREEN (2026-05-29, Cline build + operator verify)
Отчёт: `reports/research/ARM64EC-SPIKE-build-result-20260529.md`.
- Сборка `--enable-archs=arm64ec,aarch64,i386` (скрипт `scripts/build-wine-arm64ec-spike.sh`,
  baseline не тронут, отдельные build/dist-arm64ec-spike) — **прошла, 0 ошибок**.
- configure подтвердил: `arm64ec-w64-mingw32-clang supports -target arm64ec-windows -fuse-ld=lld
  ... yes` → **наш lld линкует arm64ec** (главный риск Q2 СНЯТ эмпирически).
- `aarch64-windows/ntdll.dll` = **`file format coff-arm64x`** (ARM64X-гибрид), экспортит
  **`__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`** ✅✅.
- Блокер Кими (отсутствие этих экспортов) на уровне сборки УСТРАНЁН.
- ⚠️ Cline выдал чек-лист + grep-дамп, но НЕ сам вердикт/проверку экспортов — закрыто
  operator-side. Урок: «build installed ✅» ≠ «arm64ec слинкован + экспорты есть».
- bison < 3.0 уронил первый configure — починено до успешной сборки.
- **NEXT = Part 2:** wire `xtajit64/cpu.c` → HyperBridge x64 (см. ниже), затем реальный x86_64 PE.

## SMOKING GUN (root cause подтверждён)
Блокер Кими (`__wine_unix_call_dispatcher_arm64ec not found`, ntdll `EXEC_FAULT`)
вызван тем, что наш Wine собран БЕЗ arm64ec:

- `scripts/build-wine-pure-arm64-experiment.sh:47` →
  `--enable-archs=aarch64,x86_64,i386` — **нет `arm64ec`**.
- Без флага `arm64ec` x86_64 собирается как чистый эмулируемый таргет, и
  `x86_64-windows/ntdll.dll` НЕ экспортирует `__wine_unix_call_dispatcher_arm64ec`
  + `KiUserEmulationDispatcher`.
- Wine loader (`dlls/ntdll/unix/loader.c`) при этом ХАРДКОДОМ требует эти экспорты
  для x86_64 PE на ARM64-хосте → мы уже наполовину в arm64ec-модели, ntdll собран не так.

## У НАС ЕСТЬ ВСЁ, ЧТОБЫ ПОПРОБОВАТЬ
- **Toolchain умеет arm64ec:** `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal/bin/`
  содержит `arm64ec-w64-mingw32-clang++`, `arm64ec-w64-mingw32-strip`,
  `arm64ec-w64-mingw32-clang-scan-deps`. Дата 2026-05 — за LLVM 21 (2025), где arm64ec дозрел.
- **Версия Wine:** форк Wine 11 (> 10.0, где «полная поддержка arm64ec» — релиз янв 2025).
- Конфиг по отчёту: `--enable-archs=arm64ec,aarch64,i386 --with-mingw=clang --disable-tests`.

## ГИПОТЕЗА (проверить, не факт): arm64ec может растворить блокеры разом
В arm64ec-модели системные DLL (ntdll, CRT) — гибридные, бегут НАТИВНО на ARM;
HyperBridge эмулирует только x64-код игры. Тогда:
- `UNSUPPORTED_OPCODE в CRT init` (`dx11_clear_present`) может ИСЧЕЗНУТЬ (CRT не эмулируется).
- ntdll `EXEC_FAULT` исчезает (ntdll нативный).
- Opcode-кампания для системных DLL теряет срочность (опкоды нужны только для x64-кода игры).
ЕСЛИ подтвердится → arm64ec важнее opcode-кампании. Проверить спайком.

## ГЛАВНЫЙ РИСК (открытый вопрос)
LLD может НЕ до конца линковать arm64ec hybrid PE (отчёт: кто-то всё ещё юзает MSVC
`link.exe`). У нас есть arm64ec-**компилятор**, но умеет ли наш **lld линковать** arm64ec —
НЕПРОВЕРЕНО. Это убивающий риск спайка. (Доп. research отдан ChatGPT — см. ниже.)

## BOUNDED SPIKE (1-2 дня, когда появятся руки — Codex/Opus)
1. Добавить `arm64ec` в `--enable-archs` (новый билд-скрипт, НЕ ломать рабочий).
2. Прогнать `configure` — проходит ли.
3. Собрать ntdll — **линкуется ли нашим lld** (главная проверка риска).
4. `llvm-nm`/`llvm-objdump` по `x86_64-windows/ntdll.dll`: появились ли
   `__wine_unix_call_dispatcher_arm64ec` + `KiUserEmulationDispatcher`.
5. РЕШЕНИЕ: зелёно → arm64ec путь (разблокирует видеоядро). lld не линкует → opcode-кампания
   (Model A) основная, arm64ec позже / Rosetta-fallback.
- **STOP-условие:** если lld не линкует arm64ec — НЕ городить MSVC link.exe через wine
  на ходу, эскалировать оператору. Спайк = ответ feasibility, не имплементация.

## ВТОРОЙ ОТКРЫТЫЙ ВОПРОС → отдан ChatGPT (research #2)
Может ли НАШ HyperBridge встать на место FEX за ARM64EC `KiUserEmulationDispatcher`
(контракт эмулятора), или ARM64EC жёстко завязан на FEX? Это экзистенциально для тезиса
«свой транслятор, не FEX, не Rosetta». См. `docs/CHATGPT-SEARCH-arm64ec-emulator-contract-prompt.md`.

## RESEARCH #2 РЕЗУЛЬТАТЫ — ОБА ВОПРОСА ЗЕЛЁНЫЕ (2026-05-29)
Отчёт: `reports/research/ARM64EC-EMULATOR-CONTRACT-chatgpt-20260529.md`.

**Q1: Может ли HyperBridge заменить FEX? → ДА, подтверждено.**
- Wine грузит эмулятор ПО ИМЕНИ DLL, не хардкод FEX. Дефолт `xtajit64.dll`,
  override через реестр `HKLM\Software\Microsoft\Wow64\amd64`. Wine не проверяет
  происхождение DLL → любой модуль с нужными экспортами годится.
- Контракт = экспорты `BTCpu*`. Минимум обязательных: `BTCpuProcessInit`,
  `BTCpuSimulate`, `BTCpuGetBopCode`. Остальное — опционально/заглушки.
- `signal_arm64ec.c` грузит указатели через `RtlFindExportedRoutineByName`,
  `load_arm64ec_module()` в `loader.c` выбирает имя.

**Q2: Линкует ли наш lld arm64ec? → ДА.**
- LLVM 21+ (осень 2025) имеет достаточную поддержку arm64ec/arm64x. llvm-mingw 2026
  (22.x) ставит lld, линкующий arm64ec; префикс `arm64ec-w64-mingw32` работает без
  MSVC link.exe. Наш toolchain (20260505) — в этом диапазоне.
- Caveat: редкие фичи (import-thunk оптимизация, `arm64xsameaddress`) могут ещё
  требовать link.exe, но базовая функциональность есть.

## КЛЮЧЕВОЕ ОТКРЫТИЕ: СКЕЛЕТ УЖЕ В ДЕРЕВЕ
- `engine/wine/dlls/xtajit64/` УЖЕ существует: `cpu.c` (250 строк) + `xtajit64.spec`.
- `xtajit64.spec` УЖЕ экспортит правильный интерфейс: `BTCpu64FlushInstructionCache`,
  `BTCpu64IsProcessorFeaturePresent`, `BeginSimulation`, `ProcessInit`, `ThreadInit`,
  `ResetToConsistentState`, `DispatchJump`, `RetToEntryThunk`, `ExitToX64` и т.д.
- `engine/wine/dlls/ntdll/signal_arm64ec.c` тоже есть (Wine-сторона готова).
- **НО `xtajit64/cpu.c` — ЗАГЛУШКА:** строки 41/53/65/75 = `ERR("x64 emulation not
  implemented")`. Симуляция (BeginSimulation → диспетч в HyperBridge) НЕ подключена.
- **Рабочий reference:** `engine/wine/dlls/xtajit/cpu.c` (32-бит) УЖЕ роутит BTCpu →
  HyperBridge x86 (поэтому PE32 вообще бежит). xtajit64 = зеркаль этот паттерн для x64.

## ИТОГОВЫЙ ПЛАН (когда появятся руки — Codex/Opus, НЕ Flash)
Это НЕ greenfield. Две части:
1. **Build:** новый билд-скрипт с `--enable-archs=arm64ec,aarch64,i386 --with-mingw=clang`.
   Собрать → проверить что ntdll экспортит `__wine_unix_call_dispatcher_arm64ec` +
   `KiUserEmulationDispatcher` и что xtajit64.dll собирается (lld линкует — Q2 green).
2. **Wire:** заполнить заглушки в `engine/wine/dlls/xtajit64/cpu.c` — подключить
   `BeginSimulation`/dispatch в HyperBridge x64 (`hb_decode_x64`/`hb_lift_x64`/
   `hb_interpreter`). Зеркалить рабочий `dlls/xtajit/cpu.c` (32-бит).
- Это путь к x64-играм БЕЗ Rosetta/FEX на нашем движке. Стратегически > Rosetta-fallback.

## RESEARCH #3 (отдан ChatGPT) — имплементационный reference
Точная семантика `BeginSimulation`/`ProcessInit`/BOP-code/`DispatchJump`/`RetToEntryThunk`/
`ExitToX64` — что они должны делать, как FEX `libarm64ecfex.dll` это реализует. Чтобы
заполнить `xtajit64/cpu.c`. См. `docs/CHATGPT-SEARCH-arm64ec-xtajit64-impl-prompt.md`.

## RESEARCH #3 РЕЗУЛЬТАТЫ — ИМПЛ-КОНТРАКT xtajit64 ГОТОВ (2026-05-29)
Отчёт: `reports/research/ARM64EC-XTAJIT64-IMPL-chatgpt-20260529.md`. ARM64EC research ЗАКРЫТ.

**ОБЯЗАТЕЛЬНЫЕ экспорты (без них Wine падает / некорректность):**
- `ProcessInit` — раз при загрузке (`arm64ec_process_init()`), вернуть STATUS_SUCCESS.
- `ThreadInit` — на каждый поток, инициализировать состояние эмулятора.
- `BeginSimulation` — ЯДРО. Вызывается из `dispatch_emulation()` в `KiUserEmulationDispatcher`.
  x64-контекст уже в `get_arm64ec_cpu_area()->ContextAmd64`. Крутить JIT/interp, исполняя
  x64, пока не встретит ARM64-адрес → выйти, сохранив контекст, `InSimulation=0`.
- `DispatchJump`/`RetToEntryThunk`/`ExitToX64` — транзишн-thunk'и x64↔ARM64
  (= `__os_arm64x_x64_jump`/`_dispatch_ret`/`_dispatch_call_no_redirect`). Через `x9`
  (целевой адрес) + `RtlIsEcCode`: ARM64-код → entry-thunk, иначе → переход в эмулятор.
- `UpdateProcessorInformation` — заполнить как x86_64 (`PROCESSOR_ARCHITECTURE_AMD64`).
- `BTCpu64IsProcessorFeaturePresent` — TRUE для реализованных x86_64 фич (MMX/SSE/NX…).

**Заглушки ОК на первом этапе (дополнять по тестам):**
- `ThreadTerm`, `ProcessTerm`, все `Notify*` (MemoryAlloc/Free/Protect, Map/UnmapView,
  MemoryDirty, ReadFile), `FlushInstructionCache*` (нужны для self-modifying code/
  системных либ/отладчика, но не для первого запуска), `ResetToConsistentState` (нужна
  при исключениях — заглушка возможна, но возможны баги на SEH).

**КЛЮЧЕВОЕ ОТЛИЧИЕ от 32-бит xtajit:** в ARM64EC-x64 НЕТ аналога BOP-кода
(`BTCpuGetBopCode` есть только в 32-бит). Переход в эмулятор идёт через аппаратное
исключение BRK → `KiUserEmulationDispatcher` → `BeginSimulation`, и через thunks, НЕ через
BOP. То есть зеркалить 32-бит xtajit можно по структуре, но механизм входа другой.

**FEX как reference:** исходники `libarm64ecfex.dll` непубличны, но: модуль зависит только
от ntdll (грузится рано), `BeginSimulation` берёт контекст из `get_arm64ec_cpu_area()`
(FEX-2409 коммит `cc589ba` «Always use the CPU area context for BeginSimulation»).

**ИТОГ: вся ARM64EC-разведка завершена.** Дальше — только исполнение (build + wire
xtajit64/cpu.c → HyperBridge x64). Больше ресёрча по ARM64EC не нужно.

## СВЯЗЬ С ЛЕЙНАМИ
- Если arm64ec-спайк зелёный → пересборка Wine с arm64ec разблокирует Кими (видеоядро) и
  снимает срочность с системных-DLL опкодов.
- Opcode-кампания (`CODEX-OPUS-x64-opcode-coverage-MEGA`) всё равно нужна для x64-кода
  ИГРЫ (он эмулируется всегда), но для CRT/ntdll — нет.
- 32-бит (`CONTINUE-PE32-qsi-class102-hotspin`) — припаркован, для полноты.
