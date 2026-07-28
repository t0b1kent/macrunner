# The ARM64X dual-`.data` twin class: the mirror was the bug

**Lane:** HK-TWINSWEEP (offline) · **Date:** 2026-07-28 · **Status:** class closed, fix in tree, not deployed

## Verdict

The ARM64X twin hazard we have been chasing **does not exist as described**. The linker emits both
`.data` views fully and correctly initialised, including every self-referential static. Both
recorded "casualties" of the class were written by `macrunner_hb_sync_locale_ec_copies` itself,
which subtracts the twin delta where it must add it.

That mirror has **never once reached a twin** in its entire existence. What it did instead was
`memcpy` 8192 bytes of zeroed `.bss` over 30 live statics on every process start — among them
`reg_mui_cache`, `reg_mui_cs`, `reg_mui_cs_debug` and `entry_sintlsymbol`, i.e. exactly the two
symbols the class was named after.

## What was inspected

| | |
|---|---|
| Binary | `engine/wine/dist-arm64ec-spike/lib/wine/aarch64-windows/kernelbase.dll` |
| SHA-256 | `b7e96fb9e48c5825071f608cb4e18afcf254cf7bc7b5062e62bc736128ede3d4` |
| Tools | `llvm-nm`, `llvm-objdump`, `llvm-readobj` from `engine/toolchain/llvm-mingw-20260505-ucrt-macos-universal` |
| Machine | `IMAGE_FILE_MACHINE_ARM64X`, single `.data` at RVA `0x14F000`, vsize `0x8678`, raw `0x3800` |

Note the `dist-arm64ec-spike` at the repo root is a near-empty stub; the live tree is
`engine/wine/dist-arm64ec-spike`.

Symbol names come from **DWARF**, not a COFF symbol table (`PointerToSymbolTable` is 0). Any tool
that reads only the COFF table will report "no symbols" here — that is a tooling artefact, not an
absence.

## The twin set, derived from the binary

301 `.data` symbols → **141 twin pairs**, in four strictly non-overlapping zones:

| zone | range | delta to twin |
|---|---|---|
| native `.data` | `0x18014F030 .. 0x180150B28` | `+0x1B00` (5 pairs `+0x1AF8`) |
| EC `.data` | `0x180150B28 .. 0x180152618` | |
| native `.bss` | `0x180152638 .. 0x180154D00` | `+0x27E8` |
| EC `.bss` | `0x180154E20 .. 0x1801574E8` | |

**Zone disjointness is the twin-vs-name-collision discriminator.** Every duplicated name has
exactly one copy in a native zone and one in the corresponding EC zone; a mere name collision
between two file statics would put both copies in the *same* zone, and would appear with
multiplicity 4 rather than 2. Observed multiplicities: 137×2, 1×8 (`critsect_debug`, four distinct
statics in four files × two views), 19 singletons (all ARM64EC/CFG helper import slots, filled by
the loader — no hazard). **Zero collisions.**

Which zone is which is fixed by the **CHPE code map**, not by guesswork:

```
range 0: RVA 0x001000 .. 0x0775E4  type=0 ARM64 (native)
range 1: RVA 0x078000 .. 0x0E48DC  type=1 ARM64EC
range 2: RVA 0x0E5000 .. 0x0EC330  type=2 X64
```

Functions are twinned too (1370 duplicated names). `load_mui_string` exists at RVA `0x5CBC0`
(type 0, native) and `0xC46D8` (type 1, EC). **The lower blob is native; the EC twin is at
+delta.** I initially had this backwards and the code map corrected it.

## The load-time hazard class is empty

Byte-comparing **all 75 `.data` twin pairs** in the shipped image:

| result | count |
|---|---|
| byte-identical | 23 |
| pointer-shifted, self-consistent per view | 52 |
| **EC copy zero while native non-zero** | **0** |

Every self-referential static is correctly initialised in **both** views:

```
native reg_mui_cache 0x1801525B0 : {0x1801525B0, 0x1801525B0}   <- points at ITSELF
EC     reg_mui_cache 0x180150AB0 : {0x180150AB0, 0x180150AB0}   <- points at ITSELF
native reg_mui_cs    -> DebugInfo 0x180152580 (native debug struct)
EC     reg_mui_cs    -> DebugInfo 0x180150A80 (EC debug struct)
native critsect_debug-> CriticalSection 0x180150B38, ProcessLocksList self-linked 0x180150B88
EC     critsect_debug-> CriticalSection 0x18014F040, ProcessLocksList self-linked 0x18014F090
```

Supporting evidence that nothing rewrites this at load time:

- **Base relocations cover both zones** (81 EC + 79 native DIR64 entries in `.data`), so both
  copies survive relocation. (`llvm-readobj` reports double these counts because it prints the
  ARM64X and ARM64EC views separately.)
- **The DVRT is 152 bytes and has zero `.data` entries** — all its ARM64X fixups are in `.text`
  and `.rdata`. The loader never rewrites `.data` per view.

So `LIST_INIT`, `CRITICAL_SECTION` initialisers and friends are *not* a hazard here. File-static
symbols resolve within their own object, so each view's copy is linked against its own view's
addresses.

## What actually happened

`macrunner_hb_sync_locale_ec_copies` does `memcpy(&g - delta, &g, sizeof(g))`. It runs in the
**native** view — as it assumed — but the native blob is the *lower* one, so the twin is at
`+delta`. Subtracting never lands on a twin:

- **CORE mirrors** (`delta 0x27E8`, `.bss` globals) land back inside initialised `.data`.
- **ENTRY mirrors** (`delta 0x1B00`, `.data` globals) land *below* the section, in `.buildid`.
  These have never copied anything anywhere.

The damage is dominated by one line. `static CPTABLEINFO codepages[128]` is 8192 bytes:

```
&codepages           RVA 0x152BD8  (native .bss, still all zeros at init_locale time)
dest = &codepages - 0x27E8
                     RVA 0x1503F0  (native .data)     <-- writes 0x1503F0..0x1523F0
correct twin = +0x27E8
                     RVA 0x1553C0  (EC .bss)
```

That range covers the tail of native `.data` and runs on into the EC `.data` blob. The 30 native
statics it zeroes:

```
entry_spositivesign  entry_sshorttime  entry_sthousand  entry_syearmonth  entry_sshortdate
entry_stimeformat    entry_sintlsymbol  __wine_dbch_kernelbase  __wine_dbch_heap
__wine_dbch_virtual  __wine_dbch_globalmem  memstatus_section  critsect_debug
__wine_dbch_path     __wine_dbch_process  restart_section  shutdown_priority
restart_section_debug  __wine_dbch_reg  reg_mui_cs  reg_mui_cs_debug  reg_mui_cache
__wine_dbch_security  SetFileSecurityW.is_office  __wine_dbch_string  __wine_dbch_sync
__wine_dbch_thread   ConvertThreadToFiberEx.is_halo_mcc  __wine_dbch_ver  __wine_dbch_volume
```

Both "casualties" are on that list:

1. **`entry_sintlsymbol`** (RVA `0x150878`) — the NULL subkey at `kernelbase+0x59930`.
2. **`reg_mui_cache`** (RVA `0x150AB0`) — zeroed to `{0,0}`, so `LIST_FOR_EACH_ENTRY` starts at
   `(char*)0 - offsetof(...)` and the first field load faults. The faulting code is native
   `load_mui_string` at RVA `0x5CBC0`, which addresses `reg_mui_cache` at `0x180150AB0` — the
   native copy — via `adrp x8, 0x180150000 / ldr x24, [x8, #0xab0]`.

### Runtime confirmation

From `reports/phase4-hollow-knight/laneA-INPUT-MUIFIX-try16-172249/run.log`, image at
`0x87FFF830000`:

```
macrunner-hb-sync-locale-ec: verify native_sort=0000087FFF982AD8 ec_sort=0000087FFF9802F0
macrunner-hb-sync-locale-ec: verify registry ... native_sintl_value=0 native_sintl_subkey=0 ...
```

- `native_sort` → RVA `0x152AD8` = the **native** `.bss` copy of `sort`. The mirror is in the
  native view, and `&g` is the native copy.
- `ec_sort` → RVA `0x1502F0`, which is **inside native `.data`** — not a copy of `sort` at all.
  The real EC twin is `&sort + 0x27E8` = RVA `0x1552C0`.
- `native_sintl_value=0 native_sintl_subkey=0` **is the corruption, printed back one line after
  it happened.** It was read as evidence *for* the missing-mirror theory, which is how the
  mirror's own damage kept motivating more mirroring.

This is still live: the newest run (`laneA-JITHINT-try20-try1-183535`, 18:56, i.e. *after* the
input fix landed at 18:31) prints the same `ec_sort=0000087FFF9802F0`.

## Why the fix is "disable", not "flip the sign"

Flipping the sign is not enough, because the deltas are hand-typed layout constants that nothing
re-derives:

- Adding the few statics needed for view detection moved the CORE delta from `0x27E8` to `0x2808`
  (shipped `b7e96fb9`: 65 pairs at `0x27E8`; rebuild `407e6a0d`: 86 pairs at `0x2808`).
- A shift that small stays in the correct half of `.data`, so a region/bounds check does **not**
  catch it. It would just memcpy every global `0x20` bytes off its twin — the same corruption,
  quieter.
- The delta is not even uniform within one build: both images carry a second family of 5 pairs at
  `0x1AF8` instead of `0x1B00`, because the EC-only `__imp_aux_RaiseException` shifts part of the
  blob. **A single constant is wrong for those 5 by construction.**

And the mirror is demonstrably not load-bearing: it has never copied a byte to a twin, and the
entire locale/NLS path has worked anyway. The only thing it ever did was corrupt 30 statics.

## Change made

`engine/wine/dlls/kernelbase/locale.c`:

- `MACRUNNER_HB_LOCALE_EC_MIRROR_ENABLED 0` — mirroring off; the `memcpy` is unreachable.
- View detection via the CHPE code map (`macrunner_hb_detect_view`), retained for diagnostics and
  as the basis for a correct direction if re-enabled.
- `macrunner_hb_mirror_ec_copy` reworked: direction derived from the executing view, plus the
  invariant **a `.data` object's twin is in `.data`, a `.bss` object's twin is in `.bss`** — which
  alone would have caught both halves of this bug.
- The two comments that encoded the false premise are corrected in place, with the address
  evidence, so the next reader does not re-derive the wrong model.

### Verified in the artifact, not the source

| | |
|---|---|
| Built | `engine/wine/build-arm64ec-spike/dlls/kernelbase/aarch64-windows/kernelbase.dll` |
| SHA-256 | `05fa3326982e0e2c4528f947b46cc05c1f513fe842ab3cb2781896de65926a75` |
| Both views compile | `aarch64-windows/locale.o` + `arm64ec-windows/locale.o`, clean under `-Wall` |
| `macrunner_hb_mirror_ec_copy` | **2 copies in shipped `b7e96fb9` → 0 in the new build** — the linker eliminated the memcpy as dead code |
| Twin integrity | 0 EC-zero pairs in the new artifact (23 identical / 52 pointer-shifted / 86 bss) |

**Not deployed.** `dist` still holds `b7e96fb9`. Another lane is active and owns the title slot.

## Other modules

`scripts/build-wine-arm64ec-spike.sh:44` configures `--enable-archs=arm64ec,aarch64,i386,x86_64`,
so **every one of the 644 shipped `aarch64-windows` builtins is dual-compiled ARM64X**. The twin
condition is universal, not a kernelbase peculiarity. (The build tree shows this directly:
`dlls/kernelbase/aarch64-windows/locale.o` and `dlls/kernelbase/arm64ec-windows/locale.o`.)

Checked individually with the same `llvm-nm` + byte-compare method — **978 twin pairs, 0 EC-zero
in every module**:

| module | pairs | dominant delta |
|---|---|---|
| kernelbase | 140 | `0x1B00` / `0x27E8` |
| ntdll | 184 | `0xB40` |
| ucrtbase | 187 | `0x1838` |
| ole32 | 146 | `0x1680` |
| user32 | 106 | `0xD50` |
| gdi32 | 80 | `0x7B0` |
| combase | 64 | `0x3E8` |
| kernel32 | 40 | `0x1F8` |
| advapi32 | 27 | `0xA4` |
| win32u | 4 | — (too few to establish a family) |

**Not checked individually: the other 634 builtins.** A structural screen over all of them was
inconclusive — modules with small `.data` do not give the correlation enough signal, and the
detector returns a best-in-band delta whether or not a twin structure exists. Those screen results
are not evidence and are not reported as such. Since no module can exhibit the load-time hazard by
construction (the linker initialises both copies; the DVRT never touches `.data`), the residual
risk in the unchecked 634 is that some *other* module carries its own hand-rolled mirror with the
same sign error.

**Swept, and there is none.** `git grep -E 'macrunner_hb_mirror|_EC_DELTA|EC_COPY'` over
`engine/wine/**/*.{c,h}` returns hits in exactly two files: `kernelbase/locale.c` (7, the mirror
itself) and `kernelbase/registry.c` (1, a comment referencing it, now corrected).
`macrunner_hb_sync_locale_ec_copies` was the only instance of this construct in the tree.

## The residual class: runtime-split state (real, but not a mirroring problem)

The lane asked for each unmirrored twin to be classified `memcpy` vs `init-in-place`. **Neither
axis applies** — no twin needs either, because the linker already initialises both copies. The
question that survives is different: for a twin populated *at runtime*, is a per-view instance
self-consistent, or must the value be process-global?

84 of the 141 pairs are unmirrored. Most are **split-safe**, and for a structural reason worth
recording: a file static and the critical section guarding it are *both* twinned, so each view
gets a complete, independent, self-consistent instance. `reg_mui_cs` + `reg_mui_cache` +
`reg_mui_cache_count` is the model case — each view caches into its own list under its own lock
and never sees the other's. The same holds for `tzname_section`/`cached_tzname`,
`memstatus_section`/`GlobalMemoryStatusEx.cached_status`, `restart_section`/`restart_*`,
`exclusive_datafile_list_section`/`exclusive_datafile_list`, and every `*.once` /
`*.init_once` guard (double init of an idempotent cache). The `__wine_dbch_*` channel descriptors
are likewise resolved independently per view to the same answer.

**Split-unsafe** — values that must be process-global because one view writes and another reads:

| symbol | file | why it must be global |
|---|---|---|
| `top_filter` | `debug.c:48` | `SetUnhandledExceptionFilter` writes (`:457`), `UnhandledExceptionFilter` reads (`:790`). Guest sets it through EC; a raise through native sees NULL and the guest handler never runs. |
| `global_data`, `next_free_mem` | `memory.c:979-980` | The GlobalAlloc/LocalAlloc handle table. A handle allocated through one view and freed through the other indexes a different table. |
| `oem_file_apis` | `file.c:98` | `SetFileApisToOEM/ANSI` vs every A→W conversion — silent per-view behaviour divergence. |
| `is_wow64` | `main.c:38` | Set once in DllMain; the other view's copy stays FALSE. |
| `special_root_keys` | `registry.c:63` | Lazy caching is self-consistent, but `RegOverridePredefKey` (`:555`) is not — an override set in one view is invisible to the other. |

Reachability rests on **both views executing in one process**, which is the design: native for
host-side and intra-module calls, ARM64EC for anything entering from the x64 guest through the
21296-byte `.hexpthk` thunk table. Both are demonstrably live here — the mirror and
`load_mui_string` run native (code-map type 0), while HK's guest API calls necessarily enter EC.
**[HYPOTHESIS]** the specific per-symbol crossings above; I have not traced a guest
`SetUnhandledExceptionFilter` or `GlobalAlloc` to its view, and no failure has been observed for
any of them.

**These are latent and none is the current wall.** Recording them so they are not rediscovered as
a fresh "twin class". Note that `memcpy` mirroring would *not* fix any of them — mutable state
shared across views needs one instance, not two synchronised ones.

## Consequences for the input fix

`reg_mui_cache_ensure_init()` (commit `e4b90849`) worked, but not for the stated reason. It
repaired one of the 30 statics *after* the memcpy zeroed it. The other 29 stayed corrupt — the
critical section guarding that very cache among them.

With the mirror off, all 30 keep their correct linker-provided values and `ensure_init` becomes a
genuine no-op (`.next` is non-zero on disk). It is harmless defensive code; I left it.

**This is a real behaviour change and wants one run to confirm.** Statics that were silently zero
for months are now correctly initialised — including `memstatus_section`, `restart_section` and
four `critsect_debug` structs that previously had a NULL `DebugInfo`. More correct, but not
identical. I could not run it: this is an offline lane and another lane owns the title slot.

## Open items for the operator

1. **Deploy + one HK run.** Expect `skip reason=disabled-...` in the log and no
   `ec_sort=...9802F0` line. Input should still work, via correct statics rather than a repair.
2. **Is the mirror needed at all?** It has never worked, and the locale path worked anyway. If a
   run confirms nothing regresses, delete it rather than fix it. Re-enabling requires runtime
   delta derivation — a sketch for the initialised half is in the source comment; the `.bss` half
   has no self-referential anchor and needs a separate answer.
3. ~~Sweep for sibling mirrors.~~ **Done — none exist** (see *Other modules*).
4. **Optional, later:** the runtime-split twins (`top_filter`, `global_data`, `oem_file_apis`,
   `is_wow64`, `RegOverridePredefKey`). Latent, not the current wall, and not fixable by mirroring.

## Method notes worth keeping

- **`llvm-readobj` double-reports ARM64X images** (once per view). Relocation and section counts
  from it are 2× the truth.
- **`ps -o comm` truncates**, which hid a live `wineserver` behind a long path in this session.
  Match on the full command, or on `comm` with the knowledge that it is truncated.
- The mirror's own verify `MESSAGE` had the evidence in it from the beginning. It was mislabelled
  — `native_*` was the native copy, but `ec_*` was never an EC copy — and the labels were believed
  over the addresses.
