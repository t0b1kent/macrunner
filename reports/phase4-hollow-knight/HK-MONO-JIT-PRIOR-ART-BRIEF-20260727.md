# Prior art: how other x86→ARM64 translators handle Mono's JIT

Date: 2026-07-27. Coordinator research brief for the HK Mono/JIT lane. **Everything here is
external prior art, not our measurement.** Treat each item as `[HYPOTHESIS]` until tested
against our runtime. Sources at the bottom.

## Why this is relevant

We are not the first to stack a JIT (Mono) on top of a JIT (our translator). Two projects
solve exactly this problem class — box64 (x86-64→ARM64 on Linux) and FEX-Emu (including its
Wine / Arm64EC / WOW64 path, which is architecturally closest to us). Both had to add
**Mono-specific** handling. Their symptom vocabulary matches ours: stutter, spurious hangs,
incorrect invalidations.

FEX states the general principle plainly: *"When JITs get stacked in emulation it is almost
always a bad time"* — Mono's use of self-modifying code *"causes significant code
invalidation and stutters due to how we interact with each other."*

## The three concrete mechanisms they found

### 1. Self-modifying code / stale translations — the strongest candidate

FEX's release notes for FEX-2601 record that they *"resolved some handling of self-modifying
code on our Wine implementation that could fix some **spurious hangs** or incorrect
invalidations"*, alongside "invalidate code in freed memory after the free syscall" and
"improve handling of RWX memory". FEX tracks guest allocations of executable memory and
clears its JIT caches both for true self-modifying code and for libraries being loaded.

**Why it fits our symptom exactly:** Mono compiles a method, writes the new machine code into
an executable page, then executes it. If our translator has already cached a translation for
that page and does not invalidate on write, the guest keeps executing the *previous*
translation. A thread doing that is alive, running, burning CPU, and never advancing — which
is precisely what we observe after `Performing automatic level start.`

**The specific mechanism to check in our code**, from Darek Mihocka's ARM64 Boot Camp (which
describes the Windows-ARM64 xtajit design we are effectively reimplementing): *flushing the
instruction cache sends a callback to the x64 translator to flush any cached translations.*
So the question for us is concrete and answerable offline:

> Does a guest `NtFlushInstructionCache` / `FlushInstructionCache` (and the ARM64 cache-
> maintenance path Mono's `__clear_cache` compiles to) actually reach HyperBridge's
> translation-cache invalidation? If that callback is missing or partial, stale translation
> is not a possibility — it is guaranteed.

### 2. Translation block size — box64 turns big blocks OFF for Mono

box64 detects `MonoBleedingEdge` (Unity's Mono) and applies `BOX64_DYNAREC_BIGBLOCK=0`. Its
docs recommend that setting for "programs using lots of threads and JIT, like Unity", and
box64 added a mechanism to build smaller blocks for JIT'd programs and to cancel dynarec
block construction if it segfaults.

**Why:** a big translated block spans more guest bytes, so it is far more likely to contain
code that Mono later rewrites — widening the stale-translation window and making
invalidation coarser. Worth checking whether our block formation has an equivalent knob and
what it does around Mono-written pages.

### 3. Memory ordering — box64 turns strong memory ON for Mono

The same box64 auto-detection applies `BOX64_DYNAREC_STRONGMEM=1` (emulate x86's TSO). FEX
frames its Mono work as making it *"less likely to crash when TSO memory model emulation is
**disabled**"* — i.e. TSO emulation is exactly the thing Mono is sensitive to.

**Why it fits:** Mono's `mono_jit_info_table_find_internal` uses **hazard pointers** to read
the JIT info table lock-free. Hazard-pointer algorithms are retry loops: publish a pointer,
re-read, retry if it changed. Under x86-TSO those loops terminate; under ARM's weaker
ordering, a translator that drops the implied ordering can leave a reader retrying against a
value it never observes being published. **A lock-free retry loop that never observes the
publish is a live, spinning, non-advancing thread** — again our symptom, and it lands in the
same `jit_code_hash` / hash-table-lookup neighbourhood our stack capture points at.

We already have the relevant knobs in-tree — `MACRUNNER_HB_SYNC_IMPORT_BARRIER` (presence-
based: `=0` turns it ON) and `MACRUNNER_HB_JIT_DIRECT_MEM` — so this is testable without new
machinery.

## Suggested order for the lane

Cheapest-and-most-decisive first:

1. **Offline, no run:** trace whether guest instruction-cache flush reaches our translation-
   cache invalidation at all. A missing callback is a code-reading result, not an experiment.
2. **Offline, no run:** check whether Mono-written executable pages are tracked as
   self-modifying at all, and what happens to cached translations when they are written.
3. **One run:** the TSO/ordering knob against the ordinal-200 regression detector.
4. **One run:** block-size behaviour around Mono pages, if 1–3 have not settled it.

Note that 1 and 2 cost no run slot and could settle the mechanism question that the verdict
left open — *why a finite spin is followed by permanent non-advancement.* A stale translation
explains that shape better than the spin itself does: the spin ends, the thread resumes, and
resumes into code that can never make progress.

## Honest limits of this brief

- None of this is measured on our runtime. It is other projects' experience with the same
  guest (Unity/Mono) and the same problem class (JIT-on-JIT), which makes it a strong
  prior — not evidence.
- FEX deliberately did not publish the details of its Mono detection: *"I won't go in to the
  details since it's fairly complex."* So we can borrow the *direction*, not their patch.
- box64 and FEX target Linux; we target macOS with ARM64 Wine. The Wine/Arm64EC parts of FEX
  are the closest analogue, not the Linux-native parts.

## Sources

- FEX-Emu, FEX-2509 release notes — Mono detection on Arm64EC/WOW64, SMC-driven invalidation
  and stutter, TSO sensitivity: https://fex-emu.com/FEX-2509/
- FEX-Emu, FEX-2601 release notes — SMC handling on Wine fixing spurious hangs / incorrect
  invalidations; invalidate code in freed memory; RWX handling:
  https://github.com/FEX-Emu/FEX/releases/tag/FEX-2601
- box64 documentation and changelog — MonoBleedingEdge detection, `BOX64_DYNAREC_BIGBLOCK=0`,
  `BOX64_DYNAREC_STRONGMEM=1`, smaller blocks for JIT'd programs:
  https://github.com/ptitSeb/box64/blob/main/docs/CHANGELOG.md and
  https://manpages.debian.org/testing/box64/box64.1.en.html
- Mono runtime, `jit-info.c` — `mono_jit_info_table_find_internal` hazard-pointer access:
  https://github.com/mono/mono/blob/main/mono/metadata/jit-info.c
- Darek Mihocka, ARM64 Boot Camp — icache flush as a callback into the x64 translator to
  flush cached translations: http://www.emulators.com/docs/abc_exit_xta.htm
- Mono, "A tale of an impossible bug: big.LITTLE and caching" — how incomplete icache
  flushing manifests: https://www.mono-project.com/news/2016/09/12/arm64-icache/
