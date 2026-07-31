# LANE SPEED / SNAPSHOT — progress

One line per step. Numbers or it did not happen.

## 2026-08-01 — STEP 1 SETTLED: the fault-recovery branch is DEAD, and the instrument proves itself

**Question (MEGA-BRIEF step 1):** every dispatch pays a 790-byte frame memset, a 760-byte
`hb_ctx_snapshot_save` and a `sigsetjmp` so that a fault can roll the guest back. How often is that
rollback actually used? The prior belief ("20 480 faults and `ripmap_check` printed nothing") was
NOT evidence: `ripmap_check` sits behind `MACRUNNER_HB_RIPMAP` and prints once per 1024 calls, so
its silence is equally consistent with the gate being off.

**Instrument:** `macrunner-hb-guard-census`, deliberately UNGATED — no env var, no dependency on
`trace_dispatch_stats_enabled()`. Per-thread counters folded into a global only at flush, printed
every 2^20 dispatches (these runs die on the timeout's SIGKILL and never reach `atexit`).
`run_jit_block_with_signal_guard`: `t_guard_dispatch` before `sigsetjmp`, `t_guard_recover` as the
first statement of the branch `siglongjmp` lands in.

**Result — two full boots to `Restored language`, both GOOD BOOT:**

| run | dispatches | recoveries | faults in run |
|---|---|---|---|
| GUARDCENSUS1 (430 s, marker +368.0 s) | 657 457 152 | **0** | 851 968 |
| GUARDCENSUS2 (430 s, marker +319.4 s) | 741 343 232 | **0** | 3 612 672 |

**The zero is load-bearing only because a counter beside it moved.** Rule two says a zero from an
instrument nobody proved could fire is not a fact, so GUARDCENSUS2 added counters to the fault-claim
entry point — the mechanism one fault away from the recovery branch:

```
claim_calls=7 193 080  claim_taken=0  claim_declined_frame=7 193 080  claim_declined_range=0
```

The signal handler consults the guard **7.19 million times** and hands it control **zero** times.
The instrument is demonstrably live; the branch is simply never taken. Every one of those 7.19 M
declines happens at the FIRST check (`!frame || !frame->rt || !frame->entry || !native_code ...`),
none at the pc-range check — so at fault time there is no active guard frame at all, i.e. these
faults do not occur inside a guarded native dispatch. `sigbus-invalidate`, `sigill-own` and the
quarantine path likewise logged 0 lines in both runs.

**Conclusion:** the per-dispatch snapshot preamble is paid ~741 M times per boot and used 0 times.
Step 2 (lazy / on-demand snapshot, FEX-style) is authorised by measurement rather than by analogy.

**Not claimed:** the marker times (368.0 s vs 319.4 s) are NOT offered as a speed result — n=1 each
against a 210…628 s spread, and the two runs differ 4.2x in fault count (851 968 vs 3 612 672),
which is itself a reminder of why cross-run wall-clock is the last resort. The magnitude of the win
is still to be measured, per rule one, from within a single run.

**Also in this commit, NOT measured, default OFF:** the FEX-style host-offset map
(`host_off[]` in `hb_codegen_buffer_t`, `ripmap_*` in `hb_runtime.c`, gate
`MACRUNNER_HB_RIPMAP`) and `MACRUNNER_HB_CHAIN_SCOPED_ROLLBACK`. Scaffolding for step 2; neither has
been run, and `avg_chain` is 1.0000 (chaining still off), so scoped rollback cannot have an effect
yet.

Evidence: `reports/phase4-hollow-knight/laneA-GUARDCENSUS{1,2}-a1-try1-*`.
