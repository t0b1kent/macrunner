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

## 2026-08-01 — THE INSTRUMENT COST 17 POINTS, AND A PER-DISPATCH SCAN WAS THE REAL TOP ITEM

### 1. Correction: `MACRUNNER_HB_TRACE_DISPATCH_STATS=1` inflates the critical thread by ~17 points

Same config, trace ON vs OFF, critical thread (`Thread_175198873` / `Thread_175227269`, both samples
of each arm agreeing):

| self time | ON s1 | ON s2 | OFF s1 | OFF s2 |
|---|---|---|---|---|
| `run_jit_block_with_signal_guard` | 14.4 % | 13.3 % | **2.1 %** | **5.2 %** |
| `_tlv_get_addr` | 8.9 % | 7.8 % | 4.5 % | 5.1 % |
| `dispatch_stats*` | 2.4 % | 3.7 % | 0 % | 0 % |
| `hb_contract_telemetry_record_dispatch` | 1.7 % | 1.0 % | 0 % | 0 % |

**The entry above that called the guard "the largest attackable item at 14.4 %" was measuring the
instrument.** `dispatch_stats_note_terminal()` is called INSIDE the guard and inlines into it, so
~10 of those points were the trace. With the trace off the guard is 2-5 %. The *direction* of the
previous entry survives — the snapshot is a small lever, retired — but its headline number was
wrong, and the ~4-point snapshot pricing was measured inside an inflated regime, so the true
snapshot prize is smaller still.

Standing consequence: **any timing arm carrying `MACRUNNER_HB_TRACE_DISPATCH_STATS=1` is pessimistic
by roughly 17 % of the critical thread.** The census added last entry is unaffected — it is one
fprintf per 2^20 dispatches — but it does keep its own `__thread` counters, which is part of the
residual `_tlv_get_addr`.

### 2. The real top item: a per-dispatch linear scan for the block's terminal

With the instrument off, `hb_jit_runtime_run` self was 24.2 %/18.6 % — and taking the instruction
OFFSETS rather than the symbol (the lesson from the import-scan find) collapsed nearly all of it
onto two PCs, `+5392` and `+2272`, in both samples. Disassembly of `+5392` shows a 184-byte-stride
loop testing each op against a bitmask — `first_control_transfer_instr()`, i.e. `hb_ir_instr_t` is
184 bytes and every dispatch re-walked the block's instruction array to answer "where is the
terminal". `hb_jit_runtime_run:6866` runs it on EVERY dispatched block.

The answer is a pure function of `instrs`, which never changes after translation. Memoised it on
`hb_ir_block_t.first_transfer_idx` (`-2` uncomputed / `-1` none / `>=0` index), invalidated in
`hb_ir_emit` (the only mutation path) and carried across `block_clone_for_cache` (which fills
`instrs` by memcpy, bypassing `hb_ir_emit` — the one place that could have gone stale silently).
The racing write is benign by construction: two threads derive the SAME value from the same
immutable input.

**Verified within-run, by the offsets themselves:**

| self time | BASE s1 | BASE s2 | MEMO s1 | MEMO s2 |
|---|---|---|---|---|
| `hb_jit_runtime_run` | 24.2 % | 18.6 % | **12.7 %** | **10.9 %** |
| hot leaf offsets | `+5392,2272` (984) | `+5392,2272` (807) | `+4568,4632` (487) | `+4568,2292` (476) |

The previously dominant PCs are gone and the symbol's self time roughly halved — ~8-10 points off
the critical thread.

**What this does NOT show.** The memo run did **not** reach `Restored language` inside its 380 s
budget (the trace-off baseline did, at 303.7 s) and logged fewer dispatches in similar wall time
(629 M vs 757 M). Phase markers up to that point were all slightly EARLIER (`Initialize engine
version` 42.6 s vs 44.2 s, `Begin MonoManager` 44.1 vs 45.7, `UnloadTime` 139.6 vs 146.5), no
`HyperBridge run failed`, no new error class, `recover=0`. So: the change is confirmed to do what it
was designed to do, and is **not** confirmed as a net speed win. One 380 s run sits well inside the
documented 210-628 s spread (SPEEDPROF1 missed the marker too), and rule one says cross-run
wall-clock at n=1 decides nothing. The next arm should be a longer budget so the marker is reached
in both.

Evidence: `SPEEDPROF3_NOTRACE-s{1,2}.txt`, `SPEEDPROF4_MEMO-s{1,2}.txt`; runs
`laneA-SPEEDPROF3NOTRACE-*`, `laneA-SPEEDPROF4MEMO-*`.
