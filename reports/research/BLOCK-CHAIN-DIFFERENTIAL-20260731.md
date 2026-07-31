# Block chaining: the defect is general, not one block pair — 2026-07-31

First runs of `MACRUNNER_HB_BLOCK_CHAIN=1` since the W^X fix landed. Two questions were open:
does chaining still die, and if so is it the known `0x87ef246adb6 -> 0x87ef2469a30` pair or
something systemic. Both are now answered.

## Why chaining matters at all — the prize, measured

Every guest basic block round-trips through the C dispatcher. `avg_chain` measures **exactly
1.0000** over 205.8 M dispatches, and the gate comment records it has never been otherwise.

Per dispatch the engine pays: 2-3 probes into a 524288-entry (~36 MB, past LLC) open-addressed
block cache; `smc_reverify_entry`, which re-reads and FNV-hashes the block's guest bytes for
W+X regions (Mono's code heap qualifies, so Mono blocks pay it every time, and the IR cache
hashes the same bytes a second time); a ~790-byte `memset` of the fault frame; a 760-byte
`hb_ctx_snapshot_save`; `sigsetjmp`; three `__thread` accesses that each become a
`_tlv_get_addr` call in a dylib; a PC sync; and a rescan of the block's IR to classify its
terminal.

**~350-500 host instructions and ~1.5 KB of memory traffic per block**, against a translated
block averaging **34 ARM64 instructions** of which ~23 are guest work — 15-20x more machinery
than work. Wall-clock anchor: **205.8 M dispatches in 85.6 s = 416 ns per guest basic block**
for 4.64 guest instructions. On the profiled thread, translated code is 2.4-6% of samples while
round-trip machinery is ~54%.

This is why none of today's other fixes moved the wall clock: they all worked INSIDE blocks
(helper calls, flags, SMC, cache opens), and the cost is BETWEEN them.

## Run 1 — chaining on, unmodified

`reports/phase4-hollow-knight/laneA-chain-on-a1-try1-211849`

- Control arm (chaining off) reached "Restored language"; xinput 86.415 s; last activity 624.0 s.
- Chained arm: **no milestone, last activity 36.9 s**, process alive until the 660 s timeout
  (`exit=124`), i.e. the main thread died and the process hung rather than crashing.
- `macrunner-hb-run-exit: status=c000007b reason=runtime pc=0x87ef2469a30
  last_block=0x87ef246adb6`.

**The W^X fix holds.** Earlier attempts died with `exit=5` within a second of the first dispatch,
building a trampoline into a page `jit_commit_blob` had just re-protected. This run got through
winemac.drv init, Cocoa startup, NSApp entering its run loop and `CreateDXGIFactory1` before
failing. Four of the five documented defects are genuinely closed; the register corruption is
the one that is not.

## Run 2 — the near-filter finally sees the edge

`MACRUNNER_HB_CHAIN_EDGE_NEAR=0x87ef2469a30` + `MACRUNNER_HB_TRACE_CHAIN_EDGE=1`.
`reports/phase4-hollow-knight/laneA-cn-trace-a1-try1-214336`

**12 798 edges logged, 12 766 of them NEAR.** The old 32-edge cap never reached this region —
exactly what the filter was written for. The last edge before death:

```
NEAR from=0x87ef246adb6 pc_after=0x87ef2469a30 blocks=2 steps=71
     rcx 5000000000000 -> f0e0993f
     rdx 11a5fdb60     -> 320eec31
     rbp 8 -> 8
```

`f0e0993f` and `320eec31` are the same garbage values three earlier runs reported. They are
32-bit quantities where 64-bit stack pointers belong: the high half is gone.

And the corruption does not start there. One edge earlier:

```
NEAR from=0x87ef246aed4 pc_after=0x87ef246ad96 blocks=3 steps=29
     rcx 0->38  rdx 87ef2a74744->0  rbp 1186a1f10->8
```

`rbp` collapses to 8 and `rdx` is zeroed on the PREVIOUS edge. The fatal edge is the consequence,
not the cause: state degrades across several chained edges before anything faults.

## Run 3 — the differential, and the verdict

`MACRUNNER_HB_CHAIN_REFUSE_NEAR=1` refuses to chain exactly the selected edges. The gate's own
comment sets the criterion: "If the fault then vanishes, the defect belongs to this block pair;
if it merely relocates to another chained edge, the defect is general to chaining."

`reports/phase4-hollow-knight/laneA-cn-refuse-a1-try1-215502`

```
trace  : status=c000007b reason=runtime      pc=0x87ef2469a30
refuse : status=c000007b reason=import-thunk pc=0x6f0000003b90
```

**It relocated.** Different pc, different reason, same death at ~37 s.

**Therefore the defect is general to chaining, not specific to that block pair.** A workaround
that declines problem edges is closed off: the failure simply moves to the next chained edge.
What is needed is correct register/context preservation across a chained edge.

## What the fix has to explain

- Garbage is identical run to run (`f0e0993f` / `320eec31`), so this is deterministic, not a race.
- Damage accumulates over consecutive chained edges rather than appearing at one.
- The visible symptom is 64-bit values arriving with their high half missing, plus `rbp`
  collapsing to a small integer.
- `blocks=2..6` per transition: several blocks retire natively before control returns, which is
  the point of chaining — and also the window in which the context drifts.

## Status

Gate stays default OFF; nothing regresses. Chaining went from "dies instantly, cause unknown"
to a defect that reproduces four runs running, with identical values, localised to named
registers and a named edge, and proven systemic rather than local.

Not claimed: any wall-clock number for chaining. No chained run has reached a milestone yet.
