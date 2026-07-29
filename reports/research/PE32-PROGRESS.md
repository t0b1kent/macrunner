# PE32 Progress Log

## 2026-06-07 — Lane PE32 (Sonnet) post-SEH re-probe

09:00 · start · Lane A SEH fix confirmed (MILESTONE-20260607). Re-running Diablo i386 + Terraria i386 to check if c0000026 is cleared. Previous evidence: btcpu=11/44 then seh_tail=64. Running via mr-run.sh + MACRUNNER_RUN_DIR auto-triage.

### Phase 1 confirmation (context-resumed session)

**Phase 1 = PASSED** — BTCpuProcessInit + BTCpuThreadInit + BTCpuSimulate all confirmed for Diablo.exe (i386).

Evidence from `reports/phase-h/pe32-diablo-xtajit-syscalls-20260607-100835/`:
- `BTCpuProcessInit status=00000000` ✓
- `BTCpuThreadInit status=00000000` ✓
- `BTCpuSimulate progress count=1 eip=7bdc7640 steps=3534 blocks=851` ✓
- 427 syscalls dispatched within single BTCpuSimulate call (30s trace)
- SEH/unwind 192 occurrences (c0000026 still fires, WOW64 thunk range 0x87FFF...)

### Syscall decode (via winedump on i386 ntdll.dll)

Dominant calls in 30s trace (in-order, all DLL init):
```
svc=0x50 (184x) → NtProtectVirtualMemory  ← JIT marking code exec per-block
svc=0x0f  (58x) → NtClose
svc=0x51  (32x) → NtQuerySection
svc=0x33  (22x) → NtOpenFile               (3 c0000034 = file-not-found)
svc=0x37  (18x) → NtOpenSection            (4 c0000034)
svc=0x39  (18x) → NtFsControlFile          (FSCTL_GET_OBJECT_ID=0x9009c, handle 0x30 reused)
svc=0x4a  (18x) → NtCreateSection         ← 18 PE sections mapped across ~5 DLLs
svc=0x28  (18x) → NtMapViewOfSection      ← 18 = ~5 DLLs × sections
svc=0x17  (14x) → NtQueryValueKey          (11 c0000034, 1 80000005 = probe+retry)
svc=0x1d   (8x) → NtCreateKey
svc=0x18   (6x) → NtAllocateVirtualMemory
svc=0x12   (5x) → NtOpenKey               (1 c0000034)
svc=0x9b   (5x) → NtGetNlsSectionPtr      ← NLS/locale init
```

Forward progress confirmed: ecx addresses in NtOpenFile calls strictly increase
(0x00294476 → 0x00295974), different file paths, not a loop.

NtMapViewOfSection handles seen: 0x34, 0x38, 0x3C, 0x40, 0x44 (5 distinct DLL file handles,
each section mapped independently). App still in DLL initialization at 30s timeout.

### 120s + 60s investigation results (context-resumed session, 2026-06-07 10:xx)

**Run: reports/phase-h/pe32-diablo-120s-wow64trace-20260607-102700/**
- exit=143 (SIGTERM at 120s — app alive for full 120 seconds!)
- DLL loading completes at ~line 589 (NtQuerySystemInformation class=102 = timezone)
- After DLL loading: 100+ seconds of activity with NO NtMapViewOfSection, NO Wow64SystemServiceEx
- This means NO window creation calls in 120 seconds

**Run: reports/phase-h/pe32-diablo-60s-postdll-20260607-103056/**
- exit=143 (SIGTERM at 60s)
- Post-DLL svc distribution:
  - 0x50 (NtProtectVirtualMemory): 28x — heap/JIT guard pages
  - 0x1d (NtCreateKey): 5x — registry writes
  - 0x17 (NtQueryValueKey): 5x (2 failed = key not found)
  - unix bop id=2 (wow64_wine_dbg_write): MANY — 32-bit Wine debug output (suppressed by -all)
- NO svc=0x1000+ (NtUser/win32k) calls → window creation NOT reached
- 1 post-DLL NtMapViewOfSection → one DLL delay-loaded (possibly ddraw.dll)

**unix bop id=2 decoded:**
From `engine/wine/dlls/ntdll/unix/loader.c:1855 unix_call_wow64_funcs`:
- id=0: wow64_load_so_dll
- id=1: wow64_unwind_builtin_dll
- **id=2: wow64_wine_dbg_write** ← all returning ffffffff = debug output suppressed by WINEDEBUG=-all

**Status:** App runs 120s alive but NO window. App is pre-CreateWindow, doing:
- Registry reads (game config?)
- VirtualProtect (heap guard pages)
- Wine debug output from 32-bit DLLs (storm.dll? diabloui.dll? ddraw.dll?)

Next: run with WINEDEBUG=+ddraw,+win,+user,+macdrv to see what the 32-bit code is doing.

## 2026-06-06 — wow64cpu/xtajit/i386-ntdll handoff checkpoint

- `engine/wine/dlls/wow64/syscall.c: process_init()` is the handoff entry for BTCpu init:
  - calls `get_cpu_dll_name()`
  - `module = load_64bit_module( get_cpu_dll_name() )`
  - resolves all `BTCpu*` exports via `RtlFindExportedRoutineByName`
  - logs `before BTCpuProcessInit` -> `pBTCpuProcessInit()`
- `get_cpu_dll_name()` selects:
  - `xtajit.dll` for `current_machine == IMAGE_FILE_MACHINE_I386` on `IMAGE_FILE_MACHINE_ARM64` native.
  - `wow64cpu.dll` otherwise for x86\_64 path.
- In run `reports/phase-h/npp-x86-diagnostic-20260606-150550`, triage shows required i386 DLLs are missing:
  - `i386 wow64cpu.dll` in `bottles/generic-x86/drive_c/windows/syswow64` is missing.
  - `i386 xtajit.dll` in `bottles/generic-x86/drive_c/windows/syswow64` is missing.
- `summary.txt`/`triage-summary.txt` classify `PE32_WOW64CPU_NOT_LOADED` and `LOADER_STALL_BEFORE_BTCPUSIMULATE` with `BTCpuSimulate_status_count=0`.
- `stdout.log`/`stderr.log` contain no `BTCpuProcessInit` or `BTCpu*` markers, while the prefix contains valid `i386 ntdll.dll` and `i386 kernel32.dll`.
- Finding: handoff chain is blocked before execution reaches `BTCpuProcessInit`; missing i386 `syswow64` CPU runtime DLL in the prefix prevents `load_64bit_module(get_cpu_dll_name())` from producing a module with BTCpu exports.

Next step: populate `bottles/generic-x86/drive_c/windows/syswow64/{wow64cpu.dll,xtajit.dll}` (or registry override via `Wow64\\x86`) and re-run with WOW64 handoff markers enabled.

## 2026-06-07 — clean handoff trace classification

Run dirs:
- `reports/phase-h/npp-x86-handoff-manual-20260607-031851`
- `reports/phase-h/npp-x86-handoff-clean-20260607-032035`

Command shape:
- Manual equivalent of `reports/phase-h/run_window_clean.sh`, because that script forces `WINEDEBUG=-all`.
- `WINEPREFIX=$PWD/bottles/generic-x86`
- `WINEDLLPATH=$PWD/engine/wine/dist-pure-arm64/lib/wine`
- `WINEDEBUG=+loaddll,+module`
- `MACRUNNER_XTAJIT_TRACE_SYSCALLS=1`
- `MACRUNNER_XTAJIT_TRACE_STACK=1`
- Scoped cleanup only: `WINEPREFIX=$PWD/bottles/generic-x86 engine/wine/dist-pure-arm64/bin/wineserver -k`

Observed loader evidence from `reports/phase-h/npp-x86-handoff-clean-20260607-032035/stderr.log`:
- `notepad++.exe` is mapped as a PE32/i386 executable:
  - `trace:module:get_load_order ... artifacts\\phase-h\\npp-x86\\notepad++.exe`
  - `trace:module:map_image_into_view mapping PE file ... notepad++.exe`
- `C:\\windows\\system32\\start.exe` and `C:\\windows\\system32\\ntdll.dll` load.
- Counts from the processed log:
  - `notepad++`: 9
  - `wow64.dll`: 0
  - `wow64cpu.dll`: 0
  - `wow64win.dll`: 0
  - `xtajit.dll`: 0
  - `BTCpu`: 0
  - `macrunner-wow64`: 0
  - `macrunner-xtajit`: 0
  - loader/errors: 0

Prefix DLL presence captured in `env.log`:
- `syswow64/wow64.dll exists=0`
- `syswow64/wow64cpu.dll exists=0`
- `syswow64/wow64win.dll exists=0`
- `syswow64/xtajit.dll exists=0`
- `syswow64/ntdll.dll exists=1`
- `syswow64/kernel32.dll exists=1`
- `system32/wow64.dll exists=1`
- `system32/wow64cpu.dll exists=1`
- `system32/wow64win.dll exists=1`
- `system32/xtajit.dll exists=1`

Post-run classifier:
- `python3 tools/triage/classify_run.py reports/phase-h/npp-x86-handoff-clean-20260607-032035`
- Output class: `WAIT_TRACE_INSUFFICIENT` with secondary `PE32_HANDOFF_TRACE_INSUFFICIENT`.
- This generic classifier does not override the handoff-specific evidence above.

Handoff classification:
- `wow64cpu` load: no evidence of load.
- `xtajit.dll` lookup/load: no evidence of lookup/load.
- `BTCpu*` export resolution: not reached.
- `BTCpuProcessInit`: not called.
- Root class for this narrow gate: backend not loaded before BTCpu resolution, not export mismatch and not `BTCpuProcessInit` failure.

Current conclusion:
- The i386 handoff never reaches `wow64/syscall.c:process_init()` markers. The PE32 image maps, but the WOW64 CPU-provider layer is not entered. The prefix has i386 `ntdll.dll`/`kernel32.dll`, but no i386 `syswow64` copies of `wow64.dll`, `wow64cpu.dll`, `wow64win.dll`, or `xtajit.dll`; only `system32` has those CPU-provider DLLs.

## 2026-06-07 — phase 0 deploy+trace checkpoint

2026-06-07 03:48 · Phase 0 · sync-prefix fix + trace retry ·
Что сделано: `scripts/sync-prefix-from-dist.sh` теперь деплоит `wow64cpu.dll`, `wow64.dll`, `wow64win.dll`, `xtajit.dll` в `syswow64`; ручной redeploy для `bottles/generic-x86` подтвердил все файлы в `syswow64`.
Результат/классификация: `macrunner_hb_x64_main_requested` логи есть, но `wow64/xtajit/BTCpuProcessInit` маркеры отсутствуют, `PE32-` трасса до сих пор `WAIT_TRACE_INSUFFICIENT`; первичная причина текущего узла сейчас — `PE32_WOW64CPU_NOT_LOADED` / backend не грузится до handoff.
Next: keep Phase 0 active, identify why 32-bit path still stays on 64-bit bootstrap path before wow64 handoff and retry with explicit process-init-only wow64 tracing.
03:58 · диагностика каналов · ctx_batch_execute через shell не работает (ENOENT /bin/zsh), переключаюсь на exec_command для rg/чтения точечных файлов · продолжить локализацию причины syswow64-path
03:58 · локальный поиск bootstrap ·  без wildcards, т.к. shell прерывал из‑за пустых матчей путей · детектировать функцию выбора WOW64 CPU DLL в 
04:00 · Поиск причин выбора bootstrap и WOW64 path · context-mode запускался с ENOENT/запрос в zsh, анализ не получен · Проверю целевые файлы через прямые команды
04:01 · Продолжение диагностики bootstrap WOW64 для i386 · Проверка текущего состояния и трасс PE32 · Далее прогон с трассой bootstrap
04:04 · локальная трасса конфликта bootstrap · найден источник x64-bootstrap env override для PE32 (конфиг/скрипты) -> следующий шаг: безопасно отключить x64 bootstrap для machine=x86 в скриптах запуска/обертках; затем немедленный rerun run_window_clean
04:04 · правка деплой-раннеров в процессе · MACRUNNER_HB_X64_LOADER=0 для machine=x86 в run-windows-app + run_window_clean; next run_window_clean TRACE check (BTCpu/wow64 markers)
04:08 · heartbeat start · прочитал требования и стартовал поиск в wow64/x86 chain · next: найти функцию выбора machine/bootstrap
04:22 · локальная диагностика -> точечный правка в run-notepad-x64.sh · обнаружена hardcoded MACRUNNER_HB_X64_LOADER=1 для x86-кейсов · следующий шаг: изменить на machine-aware env и rerun handoff trace
04:25 · start · inspect progress and handoff context for PE32 root-cause path
04:25 · step1 · reviewed PE32-PROGRESS checkpoints and last findings about syswow64 path/handoff markers
04:28 · диагностика · найден глобальный дефолт X64-loader в mr-run + run-notepad для i386 проверки · переходим к чтению runner скриптов и правке локального scope
04:29 · диагностика · сверка wow64 handoff функции на предмет x64/arm64ec/bootstrap флага и current_machine · план: оставить логику в script/runner без правок ядра
04:30 · Инициация фикс-прохода PE32 · подтверждена трасса: x64 bootstrap включается для i386 через MACRUNNER_HB_X64_LOADER · правка скриптов запуска и повторный сбор/прогон
04:36 · Анализ скриптов — найдено несколько точек конфигурации x64 loader (run-notepad*, run-notepad-x64-ui-smoke, возможно run-windows-app) с потенциалом перетирания значения для x86
04:41 · Выявил детерминированный источник включения x64-загрузчика: в run-notepad* используется set-if-unset для MACRUNNER_HB_X64_LOADER
04:45 · Патч: run-notepad-x64.sh теперь всегда ставит x64-loader=0 для x86 PE, иначе не трогает если x64 и уже задано вручную
04:48 · Патч: run-notepad-x64-ui-smoke.sh синхронизирован с тем же правилом x86=0, иначе только когда env не задан
04:52 · Подтверждена доступность PE32 артефакта npp-x86/notepad++.exe для проверки после правки
05:00 · Проверочный прогон выполнен с принудительно заданным MACRUNNER_HB_X64_LOADER=1 для x86 app; запуск завершился неуспешно (rc=1), собираю артефакты
05:10 · run-windows-app запуск 30s на npp-x86 завершился по таймауту инструмента/внутренне; логи есть в /tmp/pe32-rwa.log для пост-анализа
05:15 · run-windows-app дал timeout и записал stdout/stderr в reports/run-windows-app/run-20260607-043106.*; продолжаю чтение только через ctx_* по сути запуска
05:23 · Для проверки переключения bootstrap запускаю системный поиск по логу run-windows-app на строки x64-loader и BTCpuProcessInit
05:33 · Второй прогон run-notepad-x64.sh завершён (без свежести-превью); логи в /tmp/pe32x86-run2.log для анализа
05:43 · Прогон с MACRUNNER_NPP_X64_DIR=npp-x86 завершился; собираю run-лог и проверяю machine/loader признаки
05:46 · Лог теперь указывает APP=/artifacts/phase-h/npp-x86; запускается и сразу kill после 20s, проверяю трассу в RUN-dir
05:55 · Прогон синтакс-проверки: run-notepad-x64.sh и run-notepad-x64-ui-smoke.sh OK после правок
LOOP-STATUS: CONTINUE — верификация bootstrap частичная: два фронтовых скрипта переведены на принудительный 0 для x86; следующий шаг — подтверждение через run-windows-app/run-notepad x86 с трассой macrunner_hb_x64_main_requested и BTCpuProcessInit в live-запусках

08:01 · старт PE32 loop · прочитал checkpoint: syswow64 deploy уже закрыт, текущий blocker — PE32 уходит в x64/arm64ec bootstrap до WOW64/i386 · следующий шаг: статически найти machine/bootstrap выбор и проверить, не требуется ли Lane C loader.c
08:01 · dirty-tree audit · обнаружены многочисленные чужие изменения, включая loader.c/x64/graphics; свои действия ограничиваю PE32/deploy и reports/research · следующий шаг: ctx-поиск routing-пунктов bootstrap
08:02 · ctx_batch limitation · ctx_batch_execute в этом окружении всё ещё падает на spawn /bin/zsh ENOENT · переключаюсь на ctx_execute javascript с execFile/FS без сырого вывода
08:03 · mega-program сверка · актуальный файл найден в reports/research/CODEX-MEGA-PROGRAM-pe32-32bit-to-real-apps.md; gate Phase 0 = BTCpuProcessInit/BTCpuThreadInit в логе · следующий шаг: проверить live routing после предыдущих runner fixes
08:06 · loader read-only audit · loader.c показывает x64 bootstrap только если main image AMD64 + env enabled; PE32 может обходиться через 64-bit wrapper/start.exe или bootstrap host-module remap · следующий шаг: доказательный NPP x86 trace с machine/env markers
08:04 · proof run · NPP x86 trace собран в reports/phase-h/pe32-handoff-proof-20260607-080428, процесс остановлен scoped timeout через wineserver -k · следующий шаг: ctx-классификация stderr/stdout на BTCpu/wow64/bootstrap markers
08:08 · proof classification · первый trace показал только prefix wineboot main=aa64 при loader env=0, до notepad++.exe не дошёл; wow64/BTCpu=0 · следующий шаг: повторить с MACRUNNER_HB_SKIP_WINEBOOT/MACRUNNER_SKIP_WINEBOOT=1
08:06 · skipboot proof run · trace собран в reports/phase-h/pe32-handoff-proof-skipboot-20260607-080556, scoped timeout остановил процесс · следующий шаг: классифицировать notepad++/WOW64 markers
08:10 · root evidence · notepad++.exe PE32 мапится, но build_main_module выбирает C:\\windows\\system32\\start.exe machine=aa64; wow64/xtajit/BTCpu=0 · следующий шаг: найти source fallback на start.exe для non-native PE32
08:13 · source trace · fallback source: ntdll/unix/env.c load_main_exe() treats non-success PE32 map as invalid and calls load_start_exe(); Unix loader PE32 routing is gated by macrunner_hb_x64_loader · следующий шаг: compare env=1 vs env=0 before writing PE32-NEEDS
08:17 · compare env=1 · MACRUNNER_HB_X64_LOADER=1 still maps notepad++.exe then falls back to start.exe machine=aa64; no PE32 i386-lane/wow64/BTCpu markers · следующий шаг: identify virtual_map_module mismatch path and write Lane C need if outside PE32 ownership
08:20 · Lane C handoff written · reports/research/PE32-NEEDS.md создан с evidence по start.exe fallback и mapping.c/env.c/loader.c точками · следующий шаг: укрепить PE32-owned runner/deploy без правки ntdll/server
08:14 · deploy readiness · sync-prefix-from-dist rerun OK for bottles/generic-x86, log reports/phase-h/pe32-sync-prefix-20260607-081418.log (1234 lines) · следующий шаг: проверить машинные типы и BTCpu exports в деплое
08:22 · PE32 deploy/export readiness · syswow64/start.exe+kernel32.dll are x86; xtajit.dll is arm64 and exports BTCpuProcessInit/BTCpuThreadInit/BTCpuSimulate; wow64cpu exports BTCpuProcessInit too · следующий шаг: run classifier and finalize blocked/continue status
LOOP-STATUS: BLOCKED Lane C loader/server handoff: PE32 notepad++.exe maps, then ntdll/unix env.c load_main_exe/load_start_exe replaces it with ARM64 C:\windows\system32\start.exe before WOW64, so BTCpuProcessInit cannot be reached; exact evidence and requested fix recorded in reports/research/PE32-NEEDS.md.

## 2026-06-07 — context-resumed session (post-MEM_COMMIT fix)

### Root-cause: Storm.dll MEM_COMMIT tracking bug in xtajit/HyperBridge

**Bug**: `hb_memory_guest32_map` in HyperBridge rejects any address range that overlaps an existing region (line 585 of `hb_memory.c`: `if (any_overlap(...)) return HB_ERR_INVALID_ARG`).

When 32-bit Storm.dll does:
1. `NtAllocateVirtualMemory(MEM_RESERVE, PAGE_NOACCESS)` size=0x10000 at 0x05f50000
   → `hb_memory_guest32_map(base=05f50000, size=10000, perm=0)` succeeds — creates [05f50000-05f60000] perm=0
2. `NtAllocateVirtualMemory(MEM_COMMIT, PAGE_EXECUTE_READWRITE)` size=0x1000 at 0x05f50000
   → `hb_memory_guest32_map(base=05f50000, size=1000, perm=7)` **FAILS** with HB_ERR_INVALID_ARG (overlap!)

Result: first 4KB stays perm=0 in xtajit's guest memory map. Storm.dll writes to [esi+4]=0x05f50004 → MEMORY_FAULT → c0000005 → exception handler exits with code 5.

**Fix applied**: `engine/wine/dlls/xtajit/unixlib.c` → `map_guest_range()`:
```c
result = hb_wow64cpu_notify_memory_alloc( &process, base, params->size, perm );
/* MEM_COMMIT on an already-reserved range fails the overlap check in guest32_map.
 * The OS accepted it, so split and reperm the existing region instead. */
if (result == HB_ERR_INVALID_ARG && (params->type & MEM_COMMIT))
    result = hb_memory_guest32_protect( process.memory, base, params->size, perm );
```

`hb_memory_guest32_protect` correctly splits the existing [05f50000-05f60000] region into [05f50000-05f51000] perm=7 + [05f51000-05f60000] perm=0.

### Run results with fix (reports/phase-h/pe32-diablo-commitfix2-20260607-105322/)

- **exit=143** (SIGTERM at 90s) — Diablo now runs full 90s! (was exit=5 before fix)
- **MemoryWineUnixFuncs num=0x23 → status=00000000** — unix function table lookup now SUCCEEDS (was c0000139 before)
- **NO MEMORY_FAULT / NO Storm.dll crash** — confirmed zero c0000005 events
- **c0000026 loop**: starts after NtMapViewOfSection of 4.4MB DLL (ddraw/storm?), pc=0x0000087FFF80C374 in WOW64 thunk image=0x87FFF7A0000 offset=0x6C374 — **Lane A blocker**

### Phase 2 blocker map (as of 2026-06-07 10:55)

| Blocker | Status | Owner |
|---|---|---|
| Storm.dll MEM_COMMIT perm=0 → MEMORY_FAULT (exit=5) | FIXED | PE32 |
| MemoryWineUnixFuncs c0000139 → funcs=NULL | CLEARED (side-effect) | PE32 |
| c0000026 at WOW64 thunk image=0x87FFF7A0000 pc offset=0x6C374 | BLOCKING Phase 2 | Lane A |

**c0000026 trigger sequence (lines 459-469):**
1. MemoryWineUnixFuncs succeeds (status=00000000)
2. NtMapViewOfSection handle=0x34 at 0x7bc20000 (1.25MB DLL) + BTCpuGetBopCode
3. NtMapViewOfSection handle=0x38 at 0x7b7c0000 (4.4MB DLL) + BTCpuGetBopCode
4. → c0000026 loop starts (DllMain of the 4.4MB DLL triggers an exception that can't unwind through the WOW64 thunk)

Lane A needs to register unwind metadata (`RtlInstallFunctionTableCallback` or RUNTIME_FUNCTION entries) for the thunk region at image=0x87FFF7A0000 specifically covering offset 0x6C374. The bulk CFI fix from commit 50c1eea may not cover this thunk address.

LOOP-STATUS: BLOCKED Lane A — c0000026 at WOW64 thunk image=0x87FFF7A0000 offset=0x6C374 prevents DLL initialization from completing. PE32 fix (MEM_COMMIT tracking in unixlib.c) applied and verified: Storm.dll crash eliminated, MemoryWineUnixFuncs unblocked, Diablo survives 90s. Remaining path to Phase 2 (i386 window) requires Lane A to cover thunk offset 0x6C374 with unwind metadata.
17:06 · PE32 resume · принял i386→window миссию; dirty tree большой, PE32 latest читаю через ctx · следующий шаг: disk-guard + latest verdict
17:08 · disk/latest triage · 148GB free; latest primary=GENERIC_ACCESS_VIOLATION eip=00000018, callback trace only secondary · следующий шаг: parse run.log event order через ctx
17:10 · run.log event parse · процесс стартует; финал зациклен на i386 fetch из non-exec stack/arena 00410000-01410000, затем ARM64EC breakpoint contexts · следующий шаг: найти PE32 exception handoff код
17:13 · xtajit source read · exception handoff вызывает Wow64PassExceptionToGuest; callback route trace уже есть env-gated, но был выключен · следующий шаг: diagnostic run с callback/PE32 bytes trace
17:16 · diagnostic run setup · создан reports/pe32/run-20260609-callbacktrace-1716 · следующий шаг: запустить Diablo/Half-Life i386 через dist-arm64ec-spike с WINEDEBUG=+seh
17:18 · exe check · latest воспроизводимый target найден: artifacts/games/half-life/Half-Life/hl.exe; Diablo path в bounded search не найден · следующий шаг: run с тем же exe для apples-to-apples
17:19 · diagnostic run launched · pid=13487, run.log=reports/pe32/run-20260609-callbacktrace-1716/run.log, env WINEDEBUG=+seh + callback/PE32 bytes · следующий шаг: читать growing log через ctx
17:21 · run launch invalid · pid=13487 exited immediately with empty run.log, verdict invalid · следующий шаг: restart same run via exec session без shell background
17:23 · diagnostic run evidence · run.log already 67MB; callback-route rejects present; repeated native guest32 execute route pc=00418000 → xtajit non-exec stack fault · следующий шаг: aggregate route/eip sequence
17:25 · root evidence · native SEH calls handler=00418000 with frame=00000003001F2000 (host TEB32), so TEB32 is being treated as native SEH frame · следующий шаг: fix xtajit native ExceptionList isolation
17:28 · root fix applied · xtajit/unixlib.c treats non-fault HB_ERR_STEP_LIMIT as cooperative yield instead of STATUS_UNSUCCESSFUL/RtlRaiseStatus · следующий шаг: rebuild dist-arm64ec-spike xtajit
17:31 · build plan · target=dlls/xtajit/all, install artifacts copied into dist-arm64ec-spike only · следующий шаг: run make with config/env.sh and ccache
17:34 · cleanup diagnostic run · stopped only our classify_run/analyze_generic_fallback for 67MB diagnostic log; exec session exited 143 · следующий шаг: wait xtajit build
17:36 · build/install · make dlls/xtajit/all PASS; copied fresh xtajit.so into dist-arm64ec-spike/aarch64-unix · следующий шаг: real run WINEDEBUG=+seh without noisy callback trace
17:38 · verify run setup · disk free 143GB; run dir reports/pe32/run-20260609-step-limit-fix-1738 · следующий шаг: launch WINEDEBUG=+seh real run
17:41 · verify read · process starts; no c0000001/native TEB handler/00418000 route after fix; new blocker is livelock at eip=7bdcd373 with repeated 1,000,006-step chunks · следующий шаг: root-cause 7bdcd373 loop
17:45 · step-limit fix applied · hb_context.c keeps x64 default 1M, raises x86 default to 10M with MACRUNNER_HB_X86_STEP_LIMIT override · следующий шаг: rebuild hyperbridge + relink xtajit
17:48 · rebuild/install · libhyperbridge.a rebuilt (hb_context.o), xtajit relink PASS, fresh xtajit.so copied to dist · следующий шаг: disk guard + verification rerun
17:49 · verify2 setup · disk free 141GB; run dir reports/pe32/run-20260609-x86-step10m-1749 · следующий шаг: launch WINEDEBUG=+seh with x86 10M default
17:51 · verify2 read · 10M cap active but still no PC progress at ntdll32!__wine_dbg_header eip=7bdcd373; no native TEB handler/00418000 regression · следующий шаг: root-cause Wine debug header livelock
17:56 · debug-options fix applied · xtajit mirrors minimal PEB32+0x1000 debug channel table for WINEDEBUG=+seh plus sentinel · следующий шаг: rebuild xtajit and verify
17:58 · debug-options rebuild · xtajit/unixlib.o + xtajit.so rebuilt clean and copied to dist · следующий шаг: verification run after PEB32 debug table fix
17:59 · verify3 setup · disk free 140GB; run dir reports/pe32/run-20260609-debugopts-fix-1759 · следующий шаг: launch WINEDEBUG=+seh
18:03 · unhandled-SEH guard applied · xtajit params now carry guest SEH head/validity; pass_guest_exception refuses redispatch when head is ffffffff/invalid · следующий шаг: rebuild xtajit dll+so
18:07 · PE compiler absent · aarch64-w64-mingw32-clang missing; moved unhandled-SEH guard into unixlib-only status override and reverted PE DLL ABI/source changes · следующий шаг: build xtajit.so target only
18:09 · unhandled-SEH rebuild · xtajit.so target PASS and copied to dist; PE DLL unchanged due missing compiler · следующий шаг: verify no guest exception recursion
18:10 · verify4 setup · disk free 138GB; run dir reports/pe32/run-20260609-sehguard-fix-1810 · следующий шаг: launch WINEDEBUG=+seh
22:07 · resume after sehguard verify · native SEH still routes TEB32 handler 00418000; checking toolchain/process/diff · fix native raise path or unblock PE dll build
22:08 · toolchain probe · found aarch64 clang shim under artifacts/phase-h/toolshim-aarch64-clang · inspect PE raise wrappers and rebuild with PATH shim
22:08 · cpu raise-path inspect · RtlRaiseStatus after no-guest-SEH publishes native status while TEB ExceptionList is guest-mirrored · add scoped native ExceptionList isolation around native raises
22:10 · native raise patch · added xtajit_raise_status for main simulate raises; auditing remaining direct RtlRaiseStatus · patch sibling raise sites then build
22:11 · build xtajit all · PATH includes phase-h aarch64 toolshim · copy rebuilt PE/unix artifacts then run verify
22:14 · rebuild retry · adding llvm@15 bin for ld.lld/clang link · if pass copy xtajit.dll/so to dist and run
22:14 · rebuild retry · adding llvm@15 DYLD_LIBRARY_PATH for clang dylibs · copy artifacts after pass
22:17 · prepare llvm15 fixed toolchain · patched local .tmp LLVM tools to absolute dylib paths · test clang/lld then rebuild xtajit/all
22:17 · rebuild xtajit all · using .tmp/llvm15-fixed/bin for PE link · copy dist artifacts and verify run
22:17 · toolchain wrapper · clang wrapper filters Wine --no-default-config for llvm15 · retry xtajit/all build
22:18 · toolchain wrapper fix · bash-array clang wrapper now strips only --no-default-config · retry xtajit/all build
22:18 · rebuild xtajit all · fixed clang wrapper active · copy artifacts after pass
22:19 · xtajit build pass · PE DLL and unix SO rebuilt with native raise isolation · copy named artifacts to DIST
22:19 · copy xtajit artifacts · updated DIST xtajit.so and aarch64-windows/xtajit.dll · launch verify run WINEDEBUG=+seh
22:20 · launch verify · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260609-native-raise-fix-2220 exe=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/games/half-life/Half-Life/hl.exe WINEDEBUG=+seh callback-route=1 · read run.log immediately via ctx
22:21 · verify result classified · native TEB handler loop gone; new fatal guest eip=00000018 after hbc syscall/NtAllocateVirtualMemory · map ntdll32 addresses and fix return/dispatcher path
22:24 · root cause mapped · i386 RET imm16 C2 returns to 00000018 instead of stack pop target 7bd91f8a; ESP adjustment correct · family-fix RET near C2/C3 in decode/lift/interp/JIT/tests
22:24 · ret family audit · existing decode/interp ret imm16 tests found; app failure points to JIT RET path · inspect codegen and add JIT regression if missing
22:25 · ret JIT audit · interpreter/codegen helper read stack correctly; checking runtime terminal PC handling · patch runtime if it overwrites PC with ret_imm
22:26 · ret JIT root · emit_return_if_helper_failed clobbers X0 return address before RET stores ctx->pc · save X0 to temp reg before failure check and add JIT test
22:30 · add ret JIT regression · test_jit_x86_ret_imm16_uses_guest_return_slot added · run hb_test_runner before fix
22:34 · ret cache probe · running hb_test_runner with persistent translation cache root · if failure, fix cache version/key/invalidation
22:35 · launch ret/bop probe · trace_syscalls=1 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260609-ret-bop-probe-2247 · read stack slots around svc 18
22:39 · continue PE32 ret-imm blocker · inspecting ARM64 codegen specialized RET paths · next root-cause patch
22:44 · PE32 disk-guard before JIT trace probe · checking free space · next launch narrow Half-Life probe
22:45 · PE32 JIT ret trace probe exited · reading run.log via ctx · next classify emitted IR/native path
22:47 · PE32 JIT trace probe wrapper orphaned before exec output · scoped-cleaned prefix _mr-run.1LzK9L · next inspect background launch issue and continue root-cause
22:49 · PE32 JIT trace classified · RET returned to caller; second CALL block likely target/decode issue · next trace 7bd92168 IR
22:51 · PE32 adding low-PC JIT trace · env-gated previous-block dump for pc<0x10000 · next rebuild hyperbridge/xtajit
22:52 · PE32 build after low-PC trace patch · rebuilding libhyperbridge and xtajit.so · next run low-PC probe
22:53 · PE32 low-PC trace build deployed · copied xtajit.so to dist-arm64ec-spike · next run Half-Life low-PC probe
22:53 · PE32 disk-guard before low-PC probe · checking free space · next run with MACRUNNER_HB_TRACE_X86_LOW_PC
22:58 · PE32 low-PC trace evidence · pc=0x18 from ___wine_syscall jmp block; cache influence suspected · next run cache-off
22:59 · PE32 disk-guard before cache-off probe · checking free space · next run without translation cache
23:00 · PE32 cache fix patch · bumped cache version and honored cache=0 · next rebuild/test/run
23:01 · PE32 cache fix built and deployed · hb_test_runner 459/0; xtajit.so copied · next verify app run
23:01 · PE32 disk-guard before cache-v17 verify · checking free space · next Half-Life run
23:07 · PE32 unit regression added · hb_test_runner 460/0; standalone CALL/JMP reg passes · next post-fix cache-off app run
23:08 · PE32 disk-guard before post-fix cache-off · checking free space · next Half-Life run with cache=0
23:10 · PE32 forced xtajit relink · Wine target missed libhyperbridge dependency; rebuilt 23:10 · next rerun cache-off
23:10 · PE32 disk-guard before relinked cache-off · checking free space · next Half-Life run
23:15 · PE32 adding syscall dispatcher slot trace · xtajit cpu.c instrumented · next build xtajit/all
23:16 · PE32 dispatcher slot trace built · xtajit/all copied · next run slot probe
23:16 · PE32 disk-guard before dispatcher slot probe · checking free space · next Half-Life slot trace
23:19 · PE32 guest dispatcher slot trace built · xtajit/all copied · next rerun slot probe
23:20 · PE32 disk-guard before guest-slot probe · checking free space · next Half-Life run
23:21 · PE32 disk-guard before dispatcher write-watch · checking free space · next trace guest writes to 7be048bc
23:24 · PE32 adding branch-helper trace · x86 call/jmp operand target logging · next rebuild/test/relink
23:28 · PE32 resume · continue branch-helper probe from hb_arm64_codegen · build libhyperbridge
23:28 · PE32 build harness · ctx shell zsh status var readonly · rerun with rc variable
23:29 · PE32 branch-helper trace · moved trace_x86_branch_operand forward declaration before call helper · rebuild libhyperbridge
23:30 · PE32 libhyperbridge build · PASS after branch-helper trace declaration fix · build/run hb_test_runner
23:30 · PE32 hb_test_runner · PASS 460/0 · force relink xtajit.so
23:31 · PE32 xtajit relink · xtajit.so copied to dist · disk guard then Half-Life branch-helper run
23:31 · PE32 disk guard · 136Gi free on workspace volume · start Half-Life branch-helper run
23:33 · PE32 Half-Life branch-helper run · root evidence: syscall jmp target OK=0x270000; RET imm16 at 7bdc5e9c emitted via jmp helper target=0x18 · fix HB_IR_RET JIT dispatch/root path
23:34 · PE32 root-cause · embedded hb_ir_instr_t* in cached branch helper becomes stale/reused: JMP[slot] helper sees later RET imm16 instr · fix JIT IR lifetime/copy for helper-backed blocks
23:43 · PE32 JIT IR lifetime fix · implemented block-cache owned IR clone + stale-helper regression · build libhyperbridge/tests
23:44 · PE32 libhyperbridge build · PASS with JIT IR ownership fix · run hb_test_runner
23:44 · PE32 hb_test_runner · PASS 461/0 including stale helper IR ownership regression · force relink xtajit.so and rerun Half-Life
23:45 · PE32 xtajit relink · xtajit.so copied after JIT IR ownership fix · disk guard then real Half-Life run
23:45 · PE32 disk guard · 136Gi free before post-fix run · start Half-Life post-IR-ownership run
23:46 · PE32 Half-Life post-fix run · PASS old blocker: no PC=0x18; dispatcher jmp stays 0x270000; next gate step-limit loop eip=7bdc7bb0 · map 7bdc7bb0 and classify loop
23:49 · PE32 Half-Life verdict · exit=143 timeout; old PC=0x18 fixed; new blocker self-loop step-limit in ntdll32!memcpy at 7bdc7bb0 · inspect self-loop/Jcc JIT path
23:53 · PE32 memcpy-loop probe · added exact JIT x86 ntdll32!memcpy SSE backedge regression · build/run hb_test_runner
23:54 · PE32 memcpy-loop regression · PASS 462/0; exact loop exits in unit test · add targeted stuck-PC regs/stack trace for real app
23:56 · PE32 stuck-PC trace · added MACRUNNER_XTAJIT_TRACE_STUCK_PC regs/stack dumps in xtajit cpu.c · build xtajit/all
23:59 · PE32 resume · xtajit stuck-PC build ready · disk guard/run next
00:00 · HL run start · stuck-PC trace 0x7bdc7bb0 · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-stuck-pc-000013/run.log
00:01 · HL run inspect · stuck at ntdll32 memcpy 0x7bdc7bb0, ebx=00293dd0 target~00290000 · disasm/prologue next
00:02 · HL classify · memcpy scalar tail should exit after ESI 64->68->6c but stays at 64 · add exact unit repro next
00:03 · unit repro start · inspect hb_test_runner around memcpy tests · scalar-tail repro next
00:05 · unit repro build · memcpy scalar-tail test added · running hb_test_runner
00:05 · unit repro retry · zsh status var issue only · rerun hb_test_runner
00:06 · unit repro result · scalar-tail one-shot PASS, app still stuck · inspect hot-loop/step-limit sync
00:08 · unit promoted result · exact self-loop fusion PASS · inspect scalar-scan promotion guard next
00:11 · helper-loop trace build · env-gated PC trace added · run hb_test_runner
00:11 · helper-loop trace rebuild · fixed x64 rflags field · rerun hb_test_runner
00:12 · xtajit relink · helper-loop trace tests PASS · rebuild/copy dist
00:12 · disk guard · helper-loop trace dist copied · run next
00:12 · HL run start · helper-loop watch 0x7bdc7bb0 · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-helper-loop-001256/run.log
00:14 · root evidence · helper first=7bd9213e while pc=7bdc7bb0 => stale promoted block pointer · fix promotion source
00:16 · HL diagnostic done · exit 143 with stale promoted block evidence · build/test root fix
00:17 · xtajit relink · promoted self-loop owned-block fix tests PASS · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-xtajit-promoted-owned-20260610-001728.log
00:18 · disk guard · promoted-owned fix copied · post-fix run next
00:18 · HL run start · promoted self-loop owned-block fix · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-promoted-owned-001850/run.log
00:20 · new gate classify · eip 00418000 region RW no-X · inspect hl.exe PE sections/map lines
00:21 · new root evidence · native SEH calls guest TEB handler 00418000 · inspect xtajit/wow64 route masking
00:24 · xtajit/all build · mask native ExceptionList around wow64 syscalls · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-xtajit-mask-native-seh-20260610-002410.log
00:24 · disk guard · native ExceptionList mask copied · post-fix run next
00:24 · HL run start · native ExceptionList masked during wow64 syscall · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-mask-native-seh-002444/run.log
00:25 · native SEH mask result · TEB handler 00418000 still present · inspect call sites/prepare route
00:28 · wow64 build · mask native guest32 SEH in PrepareForException · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-wow64-mask-guest-seh-20260610-002804.log
00:28 · wow64 aarch64 build · exact target after x86_64 all failure · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-wow64-aarch64-mask-guest-seh-20260610-002840.log
00:30 · disk guard · checking free space before HL/wow64 PrepareForException mask run · next=run
00:31 · HL run start · wow64 PrepareForException guest SEH mask · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-wow64-prepare-mask-003111/run.log
00:32 · classify run · wow64 guard triggers but native SEH still calls guest handler 00418000 · next=make mask persistent
00:36 · build start · xtajit persistent native guest32 SEH mask · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-xtajit-native-seh-mask-003647.log
00:36 · build ok · xtajit copied to dist · next=HL run
00:37 · disk guard · checking free space before xtajit native SEH mask run · next=run
00:37 · HL run start · xtajit persistent native guest32 SEH mask · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-native-seh-mask-003713/run.log
00:37 · classify run · fixed native guest32 SEH walk; next gate EXCEPTION_DATATYPE_MISALIGNMENT at wow64 rva 0x13bd8 · next=disassemble wow64
00:39 · build start · wow64 scalar PE32 syscall arg readers · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-wow64-scalar-args-003947.log
00:39 · build ok · wow64 scalar arg readers copied to dist · next=objdump verify
00:40 · objdump verify · wow64 syscall arg pair-load from x0 gone · next=HL run
00:40 · HL run start · wow64 scalar PE32 syscall arg readers · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-wow64-scalar-args-004056/run.log
00:45 · build start · xtajit scalar guest trace copies · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-xtajit-scalar-trace-copy-004545.log
00:45 · build ok · xtajit scalar trace copies copied to dist · next=HL run
00:45 · disk guard · checking free space before xtajit scalar trace copy run · next=run
00:46 · HL run start · xtajit scalar guest trace copies · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-scalar-trace-copy-004610/run.log
00:47 · build start · xtajit syscall dispatcher trace guest-read guard · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-xtajit-syscall-trace-guard-004732.log
00:47 · build ok · xtajit syscall dispatcher trace guard copied to dist · next=HL run
00:47 · disk guard · checking free space before xtajit syscall trace guard run · next=run
00:47 · HL run start · xtajit syscall dispatcher trace guard · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-syscall-trace-guard-004756/run.log
00:48 · verify run · svc 0x19 now dispatches/returns; process still running · next=poll log
00:49 · classify run · passed svc19 and trace guards; new gate HB JIT unsupported at 7bdc76b0 bytes=66 90 prefix-NOP · next=NOP family fix
00:58 · build start · HyperBridge i386 x87 env/save family · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-hb-x87-env-save-005813.log
00:58 · build ok · HyperBridge x87 env/save family · next=hb_test_runner
00:58 · test start · hb_test_runner x87 env/save family · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/test-20260610-hb-x87-env-save-005839.log
00:58 · test ok · hb_test_runner x87 env/save family · next=relink xtajit
01:01 · build start · relink xtajit against HyperBridge x87 env/save · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-relink-xtajit-x87-env-save-010124.log
01:01 · build ok · xtajit relinked with HyperBridge x87 env/save · next=HL run
01:01 · disk guard · before HL x87 env/save run · free=134G used=69%% next=HL run
01:01 · HL run start · HyperBridge i386 x87 env/save family · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-hb-x87-env-save-010153/run.log
01:03 · analyze · callback misalignment pc=0x104e72bb8 trap=0x184e157b8 · next=map symbols
01:04 · HL run verdict · x87 gate fixed, process started, mr-run exit=143, new callback/misalignment pc=0x104e72bb8 trap=0x184e157b8 · next=symbolize callback route
01:11 · edit plan · add xtajit ARM64 PE call trace for callback/misalignment target ownership · next=patch cpu.c
01:13 · build start · xtajit ARM64EC call trace · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-xtajit-arm64ec-call-trace-011338.log
01:13 · build ok · xtajit ARM64EC call trace deployed · next=HL trace run
01:13 · disk guard · before HL ARM64EC call trace run · free=134G used=69%% next=HL trace run
01:14 · HL run start · xtajit ARM64EC call trace · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-arm64ec-call-trace-011410/run.log
01:15 · HL run finished · xtajit ARM64EC call trace rc=37 · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-arm64ec-call-trace-011410/run.log
01:15 · deploy ok · copied aarch64-windows xtajit.dll with ARM64EC call trace marker · next=HL trace rerun
01:15 · disk guard · before HL correct xtajit PE trace run · free=134G used=69%% next=HL correct trace run
01:15 · HL run start · xtajit ARM64EC call trace PE deployed · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-arm64ec-call-trace-pe-011544/run.log
01:16 · HL run finished · xtajit ARM64EC call trace PE rc=37 · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-xtajit-arm64ec-call-trace-pe-011544/run.log
01:17 · HL run verdict · callback trace sufficient, final gate i386 0F AE /0 FXSAVE at 7bdc76f0, rc=37 · next=0F AE family audit/fix
01:20 · 0F AE family start · real gate FXSAVE 0f ae /0 at 7bdc76f0 · inspect x87 helpers and implement family
01:21 · 0F AE audit · cover FXSAVE/FXRSTOR + LDMXCSR/STMXCSR/CLFLUSH/fence; leave XSAVE-class unsupported · patch decoder/lift/interp/JIT/tests
01:24 · 0F AE patch · decoder/lift/interp/JIT/tests added · build HyperBridge and fix compiler fallout
01:25 · build start · HyperBridge 0F AE family · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-012541-hb-0f-ae-family.log
01:25 · build ok · HyperBridge 0F AE family compiled · run hb_test_runner
01:26 · test ok · hb_test_runner 0F AE family passed · relink xtajit.so
01:26 · relink ok · xtajit.so deployed with 0F AE family · disk guard then HL run
01:26 · HL run start · 0F AE family deployed · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-012643-hb-0f-ae-family
01:28 · HL run verdict · 0F AE fixed; final rc=16 NtRaiseException c0150010 addr=7bd8ff96 svc=0xcf · classify status/caller
01:30 · actctx patch · preserve TEB32 ActivationContextStack across mirror · build xtajit.so
01:30 · build ok · xtajit TEB32 actctx preserve deployed · disk guard then rerun
01:30 · HL rerun start · TEB32 actctx preserve deployed · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-013046-teb32-actctx-preserve
01:33 · HL rerun verdict · 0F AE+c0150010 fixed; rc=143 timeout; final bus fault pc=7ffd07040ac fault=b00000038 svc=3d · classify callback/syscall route
01:38 · callback/syscall classify · final maps to wow64 thunk; need args-upper probe for svc=3d · instrument xtajit dispatch
01:44 · syscall table fix start · patch wow64 syscall_thunks/syscall_args to explicit syscall-id slots · next build wow64.dll
01:45 · syscall table fix patch · wow64 syscall arrays now use explicit [id] initializers and reject NULL syscall slots · next build wow64.dll
01:46 · build wow64 syscall table fix · starting make dlls/wow64/aarch64-windows/wow64.dll · next deploy into dist
01:48 · build wow64 syscall table fix retry · add bundled llvm-mingw bin to PATH · next deploy into dist
01:48 · deploy wow64 syscall table fix · copied aarch64-windows/wow64.dll to dist · next PE32 run
01:49 · PE32 run start · wow64 syscall id table fix, WINEDEBUG=+seh, run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-014934-wow64-syscall-id-table · next ctx-read run.log immediately
01:51 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-014934-wow64-syscall-id-table · next classify log
01:52 · PE32 run classify · process started, no unsupported/c0150010, svc=0x3d still bus pc=0x7ffd07040ac fault=0x300000038 rc=143 · next map fresh wow64.dll handler/disasm
01:58 · syscall dispatch probe patch · trace svc=0x3d thunk pointer in Wow64SystemServiceEx · next rebuild wow64.dll and rerun
01:58 · build wow64 svc3d probe · starting make · next deploy/run
01:58 · deploy wow64 svc3d probe · copied aarch64-windows/wow64.dll to dist · next disk guard/run
01:59 · PE32 run start · wow64 svc=0x3d dispatch probe run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-015900-wow64-svc3d-probe · next ctx-read run.log immediately
02:01 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-015900-wow64-svc3d-probe · next classify probe
02:01 · PE32 run classify · svc3d probe run reached new earlier bus pc=0x104e3fb0c fault=0x300000018 before svc=0x3d, rc=143 · next inspect surrounding log/context
02:06 · xtajit stack-slack guard patch · current_native_stack_slack now probes TEB memory before reading StackLimit · next build xtajit.so/xtajit.dll
02:06 · build xtajit stack-slack guard · starting make xtajit.so + xtajit.dll · next deploy/run
02:06 · deploy xtajit stack-slack guard · copied xtajit.so and xtajit.dll to dist · next disk guard/run
02:07 · PE32 run start · xtajit stack-slack guard + wow64 svc3d probe run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-020715-xtajit-stack-slack-guard · next ctx-read run.log immediately
02:09 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-020715-xtajit-stack-slack-guard · next classify log
02:11 · wow64 NtQueryAttributesFile probe patch · trace attr32/name32/attr64 before native call · next build wow64.dll
02:12 · build wow64 queryattrs probe · starting make wow64.dll · next deploy/run
02:12 · deploy wow64 queryattrs probe · copied wow64.dll to dist · next disk guard/run
02:12 · PE32 run classify · stack-slack guard cleared trace bus; svc=0x3d thunk correct, crash now in NtQueryAttributesFile marshalling · next run with queryattrs attr trace
02:13 · PE32 run start · wow64 NtQueryAttributesFile attr trace run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-021315-wow64-queryattrs-probe · next ctx-read run.log immediately
02:15 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-021315-wow64-queryattrs-probe · next classify queryattrs trace
02:19 · wow64 file redirect guard patch · get_file_redirect validates ObjectName/Buffer readability before path redirection · next build wow64.dll
02:19 · build wow64 file redirect guard · starting make wow64.dll · next deploy/run
02:19 · deploy wow64 file redirect guard · copied wow64.dll to dist · next disk guard/run
02:20 · PE32 run start · wow64 file redirect guard run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-022024-wow64-file-redirect-guard · next ctx-read run.log immediately
02:22 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-022024-wow64-file-redirect-guard · next classify file redirect guard
02:23 · wow64 guest32-base cache patch · guest32_host_ptr uses cached high base before unsafe TEB TLS deref · next build wow64.dll
02:23 · build wow64 guest32-base cache · starting make wow64.dll · next deploy/run
02:24 · deploy wow64 guest32-base cache · copied wow64.dll to dist · next disk guard/run
02:24 · PE32 run start · wow64 guest32-base cache run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-022430-wow64-guest32-base-cache · next ctx-read run.log immediately
02:26 · PE32 run finished · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-022430-wow64-guest32-base-cache · next classify guest32-base cache
02:28 · resume PE32 svc=0x3d · previous run exited code 143 after bus at NtQueryAttributesFile entry path · add staged wow64 qattr trace
02:31 · instrument wow64 NtQueryAttributesFile · added entry/after-attr/after-info/after-name/conversion trace with guarded field reads · build wow64
02:31 · disk guard · 136Gi free on workspace volume · build wow64 trace patch
02:32 · build wrapper retry · zsh read-only variable status before make · rerun with rc
02:33 · build wow64 · qattr staged trace build/deploy OK · run PE32 gate
02:33 · run qattr staged trace · started reports/pe32/run-20260610-023336-qattr-staged-trace · ctx-read run.log immediately
02:34 · ctx read run qattr trace · early log: failedOpen=0 unsupported=0 c0150010=0 svc3d not reached yet · poll run then ctx-read again
02:35 · ctx classify qattr run · svc=0x3d reaches wrapper; attr32/info/name32 readable; bus before attr64 trace => objattr_32to64_redirect · inspect object attr conversion
02:41 · fix file redirect env query · cache WINEWOW6432BPREFIXMODE in init_file_redirects; get_file_redirect no longer calls RtlQueryEnvironmentVariable_U in PE32 callback · build wow64
02:42 · build wow64 env-cache · build/deploy OK · run PE32 gate
02:42 · run file redirect env-cache · started reports/pe32/run-20260610-024228-file-redirect-env-cache · ctx-read run.log immediately
02:43 · ctx read env-cache run · early log clean; svc3d not reached yet · poll run then ctx-read
02:44 · ctx read env-cache run · svc3d gate moved; new callback reject around svc=0x50 pc=0x7ffd0463450 fault=0x4077b0004 plus nested bus/trap · poll completion and classify terminal gate
02:45 · final classify env-cache run · exit=143; old svc3d/qattr bus gone; terminal callback reject pc=0x7ffd0463450 fault=0x4077b0004 then bus=0x104f22dc8 trap=0x184e157b8 · inspect xtajit callback routing
02:49 · instrument pending-cross callback path · added before/after publish-stackbase-prepare-process probes and publish_status guard · build xtajit
02:49 · disk guard · 136Gi free before xtajit callback probe run · build xtajit
02:50 · build xtajit pending-cross probe · build/deploy OK · run PE32 gate
02:50 · run pending-cross probe · started reports/pe32/run-20260610-025018-pending-cross-probe · ctx-read run.log immediately
02:51 · ctx read pending-cross probe · early: pending-cross stages complete for svc=0x50; no terminal yet · poll completion and classify
02:52 · ctx read pending-cross probe · pending-cross passes; reached svc3d/qattr; bus after after-name at pc=0x7ffd053f214 fault=0x300000030 · poll whether terminal
02:55 · fix wow64 temp heap · cache process heap in process_init; Wow64AllocateTemp/free_temp_data no longer call GetProcessHeap under PE32 callback x18 · build wow64
02:56 · build wow64 temp heap cache · build/deploy OK · run PE32 gate
02:56 · run wow64 temp heap cache · started reports/pe32/run-20260610-025632-wow64-temp-heap-cache · ctx-read run.log immediately
02:57 · ctx read temp heap run · early clean; qattr not reached yet · poll run then ctx-read
02:58 · ctx read temp heap run · qattr passes final trace; new gate NtQuerySystemInformation class=0 put-before bus pc=0x104e57b0c fault=0x300000018 · poll terminal and symbolicate
03:01 · continue · gate NtQuerySystemInformation class=0 crash after basic put-before · localize system.c/xtajit PC
03:02 · analyze · NtQuerySystemInformation crash isolated to put_system_basic_information write window · inspect marshal/write guards
03:02 · evidence · fault address 0x300000018 inside basic put window points to __TRY/x18 TEB guard · inspect prepare_write before edit
03:03 · edit-plan · remove unsafe __TRY around SystemBasicInformation32 marshal and guard sibling write · next build wow64
03:04 · build-start · wow64 SystemBasicInformation32 no-SEH marshal fix · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-030429-wow64-basic-info-no-seh.log
03:04 · build-ok · wow64 deployed after SystemBasicInformation32 fix · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-030429-wow64-basic-info-no-seh.log
03:05 · run-start · Diablo after SystemBasicInformation32 no-SEH marshal fix · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-030511-basic-info-no-seh
03:07 · run-read · old class=0 gate cleared; new NtQuerySystemInformation class=102 x18 fault at pc=0x104de3b0c · classify class/branch
03:07 · run-exit · Diablo SystemBasicInformation32 no-SEH run exit=143 · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-030511-basic-info-no-seh
03:07 · classify · class=102 returned; crash moved to svc=0xb0 pending-cross after-stackbase→after-prepare · inspect xtajit prepare path
03:09 · run-exit · run-20260610-030511 exit=143; class=0 cleared; terminal prepare_arm64_pe_call NtCurrentTeb with guest x18 fault=0x300000018 · find native TEB source
03:09 · evidence · symbolicate pc=0x104de3b0c and inspect x18 helpers before xtajit callback-prepare fix
03:10 · fix-plan · cache native TEB at BTCpuSimulate_impl entry; pass to prepare_arm64_pe_call instead of NtCurrentTeb after guest x18 · edit xtajit/cpu.c
03:12 · edit · xtajit caches native TEB at BTCpuSimulate_impl entry and passes it to ARM64 PE prepare · validate refs
03:14 · build-start · xtajit native TEB prepare/callback fix · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-031409-xtajit-native-teb-prepare.log
03:14 · build-ok · xtajit deployed native TEB prepare/callback fix · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-031409-xtajit-native-teb-prepare.log
03:14 · run-start · Diablo after xtajit native TEB prepare fix · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-031431-xtajit-native-teb-prepare
03:15 · run-read · xtajit native TEB fix passes after-prepare; run alive in repeated svc=0x50 pending-cross loop · wait for real exit/verdict
03:16 · run-exit · Diablo xtajit native TEB prepare run exit=143 · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-031431-xtajit-native-teb-prepare
03:17 · run-read · native TEB prepare fixed; new terminal after active-current-syscall-switch svc=0x1d pc=0x104dcbb0c fault=0x300000018 · symbolicate trampoline
03:18 · symbolicate · pc=0x104dcbb0c -> is_direct_user_xmm_mem_operand+72 (xtajit.so base 0x104d80000) · inspect HB codegen operand path
03:19 · root · direct-mem predicate only rejects addr32; i386 operands can have addr32=false, so x64 direct-mem fast path enters HB_ARCH_X86 · patch codegen guard
03:21 · continue · gate=HB direct-mem fast-path i386 crash in is_direct_user_xmm_mem_operand · patch direct-mem family gate
03:31 · patch · hb_arm64_codegen direct-mem fast-path gated off for HB_ARCH_X86 across scalar/XMM/block/RMW family · build next
03:32 · build-start · xtajit/hyperbridge direct-mem x86 gate · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-033210-hb-direct-mem-x86-gate.log
03:32 · build-end · xtajit/hyperbridge direct-mem x86 gate · rc=0 log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-033210-hb-direct-mem-x86-gate.log
03:32 · run-start · Diablo/Half-Life PE32 after HB direct-mem x86 gate · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-033236-hb-direct-mem-x86-gate
03:34 · run-end · Diablo/Half-Life PE32 after HB direct-mem x86 gate · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-033236-hb-direct-mem-x86-gate
03:35 · run-result · hb direct-mem x86 gate cleared old is_direct_user_xmm fault; new terminal pc=0x104eafb0c fault=0x300000018 at NtQuerySystemInformation basic put-before · symbolize
03:40 · patch · xtajit arm64 shims save/restore x18 around ARM64EC calls · build next
03:41 · build-start · xtajit x18 restore + HB direct-mem gate · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-034108-xtajit-x18-restore.log
03:41 · build-end · xtajit x18 restore + HB direct-mem gate · rc=0 log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-034108-xtajit-x18-restore.log
03:41 · run-start · Diablo/Half-Life PE32 after xtajit x18 restore · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-034128-xtajit-x18-restore
03:42 · run-result · x18 restore did not clear NtQuerySystemInformation basic put-before fault pc=0x104dd7b0c fault=0x300000018 · symbolize next
03:43 · run-end · Diablo/Half-Life PE32 after xtajit x18 restore · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-034128-xtajit-x18-restore
03:47 · patch · hb_jit_buffer magic validation to avoid guest-base deref and expose invalid caller · build next
03:47 · build-start · hb jit-buffer guard · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-034759-hb-jit-buffer-guard.log
03:48 · build-end · hb jit-buffer guard · rc=0 log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-034759-hb-jit-buffer-guard.log
03:48 · run-start · Diablo/Half-Life PE32 after hb jit-buffer guard · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-034821-hb-jit-buffer-guard
03:49 · run-result · hb jit-buffer guard did not intercept; terminal pc=0x104e23b0c fault=0x300000018 · symbolize
03:50 · run-end · Diablo/Half-Life PE32 after hb jit-buffer guard · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-034821-hb-jit-buffer-guard
03:51 · cross-lane · terminal in ntdll macrunner_hb_pc_is_x64_guest_code_no_lock during PE32 NtQuerySystemInformation basic write; PE32 will probe field write, ntdll core left untouched · next=wow64 staged field trace
03:52 · patch · wow64 NtQuerySystemInformation basic field-stage trace · build next
03:52 · build-start · wow64 basic field trace · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-035228-wow64-basic-field-trace.log
03:52 · build-end · wow64 basic field trace · rc=2 log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-035228-wow64-basic-field-trace.log
03:53 · build-start · wow64 basic field trace correct target · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-035317-wow64-basic-field-trace.log
03:53 · build-end · wow64 basic field trace correct target · rc=0 log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-035317-wow64-basic-field-trace.log
03:53 · run-start · Diablo/Half-Life PE32 wow64 basic field trace · run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-035339-wow64-basic-field-trace
03:55 · run-end · Diablo/Half-Life PE32 wow64 basic field trace · rc=143 run=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-035339-wow64-basic-field-trace
03:56 · field-trace read · NtQuerySystemInformation class=0 faults on first basic field unknown ptr=0x37BC65244 pc=ntdll-route fault=0x300000018 · inspect wow64 basic writer
03:58 · fix wow64 basic output · SystemBasicInformation now writes local struct via write_guest32_output incl retlen · rebuild wow64
04:00 · build start · wow64 basic write helper · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040001-wow64-basic-write-helper.log
04:00 · build finish · wow64 basic write helper rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040001-wow64-basic-write-helper.log
04:00 · run start · wow64 basic write helper · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040034-wow64-basic-write-helper
04:02 · run invalid · wow64 basic write helper produced only launcher prefix line, no wine-process-primary/exit · inspect launcher then rerun managed
04:02 · cleanup · removed invalid managed prefix /Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/_mr-run.ydQkZ1 · rebuild same wow64 source before rerun
04:03 · build start · wow64 basic write helper rerun guard · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040305-wow64-basic-write-helper-rerun.log
04:03 · build finish · wow64 basic write helper rerun guard rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040305-wow64-basic-write-helper-rerun.log
04:03 · run start · wow64 basic write helper managed · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040324-wow64-basic-write-helper-managed
04:04 · run finding · basic direct store cleared but write_guest32_output fallback faults at memcpy after protect status=0 old=PAGE_WRITECOPY · inspect syscall-copy alternatives
04:05 · run process exit · wow64 basic write helper managed rc=143 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040324-wow64-basic-write-helper-managed
04:05 · run stop · scoped wineserver-k for terminal run prefix=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/_mr-run.gjacpC · session drain next
04:06 · fix wow64 writer · ARM64+i386 write_guest32_output uses NtWriteVirtualMemory instead of unsafe memcpy fallback · rebuild wow64
04:06 · build start · wow64 guest32 ntwrite · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040621-wow64-guest32-ntwrite.log
04:06 · build finish · wow64 guest32 ntwrite rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040621-wow64-guest32-ntwrite.log
04:06 · run start · wow64 guest32 ntwrite · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040634-wow64-guest32-ntwrite
04:07 · run process exit · wow64 guest32 ntwrite rc=66 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040634-wow64-guest32-ntwrite
04:08 · run result · class=0 passed; next blocker MemoryWineUnixFuncs writer c0000022 on module slot 0x37B75B1E8 size=8 · fix protected NtWrite chunks
04:09 · fix wow64 writer · ACCESS_DENIED now falls through to protect+NtWrite chunks, no PE32 memcpy · rebuild wow64
04:09 · build start · wow64 guest32 protected ntwrite · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040913-wow64-guest32-protected-ntwrite.log
04:09 · build finish · wow64 guest32 protected ntwrite rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-040913-wow64-guest32-protected-ntwrite.log
04:09 · run start · wow64 guest32 protected ntwrite · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040926-wow64-guest32-protected-ntwrite
04:11 · run result · MemoryWineUnixFuncs protected writer passed; next terminal svc=0x23 with teb32=0x3001f002b before-publish then ntdll route bus fault=0x300000018 · inspect xtajit teb32 source
04:11 · run stop · scoped wineserver-k for protected ntwrite terminal prefix=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/_mr-run.oUQq1L · session drain next
04:11 · run process exit · wow64 guest32 protected ntwrite rc=1 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-040926-wow64-guest32-protected-ntwrite
04:12 · fix xtajit teb32 · prefer TEB itself when TLS guest32 base shares host window, avoid WowTebOffset=0x2b drift · rebuild xtajit
04:12 · build start · xtajit teb32 stable · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041237-xtajit-teb32-stable.log
04:12 · build finish · xtajit teb32 stable rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041237-xtajit-teb32-stable.log
04:12 · run start · xtajit teb32 stable · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041251-xtajit-teb32-stable
04:13 · run process exit · xtajit teb32 stable rc=37 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041251-xtajit-teb32-stable
04:13 · revert xtajit teb32 guess · run rc=37 proved real teb32 is teb64+0x2000; next add WowTebOffset drift probe · build after probe
04:15 · fix xtajit pending-teb32 · use ctx->teb32_host for pending stackbase, log WowTebOffset drift separately · rebuild xtajit
04:15 · build start · xtajit pending teb32 host · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041540-xtajit-pending-teb32-host.log
04:15 · build finish · xtajit pending teb32 host rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041540-xtajit-pending-teb32-host.log
04:15 · run start · xtajit pending teb32 host · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041552-xtajit-pending-teb32-host
04:17 · run stop · scoped wineserver-k for pending teb32 probe prefix=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/artifacts/_mr-run.ZoZXuk · session drain next
04:17 · run process exit · xtajit pending teb32 host rc=1 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041552-xtajit-pending-teb32-host
04:17 · fix xtajit teb32 cache · cache sane aligned WowTebOffset TEB32 pointer in TLS slot 13, reuse when offset drifts to 0x2b · rebuild xtajit
04:17 · build start · xtajit teb32 cache · log=/Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041750-xtajit-teb32-cache.log
04:17 · build finish · xtajit teb32 cache rc=0 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/build-20260610-041750-xtajit-teb32-cache.log
04:18 · run start · xtajit teb32 cache · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041803-xtajit-teb32-cache
04:19 · run process exit · xtajit teb32 cache rc=5 · /Users/timurtoby/Documents/MacRunner/Main/MacRunner/reports/pe32/run-20260610-041803-xtajit-teb32-cache

## 2026-06-10 06:30 — Opus coordinator resume (post-Codex burnout)
- **DIABLO EXE (Phase-2 window/DirectDraw validation target — DO NOT LOSE):** `/Users/timurtoby/Documents/MacRunner/Main/Diablo.Hellfire-Rutracker/extracted/Diablo.exe` (full game: DIABDAT.MPQ, Storm.dll; hellfire/hellfire.exe nearby). i386 root-cause iteration on Half-Life hl.exe (same root); window validation → Diablo.exe.
- **RABBIT-HOLE called:** prev session did ~12 runs 02:00-04:18 permuting wow64 NtQuerySystemInformation/qattr field-write + teb32 caching; ended driving into Lane A forbidden fault. NET: it DID advance the blocker (NtQuerySystemInformation → first win32u syscall). Triage "WINDOW_SERVER_ERROR" is NOISE (status=40000003=STATUS_IMAGE_NOT_AT_BASE benign reloc, mislabeled by classify_run — TRIAGE-NEEDS).
- **REAL FRONTIER (run-20260610-041803, hl.exe):** i386 reaches FIRST win32u (NtUser/NtGdi) syscall **svc=0x147a** (≥0x1000 win32u table) from 32-bit win32u.dll @0x7ab80000 → native handler faults c0000005 deref **0x201094** = raw 32-bit guest ptr (~0x201000, x8) NOT rebased to host (guest32_base=0x3_00000000 → expected 0x300201094). x18(host TEB)=0 at fault; native pc=0x10774fe20 (NOT xtajit.so@0x104d80000 — different .so). macrunner_hb x64-callback router correctly REJECTS i386 fault → unhandled c0000005 → exit=5.
- Read confirms: signal_arm64.c:895 route_x64_callback_fault rejection is CORRECT (not Lane A bug). xtajit/cpu.c owns arm64ec-call/win32u switch + guest32_host_ptr rebasing (mine). Next: identify pc=0x10774fe20 module via fresh run + read wow64 win32u dispatch / wow64win thunks.

## 2026-06-10 07:15 — root-cause refined; cross-lane boundary + repro blocker
- **win32u svc=0x147a = `NtUserInitializeClientPfnArrays`** (win32syscalls.h:1150, 16-byte args=4 ptrs) — user32's first win32u call at process-attach. Native impl (win32u/class.c:275) stores the WNDPROCs then runs **`init_user()`** (class.c:259): NtQuerySystemInformation → init_startup_info → gdi_init → shared_session_init → sysparams_init → winstation_init → register_desktop_class. **The fault is inside one of these native win32u init steps dereferencing an unrebased guest ptr ~0x201000+0x94.**
- **RULED OUT (my scope is clean):** wow64/wow64win thunk infra rebases correctly (`guest32_host_ptr` patched in both wow64_private.h:90 & wow64win_private.h:52; win32u table wired via sdwhwin32). `ctx->guest32_base` IS set per-thread (hb_wow64cpu.c:547 via xtajit thread_init). JIT direct-mem & direct-stack paths are correctly arch-gated OFF for HB_ARCH_X86 (codegen.c:371 `direct_mem_codegen_arch_enabled`; direct_stack env=0). So this is **NOT** a JIT-rebasing or thunk bug.
- **CROSS-LANE (root in forbidden files):** (a) native win32u `init_user` derefs a guest ptr → **Lane D** (win32u); (b) **x18(host TEB)=0** at the fault ⇒ host-TEB lost *inside* the native win32u call (NtCurrentTeb mis-resolves) — the asm thunk sets x18=teb at blr (cpu.c:68) but it's lost during a native syscall/callback in init_user → **xtajit↔Lane-A boundary** (native `__wine_syscall_dispatcher` x18 restore for bridge-context syscalls, OR a callback path missing `prepare_arm64_pe_call`).
- **REPRO BLOCKER:** cold-prefix runs (timeout 25-120s) drown in a **bootstrap `c0000026` SEH unwind-metadata recursion** in kernelbase.dll@0x87FFF7A0000 (wineboot/services, pid 002c — `virtual_unwind exception data not found`) and never reach hl.exe win32u. Run 041803 (the reference) got past it (warm cache/nondeterministic). +seh on the recursion = log runaway (capped two runs at 200-300MB, cleaned). **Need a warm-prefix/bottle template (MACRUNNER_MR_RUN_PREFIX_TEMPLATE=bottles/generic-x86) to skip wineboot and reach win32u deterministically for live iteration.**
- NEXT: warm-prefix repro on Diablo.exe to nail the exact init_user callee + the x18-loss point; if x18-loss is in a callback path inside xtajit → fix there (mine); else NEEDS to Lane A/D (recorded).

## 2026-06-10 07:30 — BLOCKED: Lane-A ntdll.so rebuild (17:09) regressed i386 SEH → c0000026 recursion before win32u
- **Cannot reproduce/iterate the win32u fault.** Every fresh run now (cold OR warm prefix) dies in an **unbounded c0000026 SEH unwind-metadata-missing recursion** (`virtual_unwind exception data not found`, 35k+ c0000026 / 173k dispatch_exception in 30s) and **never reaches win32u** (svc=0x147a / InitializeClientPfn = 0 occurrences; c0000005 = 0). 041803 (04:19) had c0000026 *bounded/handled* (~192×) and DID reach win32u.
- **Root = shared-dist regression by Lane A:** `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-unix/ntdll.so` mtime = **06-10 17:09** (rebuilt mid-session, after 041803). Sources: `unix/macrunner_hb.c` mod 14:50 (uncommitted ` M`), `unix/signal_arm64.c` mod 13:20. My `xtajit.so`/`.dll` unchanged (04:17). So Lane A's in-progress SEH/unwind edits to ntdll.so broke the i386 c0000026 path (bounded→unbounded). This is the shared-dist coordination hazard (handoff rule #8).
- **PE32 is gated on Lane A** for BOTH: (1) the immediate c0000026 dist regression (need it bounded again to even reach win32u), and (2) the x18-host-TEB-loss root at the win32u syscall. Both in forbidden files (signal_arm64.c/macrunner_hb.c). Recorded in PE32-NEEDS.
- xtajit/wow64/wow64win/hb-i386 scope verified CLEAN this session (no in-scope fix to make on the win32u path). Holding for Lane A / operator coordination; do NOT clobber Lane A's 17:09 ntdll.so on the shared dist.
08:07 · start · found pe32 worktree · next: sync pe32 with main branch
 · PE32 progress · перехожу на точные файлы в reports: найдено 2 существующих pe32 run.log и теперь иду на A/B прогоны с логированием только по grep -m5
