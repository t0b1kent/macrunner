# Сборка ARM64X (EC-capable) DLL для DXMT — ресёрч-отчёт

**Задача:** заставить ARM64EC-загрузчик Wine брать `dxgi.dll`/`d3d11.dll`/`d3d10core.dll` от DXMT, а не от Wine. Для этого DXMT-овские DLL должны быть **ARM64X** (гибридными), а не plain-ARM64, как сейчас.

**Легенда тегов:** `[ФАКТ]` — прямо сказано в первоисточнике; `[ВЫВОД]` — синтез/инференс из источников; `[?]` — не подтверждено до конца, требует личной проверки на бинаре.

**TL;DR.** Перелинковать существующие `arm64ec`-объекты DXMT «одним флагом» в ARM64X **нельзя**: ARM64X по определению содержит ДВА представления кода — `aarch64` (нативный ARM64) и `arm64ec`, — слитых линкером. Нужен второй проход компиляции (плюс комбинирующая линковка `-machine:arm64x` / winebuild `-marm64x`). Хорошая новость: Wine уже умеет это ровно теми же инструментами (llvm-mingw + `winebuild -marm64x`), на которых DXMT уже собирается, так что это известный рабочий путь, а не исследовательский риск.

---

## 0. Почему сейчас выигрывает Wine, а не DXMT (механика проблемы)

`[ФАКТ]` На Windows-on-ARM нет отдельной папки «x64 system32»: и нативные ARM64-процессы, и эмулируемые x64-процессы грузят **один и тот же** файл из System32, потому что системные бинарники пересобраны как ARM64X. ([Microsoft Learn — Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe))

`[ФАКТ]` ARM64X-бинарь несёт ДВА представления (две «namespace»): нативный ARM64 и x64/ARM64EC, слитых в один PE с устранением дублей. ([Microsoft Learn — Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe))

`[ФАКТ]` Загрузчик/эмулятор для x64-гостя применяет к ARM64X-DLL «трансформации» через ARM64X dynamic relocations (DVRT): в памяти переписывается поле `Machine` заголовка с `0xAA64`(ARM64) на `0x8664`(x86-64) и подменяется RVA таблицы экспортов (EAT) на «x64-шный» EAT. Применяется из ядра функцией `nt!MiApplyConditionalFixups`. Именно поэтому один и тот же файл работает и для x64-эмуляции, и для нативного ARM64. ([FFRI / Project Chameleon, K. M. Nakagawa](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/))

`[ВЫВОД]` Plain-ARM64 DLL от DXMT (`Machine=0xAA64`, `CHPEMetadataPointer==0`, нет DVRT/`.hexpthk`) для эмулируемого x64-процесса **не EC-capable**: у неё нет ни x64-EAT, ни entry-thunk'ов, через которые x64-код вызывает ARM64EC-функции. Поэтому ARM64EC-загрузчик предпочитает ARM64X-компаньон Wine (у которого всё это есть). Чтобы DXMT-овская DLL могла «выиграть», она должна сама стать ARM64X. (Синтез [Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe) + [FFRI](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/) + [Arm64EC ABI](https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi))

---

## (a) Как вообще собираются ARM64X DLL: механизм линковки и нужные секции

### Концепция

`[ФАКТ]` Машинные типы в заголовке PE: `IMAGE_FILE_MACHINE_ARM64 = 0xAA64`, `IMAGE_FILE_MACHINE_ARM64EC = 0xA641`, `IMAGE_FILE_MACHINE_ARM64X = 0xA64E`, `IMAGE_FILE_MACHINE_AMD64 = 0x8664`. ([Microsoft — PE Format, Machine Types](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format))

`[ФАКТ]` ARM64EC — это «не новый машинный тип» в смысле формата: ARM64X **расширяет** AA64-формат дополнительной метадатой. По умолчанию ARM64X-бинарь выглядит как обычный ARM64 (`Machine=0xAA64`), поэтому даже система, не знающая про ARM64X, может загрузить его в ARM64-процесс. ([emulators.com — ARM64EC explained, D. Mihocka](http://emulators.com/docs/abc_arm64ec_explained.htm); [Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe))

### Какие бинарные фичи делают DLL «ARM64X/EC-capable»

1. **CHPE-метадата / `IMAGE_ARM64EC_METADATA`.** `[ФАКТ]` В load config (`_load_config_used`) поле `CHPEMetadataPointer` указывает на блоб `__chpe_metadata`. Plain-ARM64 имеет `CHPEMetadataPointer == 0`; у ARM64EC/ARM64X оно ненулевое. Правило декодирования: при наличии CHPE-метадаты `Machine==AMD64 ⇒ ARM64EC`, `Machine==ARM64 ⇒ ARM64X`. ([FFRI](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/); [Binary Ninja issue #8096](https://github.com/vector35/binaryninja-api/issues/8096); [mingw-w64 CHPE struct, mail-archive](https://www.mail-archive.com/mingw-w64-public@lists.sourceforge.net/msg24271.html))

   `[ФАКТ]` В `__chpe_metadata` лежат, в частности: `CodeMap` → `__hybrid_code_map` (range-table: какие диапазоны адресов — ARM64EC, какие — x64), `RedirectionMetadata` → `__arm64x_redirection_metadata`, `AuxiliaryIAT` → `__hybrid_auxiliary_iat`, `AlternateEntryPoint` → `__arm64x_native_entrypoint`, и RVA на эмулятор-хелперы `__os_arm64x_dispatch_*`/`__os_arm64x_check_*`. ([mingw-w64 struct](https://www.mail-archive.com/mingw-w64-public@lists.sourceforge.net/msg24271.html))

2. **ARM64X dynamic relocation table / DVRT** (та самая `.a64xrm` в вашем описании). `[ФАКТ]` Реализуется записью `IMAGE_DYNAMIC_RELOCATION_ARM64X` — новый тип Dynamic Value Relocation Table, лежит после base-relocation блока в `.reloc`. Содержит фиксапы трёх типов (zero-fill / assign-value / add-delta), которые на лету переписывают `Machine` и RVA экспортов под архитектуру загружающего процесса. ([FFRI](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/))

   `[?]` Конкретное имя секции **`.a64xrm`** документировано только исследователем FFRI (в его статье оно прямо отнесено к «не разобранным» деталям); официальной MS-доки на байт-лейаут именно секции `.a64xrm` я не нашёл. Сам механизм DVRT/`IMAGE_DYNAMIC_RELOCATION_ARM64X` подтверждён надёжно. ([FFRI](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/))

3. **Export / entry / exit thunks** (ваша `.hexpthk`). `[ФАКТ]` Для интеропа x64↔ARM64EC компилятор/линкер генерирует: **entry thunks** (x64→EC, по одному на сигнатуру), **exit thunks** (EC→x64), и **fast-forward sequences (FFS)** — крошечные x64-функции-переходники, включённые по умолчанию для всех экспортов DLL и для `__declspec(hybrid_patchable)`. Благодаря FFS `GetProcAddress`/`&func` отдаёт x64-адрес, что удовлетворяет x64-код, который хукает/вызывает функцию. ([Arm64EC ABI](https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi); [hybrid_patchable](https://learn.microsoft.com/en-us/cpp/cpp/hybrid-patchable))

   `[?]` Конкретное имя секции **`.hexpthk`** в MS/LLVM-доках я не нашёл — это, по-видимому, внутреннее имя секции MSVC-линкера. Подтверждённые рядом имена: entry/exit-thunk'и LLVM кладёт в `.wowthk` (COMDAT, для дедупликации). Сам концепт (FFS/export-thunk, символы вида `EXP+#func`) подтверждён. Рекомендую проверить `.hexpthk` напрямую через `llvm-readobj`/`link /dump /headers` на MSVC-собранной EC-DLL. ([LLVM D133256](https://reviews.llvm.org/D133256); [hybrid_patchable](https://learn.microsoft.com/en-us/cpp/cpp/hybrid-patchable))

### Механизм линковки: как из ARM64- и ARM64EC-объектов слинковать ОДИН ARM64X DLL

`[ФАКТ]` «Two-pass» в доке Microsoft — это про **двойную компиляцию** (раз под ARM64, раз под ARM64EC), а финальная **линковка одна**: `/machine:arm64x` сливает оба набора объектов/библиотек в один ARM64X PE. ([Microsoft Learn — Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build))

`[ФАКТ]` Канонический MSVC-рецепт (pure-forwarder, чище всего показывает флаги):

```bat
cl /c /Foempty_arm64.obj empty.cpp
cl /c /arm64EC /Foempty_x64.obj empty.cpp
link /lib /machine:x64   /def:foo_x64.def   /out:foo_x64.lib
link /lib /machine:arm64 /def:foo_arm64.def /out:foo_arm64.lib
link /dll /noentry /machine:arm64x /defArm64Native:foo_arm64.def /def:foo_x64.def ^
     empty_arm64.obj empty_x64.obj /out:foo.dll foo_arm64.lib foo_x64.lib
```

Ключевое: `/machine:arm64x` (режим слияния), `/def:` → экспорты EC/x64-вью, `/defArm64Native:` → экспорты нативного ARM64-вью, и **оба** набора объектов (`arm64` и `arm64EC`) в одной команде. ([Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build))

`[ФАКТ]` **LLD (lld-link) поддерживает то же самое одной командой** — это критично, т.к. DXMT и Wine на нём и собираются. Пример прямо из апстрим-тестов LLD:

```
lld-link -machine:arm64x -dll -out:out.dll arm64ec-func.obj arm64-func.obj \
         loadconfig-arm64.obj loadconfig-arm64ec.obj -noentry -export:func
```

где `arm64-func.obj` собран `-triple=aarch64-windows`, а `arm64ec-func.obj` — `-triple=arm64ec-windows`. LLD поддерживает и `-defArm64Native:` так же, как MSVC. ([LLD тест arm64x-export.test](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-export.test); [LLD PR #123850](https://github.com/llvm/llvm-project/pull/123850))

`[ФАКТ]` Обоим вью нужен свой load config: нативный `_load_config_used` (aarch64) и EC-вариант (arm64ec). Без любого из них — предупреждение `native/EC version of '_load_config_used' is missing`. ([LLD тест arm64x-loadconfig.s](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-loadconfig.s))

`[ФАКТ]` Под капотом LLD моделирует слияние как **два независимых `SymbolTable`** (ARM64 и ARM64EC namespace'ы, которые не могут ссылаться друг на друга), пишет два load config'а, две таблицы экспортов, общий (merged) IAT и ARM64X dynamic relocations. ([LLD PR #119294 — separate symbol tables](https://github.com/llvm/llvm-project/pull/119294); [#120326 — both load configs](https://github.com/llvm/llvm-project/pull/120326); [#118035 — ARM64X dynamic relocations](https://github.com/llvm/llvm-project/pull/118035); [#124189 — hybrid IAT](https://github.com/llvm/llvm-project/pull/124189); [#121337 — EC load config relocs](https://github.com/llvm/llvm-project/pull/121337))

`[ФАКТ]` Поддержка `/machine:arm64ec` в LLD «завершена» начиная с **LLVM 20.1.0** (release notes COFF). ARM64X-слияние докручивалось серией PR cjacek в том же цикле 19→20. ([LLD 20.1.0 ReleaseNotes](https://releases.llvm.org/20.1.0/tools/lld/docs/ReleaseNotes.html))

---

## (b) Конкретное минимальное изменение в meson-сборку DXMT

### Что DXMT делает сейчас

`[ФАКТ]` DXMT собирается через Meson (>=1.3.0) с тремя cross-файлами: `build-win32.txt`, `build-win64.txt`, `build-arm64ec.txt`. Тулчейн — **llvm-mingw** (mstorsjo), бинарники вызываются по triple-префиксу. ([root meson.build](https://raw.githubusercontent.com/3Shain/dxmt/main/meson.build); [CI](https://raw.githubusercontent.com/3Shain/dxmt/main/.github/workflows/ci.yml))

`[ФАКТ]` `build-arm64ec.txt` задаёт компиляторы `arm64ec-w64-mingw32-gcc/g++`, а `host_machine` — обобщённый `aarch64`/`aarch64`. То есть «EC-ность» определяется **исключительно triple'ом** llvm-mingw-обёртки; meson про `arm64ec` как про CPU ничего не знает. ([build-arm64ec.txt](https://raw.githubusercontent.com/3Shain/dxmt/main/build-arm64ec.txt))

`[ФАКТ]` Линк-флаги во всём проекте — GNU-driver стиля: `-static`, `-Wl,--file-alignment=4096` (+ `-Wl,--enable-stdcall-fixup`/`-Wl,--kill-at` только на 32-бит). **Нигде нет** `-machine`/`/machine`/`/machine:arm64x` и никакого ARM64X. Аarch64-конфиг существует только как `arm64ec` — отдельного plain-`arm64` cross-файла (`build-arm64.txt`) нет. ([root meson.build](https://raw.githubusercontent.com/3Shain/dxmt/main/meson.build); все per-DLL meson.build)

`[ФАКТ]` Производимые DLL: `dxgi.dll`, `d3d11.dll` (код d3d10 вкомпилён сюда), `d3d10core.dll`, `winemetal.dll` (+ нативный `winemetal.so` unixlib). Каждая — `shared_library(..., name_prefix:'')`, после чего прогоняется через `winebuild --builtin` (`-Dwine_builtin_dll=true` по умолчанию), и ставится в Wine-арх-каталог `aarch64-windows`. ([src/*/meson.build](https://raw.githubusercontent.com/3Shain/dxmt/main/src/meson.build))

`[ФАКТ]` ARM64EC-сборка в CI — это job `build-clang-debugoptimized-arm64ec-windows-cross`, который зависит от форка `3Shain/wine` (`WINE_ARM64EC_VERSION: wine-11.2`) и нативного arm64-LLVM. ([CI](https://raw.githubusercontent.com/3Shain/dxmt/main/.github/workflows/ci.yml))

### Ответ: можно ли «просто перелинковать» один флаг?

`[ВЫВОД]` **Нет.** ARM64X требует ДВУХ представлений кода. Сейчас DXMT компилирует только `arm64ec`-объекты. Чтобы получить ARM64X, нужен ещё нативный `aarch64`-проход компиляции, а затем комбинирующая линковка. Это следует напрямую из того, что `/machine:arm64x`/`-machine:arm64x` принимает на вход **оба** набора объектов и для каждого вью нужен свой `_load_config_used`. ([Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build); [LLD arm64x-loadconfig.s](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-loadconfig.s))

`[ВЫВОД]` Нужны ли ОБА набора объектов? Да. Минимум — `arm64ec` (уже есть) **плюс** `aarch64` (нативный ARM64). Чисто-форвардерный вариант ARM64X технически позволил бы крошечный нативный вью (пустой `empty_arm64.obj`), но для translation-layer'а это бессмысленно: цель — чтобы x64-гость попадал именно в DXMT-овский EC-код, и достаточно полноценного EC-вью + минимального нативного вью. Конкретный объём нативного вью — это вопрос дизайна (см. подводные камни). `[?]`

### Минимальное изменение (рекомендуемый путь — «как у Wine», без сырого lld-link)

`[ВЫВОД]` Поскольку DXMT уже финализирует каждую DLL через `winebuild --builtin`, а Wine-овский `winebuild`/`winegcc` умеют делать ARM64X напрямую опцией **`-marm64x`** ([Wine 10.0 ANNOUNCE](https://www.linuxcompatible.org/story/wine-100-released/)), самый «минимальный» и наименее рискованный путь повторяет рецепт Wine:

1. **Добавить второй cross-файл** `build-arm64.txt` с нативным ARM64-тулчейном llvm-mingw: `c = 'aarch64-w64-mingw32-gcc'`, `cpp = 'aarch64-w64-mingw32-g++'`, `host_machine.cpu_family = 'aarch64'`. (Триплет `aarch64-w64-mingw32` даёт plain-ARM64, в отличие от `arm64ec-w64-mingw32`.) `[ВЫВОД]` по аналогии с [build-arm64ec.txt](https://raw.githubusercontent.com/3Shain/dxmt/main/build-arm64ec.txt) и [FEX/Wine рецептом](https://wiki.fex-emu.com/index.php/Development:ARM64EC).

2. **Собрать каждую DLL дважды** — текущий `arm64ec`-build (EC-вью) и новый `aarch64`-build (нативный вью).

3. **Комбинирующий шаг ARM64X.** Два варианта реализации внутри meson:
   - **Через Wine (предпочтительно, минимально):** заменить финальный `winebuild --builtin` на путь, генерирующий гибрид `winebuild -marm64x` (как это делает сам Wine при `--enable-archs=arm64ec,aarch64`). Это держит DXMT в том же тулчейне и снимает ручную возню с load config'ами/thunk'ами. `[ВЫВОД]` (опора: `-marm64x` подтверждён в [Wine 10.0 ANNOUNCE](https://www.linuxcompatible.org/story/wine-100-released/)).
   - **Через сырой lld-link:** добавить `custom_target`, который вызывает `lld-link -machine:arm64x -dll -noentry <ec.objs+ec.libs> <arm64.objs+arm64.libs> loadconfig-arm64.obj loadconfig-arm64ec.obj -def:<ec.def> -defArm64Native:<arm64.def> -out:<name>.dll`. `[ФАКТ]`-форма команды — из [LLD arm64x-export.test](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-export.test).

`[ВЫВОД]` Глубина изменения: это **не однострочный флаг**, но и не переписывание архитектуры. Объём = новый cross-файл + дублирование build-таргетов для aarch64 + один combine-`custom_target` на каждую из `dxgi/d3d11/d3d10core`. Это та же по сложности правка, что Wine внёс через `--enable-archs=arm64ec,aarch64` + `-marm64x`.

`[?]` **Осталось проверить на месте (вне рамок этого ресёрча):** (1) как именно DXMT-овский `winebuild --builtin`-шаг сочетается с `-marm64x` (нужен ли DXMT-овский форк winebuild, или хватит апстрим-winebuild ≥ Wine 10); (2) совпадают ли набор экспортов `dxgi/d3d11/d3d10core` в обоих вью (def-файлы); (3) нет ли в DXMT уже заведённого issue про arm64x (issue-search через JS-UI не охвачен). ([CI](https://raw.githubusercontent.com/3Shain/dxmt/main/.github/workflows/ci.yml))

---

## (c) Рецепт сборки ARM64X у Wine — рабочий прецедент

`[ФАКТ]` Опция configure: `--enable-archs={i386,x86_64,arm,aarch64,arm64ec,none}`. ([Wine configure.ac](https://raw.githubusercontent.com/wine-mirror/wine/master/configure.ac))

`[ФАКТ]` Из Wine 10.0 release notes дословно: *«Hybrid ARM64X modules are fully supported… All of Wine can be built as ARM64X by passing the `--enable-archs=arm64ec,aarch64` option to configure. This still requires an experimental LLVM toolchain, but… the upcoming LLVM 20 release will be able to build ARM64X Wine out of the box.»* ([Wine 10.0 ANNOUNCE, полная репродукция](https://www.linuxcompatible.org/story/wine-100-released/))

`[ФАКТ]` `arm64ec` автоматически тянет x86_64 как «extra arch» (`arm64ec) test ${extra_arch+y} || extra_arch=x86_64`), потому что EC-модули должны хостить/прокидывать x86-64-код. ([configure.ac](https://raw.githubusercontent.com/wine-mirror/wine/master/configure.ac))

`[ФАКТ]` Тулчейн: Wine ищет `arm64ec-w64-mingw32-clang` (затем `-gcc`, затем `clang`) и `aarch64-w64-mingw32-clang`; при `--with-mingw=llvm-mingw`/`clang` линкер форсится `-fuse-ld=lld`. То есть **llvm-mingw + lld-link**. ([configure.ac](https://raw.githubusercontent.com/wine-mirror/wine/master/configure.ac))

`[ФАКТ]` Wine-овский механизм слияния: *«The `winegcc` and `winebuild` tools can create hybrid ARM64X modules with the `-marm64x` option.»* ([Wine 10.0 ANNOUNCE](https://www.linuxcompatible.org/story/wine-100-released/)) `[ВЫВОД]` `-marm64x` появился в Wine 9.7 (апр. 2024, J. Caban) — вторичные источники; сам факт наличия опции подтверждён в Wine 10.0 ANNOUNCE.

`[ФАКТ]` Полный copy-paste рецепт (из FEX-Emu wiki; тулчейн — форк `bylaws/llvm-mingw` в PATH):

```sh
./configure --enable-archs=arm64ec,aarch64,i386 --prefix=/usr --with-mingw=clang --disable-tests
make -j$(nproc)
sudo --preserve-env=PATH make install -j$(nproc)
```

DXVK/vkd3d-proton там же кросс-собираются meson-cross-файлом с `c = 'arm64ec-w64-mingw32-gcc'`, `cpu_family = 'aarch64'`. ([FEX-Emu — Development:ARM64EC](https://wiki.fex-emu.com/index.php/Development:ARM64EC))

`[ВЫВОД]` Прямую `lld-link`-команду на Wine-овский `dxgi.dll` опубликованной нигде нет (её прячут `winegcc -marm64x`/`winebuild -marm64x`); но это ровно та же обёртка вокруг `/machine:arm64x`, что задокументирована Microsoft. ([Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build))

---

## llvm-mingw и ARM64X (вопрос 5)

`[ФАКТ]` Возможность линковать ARM64EC «завершена» в LLD **LLVM 20.1.0**; ARM64X-слияние докручено в том же цикле. ([LLD 20.1.0 ReleaseNotes](https://releases.llvm.org/20.1.0/tools/lld/docs/ReleaseNotes.html))

`[ФАКТ]` README llvm-mingw в основном описывает классические таргеты (i686/x86_64/armv7/arm64) и не упоминает arm64ec/arm64x в заголовке — но обёртка `clang-target-wrapper.sh` универсальна: triple строится из basename, так что `arm64ec-w64-mingw32-clang` → `-target arm64ec-w64-mingw32`, без спец-кейсов. ([llvm-mingw README](https://raw.githubusercontent.com/mstorsjo/llvm-mingw/master/README.md); [clang-target-wrapper.sh](https://raw.githubusercontent.com/mstorsjo/llvm-mingw/master/wrappers/clang-target-wrapper.sh))

`[ФАКТ]` «Экспериментальный LLVM-тулчейн», на который ссылается Wine 10.0, — это форк **`bylaws/llvm-mingw`** (Billy Laws), нёсший arm64ec/arm64x-патчи до апстрима LLVM 20. ([FEX-Emu wiki](https://wiki.fex-emu.com/index.php/Development:ARM64EC); [bylaws/llvm-mingw](https://github.com/bylaws/llvm-mingw))

`[ФАКТ]` Версии llvm-mingw: `20241203`=LLVM 19.1.5, `20251216`=LLVM 21.1.8, `20260311`=LLVM 22.1.1. `[?]` Точный date-tag, где впервые заработали `arm64ec`-обёртки, в release-описаниях не указан (там нет прозы-чейнджлога); привязка — к версии LLVM (≥20 = апстрим-готово). ([llvm-mingw releases](https://github.com/mstorsjo/llvm-mingw/releases))

`[ФАКТ]` Со стороны CRT: mingw-w64 v13.0.0 (2025-06-08) — «Basic support for ARM64EC (arm64ec-w64-mingw32)»; v14.0.0 (2026-03-27) — «Initial support for arm64ec-w64-mingw32». ([mingw-w64 changelog](https://www.mingw-w64.org/changelog/))

`[ВЫВОД]` Поскольку DXMT-CI тянет свежий llvm-mingw (`20251216`, LLVM 21) — тулчейн **уже способен** выдавать ARM64X. Блокер не в тулчейне, а в build-графе meson (нет второго aarch64-вью и combine-шага).

---

## (d) Подводные камни

1. **Перелинковать без перекомпиляции — нельзя.** `[ВЫВОД]` ARM64X = два вью; существующие `arm64ec`-объекты дают только EC-вью. Нужен отдельный `aarch64`-проход компиляции + combine. ([Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build))

2. **Два load config'а обязательны.** `[ФАКТ]` Нужны и нативный, и EC `_load_config_used`; пропуск любого → предупреждение линкера и, вероятно, нерабочий ARM64X. У llvm-mingw это идёт из CRT-объектов соответствующего triple. ([LLD arm64x-loadconfig.s](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-loadconfig.s))

3. **Совпадение экспортов между вью.** `[ВЫВОД]` У ARM64X две таблицы экспортов (native + EC). Для `dxgi/d3d11/d3d10core` экспорты должны корректно лечь в оба вью (через `-def:`/`-defArm64Native:` или `.drectve` объектов). Рассинхрон → отсутствующие символы в одном из вью. ([LLD arm64x-export.test](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-export.test))

4. **C++/CRT.** `[?]` Нативный ARM64-вью и EC-вью линкуются против разных CRT-вариантов llvm-mingw (`aarch64-` vs `arm64ec-`). Нужно убедиться, что DXMT-овский `-static` + `api-ms-win-crt-*` (ucrt) одинаково доступен для обоих triple'ов. Конкретных ограничений C++ ABI для ARM64X в найденных первоисточниках не зафиксировано; mingw-w64 arm64ec — «initial/basic support», т.е. возможны шероховатости в STL/исключениях. ([mingw-w64 changelog](https://www.mingw-w64.org/changelog/); [DXMT meson.build](https://raw.githubusercontent.com/3Shain/dxmt/main/meson.build))

5. **Размер/перф.** `[ФАКТ]` ARM64X крупнее (несёт оба вью, минус устранённые дубли) — это сознательный trade-off MS «больше на диске, один файл». `[?]` Численного оверхеда по размеру/скорости для DXMT-DLL в источниках нет; рантайм-стоимость интеропа — это thunk-переходы на границе x64↔EC, которые в DXMT и так уже есть (он уже EC). ([Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe))

6. **Зависимость от форка Wine.** `[ФАКТ]` DXMT-CI собирает arm64ec против `3Shain/wine wine-11.2`. `[?]` Поддерживает ли именно этот форк-winebuild `-marm64x` так же, как апстрим Wine ≥10 — надо проверить на месте. ([CI](https://raw.githubusercontent.com/3Shain/dxmt/main/.github/workflows/ci.yml))

7. **DXVK как прецедент — слабый.** `[ФАКТ]` Апстрим DXVK first-party ARM64EC/ARM64X не даёт (мейнтейнер doitsujin: «We don't have the resources…»); только форки. Прецедент для опоры — это **Wine**, а не DXVK. ([DXVK issue #5423](https://github.com/doitsujin/dxvk/issues/5423))

---

## Сводка ответов на 6 вопросов

1. **Механика ARM64X-линковки:** одна команда `lld-link -machine:arm64x` (или MSVC `link /machine:arm64x`) с **обоими** наборами объектов (`aarch64-windows` + `arm64ec-windows`), load config'ами обоих вью, `-def:`/`-defArm64Native:`. `[ФАКТ]`
2. **Что делает DLL EC-capable:** ненулевой `CHPEMetadataPointer`→`__chpe_metadata` (code map, redirection, aux IAT), DVRT `IMAGE_DYNAMIC_RELOCATION_ARM64X` (ваша `.a64xrm`), export/entry/exit thunks/FFS (ваша `.hexpthk`). Генерируются `/machine:arm64x` + EC-объектами + load config'ами. `[ФАКТ]` (имена секций `.a64xrm`/`.hexpthk` — `[?]`)
3. **Минимум для DXMT:** не один флаг. Добавить `build-arm64.txt` (aarch64), собирать `dxgi/d3d11/d3d10core` дважды, combine-шаг `winebuild -marm64x` (предпочтительно) либо сырой `lld-link -machine:arm64x`. Нужны **оба** набора объектов. `[ВЫВОД]`
4. **Wine-прецедент:** `./configure --enable-archs=arm64ec,aarch64` + llvm-mingw + `winegcc/winebuild -marm64x`. `[ФАКТ]`
5. **llvm-mingw:** да, через lld; готово в апстриме с LLVM 20.1.0, до того — форк `bylaws/llvm-mingw`. DXMT-CI уже на LLVM 21 → тулчейн готов. `[ФАКТ]/[ВЫВОД]`
6. **Подводные камни:** перелинковка без второго прохода невозможна; два load config'а; синхронизация экспортов; CRT-двойственность aarch64/arm64ec; размер; зависимость от форк-winebuild. `[ВЫВОД]/[?]`

---

## Источники

Microsoft Learn:
- [Building Arm64X binaries](https://learn.microsoft.com/en-us/windows/arm/arm64x-build)
- [Arm64X PE files](https://learn.microsoft.com/en-us/windows/arm/arm64x-pe)
- [Arm64EC ABI overview](https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi)
- [Arm64EC ABI conventions (cpp)](https://learn.microsoft.com/en-us/cpp/build/arm64ec-windows-abi-conventions)
- [hybrid_patchable](https://learn.microsoft.com/en-us/cpp/cpp/hybrid-patchable)
- [/MACHINE reference](https://learn.microsoft.com/en-us/cpp/build/reference/machine-specify-target-platform)
- [PE Format — Machine Types](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format)

LLVM/LLD:
- [LLD 20.1.0 ReleaseNotes](https://releases.llvm.org/20.1.0/tools/lld/docs/ReleaseNotes.html)
- [PR #118035 (ARM64X dynamic relocations)](https://github.com/llvm/llvm-project/pull/118035) · [#119294 (separate symtabs)](https://github.com/llvm/llvm-project/pull/119294) · [#120326 (both load configs)](https://github.com/llvm/llvm-project/pull/120326) · [#121337 (EC load config relocs)](https://github.com/llvm/llvm-project/pull/121337) · [#123850 (-defArm64Native)](https://github.com/llvm/llvm-project/pull/123850) · [#124189 (hybrid IAT)](https://github.com/llvm/llvm-project/pull/124189)
- [arm64x-export.test](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-export.test) · [arm64x-loadconfig.s](https://raw.githubusercontent.com/llvm/llvm-project/main/lld/test/COFF/arm64x-loadconfig.s)

DXMT / DXVK:
- [3Shain/dxmt meson.build](https://raw.githubusercontent.com/3Shain/dxmt/main/meson.build) · [build-arm64ec.txt](https://raw.githubusercontent.com/3Shain/dxmt/main/build-arm64ec.txt) · [CI](https://raw.githubusercontent.com/3Shain/dxmt/main/.github/workflows/ci.yml) · [DEVELOPMENT.md](https://raw.githubusercontent.com/3Shain/dxmt/main/docs/DEVELOPMENT.md)
- [DXVK issue #5423 (arm64ec/arm64x declined)](https://github.com/doitsujin/dxvk/issues/5423)

Wine / llvm-mingw / FEX:
- [Wine configure.ac](https://raw.githubusercontent.com/wine-mirror/wine/master/configure.ac)
- [Wine 10.0 ANNOUNCE (репродукция)](https://www.linuxcompatible.org/story/wine-100-released/)
- [FEX-Emu — Development:ARM64EC](https://wiki.fex-emu.com/index.php/Development:ARM64EC)
- [mstorsjo/llvm-mingw README](https://raw.githubusercontent.com/mstorsjo/llvm-mingw/master/README.md) · [releases](https://github.com/mstorsjo/llvm-mingw/releases) · [clang-target-wrapper.sh](https://raw.githubusercontent.com/mstorsjo/llvm-mingw/master/wrappers/clang-target-wrapper.sh) · [bylaws/llvm-mingw](https://github.com/bylaws/llvm-mingw)
- [mingw-w64 changelog](https://www.mingw-w64.org/changelog/)

Реверс-инжиниринг / внутренности (не-MS первоисточники):
- [FFRI Project Chameleon (CHPEv2/ARM64X relocs)](https://ffri.github.io/ProjectChameleon/new_reloc_chpev2/)
- [corsix — Windows ARM64EC notes](https://www.corsix.org/content/windows-arm64ec-notes)
- [emulators.com — ARM64EC explained](http://emulators.com/docs/abc_arm64ec_explained.htm)
- [mingw-w64 CHPE metadata struct (mail-archive)](https://www.mail-archive.com/mingw-w64-public@lists.sourceforge.net/msg24271.html)
- [Binary Ninja issue #8096 (machine-type decode)](https://github.com/vector35/binaryninja-api/issues/8096)

**Не удалось получить (за Anubis-стеной / JS-UI):** wine-devel thread по `-marm64x`, gitlab.winehq.org коммиты, issue-search в 3Shain/dxmt по `arm64x`. Эти три пункта — кандидаты на ручную проверку.
