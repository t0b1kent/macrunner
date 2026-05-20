# Codex MEGA Directive v2 — Batch Diagnostics + Notepad++ Full Functional

**Supersedes**: CODEX-DIRECTIVE-notepad-plus-plus-full-functional.md (closure criteria still valid, this adds METHOD)

**Core mandate**: Довести Notepad++ x64 до **полностью рабочего идеального** состояния. Сколько надо — неделя, месяц. Quality > speed. NO KeePass until all green.

**Critical method change**: Перестань чинить по одному и переделывать тысячу раз. **Batch-trace ВСЁ за один проход**, найди root cause классы, fix families.

---

## 1. METHODOLOGY — как НЕ повторять ошибки (read carefully)

### Anti-pattern которого ты делал (stop)

❌ Fix one opcode → rebuild → run → find next opcode → rebuild → run → ... (1000 iterations)
❌ Fix one visual bug → check → find next visual bug → fix → check → ...
❌ Hit block-limit → guess → raise a little → still hit → raise more → ...
❌ Trace one symptom at a time

Это **slow и wasteful**. Каждый rebuild = minutes. 50 iterations = hours wasted.

### Correct method — single-pass batch diagnostics

✅ **Maximize trace coverage в ОДНОМ run.** Когда запускаешь Notepad++ — включи **ВСЕ** relevant traces сразу:
- Block/step limits **высокие** (10M+) чтобы не упереться в cap mid-trace
- ALL trace categories on: opcode faults, GetSysColor, ImageList, GDI fills, font metrics, menu IDs, dialog creation
- Один long run (60-120s) собирает **полную картину** всех failures

✅ **Collect ALL failures, THEN classify into families.** После одного comprehensive run:
- List every unsupported opcode → group into families → fix all families в одном commit
- List every wrong GetSysColor → fix color table once
- List every black-rendered control → trace common paint path
- List every missing icon → trace ImageList path

✅ **Raise limits to "never hit during legitimate work".** Если block-limit рубит — ставь сразу 10000000 (10M) или unlimited для x64-signal-callback. НЕ raise by small increments. Безопасность через step-limit, не block-limit.

✅ **One trace harness, many probe points.** Build ONE instrumentation pass что logs:
- Every GetSysColor(index) → returned value
- Every ImageList_Draw / icon load → success/fail
- Every FillRect / BitBlt color
- Every GetTextMetrics result
- Every unsupported opcode bytes+pc
- Every menu command ID resolution
- Every dialog HWND creation

Run once → grep different categories → fix all classes.

### Example — как надо было с иконками

**Wrong** (что ты делал): noticed gray icons → investigate icons only → fix → notice black buttons → investigate separately → fix → notice scroll → ...

**Right**: ОДИН run с visual trace:
```
MACRUNNER_HB_TRACE_GDI=1          # all GDI ops
MACRUNNER_HB_TRACE_SYSCOLOR=1     # GetSysColor returns
MACRUNNER_HB_TRACE_IMAGELIST=1    # icon loads
MACRUNNER_HB_TRACE_FONT=1         # text metrics
MACRUNNER_HB_X64_BLOCK_LIMIT=10000000
```
→ один stderr.log содержит ВСЕ visual rendering data
→ grep "syscolor" → see all black returns → fix color table (one fix, fixes buttons+bands+scroll)
→ grep "imagelist" → see icon load failures → fix resource path (one fix, all icons)
→ grep "font" → see metric mismatches → fix font layer

**Three fixes, one run, fixes ~15 symptoms.** Не 15 iterations.

---

## 2. CURRENT VISUAL BUG — single root cause hypothesis

From screenshots, the pattern is clear and pointing к **ONE root cause**:

**Symptoms** (all observed):
- Save As dialog: Save/Cancel buttons render as **black rectangles**, text invisible
- Print dialog: OK button **black rectangle**
- Toolbar: black bands, gray icon placeholders
- Scroll regions: black

**Hypothesis**: `GetSysColor()` returns **black (0x000000)** для system color indices instead of proper values:
- COLOR_BTNFACE (15) should be ~0xC0C0C0 / 0xECECEC
- COLOR_BTNTEXT (18) should be 0x000000 (black text) — but на black face = invisible
- COLOR_3DFACE, COLOR_WINDOW, COLOR_WINDOWTEXT, COLOR_3DSHADOW, COLOR_3DHIGHLIGHT

If system color table uninitialized или resolves к 0 → every control paints black, text invisible.

**Single fix** likely resolves: black buttons + black bands + scroll backgrounds. Icons separate (ImageList).

**Verify first** (per patch-by-evidence):
```c
// Trace GetSysColor в win32u/sysparams.c или wherever resolved:
fprintf(stderr, "macrunner-syscolor: index=%d returned=0x%06x\n", index, color);
```
Run Notepad++ → grep "macrunner-syscolor" → see if COLOR_BTNFACE etc return 0.

If confirmed → fix system color initialization (default Windows palette). This is **one fix, many symptoms** — exactly the batch method.

---

## 3. CLOSE Kimi's engine/notepad-relevant findings

Kimi audit found real bugs. Close the **engine/Notepad-relevant** ones (skip pure cosmetic если не affect functional):

### From bootstrap telemetry (#33) — HB fault breakdown:
```
8x bytes=cd pc=0x1403eba17 result=UNSUPPORTED_OPCODE  → INT family (fastfail path)
1x bytes=0f0d pc=0x140408902                          → prefetch/NOP-hint family
1x bytes=c0 pc=0x1401db930                            → byte shift/rotate family
1x bytes=fe pc=0x14040d093                            → INC/DEC r/m8 family
1x bytes=660f7e pc=0x140417e10                        → SSE-to-GPR transfer family
```

**Batch-fix all these opcode families в одном pass** — they're все documented. Family audit each per AGENTS.md, fix together.

### From dim 06+08 (COM apartment):
- 5 INFINITE wait sites в combase/ — **verify if they affect clean exit** (current "clean bounded exit not certified" issue)
- If clean exit hangs → these are likely culprit → bounded timeout fix
- If clean exit fine → leave as documented defensive issue (lower priority)

### Native faults:
```
pc=0x7ffd0af30d4 fault address 0x6d6974ff0000082f
```
- Cluster — investigate if affects Notepad++ specific path

**Method**: ONE comprehensive run с MACRUNNER_HB_TRACE_FAULTS=1 + high limits → collect ALL faults → classify → batch-fix families.

---

## 4. Full functional closure (from v1 directive — still binding)

All criteria из CODEX-DIRECTIVE-notepad-plus-plus-full-functional.md:
- Core editor (text, find/replace, fold, etc.)
- All 12 menus working
- All dialogs (Open/Save/Find/Preferences) functional
- Toolbar icons real (not gray)
- Font matching Windows baseline
- File operations (save/open/multi-tab)
- Keyboard shortcuts (Cmd translated)
- Stability (30-min, no crash, clean exit)
- Visual polish (no black artifacts)

**Method per criterion**: batch-trace category, classify, family-fix, regression test, advance.

---

## 5. Leverage Kimi's Visual Regression Lab

Kimi built `Visual Regression Lab v1 — automated black-band / UI artifact detection` (commit 9dc63c7).

**USE IT.** Don't manually screenshot-compare:
- Run Visual Regression Lab → automated detection of black bands, missing icons, artifacts
- It gives objective PASS/FAIL per visual element
- Saves manual screenshot review
- Coordinate с Kimi if lab needs extensions for new artifact types

Kimi also built `AI Configurator MVP v0.1 — unified orchestrator for Codex task routing` (191ccf1) + `CODEX-NEXT-TASK.md`. Read `reports/ai-configurator/CODEX-NEXT-TASK.md` — Kimi may have already routed specific tasks to you.

---

## 6. Build/test discipline

### ccache — USE IT (currently 0% hits)
```bash
# Verify config in config/env.sh sourced:
. config/env.sh
echo $CCACHE_DIR    # should be /Volumes/MacOS/MacRunner/artifacts/ccache
ccache -s           # check hit rate

# Builds должны go through ccache wrappers:
./scripts/build-hyperbridge.sh   # uses ccache clang
```
0% hit rate means builds не используют ccache — fix this, saves 5-10x rebuild time. Critical для batch method (fewer rebuilds но faster ones).

### Stale artifact traps (per AGENTS.md known traps)
- After HyperBridge source change: `rm libhyperbridge.a` + rebuild + force ntdll relink
- After enum change in headers: `rm src/*.o` full rebuild
- Verify timestamps: built artifact newer than source

### Tests
- Each family fix → regression test
- Full suite green before advancing
- Watch stale .a trap

---

## 7. Reporting

Each session к user:
```
Phase H batch session YYYY-MM-DD:
Run: [single comprehensive trace run dir]
Failures found this run: [N classified into M families]
Fixed this session: [families closed]
Closure: X/80 criteria green
Next batch: [what next comprehensive run targets]
```

Obsidian: architectural classes → 90-architectural-discoveries-fundamental-bugs.md

---

## 8. The single most important behavioral change

**Before**: probe one symptom → fix → rebuild → probe next → repeat 50x = hours
**After**: ONE comprehensive trace run (all categories, high limits) → classify all failures → batch-fix families → ONE rebuild → verify → advance

Think like a **doctor ordering full bloodwork once** vs ordering one test at a time over weeks. Get the complete diagnostic picture in one pass, then treat systematically.

When you hit ANY limit (block, step, budget) — raise it to "comfortably never hit during legit work" immediately. Don't increment. The goal is **complete data per run**, not minimal-change-per-iteration.

---

## 9. Stop criteria

Phase H closes when:
- All 80 closure criteria green с automated evidence (Kimi's Visual Regression Lab + smoke harness)
- 3 consecutive clean smoke runs
- 30-min manual heavy-use no crash
- Clean exit certified
- Visual: no black artifacts, real icons, font matches baseline
- User signs off
- Tag: v0.2-engine-phase-h-x64-notepad-plus-plus-production

Then Phase I (KeePass 1.43).

---

Поехали. Batch-trace. Family-fix. Raise limits decisively. Use Kimi's Visual Lab. Quality > speed. Один проход вместо тысячи.
