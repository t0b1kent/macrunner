# MILESTONE 2026-06-21 — ABZU CrossOver differential oracle: AK-init crash root-caused to the C++ `_initterm` static-init dispatch

**Verdict (evidence, not status):** ABZU runs **correctly** under CrossOver (Wine + Rosetta 2). The
AK/Wwise SoundEngine singleton creator at **rva `0x102400`** IS dispatched there, the singleton globals
are populated, and the game reaches a live D3D11 render window. → The MacRunner crash (NULL-deref of the
uninitialized AK singleton) is a **HyperBridge MISTRANSLATION**, *not* a Wine-layer issue. The exact
divergence is localized to the **C++ dynamic-initializer table walk** (`ucrtbase!_initterm` driven from
the game's CRT startup at `abzugame+0x1d750a4`).

Oracle = CrossOver `wine` is **x86_64** (`.../lib/wine/x86_64-unix/ntdll.so` = Mach-O x86_64) → it runs
ABZU's x64 code under Rosetta 2. Module: `AbzuGame-Win64-Shipping.exe`, **ImageBase `0x140000000`**,
EntryRVA `0x1d7518c`. Bottle: `MacRunnerGames`. HK untouched; golden untouched; MacRunner tree untouched.

---

## Step 1 — does CrossOver get PAST the AK-init point? → **YES (WORKS)**

Run: `AbzuGame-Win64-Shipping.exe -log`, cwd = `Binaries/Win64`, bottle `MacRunnerGames`, `WINEDEBUG=-all`.

- Process sustained: ~190% CPU, **RSS ≈ 2.0 GB** at 60–90 s (MacRunner dies in seconds, pre-allocation).
- Foreground window title (System Events): **`AbzuGame (64-bit, PCD3D_SM5)`** = UE4 D3D11 (SM5) RHI window.
- winedbg thread list shows AK worker threads spawned: **`AK::IOTh…`, `AK::Bank…`, `AK::Even…`**, plus
  `RenderTh`, `SlateLoa`, `TaskGrap`, `PoolThre`. → **AK::SoundEngine::Init ran and created its
  subsystem** (under MacRunner, Sess16: all AK singletons NULL → AK created nothing).

Per the oracle plan branch (1-WORKS): confirmed MacRunner-HyperBridge mistranslation → proceed to trace.

(NB: CrossOver renders via **wined3d + libvkd3d/MoltenVK**, not D3DMetal — irrelevant to the audio-init
question but noted.)

## Step 2 — reference dispatch chain at the creator (`0x140102400`)

winedbg `break *0x140102400; cont; bt` — **breakpoint HIT**. Backtrace (identical frame-1/2 for the
creator and for a sibling init-entry, same `rsp=0x5efec8`/`rbp=0x6fffffa902b8` → one `_initterm` frame):

```
=>0 0x140102400  abzugame+0x102400     creator  (AK SoundEngine singleton factory)
  1 0x6fffffa2b38a ucrtbase+0x2b38a    _initterm loop: insn AFTER the indirect `call [tbl]`
  2 0x141d750a9  abzugame+0x1d750a9    CRT startup: return addr after `call _initterm(__xc_a,__xc_z)`
  3 0x6fffffed1469 kernel32+0x11469    BaseThreadInitThunk
  4 0x6ffffff50da3 ntdll+0x10da3       RtlUserThreadStart
```

Regs at creator: `rbx=0x141ed2fa0` (the `.rdata` descriptor at rva `0x1ed2fa0`, from memory),
`rdi=0x141edb6d8` (= `__xc_z`, the table end iterator — see below).

### The static-init driver (game x64, JIT'd by HB) — `abzugame` rva `0x1d75050..0x1d750a9`

```
0x1d75075  mov  dword [rip+0xd810eb], 1
0x1d75075  lea  rdx,[rip+0x16668c]      ; __xi_z  = 0x141edb708   (C-init table end)
0x1d7507c  lea  rcx,[rip+0x16665d]      ; __xi_a  = 0x141edb6e0   (C-init table start, 5 entries)
0x1d75083  call 0x1d767d0              ; _initterm_e(__xi_a, __xi_z)   [thunk -> ucrtbase]
0x1d75088  test eax,eax ; jne -> bail
0x1d75096  lea  rdx,[rip+0x16663b]      ; __xc_z  = 0x141edb6d8   (C++-init table END)
0x1d7509d  lea  rcx,[rip+0x14b7ac]      ; __xc_a  = 0x141ec0850   (C++-init table START)
0x1d750a4  call 0x1d767ca              ; _initterm(__xc_a, __xc_z)  [thunk -> ucrtbase+0x2b38a]
0x1d750a9  mov  dword [rip+0xd810ad], 2 ; <- return addr seen in backtrace frame 2
```

**C++ static-initializer table = `[0x141ec0850 .. 0x141edb6d8)` = `0x1ae88` bytes = 13777 fnptr entries.**
This matches memory's `.rdata 0x1ec0858` table (~13776). `ucrtbase!_initterm` walks it and calls each
non-null entry; the **creator `0x102400` is one entry**, dispatched directly (backtrace frame 1 = the
`_initterm` call site, no intervening game frame).

### Creator body — `abzugame` rva `0x102400` (what it produces)

```
0x102400  sub  rsp,0x38
0x102404  mov  rcx,[rip+0x280a93d]     ; load singleton G @ VA 0x14290cd48
0x10240b  test rcx,rcx ; jne 0x10241c  ; create only if null
0x102410  call 0x4ec000               ; allocate the singleton object
0x102415  mov  rcx,[rip+0x280a92c]
0x10241c  mov  rax,[rcx]              ; vtable
0x10243b  call [rax+0x10]            ; VIRTUAL dispatch  (= "vtable-dispatched creator", .rdata 0x1ed2fa0)
0x102445  mov  [rip+0x29b5444], rax   ; STORE singleton -> VA 0x142ab7890
0x10244c  mov  [rip+0x29b5435], rcx   ; STORE           -> VA 0x142ab7888
```

→ The creator writes the AK singleton into the global cluster at **`0x142ab7888` / `0x142ab7890`** —
adjacent to **`0x142ab7898`**, the pointer memory recorded as NULL at the MacRunner crash. Creator →
consumer link confirmed end-to-end.

## Step 3 — divergence vs MacRunner

| | CrossOver / Rosetta (correct) | MacRunner / HyperBridge |
|---|---|---|
| `ucrtbase!_initterm(__xc_a,__xc_z)` | walks 13777 entries, **calls the creator** | per Sess18/19 edge probe: creator block never executes; table `0x1ec0858` shows 0/13777 dispatched |
| creator `0x102400` | runs → singleton @ `0x142ab788x` populated | never runs → globals stay NULL |
| consumer (rva `0x109021a`) | NULL-branch **not taken** (bp never hit in 40 s of valid run) | reached with NULL ptr → `c0000005` |

**Why the edge probe "saw" nothing:** `ucrtbase!_initterm` is **native ARM64 Wine code** (PE-Wine, not
JIT'd). The Sess19 edge probe only traces *guest* JIT'd blocks, so it is structurally blind to
`_initterm` — it could only ever observe the creator block *if the host→guest call landed*. It didn't.
This **reconciles** Sess18 (creator = a real `_initterm` table entry — CORRECT) with Sess19 (creator
"never a call target" in the guest trace — an artifact of the probe's blindness to native code), and
retires the "static-init walk is a red herring" conclusion. The static-init walk **is** the mechanism.

## Step 4 — precise Lane A hand-off (MacRunner-side, one JIT block-watch run)

The failure is exactly one of three, all narrowly testable by watching `abzugame` rva
`0x1d75050..0x1d750a9` under MacRunner (block-watch, COLD cache):

- **(a) Bad table bounds.** HB mis-JITs the RIP-relative `lea rcx,[rip+0x14b7ac]` (rva `0x1d7509d`) /
  `lea rdx,[rip+0x16663b]` (rva `0x1d75096`), so native `_initterm` receives wrong `__xc_a`/`__xc_z`
  (empty or inverted range → 0 dispatched). **Check: at the `call _initterm` (rva `0x1d750a4`),
  rcx must = `0x141ec0850`, rdx must = `0x141edb6d8`.**
- **(b) Broken host→guest dispatch.** Bounds correct, but the native(ucrtbase)→guest(x64 fnptr)
  indirect-call transition inside `_initterm` no-ops/swallows the fault for table entries → iterate but
  never enter the creator. Inspect HB's host→guest call thunk for fnptrs read from guest `.rdata`.
- **(c) C++ `_initterm` never reached.** The preceding `_initterm_e(__xi_a,__xi_z)` at rva `0x1d75083`
  or the flag logic at `0x1d75052/0x1d75067` mis-translates and control bails (the `jne 0x1d750b5` /
  `jmp 0x1d75181` paths) before the C++ table. Confirm rva `0x1d750a4` actually executes.

Name the offending instruction → shared-core HB fix → verify: creator runs → `0x142ab789x` non-NULL →
boot advances past AK-init.

---

## Reproduction

```sh
CX="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"
export CX_ROOT="$CX" CX_BOTTLE=MacRunnerGames
export WINEPREFIX="$HOME/Library/Application Support/CrossOver/Bottles/MacRunnerGames"
WIN64="/Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64"
cd "$WIN64"
# behavioral (step 1): just run it -> window "AbzuGame (64-bit, PCD3D_SM5)", AK threads, ~2GB RSS
WINEDEBUG=-all "$CX/bin/wine" "$WIN64/AbzuGame-Win64-Shipping.exe" -log
# trace (steps 2-3): winedbg, commands on stdin
printf 'break *0x140102400\ncont\nbt\ninfo reg\ndisas 0x141d75050,0x141d750b0\nquit\n' \
  | "$CX/bin/wine" winedbg "$WIN64/AbzuGame-Win64-Shipping.exe" -log
# cleanup (scoped, NEVER global pkill): "$CX/bin/wineserver" -k
```

**Gotcha:** raw CrossOver `wine` selects bottles via `CX_BOTTLE` (not `WINEPREFIX`); winedbg `quit`
*detaches* and leaves the debuggee running — sweep with the bottle's `wineserver -k`.

Helper scripts used: `/tmp/abzu-crossover.sh`, `/tmp/abzu-winedbg.sh` (perl-`alarm` self-timeout +
scoped `wineserver -k`; this harness has no `timeout`/`gtimeout`, and foreground `sleep` is blocked).

---

## Step 4 — MacRunner block-watch RESULT: root cause = HB `_initterm` shim clamp (NOT a/b/c)

Run: `MacRunner-abzu` worktree, `engine/wine/dist-arm64ec-spike` (dxmt), COLD cache, `mr-run.sh ...
AbzuGame-Win64-Shipping.exe`, `MACRUNNER_HB_TRACE_LIFT_PROBE=1` (per-guest-x64-block watch).

**The block trace + static read NAMED it — and it is none of (a)/(b)/(c):**

HyperBridge **shims `_initterm`/`_initterm_e` itself** (`macrunner_hb.c:10800`, abzu worktree; **`:10924`
in main**) and the shim has an over-tight upper-bound clamp:

```c
if (!begin || !end || begin > end || end - begin > 0x10000) { *ret = 0; return TRUE; }  // bail: dispatch NOTHING
```

ABZU's C++ initializer table is `[__xc_a=0x141ec0850, __xc_z=0x141edb6d8)` → `end-begin = 0x1ae88`
(110,216 bytes / 13777 entries). **`0x1ae88 > 0x10000` (65536 = 8192 entries) → the guard trips → HB
returns 0 and skips the ENTIRE C++ initializer table**, including the AK creator `0x102400`. The C table
(`_initterm_e`) is only `0x28` bytes → passes → all C initializers run.

Pre-fix block trace (proof):
- C++ `_initterm` call site IS reached: block `0x1d75096` → thunk `0x1d767ca` → return `0x1d750a9`,
  **zero** initializers in between → **rules out (c)**.
- `_initterm_e` dispatched fine (**87** `label=_initterm_e` blocks via the same native→guest call
  mechanism) → **rules out general (b)**.
- C++ `_initterm` dispatched **0** (`label=_initterm` count = 0; creator `0x102400` never appears).

→ **Fourth cause (d): over-tight initializer-table size clamp.** Shared-core: the same clamp is in the
main tree and every worktree (laneD/pe32/d3d12) → any x64 program with >8192 C++ static initializers is
affected (Unity/UE4 AAA, likely HK too).

### Fix (applied + built + deployed in MacRunner-abzu, UNCOMMITTED)

`macrunner_hb.c` clamp `end - begin > 0x10000` → `> 0x400000` (the per-slot loop already validates each
entry: null skip / non-AMD64-module skip / read-fail bail — so this is only a coarse garbage guard).
Rebuilt `dlls/ntdll/ntdll.so`, deployed to `dist-arm64ec-spike/lib/wine/{aarch64-unix,aarch64-windows}`
+ adhoc codesign. Golden untouched.

### Verification (cold re-run) — fix CORRECT, boot MOVED, but a second wall appears before the creator

- C++ `_initterm` dispatch: **0 → 215 distinct initializers** run (582 `label=_initterm` blocks). The
  deterministic AK-init `c0000005 @ rva 0x109021a` is no longer the failure.
- **New wall (unmasked):** at the ~216th C++ initializer, **rva `0x4f43fd`** executes
  `call qword [rip+0x19cb26c]` (an indirect IAT call) to native target `0x87fff94fde4` and HB returns
  **`c000007b reason=non-application-target`** (a separate, known HB dispatch class, ~`macrunner_hb.c`
  x64-dispatch recognition, cf. `:19349`). The shim's `if (status) return FALSE;` (`:10835`/`:10949`)
  then **aborts the entire C++ table walk** → the creator (entry ~#9449) is not reached yet, so the AK
  singleton is still uncreated (boot now diverges here instead of at the AK consumer).

### Remaining work to reach "creator runs → singleton non-NULL → D3D11 window"

1. **Clear the new c000007b** — make HB dispatch the initializer's indirect IAT call to the native
   target `0x87fff94fde4` (non-application-target gap). This is the next wall.
2. **Shim robustness (judgment call):** Windows `_initterm` (non-`_e`) never stops on error — it calls
   every initializer unconditionally. HB's `return FALSE` on a single callback's dispatch failure aborts
   all subsequent initializers (incl. the creator). Consider, for non-`_e`, logging + continuing past a
   callback that fails to dispatch so the rest of the table (the creator) still runs.

### Fix #2 applied + verified — log-and-continue (non-`_e` `_initterm`)

Windows `_initterm` (non-`_e`) never stops on a callback error; HB's `if (status) return FALSE`
(`:10846` abzu / `:10952` main) aborted the whole walk on the first un-dispatchable callback. Changed
to: non-`_e` logs (`WARN`) + `continue`; `_initterm_e` keeps its abort semantics. **Verified (cold
re-run):** the walk continues past the recurring c000007b — **285 initializers dispatched, 13 c000007b
survived-and-continued** (vs 215-then-aborted). But the creator (entry ~#9449) is still not reached: the
boot **stalls** (~0% CPU) around init #285 because the recurring c000007b is an *essential* import
(below) — skipping those callbacks leaves uninitialized/partial critical sections → downstream
`EnterCriticalSection` deadlock. So fix #2 is correct + necessary but not sufficient; **fix #1 (below)
is the real unblock.**

### Fix #1 (follow-on, characterized) — the recurring c000007b = `KERNEL32!SetCriticalSectionSpinCount`

Site: AbzuGame `rva 0x4f43fd`, `mov edx,0xfa0; lea rcx,[rdi+0x10]; call qword [rip+0x019cb26c]` →
IAT slot **rva `0x1EBF678`** = **`KERNEL32.dll!SetCriticalSectionSpinCount`** (import #118/185; runtime
native target `0x87fff94fde4`). HB's x64→native dispatch refuses it with **`c000007b
non-application-target`** inside the nested `_initterm`-callback run (`macrunner_hb_run_x64` sub-call;
the refusal path is the x64-dispatch `non-application-target` branch, ~`:19349`). It is hit by *many*
C++ initializers (every global that sets a CS spin count), so it gates the whole static-init walk. **Fix
direction:** make the x64→native import dispatch recognize `SetCriticalSectionSpinCount` (and the
native-import class) as a valid call target in the nested-callback context = the shared
c000007b/x64-dispatch correctness track (also benefits HK). Then the walk completes → creator #9449 runs
→ AK singleton non-NULL → boot advances toward the live D3D11 window CrossOver reaches.

### Fix #1 IMPLEMENTED + VERIFIED — allow direct-native dispatch from `_initterm` callbacks

Root: `macrunner_hb_label_allows_direct_native()` whitelisted only `x64-wndproc/x64-subclassproc/
x64-signal-callback/dll/thread` — **not** `_initterm`. The nested static-init callback runs with label
`_initterm`, so the guest `call [IAT]` → native `KERNEL32!SetCriticalSectionSpinCount` failed the
label gate at `:19533` and fell to the `non-application-target` refusal (`pc_is_native_pe_builtin`
would have accepted the target — kernel32 is an ARM64X builtin in an exec section; the label was the
only blocker). **Fix:** add `_initterm`/`_initterm_e` to the allow-list (target still gated by
`pc_is_native_pe_builtin`).

Verified (ABZU, cold, dxmt):
- `non-application-target` c000007b **= 0** (the `SetCriticalSectionSpinCount` refusal is gone).
- The C++ static-init walk **no longer deadlocks**; full-speed run advances from the AK-init
  crash/stall to **sustained engine init — a full 120 s run, clean timeout (exit=143), RSS ~450 MB**
  (vs the prior `#285` deadlock). → the static-init table completes, so the AK creator (`0x102400`)
  runs and the singleton is created. (Literal `rva=0x102400` capture was budget-limited — the creator
  is deep in the 13777-entry table and a 600000-block lift-probe budget didn't reach it; the
  no-deadlock + deep-engine advancement is the proof.)
- **New, deeper wall** surfaced (separate class): `c000007b reason=runtime` at game rva `0x58576f`
  (~90045 blocks deep, engine code — well past static-init/AK-init; possibly timing-dependent — the
  full-speed run reached the 120 s cap without it). This is the next frontier.

### Commits / upstream

- **abzu-lane `8d18309`** — clamp `0x10000→0x400000` + log-and-continue. **abzu-lane `8f14270`** —
  fix #1 (`_initterm` direct-native allow). `macrunner_hb.c` only; abzu dist rebuilt + redeployed
  (`ntdll.so` aarch64-unix/windows, adhoc-signed). Golden untouched. Durable on abzu-lane.
- **main (canonical hyperbridge)** — all three edits applied to `macrunner_hb.c` **source**
  (non-disruptive: HK's live `dist-arm64ec-spike` NOT rebuilt — `.so` mtime unchanged; next clean build
  picks them up). NOT git-committed on main: the file has unrelated uncommitted Lane WIP
  (`macrunner_hb_syncmeter_wfso`) that must not be bundled, and `git add -p` is unavailable to isolate
  the hunks — and committing now would disrupt HK's live HEAD. Pending operator's commit decision.

Repro (MacRunner side):
```sh
cd MacRunner-abzu
COLD=$(mktemp -d artifacts/_hbcache_cold.XXXXXX)
MACRUNNER_GRAPHICS_BACKEND=dxmt MACRUNNER_MR_RUN_SKIP_WINEBOOT=0 \
MACRUNNER_HB_TRANSLATION_CACHE_ROOT="$COLD" \
MACRUNNER_HB_TRACE_LIFT_PROBE=1 MACRUNNER_HB_TRACE_LIFT_PROBE_BUDGET=100000 WINEDEBUG=-all \
  scripts/mr-run.sh engine/wine/dist-arm64ec-spike \
  /Users/timurtoby/Documents/MacRunner/Main/ABZU/game/AbzuGame/Binaries/Win64/AbzuGame-Win64-Shipping.exe 75 \
  2>&1 | grep -aE 'label=_initterm|run-exit|\[mr-run\]'   # count label=_initterm: 0 before fix, 215+ after
rm -rf "$COLD"
```

---

## Next wall (`0x58576f`) — CHARACTERIZED (measure-first): timing-dependent NULL-`this`, not a hard blocker

**WHAT.** `c000007b reason=runtime` (`macrunner_hb.c:20647`) = a JIT'd guest block faulting at **runtime**
(HB's generic "run failed" status), *not* a host→guest dispatch refusal — a different class from
static-init's `non-application-target` (`:19593`) and from HK's CHPE/dispatch c000007b. The faulting block
is a **NULL-`this` virtual call** in a real UE4 method (`0x585670`, has `__security_check_cookie`):
```
0x5856a4: mov  r14, rcx          ; r14 = this (the method's 1st arg)
...
0x585779: mov  rcx, r14
0x585781: mov  rax, [r14]        ; r14 = 0x0  -> NULL deref
0x585787: call [rax+0x10]        ; virtual dispatch
```
`r14 = rcx` = the function's first argument, so the **NULL `this` is passed by the caller (upstream)** —
not produced in this function.

**DETERMINISM.** Intermittent / timing-dependent. **3/3 full-speed runs are CLEAN** (pass `0x58576f`, run
to the 80 s timeout, 0 c000007b); only the *slow* lift-probe run faulted. The per-block logging widens a
race window that yields the NULL `this`.

**CLASS.** A timing-dependent **guest-state corruption/race** (NULL object passed as `this`), matching the
shared "timing-dependent c000007b corruption" family (cf. HK) — NOT the static-init dispatch class just
fixed, and NOT a deterministic ABZU UE4-codegen bug. CrossOver/Rosetta runs ABZU past this into a full
window, so MacRunner produces a NULL where Windows has a valid object.

**Implication.** `0x58576f` is a **latent race surfaced under instrumentation**, not the real boot
frontier. At full speed ABZU runs sustained engine init without hitting it. The real frontier is later —
being measured with a longer full-speed run. (The latent race is a real correctness item for the shared
timing-corruption track, but not the immediate boot blocker.)

---

## Engine-init "grind" PINNED (measure-first: native `sample` + heartbeat + CrossOver diff) — it's HB per-callback churn, NOT a UE4 function

**Method.** Heartbeat (`MACRUNNER_HB_TRACE_HEARTBEAT`) fired 0 lines — the grind is many *short* nested
`run_x64` calls, so the per-call heartbeat never trips. Switched to a **native `sample`** of the live
grinding process (the tool that found HK's hot spots).

**Result — the hot spot is HyperBridge overhead, not guest code.** Heaviest non-idle native leaves:
`__write_nocancel 2802, hb_cache_get 1279, hb_jit_runtime_destroy 700, fsync 554, __open 228,
__rename 173, __read 768, __munmap 55`. Call tree of the busy thread:
```
macrunner_hb_run_x64 + 6868
  -> hb_jit_runtime_create 2319
       -> hb_cache_open 2205
            -> load_entries 960   (re-reads/parses the on-disk translation cache)
       ... cache store -> __write_nocancel + fsync + __rename (cold cache) ...
  -> hb_jit_runtime_destroy
```
The `_initterm` shim (`macrunner_hb_try_msvcrt_exit_semantic`) is a dominant ancestor — it calls
`run_x64` **per static-init callback (13777×)**, and **every `run_x64` call does `hb_jit_runtime_create`
+ `hb_cache_open`→`load_entries` (re-reads the on-disk cache) + cold-cache `store`→`write`+`fsync`+
`rename` + `hb_jit_runtime_destroy`**. (The main thread, separately, is mostly *blocked* in
`NtWaitForSingleObject`→`server_wait` — waiting, not burning CPU.)

**Reframe.** The ~200 s "engine-init grind" is **still the C++ static-init walk** — static-init itself
costs ~13777 × (runtime+cache create/open/load/store/destroy) ≈ ~200 s, so ABZU never reaches UE4 engine
init / graphics within 240 s. It is *not* post-static-init UE4 code.

**CrossOver diff (quantification).** Rosetta reaches the `PCD3D_SM5` window in **~tens of seconds**
(~32–46 s cold; near-instant warm) — it has no per-callback JIT-runtime/on-disk-cache churn. MacRunner
spends **240 s+ on static-init alone** and never reaches the window. Gap on the engine-init phase ≈ **>5×**
(and unbounded since it doesn't complete).

**Throughput target (shared-core, benefits HK).**
1. **Reuse one ctx/JIT runtime across the `_initterm` table walk** instead of a fresh `run_x64`
   (create+cache-open+`load_entries`+destroy) per callback — the biggest lever, directly in the shim.
2. **`hb_cache_open` re-runs `load_entries` on every call** — cache the loaded index process-globally /
   lazy-load so it isn't re-parsed per `run_x64`. (Worsens with a *warm* cache — more entries to reload.)
3. **Cold-cache `store` does per-block `write`+`fsync`+`rename`** — batch/async the persistence, drop
   per-block `fsync`. (Cold-specific; a warm cache avoids it but pays #2 more.)

Caveat: measured with a COLD cache (per operator's prior guidance); #1/#2 are cache-warmth-independent
(create/open/load/destroy per call), #3 is cold-specific. The per-callback churn (#1/#2) is the core
shared HB-throughput issue — the same family as HK's translation/IR-cache throughput work
(block-cache/ir-cache grows committed for HK).

---

## Throughput fix #1 IMPLEMENTED + VERIFIED — pool JIT runtimes for nested run_x64 frames

**Root (from the `sample`):** the per-thread JIT-runtime pool (`tls_jit_rt`) only covered the OUTERMOST
`run_x64` frame; nested frames (the busy-guard fallback, `macrunner_hb.c`) did
`hb_jit_runtime_create`+`destroy` a fresh runtime each. The `_initterm` shim dispatches 13777 callbacks
via nested `run_x64` → 13777× (`hb_cache_open`→`load_entries` [O(N²) on the growing on-disk cache] +
128 MB MAP_JIT mmap + teardown).

**Fix (abzu-lane `b6c9805`; main source applied, non-disruptive):** a per-thread free-list pool (cap 2)
of runtimes for nested frames. Acquire pops+`hb_jit_runtime_reset`s a free runtime (keeps
`persistent_cache` open → no re-`load_entries`; rewinds arena → SMC-safe; re-points ctx) or creates one;
release returns it (or destroys past the cap). Kept separate from `tls_jit_rt` so a nested frame never
clobbers the outer frame's pooled runtime. Extends the existing reset-not-recreate pool (added for HK's
Unity scene-load) to the nested re-entrant case it had punted on.

**Verified (ABZU, cold, dxmt):**
- **translation-cache opens: 13777 → 5** (decisive — per-callback cache churn eliminated).
- The churn functions (`hb_cache_open`/`load_entries`/`hb_jit_runtime_create`/`destroy`/`store` +
  `write`/`fsync`) are **gone from the profile** (re-`sample` after the fix).
- **No regression:** boot reaches the same point, no new crash (the `0x58576f` c000007b that appears is
  the pre-existing intermittent NULL-`this` race, not caused by this fix).
- The CPU-bound static-init phase shortened (cache I/O removed).

**Honest end-to-end:** the fix does NOT by itself reach the render window. After the now-faster
static-init, the boot is gated by the **next** bottlenecks — the intermittent `0x58576f` race and
**wineserver-wait-bound** execution (`macrunner_hb_run_x64`→`call_import_thunk`→
`try_kernel32_handle_semantic`→`NtWaitForSingleObject`→`server_wait`→`read`). The remaining static-init
CPU is JIT lift+exec of the 13777 callbacks (compute), no longer cache I/O. **Shared-core throughput win
(benefits HK's nested callbacks); a correct, necessary layer peeled — not the last one.**

**Follow-on levers:** (#2) `hb_cache_open`/`load_entries` now runs only ~5× — much less critical, but a
process-global/lazy cache index is still cleaner. (next) the wineserver-wait pattern (per-sync server
round-trips) and the static-init *compute* (13777 callback lift+exec) are the new throughput/latency
targets; plus the `0x58576f` timing-corruption race (secondary track).

---

## Wineserver-wait profile (measure-first) + HK perf-series merge — the gate is cold-cache JIT compute, NOT sync

**Wait hypothesis REFUTED by count.** Added an env-gated counter (`MACRUNNER_HB_TRACE_SYNCCOUNT`) in
`try_kernel32_handle_semantic`. Over 48 s of grind: **total=35000 handle-semantic calls, wait=75**
(`wait_per_s`≈1). The traffic is dominated by **`TryEnterCriticalSection`/`LeaveCriticalSection`**
(uncontended, fast — not server round-trips). So the boot is NOT wineserver-round-trip-bound; the earlier
`sample` showing `NtWaitForSingleObject` caught a *single late long wait* (the thread blocks once after the
compute phase), not many round-trips. (Counter reverted after measuring; abzu source unchanged.)

**HK perf-series merged into ABZU (`coordinate, don't dup`).** abzu-lane was only 3 commits behind main
(it already had find_region-treap / module_from_pc-cap / IR-cache-grow / incremental-VM-map). Merged main
→ abzu-lane (`48f5dcc`), bringing HK's `3eeefc6` (mach_vm region cache), `2b9fbe8` (context-snapshot
de-contention + module-classification cache), `51605cd` (PC-classification cache). Trivial comment-only
conflict in `macrunner_hb.c` (both branches already carried the runtime-pool fix). Combined tree verified:
HK region-cache + the 4 `_initterm` fixes all present.

**Result: the grind PERSISTS on the merged engine** (full-speed: ~98 % CPU, RSS ~440 MB, no window, hits
the intermittent `0x58576f`, exit=143 timeout — identical to pre-merge). → ABZU's gate is **not** the
cached sinks HK fixed, **not** sync waits, **not** the CS strieq-chain (CS ops complete fast). It is
**cold-cache JIT compute** — the actual lift+exec of the 13777 static-init callbacks' code (the
"double-emulation floor"), which `sample` can't symbolize (scattered MAP_JIT addresses).

**Next levers (revised, measure-grounded):**
1. **Warm translation cache** — ALL runs here were COLD (per the determinism guidance). The persistent
   cache makes re-lift a disk hit instead of re-translation; a warm re-run should skip most of the
   13777-callback lift compute. UNTESTED — the highest-value cheap next measurement.
2. The cold-cache lift compute itself (if warm doesn't suffice): faster lifter / batched translation.
3. The late single `WaitForSingleObject` block (a producer-wait, post-compute) + the intermittent
   `0x58576f` NULL-`this` race (shared timing-corruption track).

Shared-core: the runtime-pool win (`b6c9805`) helps HK's nested callbacks; HK's region-cache/caches now
in ABZU. The remaining JIT-compute floor is the convergent frontier for both.

---

## Warm-cache A/B (the highest-value lever) — RESULT: JIT throughput is NOT the gate; the wall is `0x58576f` + idle block

Method: persistent translation-cache root. Run #1 COLD (populate), run #2 WARM (same root). Lift is
cached per-block, so warm = exec-only (skips re-translation).

| | cold #1 | warm #2 |
|---|---|---|
| cache after | **6.0 MB** | **6.0 MB (unchanged)** |
| new lifts | populated | **ZERO** (all cache hits) |
| reached | `0x58576f`, ground to 180 s cap | `0x58576f` at ~85 s, then **idle 0 % CPU** to cap |
| window / graphics | none | none |

**Findings (decisive):**
1. **LIFT is fully cacheable** — the warm run added ZERO bytes to the cache (every block a hit). So JIT
   *translation* throughput is **not** the gate to the first frame.
2. **Warm does not reach further** — same `0x58576f` wall, no window. After the ~85 s one-time static-init
   *exec*, the thread goes **fully idle (0 % CPU)** for the rest of the run — it is **not** exec-bound past
   that point either; it **blocks**.
3. ⇒ The window-blocker is **NOT JIT throughput (lift or exec)**. It is the **`0x58576f` NULL-`this`
   (r14=0) event + the subsequent idle block** (the late single `WaitForSingleObject` the earlier sample
   caught). The boot reaches `0x58576f`, the c000007b is recovered, then the thread blocks and never
   progresses to graphics.

**Reframe of the frontier:** the "compute grind" was the one-time static-init exec (the *path* to
`0x58576f`); a faster lifter would NOT help (lift is cached/free when warm). The **`0x58576f` NULL-`this`
race — previously filed secondary — is the actual gate** to ABZU's first frame: fix the NULL `this`
(r14=0 passed by the caller) so the boot advances past it, then chase whatever it then blocks on. This is
the shared timing-corruption track (cf. HK's c000007b family). CrossOver runs past `0x58576f` to a full
PCD3D_SM5 window, so MacRunner produces the NULL where Windows has a valid object.

---

## `0x58576f` root-cause via CrossOver diff — the NULL `this` is an engine-init object in the main-init chain

CrossOver `break *0x140585670` (the function that faults `mov rax,[r14]` with r14=this). Hit **once**
(one-time engine-init call), with **`this`(rcx)=`0x020d60e0` — a valid heap object** (where MacRunner has
NULL). Backtrace (the producing chain):
```
0x585670  <- 0x4f48df <- 0x4d3889 <- 0x4de4e4 <- 0x4eba9b <- 0x13eeed <- 0x146ebd <- 0x1413ea
          <- 0x141a7a <- 0x14abb0 <- 0x1d75137 (CRT 'call main') <- BaseThreadInitThunk <- RtlUserThreadStart
```
This is the game's **main/engine init** (from the CRT `call main`) — the *same chain* MacRunner's old
AK-init crash was on (`0x1d75123→0x14aae0…`), now reaching far deeper after the `_initterm` fixes.

Immediate caller `0x4f48df` (disasm): `... lea rcx,[rsi+0x28]; call SetCriticalSectionSpinCount(4000);
... mov rcx, rsi; call 0x585670` — so **`this` = `rsi`**, the caller's *own* object which it is
initializing (sets members + a CS). The valid object propagates *down* the chain as `rsi`/`this`. Under
MacRunner this is NULL at `0x585670`.

**Status & next step (MacRunner-side trace, the divergence).** CrossOver names the valid object + producing
chain. The remaining work is the operator's step 1b: trace the *same chain* under MacRunner to find the
frame where `rsi`/`this` becomes NULL (where MacRunner diverges from the valid CrossOver object). MacRunner
has no winedbg for guest x64 (JIT'd), so this needs a targeted register probe at the chain frames
(`0x585670` prologue + `0x4f48df`/`0x4d3889`/…/`0x14abb0`) dumping rcx/rsi. Two hypotheses to settle:
- **Race** (the wall is intermittent — some runs reach `0x58576f`, some time out in static-init first):
  the object is produced asynchronously (a worker the main-init spawns) and the main thread reads NULL
  before it's published = a visibility/memory-ordering issue → the **shared corruption/memory-ordering
  track** (cf. the lock-prefix/atomic work, HK's timing-dependent c000007b).
- **Deterministic producer-gap**: a constructor/factory/vtable-dispatch in the chain isn't run under
  MacRunner (another dispatch gap like the AK creator was) → the object is never created → NULL.

Cross-check vs HK's c000007b: HK's were *dispatch* refusals (CHPE / non-application-target); ABZU's
`0x58576f` is a guest NULL-`this` deref mapped to c000007b (`reason=runtime`) — a different *immediate*
cause, but if it proves to be a race they share the timing/visibility substrate. The fix (a valid object
like CrossOver) → past `0x58576f` → no idle block → graphics → the proven present path → ABZU's first frame.

---

## `0x58576f` ROOT-CAUSED (MacRunner register probe) — NULL global singleton, constructor never runs (producer-gap)

Env-gated chainwatch probe (`MACRUNNER_HB_TRACE_CHAINWATCH`, reverted after) dumping `this`/regs/return-addr
at the chain RVAs. Result:

- At `0x585670` **entry under MacRunner**: `this`(rcx)=**NULL**, `rsi`=`0x142545f30` (valid, ≠rcx) — so it
  is **NOT** CrossOver's `0x4f48df` (`mov rcx,rsi`) path. Return addr = **rva `0x575548`** — a *different*
  caller than CrossOver's `0x4f48df`.
- MacRunner caller `0x575518`: **`mov rcx, [rip+0x23c9881]` → `this` = `*(global VA 0x14293eda0)`** then
  `call 0x585670`. The global `0x14293eda0` is a **`.data` (writable), zero-init singleton pointer slot** —
  **NULL under MacRunner**, valid (`0x020d60e0`) under CrossOver.
- The chainwatch caught `0x585670` **only** from the consumer (`0x575548`), **never** from the constructor
  path (`0x4f48df`, a call target that would re-enter the dispatcher). ⇒ under MacRunner **the constructor
  that creates+stores the singleton at `0x14293eda0` never runs.**

**Verdict: deterministic PRODUCER-GAP, not a race.** The singleton's constructor (CrossOver's
`0x585670 ← 0x4f48df ← … ← 0x14abb0 ← CRT main` chain) does not execute under MacRunner, so the `.data`
slot `0x14293eda0` stays NULL; a consumer at `0x575518` loads NULL and calls `0x585670(this=NULL)` →
`c000007b` at `0x58576f` → the boot then idles. **Same family as the AK creator** (a creator not running
leaves a global NULL) — but a *different* singleton and in **main/engine init** (not static-init), so the
`_initterm` fixes don't cover it.

Cross-check vs HK c000007b: HK's were *dispatch refusals*; this is a *control-flow divergence* in main-init
(a constructor not reached) → a different mechanism. Not the same root.

**Fix direction (next step):** find WHY the main-init chain diverges before the constructor `0x4f48df`
(MacRunner reaches CRT main + grinds, but doesn't reach `0x4f48df`). Trace the chain
`0x14abb0 → 0x141a7a → 0x1413ea → 0x146ebd → 0x13eeed → 0x4eba9b → 0x4de4e4 → 0x4d3889 → 0x4f48df` under
MacRunner (chainwatch the *function entries*, which are call targets) to find the frame where MacRunner
branches away from the constructor. Likely a mistranslated branch / a sub-call returning early or NULL that
skips the construction (the operator's "mistranslated branch skipping the setup" hypothesis). Fix → the
constructor runs → `0x14293eda0` populated → `0x575518` consumer gets a valid `this` → past `0x58576f` →
no idle block → graphics → present → ABZU's first frame.

---

## `0x58576f` constructor-divergence TRACED (6 levels) — a lock-protected `.data` counter is ≤1 under MacRunner

Chainwatch on the constructor-path **function entries** (env-gated probe, reverted) localized the divergence:

1. **`0x58576f`**: `c000007b` NULL-`this` (r14=0) → boot idles, no first frame.
2. `this` = `*(.data singleton 0x14293eda0)`, loaded by consumer `0x575518` (`mov rcx,[rip+0x23c9881]`) — NULL under MacRunner.
3. The singleton's **constructor** (`…→0x4f48df→0x585670`) is **skipped**: chainwatch fired `0x145bc0`(entry)→`0x145c61`→`0x145c6f`→**`0x145ebb`**(a skip-forward target)→`0x585670`(consumer). So branch **`0x145ce1: je 0x145ebb` is TAKEN** under MacRunner, bypassing the constructor path (`0x145ce7→…→0x13ecc0→…→0x4f48df→0x585670`).
4. That `je` tests `al` from **`call 0x5844a0` (@`0x145cc8`)**: `0x5844a0` returns **0 (FALSE)** under MacRunner (≠0 on Windows). → operator hypothesis (b): **a sub-call returns the wrong value**.
5. `0x5844a0` = a **lock-protected accessor**: `TryEnter/Enter/LeaveCriticalSection` on CS `0x14293f0b0` (= the `rcx=0x14293f0b0` seen at `0x145ebb`), then `mov ebx,[rip+0x243d009]` (= `.data` counter **`0x1429c14f8`**); `cmp ebx,1; setg al` → **returns `(counter > 1)`**.
6. **Counter `0x1429c14f8`** (`.data`, BSS/zero-init, runtime-incremented under the CS) is **≤1 under MacRunner, >1 on Windows** — a registration/refcount that is **under-incremented under MacRunner**. The singleton/CS/counter all live in one `0x14293exxx`–`0x1429c1xxx` `.data` cluster.

**The branch is correct; the divergence is DATA** — counter `0x1429c14f8` is wrong. **Next step (needs a different tool):** runtime **memwatch on `0x1429c14f8`** (catch its writer + value progression), or Ghidra/IDA recursive xref (linear `.text` disasm drifts on the 32 MB section). Determine: **producer-gap** (a registration that ran fewer times under MacRunner — ABZU-specific) vs **visibility/TSO** (lock-protected writes not visible — shared with HK's TSO/c000007b track). Currently leans registration-count (the lock+counter shape); the lock means a TSO/visibility miss is also possible. Fix the counter → `0x5844a0` returns true → constructor runs → singleton populated → past `0x58576f` → graphics → present → ABZU's first frame.

---

## Counter `0x1429c14f8` memwatch — PRODUCER-GAP confirmed (counter=0, not TSO); shared with HK

Static xref: the counter has **one writer** — `0x58cc39` (`mov [0x1429c14f8], edx`) inside function `0x58cbd0`,
which does `edx = [rcx+8]; counter = edx` (publishes `arg->count`). `0x58cbd0` has 7 callers (early ones at
`0x143342/0x14336b/0x1437f4/0x1439db`, plus `0x145deb` which is *inside* the `je 0x145ebb`-skipped region).

Runtime memwatch (env-gated probe, reverted) on the publisher `0x58cbd0` + gate `0x5844a0`, reading the live
counter:
```
GATE(0x5844a0) counter=0 ...
GATE(0x5844a0) counter=0 ...
PUBLISH(0x58cbd0): 0 hits
```
**The gate reads `counter=0` — the pristine BSS value — and the publisher never wrote it (0 hits).**

**Verdict (settles the operator's HK cross-check): PRODUCER-GAP, NOT TSO/visibility.** A TSO/visibility miss
would surface a *written-but-stale* value; `counter=0` is the untouched initial value, so the write simply
**never happened** — the publisher `0x58cbd0` does not run/publish before the gate. This matches HK's refuted-TSO
finding (a constructor/producer that doesn't run, not a memory-ordering bug).

**Systemic pattern (the session's through-line):** this is the *third* producer in ABZU's main-init that fails
to run under MacRunner — the AK SoundEngine creator (Sess16-19, pre-`_initterm`-fix), the singleton-`0x14293eda0`
constructor (`0x4f48df` chain), and now the counter publisher (`0x58cbd0`). They form a **cascade of producer-gaps
in main/engine init**: a constructor/publisher/registration is skipped → a global stays NULL/0 → a downstream
gate/consumer takes the wrong path or derefs NULL → `0x58576f`. **Shared-core class with HK** (uninit-slot /
constructor-not-run producer-gap).

**Next (the cascade's first cause):** the mechanism recurses — `0x58cbd0`'s caller chain is itself skipped. Rather
than recurse another level by hand, the efficient attack is a **full main-init differential trace vs the CrossOver
oracle**: break at CRT-main under CrossOver and at the FIRST producer that diverges, compare the control flow to
MacRunner's, and find the single earliest branch/condition where MacRunner's main-init first departs from Windows.
That earliest divergence is the cascade's root; one fix there likely unblocks the whole chain (and HK's, same class).
Fix → producers run → globals populated → past `0x58576f` → graphics → present → ABZU's first frame.

---

## Main-init differential trace (vs CrossOver) — corrected divergence + the circular seed

Top-level main-init (`0x14abb0→0x141a60→0x1412e0→0x145bc0`) runs on **both** (chainwatch fired all under
MacRunner). The divergence is **inside `0x145bc0`** — and the trace+disasm (not pre-labeled) name it:

- `0x145bc0` gate at `0x145cc8` = `call 0x5844a0` (returns `counter>1`), then `0x145ce1: je 0x145ebb`.
- **je-taken** (counter≤1): `0x145ebb → … → 0x14602a` = the function's **early-return epilogue** (restore
  callee-saved + `__security_check_cookie`) → **returns, SKIPPING the publisher (`0x146ce1`) and the
  singleton ctor (`0x146eb8`)**.
- **je-not-taken** (counter>1): `0x145ce7 → … → 0x146ce1` (publisher seeds counter) `→ 0x146eb8` (ctor).

**MacRunner** (counter=0) takes the je → early-return → skips publisher+ctor. **CrossOver** (counter>1)
runs the full path. (CrossOver winedbg: publisher `0x58cbd0` first hit is from `0x146ce1` inside `0x145bc0`.)

**The cascade is circular:** the counter is gated by `0x5844a0` (counter>1) but only *written* by the
publisher `0x58cbd0`, which is only reached on the je-not-taken (counter>1) path. So the counter must be
**seeded >1 by an EARLY publisher caller before `0x145bc0`** — the publisher's other call sites are
`0x143342 / 0x14336b / 0x1437f4 / 0x1439db` (in the `0x143xxx` early-main-init region) + `0x145deb /
0x1468ce / 0x146ce1`. On CrossOver an early `0x143xxx` call seeds the counter; **on MacRunner that early
seed does not happen** → counter stays 0 → every later `0x145bc0`-class gate fails → the whole cascade.

**The earliest divergence is the `0x143xxx` early seed call** (or higher). NEXT: chainwatch the `0x143xxx`
publisher-caller functions under MacRunner (do they run + call `0x58cbd0`?), and CrossOver single-step the
first `0x145bc0`/seed to confirm where the counter first becomes >1. That early seed's non-execution is the
cascade root (producer-gap, not TSO — consistent with the counter=0 memwatch). Fix it → counter seeded →
gates pass → producers run → past `0x58576f` → graphics → present → first frame; same class as HK.

---

## Seed chainwatch + CrossOver order trace — TWO corrections; divergence = a virtual call in 0x145bc0 (HK [r15+0x10] class)

**MacRunner seedwatch** (env-gated, reverted): the early publisher-caller functions `0x143310 / 0x1434e3 /
0x143985` **never ran** (0 hits); publisher `0x58cbd0` **0 hits**; gate `0x5844a0` ran 2× reading **counter=0**
(ret `0x145ccd` = the `0x145bc0` gate, ret `0x4dccdf` = a 2nd gate site `0x4dccda`).

**CrossOver order trace** (break seeds + `0x145bc0` + publisher): first hit = **`0x145bc0`**, then publisher
`0x58cbd0` from **`0x146ce1` *inside* `0x145bc0`**. The seeds `0x143310/0x143985` did **not** hit before
`0x145bc0`.

**Correction #1 — the seeds are NOT the cause.** The counter is first seeded by `0x145bc0`'s **own** call to
the publisher at `0x146ce1`; the `0x143xxx` seed functions run *after* `0x145bc0` (so their non-execution under
MacRunner is a *consequence* of the boot dying, not the cause).

**Correction #2 — the `je 0x145ce1` is NOT the divergence.** `0x14602a` is **not** the epilogue — `0x145bc0`
continues past it (`0x146091, 0x1460bd, … 0x146ce1, 0x146eb8`). Both `je` paths **converge** at `0x14602a` and
reach the publisher+ctor. CrossOver's first `0x145bc0` (counter=0, je taken) still reaches `0x146ce1`.

**The real divergence:** MacRunner's `0x145bc0` reaches `0x145ebb` but **not `0x146064`** (bracket cw6) — it
takes a different path in **`0x145ebb..0x146064`**, skips the publisher (`0x146ce1`) + ctor (`0x146eb8`), and
returns (the consumer `0x575518` then derefs the NULL singleton → `0x58576f`). That ~0x1a0-byte range has 16
sub-calls — **5 of them INDIRECT/VIRTUAL**: `0x145edb call [rdx]`, `0x145fd6 call [rax]`, `0x145fee call
[rax+8]`, `0x146010 call [rax]`, `0x146027 call [rax+8]`.

**★ Unifying hypothesis (the operator's HK cross-check):** these are **indirect vtable dispatches — the same
class as HK's `[r15+0x10]` `c000007b`**. A virtual/indirect-call divergence under MacRunner (NULL/wrong vtable
target, or non-return) is the likely shared root across ABZU's cascade *and* HK. NEXT: bracket the
`0x145ebb..0x146064` sub-calls to find which returns differently / steers the path away (prime suspects = the 5
virtual calls); disasm the object/vtable feeding it; cross-check HK's `[r15+0x10]`. Producer-gap, not TSO.

**Honest status:** the control-flow model was corrected twice this turn; "one earliest divergence" is proving to
be a deep in-function cascade, now narrowed to a virtual-call divergence inside `0x145bc0` (`0x145ebb..0x146064`).

---

## Virtual-call bracket — chaining METHOD WALL; divergence pinned to 0x145bc0's path region (0x142a00), method change needed

Bracketed `0x145bc0`'s `0x145ebb..0x146091` path blocks + the 5 virtual calls (env-gated probe, reverted).
**Result: only `0x145ebb` fired** (rbx=`0x1027e4292`, `[rbx]=0x5c003a005a0000` = UTF-16 `"Z:\…"` path), then
`c000007b` (`0x58576f`). None of the successor blocks (`0x145ed0/0x14602a/…`) nor the 5 virtual calls fired.

**Method wall:** the chainwatch only sees **dispatcher-entry** blocks; successor/fall-through blocks are
chained (executed but invisible), so hand-bracketing can't trace the intra-`0x145bc0` path reliably. (No
"chain"-named knob in libhyperbridge to disable it.)

**What IS decisive (logic, not the trace):** `0x58576f` firing ⇒ singleton NULL ⇒ publisher+ctor skipped ⇒
`0x145bc0` genuinely diverges **after `0x145ebb`**. The last reliable point is `0x145ebb → call 0x142a00`
(rcx = the `"Z:\…"` path). **`0x142a00` is the prime suspect** — and disasm shows it is itself a **by-name
factory lookup + virtual dispatch**: `call 0x50b0b0(rcx, "<str-const>")` → object `rdi`; `je` if NULL; else
`call [r9+0x18]` then `call [r9+0x20]` (r9=`[rdi]`=vtable) with the path. So the divergent path is dense with
indirect/vtable dispatch at *every* level (AK creator → singleton ctor → counter publisher → `0x145bc0` →
`0x142a00`), consistent with HK's `[r15+0x10]` class — **but the single failing instruction is NOT yet
isolated** (the "5 virtual calls in 0x145bc0" may be downstream of an earlier divergence at `0x142a00`).

**★ Method change (hand-bracketing isn't converging — 3 model corrections):** the right tools for this depth:
1. **Instrument HB's indirect-call resolver** (best, chaining-agnostic): in the JIT/dispatch path, when an
   indirect `call [mem]`/`call reg` resolves for a block in the game module's range, log `block_pc, target,
   object_ptr`. This captures EVERY vtable dispatch's resolved target directly — names the failing one
   regardless of chaining. (libhyperbridge indirect-branch handler.)
2. **CrossOver single-step** the reference path `0x145ebb → 0x142a00 → … → 0x146ce1` (winedbg `stepi`/`x`) to
   get the exact instruction sequence + what `0x142a00`/`0x50b0b0` return on Windows, then a TARGETED
   MacRunner probe at that one instruction.
3. Disable block-linking for a full reliable block-trace (mechanism not named "chain"; needs source dig).

Producer-gap (not TSO) stands. The shared-root-with-HK (indirect dispatch) hypothesis is *supported by the
recurring pattern* but not yet proven to a single instruction. NEXT = instrument the indirect-call resolver.

---

## DISCRIMINATING FORK + CrossOver diff — root is the vtable dispatch 0x142a56 → 0x19820c0 (HK [r15+0x10] class)

**MacRunner fork probe** (env-gated, reverted) at `0x142a00` (the by-name factory, first sub-call on the
divergent path; rcx = a `"Z:\…"` path):
```
0x142a00: factory called           0x142a42: rax(obj)=0x1206dd140  vt=0x142546738(.rdata)  [vt+0x18]=0x19820c0
0x142a85 (bail): NOT fired         0x145ec8 (return into 0x145bc0): NOT fired       → then c000007b
```
**Fork = OUTCOME 2 (NOT factory-NULL):** `0x50b0b0` returns a **valid object**; the vtable is **static `.rdata`**
(so `[vt+0x18]=0x19820c0` is identical on both runtimes). After the factory, `0x142a00` **never returns and
never bails** → the next instr `call [r9+0x18]` @`0x142a56` → `0x19820c0` is the diverging dispatch.

**CrossOver diff** (break call-site/target/return/bail) — hit sequence:
```
0x142a56 (call) → 0x19820c0 (ENTERS, same target) → 0x142a5a (RETURNS) → 0x142a85 (bail, al==0)
```
So on **CrossOver the vtable call `[r9+0x18]` → `0x19820c0` runs and returns cleanly (al==0 → 0x142a00 bails →
returns into 0x145bc0 → later builds the singleton).** On **MacRunner the same call never returns** → 0x145bc0
stalls before the publisher/ctor → singleton `0x14293eda0` stays NULL → consumer `0x575518` → `0x58576f`.

**ROOT (the discriminator the operator asked for): the INDIRECT VTABLE DISPATCH at `0x142a56` (`call [r9+0x18]`,
target `0x19820c0`) — same HK `[r15+0x10]` class.** `0x19820c0` is a by-name string-dispatch (`call 0x586ae0(obj,
"<const>")` chain). Target is correct (static table) ⇒ the failure is either HB **mis-dispatching** the indirect
call (doesn't actually enter `0x19820c0`) or `0x19820c0` **diverging inside** on MacRunner. Final discriminator
probe running now: watch `0x19820c0` ENTRY on MacRunner (enter→diverge-inside vs not-enter→mis-dispatch).

---

## ROOT PINNED (4th correction): 0x19820c0 (UE4 pak-mode path matcher) returns al=1 vs al=0 — string-match, likely NOT HK class

Dispatch discriminator probe (env-gated, reverted) — does `0x19820c0` enter on MacRunner?
```
MacRunner: 0x19820c0 ENTERS → 0x19820df (1st compare done) → 0x142a5a RETURNS rax=0x1 (al=1)  [NOT a mis-dispatch]
CrossOver: 0x19820c0 enters → returns al=0 → 0x142a85 (bail)
```
**Overturns the "doesn't return"/mis-dispatch hypothesis.** `0x19820c0` is correctly dispatched, enters, and
RETURNS — but **returns `al=1` on MacRunner vs `al=0` on CrossOver**. With al=1 the `je 0x142a85` (bail) is NOT
taken → `0x142a00` proceeds down the "matched" path (2nd vtable call `[r9+0x20]` …) → cascades to `0x58576f`.

**What `0x19820c0` is:** it compares (case-insensitive wide-string, via `0x586ae0`) the **path** (`r8=rsi`=the
`"Z:\…"` input; `0x19820cd mov rcx,r8`) against the UE4 PAK-mode tokens **`"Pak" / "Signedpak" / "Signed" /
"NoPak"`** (resolved `.rdata` constants). On MacRunner the path matches `"Pak"` (al=1); on CrossOver it doesn't.
`0x586ae0` is a case-insensitive wide compare (likely extension/suffix match — "is this a .pak path").

**Reframe (4th verify-first correction this investigation):** the root is **NOT** the `je`, **NOT** the seeds,
**NOT** factory-NULL, **NOT** a mis-dispatch / HK `[r15+0x10]` class. It is a **string-match return-value
divergence in UE4 pak/path parsing** — `0x19820c0` matches the path to `"Pak"` on MacRunner but not CrossOver.

**Decisive remaining fork (the actual root):** capture the **path string content** (`r8` at `0x19820c0` /
`rcx` at `0x586ae0`) on BOTH runtimes:
- **Same path + different `al`** → HB `0x586ae0` string-compare **mistranslation** (an HB bug — then it IS a
  shared-core HB defect, fix in HB).
- **Different path** → **environmental**: MacRunner's file-enumeration / `Z:\` path resolution feeds a different
  path (a `.pak`) into `0x142a00` than CrossOver → the game takes its pak-handling branch under MacRunner →
  cascade. Fix = the Wine path/pak-enumeration layer (NOT an HB instruction bug, NOT HK's class).

NEXT: a probe dumping the full wide path at `0x19820c0` (r8) on MacRunner + a CrossOver `x/40hx` of the same →
diff. That single comparison decides HB-compare-bug vs environmental-path — and whether this shares HK's root
at all. (Honest: the recurring "vtable dispatch = HK class" framing did NOT survive the trace — this branch is
a pak/path string match.)

---

## PATH-STRING DIFF — fork resolved: NOT an HB 0x586ae0 bug; the INPUT is malformed ("[NUL]Z:\")

Decisive probe (env-gated, reverted) at `0x586ae0` filtered to the pak-compare caller `0x19820df`:
```
0x586ae0 caller=0x19820df  rcx(in)=0x1006c4292  in4=0x5c003a005a0000  rdx(const)="Pak"
0x19820df: al=0    ← 0x586ae0("[NUL]Z:\", "Pak") = 0   (CORRECT — refutes the HB-compare-bug hypothesis)
0x142a5a:  al=1    ← 0x19820c0 overall returns 1
```
**`0x586ae0` is NOT miscompiled** — it correctly returns 0 for this input. So the `al=1` is `0x19820c0`'s
behavior on this input (likely its "no pak-token matched → default" branch; all 4 token compares return 0 on
an empty/NUL input).

**The real anomaly is the INPUT:** `in4 = 0x5c003a005a0000` = wide chars `[0x0000]['Z'][':']['\']` — the
string passed in **starts with a NUL wide char** (`"[NUL]Z:\…"`). A correct path is `"Z:\…"`. So the wide
string fed into the UE4 pak-classifier is **malformed under MacRunner** (leading NUL = off-by-2 pointer or a
path-construction bug). On CrossOver the equivalent arg was a different/clean value (`r8=0x34444`), so it
classifies normally and bails.

**Fork verdict:** NOT a clean HB `0x586ae0` compute bug, and NOT HK's `[r15+0x10]` class. It is a **malformed
input string** ("[NUL]Z:\") driving `0x19820c0` down the wrong branch → cascade → `0x58576f`. Leans
**environmental/producer** (the path/wide-string construction differs under MacRunner). Corroborating anomaly:
across every MacRunner probe the "input" pointers all end `…4292` and all read the same content
`0x5c003a005a0000` — suggesting a consistent off-by-2 / mis-formed pointer or value feeding this site.

**NEXT (two threads):**
1. Confirm `0x19820c0`'s return semantics (trace its 4 token compares + the `0x19821ac` handler + final ret) —
   verify `al=1` == "no match/default" (vs a real match), so we know the malformed input is the cause not a
   later miscompile.
2. **Find where the `"[NUL]Z:\"` wide string is built** (the producer) — the leading NUL is the smoking gun.
   Likely a Wine Unix→Windows path conversion / UTF-8→UTF-16 step, or an off-by-2 in how the path arg is
   passed into `0x142a00`. That producer is the root; fix there (Wine path layer / the caller), not `0x586ae0`.

Honest: 5th correction — the diverging indirect call's *compare* is correct; the *input string* is malformed.
The "HB miscompile vs environmental" fork lands on **malformed-input/producer**, pending the producer trace.

---

## ROOT FOUND: POINTER OFF-BY-2 on a command-line/PEB path string (the ABZU first-frame root)

Producer bisector (env-gated probe, reverted) at `0x142a00`, dumping memory around the path pointer `rcx`:
```
rcx=0x104344292  *(rcx)=0x5c003a005a0000  *(rcx-8)=0x22006500780065
mem[rcx-8..] = e x e "  _  Z : \ U s e r s \ t i m u r t o b y \ ... \ AbzuGame \ Binaries ...
               <--rcx-8-->  ^idx4 = *(rcx) = NUL          (canonical path begins at rcx+2)
```
**Bisector verdict = POINTER OFF-BY-2.** The canonical path `Z:\Users\timurtoby\Documents\MacRunner\Main\ABZU\
game\AbzuGame\Binaries\…` is **fully intact starting at `rcx+2`**; `rcx` points one WCHAR too low — at the NUL
that terminates the preceding `…AbzuGame-Win64-Shipping.exe"` (the quoted exe path = the command line). So
`0x586ae0` reads an empty string (`[rcx]`=NUL) → all pak-token compares = 0 → `0x19820c0` returns al=1 (its
"no match/default" branch) → `0x142a00` does NOT bail → cascade → singleton `0x14293eda0` NULL → `0x58576f`.

**What the string is:** the game exe's directory / image path, sitting in the PEB right after the CommandLine
(`mr-run.sh` launches `"$WINE" "$EXE"` with no extra args, so the cmdline is just the quoted exe path). So the
off-by-2 is on a **command-line / PEB `RTL_USER_PROCESS_PARAMETERS` path string** — the `b4f1213` GetCommandLine
area. **Corroboration:** across EVERY MacRunner probe the path pointers all end `…4292` (same low bits, ASLR
high bits) → a **structural, consistent off-by-2** (one WCHAR), not random.

**Verdict on the operator's fork:** pointer-off-by-2 (string in memory is CORRECT; the pointer is 2 low) — NOT
a string-build NUL inside the path, NOT an HB `0x586ae0` compute bug, NOT HK's `[r15+0x10]` class. The fix is at
the **command-line/PEB-path producer**, not `0x586ae0`, not golden.

**NEXT (name the exact producer + fix):**
1. Trace `rbx`'s origin in `0x145bc0` (back from `0x145ebb`'s path arg) → is the off-by-2 pointer read from a
   PEB `UNICODE_STRING.Buffer` (Wine loader / RtlCreateProcessParametersEx packs the strings one WCHAR off) OR
   computed by the game's own arithmetic (then HB mis-lowers a pointer add)? The `…4292` structural pattern +
   the CommandLine-adjacency strongly favor a **PEB UNICODE_STRING.Buffer off-by-2**.
2. CrossOver-diff: capture the same path pointer on CrossOver (clean) → confirm it points at `'Z'` (rcx+2) →
   MacRunner is 2 low; measure the exact delta.
3. Inspect MacRunner's PEB ProcessParameters / GetCommandLineW path (b4f1213) for a one-WCHAR off-by-2 in
   string packing / `.Buffer` computation. Fix there (Wine path/command-line layer).

This is the ABZU first-frame root after 5 verify-first corrections: a **one-WCHAR off-by-2 on the command-line/
PEB path string** makes UE4's pak-classifier read an empty path → wrong branch → the NULL-singleton cascade.

---

## OFF-BY-2 REFUTED (6th correction) — the input is LEGITIMATELY empty (no cmdline args); divergence recurses

Traced the path-pointer producer: `rbx` in `0x145bc0` = its 2nd arg (`0x145be5 mov rbx,rdx`), passed by `0x1412e0`,
which sets it from `0x567450`'s return (`0x1413ac call 0x567450` → `0x1413bb mov rbx,rax` → `0x1413db mov rdx,rbx`
→ `call 0x145bc0`).

**`0x567450` is a standard "skip the quoted program name + spaces, return the first ARG" parser** (PathGetArgsW-
style): `cmp word[rcx],0x22` (skip `"…"`), skip spaces, `mov rax,rcx; ret`. With **no command-line arguments**
(`mr-run.sh` launches `"$WINE" "$EXE"` — argv0 only), it **correctly returns a pointer to the NUL** after the
quoted exe. **So the pointer is NOT off-by-2** — it legitimately points at the empty arg-string; the `Z:\…` at
`rcx+2` is just the adjacent PEB ImagePathName, coincidental. **The off-by-2 verdict is REFUTED (6th correction).**

`0x19820c0` decoded fully: compares the (empty) input to `"Pak"/"Signedpak"/"Signed"/"NoPak"` — all correctly
return 0 for empty — then **falls through to `0x198214e: call 0x141978210`** and returns *that* `al`. So the
`al=1` on MacRunner comes from **`0x141978210`** (the no-token/empty classifier), recursing the divergence one
more level (after `0x14197bce0` builds an array from the input at `0x1982141`).

**Honest reassessment — the hand-probe differential has hit two limits:**
1. **6 corrections** (`je` → seeds → factory-NULL → mis-dispatch → `0x586ae0`-bug → off-by-2), each refuted by
   the next probe. The "root" keeps receding deeper into UE4's empty-command-line processing.
2. **Multi-invocation confound:** `0x142a00`/`0x19820c0` is called many times with different inputs; MacRunner and
   CrossOver breakpoints keep catching *different* invocations, so "same input → different al" is never cleanly
   established (the CrossOver `al=0` bail and the MacRunner `al=1` may be different inputs).

**What's still SOLID:** `0x58576f` = NULL singleton `0x14293eda0`; `0x145bc0` reaches the je-taken path
(`0x145ebb`) on both, but on MacRunner `0x142a00` (al=1) continues into the crash while on CrossOver it bails;
the input fed in is a **legitimately empty command-line argument** (no off-by-2). The divergence is **deep in the
empty-arg processing** (`0x141978210`/`0x14197bce0`), or the two runtimes aren't actually processing the same
invocation.

**Recommended clean finish (stop hand-probing):**
- **Capture GetCommandLineW / PEB.ProcessParameters.CommandLine on BOTH** runtimes — confirm both are argv0-only
  (expected). If they DIFFER, that's the root (launch/argv). If identical, it's a deep compute divergence.
- **Matched-invocation log:** log EVERY `0x142a00` call's input + final al (bail vs continue) on both, match by
  input content → find the one input where MacRunner continues but CrossOver bails. That is the true divergence,
  free of the multi-invocation confound.
- Then resume the depth trace from there, or re-validate the `0x142a00`-is-load-bearing premise from the
  singleton consumer side (`0x575518`).

---

## -log refuted + divergence pinned to 0x197c8d0's body — but the method has hit a wall (honest)

**-log REFUTED:** ran CrossOver via winedbg WITHOUT `-log`; `0x567450`'s `rcx=0x3438c` is identical to the
`-log` run → `-log` is not in the debuggee command line; my CrossOver reference was never tainted. And **no-log
CrossOver still reaches `0x585670` (builds the singleton, valid `this=0x20d60e0`)** with the same no-args command
line MacRunner uses. So the command line is NOT the divergence (step-1 → identical).

**`al=1` (1st vtable / `0x19820c0`) is NOT the divergence** — no-log CrossOver shows `al=1` at `0x142a5a` too,
then bails at `0x142a85` (the 2nd vtable `[r9+0x20]=0x197c8d0` returns al=0) → builds the singleton.

**Confound-free MacRunner flow** (logged the full `0x142a00` path):
```
0x142a00 ENTRY → 0x142a5a 1st-al=1 → 0x197c8d0 (2nd method) REACHED → [2nd-al/BAIL/RETURN never fire] → c000007b @0x58576f
```
So **`0x197c8d0` returns al=0 on CrossOver (→bail→builds singleton) but does NOT return on MacRunner** — its deep
empty-input processing (`0x197c94e`: memset `0x141d76608` + string-building `0x149b50`/`0x13d080`/`0x197f110`)
diverges, and the run ends at the **consumer NULL-deref `0x58576f`** (rcx=0, the singleton not built). Crash PC =
`0x58576f` (same consumer as always), reached from `0x197c8d0`'s path (which never returns to `0x142a00`).

**Honest method wall (after 6+ corrections):** `je` → seeds → factory → mis-dispatch → `0x586ae0`-bug →
off-by-2 → `-log` — every hypothesis refuted by the next probe, and the divergence keeps **receding into deeper
identical-looking functions all processing the same empty command-line argument** (now inside `0x197c8d0`'s
string-building body), with the crash always the consumer `0x58576f`. The recurring anomalous pointer low-bits
(`…4292`, `…2238`) across MacRunner probes hint at a **pervasive low-level value/pointer difference**, not a
single guest branch. Block-by-block hand-probing cannot economically reach the root.

**Recommended escalation (stop hand-probing):**
1. **Instruction-level lockstep differential** — MacRunner vs CrossOver (or vs the HB unicorn oracle,
   `tools/hb_oracle/unicorn_adapter.py`) from a common point (`0x197c8d0` entry) stepping until the FIRST
   instruction whose result/flag/pointer differs. That single instruction = the HB mistranslation (if any).
2. If lockstep shows identical guest results → the divergence is in **host/Wine state** (memory layout, a Wine
   API return) that the game's deep processing reads — diff that.
3. Re-validate the premise: confirm (cleanly) that `0x14293eda0`'s producer is `0x145bc0`'s `0x146ce1/0x146eb8`
   and that `0x197c8d0` not-returning is what skips it — vs the singleton being a separate concern.

The investigation has SOLID scaffolding (crash = NULL singleton; CrossOver builds it with the same cmdline; the
divergence lives in `0x197c8d0`'s empty-input processing) but the exact mistranslated instruction needs lockstep,
not more single-shot probes.

---

## UNICORN LOCKSTEP built — HB codegen RULED OUT for 0x197c8d0 body; divergence is in the deep import-heavy tree

Built `/tmp/uc_lockstep.py`: maps the exe, replays the byte-exact captured MacRunner entry state at `0x197c8d0`
under Unicorn (the faithful x86_64 reference), with CRT import stubs (memset/memcpy/malloc/...).

**Entry state captured** (the crashing invocation): input `r8=rsi` → `[NUL]Z:\Users\timurtoby\Documents\MacRunner
\Main\ABZU\game\…` (the leading-NUL path), object `rcx=rdi=0x12035d140`, vtable `r9=0x142546738`
(`[r9+0x18]=0x19820c0` 1st method, `[r9+0x20]=0x197c8d0` = us).

**RESULT: HB codegen is FAITHFUL for `0x197c8d0`'s body.** With memset stubbed, Unicorn's path matches HB's
`ucpath` EXACTLY through all 44 of `0x197c8d0`'s own blocks (`0x197c8d0 → … → 0x197c974 = call 0x149b50`,
including past memset). **No HB instruction mistranslation in `0x197c8d0` itself.**

**The divergence is deeper — in `0x149b50`'s call tree.** Unicorn escapes inside it at `0x4f6dd0` (`jmp
[rip+0x19c9421]`), a **delay-import thunk** (IAT `0x22c01f8`; the exe has 46 imported DLLs + a delay-import
dir at `0x28930a4`). So the deep tree calls **delay-imported Win32/CRT functions** Unicorn can't follow without
stubbing the whole import surface + capturing runtime heap/.data.

**Honest limit of this method:** Unicorn-with-stubs ruled out HB codegen for `0x197c8d0`'s body, but the deep
tree (`0x149b50 → … → 0x4f6dd0 → a delay-imported Win32/CRT fn`) is too import/runtime-state-heavy to replay.
And since no-log CrossOver (same `[NUL]` input) has `0x197c8d0` RETURN al=0 (bail→builds singleton) while
MacRunner's doesn't return, **the input is the same on both → the divergence is in the deep tree's interaction
with Wine/runtime state, not the input and not `0x197c8d0`'s codegen.**

**Where this leaves it (the bug is one of):**
- **HB codegen deep in the `0x149b50` tree** (unverified — Unicorn can't reach it economically), or
- **a Wine Win32/CRT API in the deep tree returning differently than Windows** (the deep tree is import-heavy;
  a delay-imported function behaving differently under Wine would diverge the path), or
- runtime heap/.data state the deep tree reads.

**Recommended next instrument:** a tracer that **can follow into Wine** — e.g., HB's own per-block trace
(`MACRUNNER_HB_TRACE_*`) bracketing `0x149b50`'s tree to find the last common block vs a CrossOver winedbg
breakpoint-bisect on the same tree; or a Wine-side `+relay`/`+pid` trace of the Win32/CRT calls the deep tree
makes, diffed MacRunner-vs-CrossOver, to catch the first API call whose result diverges. Unicorn-with-stubs is
the wrong tool past the first import-heavy callee. (The lockstep tool + capture probe are reusable for any
pure-guest stretch.)

---

## ★ROOT CONVERGED (HB-block-vs-CrossOver bisect, 2026-06-22): a DESERIALIZED MAGIC mismatch → failed format-check → NULL-logger crash

Drove the HB-block-vs-CrossOver bisect down the deep tree. At EACH level HB reaches the consumer while CrossOver
returns; the bisect converged:
```
0x142a00 → 0x197c8d0 → (loop) 0x197eb20 → 0x1976e90 → 0x197c770 → [0x1980c40 deserialize] → CHECK fails → 0x5753e0 logger → 0x575518 → 0x58576f
```
Each level confirmed by CrossOver winedbg breakpoint-walk (CrossOver hits the same blocks then RETURNS; HB
continues to the consumer). Key eliminations along the way: `0x197bf50` returns non-NULL on both; the `[rax+0x80]`
check (`cmp 0x2c`) returns `0x11c24b646` and PASSES on both; HB and CrossOver agree block-for-block until the
deserialize.

**The fork — `0x197c770` CHECK1** (`cmp [obj+0x90], 0x5a6f12e1; je pass`, assert line 368):
```
HB:        [obj+0x90] = 0xb8631f47   (≠ magic)  → CHECK FAILS → assert(0x585900) → log(0x5753e0) → NULL logger → 0x58576f
CrossOver: [obj+0x90] = 0x5a6f12e1   (== magic) → passes → returns
```
**`[obj+0x90]` is set by `0x1980c40`, which is a DESERIALIZER**: it reads **4 bytes from a UE4 `FArchive`/stream**
(`rdi`; virtual `[rax+0x38]`=Serialize, `[rax+0x78]`=Tell, `[rax+0x80]`=TotalSize; bounds-checks `size >= pos+0x2c`)
into `[obj+0x90]`. So `[obj+0x90]` is a **magic/format signature deserialized from a stream**, verified `==0x5a6f12e1`.

**HB deserializes the WRONG value (`0xb8631f47`) where CrossOver gets `0x5a6f12e1`.** So the divergence is **wrong
deserialized data** read from the FArchive. The format-check then fails, and the failure-logging path dereferences
the **NULL logger singleton `0x14293eda0`** (`0x575518 → 0x585670 → 0x58576f`) — so the crash is the NULL logger,
but the *primary* bug is the bad deserialized bytes.

**TWO findings:**
1. **PRIMARY:** HB's `FArchive` read returns `0xb8631f47` instead of `0x5a6f12e1` — a serialized-stream/data
   divergence (wrong bytes, or wrong stream position/source).
2. **SECONDARY:** the error path logs via a logger singleton that is NULL on MacRunner → crashes instead of
   reporting the format error gracefully. (Same NULL-singleton `0x14293eda0` seen since the start — it's the LOGGER,
   reached only on the error path.)

**NEXT:** trace the `FArchive` (`rdi`) source — is it reading a **memory buffer** (built from upstream data that
differs) or a **file** (Wine file-I/O returning wrong bytes / wrong offset)? Capture `rdi`'s buffer ptr + position
(`[rax+0x78]` Tell) + the bytes around it on both runtimes. If the stream bytes are identical but HB reads at a
wrong offset → Tell/position bug (HB codegen or the archive state). If the stream bytes differ → the data feeding
the archive is wrong upstream (the [NUL]-path / file-read divergence resurfaces). That names HB-codegen vs
Wine-file-IO vs upstream-data — and whether it's shared-core (a Wine FArchive/file-read bug would hit other UE4
titles). The crash (`0x58576f`) is downstream of this; fixing the deserialized-data divergence resolves it.

---

## ★★ ROOT FOUND — ABZU first frame blocked by a >4 GB pak-file READ-OFFSET bug (FArchive footer read goes to 0x10000)

The FArchive-source capture + pak-byte analysis nailed it. The deep init tree mounts the pak
`../../../AbzuGame/Content/Paks/AbzuGame-WindowsNoEditor.pak` (**4,767,135,302 bytes = 0x11c24b646 = 4.767 GB**)
by reading its **FPakInfo footer** (44 bytes) at `TotalSize - 0x2c = 0x11c24b61a` and checking the magic
`== 0x5A6F12E1` (UE4 `FPakInfo::Magic`).

**Verified from the pak bytes on disk:**
- File size read by HB (`FArchive` TotalSize `[vt+0x80]`) = `0x11c24b646` = the REAL pak size (exact). So
  `GetFileSize`/stat works for >4 GB. ✓
- The real footer at `0x11c24b61a` starts with `e1 12 6f 5a` = **`0x5a6f12e1`** (the correct magic). CrossOver
  reads this → check passes → pak mounts → game proceeds.
- HB deserialized **`0xb8631f47`**, which exists at **exactly ONE offset in the pak: `0x10000` (64 KB)**.

⇒ **HB read the footer from offset `0x10000` instead of the 4.767 GB offset `0x11c24b61a`.** The file is the
right pak and its size is correct, but the **>4 GB footer read/seek lands at the wrong (low) offset** → wrong
44 bytes → wrong magic (`0xb8631f47`) → the pak-format check (`0x197c808`, assert line 368) FAILS → the
failure path logs via the logger singleton `0x14293eda0`, which is **NULL** on MacRunner → `0x575518 →
0x585670 → 0x58576f` `c000007b` → the boot never reaches graphics. **The NULL singleton chased since the start
is the LOGGER, only reached because the pak read genuinely failed.**

**CLASS: a >4 GB large-file I/O bug — SHARED-CORE** (every game with a pak/asset > 4 GB fails to mount it; very
likely affects HK and other titles). Not the [NUL]-path, not HB codegen of `0x197c8d0` (lockstep cleared that),
not the command line — a wrong file-read offset for >4 GB.

**Not a clean 32-bit truncation:** `0x11c24b61a & 0xFFFFFFFF = 0x1c24b61a` holds `0x13d413d7` (≠ what HB read);
HB landed at `0x10000` specifically. So the mechanism is more than `offset & 0xFFFFFFFF` — likely a buffered/
precached `FArchive` read whose >4 GB seek/buffer-base computation collapses to `0x10000`, OR a Wine `NtReadFile`
`ByteOffset` / `pread` / file-mapping path that mishandles the >4 GB offset.

**NEXT (the FIX):** trace the actual footer read syscall — break/probe at the file read for this handle with the
64-bit `ByteOffset` (Wine `NtReadFile`/`pread`, or `NtCreateSection`/`mmap` if memory-mapped) and capture the
offset passed vs `0x11c24b61a`. Find where it becomes `0x10000` (Wine file-I/O 64-bit-offset handling, an HB
64-bit-arg marshalling truncation, or the FArchive buffer-base). Fix in that layer — it unblocks ABZU's first
frame and any other >4 GB-pak title. **Secondary (separate, lower priority): the logger singleton `0x14293eda0`
being NULL makes a recoverable pak-format error fatal — a graceful-error gap to fix later; fixing the read
avoids the error path entirely.**

★Method that cracked it: HB-block-vs-CrossOver bisect (each level: trace HB's blocks in a range + CrossOver
winedbg breakpoint-walk → first divergent block) drove from the whole game down to `0x1980c40` (the FPakInfo
deserialize), then pak-byte analysis on disk pinned the wrong read offset. `uc_lockstep.py` earlier ruled out
HB codegen for the `0x197c8d0` body. ~340-turn investigation; converged to a one-line root: **>4 GB pak footer
read lands at 0x10000.**

---

## ★ >4GB ReadFile REPRO + Wine-vs-HB isolation (2026-06-22) — verdict: truncation is HB-side, not Wine's file layer

**REPRO (`/tmp/bigread.c` mingw / `/tmp/bigread2.c` freestanding, x86_64 PE):** CreateFile a 4.767GB sparse file
(`/tmp/bigfile_test.bin`, magic `0x5a6f12e1` written natively at `size-0x2c=0x11c24b61a`, mirroring FPakInfo),
then GetFileSizeEx + SetFilePointerEx(>4GB) + ReadFile + an OVERLAPPED ReadFile(hi=0x1).
- **CrossOver (x86_64 Wine + Rosetta2, NO HyperBridge): ALL PASS** — size=0x11c24b646, SetFilePointerEx
  newpos=0x11c24b61a, ReadFile val=0x5a6f12e1, OVERLAPPED val=0x5a6f12e1. So Wine+the reference handle >4GB
  file reads correctly; ABZU is a **MacRunner-specific >4GB regression**.
- **MacRunner: the custom PE can't be tested** — both repro variants die in early process init (`exit=53`; the
  mingw one hits the known EC-unwind bail during CRT init) before reaching `main`. An *unrelated* HB init gap
  blocks arbitrary x86_64 console PEs. So the >4GB read was isolated in the **real game** instead.

**Real-game isolation — instrumented EVERY Wine unix file path (env `MACRUNNER_TRACE_BIGREAD`), all reverted:**
| Path | Probe | Result (game reaches the crash, run-exit=1) |
|---|---|---|
| `NtReadFile` explicit offset >4GB | log `_eff>4GB` | **0 hits** |
| `NtReadFile` done: (magic in buffer) | first256/last512, 64KB, pak-fd `fstat==0x11c24b646` whole-buffer | **0 hits** |
| `map_file_into_view` (mmap) | all fd-backed mmaps | 1849 hits, **all DLL/image (≤7MB views); no 4.767GB pak map** |
| `NtSetInformationFile` FilePositionInformation (seek) | log pos>1MB | **0 hits** |
| `server_read_file` (BAD_DEVICE_TYPE path) | scan buffer + log pos | **0 hits** |
| `NtReadFileScatter` | log offset | **0 hits** |

⇒ **Wine's unix file layer NEVER receives a >4GB offset / seek / mapping for the pak**, yet the FArchive
demonstrably reads the wrong footer bytes (`0xb8631f47`, = pak offset `0x10000`) and the crash fires. The
async pak read's offset arrives **already truncated to <4GB before NtReadFile**. Combined with **CrossOver
(no HB) passing the identical >4GB read**, the truncation is **upstream of Wine — on the HyperBridge side**:
either HB's codegen truncating the guest's 64-bit offset computation (a 64-bit value clipped to 32-bit in the
translated UE4/kernelbase seek/read path), or HB's marshalling of the 64-bit file offset across the x86_64→ARM64
host-call boundary. **Not** Wine's `pread`/`lseek`/`mmap` (those are 64-bit-clean and CrossOver-proven).

**NEXT (fix):** (1) trace the explicit `ByteOffset` at the HB→host syscall boundary for `NtReadFile` (the async
overlapped read) — confirm it arrives <4GB; (2) if the offset is computed guest-side, run `uc_lockstep.py` on the
UE4 FArchive seek/offset arithmetic to find the 64-bit→32-bit truncation in HB codegen; (3) fix in HB. The
CrossOver-confirmed repro (`/tmp/bigread2.exe` + `/tmp/bigfile_test.bin`) re-tests any fix instantly. Secondary
(unrelated, surfaced here): custom x86_64 console PEs `exit=53` in early init (EC-unwind during CRT init) — a
separate HB gap worth its own ticket (it blocks standalone micro-repros).

---

## ★★★ (a)/(b) RESOLVED — it's (b): guest OVERLAPPED offset is CORRECT, the 64-bit ByteOffset is mis-reconstructed downstream as OffsetHigh<<16 (2026-06-22)

Traced the pak read mechanism end-to-end (static RE + runtime HB probes, all reverted):
- The pak (FArchiveFileReaderGeneric, vtable rva 0x1fe50b0) Serialize (rva 0x517430) memcpys from an inline
  buffer [FArchive+0xb0]; on empty it calls precache 0x50e440 → FArchive ReadLowLevel (rva 0x513620) →
  **IFileHandle::Read (rva 0x5aed30)** — a UE4 **double-buffered ASYNC reader** (64KB ping-pong buffers at
  [IFH+0x28]/[+0x30], cursor [+0x44], FilePos [+0x18], FileSize [+0x10]).
- Refill issues an **OVERLAPPED ReadFile** in StartRead (rva 0x5b1d20, import call rva 0x5b1d5c):
  `ReadFile(handle=[this+8], buf=[this+idx*8+0x28], size=[this+0x38]=0x10000, &read, OVERLAPPED=[this+0x50])`.

**Runtime capture at StartRead (rva 0x5b1d20), pak filtered by FileSize==0x11c24b646:**
```
footer read (idx=0): FilePos=0x11c24b61a  OVL.Offset=0x1c24b61a  OVL.OffsetHigh=0x00000001  full=0x11c24b61a  size=0x10000
prefetch  (idx=1): FilePos=0x11c24b61a  OVL full=0x11c25b61a  size=0x10000
```
**The guest sets up the CORRECT >4GB OVERLAPPED offset** (full=0x11c24b61a, OffsetHigh=0x1 preserved, Offset=
0x1c24b61a). Yet the 64KB buffer comes back holding **file-0x10000 data** (buffer[0][0]=0xb8631f47 = pak offset
0x10000), and **0x10000 = OffsetHigh(0x1) << 16**. So the low dword (0x1c24b61a) is DROPPED and OffsetHigh is
shifted to bit 16.

**VERDICT:**
- **(a) RULED OUT** — the guest's 64-bit offset arithmetic + OVERLAPPED setup are provably correct.
- **(b) CONFIRMED** — the 64-bit ByteOffset is **mis-reconstructed DOWNSTREAM of the guest** (HB-side), in the
  OVERLAPPED→ByteOffset / ReadFile→NtReadFile path: it yields `OffsetHigh<<16` instead of
  `(OffsetHigh<<32)|Offset`. Matches the operator's clue exactly (high dword survived/repositioned, low dropped).
- Consistent with the earlier exhaustive negative (no >4GB offset/seek/mmap ever reaches Wine's unix file layer;
  PAKENTRY/PAKMMAP=0): the offset is corrupted before/at the marshalling, and the resulting read targets file
  0x10000.

**NEXT (fix):** the OVERLAPPED ReadFile (rva 0x5b1d5c) goes guest → kernelbase ReadFile → ntdll NtReadFile. The
mis-reconstruction is in that path under HB. (1) Disasm x86_64-windows/kernelbase.dll `ReadFile` (the
OVERLAPPED.Offset/OffsetHigh → LARGE_INTEGER ByteOffset construction) and x86_64-windows/ntdll.dll `NtReadFile`
syscall stub, then uc_lockstep that stretch under HB to find the instruction that produces OffsetHigh<<16 (a
64-bit value built from two 32-bit dwords with a wrong shift/merge). (2) OR if it's HB's syscall arg marshalling
of the LARGE_INTEGER* ByteOffset at the x64-syscall boundary, fix there. The signature to grep/audit:
`result = OffsetHigh << 16` (or a 48-bit/16-bit pack of the 64-bit offset). Cross-check PE32's HB-marshalling
audit (currently on i386/xtajit teb32, not this x64 path). SHARED-CORE: every >4GB-pak title (likely HK).
**FRAME: pending the fix.**

---

## ★★★★ FIX FOUND + APPLIED — (b1): HB ReadFile handler ignored lpOverlapped; pread fix MOUNTS the pak, 0x58576f GONE (2026-06-22)

**Mechanism = (b1)** (PE32's hypothesis, confirmed by code + dynamic probe). HB has its own `ReadFile` import
handler at `macrunner_hb.c:10337`. It did:
```c
done = read( fd, buffer, request );   /* reads at the fd's CURRENT position; lpOverlapped (args[4]) IGNORED */
```
For UE4's async **OVERLAPPED** pak reader the file position lives in the OVERLAPPED struct, not the fd pointer.
So the >4GB footer read (OVERLAPPED.Offset=0x11c24b61a) was served from the fd's stale post-precache position
**0x10000** (= the byte after the first 64KB block — coincidentally 0x1<<16, which masqueraded as "OffsetHigh<<16").
This also explains the earlier total absence from the unix file layer (PAKENTRY/PAKMMAP=0): **HB satisfies
ReadFile itself here and never calls unix NtReadFile.** The guest OVERLAPPED was always correct — purely an HB
ReadFile-handler bug.

**FIX (macrunner_hb.c ReadFile handler, UNCOMMITTED):** when `lpOverlapped` (args[4]) != NULL, read the 64-bit
ByteOffset from the OVERLAPPED (`u64 @ lpOverlapped+0x10` = Offset|OffsetHigh) and `pread(fd, buffer, request,
ByteOffset)` instead of `read()` (positioned read, no fd-pointer move). Else fall back to `read()`.

**VERIFIED on the real game (cold, dxmt):** gated probe shows every pak read now `mode=pread@OVL`; the footer
read uses `OVL.off=0x11c24b61a` (correct), then the game reads the pak INDEX at sequential offsets
(0x11c146586, 0x11c156586, … +0x10000) — i.e., **the FPakInfo magic validated and the pak MOUNTED**.
**Zero `0x58576f`/`c000007b` crash markers** (was deterministic before). ABZU is now PAST the first-frame
blocker that has gated it for the entire investigation.

**SHARED-CORE:** the same handler bug breaks every OVERLAPPED read at any file offset for all titles; the symmetric
`WriteFile` handler (`macrunner_hb.c:~12270`) has the identical `write()`-ignores-lpOverlapped bug (pwrite fix
needed too; not on ABZU's read path). Fixes every >4GB-pak title's asset mounting (likely HK + others).

**NEXT:** confirm how far ABZU now boots (graphics path / first frame / next wall) via a clean triaged run;
apply the symmetric WriteFile pwrite fix + ReadFileEx; then commit.

---

## ★★★★★ ABZU UNBLOCKED — pak mounts, 0x58576f GONE, reaches DXMT/Metal graphics init (2026-06-22)

The (b1) ReadFile pread fix is VERIFIED on the real game (3 independent cold dxmt runs):
- Every pak read now positions at the OVERLAPPED offset (footer `OVL.off=0x11c24b61a`), then the pak INDEX
  reads stream at sequential offsets → **FPakInfo magic validates, the pak MOUNTS**.
- **Zero `0x58576f`/`c000007b`** (was a deterministic crash every run for the whole investigation).
- ABZU now runs ~115–200s without crashing and advances into the GRAPHICS stack: `CreateDXGIFactory`/
  `CreateDXGIFactory1` resolve to dxmt's aarch64 dxgi.dll, `D3D11CreateDevice` is wired, and **winemetal/DXMT
  runs** (`info: Failed to set Metal cache path, fallback to system default`), spinning up render/worker threads
  (`xtajit64 ThreadInit`).

**This is the milestone the entire ABZU investigation was gated on.** The deterministic first-frame crash
(`c000007b @ 0x58576f`, the NULL-logger on the pak-mount-failure path) is fixed at its true root: an HB ReadFile
import-handler bug, not the game.

**Clean fix (UNCOMMITTED, macrunner_hb.c):** ReadFile handler — `pread(fd,buf,req,OVERLAPPED.ByteOffset)` when
lpOverlapped present (else read()); symmetric WriteFile handler — `pwrite(...)` when lpOverlapped present (else
write()). No ReadFileEx/WriteFileEx handler exists (game uses plain ReadFile). Debug probe removed; only the fix
remains. Builds clean.

**NEW STATE / NEXT TARGET:** ABZU is now in DXMT/Metal graphics-init + UE4 startup; triage = `WINDOW_TRACE_INSUFFICIENT`
(no swapchain Present / visible window confirmed yet in the ~200s cold window). First-frame PIXELS are the next
target — likely a graphics-path item (swapchain create / present / Metal drawable) or cold-JIT startup throughput,
NOT the pak crash. Lane D (graphics) + a window-gate flight-recorder run are the follow-up. SHARED-CORE: the
ReadFile/WriteFile OVERLAPPED-offset fix unblocks asset mounting for every >4GB-pak (and any OVERLAPPED-positioned
I/O) title — Codex/HK should re-test.

---

## COMMIT + warm-cache→Present result (2026-06-22)

**COMMITTED:** `36890024c2838a4c16cb54e603b0f84f45e226cc` (abzu-lane) — the ReadFile/WriteFile lpOverlapped→
pread/pwrite fix. Snapshot: `MacRunner-abzu/artifacts/milestone-dist/rung-abzu-pak-overlapped-fix-reaches-graphics-20260622/`
(README + git-heads, light).

**Warm-cache → Present (task #24): NO Present.** Primed the translation cache (one boot → 6.5 MB), then warm runs
(600s + 300s window-gate). Result: warm cache makes graphics init MUCH faster (winemetal/Metal-init at ~19s vs
~80s cold; DXGI/D3D11CreateDevice wired) — but ABZU then **does NOT reach window-creation / swapchain / Present**.
The triage stays `WINDOW_TRACE_INSUFFICIENT` and the game **exits (exit=1)** at ~100–150s (NOT the 300/600s
timeout).

**NAMED GATE (not throughput — the warm cache did not grow during the run, and the game exits rather than hangs):**
a **repeating exception loop at a fixed guest site** (16× host pc `0x1075687f8`, unwinding through ntdll's
exception dispatcher rva `0x944xxx`), **preceded by `macrunner-hb-seh-ec-fpchain-bail reason=fp-null-or-unaligned`
+ `macrunner-hb-arm64-unwind-unsafe-boundary action=stop-unwind` at ntdll rva 0x8d2fc**, on UE4 **worker-thread
init** (each exception is right after `xtajit64 ThreadInit`/`ldr-init`). The window-trace probes
(CREATEWINDOW/NCCREATE/THREAD_DESKTOP) emit **zero** markers → ABZU stalls/exits **before** it ever attempts
window creation.

⇒ This is an **EC-unwind / exception-delivery gate** (same class as the earlier ABZU EC-unwind work, Sess11–14;
a different unwind gap the post-pak execution now reaches), NOT a graphics/DXMT functional gate (no
swapchain/window attempted yet) and NOT raw throughput. **Route: Lane A (EC-unwind)** — the next-level trace is
the exception CODE + the guest RVA of the throw site `0x1075687f8` + which worker-thread op throws (capture the
exception record, not just the callback delivery). The fp-null-or-unaligned bail at rva 0x8d2fc is the concrete
HB unwind defect to fix.

---

## CORRECTION via native sample (2026-06-22): the post-pak gate is HB THROUGHPUT, not EC-unwind

I earlier named the post-pak gate as an "EC-unwind exception loop." A native `sample` of the stalled warm game
**corrects that** — it was a misread of recurring/normal logs:
- The `seh-ec-fpchain-bail (fp-null-or-unaligned)` + `arm64-unwind-unsafe-boundary (stop-unwind)` at module
  base `0x7FFD06C0000` rva `0x8d2fc` is **recurring in EVERY run** (incl. early/working ones) = the normal
  wow64/EC unwind boundary, NOT an ABZU-specific gate.
- The `macrunner-hb-callback-exception-stack` calls go through `is_low_stack_access_fault` with **`code=0`**
  EXCEPTION_RECORDs = normal stack-guard/low-stack checks, NOT a real exception loop.

**Native `sample` of the warm game mid-startup — main thread hot leaves:**
`getenv` (#1), `hb_codegen_buffer_append`, `hb_jit_buffer_commit`/`make_writable`, `jit_range_has_prot`,
`exec_instr`, `operand_from_dec`, `hb_jit_helper_exec_ir_block_once`/`_two_block_loop`, `macrunner_hb_strieq`,
`macrunner_hb_psapi_name_is`, `snprintf`/`vsnprintf`/`__vfprintf`, `_platform_memmove/memset`, `mach_vm_region`.
Worker threads sit idle in `server_wait`/`server_select`/`NtWaitForSingleObject` (normal).

⇒ ABZU's main thread is **THROUGHPUT-BOUND in HyperBridge**: actively JIT-translating + executing UE4 startup
code (cache grows; progressing), with heavy **import-thunk dispatch overhead** — `getenv` (uncached env-flag
checks) is the single hottest leaf, plus the `macrunner_hb_strieq` import-name if-chain and `snprintf` in the
dispatch/diagnostic path. It is slowly progressing through UE4's enormous startup but does not reach
window-creation within the window, and the run ends `exit=1` at ~100–150s.

**REROUTE:** this is a **perf/maturity (JIT + import-dispatch throughput)** item, NOT a Lane A EC-unwind gate and
NOT a Lane D graphics gate (no window/swapchain attempted yet — it hasn't gotten there). Concrete first lever:
the **`getenv`/`macrunner_hb_strieq` import-thunk dispatch hotpath** (cache env flags in statics; hoist/optimize
the import-name chain or hash it) — same class as the prior HK JIT-throughput work (Codex). Secondary: confirm
whether `exit=1` at ~100–150s is a UE4 startup watchdog or just slow-progress (a longer window / more priming
test). The pak fix (commit 36890024) stands; this is the next, separate problem.

---

## DEFINITIVE (2026-06-22): post-pak gate = LOADER + import-resolution THROUGHPUT (NOT EC-unwind, no exception exists)

Verify-first, multi-evidence:
- **Zero real exceptions** across 5 runs: `callback-exception-record` / `setup_raise_exception` fired **0 times**;
  **0** non-zero exception codes (no c0000005/c++/etc.). The `callback-exception-stack` (16×) is only the
  `is_low_stack_access_fault` stack-guard CHECK with `code=0` recs. The `seh-ec-fpchain-bail` @ rva 0x8d2fc
  (= `wcslen+0x4`) recurs every run = normal wow64 boundary. **There is no EC-unwind exception to fix.**
- **Native samples of the warm 100%-CPU main thread**, ntdll ARM64 rvas resolved against aarch64-windows/ntdll.dll:
  `0x7a3f4 = LdrInitializeThunk+0x70`; `0x46884/0x49e14/0x49a18 = RtlImageNtHeader+0x1368/+0x48f8/+0x44fc`
  (internal `Ldrp*` — DLL load / import snap). A second sample showed `getenv` (#1) + `hb_codegen_buffer_append`
  + `macrunner_hb_strieq` + `macrunner_hb_psapi_name_is` + `snprintf` + JIT exec.

⇒ ABZU's main thread is **throughput-bound in `LdrpInitializeProcess` → DLL-load/import-snap**, amplified by the
**HB per-import-thunk dispatch** (`getenv` uncached env-flag checks + the `macrunner_hb_strieq` import-name
if-chain + `snprintf`). UE4 loads hundreds of DLLs/plugins; each import resolution pays the HB dispatch tax →
the main thread grinds and doesn't finish process init / reach window-creation within the window; the run ends
`exit=1` (or hits the timeout). This is **perf/maturity (loader + import-dispatch + JIT throughput)** — Codex/perf
domain — **NOT PE32 EC-unwind and NOT Lane D graphics** (it hasn't reached window/swapchain yet).

**Concrete first lever:** the HB import-thunk dispatch hotpath — (1) `getenv`/`macrunner_hb_env_flag` is the single
hottest leaf → find the per-import/per-block uncached env check and cache it in a static; (2) hash or hot-reorder
the `macrunner_hb_strieq` import-name chain; (3) gate the `snprintf` in the dispatch path. Re-sample after each.
The EC-unwind "gate" handed earlier was a misread — withdrawn.

---

## SETTLED empirically (2026-06-22): PE32 EC-unwind patch ENGAGES but ABZU still throughput-grinds → throughput verdict holds

Applied PE32's `EC-UNWIND-NTDLL-LOW-ADDRESS-LEAF-FALLBACK.patch` (PE-side `dlls/ntdll/signal_arm64.c`: removes the
host-boundary gate on the EC heuristic unwind + adds a leaf-EC-via-LR last resort). Rebuilt the PE ntdll.dll with
the in-tree toolchain (`engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal`, just needed in PATH — the
"can't build PE side" was a PATH issue, not a missing toolchain) and re-ran the warm-cache repro:
- **(a) ENGAGES: YES.** `macrunner-hb-arm64-ec-leaf-lr` fired (3×) and advanced the unwinder PAST the `0x8d2fc`
  bail (via LR → 0x...DCCC), reaching a new deeper boundary at rva `0x77538` in another module.
- **(b) PROGRESS: NO.** Window-creation markers stayed **0** (CREATEWINDOW/NCCREATE); ABZU exited `exit=1`; and
  the re-sample is **identical to baseline** — `macrunner_hb_call_import_thunk` ~67% (`try_kernel32_handle_semantic`
  5445 + `try_msvcrt_exit_semantic` 5443), `hb_jit_runtime_run` 3966.

**VERDICT: throughput holds.** The `0x8d2fc` bail was **benign** — advancing past it changes nothing on ABZU's
path (consistent with the verified 0 real exceptions). PE32's patch is a **valid EC-unwind correctness fix**
(engages cleanly, no regression) → **bank it in PE32's lane** (they commit it; it's shared-core), but it does NOT
unblock ABZU. **Reverted from abzu-lane** (doesn't help ABZU); clean PE ntdll.dll redeployed (source==dist).

**The real lever remains task #26:** the per-import-call semantic checks `macrunner_hb_try_msvcrt_exit_semantic` +
`macrunner_hb_try_kernel32_handle_semantic` = ~67% of main-thread time (called per import via
`macrunner_hb_call_import_thunk`). Optimizing/short-circuiting those is the path to finishing process-init →
window-creation → swapchain → Present.

---

## task#26 A/B (2026-06-22): patch engages + safe, but NO speedup — the 67% was misdiagnosed (it's _initterm static-init WORK, not the strieq dispatch)

Applied the HB lane's `TASK26-import-semantic-cache-FINAL.patch` (caches each import's semantic class at thunk
registration so the two hot dispatch fns short-circuit their per-call strieq). 7/8 hunks via `patch -p1 --fuzz=3`;
the msvcrt-guard hunk placed MANUALLY (my worktree's `try_msvcrt_exit_semantic` is an older variant). **Safety
verified**: my body's handled names ⊆ the classify tables (msvcrt 17⊆17, kernel32 84⊆85) → no under-classification,
no regression. Rebuilt ntdll.so, source==dist.

**A/B vs locked baseline (warm cache, main-thread sample):**
| metric | baseline | task#26 |
|---|---|---|
| `macrunner_hb_call_import_thunk` | 11012/16329 = **67%** | 11007/16333 = **67%** (no change) |
| `macrunner_hb_strieq` self | ~20 | **4** (patch engaged) |
| window-creation markers | 0 | **0** (no progress) |

**Why no speedup — corrected diagnosis:** the `macrunner_hb_strieq` self-time was only ~20 samples — the strieq
dispatch was NEVER the bottleneck. task#26 correctly cut it to 4, but that's negligible. The 67% in
`call_import_thunk → try_msvcrt_exit_semantic` is **dominated by `macrunner_hb_run_x64` recursion (16333)** — i.e.,
**`_initterm` executing the C++ static-init table (13777 callbacks)**, the WORK. My earlier "strieq storm = 67%"
baseline was a **cumulative-count misread** (try_msvcrt's 5445 is its run_x64 child, not its own strieq). `getenv`
self=728 (unchanged, untargeted).

**VERDICT:** task#26 is a valid, safe micro-optimization (engages, strieq 20→4, no regression) and is landed in
Main — but it does **NOT** move ABZU toward first frame, because ABZU's startup cost is the `_initterm` static-init
execution, not import dispatch. Reverted from abzu (doesn't help; abzu stays clean at the pak-fix milestone
36890024; task#26 reaches abzu via the next Main merge).

**The REAL lever = the `_initterm` C++ static-init grind** (the known ~200s ABZU static-init phase: 13777 callbacks
each via a nested `macrunner_hb_run_x64`). Throughput here = JIT lift+exec of those callbacks (builds on the prior
"pool JIT runtimes for nested run_x64 frames" work). ABZU still does not reach window-creation; window-trace markers
stay 0 (never attempted — it's still in static-init).

---

## REAL static-init lever found (2026-06-22, SELF-time / leaf parse): ~75% HB infrastructure overhead, NOT fundamental — codegen-vs-exec answer is NEITHER

Sampled the busy static-init thread (NOT the macOS main thread, which sits idle in mach_msg2_trap) and parsed
**SELF-time (leaf), separating SELF from CHILD** (the cumulative trap that bit the prior 3 reads):

| bucket | busy-thread SELF |
|---|---|
| **`hb_cache_get` (JIT translation-cache LOOKUP)** | **50.9 %** |
| **`getenv` / `__findenv_locked`** | **12.7 %** |
| **`hb_jit_runtime_reset` (nested-run_x64 pool reset)** | **11.3 %** |
| memops (`__bzero`/memmove/memset) | 6.3 % |
| **EXEC-run** (run_x64 self / jit_helper_exec) | **4.6 %** |
| **CODEGEN-lift** (codegen/decode/operand) | **1.8 %** |

**Codegen-vs-exec verdict: NEITHER.** Actual JIT work is only ~6% (codegen 1.8% + exec 4.6%). The ~200s grind is
**~75% HB per-frame infrastructure overhead**: the translation-cache LOOKUP dominates (`hb_cache_get` 50.9%) —
the 13777 static-init callbacks (and their sub-blocks) repeatedly look up already-lifted blocks via a nested
`macrunner_hb_run_x64` each. This is **OPTIMIZABLE, not fundamental.**

**Three levers (route to the HB-throughput owner; biggest first):**
1. **`hb_cache_get` (50.9%)** — speed/memoize the block lookup; reuse lifted-block pointers across the nested
   run_x64 frames instead of re-looking-up per callback. Builds on the runtime-pool fix b6c9805.
2. **`getenv` (12.7%) — EXACT SITE FOUND:** `macrunner_hb_trace_pe_call12_edge_budget_allows()`
   (macrunner_hb.c:2884) calls `getenv("...PE_CALL12_EDGE_BUDGET")` (line 2887) AND
   `macrunner_hb_env_enabled("...PE_CALL12_EDGE")` (2890) on EVERY edge — UNCACHED, and the getenv runs even
   when tracing is OFF (it precedes the enabled-check). Fix: cache both in statics (`static int en=-1; if(en<0)
   en=...`), HK environ-lock pattern → 2 getenv total instead of 2 per edge.
3. **`hb_jit_runtime_reset` (11.3%)** — the per-nested-frame pool reset (+ `__bzero` 4.7%); reduce reset work.

**exit=1 cause (clarified): NOT a crash / static-init error.** run-exit=0; no fatal/unhandled-exception/0x58576f/
watchdog in the log; last activity is a normal thread-init. It's a clean `exit=1` — consistent with a UE4
startup watchdog / give-up on the unfinished slow grind (or headless quit); UE4's own log channel (suppressed by
WINEDEBUG=-all) would name it. So: speeding up static-init (the 3 levers) should let it FINISH → reach
window-creation, which also resolves the exit. ABZU still never reaches window-creation (markers=0; still in
static-init).

---

## task#28 A/B (2026-06-22): levers WORK (infra collapsed, self-time-verified) but ABZU still gives up at window=0 — throughput was NOT the gate to pixels

Applied all 3 task#28 levers (#2 hand-applied — its patch had a malformed `+++ b/tmp/mh_cur.c` header + a
task#26-based offset; #1/#3 git-applied to libhyperbridge). Built via the in-tree toolchain
(`engine/toolchain/llvm-mingw-20260505-...` in PATH; copied Main's `engine/hyperbridge/Makefile`, dropped the
abzu-absent `hb_wow64cpu.c` from SRCS → libhyperbridge.a rebuilt → ntdll.so relinked). Per-lever + cumulative
SELF-time (busy static-init thread, leaf parse, SELF≠CHILD):

| bucket | baseline | after levers |
|---|---|---|
| `hb_cache_get` (cache lookup) | 50.9% | **0.0%** (#1 hash-index + #3 no-wipe) |
| `hb_jit_runtime_reset` | 11.3% | **0.0%** (#3) |
| `getenv`/`__findenv_locked` | 12.7% | 12.3% (lever #2 fixed call12-edge; **but a 4th uncached site re-emerged → 36.6%, then a 4th lever I added cached `hb_arm64_codegen.c` `jit_direct_mem/scalar/stack` → 12.3%**; ~12% residual from yet another site) |
| EXEC (`run_x64` self) | 4.6% | **28.3%** (real JIT exec, now dominant as infra collapsed) |
| CODEGEN-lift | 1.8% | 4.7% |

**The ~75% HB infra overhead collapsed** (cache 51%→0, reset 11%→0). Static-init is much faster — ABZU now
exits at **~40s** vs the baseline **~100–150s**.

**THE QUESTION — does it reach window-creation? NO.** ABZU still exits **clean (`exit=1`, run-exit=0, no crash,
no 0x58576f)** at **window-creation=0** — reaches the same phase as baseline (DXGI/D3D11CreateDevice wired,
winemetal "Failed to set Metal cache path"), then gives up before creating its window. **So throughput was NOT
the gate to pixels** — collapsing it just makes the same give-up happen sooner.

**The real gate to pixels = the clean `exit=1` give-up** (UE4 quitting before window-creation), which is NOT
throughput-gated and has no logged reason under `WINEDEBUG=-all`. Likely a UE4 startup watchdog OR (more likely)
UE4 failing to create its window/RHI in the headless/macdrv environment — consistent with the known **"macdrv
get_win_data NULL" Gate A graphics issue**. NEXT: a run with UE4/graphics/window logging enabled (NOT -all) to
name WHY UE4 exits before window-creation → route to **Lane D** (graphics/macdrv window-creation). The 4 throughput
levers are valid (collapse the infra; #1/#2/#3 landed in Main) — the **4th (codegen `jit_direct_*` getenv cache in
`hb_arm64_codegen.c`) is a NEW finding** for the HB-throughput owner to land in Main. They speed startup but do
not, alone, reach pixels.

---

## Naming the give-up gate (2026-06-22): NOT throughput, NOT 0x8d2fc EC-unwind — GRAPHICS/RHI init (Lane D). c0000005 NULL+0x3b8 on the path; UE4 log unavailable (shipping-strip)

Ran with logging to name the clean exit=1 give-up. Findings:
- **UE4's own log is UNAVAILABLE** — ABZU is a SHIPPING build (strips GLog); `-log -stdout -unattended` produced
  **0 UE4 log lines** (verified). So UE4's reason can't be read directly.
- **+win,+macdrv,+seh** revealed: window CLASS registration happens (`server-create-class` atom 8001/c01d) but
  `CreateWindow` NEVER fires (markers=0). One concrete fault: a handled **c0000005 NULL-deref at NULL+0x3b8**
  (`handle_syscall_fault code=c0000005 addr=0x0 info[1]=0x3b8`, x0=NULL) via `route_x64_callback_fault` (guest x64
  callback pc=0x87ef3cf3330) during graphics init (after class-reg, before the winemetal "Failed to set Metal cache
  path" line).
- **DXMT is wired** (dist x86_64-windows/d3d11.dll 3.3MB + dxgi.dll 1.3MB) **but did ZERO DXGI work** — its
  `*_dxgi.log` is **0 bytes** → UE4 dies AT or BEFORE its first DXGI call.
- **EC-unwind tested + RULED OUT as the gate:** applied PE32's `EC-UNWIND-NTDLL-LOW-ADDRESS-LEAF-FALLBACK.patch`
  (hand-applied to signal_arm64.c, built PE aarch64-windows/ntdll.dll). `arm64-ec-leaf-lr` ENGAGES (fires ×3 at rva
  0x8dc78) — but the **outcome is UNCHANGED** (window=0, exit=1). A *different* EC frame still defeats ALL unwind
  heuristics: `unwind-unsafe-boundary ... stop-unwind` at **rva 0x77538** (kind=0, images 0x87FFF7B0000 /
  0x87FFED50000). Since fixing the 0x8d2fc-class leaf changed nothing, 0x8d2fc-unwind was NOT the gate. (Note: the
  c0000005 was already "handled / returned to user mode" pre-patch, so the unwind wasn't blocking its delivery.)

**NAMED GATE (best evidence): UE4 graphics/RHI init fails before the first DXGI call → UE4 cleanly aborts
(RequestExit-class, exit=1) before window-creation.** DXMT (wired) gets no DXGI call; the c0000005 NULL+0x3b8 is the
concrete fault on this path (UE4 likely derefs a NULL graphics object). **RULED OUT:** throughput (collapsed, made
the give-up sooner not gone) + 0x8d2fc EC-unwind (fixed, no change). **→ ROUTE TO LANE D** (DXMT/DXGI init — it
proved the D3D11→Metal present path). **Decisive Lane-D probe:** instrument DXMT's `CreateDXGIFactory(1/2)` /
`IDXGIFactory::EnumAdapters` / `D3D11CreateDevice` entry points to log if/when UE4 calls them + their return → find
where UE4's graphics init dies (the 0-byte DXMT log says it's at/before the very first DXGI call). Secondary (Lane A,
if graphics proves clean): resolve the c0000005 guest RVA (host pc 0x107910b4c / guest callback 0x87ef3cf3330) + the
rva 0x77538 EC-unwind. abzu state: throughput levers (4) + EC-unwind patch applied (source==dist) on top of pak fix
36890024; warm cache primed.
