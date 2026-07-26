# SMC stale translation proven live — first non-black frame (2026-07-27)

**Class:** `VERIFIED_MECHANISM_LIVE_SINGLE_RUN`. The mechanism is proven live and the
regression detector flipped once. It is **not** A/A confirmed, the managed bootstrap still
does not reach level start, and nothing is shipped.

## The finding

A guest instruction-cache flush invalidated **no** translation anywhere in our stack:
`signal_arm64ec.c:771` correctly calls `pBTCpu64FlushInstructionCache(addr,size)`,
`xtajit64/cpu.c:198` discards the range and passes NULL, and
`xtajit64/unixlib.c:710` is `return STATUS_SUCCESS;`. `hb_cache_invalidate*`
(`hb_aot_cache.c:452,473`) had no production caller at all.

Found by reading Mihocka's ARM64 Boot Camp — he is one of the Microsoft engineers who built
xtajit/xtajit64/xtabase, and he measures that callback costing 50x (emulated x64) / 300x
(native ARM64) in a hot loop, i.e. a correct translator does substantial work there.

## What the live run proved (run5)

- Guest code **is** rewritten behind cached translations: 3295 evictions over 60681 tracked
  blocks and 83.9M reverifications, `unreadable=0`. Eviction spans are 32-35 bytes at
  adjacent addresses — the size and clustering of Mono trampoline stubs re-pointed as
  methods compile.
- **Regression detector flipped**: ordinal-200 readback `hash=0xf7ea9ca044b2fdb5`
  (expected-black `0xc770038f717d0383`), `black=774464 nonblack=11968 colorful=0`,
  greyscale R=G=B to full white, ~1.5% of frame. Both prior A/A runs: `nonblack=0`,
  byte-identical.
- Instrument liveness was proven adversarially. Runs 3 and 4 measured nothing because the
  hook sat in `hb_jit_runtime_run_legacy` while the live path is SINGLE_LOOKUP
  `hb_jit_runtime_run` — the same `WRONG_HELPER_STATE` class that invalidated earlier
  attempts. After the fix the control passes on both dispatch paths and in disabled mode,
  and a real use-after-free (eviction destroying a block the dispatch fastpath had borrowed)
  was fixed on the way.

## What this does NOT claim

- The non-black readback is **single-run**; run6 is the A/A.
- run5 never reached `Performing automatic level start.` in 3000s where prior runs did —
  reverify costs ~2x on dispatch and the gap to the oracle's 33.9s is still >88x.
- **The remaining stall is NOT stale translation.** Evictions went flat at +1017s while the
  instrument stayed live (+16.8M reverifications) and the game burned 120-140% CPU. The lane
  refuted its own hypothesis with its own instrument.
- The non-black content is unidentified: no raw pixels were persisted, only the aggregate.

## Contents

- `verdicts/` — the run5 validation, the SMC implementation note, the icache-flush stub
  audit, the ARM64 Boot Camp extraction, the prior-art brief, the black-frame root-cause
  verdict, the loop-trace instrument design.
- `method/` — **`engine-smc-reverify.patch` (805 lines): the engine change exists ONLY here,
  it is uncommitted in the working tree.** Plus the lane prompt and the loop scripts.
- `run5-evidence/` — run log, triage summaries, run contract, flight journal.
  `final-child.json` deliberately excluded: it holds credentials.
- `journals/` — the hk-mono lane journal.
- `138-MILESTONE-*.md` — the Obsidian vault milestone note, copied here so the narrative
  survives with the evidence.

## Regression detector

Ordinal-200 presented-surface readback. Expected-black hash `0xc770038f717d0383`.
**A working fix turns it non-black.** Acceptance checklist is the oracle's post-level marker
sequence: `Loaded saved language code 'EN'` -> `Making UI menu lean.` -> `Opening_Sequence`
-> first visible pixel.

## Integrity

`SHA256SUMS` covers every file except itself. A credential-pattern sweep found nothing and
no `final-child*` file is present.
