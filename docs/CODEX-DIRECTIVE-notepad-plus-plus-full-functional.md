# Codex Directive — Notepad++ x64 FULL FUNCTIONAL Closure

**Target**: Notepad++ x64 на MacRunner должен выглядеть и работать **идеально** — indistinguishable от native Notepad++ на Windows, кроме того что обёрнут в macOS chrome.

**Duration**: Сколько потребуется. Неделя. Месяц. Два месяца. **Quality > speed.**

**No move to KeePass / Phase I until all closure criteria met.**

**Per AGENTS.md Zeroth Principle**: native, root-cause, seamless. Никаких "good enough" workarounds.

---

## 1. Definition of "full functional" — закрытие критерии

Все эти пункты должны быть **green с automated evidence**, не "looks ok на скрине":

### Core editor (Scintilla)
- [ ] Text input: type 1000 chars rapidly, no lag, no missed keystrokes
- [ ] UTF-8 / Cyrillic / Chinese / emoji rendering correctly
- [ ] Multi-line edits, paragraph wrap, undo/redo
- [ ] Selection (mouse drag, keyboard shift), copy/cut/paste
- [ ] Find/Replace dialog opens, finds text, replaces correctly
- [ ] Search in files (multi-file find)
- [ ] Goto line dialog
- [ ] Word wrap toggle
- [ ] Line numbers gutter aligned correctly
- [ ] Fold/unfold code blocks
- [ ] Auto-indent
- [ ] Bookmark add/remove/navigate
- [ ] Column edit mode (Alt+drag)
- [ ] Macros record/playback

### Menus (all 12 top-level)
- [ ] File menu: New, Open, Open Recent, Save, Save As, Save All, Close, Close All, Print, Exit — **all work**
- [ ] Edit menu: Undo/Redo, Cut/Copy/Paste/Select All, Find/Replace, Goto Line, Column Mode, Comment/Uncomment, всё работает
- [ ] Search menu: Find, Find Next/Prev, Replace, Find in Files, Mark, Goto Line, Goto Bracket
- [ ] View menu: Toggle Sidebar, Fold All, Unfold All, Show Symbol, Zoom In/Out, Full Screen
- [ ] Encoding menu: UTF-8, UTF-8 BOM, ANSI, конвертация между encodings
- [ ] Language menu: 80+ syntax highlighters — все selectable, syntax работает
- [ ] Settings menu: Preferences dialog opens, all tabs работают, settings persist
- [ ] Tools menu: MD5/SHA hash, ASCII Art, etc.
- [ ] Macro menu: Record, Playback, Save, Run Multiple
- [ ] Run menu: Run command, custom commands
- [ ] Plugins menu (даже если plugins disabled — menu present)
- [ ] Window menu: list of open documents, switch between
- [ ] ? menu: About, Help, Update Check

### Dialogs
- [ ] File Open dialog: shows native macOS NSOpenPanel (или Win32 если decided), files load correctly
- [ ] File Save As dialog: filename input, extension filter, saves to disk
- [ ] Find dialog: text input, options checkboxes (case, whole word, regex), finds in document
- [ ] Replace dialog: find + replace, replace all, count occurrences
- [ ] Preferences dialog: tabs (General, Editing, New Document, Default Directory, Recent Files, File Association, Language, Highlighting, Print, Search Engine, MISC, Backup, Auto-Completion, Multi-Instance & Date, Delimiter, Cloud & Link, Searching, Performance, Dark Mode) — все tabs open, controls работают, settings save
- [ ] Goto Line: dialog opens, number input, jumps к line
- [ ] About Notepad++: shows version, author, license

### Toolbar
- [ ] All toolbar icons render как real colored icons (не gray placeholders)
- [ ] Icons proper resolution (HiDPI на retina display)
- [ ] Hover tooltips appear (Win32 tooltip mechanism)
- [ ] Click execute corresponding action
- [ ] Toolbar customization (right-click, hide/show buttons)
- [ ] Toolbar style options (small/large icons, standard/fluent UI)

### Font rendering
- [ ] Default font matches Windows Notepad++ baseline
- [ ] Font metrics correct (ASCENT, DESCENT, LINEGAP, line height)
- [ ] Glyph rendering crisp (не fuzzy, не aliased badly)
- [ ] Cleartype / antialiasing matches Windows
- [ ] Different fonts selectable (Settings → Style Configurator → font)
- [ ] Font size changeable, applies immediately
- [ ] Bold/italic/underline render correctly
- [ ] Unicode glyph coverage (CJK, RTL languages if applicable)

### File operations
- [ ] Create new file: empty document, can type, save с newpath
- [ ] Open file: existing file loads, content shown, encoding detected
- [ ] Save file: writes к disk, can re-open
- [ ] Save As: choose new location, file created there
- [ ] Open large file (10MB+): loads без freeze, scrollable
- [ ] Open binary file: handles gracefully (warning or hex view)
- [ ] Multi-file tabs: open 5+ files, switch between с tabs
- [ ] Tab reordering (drag)
- [ ] Tab context menu (close, close others, close all)
- [ ] Drag-and-drop file onto Notepad++ icon → opens

### Window behavior
- [ ] Window resize (drag corners, max/min buttons)
- [ ] Cmd+W closes current document tab
- [ ] Cmd+Q quits application cleanly (no force kill)
- [ ] Cmd+M minimize
- [ ] Cmd+H hide (macOS standard)
- [ ] Cmd+~ switch windows (multiple Notepad++ instances if applicable)
- [ ] Full screen mode (proper macOS Cmd+Ctrl+F)
- [ ] Restore window position after quit/relaunch

### Keyboard shortcuts
- [ ] Cmd+N (translate Ctrl+N) — new file
- [ ] Cmd+O — open file
- [ ] Cmd+S — save
- [ ] Cmd+Shift+S — save as
- [ ] Cmd+F — find
- [ ] Cmd+H — replace (Win32 Ctrl+H translated)
- [ ] Cmd+G — find next
- [ ] Cmd+Z / Cmd+Shift+Z — undo / redo
- [ ] Cmd+X/C/V/A — cut/copy/paste/select all
- [ ] Cmd+W — close tab
- [ ] Cmd+Q — quit
- [ ] Cmd+T — new tab
- [ ] Cmd+P — print
- [ ] Cmd+1..9 — switch к tab N
- [ ] Tab / Shift+Tab — indent / outdent
- [ ] Cmd+/ — toggle comment
- [ ] Standard navigation: arrows, Home, End, PageUp/PageDown, Cmd+Arrow для word/line jumps

### Stability
- [ ] 5-minute idle: no crash, no degradation, memory stable
- [ ] 30-minute heavy use: type, save, switch tabs, search — no crash
- [ ] Open 50 files sequentially: no leak, no slowdown
- [ ] Switch between 20 tabs rapidly: responsive
- [ ] Force kill cleanup: после force kill, restart works (state не corrupted)

### Process lifecycle
- [ ] Launch latency: < 5 seconds от click к ready
- [ ] Clean exit: Cmd+Q → process exits cleanly, no leftover Wine processes
- [ ] Re-launch: state restored where appropriate (Recent Files, last position)
- [ ] No zombie processes after exit

### Visual polish (Apple HIG compliance scoped)
- [ ] Window chrome native (red/yellow/green traffic lights work)
- [ ] No "white window" моменты (proper paint from start)
- [ ] No flickering during resize
- [ ] No tearing during scroll
- [ ] Scrollbars styled (macOS overlay scrollbars или Win32 — consistent)
- [ ] Cursor changes properly (text cursor in editor, arrow in toolbar, etc.)
- [ ] Selection highlight color reasonable
- [ ] Caret blinks at standard rate

---

## 2. Methodology — как достичь

### Disciplined evidence-driven loop

For each failing item:

1. **Probe** — runtime evidence (sample, lldb, smoke harness output, screenshot diff vs Windows baseline)
2. **Classify** — narrow bug? family? new architectural class?
3. **Document** — `reports/phase-h/MILESTONE/functional-issues.md` с specific test case
4. **Fix** — narrow per evidence, family per audit
5. **Test** — automated smoke verifies fix
6. **Regression** — smoke test added permanent
7. **Commit** — proper message с family checklist

### Smoke harness must be comprehensive

Current `scripts/run-notepad-x64-ui-smoke.sh` уже existing — extend it:
- Each closure criterion above → automated test case
- All cases run in sequence
- Each emits PASS/YELLOW/FAIL с evidence file
- Failing cases halt harness, document, fix, retry
- Run completes only when all GREEN

Smoke harness becomes **regression suite** — runs every time engine changes.

### Windows baseline artifacts required

Для visual comparison (fonts, toolbar icons, dialog rendering):
- Run Notepad++ на Windows VM
- Capture screenshots of each closure scenario
- Save к `reports/phase-h/MILESTONE/windows-baseline/`
- Compare pixel-by-pixel или structurally
- "Yellow" until baseline exists, "green" when matches

### No screenshot-only evidence

Per AGENTS.md `patch-by-evidence`:
- "Looks ok" не достаточно
- Must have programmatic verification (Win32 message reaches handler, dialog HWND exists, file written to disk, etc.)
- Screenshots supplement evidence, не sole source

---

## 3. Areas likely to require deep work

### A. Menu/Command IDs path
Currently: `cmd_new=0 cmd_open=0 cmd_save=0` — menu inventory не resolves command IDs.

Investigation directions:
- `GetMenu(hwnd)` returns?
- `GetMenuItemInfoW` correct?
- Why all IDs zero — accelerator table not loaded? Menu not registered? Localization missing?
- Could be HyperBridge bug в menu resource loading

### B. Toolbar icon rendering
Currently: gray placeholders instead of real icons.

Investigation:
- ICONIMAGELIST resource loading
- ImageList_Draw call path
- GDI+ image alpha handling
- BMP/PNG resource decode
- HiDPI scaling factor

Could be GDI+ family bug или resource loader issue.

### C. Font metrics
Currently: yellow baseline, metrics off vs Windows.

Investigation:
- `GetTextMetricsW` returns correct values?
- `GetTextExtentPoint32W` matches Windows?
- FreeType integration в winemac.drv
- DirectWrite path (if Notepad++ uses it)
- ClearType subpixel rendering

Likely FreeType+macOS font matching layer.

### D. Dialogs (Find, Replace, Preferences)
Currently: missing_command FAIL — can't dispatch even.

Investigation:
- Once menu IDs работают → likely dialogs follow
- Common dialogs (comdlg32) integration с macOS native?
- Dialog templates loading from resources
- Modal dialog message pump

### E. Stability / clean exit
Currently: wrapper timeout, clean bounded exit not certified.

Investigation:
- ExitProcess path
- Resource cleanup sequence
- Thread join behavior
- Wine prefix shutdown

May be combase INFINITE wait sites после all (Kimi's audit was right defensively даже if not direct hang cause).

---

## 4. Coordination

### With Kimi (parallel streams)
- Kimi works networking, AI configurator, audits — independent
- Don't request Kimi для Phase H work unless surfaces specifically helpful audit
- Kimi standby tasks (AOT P3, Audio E2E, Graphics e2e) wait Phase H closure — natural

### Engine vs Wine
- HyperBridge bugs: fix in `engine/hyperbridge/`, family audit per protocol
- Wine bugs: fix in `engine/wine/dlls/...` если patch-by-evidence supports it
- Note: `engine/` directory currently `.gitignore`'d — этот **infrastructure issue**:
  - Discuss с user приоритет: track engine/ vs submodule vs patch series
  - For now: changes invisible to git, but **must verify reproducible across rebuilds**

### Tests
- Each fix adds regression test (HyperBridge unit test OR smoke harness case)
- Full test suite must stay green
- Watch для stale `libhyperbridge.a` trap (per AGENTS.md known traps)

---

## 5. Reporting cadence

### Daily-style updates к user

Format:
```
Phase H status YYYY-MM-DD HH:MM:

Closure criteria: X / N green
Worked today:
- [bullet] specific items
Evidence:
- [PASS/YELLOW/FAIL] for each touched criterion
Next:
- [bullets] tomorrow's plan
Blockers / questions:
- [TRUE blockers only]
```

### Per-bug Obsidian update

Each fixed bug → entry in `/Users/timurtoby/Documents/MacRunner/90-architectural-discoveries-fundamental-bugs.md` если architectural class.

### Per-week status doc

Every 7 days: `reports/phase-h/MILESTONE/weekly-status-YYYY-MM-DD.md` с full breakdown.

---

## 6. Stop criteria — when Phase H actually closes

Phase H closes when **all** of these true:

- [ ] All closure criteria above green с automated evidence
- [ ] Smoke harness regression suite passes 3 consecutive runs
- [ ] 30-minute manual heavy-use test без crash/hang
- [ ] Clean exit certified (no leftover processes)
- [ ] Memory leak test: 1 hour idle, RSS stable
- [ ] Windows baseline visual comparison: structurally equivalent
- [ ] User (Timur) manually verifies, signs off
- [ ] Final tag: `v0.2-engine-phase-h-x64-notepad-plus-plus-production`

**Only after this** → Phase I starts с KeePass 1.43.

---

## 7. Do NOT

- ❌ Don't say "looks good enough"
- ❌ Don't move to KeePass без full Phase H closure
- ❌ Don't fix from screenshots alone
- ❌ Don't commit без AGENTS.md family audit format
- ❌ Don't skip evidence collection
- ❌ Don't apply Kimi's audit findings вслепую (verify evidence first per recent rollback discipline)
- ❌ Don't accept timeout/hang as "kinda works"
- ❌ Don't rush к visible mode при cost of functional mode
- ❌ Don't add workarounds without TODO + tracked subtask

---

## 8. Estimated duration

**Honest estimate**: 2-6 weeks intensive focused work.

Closure criteria above ≈ 80 items. Each:
- Best case: 1-2 hours (narrow fix, clear evidence)
- Average case: half-day (investigation + fix + test)
- Worst case: 2-3 days (architectural finding, family fix)

Expected mix:
- ~40 items quick (text input, basic shortcuts, menu navigation): 1-2 weeks
- ~25 items medium (dialogs, toolbar icons, font matching): 2-3 weeks
- ~15 items hard (stability, clean exit, edge cases): 1-2 weeks

**Total realistic**: 4-7 weeks. Don't rush.

---

## 9. Сигнал начала

When Codex reads this directive:

1. Read AGENTS.md fresh
2. Read this directive end-to-end
3. Read `reports/phase-h/MILESTONE/functional-issues.md` (current state)
4. Build full closure criteria checklist в `reports/phase-h/MILESTONE/closure-checklist.md` (copy this section 1 verbatim)
5. Confirm current state of each (PASS/YELLOW/FAIL/UNVERIFIED)
6. Pick highest-priority FAIL/UNVERIFIED item
7. Per AGENTS.md methodology — evidence-driven narrow work
8. Document, fix, test, regression, commit, advance к next item
9. Repeat until all green
10. Then Phase I

Поехали. Quality > speed. Native > workaround. Truth > optics.
