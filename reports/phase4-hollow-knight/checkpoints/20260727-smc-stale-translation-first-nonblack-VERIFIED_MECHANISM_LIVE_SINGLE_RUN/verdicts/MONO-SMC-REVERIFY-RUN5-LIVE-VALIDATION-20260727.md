# SMC reverify — run5 live validation: mechanism PROVEN, detector flipped, stall residue identified (2026-07-27)

Question: does the byte-hash reverify (MONO-SMC-REVERIFY-IMPLEMENTATION-20260727.md)
confirm the stale-translation mechanism live, and does repairing it advance the
managed bootstrap?

Run: `reports/phase4-hollow-knight/laneA-smc-reverify-5-try1-053505` (VALID_RUN,
exit=124 at TMO 3000s). Build: ntdll.so 05:34, strings-gate verified in dist,
run-contract READY (oracle template prefix + save snapshot + 3 zero-branches).
Classification: `VERIFIED_MECHANISM_LIVE_PRODUCT_PROGRESS_STALL_RESIDUE_OPEN`.

## Instrument liveness (adversarial gate)

The first two attempts (run3/run4) silently measured nothing: the reverify hook
sat in `hb_jit_runtime_run_legacy` while the live game dispatches through
`hb_jit_runtime_run` (MACRUNNER_HB_SINGLE_LOOKUP=1). Same failure class as the
invalidated prior attempts (`UNKNOWN_MONO_HASH_CYCLE_VALIDATION_FAILED_WRONG_HELPER_STATE`).
Found via the positive-liveness aggregate (`progress tracked=.. reverified=..`)
that never fired. Fixed the hook site (and a real use-after-free: eviction
destroys `cached->block` which the dispatch fastpath had already borrowed),
re-proved the control on BOTH dispatch paths plus disabled mode:

- legacy path: `PASS tracked=2 reverified=3 evicted=1`
- single_lookup (live) path: `PASS tracked=2 reverified=3 evicted=1`
- `MACRUNNER_HB_SMC_REVERIFY=0`: `PASS tracked=0 reverified=0 evicted=0`
- hb_test_runner: 455 passed / 28 failed (baseline 445/38; no regression).

## What the live run proved

1. **Guest code IS rewritten behind cached translations — the mechanism is real.**
   `evicted=3` by +55s, `2082` by +451s, `2972` by +662s, final `3295`
   (tracked=60681 blocks, reverified=83.9M, unreadable=0). First-16 eviction
   detail lines show 32–35-byte spans at adjacent addresses
   (`0x11d2c0db0`, `0x11d2c0dd3`, `0x11d2c0e20`, `0x11d2c2190`, …) — the size
   and clustering of Mono trampoline stubs being re-pointed as methods compile.
   Each eviction = a translation that would otherwise have executed stale bytes.

2. **The regression detector flipped: first non-black presented surface ever.**
   Ordinal-200 readback at +1017s:
   `hash=0xf7ea9ca044b2fdb5` (expected-black `0xc770038f717d0383`),
   `black=774464 nonblack=11968 colorful=0 min=0,0,0,0 max=255,255,255,255
   mean=1.514,1.514,1.514,5.652`. Both prior A/A runs: `nonblack=0`
   byte-identical. The content is grayscale (R=G=B) up to full white, ~1.5% of
   the frame — consistent with a white-on-black logo/text screen [HYPOTHESIS:
   Team Cherry logo; no screenshot captured this run].

3. **The remaining stall is NOT stale translation.**
   Evictions stopped completely after ~+1017s (flat 3295 through +2628s) while
   the instrument stayed live (reverified +16.8M in the same window) and the
   game kept burning ~120-140% CPU without log output. `Performing automatic
   level start.` never appeared in 3000s (prior runs reached it, then the
   frustum-spam phase; run5 has zero frustum lines — genuinely pre-level-start).
   If the post-logo stall were stale-code execution, the live instrument would
   have caught further rewrites. It caught none. The spin that remains is
   genuine guest execution — consistent with the brief's warning that "the spin
   is the cause" was an unproven coincidence hypothesis.

## Timeline (run5)

```
+46s   Mono path[0]
+48s   first tracked (15), first reverify — instrument live
+55s   tracked=288 reverified=1000 evicted=3
+154s  swapchain CreateSwapChainForHwnd
+451s  tracked=48154 reverified=16.7M evicted=2082
+662s  tracked=58053 reverified=33.5M evicted=2972
+1017s ordinal-200 readback NON-BLACK (nonblack=11968)
+1315s evicted=3295 (last eviction ≈ here)
+2628s reverified=83.9M evicted=3295 (flat)
+3054s exit=124 TMO
```

## Open residue (next questions, in order)

1. Does the bootstrap reach `Performing automatic level start.` given more wall
   time? (run6, 7200s, identical build — also serves as A/A for the non-black
   readback.) Oracle does the whole thing in 33.9s; ours was still pre-level-start
   at 3000s — a >88x gap that reverify overhead (~2x on dispatch) explains only
   in part.
2. If it stalls again with zero evictions: re-capture the spin stack on the
   FIXED build. If it is still `jit_code_hash` lookup, the memory-ordering
   candidate (hazard-pointer retry never observing the publish; prior-art brief
   item 3) becomes the lead, testable via `MACRUNNER_HB_JIT_DIRECT_MEM=0` /
   barrier knobs.
3. Identify the non-black content (screenshot the window this time) — confirm
   it is the Team Cherry logo and not noise.

## What this does NOT claim

- No claim that the managed bootstrap advances past level start (it did not,
  in 3000s).
- No claim the 3295 evictions are each individually necessary (some may be
  benign data embedded in code spans); the class is proven by the byte-hash
  mismatch itself, not by per-block adjudication.
- The non-black readback is single-run until run6 confirms A/A.
