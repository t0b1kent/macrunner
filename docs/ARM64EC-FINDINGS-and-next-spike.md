# ARM64EC — ПОДТВЕРЖДЁННАЯ ПРИЧИНА + план bounded-спайка (PARKED, ждёт рук)

**Date:** 2026-05-29
**Status:** PARKED — нет свободных агентов. Зафиксировано, чтобы поднять без потери контекста.
**Research:** `reports/research/ARM64EC-NTDLL-RESEARCH-chatgpt-20260529.md` (ChatGPT web search).

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

## СВЯЗЬ С ЛЕЙНАМИ
- Если arm64ec-спайк зелёный → пересборка Wine с arm64ec разблокирует Кими (видеоядро) и
  снимает срочность с системных-DLL опкодов.
- Opcode-кампания (`CODEX-OPUS-x64-opcode-coverage-MEGA`) всё равно нужна для x64-кода
  ИГРЫ (он эмулируется всегда), но для CRT/ntdll — нет.
- 32-бит (`CONTINUE-PE32-qsi-class102-hotspin`) — припаркован, для полноты.
