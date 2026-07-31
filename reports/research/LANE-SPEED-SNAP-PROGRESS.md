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

## 2026-08-01 — STEP 2 PRICED AND RETIRED: eliding the snapshot is worth ~3-4 points, not "в разы"

Step 1 said the snapshot is never used. That makes it removable, not necessarily WORTH removing —
so the cost was measured before the machinery to remove it correctly was built.

**Fresh profile of the critical thread** (`hk-selfinit-sample-pair.sh`, +240 s and +330 s, 6 s at
1 ms), which also corrected the standing picture: the guard is far more expensive than the journal
had it (7.8 %). `hb_ctx_snapshot_save` is `static inline`, so most of the 760-byte copy is charged
to the guard's OWN self time and never appears as `_platform_memmove` — which is why the old
"memmove 3.2 %" reading understated it.

**Then the measurement arm** `MACRUNNER_HB_SNAPSHOT_MEASURE_SKIP=1` (skips the save; unsafe as a
default, valid as a stopwatch only because the ungated census reports `total_recover=0`, which both
arms did — 757 071 872 dispatches, 0 recoveries, marker reached):

| self time, critical thread | base s1 | base s2 | skip s1 | skip s2 |
|---|---|---|---|---|
| `run_jit_block_with_signal_guard` | 14.4 % | 13.3 % | 11.7 % | 12.0 % |
| `_platform_memmove` | 2.8 % | 3.5 % | 2.0 % | 1.4 % |
| `_platform_memset` | 2.8 % | 1.9 % | 1.9 % | 1.7 % |
| `hb_jit_runtime_run` | 16.9 % | 15.3 % | 11.5 % | 8.2 % |
| `_tlv_get_addr` | 8.9 % | 7.8 % | 8.6 % | 10.7 % |

**Verdict: ~2 points off the guard, ~1.5 off memmove — call it 3-4 points of critical-thread time.**
Direction is consistent in both sample pairs, so the effect is real. The magnitude is not: the lane
is chasing a multi-fold cut and this is a few percent. **The MEGA-BRIEF's ГЛАВНАЯ ЗАЦЕПКА is
therefore priced and retired as a major lever** — building the FEX-style resume map to make the
elision CORRECT would buy those same ~4 %, and that is now a known-small prize rather than an
assumed-large one.

**Noise, stated plainly:** `hb_jit_runtime_run` moved 16.9/15.3 -> 11.5/8.2, which skipping a
memcpy cannot cause. Cross-run share comparison carries real noise at this sample count, and the
2-point guard delta sits near the edge of what four samples resolve. It is reported as a bound on
the prize, not as a precise figure.

**Instrument caveat that must be cleared before the next lever:** all four samples ran with
`MACRUNNER_HB_TRACE_DISPATCH_STATS=1`, and that path touches several `__thread` counters per
dispatch. `_tlv_get_addr` at 8-11 % is therefore partly the instrument measuring itself — on Darwin
arm64 `tls_model` is silently ignored, so every `__thread` read is a real call. Next step is a
profile with the trace gate OFF, which both prices the instrument and gives the honest TLS baseline.

**Where the remaining time actually is** (same profile): `hb_jit_runtime_run` self 15-17 %,
`_tlv_get_addr` 8-11 %, `hb_memory_read` 6-7 %, `find_region_impl` ~3 %, `hb_flags_read_operand_value`
~3 %. None of these is the block-dispatch preamble.

**Sample-parsing trap, recorded so it is not repeated:** selecting the critical thread as "has
`hb_jit_runtime_run` and does NOT contain `probe_image_bytes`" picks a thread that is 79.5 % blocked
in `__ulock_wait2` and, on the second sample, picks nothing at all. Both real JIT threads (3706 and
2631 jit samples) CONTAIN `probe_image_bytes` somewhere in the tree. The livelock rule is about a
thread DOMINATED by that symbol (self share > ~50 %), not one that merely touches it.

Evidence: `MASTER-I12-SELFINIT-SAMPLES/SPEEDPROF1-s{1,2}.txt`,
`SPEEDPROF2_SKIP-s{1,2}.txt`; runs `laneA-SPEEDPROF1-*`, `laneA-SPEEDPROF2SKIP-*`.
