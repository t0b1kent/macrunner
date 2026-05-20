# Kimi SWARM Brief — KeePass 1.43 + Calc Win7, 30-agent parallel audit

**Status**: FUTURE brief. Deploy когда Кими свободен (после rendering audit / cloud
backend natural pause) AND Codex closes Notepad++ Phase H.

**Goal**: Параллельный discovery audit для следующих 2 apps (KeePass 1.43 x86,
Calc Win7 x86) — найти ВСЕ engine bugs ДО того как Codex начнёт их чинить.
30 disjoint audit lanes → reports в swarm/inbox-for-codex/ → Codex чинит серийно.

**Critical rule** (per swarm/SWARM-ARCHITECTURE.md): каждый агент = DISJOINT output
file. НИКОГДА два агента на один файл. Discovery only — не редактировать engine,
не запускать Wine concurrently (RAM). Static analysis + code audit + PE inspection.

---

## Почему эти 2 apps next

- **KeePass 1.43 Portable (x86, 32-bit)**: первый 32-bit app → WoW64 path, x86
  opcode coverage (отличается от x64), CryptoAPI, ListView/TreeView. Simple GUI =
  чистый baseline для 32-bit.
- **Calc Win7 (x86)**: GDI+ heavy (custom button drawing), MUI dialog templates,
  WIC/PNG decode, shell32 AnimateWindowSize, COM apartment. Quirks-heavy stress.

Вместе покрывают: 32-bit engine path + GDI+ graphics + MUI + crypto + shell COM.

---

## 30-agent partition (disjoint lanes)

Каждый агент: audit одного subsystem path, как KeePass/Calc его exercise. Output →
`swarm/inbox-for-codex/<lane-id>.md` (symptom potential, root cause hypothesis,
file:line, fix scope, confidence). Read-only.

### Group A — x86 / 32-bit engine (KeePass) — lanes 1-8
1. **x86 decoder coverage**: audit hb_decode_x86.c vs Intel SDM 32-bit. Какие opcodes missing vs x64? (KeePass x86 binary disasm → opcode histogram)
2. **WoW64 path**: how 32-bit PE loads на ARM64. wow64 thunks, 32-bit ntdll path
3. **32-bit calling convention**: stdcall/cdecl/thiscall vs x64 SysV. ABI marshalling
4. **32-bit heap layout**: RtlAllocateHeap 32-bit, pointer truncation risks
5. **32-bit register set**: no R8-R15, different reg pressure. Interpreter 32-bit state
6. **x86 SSE/MMX**: legacy MMX (mm0-7), 32-bit SSE encoding differences
7. **32-bit TEB/PEB**: fs segment (32-bit) vs gs (64-bit). Segment translation
8. **32-bit exception handling**: SEH chain (fs:[0]) vs x64 table-based

### Group B — CryptoAPI (KeePass) — lanes 9-12
9. **crypt32**: certificate, encoding APIs KeePass uses
10. **advapi32 crypto**: CryptAcquireContext, CryptHashData, CryptEncrypt (KeePass AES)
11. **bcrypt/ncrypt**: modern crypto KeePass 2.x may use (note for later)
12. **RNG**: RtlGenRandom / CryptGenRandom (key generation)

### Group C — Common controls (KeePass + Calc) — lanes 13-18
13. **comctl32 ListView**: KeePass entry list. LVM messages, owner-draw
14. **comctl32 TreeView**: KeePass group tree
15. **comctl32 Header**: list column headers
16. **comctl32 toolbar/rebar**: (reuse Notepad++ findings if any)
17. **comctl32 statusbar**: status bar at bottom
18. **comctl32 tooltip/updown**: tooltips, spin controls

### Group D — GDI+ graphics (Calc) — lanes 19-23
19. **gdiplus core**: GdipDrawImage, GdipFillRectangle (Calc custom buttons)
20. **gdiplus brushes/pens**: gradient fills Calc uses
21. **WIC image decode**: PNG decode chain (Calc button images)
22. **gdi32 BitBlt/AlphaBlend**: alpha compositing for Calc buttons
23. **gdi32 font/text**: GetTextMetrics, DrawText (reuse Notepad++ font findings)

### Group E — MUI / resources (Calc) — lanes 24-26
24. **MUI dialog templates**: RT_DIALOG resource loading, Win7 MUI fallback
25. **LoadString/resource**: MUI string tables, language fallback
26. **AnimateWindow**: shell32 AnimateWindowSize COM helper (Calc quirk)

### Group F — COM/shell (both) — lanes 27-30
27. **COM apartment**: STA init (reuse dim06/08 findings — verify for these apps)
28. **shell32 SHGetFileInfo**: icons (reuse Notepad++ folder icon fix)
29. **ole32/oleaut32**: BSTR, VARIANT marshalling
30. **clipboard/DDE**: data exchange paths

---

## Per-lane output spec

`swarm/inbox-for-codex/lane-NN-<subsystem>.md`:
```markdown
# Lane NN — <subsystem>
App(s): KeePass / Calc
Status: AUDIT COMPLETE

## How app exercises this subsystem
[which APIs called, what scenario]

## Potential issues found
### Issue NN.1
Severity: CRITICAL/HIGH/MED/LOW
Confidence: high/med/low
File: path:line
Symptom: [predicted]
Root cause hypothesis: [analysis]
Fix scope: [where Codex should look]

## Reuse from prior findings
[link to existing audits/bugs that apply]

## What looks OK
[validated paths]
```

---

## Batch sizing
- Launch in waves of ~20-30 (token budget)
- Each lane independent → incomplete wave OK, resume remaining
- Idempotent: lane done = file exists in inbox

## Coordination
- Codex consumes inbox/*.md → fixes engine serially → moves to swarm/reports/done/
- Codex only engine editor (no merge conflict)
- Wine RUNS serialized (RAM) — swarm is static analysis, Codex/queue runs apps

## When to deploy
1. Codex closed Notepad++ Phase H (functional + visual + stability green)
2. Кими free (rendering audit done OR cloud at natural pause)
3. KeePass 1.43 + Calc Win7 payloads extracted to artifacts/

## Methodology
Per AGENTS.md: patch-by-evidence (predictions с confidence), family audit thinking,
honest confidence. Reuse GUI bug catalog (Marlett/GetSysColor/comctl32/shell32 from
Notepad++). Disjoint outputs = no merge conflict.

---

## Expected payoff
30 parallel audits surface most KeePass+Calc engine bugs upfront. Codex fixes
serially с root causes pre-found (like combase dim06/08 → fast fix). Breadth
(swarm) + depth (Codex serial) = both apps closed much faster than reactive
one-bug-at-a-time. The swarm pattern realized.
