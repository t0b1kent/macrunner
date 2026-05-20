# AGENTS.md — onboarding для AI агентов (Codex, Claude, Cursor)

Этот файл — единый источник истины для путей, скриптов и known traps в MacRunner.
Любой агент, который потерял контекст (compaction, session restart, новая сессия),
должен прочитать его прежде чем запускать команды.

## 🎯 ZEROTH PRINCIPLE — Native, root-cause, seamless

**MacRunner существует чтобы Windows-приложения работали на Mac как нативные.
Не "запустить и пусть как-то крутится". Не "почти работает с workaround'ами".
А seamless, native, профессиональный продукт.**

Это означает для каждого решения:

### 1. Solve at the root, never at the symptom

- Когда находишь bug — fix его в правильном архитектурном слое, не там где удобнее
- Workaround acceptable **только** как temporary scaffolding пока ищется real fix,
  и **только** если явно помечен `TODO: workaround for #N, real fix pending`
- Если есть выбор между "быстрый patch который скрывает symptom" и
  "правильный fix который требует больше работы" — **всегда выбирай правильный**
- "Запустить сначала, исправить потом" — **запрещено**. "Потом" не наступает.

### 2. Native by default, не emulation-style

- Используй macOS APIs напрямую (AVAudioEngine, Metal, CoreText) а не Linux shims
- Используй Apple Silicon strengths (unified memory, AMX, GPU) когда применимо
- Не порти Linux подход слепо — Mac имеет свои idioms, lean into them
- Performance equal или better чем native Mac app — наш baseline, не stretch goal

### 3. Architectural completeness over feature breadth

- 5 fully-correct apps лучше чем 50 partially-broken
- Каждый class bug закрывается **полностью** через family audit, не narrow patch
- Cross-arch boundaries explicit и safe, не "обычно работает"
- Memory model, signal model, threading model — все coherent и documented

### 4. Decision rubric для типичных dilemmas

| Choice | Default answer | Override only if |
|---|---|---|
| Quick workaround vs proper fix | **Proper fix** | Production blocker AND проперно помечен TODO |
| Native API vs ported Linux API | **Native** | Native API не существует / incomplete |
| Bug at root layer vs at symptom layer | **Root layer** | Root layer изменения требуют scope больше чем доступно (rare) |
| Family fix vs single opcode | **Family** | Family > 30 members AND split явно документирован |
| Implement now vs defer | **Now if scope allows** | Defer only with explicit "Phase X.Y subtask" tag |
| New abstraction vs use existing | **Use existing** | Existing genuinely doesn't fit (justified in commit) |

### 5. "Why this matters" — стратегический контекст

Цель MacRunner — **продаваемый продукт** для Mac users которые хотят запускать
Windows apps без Rosetta. Конкуренты:

- **Mythic / Whisky** — построены на GPTK (Apple) + DXVK + Rosetta. Когда Apple
  выключит Rosetta (2027-2028) — они умрут. Мы — нет, у нас собственный HyperBridge.
- **CrossOver** — commercial, но general-purpose. Мы — focused на productivity,
  AI-driven configuration, premium UX.

Чтобы выжить и **обогнать** конкурентов:

- Архитектурный долг **убийствен** — нельзя оставлять "недоделанные пятна". Каждое
  "потом исправим" становится yet another competitive disadvantage.
- **Performance matters**: пользователь сравнивает MacRunner с native Mac app, не
  с "Wine на Linux". Любая лагающая часть = плохое первое впечатление = no sale.
- **Reliability matters**: enterprise users (AutoCAD, 1С, Navisworks) не терпят
  random crashes. Bug сейчас = lost customer навсегда.

Поэтому **никогда не откладывай качественный fix ради скорости**. Время вложенное
в root-cause solution окупается с лихвой когда тот же класс bug перестаёт
проявляться в следующих 10 apps.

### 6. Применение в практике

Когда обнаруживаешь проблему:

1. **Stop**. Не пиши код пока не понял root cause.
2. **Probe**. Evidence-driven, не догадки (см. patch-by-evidence rule ниже).
3. **Identify family**. Узкая ли это проблема или class? (см. family audit ниже)
4. **Design fix**. Architectural separation, native APIs, complete coverage.
5. **Implement**. С regression tests на класс, не только trigger.
6. **Commit**. Family checklist в commit message обязателен.
7. **Document**. Если fundamental class — entry в Obsidian
   [90-architectural-discoveries-fundamental-bugs.md](file:///Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md)

Если ты ловишь себя на мысли "просто пусть пока работает, потом починим" —
**это antipattern**. Остановись. Сделай правильно сейчас.

---

## 🛑 MANDATORY PROTOCOL: Opcode/API family audit (read first)

**Когда ты собираешься добавить поддержку нового x86_64 opcode, IR op, WinAPI thunk,
или decoder/lifter/interpreter/JIT case — этот блок обязателен. Без него фикс не считается законченным.**

### Шаг 1 — Identify the family BEFORE writing the fix

Любой opcode/API живёт в семействе. Семейство = группа инструкций, объединённых
одним из критериев:

- Same opcode group с разными ModRM /reg (e.g. `C0 /0..7` — ROL/ROR/RCL/RCR/SHL/SHR/SAL/SAR)
- Same opcode, разные operand sizes (e.g. `C0` byte vs `C1` word/dword vs `D1`/`D3` shift-by-1/CL)
- Same semantic class (e.g. all prefetch hints: `0F 0D`, `0F 18`, `0F 1F`)
- Same prefix variants (e.g. `0F 6E` / `66 0F 6E` / `REX.W 0F 6E`)
- Same SSE↔GPR transfer family (e.g. `0F 6E`, `0F 7E`, `66 0F D6`, `F3 0F 7E`)

**Не нашёл семейство за 2 минуты — спроси Intel SDM Vol 2 (table by opcode).
Никогда не "это одиночный opcode, family нет" без проверки.**

### Шаг 2 — Pre-fix checklist (вставлять в commit message)

Перед коммитом opcode fix обязательно заполни:

```
Family: <name, e.g. "C0/C1/D0-D3 byte vs word shift">
Members: <list all opcodes in family>
Trigger: <byte sequence that caused the blocker, e.g. "C0 E0 02 from notepad++.exe">
Coverage in this fix:
  [x] <opcode A> — added decode + interp + JIT + test
  [x] <opcode B> — added decode + interp + JIT + test
  [ ] <opcode C> — NOT applicable because <reason>
Regression tests:
  - <test name 1> — covers <opcode>
  - <test name 2> — covers sibling
Audit completed: yes/no
```

**"Audit completed: no" блокирует коммит.** Если есть причина не покрывать всё семейство
(другой layer, требует JIT поддержки которой нет, и т.д.) — пиши "NOT applicable because ..."
явно для каждого пропущенного sibling.

### Шаг 3 — Regression covers ≥2 siblings, не только trigger

Тест только на конкретный байт, который упал — НЕ ДОСТАТОЧНО. Тест должен покрывать
как минимум:
1. Trigger byte (тот что упал в реальном app)
2. Один соседний sibling в family (чтобы поймать regression если family заденется)
3. Edge case (например, если family имеет imm8 form — проверь 0 и max value)

### Worked example (как должно выглядеть)

Codex поймал `C0 E0 02` (shl al, 2) — UNSUPPORTED.

**❌ Wrong reaction:** add C0 /4, test that one byte, commit, move on.

**✅ Right reaction:**
1. Identify family: byte-shift group = `C0 /0..7` (8 sub-opcodes by /reg field)
2. Note siblings: `D0 /0..7` (shift-by-1 byte), `D2 /0..7` (shift-by-CL byte)
3. Add ALL of `C0`, `D0`, `D2` decode + interp + JIT in one fix
4. Tests: `shl al, 2` (trigger), `shr byte [mem], 1` (D0), `rol bl, cl` (D2)
5. Commit:
```
Add C0/D0/D2 byte-shift family

Family: byte-shift group (8/16-bit GPR or memory by imm8/1/CL)
Members: C0/D0/D2 (byte) — counterparts to existing C1/D1/D3 (word+)
Trigger: C0 E0 02 (shl al, 2) from notepad++.exe MSVC prologue
Coverage:
  [x] C0 /0..7 — all 8 shift ops, byte operand, imm8 count
  [x] D0 /0..7 — all 8 shift ops, byte operand, count=1
  [x] D2 /0..7 — all 8 shift ops, byte operand, count=CL
Tests: interp_x64_shl_al_imm8, interp_x64_shr_mem8_by_1, interp_x64_rol_bl_cl
Audit completed: yes
```

### Known families (use this as the audit baseline)

**Shift/rotate**:
- `C0 /0..7`, `C1 /0..7` — by imm8 (byte / word+)
- `D0 /0..7`, `D1 /0..7` — by 1 (byte / word+)
- `D2 /0..7`, `D3 /0..7` — by CL (byte / word+)

**Prefetch / NOP hints (all should be NOPs in interp)**:
- `0F 0D /0..7` — PREFETCH/PREFETCHW (3DNow)
- `0F 18 /0..7` — PREFETCHNTA/T0/T1/T2 + reserved
- `0F 19..0F 1F` — multi-byte NOPs (MSVC uses heavily for alignment)

**SSE↔GPR transfer (REX.W matters!)**:
- `66 0F 6E` — MOVD xmm←r/m32 (REX.W→MOVQ qword)
- `66 0F 7E` — MOVD r/m32←xmm (REX.W→MOVQ qword) ← *bug #4*
- `66 0F D6` — MOVQ m64←xmm
- `F3 0F 7E` — MOVQ xmm←xmm/m64
- `F2 0F D6` — MOVDQ2Q mm←xmm

**Sign/zero extend**:
- `0F B6`/`0F B7` — MOVZX (byte/word → larger)
- `0F BE`/`0F BF` — MOVSX (byte/word → larger)
- `63` — MOVSXD (dword → qword, REX.W mandatory)

**Bit manipulation** (BMI1/BMI2 — modern x64 apps use heavily):
- `0F BC` — BSF, `0F BD` — BSR
- `F3 0F BC` — TZCNT, `F3 0F BD` — LZCNT
- VEX/BMI: ANDN, BEXTR, BLSI, BLSMSK, BLSR (VEX-prefixed, separate work item)

**ACCESS_MASK / WinAPI**:
- Любой WinAPI с `dwDesiredAccess` — все используют то же generic-mask expansion
- Если bug в одном (CreateFileW) → check others: OpenProcess, RegOpenKeyEx, CreateMutex, etc.

### Когда audit можно legitimately skip

- VEX-prefixed family — отдельный large work item (AVX/BMI), не audit'и без plan'а
- Privileged opcodes (IN/OUT, MSR access) — никогда не должны достигать interpreter
- Family > 30 members (rare) — split на разумные batches, документируй split в commit

### Если audit нашёл ещё bugs

Не комбинируй с trigger fix в один коммит. **Один family = один коммит**, но
**audit'и все family члены до того как fix-ишь.** Если в процессе audit обнаружил, что
3 sibling opcodes тоже missing — это OK, добавь все 3 в этот же коммит, это и есть
правильный family fix.

---

## 🛑 MANDATORY: Patch-by-evidence, никогда не patch-by-suspicion

**Если предыдущий fix не сработал (симптом повторился) — НЕ расширять scope патча "по аналогии".
Делать probe пока не появится evidence для конкретной причины, потом fix ровно туда.**

### Анти-паттерн (запрещён)

```
fix #1 не сработал → симптом тот же
↓
"может это ещё и SETcc, и LOAD, и STORE, и CMOVcc по аналогии"
↓
big patch по 5 классам в одном коммите без evidence
```

**Почему это плохо**:
- Маскирует настоящий bug — что-то "починится" но не узнаешь что именно
- Регресс risk растёт квадратично с количеством классов в одном коммите
- Нарушает Family-audit protocol — `Coverage` checklist не может честно подтвердить каждый class
- Если симптом не уйдёт — потерял способность изолировать причину
- Если уйдёт — не знаешь какой из 5 классов был реальный, не сможешь объяснить retroactively

### Правильный паттерн

```
fix #1 не сработал → симптом повторился
↓
ВОПРОС: почему fix не сработал?
  - Активен ли этот code path? (interp vs JIT, лог branch instrumentation)
  - Probe state в точке incident (RFLAGS, registers, memory)
  - Сравни fix expectation vs реальное поведение
↓
EVIDENCE: один конкретный класс/инструкция/branch
↓
fix именно туда, один class = один коммит
```

### Когда расширять scope — это OK

- **Family audit** для нового opcode (Family Protocol выше) — это **proactive**, на основе Intel SDM table, не "по аналогии с похожим"
- **Sibling found во время probe** — если probe показал что не только NEG, но и DEC тоже корраптит state — это evidence, не аналогия
- **Класс-level mechanism** (lazy flags, partial-register helper) — если evidence показывает что **mechanism** broken, и mechanism shared между N opcodes, то fix mechanism закрывает все N. Но evidence для самого mechanism обязателен.

### Diagnostic before fix — checklist

Перед любым patch когда предыдущий не сработал, отвечай в commit message:

```
Previous fix: <hash> <class>
Why it didn't work: <probe output / evidence>
This fix targets: <specific class with evidence>
Why this class: <which probe line proves it>
Not extending to: <list classes deliberately NOT touched, why>
```

Если "Why it didn't work" пустое — **не делай fix**. Сначала probe.

### Worked example

Bug #4 (MOVQ) — partial-register write fix landed.
Notepad++ run → fastfail повторился с тем же RCX=5.

**❌ Wrong**: "может это ещё SETcc, LOAD, STORE, addr32 — fix всё разом по аналогии с MOVQ"

**✅ Right**:
1. Probe: добавь instrumentation "block at PC X was executed via JIT or interpreter?"
2. Запусти, посмотри: блок с `neg al; sbb` идёт через interpreter, не JIT
3. Evidence: JIT fix не активирован для этого path
4. Conclusion: настоящий bug в **interpreter** semantics, не JIT
5. Fix: один class — interpreter NEG flag computation, с evidence в commit

---

## 🛑 MANDATORY: Root-cause known → apply it. НЕ перебирай band-aids

Если audit/probe **уже дал root cause** (file:line + механизм) — применяй **именно
его**. Запрещено перебирать speculative варианты ("попробую placeholder, нет —
попробую invert, нет — попробую fallback").

### Anti-thrashing (реальный случай — стоил 2.5 часа + регресс)

Notepad++ icons: Кими audit дал точный root — StretchDIBits 4bpp/8bpp→32bpp fails
на ARM64 (cursoricon.c:879). Вместо применения, агент попробовал 12 догадок:
mono-icon, mono-icon-invert, transparent-placeholder, copyimage-fallback, uxtheme...
→ **регрессировал работавшие toolbar иконки** + потерял 2.5 часа.

Правильно было: применить StretchDIBits conversion fix ОДИН раз.

### Правило

- Root cause известен (из audit/probe) → fix именно туда. Один proper fix.
- Speculative band-aid (placeholder/invert/fallback/hack) — **запрещён** если root известен.
- Если fix регрессировал working code → **немедленно revert**, не накапливай hacks поверх.
- Каждый "попробую другой подход" без нового evidence = thrashing. Stop, вернись к root cause.
- Band-aid допустим ТОЛЬКО как explicit temporary с `TODO: workaround, real fix = <root>`,
  и только если proper fix реально блокирован (rare).

Это усиление Zeroth Principle + patch-by-evidence: **известный root → proper fix,
не 12 догадок**.

### Revert тоже patch-by-evidence — не откатывай соседнюю семью (реальный случай)

Откат band-aids — это правка, а не «уборка». Тот же риск: revert может зацепить
**соседнюю** семью, у которой свой корень.

Случай (icons, 2026-05-21): агент откатывал icon band-aids и заодно удалил из
`comctl32/imagelist.c` фоллбэк `add_with_alpha` (zero/opaque-alpha + mono-mask →
классический image+mask путь). Этот guard был **ROOT-FIX для семьи иконок тулбара**
(32bpp BMP с нулевым alpha), а не band-aid. Реверт → иконки тулбара снова чёрные.
Корень тулбара (comctl32 BMP) ≠ корень папок (cursoricon ICO 4/8bpp) — две разные
дорожки спутали в одну.

Правило перед revert:
- Для каждого удаляемого guard спроси: **какую семью он защищает?** Если его
  комментарий дословно описывает реальный симптом — это, скорее всего, ROOT-FIX,
  ошибочно принятый за band-aid. НЕ удаляй вслепую.
- Один аудит = один корень для ОДНОЙ семьи. Не распространяй его revert/fix на
  другую семью без отдельного evidence (см. family audit).
- Сначала `git diff engine/<file>` — увидь что именно удаляешь, прочитай комментарии
  удаляемого кода. Если сомнение — замер (trace) до revert, не после.

---

## 🛑 MANDATORY: Batch diagnostics — один проход, не 1000 итераций

**Запрещено** чинить по одному symptom → rebuild → run → next → repeat. Это
сжигает часы. Каждый rebuild = минуты; 50 итераций = потерянный день.

### Правило single-pass

Когда диагностируешь app blocker(ы):
1. Включи **ВСЕ** relevant traces сразу (opcode faults, GetSysColor, GDI, font/glyph,
   ImageList, shell32, comctl32, menu IDs, dialog creation — что относится).
2. Подними **ВСЕ limits** (block/step) до "никогда не достигнуть при legit work"
   (10M+). НЕ increment по чуть-чуть — сразу высоко. Безопасность через step-limit.
3. **Один long run** (60-120s) собирает полную картину всех failures.
4. Классифицируй все failures в **families**.
5. Batch-fix каждый family → **один** rebuild → verify.

Думай как доктор заказывающий **полный анализ крови один раз**, не по тесту в неделю.

### Запрещённый anti-pattern

❌ "found one opcode → fix → rebuild → found next → fix → rebuild" (×50)
❌ Гонять тот же smoke harness по кругу когда он уже PASS
❌ Raise limit by small increments

---

## 🛑 MANDATORY: Functional PASS ≠ Visual PASS — раздельные gates

App "works" имеет **два независимых уровня**. Не путай и не закрывай app пока
оба не green:

### Functional gate (программно проверяемо)
- Window HWND exists, visible, enabled
- Editor/controls accept input, round-trip данные
- Menu command IDs резолвятся, dispatch actions
- Dialogs создаются (#32770 HWND appears)
- File ops (save/open) пишут/читают disk
- Clean process exit

→ Проверяется smoke harness (Win32 messages, HWND enumeration, не screenshots).

### Visual gate (выглядит как на Windows)
- Window control buttons (min/max/close) — настоящие glyphs, не пустые квадраты
- Tabs, toolbar icons — настоящие colored icons, не gray/black placeholders
- System colors correct (не black buttons/bands) — GetSysColor
- Folder/file icons в dialogs — настоящие shell icons, не blank
- Scrollbars styled, не артефакты
- Fonts match Windows baseline (metrics + glyphs)

→ Проверяется Visual Regression Lab + screenshot diff vs Windows baseline.

**Functional PASS НЕ значит app готов.** Если smoke green но кнопки чёрные/
иконки серые — это checkpoint, не closure. И НЕ гоняй functional smoke снова
если он уже PASS — переключайся на visual gate.

---

## 🛑 MANDATORY: Auto-continue — не останавливайся между подзадачами

Когда задача дана с master brief / closure checklist:
- Закрыл один item → **сразу продолжай следующий** по списку. НЕ останавливайся
  и не жди подтверждения пользователя между шагами.
- Останавливайся только: (1) TRUE blocker требующий решения, (2) весь checklist
  green, (3) обнаружил что-то требующее strategic decision.
- Reporting — пиши progress, но не блокируйся ожидая ответа на каждый шаг.

Пользователь дал mega-brief = мандат идти по всему списку автономно.

---

## 🛑 MANDATORY: Engine memory — git diff + change journal (НЕ перечитывай код, помни)

**Проблема которую это решает**: `engine/` в `.gitignore` (12GB). Раньше агент
не мог увидеть свои же прошлые правки → писал "сверяю реальные фрагменты",
перечитывал код, терял контекст после compaction, не отличал band-aid от root-fix.

**Решение — два механизма памяти движка, оба обязательны:**

### 1. Key engine source files force-tracked в git
Несмотря на `engine/` в gitignore, активно редактируемые файлы движка
**принудительно отслеживаются** (`git add -f`): `engine/hyperbridge/src/*.c`,
`engine/hyperbridge/include/*.h`, `engine/hyperbridge/tests/hb_test_runner.c`,
и активные Wine DLL: `user32/cursoricon.c`, `comctl32/{imagelist,tab}.c`,
`win32u/{dib,defwnd,font,sysparams}.c`, `ntdll/unix/{macrunner_hb,virtual,signal_arm64}.c`.

Поэтому:
- **`git diff engine/<file>`** = увидеть СВОИ изменения, не перечитывая весь файл.
  Это твоя память о том ЧТО ты менял. Используй ВМЕСТО "сверяю фрагменты".
- **`git log --oneline -- engine/<file>`** = история правок этого файла.
- **`git checkout engine/<file>`** = чисто откатить band-aids БЕЗ потери root-fix
  (откатывает к последнему коммиту; коммить root-fix отдельно от экспериментов).
- Новый редактируемый файл движка → сразу `git add -f` чтобы он попал в трекинг.

### 2. docs/ENGINE-CHANGE-JOURNAL.md — append-only журнал (ЗАЧЕМ менял)
Каждая правка движка ОБЯЗАНА быть записана в `docs/ENGINE-CHANGE-JOURNAL.md`:
```
## YYYY-MM-DD HH:MM — короткий заголовок
File(s): engine/path:line
Type: ROOT-FIX | REVERT | DIAGNOSTIC | WORKAROUND(TODO)
What: что изменено в одну строку
Why: гипотеза/evidence/root cause
Verify: как проверено (тест, trace, smoke)
Status: applied | reverted | needs-verify
```
git diff = ЧТО изменилось (механически). Журнал = ЗАЧЕМ (намерение, evidence,
статус). Вместе они переживают context compaction и дают полную память.

### Правило
- Перед тем как трогать движок: `git diff engine/<file>` + прочитай последние
  записи журнала по этому файлу. Это восстанавливает контекст за секунды.
- После каждой правки: добавь запись в журнал. WORKAROUND помечай `TODO` —
  потом заменяется на ROOT-FIX, старая запись → Status: reverted.
- Это работает ВМЕСТЕ с context-mode hooks (PreCompact). Hooks спасают session
  state; git+журнал — постоянная память движка через любой wipe.

---

## GUI rendering bug catalog (reuse для всех apps)

Когда GUI app выглядит неправильно — это известные классы. Проверяй сразу все
relevant за один trace pass (не открывай каждый отдельно):

| Симптом | Вероятный root cause | Где смотреть |
|---|---|---|
| Window min/max/close = пустые квадраты | **Marlett font** не загружен | win32u NC paint + font load; Windows рисует эти glyphs шрифтом Marlett (chars 0/1/2/r) |
| Чёрные кнопки/bands/контролы, текст invisible | **GetSysColor** возвращает black (0) | win32u/sysparams system color table init |
| Серые/blank/чёрные **toolbar** icons | comctl32 BMP imagelist (32bpp zero-alpha + mono-mask) — **ОТДЕЛЬНАЯ дорожка от dialog/folder ICO** | comctl32/imagelist.c `add_with_alpha` (zero/opaque-alpha+mask → image+mask путь), ImageList_Draw |
| Чёрные/blank **folder/dialog** icons | user32 ICO 4/8bpp indexed → 32bpp conversion | user32/cursoricon.c SetDIBits palette expansion (НЕ путать с toolbar BMP путём) |
| Чёрные квадраты на tabs | comctl32 tab owner-draw / close-icon imagelist | comctl32/tab.c paint |
| Blank folder/drive icons в dialogs | shell32 system image list | shell32 SHGetFileInfo / SHGetImageList / iconcache |
| Scrollbar артефакты | comctl32 scrollbar / COLOR_SCROLLBAR | comctl32 scrollbar paint + GetSysColor |
| Fonts косые/wrong metrics | FreeType integration / GetTextMetrics | winemac.drv font, GetTextMetricsW vs Windows baseline |
| Capture shows no-color но visible OK | GDI DIB capture path != macOS surface | toolbar_render capture method (icons render в surface не в DIB) |

Закрытие одного класса (напр. GetSysColor) часто чинит **много** симптомов сразу.
После каждого app — добавляй новые найденные классы сюда.

---

## Первое действие в каждой сессии

```bash
cd /Volumes/MacOS/MacRunner
. ./config/env.sh
pwd      # должно быть /Volumes/MacOS/MacRunner
```

Если `pwd` показывает `/Users/timurtoby/Documents/MacRunner` — это **Obsidian vault, не код**.
Никогда не запускай build/test/wine команды оттуда. Стратегические доки — да, код — нет.

## Канонические пути (не выдумывай альтернативы)

| Что | Путь |
|---|---|
| Repo root | `/Volumes/MacOS/MacRunner` |
| Wine dist (ARM64-native, HyperBridge lane) | `engine/wine/dist-pure-arm64/` |
| Wine build (ARM64-native) | `engine/wine/build-pure-arm64/` |
| Wine dist (legacy / non-HB) | `engine/wine/dist/` |
| HyperBridge source | `engine/hyperbridge/` |
| Notepad++ x64 payload | `artifacts/phase-h/npp-x64/notepad++.exe` |
| Notepad++ x64 cwd (mandatory) | `artifacts/phase-h/npp-x64/` |
| Reports / logs | `reports/phase-h/` |
| Obsidian vault (DOCS ONLY, NOT code) | `/Users/timurtoby/Documents/MacRunner/` |

Все доступны как exported env vars после `. ./config/env.sh`:
`$MACRUNNER_ROOT`, `$MACRUNNER_WINE_DIST_ARM64`, `$MACRUNNER_NPP_X64_APP`, `$MACRUNNER_NPP_X64_DIR`,
`$MACRUNNER_REPORTS_ROOT`, `$MACRUNNER_ARTIFACTS_ROOT`.

## Канонические скрипты (используй эти, не изобретай свои)

| Задача | Команда |
|---|---|
| Запуск Notepad++ x64 (bounded 18s) | `./scripts/run-notepad-x64.sh` |
| Notepad++ x64 (hold alive) | `./scripts/run-notepad-x64.sh --hold` |
| Notepad++ с trace | `./scripts/run-notepad-x64.sh --trace=fileinfo,abi` |
| Сборка HyperBridge | `./scripts/build-hyperbridge.sh` |
| Сборка Wine pure-arm64 | `./scripts/build-wine-pure-arm64-experiment.sh` |
| Relink только ntdll.so | `cd $MACRUNNER_WINE_BUILD_ARM64 && rm -f dlls/ntdll/ntdll.so && make dlls/ntdll/ntdll.so install && codesign --force --sign - $MACRUNNER_WINE_DIST_ARM64/lib/wine/aarch64-unix/ntdll.so` |
| Repair WindowMetrics | `./scripts/repair-wine-window-metrics.sh --prefix $PREFIX --fix` |
| Wine cleanup (mandatory before/after) | `./scripts/kill-wine-tree.sh` или см. ниже |
| HyperBridge tests | `./scripts/test-hyperbridge.sh` |

## Process hygiene (обязательно)

Перед каждым запуском Wine **и** после bounded run:

```bash
pkill -9 -f '[w]inetemp-|[w]ine-preloader|[w]inedbg|[n]otepad\+\+\.exe|[s]ervices\.exe|[r]pcss\.exe|[e]xplorer\.exe|[w]inedevice\.exe|[p]lugplay\.exe|[s]vchost\.exe|[w]ineserver'
sleep 1
# verify zero:
ps -axo comm= | awk '/^(wine-preloader|wine64-preloader|winedbg|wineserver)$/{c++} END{print c+0}'
```

Если не сделать — Dock зарастает старыми debugger/exec окнами, юзер ругается.

## Known traps (не повторяй)

- **`WINEDEBUG=+module` даёт 17MB логов** за секунды и съедает trace budget. Используй `+file`,
  или `-all` + наши собственные `MACRUNNER_HB_TRACE_*` env vars (gated fprintf в stderr).
- **`/Users/timurtoby/Documents/MacRunner/` — это Obsidian, не код**. Если запустил `pwd` там —
  немедленно `cd $MACRUNNER_ROOT`.
- **`python3` (Homebrew) НЕ имеет модуля Quartz**. Для CGWindowList probes используй `/usr/bin/python3`
  (system) или `swift tools/cg_window_probe.swift`.
- **`WINEDEBUG=-all` подавляет ERR_(...)**. Наши собственные трассы должны быть `fprintf(stderr, ...)`
  с env-gating (`if (getenv("MACRUNNER_HB_TRACE_X"))`), а не через Wine debug channels.
- **При изменении HyperBridge сорсов обязательно `rm -f dlls/ntdll/ntdll.so` перед `make`**.
  make не видит `libhyperbridge.a` как dep, не пересоберёт ntdll автоматически. Force relink.
- **`status` — reserved variable в zsh**. Не используй как имя локальной переменной в скриптах
  под zsh — поведение неочевидное.
- **Pre-launch sanity check бинарников** в начале сессии (один раз):
  ```bash
  file $MACRUNNER_WINE_DIST_ARM64/bin/wine
  file $MACRUNNER_WINE_DIST_ARM64/lib/wine/aarch64-unix/ntdll.so
  ```
  Оба должны быть `Mach-O 64-bit ... arm64`. Если выходит `ASCII text` — диагностические скрипты
  ранее перезаписали бинарь логом, recover из build dir.
- **Notepad++ требует cwd = payload dir**. Без `cd artifacts/phase-h/npp-x64` — silent exit
  (cwd-local resources: langs.xml, stylers.xml, doLocalConf.xml).
- **FreeType invisible to Wine without DYLD path**. Helper script уже ставит
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib:...`. Не убирай.
- **WindowMetrics corruption recurring** (CaptionHeight=2049 → 6x58 sliver window).
  win32u уже имеет clamp, но всё равно прогоняй `repair-wine-window-metrics.sh --fix` после wineboot.
  Порядок важен: boot → wait for user.reg → wineserver -w → repair → launch.
- **Background процесс с `&` и parent shell exit = SIGHUP**. Если запускаешь Wine в background
  и shell завершается до 20s sleep, процесс умирает silently. Держи shell живым или используй `nohup`.
- **`./scripts/run-notepad-x64.sh --hold &` в diagnostic script = SIGHUP**. `--hold` ждёт Ctrl-C,
  но если родительский shell не tty и завершается, Wine умирает. Правило: `--hold` только для
  **foreground interactive** проб (ты сидишь у терминала, ждёшь окно). Для diagnostic loops
  используй **default bounded mode** (без `--hold`) — wrapper сам ждёт 18s, печатает key-lines,
  убирает хвосты.
- **Wrapper не принимает `--run-dir`** (по состоянию на текущий момент). Любые unknown args
  попадают в `EXTRA_ARGS` и передаются в notepad++.exe как command-line args, что может
  silently сломать поведение. Если нужно фиксированное RUN-dir — extend wrapper, не передавай
  через unknown flag.
- **Stale `libhyperbridge.a` после source change**. Если меняешь
  `engine/hyperbridge/src/*.c`, **make ntdll.so может НЕ пересобрать** статическую библиотеку
  автоматически — она остаётся stale. Symptom: source изменён, runtime behavior без изменений.
  Mandatory steps when changing HyperBridge source:
  ```bash
  cd engine/hyperbridge
  rm -f src/{changed_file}.o libhyperbridge.a
  make -j4 libhyperbridge.a
  cd ../wine/build-pure-arm64
  rm -f dlls/ntdll/ntdll.so
  make -j4 dlls/ntdll/ntdll.so install
  codesign --force --sign - dist-pure-arm64/lib/wine/aarch64-unix/ntdll.so
  ```
  **Verify timestamps**: `libhyperbridge.a` должен быть свежее изменённого source.
  Эта trap нашли через debugging Heisenbug — source changed for hours, behavior unchanged,
  because static archive was stale.

## Architectural bugs reference

Найденные fundamental bugs (class-level, retroactively explain прежние симптомы):
см. [Obsidian / 90-architectural-discoveries-fundamental-bugs.md](file:///Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md)

1. RIP-relative + trailing immediate (decoder semantics, ~12 opcode class)
2. x18 sigreturn restoration (macOS Apple ARM64 platform ABI)
3. Basic block PC fall-through (interpreter execution model)
4. REX.W ignored on 0F 7E — MOVD vs MOVQ (MSVC CRT argument pack via SSE)

При новых "strange register value" симптомах — first check эти. Особенно #4 для нулевых
аргументов через WinAPI.

## Methodology rules

1. **Validate premise before fix.** Если думаешь "X сломан" — сначала trace доказательство,
   потом patch. Patch-by-symptom = workaround, не fix.
2. **Class-level audit после finding class bug.** Нашёл `REX.W` ignored на 0F 7E → audit
   всю SSE↔GPR transfer family (0F 6E, 0F 7E, 66 0F D6, F3 0F 7E, и т.д.) на ту же ошибку.
   Один час audit'а спасает 10 часов реактивного debug.
3. **Real app testing > synthetic unit tests** для finding architectural gaps. Unit tests
   verify known correctness; real apps expose unknown gaps.
4. **Live-memory probe ("рентген-trace") вместо guessing.** Печатай память pack-struct,
   xmm0, GPR до/после подозрительной инструкции через `mach_vm_read_overwrite`.
   Mechanical bisection между правильным и неправильным состоянием изолирует bug точно.

## Когда обновлять этот файл

- Новый канонический путь добавлен → обнови табличку
- Новый класс trap встречен → добавь в "Known traps"
- Новый fundamental bug найден → добавь номер в "Architectural bugs reference"

Этот файл переживает context compaction. Stale entries хуже чем missing — поддерживай актуальность.
