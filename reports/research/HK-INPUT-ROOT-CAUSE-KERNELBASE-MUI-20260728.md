# HK INPUT — the null user driver has a cause, and it is a NULL ARM64X `.data` twin in kernelbase

**Lane:** HK-INPUT · **Date:** 2026-07-28 · **Status:** root cause measured; workaround proven offline; the
one remaining step needs a title slot.

---

## 1. One-paragraph summary

Yesterday's chain ended at *"win32u is running on the null user driver, because `explorer.exe` dies ~1 s
after it is spawned"*. This report supplies the missing cause. **`explorer.exe` never reaches `main()`.** It
dies inside `kernelbase.dll`'s `DllMain(PROCESS_ATTACH)` with `c0000005`, a read of address `0x18`, at
kernelbase RVA `0x5CCC8` — inside `load_mui_string`, walking the circular list `reg_mui_cache` whose head is
**an ARM64X dual-`.data` twin that never received its `LIST_INIT` self-pointer** and therefore reads as
`NULL`. ntdll's `loader_init` turns that into `NtTerminateProcess`. No explorer ⇒ no
`Control\Video\…\GraphicsDriver` ⇒ `load_desktop_driver()` returns FALSE ⇒ **null user driver** ⇒
`winemac.drv` is never loaded ⇒ no `macdrv_init`, no Cocoa app, no `-sendEvent:` ⇒ **keyboard and mouse are
structurally impossible, both for the same single reason.** Deleting the 278 timezone `MUI_Std`/`MUI_Dlt`
values from the prefix registry makes the faulting call unreachable and eliminates the crash — a change
confined to `artifacts/`, with no engine code touched. With that one change, explorer survives, **loads
`winemac.drv`, and executes `run_cocoa_app`** (§4.1) — the function this lane spent the day proving is never
reached — before hitting a second, separate HyperBridge defect (§4.2).

---

## 2. The defect, exactly

### 2.1 Call path (every link read out of source, not inferred)

| # | Site | What happens |
|---|---|---|
| 1 | `kernelbase/main.c:47` | `DllMain(PROCESS_ATTACH)` prints `macrunner-kernelbase-dllmain: process-attach` |
| 2 | `kernelbase/main.c:~50` | calls `init_locale()`; on success prints `init_locale done` (**never printed in a dying process**) |
| 3 | `kernelbase/locale.c:5991/5993` and `:6639/:6646` | timezone init calls `RegLoadMUIStringW(key, L"MUI_Std" / L"MUI_Dlt", …)` |
| 4 | `kernelbase/registry.c:2941` | `RegLoadMUIStringW` — reaches step 5 **only if** the value exists (`:2959-2960`) **and** starts with `'@'` (`:2974-2977`) |
| 5 | `kernelbase/registry.c:2845` | `load_mui_string` walks `reg_mui_cache` (`registry.c:82`, `LIST_INIT`) |
| 6 | kernelbase RVA `0x5CCC8` | `ldr w8, [x25, #0x18]` with `x25 == 0` → **`c0000005`, `info0=0` (read), `info1=0x18`** |
| 7 | `ntdll/loader.c:10025` | `process_attach(node_kernel32)` fails → `NtTerminateProcess` |

Disassembly of the faulting loop (the head is loaded, then walked without a NULL check because a
`LIST_INIT`ed head can never be NULL):

```
18005ccac: adrp x24, ...            ; x24 = &reg_mui_cache  (VA 0x180150ab0)
18005ccb0: add  x24, x24, #0xab0
18005ccb4: ldr  x25, [x24]          ; x25 = head->next  == 0   <-- the twin was never initialised
18005ccc0: cmp  x25, x24
18005ccc4: b.eq <empty-list exit>   ; not taken, because 0 != &head
18005ccc8: ldr  w8, [x25, #0x18]    ; <-- FAULT: read of 0x18
```

### 2.2 Why the head is NULL — the ARM64X dual-`.data` class

`llvm-nm` on the shipped `aarch64-windows/kernelbase.dll` shows the global **twice**:

```
reg_mui_cache      VA 0x180150ab0   RVA 0x150ab0
reg_mui_cache      VA 0x1801525b0   RVA 0x1525b0     delta = 0x1b00
```

`0x1b00` is exactly `MACRUNNER_HB_LOCALE_ENTRY_EC_DELTA` (`kernelbase/locale.c:703`). Every locale global
has the same twin structure (`entry_sintlsymbol` 0x150878/0x152378, `entry_slongdate` 0x14ffd8/0x151ad8,
`intl_key` 0x1528e0/0x1550c8 at the CORE delta `0x27e8`). `macrunner_hb_sync_locale_ec_copies()`
(`locale.c:710-829`) exists precisely to mirror these twins, and its own comment at `locale.c:751` records
the **previous instance of this identical bug**:

> "…RegSetKeyValueW faulted on an unmirrored `entry_sintlsymbol` subkey pointer (kernelbase+0x59930,
> `ldrh w8,[x23]`)."

That one was fixed by adding `entry_sintlsymbol` to the mirror list. **`reg_mui_cache` is not in that list —
`grep -c reg_mui_cache locale.c` = 0** — and it is the next sibling, 0x3398 further along in the same
`.data` region. This is the "bulk-over-reactive" pattern the project charter warns about: the class is being
fixed one crash at a time.

⚠ **A plain `memcpy` mirror would be WRONG for this global.** `LIST_INIT(x)` is *self-referential*
(`{&x, &x}`); copying the native copy's bytes into the twin leaves the twin's pointers aimed at the *native*
address, so `cmp x25, x24` never matches and the walk runs off the end instead of terminating. The twin must
be **`list_init`ed at its own address**, not copied.

---

## 3. Evidence

### 3.1 Offline, 9 runs — deterministic, both architectures

| Run | Program | Arch | `init_locale done` | Faults | Fault address |
|---|---|---|---|---|---|
| K1 | wineboot.exe | x86_64 | 0 | 1 | `0x6FFFFEFBCCC8` |
| K2 | wineboot.exe | **aarch64** | 0 | 2 | `0x6FFFFEFBCCC8` |
| E1 | explorer.exe | x86_64 | 0 | 1 | `0x6FFFFEFBCCC8` |
| E2 | **explorer.exe** | **aarch64** | 0 | 2 | `0x6FFFFEFBCCC8` |
| R | reg.exe | aarch64 | 0 | 2 | `0x6FFFFEFBCCC8` |
| A/B/X/C2 | wineboot/explorer | both | 0 | 1–2 each | `0x6FFFFEFBCCC8` |

**Totals: 15 process-attaches, `init_locale done` = 0, 13 faults, one single faulting address.**
E2 carries the direct line: `err:module:loader_init Initializing system dll for
L"C:\windows\system32\explorer.exe" failed, status c0000005`.

The bootstrap trace (`MACRUNNER_HB_TRACE_BOOTSTRAP=1`, already compiled into the shipped ntdll — env only,
no rebuild) names the module verbatim:

```
macrunner-hb-bootstrap-process-attach-after-initdll: module=L"kernelbase.dll" status=c0000005
macrunner-hb-bootstrap-process-attach-after-deps:    module=L"kernel32.dll"   status=c0000005
bootstrap-loader-stage: after-ntdll-attach          <- ntdll's attach SUCCEEDS
bootstrap-loader-stage: before-kernel32-attach      <- and `after-kernel32-attach` is ABSENT
```

**Refuted along the way (my own pre-registered predictions):** I predicted the native aarch64 build would
survive where x86_64 died — it does not (K2, R). Architecture is **not** the variable, and the earlier
"install the native explorer" fix candidate (journal note-65) would not have helped.

### 3.2 Live, in the running title — and it needed no debug gate

`MESSAGE()` is not suppressed by `WINEDEBUG=-all`, so these counts are readable in ordinary run logs.
In the live run `laneA-INPUT-TEST-manual-language-try10-DESKTOP-155020`:

```
process-attach      = 3      (three processes began kernelbase's DllMain)
init_locale done    = 1      (exactly ONE finished it)
```

Ordered against the harness markers:

```
L1253  mr-run: LAUNCHING THE GAME
L1260  ATTACH-START     process 1  -> never finishes  (dies)
L1306  ATTACH-START     process 2
L1310  init_locale DONE            -> the survivor: the game
L1397  ATTACH-START     process 3  -> never finishes  (dies)   <- spawned after the game started = explorer
```

That third attach, arriving after the game is up and never completing, is `explorer.exe` — and it matches
the independent process watcher, which caught explorer live at `etime 00:01` and gone by the next 1 s poll:

```
15:21:00 *** EXPLORER.EXE SEEN (count=1) *** pid 85189 ppid 1 argv C:\windows\system32\explorer.exe /desktop
```

**Two of three processes in the live run die exactly as they die offline.** The game survives — *why* the
game survives the same code is the one open question (see §6).

---

## 4. The workaround — `artifacts/` only, no engine code

`RegLoadMUIStringW` returns before `load_mui_string` unless the value exists **and** begins with `'@'`. The
prefix template carries **278** such values, all `"@tzres.dll,-NNNNN"`, under
`Software\Microsoft\Windows NT\CurrentVersion\Time Zones\*`. Deleting them makes the faulting call
unreachable.

**New template: `artifacts/hk-prefix-template-NOLANG-GRAPHICSDRIVER-NOMUI`** — a copy of the
GRAPHICSDRIVER template with those 278 lines removed (`system.reg` 49501 → 49223 lines, delta exactly 278;
`.valid`, `WINDOWS-ORACLE-IMPORT.json`, `user.reg`, `userdef.reg` all carried over, so the run contract's
save-snapshot still resolves).

Paired A/B, predictions written before running:

| Arm | Template | `init_locale done` | Faults |
|---|---|---|---|
| CTL | GRAPHICSDRIVER (stock) | **0** | **1** |
| FIX | GRAPHICSDRIVER-**NOMUI** | **2** | **0** |

Under FIX, `wineboot.exe` and `services.exe` both complete `init_locale` and go on to load rpcrt4 /
setupapi / shell32 / ole32 / wininet — i.e. they do real work instead of dying at 1 s. **The crash is gone.**

Cost of the workaround: timezone *display* names fall back to the plain `Std`/`Dlt` values present in the
same keys. Cosmetic, and irrelevant to a game.

### 4.1 ★ The chain does unblock — measured, and it reaches `run_cocoa_app`

Repeating the A/B with `MACRUNNER_HB_X64_LOADER=1` added (the one env var my harness was missing, which is
why x86_64 PEs would not previously start), explorer.exe now launches in **both** arms, so the templates are
the only difference:

| | A — stock template | B — **NOMUI** template |
|---|---|---|
| `explorer.exe` loaded | 1 | 1 |
| `init_locale done` | **0** | **5** |
| `c0000005` faults | 2 | 1 (a *different* one, see below) |
| `winemac` mentions | **0** | **4** |
| loaddll lines (progress proxy) | 22 | **117** |

Arm A's log carries the verbatim control line
`err:module:loader_init Initializing system dll for L"C:\windows\system32\explorer.exe" failed, status c0000005`.

Arm B, by contrast, gets all the way through:

```
trace:loaddll:build_module Loaded L"…\aarch64-windows\winemac.drv" at 0000087FFD…
macrunner-ui-input: stage=winemac_so_load nsapp=0x0 class=(nil) main_thread=0
    0   winemac.so   0x…11205542c  macrunner_sharedApplication_hook + 200
    1   winemac.so   0x…120556dc   run_cocoa_app + 208
```

**`winemac.drv` loads and `run_cocoa_app` executes** — the exact function this lane spent the day proving is
never reached. That is the null-driver chain coming apart, observed rather than argued.

### 4.2 The next wall, precisely named (a second, independent defect)

Arm B then dies on something else entirely:

```
err:seh:dispatch_exception macrunner-hb-first-chance: code=c0000005 flags=0 addr=0x1119DC210 info0=8
macrunner-hb-native-dispatch-boundary-reject: tid=0024 pc=0x1119DC210 frame=0 reason=not-x64-main-process
macrunner-hb-arm64-unwind-unsafe-boundary: pc=0x1119DC210 … action=stop-unwind
err:seh:NtRaiseException Unhandled exception code c0000005 flags 0 addr 0x1119dc210
```

`info0=8` is an **execute** fault, and HyperBridge's native-dispatch guard rejects the callback with
`reason=not-x64-main-process` — explorer is a spawned helper, not the x64 main process. This is
HyperBridge territory (`macrunner_hb.c`, FORBIDDEN to this lane) and it is **not** the kernelbase bug; it
is what is left after it. There is already a repo-root spec that looks related:
`LANE-A-FIX-SPEC-native-dispatch-callback-exception.md`.

---

## 5. What this does NOT yet prove

Stated plainly, because the lane's standard is that silence and inference are not evidence:

1. **Not yet shown: that input works.** §4.1 gets as far as `winemac.drv` loading and `run_cocoa_app`
   executing *inside explorer*, which is a large step, but explorer still dies at §4.2's HyperBridge
   boundary reject, and nothing has yet been demonstrated in the **game** process or with a real keypress.
2. **The `GraphicsDriver`-written check (b5) was a bad test and returned nothing usable.** I looked for a
   new value in the prefix's `system.reg`; explorer creates that key with `REG_OPTION_VOLATILE`
   (`explorer/desktop.c:1059`), so it lives only in the wineserver session and is *never* written to
   `system.reg`. Both arms therefore showed exactly the 1 seeded value, which proves nothing either way.
   This is the same trap the journal already recorded at note-12; I walked into it again.
3. The live confirmation in §3.2 is by process **counting** (3 attach / 1 completion), not by reading
   explorer's own fault line — that line is on `err:module`, which every title run so far has had gated off.
4. The offline harness runs explorer as the *initial* process; in the real run it is spawned by win32u via
   `NtCreateUserProcess`. §4.2's `not-x64-main-process` reject would apply in both cases, but that has not
   been confirmed in a title run.

---

## 6. Open question, and it matters

**Why does the game process survive the identical `kernelbase` attach when explorer, wineboot, services and
reg do not?** All are the same image, and both architectures die offline. The one structural difference
observed: the survivor is the process launched **directly** by `mr-run` via `$WINE "$EXE"`, while the
casualties are spawned indirectly (by the launcher, by `wineserver`, or by `win32u`'s
`NtCreateUserProcess`, which execs a `/var/folders/…/winetemp-*/explorer.exe` copy). **[HYPOTHESIS]** the
ARM64X relocation/view selection differs on the indirect path, so the twin that gets initialised is not the
twin the code reads. Settling this decides whether the real fix is a `list_init` of the twin or something
broader about ARM64X `.data` on the spawn path.

---

## 7. Recommended next steps

1. **One title run on the NOMUI template.** Point a launcher's `MACRUNNER_MR_RUN_PREFIX_TEMPLATE` at
   `artifacts/hk-prefix-template-NOLANG-GRAPHICSDRIVER-NOMUI` (absolute path) and open the gate with
   `WINEDEBUG=+loaddll,+winediag` so a failure explains itself. Read, in order:
   `init_locale done` (expect ≥ 2, was 1) → `stage=dllmain_attach` (winemac.drv PE loading at last) →
   `stage=macdrv_init_entry` → then, and only then, ask the operator for a keypress.
2. **The real fix, for whoever owns `kernelbase`** (⚠ outside this lane's declared territory —
   `winemac.drv/**`, `tools/**`, `reports/**` — so it is deliberately NOT applied here): give the
   `reg_mui_cache` ARM64X twin its self-pointers. Not a `memcpy` — see §2.2:

   ```c
   /* kernelbase/registry.c — call from init, or from the locale EC-mirror path */
   struct list *ec = (struct list *)((uintptr_t)&reg_mui_cache - MACRUNNER_HB_LOCALE_ENTRY_EC_DELTA);
   ec->next = ec->prev = ec;              /* list_init at the TWIN's own address */
   ```
   A defensive `if (!head->next) list_init(head);` at the top of `load_mui_string` would also do it and is
   cheaper to reason about.
3. **Sweep the class, don't fix one more item.** `reg_mui_cache` is the second known casualty after
   `entry_sintlsymbol`. Enumerate every `static` in kernelbase carrying a *self-referential* or
   pointer-valued initialiser that has an ARM64X twin, and cover them once — the charter's
   bulk-over-reactive rule.
4. **The `not-x64-main-process` native-dispatch reject (§4.2) is the next wall and belongs to the engine
   lane** (`macrunner_hb.c`). Spawned helper processes — explorer, services, rpcss — are not the x64 main
   process, so any of them that reaches a native dispatch callback is rejected and killed. Cross-reference
   `LANE-A-FIX-SPEC-native-dispatch-callback-exception.md`.

---

## 8. Artifacts

- Logs: `reports/research/explorer-repro-20260728/` — `K1/K2/E1/E2/R/X` (arch + attach-stage),
  `MUI-CTL.log` / `MUI-FIX.log` (the workaround A/B), `CHAIN-A/CHAIN-B` (chain walk).
- Template: `artifacts/hk-prefix-template-NOLANG-GRAPHICSDRIVER-NOMUI`.
- Scripts (scratchpad): `explorer-repro.sh`, `explorer-arch-ab.sh`, `explorer-attach-stage.sh`,
  `explorer-muifix.sh`, `explorer-chain.sh`.
- Launcher: `reports/phase4-hollow-knight/run-EXPLORER-REPRO-realharness.sh` (try9 clone; blocked by the
  run contract because explorer is not the title — kept for reference).

**No engine file was modified. No commit, no `git add`. The live title run was never touched, and every
Wine teardown was a prefix-scoped `wineserver -k`.**
