# ARM64X / ARM64EC hybrid PE: изменяемая `.data` между двумя видами кода и инициализация загрузчиком

**Дата:** 15 июня 2026. **Формат:** ресёрч-отчёт с цитированием первоисточников.
**Легенда достоверности:** `[ФАКТ]` — прямо подтверждено первоисточником (цитата); `[ВЫВОД]` — обоснованная инференция из подтверждённых фактов; `[?]` — не подтверждено / гипотеза.

> **TL;DR по твоему багу.** Твоё наблюдение «две копии `sort` на 0x2820 врозь» — это и есть штатная раскладка Arm64X: native-объекты и EC-объекты линкуются в один образ в **разных символьных пространствах**, поэтому один и тот же логический глобал получает **две физических копии** по двум RVA. Настоящий загрузчик в **EC/x64-процессе** запускает **EC entry point** (через swap `AddressOfEntryPoint`↔`AlternateEntryPoint`, выполняемый ARM64X-релокациями), и именно EC-`DllMain`→EC-`init_locale` инициализирует **EC-копию** `sort` (та, что читают EC-экспорты вроде `GetStringTypeW`). Твой кастомный загрузчик запустил **native ARM64 `DllMain`**, который проинициализировал **native-копию** (0x186548), а EC-копию (0x183d28) оставил нулём. Фикс — гнать EC-entry, а не native-entry (детали в разделе «Выводы для фикса»).

---

## 1. Раскладка изменяемой module-private `.data`: общая или дублируется?

`[ФАКТ]` Arm64X — это один PE-файл, в который слинкованы **и** ARM64-, **и** ARM64EC-объекты/библиотеки: *«an Arm64X binary contains all of the content that would be in separate x64/Arm64EC and Arm64 binaries, but merges them into one… The built Arm64X binary has two sets of code, entry points, and other elements, while eliminating redundant parts»* (MS Learn, Arm64X PE files). Microsoft-разработчик прямо: *«ARM64X is the resulting binary from linking ARM64 and ARM64EC objs and libs into one»* (цит. в FFRI Project Chameleon).

`[ФАКТ]` На уровне линкера (LLD) native- и EC-вход живут в **раздельных символьных пространствах и не могут ссылаться друг на друга**: *«On hybrid ARM64X targets, ARM64 and ARM64EC input files operate in separate namespaces and cannot reference each other. This change introduces separate `SymbolTable` instances…»* (LLVM PR #119294). При записи образа чанки обоих пространств просто сериализуются в один файл.

`[ФАКТ]` Дублирование **избирательное**: то, что можно слить, сливается в одну копию, а различающееся раздваивается. Для IAT: *«the PE header references a single IAT for both native and EC views, merging entries where possible. When merging isn't feasible, different imports are grouped together, and ARM64X relocations are emitted as needed»* (LLVM PR #124189). ABI добавляет фундамент для шаринга: *«x64 and Arm64EC share the same symbol namespace»* и *«ARM64EC follows x86-64 data alignment rules. This makes structures binary-compatible»* (MS Learn, Arm64EC ABI; Old New Thing 2022-08-30).

**Когда один логический глобал оказывается на двух RVA.** `[ФАКТ + твоя эмпирика]` Когда **один и тот же исходник компилируется дважды** (как ARM64 и как ARM64EC) и оба объекта попадают в образ, каждый глобал, определённый в обоих, становится **двумя разными символами в двух пространствах** → **две физических копии по двум RVA**, и код каждого вида адресует копию своего пространства. Это ровно случай `kernelbase`: и `GetStringTypeW`/`init_locale`, и `sort` скомпилированы и как native, и как EC. Прямой задокументированный аналог — **две Export Address Table в одном образе**: *«CHPEV2 ARM64X has two EATs for the x64 and Arm64 processes… switched depending on whether or not this relocation entry is applied»* и две физических функции `MessageBoxA` (native) и `#MessageBoxA` (x64) в одном `user32.dll` (FFRI Project Chameleon). Твои `sort@0x183d28` (EC) и `sort@0x186548` (native), разнесённые на 0x2820, — тот же феномен для `.data`.

`[ВЫВОД]` Итого: обычный скаляр, общий для обоих видов, как правило живёт в **одной** копии (шаринг по умолчанию). Но глобал, который **определён в обеих** компиляциях (как `sort` в `kernelbase`), материализуется в **две копии**, и каждый вид кода читает/пишет свою. Это не баг линкера, а штатная модель. MS Learn явно перечисляет в дублируемом только «code, entry points, and other elements» и молчит про обычные `.data`-глобалы — поэтому вывод про «двойные пользовательские глобалы» опирается на модель namespace’ов LLD + твою прямую эмпирику, а не на дословную фразу MS.

---

## 2. ARM64X dynamic relocation table (`IMAGE_DYNAMIC_RELOCATION_ARM64X` / DVRT): что релоцирует и примиряет ли данные?

`[ФАКТ]` DVRT-запись типа `IMAGE_DYNAMIC_RELOCATION_ARM64X` (DVRT type 6, машина `IMAGE_FILE_MACHINE_ARM64X = 0xA64E`) — это **load-time патч**, на реальной Windows применяемый ядром через `nt!MiApplyConditionalFixups`: *«various information such as architecture information and an offset of Export Address Table (EAT) are overwritten at runtime»* (FFRI). Три типа фиксапов (значения enum из winnt.h/LLVM `COFF.h`):

- `IMAGE_DVRT_ARM64X_FIXUP_TYPE_ZEROFILL = 0` — обнулить 2ˣ байт;
- `IMAGE_DVRT_ARM64X_FIXUP_TYPE_VALUE = 1` — перезаписать 2ˣ байт литералом;
- `IMAGE_DVRT_ARM64X_FIXUP_TYPE_DELTA = 2` — прибавить/вычесть 16-битное значение × (4|8).

`[ФАКТ]` Подтверждено независимой реализацией в Wine (`dlls/ntdll/unix/virtual.c`, `apply_arm64x_relocations`): `ZEROFILL`→`memset(...,0,1<<arg)`, `VALUE`→`memcpy(page+offset, rel, 1<<arg)`, `DELTA`→`*(int*)(page+offset) += ±val*(4|8)`.

`[ФАКТ]` **Что именно патчится:** поле `Machine` в `IMAGE_NT_HEADERS64` (`0xAA64`→`0x8664`), `VirtualAddress` директории экспорта (переключение на вторую EAT), entry point, load config, exception table, записи IAT — и в принципе **произвольные байты** (код в `.text` и указатели в `.data`): *«the DVRT ARM64X relocation enables an arbitrary write in the target module»* (FFRI, Relock 3.0). Т.е. DVRT релоцирует **и заголовки, и IAT/EAT, и код, и указатели в данных** — это общий механизм «перезаписать данный RVA данным значением».

`[ФАКТ]` **Применяется только для EC/x64-загрузки.** *«DVRT ARM64X relocation is only applied when ARM64X is executed as an ARM64EC or x64 process. The relocation is not applied if it is run as an ARM64 process»* (FFRI). На диске по умолчанию лежит **native ARM64-вид** (MS Learn: *«By default, Arm64X binaries appear to be Arm64 binaries»*). В Wine триггер дословно: главный образ AMD64 **И** машина DLL = ARM64 → `update_arm64x_mapping()`.

**Примиряет ли DVRT data-ссылки между видами?** `[ФАКТ/ВЫВОД]` **Нет, не алиасит две копии в одно хранилище.** DVRT — односторонний **view-selecting** патч: он перезаписывает выбранные RVA так, чтобы образ *стал* EC-видом, и может **перенаправить указатель** на «другую» копию таблицы (как с EAT), но не создаёт общего backing storage для логического глобала, существующего в двух копиях. Ни один первоисточник не описывает DVRT как aliasing хранилища; везде это in-place overwrite. **Важно для твоего бага:** DVRT *не* возьмёт и не «сольёт» EC-копию `sort` с native-копией — он лишь обеспечивает, что EC-код адресует EC-копию. Наполнение этой копии — задача EC-инициализатора, а не релокаций.

---

## 3. Как настоящий загрузчик (ntdll) инициализирует hybrid ARM64X DLL: один `DllMain` или два?

`[ФАКТ]` У Arm64X **физически два entry point**: основной `AddressOfEntryPoint` в optional header и `AlternateEntryPoint` в CHPE-метаданных (`IMAGE_ARM64EC_METADATA`, на которые указывает `CHPEMetadataPointer` из load config). Поддержка в LLD: PR #123346 *«Add support for alternate entry point in CHPE metadata on ARM64X»*, с дословным комментарием в дифе: *«For the hybrid image, set the alternate entry point to the EC entry point. In the hybrid view, it is swapped to the native entry point using ARM64X relocations.»*

`[ФАКТ]` **Эти два entry мирятся swap’ом через ARM64X-релокации.** Тест линкера `arm64x-entry.test` (тот же PR) показывает зеркальную перестановку для двух видов:
- базовый (EC/x64) вид: `AddressOfEntryPoint = 0x1000`, `AlternateEntryPoint = 0x2000`;
- hybrid (native ARM64) вид: `AddressOfEntryPoint = 0x3000`, `AlternateEntryPoint = 0x1000`.

То есть в EC-виде «живой» `AddressOfEntryPoint` — это **EC entry**, а в native-виде релокации делают «живым» **native entry**.

`[ФАКТ]` **Загрузчик вызывает РОВНО ОДИН entry** — тот, что в `AddressOfEntryPoint` *после* применения релокаций. В Wine: `wm->ldr.EntryPoint = base + nt->OptionalHeader.AddressOfEntryPoint;` и единственный вызов `call_dll_entry_point(entry, module, reason, ...)`. Отдельного «прогона обоих entry» в Wine нет.

`[ВЫВОД, высокая уверенность]` Значит, **настоящий загрузчик не гоняет оба `DllMain`. Он фиксирует один вид на процесс, запускает единственный `DllMain` активного вида, и тот инициализирует копию данных СВОЕГО вида.** В EC/x64-процессе это EC-`DllMain` → инициализируется **EC-копия** глобалов. Native-копия в EC-процессе попросту мертва (Hybrid Code Map: *«If the process is ARM64EC, the area marked as x64 and ARM64EC… is executed, and the area marked as ARM64 is not used»*, FFRI). Дословной фразы MS «запускается только один DllMain» не существует публично — это инференция из entry-swap теста + поведения загрузчика.

`[ВЫВОД]` Поэтому вопрос «как делается когерентным process-global состояние (locale/NLS, CRT) между видами» в значительной мере **снимается**: при одном живом виде на процесс существует только один набор глобалов и один CRT, инициализируемый одним `DllMain`. Примирять нечего. Первоисточника, описывающего «общий NLS/CRT между двумя видами» или прогон обоих entry, я не нашёл — модель «один вид на процесс» лучше подтверждена.

---

## 4. Конкретно locale/NLS (`GetStringTypeW`, sort-таблицы) в kernelbase

`[ФАКТ]` (Wine `dlls/kernelbase/locale.c`.) Глобал — единственная file-static, нуль-инициализированная структура `sort` с полями `keys`, `casemap`, `ctypes`, **`ctype_idx`**, `guids`, … . `load_sortdefault_nls()` берёт секцию `sortdefault.nls` (`NtGetNlsSectionPtr(9,…)`) и нарезает указатели: `sort.ctypes = ctype + 2; sort.ctype_idx = (BYTE*)ctype + ctype[1] + 2; …`. Точка входа — `init_locale(HMODULE)`, которая вызывает `load_locale_nls()` и `load_sortdefault_nls()`; вызывается из `DLL_PROCESS_ATTACH` kernelbase.

`[ФАКТ]` `GetStringTypeW` читает таблицу через inline `get_char_type()` — двухуровневый trie по `sort.ctype_idx`, затем выборка из `sort.ctypes`:
```c
const BYTE *ptr = sort.ctype_idx + ((const WORD*)sort.ctype_idx)[ch >> 8];
ptr = sort.ctype_idx + ((const WORD*)ptr)[(ch >> 4) & 0xf] + (ch & 0xf);
return sort.ctypes[*ptr * 3 + type / 2];
```
Если `sort.ctype_idx == NULL` (копия не инициализирована) — это NULL-дереференс ровно с твоим симптомом.

`[ВЫВОД — суть твоего бага]` В hybrid-`kernelbase` существуют **две** структуры `sort` (EC-копия @0x183d28, native-копия @0x186548). `init_locale`/`load_sortdefault_nls` пишут в **ту `sort`, которую разрешает запущенный вид кода**. EC-экспорт `GetStringTypeW` через `get_char_type()` читает **EC-копию**. Если выполнился только native-`init_locale` (твой случай), он заполнил native-копию, а EC-копия осталась нулём → `sort.ctype_idx == NULL` → падение. На настоящей Windows в EC-процессе выполняется **EC**-`init_locale`, заполняющий именно ту копию, которую читают EC-экспорты — и всё когерентно. (Я не нашёл публичного Wine-баг-репорта именно про «`sort.ctype_idx` пуст в одном из видов ARM64X» — механизм подтверждён, конкретный отчёт-дефект — нет; `[?]`.)

---

## 5. Канонический механизм: оба entry? DVRT-алиасинг? `__os_arm64x_*` dispatch для данных?

- **Оба entry?** `[ВЫВОД]` Нет. Один вид на процесс, один `DllMain` активного вида (раздел 3).
- **DVRT алиасит две копии в одну?** `[ФАКТ/ВЫВОД]` Нет (раздел 2). DVRT — view-selecting overwrite; он направляет EC-код на EC-копию, но не наполняет её и не сливает с native.
- **`__os_arm64x_*` перенаправляет доступ к данным?** `[ФАКТ]` Нет — это про **вызовы**, не про данные. `__os_arm64x_dispatch_icall`, `__os_arm64x_dispatch_call_no_redirect`, `__os_arm64x_dispatch_ret`, `__os_arm64x_check_icall(_cfg)` и т.п. — это per-module **глобалы-указатели на функции**, которые загрузчик заполняет адресами хелперов эмулятора (xtajit.dll): *«`__os_arm64x_dispatch_icall` is not a function per-se, but… a (per-module) global variable containing a function pointer»* (corsix.org); *«Call checkers automatically invoke exit thunks when Arm64EC functions call into x64 functions»* (MS Learn Arm64EC ABI). Для адресации обычных `.data`-глобалов dispatch-механизма **нет** — каждый вид просто адресует свою копию своими `adrp/ldr`.

`[ВЫВОД]` **Канон:** ОС выбирает один вид (для x64-гостя — EC), применяет ARM64X-релокации (включая entry-swap), запускает **EC entry → EC CRT init → EC init_locale**, который наполняет **EC-копии** глобалов; EC-экспорты читают эти же EC-копии. Когерентность обеспечивается не алиасингом, а тем, что **инициализатор и читатели принадлежат одному виду**.

---

## Что делает настоящий загрузчик (Windows ntdll) — сводка

1. `[ФАКТ]` Видит, что процесс — x64/ARM64EC, а образ по умолчанию ARM64 → применяет `IMAGE_DYNAMIC_RELOCATION_ARM64X` (`MiApplyConditionalFixups`): `Machine`→`0x8664`, переключение EAT/IAT/exception-table/load-config, **swap `AddressOfEntryPoint`↔`AlternateEntryPoint`** так, что живым становится **EC entry**.
2. `[ФАКТ]` Строит EC code-range map (`RtlIsEcCode`/`set_arm64ec_range`) из `IMAGE_ARM64EC_METADATA.CodeMap`.
3. `[ФАКТ]` Заполняет `__os_arm64x_*` dispatch-глобалы адресами хелперов эмулятора.
4. `[ФАКТ/ВЫВОД]` Вызывает **единственный** `DllMain` = (после релокаций) EC entry. EC-`DllMain` прогоняет EC-CRT-init и EC-`init_locale`/`load_sortdefault_nls`, наполняя **EC-копию** `sort` (и прочих глобалов) — ту самую, что читают EC-экспорты.
5. `[ФАКТ]` Native-вид в этом процессе не исполняется (Hybrid Code Map), его копия данных остаётся неинициализированной и неиспользуемой.

---

## Выводы для фикса

**Корень проблемы** `[ВЫВОД, прямо ложится на твою эмпирику]`: твой кастомный загрузчик запустил **native ARM64 `DllMain`** hybrid-`kernelbase` (он вернул 1 и заполнил native-копию `sort@0x186548`), тогда как EC-экспорты, вызываемые из x64-гостя, читают **EC-копию `sort@0x183d28`**, оставшуюся нулевой. Настоящий загрузчик в EC-контексте запустил бы **EC entry**, который заполнил бы именно EC-копию.

**Что делать (по убыванию каноничности):**

1. **Запускать EC entry point, а не native.** `[ВЫВОД]` Это то, что делает реальная ОС. Практически: для образа, грузимого в EC/x64-контекст, перед вызовом `DllMain` (a) примени ARM64X DVRT-релокации к замапленному образу, чтобы `AddressOfEntryPoint` указывал на EC-entry (как делает Wine `apply_arm64x_relocations`/`update_arm64x_mapping` при условии «главный образ AMD64 + DLL ARM64»), **или** (b) возьми EC-entry напрямую из `IMAGE_ARM64EC_METADATA.AlternateEntryPoint` (поле в CHPE-метаданных, на которые ведёт `CHPEMetadataPointer` из load config) и вызови его. Тогда EC-`init_locale` наполнит EC-копию, и `GetStringTypeW` увидит валидный `ctype_idx`.

2. **Проверь, что DVRT ARM64X-релокации вообще применяются.** `[ФАКТ]` Если их пропустить, у тебя останется native-вид (`Machine=0xAA64`, native EAT/entry), и любая EC-семантика поедет. Триггер как в Wine: главный образ AMD64 **И** `FileHeader.Machine == ARM64` → применять. Поддержи все три фиксапа: `ZEROFILL`/`VALUE`/`DELTA`.

3. **Заполни `__os_arm64x_*` dispatch-глобалы** адресами своих эмуляторных хелперов (icall-checker, call/ret-диспетчеры) — иначе кросс-ABI вызовы из/в x64 не поедут, даже если данные починятся.

4. **Если по каким-то причинам исполняются ОБА вида** (например, твой слой реально гоняет и native ARM64 fast-path, и эмулируемый x64 в одном процессе — нетипично для чистого EC) `[?]`: тогда обе копии глобалов должны быть инициализированы. Но на настоящей Windows этого не бывает — вид один на процесс. Прежде чем городить «двойную инициализацию», убедись, что у тебя действительно живут оба вида; скорее всего, корректнее свести всё к EC-виду.

**Чего, вероятно, не делает твой загрузчик, а настоящий делает:** (1) применение ARM64X DVRT-релокаций с entry-swap → запуск **EC** `DllMain` вместо native; (2) разрешение EC-entry через `AlternateEntryPoint`/CHPE-метаданные; (3) заполнение `__os_arm64x_*`. Самый дешёвый и точный фикс под твой конкретный симптом — пункт 1: добиться, чтобы инициализатор и читатели `sort` принадлежали **одному (EC) виду**.

---

## Открытые вопросы / чего не подтвердили `[?]`

- Дословной MS-формулировки «запускается только один `DllMain`» публично нет — вывод опирается на entry-swap тест LLD + код загрузчика (Wine).
- Публичного Wine-баг-репорта именно про «пустой `sort.ctype_idx` в одном из ARM64X-видов `kernelbase`» не найдено (gitlab.winehq.org был недоступен боту). Механизм подтверждён, конкретный задокументированный дефект — нет.
- Дедуплицирует ли MSVC/LLVM обычные `.data`-глобалы или всегда оставляет две копии при двойной компиляции — дословно не задокументировано; твоя эмпирика (0x2820) подтверждает «две копии» для данного `kernelbase`.
- Внутренние имена `nt!MiApplyConditionalFixups`, `LdrpValidateEcCallTarget` — это reverse-engineering (FFRI/corsix), не официальная документация MS.

---

## Источники

**Microsoft Learn (первоисточник):**
- Arm64X PE files — https://learn.microsoft.com/en-us/windows/arm/arm64x-pe
- Understanding Arm64EC ABI and assembly code — https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi
- Building Arm64X binaries — https://learn.microsoft.com/en-us/windows/arm/arm64x-build
- GetStringTypeW (CT_CTYPE1/2/3) — https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-getstringtypew
- PE Format (IMAGE_FILE_MACHINE_ARM64X) — https://learn.microsoft.com/en-us/windows/win32/debug/pe-format

**Reverse-engineering DVRT/ARM64X:**
- FFRI Project Chameleon — new relocation entry ARM64X — https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/
- FFRI Project Chameleon — Relock 3.0 (arbitrary write, apply-conditions, Hybrid Code Map) — https://ffri.github.io/ProjectChameleon/arm64x_reloc_obfuscation/
- corsix — Windows ARM64EC notes (`__os_arm64x_*` как глобалы-указатели) — https://www.corsix.org/content/windows-arm64ec-notes
- emulators.com — ARM64EC explained — http://emulators.com/docs/abc_arm64ec_explained.htm
- Old New Thing — ARM64EC data layout — https://devblogs.microsoft.com/oldnewthing/20220830-00/?p=107069

**LLVM/LLD codegen (первоисточник реализации):**
- COFF.h (enum `Arm64XFixupType`, `IMAGE_DYNAMIC_RELOCATION_ARM64X=6`, `0xA64E`) — https://raw.githubusercontent.com/llvm/llvm-project/main/llvm/include/llvm/BinaryFormat/COFF.h
- PR #118035 basic ARM64X dynamic relocations / machine→AMD64 — https://github.com/llvm/llvm-project/pull/118035
- PR #119294 hybrid symbol table (раздельные namespace’ы) — https://github.com/llvm/llvm-project/pull/119294
- PR #124189 hybrid IAT — https://github.com/llvm/llvm-project/pull/124189
- PR #123346 alternate entry point in CHPE metadata (+ `arm64x-entry.test`) — https://github.com/llvm/llvm-project/pull/123346
- PR #123652 separate EC/native exports — https://github.com/llvm/llvm-project/pull/123652
- PR #121337 EC load config for ARM64X relocations — https://github.com/llvm/llvm-project/pull/121337
- PR #123723 ARM64X relocations for exception table — https://github.com/llvm/llvm-project/pull/123723

**Wine (первоисточник реализации загрузчика и locale):**
- `dlls/ntdll/unix/virtual.c` (apply_arm64x_relocations, update_arm64x_mapping, триггер AMD64+ARM64) — https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/unix/virtual.c
- `dlls/ntdll/loader.c` (EntryPoint из AddressOfEntryPoint, arm64ec_update_hybrid_metadata) — https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/loader.c
- `dlls/kernelbase/locale.c` (struct `sort`, load_sortdefault_nls, init_locale, get_char_type) — https://github.com/wine-mirror/wine/blob/master/dlls/kernelbase/locale.c
- Commit «Support ARM64EC code in RtlLookupFunctionEntry» — https://list.winehq.org/mailman3/hyperkitty/list/wine-commits@winehq.org/thread/D67ZPNAPX75E6UJFIPSW2FU54GYTZINI/
- Wine 8.8 announce (initial ARM64EC) — https://www.winehq.org/announce/8.8
- Wine MR !5591 (`__os_arm64x_dispatch_call`) — https://gitlab.winehq.org/wine/wine/-/merge_requests/5591
- mingw-w64 — структуры ARM64EC metadata (`__chpe_metadata`, `AlternateEntryPoint`, dispatch-символы) — https://www.mail-archive.com/mingw-w64-public@lists.sourceforge.net/msg24271.html

**Прочее:**
- FEX-Emu wiki — Development:ARM64EC (ARM64X = линкер-релокации, объединяющие EC+ARM64; форк bylaws) — https://wiki.fex-emu.com/index.php/Development:ARM64EC
- MS Developer Community — IAT/auxiliary-IAT рассинхрон при линковке ARM64EC — https://developercommunity.visualstudio.com/t/When-linking-ARM64EC-the-IAT-and-auxili/10003910
