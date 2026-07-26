# SMC translation reverify — implementation + control proof (2026-07-27)

Question: can the stale-translation mechanism (see
`MONO-SMC-STALE-TRANSLATION-MECHANISM-20260727.md`) be detected and fixed at the
translation-cache layer with a tool that proves itself before any live run?

Answer: **yes — implemented and control-proven.**
Classification: `INSTRUMENT_AND_FIX_CONTROL_PROVEN_NOT_YET_LIVE_VALIDATED`.

## What was built

Root fix at the translator layer (Zeroth Principle: fix where the bug lives —
the cache keyed by guest address with no byte re-verification — not at the
symptom):

- `engine/hyperbridge/include/hb_runtime.h` — `hb_block_cache_entry_t` gains
  `smc_span_start` / `smc_hash` / `smc_span_len`; new public diagnostic getter
  `hb_jit_smc_reverify_stats()`.
- `engine/hyperbridge/src/hb_runtime.c`
  - `smc_track_entry()` called from both `block_cache_put` return paths (and the
    two persistent-load/fresh-compile sites now pass `rt` so loaded blobs are
    tracked too). Only spans in **writable+executable** regions are tracked —
    the only spans that can change under us (Mono/JIT code heaps are RWX).
    Static RX code stays untracked and pays nothing.
  - `smc_reverify_entry()` on the single dispatch-loop cache-find choke point:
    recompute FNV-1a over the entry's guest span; mismatch → count, bounded
    trace, evict (`block_cache_evict_entry` — unchain/release/memset, count--),
    fall through to retranslation from current bytes. Unreadable span → keep
    entry, never evict blind.
  - Default ON, kill switch `MACRUNNER_HB_SMC_REVERIFY=0`; bounded diagnostics
    `MACRUNNER_HB_TRACE_SMC_REVERIFY=1` (first 16 evictions + every 65536th +
    destroy-time summary).

## Control proof (prove-the-tool gate, run FIRST)

`tools/hb_smc_reverify_control.c`, binary and logs in
`reports/phase4-hollow-knight/smc-reverify-control-20260727/`.
Known-in-advance results, all asserted on counters not stderr text:

- enabled mode: `tracked=2` (RWX block at translate + retranslate; the RX block
  stays untracked), `reverified=3`, **evicted exactly 1** — on the run after one
  guest byte was overwritten behind the cached translation; zero evictions on
  unchanged-byte runs before and after. Exit 0.
- disabled mode (`MACRUNNER_HB_SMC_REVERIFY=0`): `tracked=0 reverified=0
  evicted=0`. Exit 0.
- Trace line captured: `macrunner-hb-smc-reverify: evict guest=0x1025c9000
  span=7 old=78ac… new=b364… evicted=1 reverified=2`.
- Control bug found and fixed during bring-up: 64-byte static buffers shared a
  page, so `find_region` returned the RWX region for the RX block
  (page_align overlap). Buffers now page-aligned/page-sized. This was a control
  artifact; real guest code pages are page-granular.

## Regression check

- `tests/hb_test_runner`: **453 passed / 30 failed** — baseline before this
  change was 445/38 (the +8/−8 delta comes from the helper-loop rate-limit fix
  in `hb_arm64_codegen.c` landing in the same rebuild; no new failures).
- Helper-loop control re-run against the new objects: **PASS**
  (`reentries=64 exit=0x3e13`).

## What this does and does not claim

- PROVEN (control): cached translations of RWX guest code are now evicted and
  retranslated when the guest bytes change; static code is untouched; the
  kill switch works; output is bounded.
- NOT YET SHOWN (needs the live run): that Mono actually rewrites bytes behind
  cached translations during the HK bootstrap window, and that this fix
  advances the managed sequence past `Performing automatic level start.`
  If the live run shows `evicted=0` while the stall persists, the
  stale-translation hypothesis is falsified for this blocker and the lane
  moves to the memory-ordering candidate (`mono_jit_info_table_find_internal`
  hazard pointers, prior-art brief item 3).
- Persistent-cache note: disk entries are content-keyed, so an evicted block
  retranslated from new bytes stores/loads under the new hash — self-consistent.

## Next step

Rebuild `ntdll.so` (force relink, verify SHA in `system32`), then one bounded
live HK run with `MACRUNNER_HB_TRACE_SMC_REVERIFY=1`, gated on log growth and
the oracle post-level marker sequence + ordinal-200 readback.
