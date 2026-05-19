# Kimi MEGA Master Brief — Native macOS Integration (Premium UX Moat)

**Status**: FUTURE BRIEF — Hold для deployment пока Networking + AI Configurator phases не closing. Currently committed as reference document.

**Scope**: Transform MacRunner из "Wine wrapper" в **"Windows app которое feels native на Mac"**. Deep AppKit/Cocoa integration replacing Wine's generic macdrv shim path. **The premium UX moat** which Mythic/Whisky/CrossOver не имеют.

**Duration**: 6-9 months focused work, 8 phases.

**Autonomy level**: Maximum. Architectural ownership, AppKit API choices, UX design decisions.

**Conflict risk**: ZERO с Codex (engine territory). LOW с networking (different Wine DLLs). LOW с AI Configurator (different scope). MEDIUM coordination с existing macdrv (you extend, не rewrite).

**Strategic value**: This is **why pay for MacRunner vs free Wine fork**. The experience that **justifies $9.99-$24/seat pricing**. Without native UX integration — just functional Wine. With it — magical macOS-first product.

---

## 0. Mandatory reading

1. [/Volumes/MacOS/MacRunner/AGENTS.md](../AGENTS.md) — **🎯 Zeroth Principle**, protocols
2. [/Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md](file:///Users/timurtoby/Documents/MacRunner/88-competitive-analysis-mythic.md) — UX gaps Mythic / CrossOver / Whisky
3. Your other briefs — methodology pattern stays same
4. macOS Human Interface Guidelines: https://developer.apple.com/design/human-interface-guidelines/macos
5. This brief

---

## 1. Strategic context — why native UX is the moat

### What competitors ship

**Mythic / Whisky**: Bare Wine с Cocoa shim. Windows app shows up как:
- Window without native Mac chrome integration
- Win32 menu bar (not macOS menu bar)
- Ctrl+S, не Cmd+S
- Win32 File Open dialog, не NSOpenPanel
- No Spotlight indexing
- No Universal Clipboard
- No Quick Look для files
- Dock icon is generic Wine icon

**CrossOver**: Slightly more polish, но fundamentally same. Win32 controls translated к Cocoa surfaces but UX **feels foreign**.

**MacRunner с this brief**: Windows app **becomes native Mac app**:
- Cmd+S works (Win32 Ctrl+S translated)
- macOS menubar shows app's menus
- File Open uses NSOpenPanel
- Spotlight indexes Windows file types
- Universal Clipboard shares с iPhone/iPad
- Quick Look generates previews
- Dock badges show progress / notifications
- Continuity APIs (Handoff) work
- Touch ID для credential prompts
- Dark mode applied к Windows app correctly

### Pricing tier justification

```
Free Wine fork:           Works but ugly                  $0
Mythic / Whisky:          Slightly nicer Wine wrapper     $0 (donations)
CrossOver:                Polished Wine wrapper           $74/year
MacRunner с native UX:    Windows app feels Mac-native    $9.99-$24/seat
```

Why pay? **Premium UX**. Same Windows app, **dramatically better Mac experience**. Native shortcut keys. Native dialogs. Spotlight integration. Continuity. **Это что makes user click "Buy"**.

### Competitive timing

Mythic/Whisky depend на Rosetta — dies 2027-2028 (Apple deprecation). They have **2-year window** to differentiate. Most likely they'll **never invest** в native UX — focus stays на game compatibility (their market).

MacRunner targeting **productivity professionals** (1С, AutoCAD, Photoshop, KeePass users) — this segment **values native UX** much higher than gamers. Right segment + right timing = **defensible moat**.

---

## 2. Current state — what exists

### Existing macdrv (Wine's native macOS driver)

```
engine/wine/dlls/winemac.drv/
├── cocoa_app.m              — NSApplication wrapper
├── cocoa_window.m           — NSWindow wrapper
├── cocoa_event.m            — NSEvent handling
├── cocoa_clipboard.m        — Basic clipboard
├── cocoa_display.m          — Display detection
├── cocoa_cursorclipping.m   — Cursor handling
└── ...
```

**What works**:
- Basic NSWindow creation
- Event loop integration
- Display geometry
- Cursor visibility

**What's missing** (your work):
- Native menubar (Win32 menu → NSMenu translation)
- Native file dialogs (replace Win32 GetOpenFileName с NSOpenPanel)
- Notification integration (UserNotifications framework)
- Spotlight integration (CSSearchableItem indexing)
- Quick Look generators
- Universal Clipboard (Continuity)
- Handoff support
- Keychain integration для credentials
- Touch ID prompts
- Dock badges / progress indicators
- Dark mode propagation
- Standard keyboard shortcuts (Cmd vs Ctrl)
- Full-screen mode Mac-style
- Mission Control / Spaces awareness
- Sleep/wake handling
- AppleScript automation surface

---

## 3. Phased work plan

### Phase Φ.0 — Audit + design (1-2 weeks)

Standard methodology — audit first, design second, code third.

Deliverables:
- `docs/NATIVE-MACOS-AUDIT.md` — existing winemac.drv state, gaps vs full native UX
- `docs/NATIVE-MACOS-STRATEGY.md` — architecture, AppKit framework choices, transition plan
- `docs/NATIVE-MACOS-ROADMAP.md` — phases Φ.1-Φ.7 timeline
- First 4 ADRs:
  - **ADR-Φ001**: NSMenu strategy (mirror Win32 vs translate vs hybrid)
  - **ADR-Φ002**: File dialog replacement (intercept layer)
  - **ADR-Φ003**: Notification framework (UserNotifications.framework vs older NSUserNotification)
  - **ADR-Φ004**: Spotlight integration scope (per-app or global file types)

### Phase Φ.1 — Native menubar (3-4 weeks)

**Most visible immediate impact.**

Windows apps have menubar **внутри window** (File, Edit, etc. attached к window frame). Mac apps have menubar **в top of screen** (system-wide). Native Mac app feel **demands** this translation.

**Implementation**:
1. Hook Win32 `LoadMenu()`, `CreateMenu()`, `SetMenu()`
2. When called, build parallel NSMenu structure
3. Hide Win32 menubar inside window
4. Display NSMenu в `NSApp.mainMenu` when window becomes key
5. Menu item clicks dispatch к Win32 `WM_COMMAND` handler

**Key challenges**:
- Win32 menus have mnemonics (`&File`); Mac uses ⌘ shortcuts
- Win32 separators, submenus, popups all need translation
- Dynamic menus (Recent Files, Window list) need live sync
- Context menus (right-click popup) different paradigm

**Standard Mac menu items** to inject automatically:
- About `<AppName>` (currently Win32 apps лack это в standard place)
- Preferences... (translate Win32 Tools→Options)
- Quit `<AppName>` (Cmd+Q)
- Window menu (standard Mac convention)
- Help menu

**Tests**:
- Notepad++ menus appear в macOS menubar
- Cmd+N, Cmd+O, Cmd+S work (translated от Ctrl+N, Ctrl+O, Ctrl+S)
- Submenu navigation works
- Recent Files updates dynamically
- Context menus (right-click) work через NSMenu

### Phase Φ.2 — Native dialogs (2-3 weeks)

Win32 `GetOpenFileName()`, `GetSaveFileName()`, `MessageBox()`, etc. — все имеют **native macOS equivalents**.

**Implementation**:
1. Hook Win32 common dialogs (comdlg32.dll)
2. Replace internal Win32 dialog implementation с NSOpenPanel/NSSavePanel/NSAlert
3. Translate parameters bidirectionally
4. Preserve app's filter strings, default extensions, initial dir

**Coverage**:
- `GetOpenFileNameW` → NSOpenPanel
- `GetSaveFileNameW` → NSSavePanel
- `MessageBoxW` → NSAlert
- `ChooseColorW` → NSColorPanel
- `ChooseFontW` → NSFontPanel
- `PrintDlgW` → NSPrintPanel
- `FormatMessageW` / Error dialogs → NSAlert variants

**Tests**:
- Notepad++ File→Save shows native macOS Save dialog
- File types filter works (text/all)
- Default extension applied
- File→Open shows native picker
- MessageBox alerts use NSAlert (proper Apple style)

### Phase Φ.3 — Notifications & badges (2 weeks)

Modern macOS notifications via UserNotifications framework.

**Coverage**:
- Win32 `Shell_NotifyIcon` (system tray) → macOS menu bar items (NSStatusItem)
- Win32 toast notifications → UNUserNotificationCenter
- Win32 balloon tips → UNNotification banners
- Dock badge counts (e.g., unread emails) → `NSApp.dockTile.badgeLabel`
- Dock progress indicators (long operations) → `NSDockTile` views

**Tests**:
- App posts notification → appears в macOS Notification Center
- Action buttons на notification work
- Dock badge updates correctly
- Long file operation shows progress в Dock

### Phase Φ.4 — Spotlight + Quick Look (3-4 weeks)

**Major productivity feature**: Windows files indexed by Spotlight, previewable в Finder.

**Spotlight indexing**:
- Register Windows file types (.docx if Office installed, .xlsx, .pptx, .skp для SketchUp, .dwg для AutoCAD)
- Implement `mdimporter` plugins что extract text content
- Index per app's known formats

**Quick Look**:
- `qlgenerator` plugins per Windows file type
- Render preview using app's own thumbnailing if possible
- Fallback к generic icon + filename

**Implementation**:
- Per-app profile (from AI Configurator) specifies what types к index
- Generic text-format previews shipped с MacRunner
- App-specific previews via running app в headless mode

**Tests**:
- After installing Notepad++ — `.npp` files have Quick Look
- `.dwg` files searchable via Spotlight content (after AutoCAD installed)
- Cmd+Space search finds Windows app content

### Phase Φ.5 — Continuity (Handoff, Universal Clipboard) (3-4 weeks)

**Premium feature**: Windows app integrates с iPhone/iPad/другой Mac.

**Universal Clipboard**:
- Win32 clipboard ops automatically participate в macOS pasteboard
- Copy в Windows app — paste на iPhone works
- macOS handles cross-device sync via iCloud
- Just need: ensure Win32 clipboard data → NSPasteboard correctly typed

**Handoff**:
- App state shareable между devices
- Pick up editing где left off (within compatible apps)
- Requires NSUserActivity API integration
- Apps must register Activity types

**AirDrop**:
- Right-click file в Notepad++ → AirDrop... → sends к Mac/iPhone
- Hook share menu integration

**Tests**:
- Copy text в Notepad++ → paste на iPhone Notes (Universal Clipboard)
- Edit document → switch к iPad → continue (Handoff, if app supports)
- Right-click → Share → AirDrop visible в menu

### Phase Φ.6 — Keychain + Touch ID + Passkeys (3 weeks)

**Security UX**: Windows credential storage uses Wine's basic implementation. Replace с macOS Keychain.

**Coverage**:
- Win32 `CredWrite`, `CredRead` → Keychain SecItemAdd/SecItemCopyMatching
- Hook DPAPI for credential encryption → Keychain encryption
- Touch ID prompts for sensitive credential access
- Passkeys для Windows apps that support them (rare but future-proof)
- Smart card support (USB tokens like YubiKey)

**Implementation**:
- Replace credential storage in Wine
- Per-app keychain "service" scoping
- Touch ID prompt via LocalAuthentication framework
- User control: System Preferences → MacRunner → which apps may access Keychain

**Tests**:
- KeePass storage in macOS Keychain
- Touch ID prompt when accessing stored password
- 1С credentials persist correctly
- Sysadmin tool Smart Card auth works (если applicable)

### Phase Φ.7 — Standard macOS UX polish (3-4 weeks)

Final layer of native feel:

**Keyboard shortcuts** (Cmd vs Ctrl):
- Hook Win32 keyboard handling
- Translate Cmd+S → Ctrl+S internally (so app's handler works)
- Standard shortcuts (Cmd+Q quit, Cmd+W close, Cmd+M minimize, Cmd+H hide, etc.)
- Per-app override possible

**Full-screen mode**:
- macOS native full-screen (Cmd+Ctrl+F) → Win32 fullscreen translation
- Hide Dock + menu bar appropriately
- Mission Control space allocation

**Mission Control / Spaces**:
- Window appears в Mission Control overview
- Spaces switching preserves state
- App Exposé shows all app's windows

**Dark mode**:
- Detect macOS appearance (`NSAppearance.current.name`)
- Propagate к Windows app via env var or registry hint
- Per-app override (some apps don't dark-mode well — keep light)

**Sleep/wake**:
- `NSWorkspaceWillSleepNotification` → suspend background work
- `NSWorkspaceDidWakeNotification` → resume
- Network reconnect handling

**Hot corners + gestures**:
- Trackpad swipe between Spaces preserves window
- Hot corner Mission Control works correctly
- Three-finger swipe back/forward (in browser-like apps)

**Tests**:
- Cmd+Q quits Notepad++ cleanly
- Cmd+W closes current document
- Full-screen mode hides Dock properly
- Mission Control shows app windows
- Dark mode setting reflects через appearance change

---

## 4. Out of scope (do NOT do)

- **Don't write own DirectX/Metal layer** — DXMT/DXVK/VKD3D already done в graphics phase Γ
- **Don't replace Wine's audio** — Kimi's audio brief handles this
- **Don't reimplement networking** — Kimi's networking brief covers
- **Don't touch HyperBridge translator** — Codex's territory
- **Don't redesign Wine internals** — extend macdrv path, не rewrite Wine

**Stay focused**: macOS-side native UX integration. Wine internals stay Wine's domain.

---

## 5. Reference materials

### Apple frameworks
- AppKit (NSApp, NSWindow, NSMenu, NSAlert, NSOpenPanel)
- UserNotifications.framework
- Network.framework (already used by networking brief — share patterns)
- LocalAuthentication.framework (Touch ID)
- Security.framework (Keychain)
- CoreServices.framework (LaunchServices, Quick Look)
- MobileCoreServices (UTType definitions)
- IntelligenceCore.framework (Spotlight integration, macOS 14+)

### Wine internals
- `engine/wine/dlls/winemac.drv/` — current macOS driver (extend, don't rewrite)
- `engine/wine/dlls/comdlg32/` — common dialogs (intercept here for Phase Φ.2)
- `engine/wine/dlls/user32/` — menus, keyboard handling
- `engine/wine/dlls/shell32/` — shell integration (notifications, tray)

### macOS HIG references
- macOS Menu Bar guidelines
- Standard keyboard shortcuts
- Sandbox + entitlements (если App Store distribution someday)
- Notification Center best practices

### Competitive references
- CrossOver's NSMenu integration (closed source но usage observable)
- Whisky's macOS UX (basic, lots of room для improvement)
- Apple's Catalyst (iOS apps on Mac) — similar translation problems

---

## 6. Methodology (same as other briefs)

### Zeroth Principle
- Native AppKit APIs throughout — NSMenu, NSOpenPanel, NSAlert, не custom drawn
- Apple HIG compliance per Apple's standards
- Root-cause every UX gap — не "good enough" hacks

### Family audit
- Found one Win32 menu function intercepted → audit all menu APIs
- Found one keyboard shortcut translated → audit all standard shortcuts
- Found one dialog replaced → audit all comdlg32 entries

### Patch-by-evidence
- Real Windows apps на Windows VM как ground truth
- Screenshots compare side-by-side с macOS expected
- User-testable changes documented

### Decision autonomy
- All AppKit API choices yours
- Standard keyboard mapping (Cmd vs Ctrl per shortcut) yours
- App-specific overrides design yours
- Escalate: legal (App Store guidelines), brand (notification text), partnerships

---

## 7. Workspace

```
/Volumes/MacOS/MacRunner/
├── engine/native-macos/                            # YOUR PRIMARY workspace
│   ├── menubar/                                     # Phase Φ.1
│   ├── dialogs/                                     # Phase Φ.2
│   ├── notifications/                               # Phase Φ.3
│   ├── spotlight/                                   # Phase Φ.4
│   ├── continuity/                                  # Phase Φ.5
│   ├── keychain/                                    # Phase Φ.6
│   ├── polish/                                      # Phase Φ.7
│   ├── shared/                                      # common helpers
│   └── tests/
├── engine/wine/dlls/winemac.drv/                   # EXTEND existing
├── engine/wine/dlls/comdlg32/                      # EXTEND for Phase Φ.2
├── docs/
│   ├── NATIVE-MACOS-AUDIT.md
│   ├── NATIVE-MACOS-STRATEGY.md
│   ├── NATIVE-MACOS-ROADMAP.md
│   ├── NATIVE-MACOS-ADR-NNN-*.md                   # multiple ADRs
│   ├── NATIVE-MACOS-VALIDATION-*.md                # per-app validation
│   └── KIMI-PROGRESS-native-macos.md               # YOUR PROGRESS LOG
```

### Do NOT touch

- `engine/hyperbridge/` — Codex
- `engine/wine/dlls/ntdll/`, `kernelbase/`, `win32u/` — Codex
- `engine/audio/`, `engine/wine/dlls/winecoreaudio.drv/` — your audio
- `engine/networking/`, `cloud/` — your networking
- `engine/hyperbridge/cache/` — your AOT
- `engine/graphics/`, `engine/dxmt/`, etc. — graphics done
- `app/configurator/ai/` — your AI configurator

### Coordination
- macdrv changes — coordinate с Codex if architectural
- comdlg32 changes — coordinate с Codex
- Hooks через Wine APIs — stay outside Codex's bug-fix areas

---

## 8. Deliverables per phase

### Φ.0 (audit + design)
- [ ] 3 docs (audit, strategy, roadmap)
- [ ] 4 ADRs (menu, dialogs, notifications, Spotlight)
- [ ] Skeleton workspace dirs

### Φ.1 (menubar)
- [ ] Notepad++ menus appear в macOS top menubar
- [ ] Cmd+S works
- [ ] Submenus navigate
- [ ] Recent Files dynamic
- [ ] Standard items (About, Preferences, Quit) injected

### Φ.2 (dialogs)
- [ ] File → Save shows NSSavePanel
- [ ] File → Open shows NSOpenPanel
- [ ] MessageBox uses NSAlert
- [ ] Filter strings preserved

### Φ.3 (notifications)
- [ ] App notification appears в macOS Notification Center
- [ ] Dock badge updates
- [ ] Dock progress visible

### Φ.4 (Spotlight)
- [ ] At least 5 Windows file types indexed
- [ ] Spotlight content search works
- [ ] Quick Look preview generated

### Φ.5 (Continuity)
- [ ] Universal Clipboard works (Mac copy → iPhone paste)
- [ ] At least 1 Handoff scenario functional
- [ ] AirDrop integrates с share menu

### Φ.6 (Keychain + Touch ID)
- [ ] Windows app credentials в macOS Keychain
- [ ] Touch ID prompt для sensitive access
- [ ] System Preferences shows MacRunner access controls

### Φ.7 (polish)
- [ ] All standard shortcuts (Cmd+Q/W/M/H/N/O/S) work
- [ ] Full-screen mode native
- [ ] Mission Control shows windows
- [ ] Dark mode propagated
- [ ] Sleep/wake handled

---

## 9. Success criteria (overall)

When all 8 phases complete, the user experience:

```
1. User installs MacRunner
2. Drops .exe (e.g., Notepad++) onto MacRunner Dock
3. Notepad++ launches
4. Notepad++ menus appear в macOS top menubar
5. Cmd+N (not Ctrl+N) creates new document
6. Cmd+S opens native macOS Save dialog
7. File saved, Spotlight indexes it
8. Quick Look shows preview в Finder
9. Cmd+C copies, paste on iPhone via Universal Clipboard
10. Cmd+Q quits cleanly
11. Restart app — Handoff prompts "Continue editing на iPhone"?
12. Touch ID needed для protected credential — prompt appears
13. Dark mode change в macOS — Notepad++ respects it
```

This is what makes user pay $9.99-$24/seat. "Я даже забыл что это Windows app."

---

## 10. Communication

`docs/KIMI-PROGRESS-native-macos.md` per session — same format as your other progress logs.

Per-phase closeout: `docs/NATIVE-MACOS-PHASE-Φ-X-CLOSURE.md`

ADRs prolifically — UX decisions are many subtle ones.

---

## 11. Timeline expectations

| Phase | Weeks | Sessions |
|---|---|---|
| Φ.0 audit + design | 1-2 | 5-10 |
| Φ.1 menubar | 3-4 | 15-20 |
| Φ.2 dialogs | 2-3 | 10-15 |
| Φ.3 notifications | 2 | 8-10 |
| Φ.4 Spotlight + Quick Look | 3-4 | 15-20 |
| Φ.5 Continuity | 3-4 | 15-20 |
| Φ.6 Keychain + Touch ID | 3 | 12-15 |
| Φ.7 Polish | 3-4 | 15-20 |
| **Total to v1 production** | **20-26 weeks** | **95-130 sessions** |

5-6 months **after** Networking + AI Configurator close. Don't start before existing streams not closing — quality bar over breadth.

---

## 12. Coordination touchpoints

### With Codex
- `winemac.drv` modifications — coordinate если architectural
- `comdlg32` intercepts — verify не conflicts с Codex's WinAPI work
- Keychain integration — replaces Wine credential storage; coordinate

### With your other streams
- **Networking**: Notification system might use cloud telemetry (your networking brief Phase Δ.5)
- **AI Configurator**: Per-app config может specify "use native menubar: yes/no"
- **Audio**: Spatial audio notifications могут use AVAudioEnvironmentNode
- **AOT cache**: irrelevant
- **Graphics**: rendering decisions ortogonal

### With Timur
- App Store distribution path (требует sandbox + entitlements — own future brief)
- Brand decisions (notification text wording, app naming в menu)
- Legal (Continuity APIs — Apple developer agreement compliance)
- Marketing (premium UX как selling point)

---

## 13. The vision

When done, MacRunner UX:

```
User scenarios:

A) "I want to use Windows-only tool на Mac"
   → Drop .exe → it appears как native Mac app
   → Cmd+S, Cmd+W, Cmd+Q all work natural
   → File dialogs are macOS native
   → No "I'm in Wine compatibility layer" feeling

B) "I work across Mac/iPhone/iPad"
   → Universal Clipboard works seamlessly
   → AirDrop files between Mac and iPhone
   → Handoff between devices (where supported)

C) "Security matters"
   → Credentials in macOS Keychain
   → Touch ID for sensitive access
   → System-level permission controls

D) "I expect native Mac quality"
   → Dark mode respected
   → Full-screen mode proper
   → Mission Control integration
   → Standard keyboard shortcuts

E) "Find things in Spotlight"
   → Windows file types indexed
   → Content search works
   → Quick Look previews
```

**Result**: $9.99-$24/seat justified. Mythic/Whisky/CrossOver можно not match в timeframe. Defensible commercial moat.

---

## Appendix A — Path quick reference

```
Repo:                       /Volumes/MacOS/MacRunner
Native macOS workspace:     engine/native-macos/
Wine driver:                engine/wine/dlls/winemac.drv/ (extend)
Common dialogs:             engine/wine/dlls/comdlg32/ (extend)
Documentation:              docs/NATIVE-MACOS-*.md
Progress log:               docs/KIMI-PROGRESS-native-macos.md
```

## Appendix B — Sanity check first

```bash
cd /Volumes/MacOS/MacRunner
. config/env.sh
echo "Root: $MACRUNNER_ROOT"

# Verify existing macdrv
ls engine/wine/dlls/winemac.drv/ | head -10
wc -l engine/wine/dlls/winemac.drv/*.m engine/wine/dlls/winemac.drv/*.c | tail

# Verify AppKit accessible
swift -e 'import AppKit; print("AppKit available:", NSApp.description)'

# Verify Apple developer tooling
xcrun --show-sdk-version
xcrun --show-sdk-path

# Verify common dialogs DLL
ls engine/wine/dlls/comdlg32/
```

If all OK — environment ready when this brief activated.

---

## When this brief activates

**Trigger conditions** (all must be met):
1. Networking Δ.4 (HTTP via URLSession) closed OR in final stages
2. AI Configurator Phase A.9 (real user validation) at least design-phase done
3. Codex Phase H (Notepad++ truly functional) closed с tag
4. At least 1 other productivity app (KeePass 1.43?) running

When triggered, message к Кими:

```
Notepad++ functional, Networking core done, AI Configurator architectural
complete. Time для premium UX moat brief.

Read: docs/KIMI-TASK-native-macos-integration-MEGA-master-brief.md

This is the work что makes MacRunner commercial product worth paying for.
6-9 months focused work. Phase Φ.0 audit + design first session.

Apply Zeroth Principle. AppKit native throughout. Decision autonomy maximum.

Поехали.
```

Until then — this brief stays committed as reference. Не deploy.

---

End of brief. **Premium UX moat** — the work that **distinguishes** MacRunner from competitors when commercial shipping.

Stay in standby. Activate when other streams close.
