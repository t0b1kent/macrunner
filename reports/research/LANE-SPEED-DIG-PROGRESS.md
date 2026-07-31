# LANE SPEED-DIG — progress

Goal: time to `Restored language` in a different league from today's 593 ± 78 s (Rosetta: < 45 s).
Rule #1: a quantity from INSIDE one run beats any comparison of runs.

## 2026-07-30 — iteration 1

- step: disk was 28 GB free, below the 30 GB floor, so no run could be launched honestly. Freed to
  31 GB by pruning ONLY regenerable per-run artifacts older than 1 day under `reports/`: 94
  `translation-cache*.bin` preimages (2704 MB, no script references any of them, none newer than a
  day), 1 stale `run.log` > 200 MB, 14 per-run `prefix-template` COPIES. All 6 canonical
  `artifacts/hk-*prefix-template*` verified intact. The earlier "295 GB prunable" reading was a
  `du -g` rounding artifact — 303 dirs each rounded up to 1 GB; the parent is 20 GB total.
- step: verified statically that the trampoline's hardcoded guest-PC store offset is right:
  `offsetof(hb_context_t, pc) = 544`, `sizeof(hb_context_t) = 2616`. A wrong offset would have
  silently corrupted the context on every bail-out. It is correct — no landmine.
- step: **[FINDING, static] `avg_chain` as committed in `eb89b54d` cannot answer the question it
  was built for.** Its denominator counts calls to `hb_jit_runtime_run`, but the dispatcher is the
  `while (1)` loop INSIDE that function — one call retires many blocks. So the ratio is
  blocks-per-lifted-function-entry, which is already >> 1 with chaining off, and the documented
  reading "1.00 = the dispatcher is not being bypassed" is simply not what the counter computes.
  Refutes my own instrument before it cost a run. Fixing it: the denominator becomes the number of
  native dispatches (`exec(ctx)` calls), each of which is exactly one full round trip.
- step: **[FINDING, static] enabling `MACRUNNER_HB_BLOCK_CHAIN=1` today would corrupt guest control
  flow.** `block_terminal_is_chainable()` accepts any `HB_IR_JMP`, but the codegen proves
  (`hb_arm64_codegen.c:4625`) that `HB_IR_JMP` with `src1.type == HB_OP_NONE` is a DIRECT jump
  (`emit_set_pc_imm64(instr->target)`) while `src1.type != HB_OP_NONE` is an INDIRECT jump whose
  target is computed into x20 at run time. `patch_block_tail` would nail an indirect jump's tail to
  whichever block followed it on its first execution, so every later execution with a different
  computed target runs the wrong guest code. Since indirect jumps are how vtable / switch / import
  dispatch works, this would fire constantly. This is a second hazard, independent of the eviction
  hazard the trampoline in `f17c5202` was written to remove.
- step: **[PREDICTION, registered before the run] `avg_chain` will come back below 1.10.** Chaining
  is gated to direct-`HB_IR_JMP` terminals only, and in compiled x86 the executed terminals are
  dominated by `Jcc` (loop backedges) and `CALL`/`RET`. If that holds, chaining is not the lever the
  brief expects it to be, and the dispatcher tax has to be attacked head-on instead. Measuring it
  with a dynamic terminal-op histogram in the same run, so the ceiling is a number and not an
  argument.

### Run SPEEDDIG1 (`laneA-SPEEDDIG1-a1-try1-170024`) — one run, every instrument

Build verified by SHA **and** content before launching: `ntdll.so` 0ab0b84d → f1756944 → c10f9ca3,
with `chainable_pct` / `xfer_mid` / `macrunner-hb-findblock` all present in the shipped
`aarch64-unix/ntdll.so`. hyperbridge built before wine, both clean under `-Werror`.

- step: **PREDICTION CONFIRMED, and the brief's main lead is capped at 7.38 %.** Critical thread at
  wall_s 85.6: `thread_dispatches=205800000 thread_blocks=205800000 avg_chain=1.0000`. The terminal
  histogram over 205.8 M dispatches:
  `jcc=146290573 (71.1%)  ret=18813295 (9.1%)  call_dir=16427682 (8.0%)  jmp_dir=15186822 (7.4%)
   call_ind=5696007 (2.8%)  jmp_ind=3350503 (1.6%)  loop=0  xfer_mid=0  other=35118`.
  `jmp_dir` is the ONLY population `block_terminal_is_chainable` admits, so **turning on
  `MACRUNNER_HB_BLOCK_CHAIN` can remove at most 7.38 % of dispatcher round trips** — and that is an
  upper bound, before any successor turns out unresolvable or uncached. `avg_chain=1.0000` with the
  gates off is the negative control that validates the instrument: exactly 1, not approximately.
- step: **REFUTED — my own O(N) `find_block` hypothesis, by the number it predicted.** I found that
  `find_block` is a linear scan and that the legacy dispatch loop opens with it unconditionally, and
  expected a scan in the hundreds. Measured: `avg_scan=1.00 max_block_count=1`. The CFG holds exactly
  ONE block, so the "linear scan" is a single compare. Refuted a second time over: `laneA-run-hk.sh:288`
  sets `MACRUNNER_HB_SINGLE_LOOKUP=1` by default, so HK runs have never been on the legacy path at all
  — `find_block` is reached on only 2.57 M of 205.8 M dispatches (1.2 %), which is the block-cache miss
  rate, not a scan cost. The brief's "0 / 218 runs" applies to `BLOCK_CHAIN` and `INDIRECT_IC`; the
  third gate was already armed.
- step: **the old `avg_chain` would have read ≈ 80 where the truth is 1.0000** — the static finding is
  now numeric. `find_block` is called at least once per `hb_jit_runtime_run` entry, so entries ≤ 2.57 M
  while blocks = 205.8 M, putting the retired counter at ≥ 80. It would have reported chaining as
  working, on a build where `avg_chain` is exactly 1.
- step: **[MEASURED] where the time actually goes.** Critical thread = the one thread of 53 with any
  `hb_jit_runtime_run` frames (536 samples, `sample <pid> 6 10`); it is productive, not pinned
  (44.4 % inclusive in the dispatcher). Self time, of 536:
  `macrunner_hb_run_x64 84 (15.7%)`, `hb_jit_runtime_run 65 (12.1%)`,
  `_platform_memmove 53 (9.9%)` = the 1592-byte per-dispatch context snapshot,
  `run_jit_block_with_signal_guard 52 (9.7%)`,
  guest MMU `hb_memory_read 28 + find_region_normalized 17 + hb_memory_write 12 = 57 (10.6%)`,
  `_tlv_get_addr 26 (4.9%)`, `hb_flags_read_operand_value 13 (2.4%)`.
  **The translated guest code itself is `??? ` 13 samples = 2.4 %.** Round-trip machinery totals
  ≈ 54 %. With `steps/dispatch = 954916692 / 205800000 = 4.64`, the engine leaves native code and
  re-enters C every 4.64 guest instructions — that, not compilation and not the cache, is the 13x.
- step: **[my own instrument's cost, do not let it contaminate a timing arm] `dispatch_stats_add` →
  `clock_gettime` is 21 samples = 3.9 % of the critical thread.** `dispatch_stats_register()` is
  called from `dispatch_stats_add` on EVERY dispatch and calls `runtime_now_ns()` unconditionally
  before its once-only CAS, so enabling `MACRUNNER_HB_TRACE_DISPATCH_STATS=1` buys a syscall per
  dispatch. Any time-to-language number from an instrumented run is therefore ~4 % pessimistic
  against the 593 s baseline, and must not be quoted as a regression. Fixing the register path so the
  instrument stops charging for itself.
- step: **★ THE BIGGEST SINGLE ITEM FOUND, and it is a provably dead linear scan.** The profile's top
  self-time symbol was `macrunner_hb_run_x64`, so I took its instruction offsets instead of guessing:
  the samples sit on ONE instruction, `+15540`, with **84 of 536** in the first profile and **99 of 501**
  in a second one three minutes later — stable, not noise. Disassembling `ntdll.so` at
  `_macrunner_hb_run_x64+15540` (0x34b48) gives
  `ldr x11,[x24] / cmp x11,x8 / b.eq hit / add x24,x24,#0xf0 / subs x10,#1 / b.ne` — a linear scan with
  a **240-byte stride**, which is exactly `sizeof(struct macrunner_hb_import_thunk)`. The scanned globals
  resolve by symbol to `macrunner_hb_import_count` (0x1b86b4) and **`macrunner_hb_imports`** (0x1b86b8),
  a 4096 × 240 B ≈ 1 MB table — one fresh cache line per iteration. It is
  `macrunner_hb_find_import_thunk`'s fallback scan, inlined, and its hot caller is the dispatch loop's
  `macrunner_hb_find_import_thunk( ctx->pc )` — reached on **every** guest block dispatch, 205.8 M times
  by wall_s 85.6. `ctx->pc` is a real code address, never a synthetic import address, so the O(1) index
  check missed every single time and the scan ran to completion **only to return NULL**.
- step: **the scan is unreachable, proven from the invariant, not assumed.** All three append sites
  (`macrunner_hb.c:5767`, `:7299`, `:7346`) set
  `slot->guest_target = MACRUNNER_HB_IMPORT_BASE + macrunner_hb_import_count * STRIDE` with the count
  already incremented, so entry `i` always holds `BASE + (i+1)*STRIDE`. `macrunner_hb_import_count` is
  mutated nowhere else but the single `--` that pops a just-zeroed last slot on a failed code-thunk
  emit, so the table is append-only and never compacted from the middle, and there is no assignment or
  `memcpy` into `macrunner_hb_imports[]` anywhere in the file. The only other `guest_target =` on a
  thunk (`:35021`) writes a stack local that never enters the table. Therefore at most one index can
  match a given target, it is computable in closed form, and that is the index already tested.
  Retired the scan; kept it behind `MACRUNNER_HB_VERIFY_IMPORT_INDEX=1`, which re-runs it whenever the
  index says NULL and prints `macrunner-hb-import-index-VIOLATION` if it ever disagrees — the proof is
  mine, so it is left falsifiable instead of trusted.
- step: **[PREDICTION, registered before the next run] the fix is verifiable INSIDE one run, no arm
  needed.** `macrunner_hb_run_x64+15540` must fall from 84–99 samples to ~0 in the next profile, and no
  `import-index-VIOLATION` line may appear in a run with the verify flag on. That is a within-run
  before/after on a specific instruction, so the 593 ± 78 s spread never enters it. Expected size:
  16–20 % of the critical thread's CPU, which is the largest single item measured but on its own is not
  the multi-fold win — that still needs the 4.64-instructions-per-dispatch problem, i.e. `Jcc`.
- step: **[ASSESSMENT] where the multi-fold win has to come from.** With `jcc = 71.1 %` of terminals and
  `jmp_dir = 7.4 %`, the brief's chaining lead is capped at 7.4 %, and `avg_chain = 1.0000` today. The
  structural fix is a **PC-guarded chain**: reserve a larger slot than the current 4 NOPs, and have the
  predecessor compare the PC the block just stored against the recorded successor, branching to its
  trampoline on a match and falling through to the dispatcher otherwise. That covers `Jcc`, and because
  it validates the actual computed PC it also makes chaining indirect jumps and calls SAFE — dissolving
  the correctness hazard recorded above rather than merely avoiding it. Next iteration's main task.

### Run SPEEDDIG2 (`laneA-SPEEDDIG2-a1-try1-171859`) — the import fix, verified

Build `ntdll.so` 270e13ec4e05c7b4, verified by SHA and by `strings`. Contains the import-index fix and
the `dispatch_stats_register` fix and **nothing else** — the PC-guarded chain went into
`libhyperbridge.a` only afterwards and is not in this binary, so this run measures the import fix alone.

- step: **★ EVERY PRE-REGISTERED CHECK PASSED.** Same 536-sample budget on the critical thread:

  | check | before (SPEEDDIG1) | after (SPEEDDIG2) | |
  |---|---|---|---|
  | `macrunner_hb_run_x64` self | **84 = 15.7 %**, all on one offset `+15540` | not in the top list; largest single-offset self = **5** | ✅ |
  | `clock_gettime` (my own instrument) | 21 via `dispatch_stats_add` | **0** | ✅ |
  | `import-index-VIOLATION` lines | — | **0** | ✅ |
  | `avg_chain` | 1.0000 | 1.0000 | ✅ chaining still off, as intended |
  | terminal mix | jcc 71.1 %, chainable 7.38 % | jcc 71.1 %, chainable **7.42 %** | ✅ reproduces |

- step: **★ TIME TO `Restored language`: 828.704 s → 448.399 s (1.85×).** This is the lane's headline
  metric, and 448.4 s also sits **below the brief's 593 ± 78 s baseline** — by 145 s, about 1.9σ — while
  still carrying `MACRUNNER_HB_TRACE_DISPATCH_STATS=1` and a cold translation cache (a new ntdll SHA
  gives a fresh cache root). Caveats stated plainly: n=1 per arm, and SPEEDDIG1's 828.7 s was itself
  inflated by the `clock_gettime`-per-dispatch instrument, so the honest comparison against the project
  baseline is 448.4 s vs 593 ± 78 s. Still short of the goal, which is *в разы* below 593 s — this is
  1.32× against the baseline mean, not multi-fold. The chain is the lever for that.
- step: **`Begin MonoManager` → `UnloadTime`: 284.2 s → 150.5 s** (61.477→345.693 vs 62.185→212.676) —
  **1.89×** on the same marker pair, and below both the 212.6 s the brief records for the
  driver-deferred arm and the 237.7 s healthy-era maximum. Process-wide dispatch throughput went
  936 269 → 2 243 438 dispatches/s (**2.40×**), but that average spans different phase mixes (879 s
  including a long timeout tail vs 234 s of mostly boot), so the marker pair is the headline and the
  rate is corroboration, not an independent second result.
- step: **[HYPOTHESIS] the phase halved while the two removed items measured only ~20–24 % of the
  critical thread.** The arithmetic does not close, so I am not claiming the fix accounts for all of it:
  one run per arm, and this project's Mono phase is documented as bimodal. What is established without
  reference to any other run is the profile — the hotspot holding 84/536 and 99/501 samples is gone.
- step: **the composition shifted, and the next target with it.** New critical-thread self time:
  `_platform_memmove` **81 = 15.1 %** (now top — the 1592-byte per-dispatch context snapshot),
  `hb_jit_runtime_run` 67 = 12.5 %, `run_jit_block_with_signal_guard` 57 = 10.6 %,
  `_tlv_get_addr` 47 = 8.8 %, `find_region_normalized` 38 = 7.1 %, `hb_memory_read` 29 = 5.4 %,
  `hb_flags_read_operand_value` 24 = 4.5 %. Translated guest code (`???`) rose **2.4 % → 4.7 %** — the
  guest's own share of the CPU roughly doubled, which is what removing pure overhead should look like.

### PC-guarded chain — built and encoding-verified, not yet deployed

- step: implemented the PC-guarded chain in `chain_trampoline_build`: the trampoline loads `ctx->pc`,
  compares it against the target's own `guest_addr`, and only then branches to the live entry; a
  mismatch falls into a bare epilogue and lets the dispatcher resolve the PC as it always would. The
  offsets of both literals and both bail-outs are computed in ONE helper (`chain_tramp_layout`) that all
  three users call, because a silent disagreement between them would corrupt control flow.
  `HB_CHAIN_TRAMPOLINE_BYTES` 64 → 128. Clean under `-Werror`.
- step: **encodings verified against clang rather than by hand.** Emitted the trampoline in a standalone
  harness and assembled the *intended* instruction sequence with `clang -c -arch arm64`: all 20 words
  match **byte for byte**, including both literal-relative `LDR` offsets and the layout (`expect_lit`
  +40, `live_lit` +48, `evict_bail` +56). A wrong encoding is the one bug class here that would silently
  corrupt 90 % of block transitions, so it is checked against an independent assembler.
- step: widened `block_terminal_is_chainable` to `JMP` (direct and indirect), `Jcc` and `CALL`, which the
  guard makes safe. `RET` stays out deliberately: its successor is a per-call-site return address, so it
  would mispredict nearly always and pay the guard for nothing. Eligible population **7.4 % → 90.9 %**.
  `MACRUNNER_HB_CHAIN_WIDE=0` restores the old set to A/B the widening alone; the mechanism as a whole
  stays behind default-off `MACRUNNER_HB_BLOCK_CHAIN`.
- step: **[PREDICTION, registered before that run] with `MACRUNNER_HB_BLOCK_CHAIN=1`, `avg_chain` must
  rise above 1.00.** It has been exactly 1.0000 in both runs so far. If it does not move, chaining is not
  engaging and the cause is upstream of the trampoline — a missing chain slot or `patch_block_tail`
  bailing — not something to be waved away as run-to-run variance.

### Chaining does not boot — and the prediction was never testable, because the guest dies first

- step: **`avg_chain` UNMEASURED. Two runs with `MACRUNNER_HB_BLOCK_CHAIN=1` (`SPEEDCHAIN1`, `SPEEDCHAIN2`,
  ntdll 7a3f1a44) both died in early boot** — +23.7 s and +15.9 s, before any dispatch counter reported.
  `flight.jsonl` gives `"status":5` for both, and `final-child.json` confirms the spike-dist wine ran, so
  this is not the stale-binary trap. With the earlier `laneA-CHAINONE-a1-try1-164003` (also `exit=5`) that
  is **3 of 3 runs with chaining armed dying, against 0 of 3 with it off**. `exit=5` is on the documented
  boot-flake list, which is exactly why 3/3-vs-0/3 matters more than any single one of them.
- step: **my first two explanations for the early deaths were both WRONG, and I checked instead of
  believing them.** (a) "A lingering wineserver from the previous run made the harness force-clean my own
  guest": refuted — `laneA-run-hk.sh:198-217` runs that wait/force-clean loop *before* `RUNDIR` is created
  and before `mr-run.sh` launches, so it cannot touch our own guest; and the second run died anyway with
  the slot verified empty (`mr-clean.sh`, `pgrep -fc` = 0). (b) "The ancient `/var/folders` winetemp
  orphans caused it": refuted — `mr-clean.sh` deliberately never kills common-TMPDIR winetemp, and says so.
- step: **[SETUP MISTAKE, mine] `MACRUNNER_HB_TRACE_DISPATCH_STATS_INTERVAL=2000000` made the first chain
  run measure nothing.** A run that dies at +24 s never reaches two million dispatches, so no `chainlen`
  line is ever printed — the very number the run existed to produce. Re-ran at 100000. The lesson is the
  general one: a reporting interval is part of the instrument, and an interval longer than the failure is
  a guaranteed non-measurement.
- step: **★ REPRODUCED OFFLINE IN SECONDS — the 900 s run is no longer the debugging unit.**
  `engine/hyperbridge/tests/hb_test_runner` reproduces a chaining-only failure: 3 runs per arm (the suite
  is ASLR-flaky, so the comparison is between sets that fail in ALL 3), giving
  **OFF: 446/447/447 passed, 36 stable fail lines; ON: 438/439/439 passed, 44 stable fail lines.**
- step: **and the 13 chaining-only failures split into two classes — one benign, one real.** Being honest
  about the benign majority rather than reporting "13 new failures":
  - **8 are `ASSERT(code_buf->size <= N)` code-size budgets** (lines 6891, 6943, 7350, 13695, 13753,
    17080, 17278, 17595). With chaining armed, `emit_block_chain_slot` adds 4 NOPs and
    `emit_block_counter_accounting` adds 7 instructions to every block — about 44 bytes. These budgets
    *should* fail; they are measuring the instrumentation, not a defect.
  - **5 are genuine wrong-`ctx->pc` failures** (15681, 15772, 15860, 15953, 16475). At
    `hb_test_runner.c:15681`, `ASSERT(ctx->pc == 0x3b00)` after a guard block runs passes with chaining
    off and **fails with it on**. That is control flow ending up at the wrong guest address — precisely
    the class that would kill a boot — and it is now a seconds-long reproduction instead of a 15-minute one.
- step: **nothing broken was shipped: `MACRUNNER_HB_BLOCK_CHAIN` remains default-off**, so the tree's
  default behaviour is the measured-good SPEEDDIG2 configuration. The PC-guarded trampoline and the
  widened terminal set sit behind that gate awaiting the `ctx->pc` fix.
### Run SPEEDDIG3 — the deployed binary verified in its DEFAULT configuration, and n=2 for the fix

I had two runs on ntdll 7a3f1a44 and both had chaining armed and died, so I had **no evidence the deployed
binary boots at all with the default gates**. That gap needed closing before leaving the tree in this state.

- step: **no regression, and the prediction held: `avg_chain=1.0000`.** SPEEDDIG3 (default config, chaining
  unset, ntdll 7a3f1a44) reached **`Restored language` at +490.330 s**, `Begin MonoManager` +58.781 →
  `UnloadTime` +194.297 = **135.5 s**. So the gated chain code and the widened predicate cost nothing when
  the gate is off, which is what "default-off" has to mean to be worth anything.
- step: **the import fix now has n=2, and it reproduces.**

  | | `Restored language` | Mono phase |
  |---|---|---|
  | SPEEDDIG1 (pre-fix, instrumented) | 828.704 s | 284.2 s |
  | SPEEDDIG2 (post-fix) | 448.399 s | 150.5 s |
  | SPEEDDIG3 (post-fix, default cfg) | 490.330 s | 135.5 s |
  | brief's baseline | 593 ± 78 s | 212.6 s best / 237.7 s healthy max |

  Post-fix mean **469.4 s** to the language marker against a 593 ± 78 s baseline (≈1.26×, about 1.6σ with
  n=2), and a Mono phase of **143.0 s** mean against 284.2 s pre-fix — the phase figure is the tighter of
  the two and is below every reference number in the brief. Both post-fix runs still carry
  `MACRUNNER_HB_TRACE_DISPATCH_STATS=1` and a cold translation cache, so both are pessimistic.
- step: **honest position against the goal.** The goal is *в разы* below 593 s and Rosetta does it in under
  45 s. 469 s is an improvement, not a multiple. The multiple requires cutting the 4.64
  guest-instructions-per-dispatch, i.e. chaining — which is exactly what is broken below.
### 2026-07-30 iteration 2 — I REFUTE MY OWN "chaining breaks ctx->pc" claim

- step: **RETRACTED: the 5 wrong-`ctx->pc` failures are NOT caused by chaining.** I wrote above that
  `hb_test_runner.c:15681` "passes with chaining off and fails with it on", and called that a genuine
  control-flow break. Ran the missing control arm — `MACRUNNER_HB_SINGLE_LOOKUP=1` with
  `MACRUNNER_HB_BLOCK_CHAIN=0` — and all five (15681, 15772, 15860, 15953, 16475) fail there too:

  | line | both gates off | chaining on | single_lookup on, chaining OFF |
  |---|---|---|---|
  | 15681 / 15772 / 15860 / 15953 / 16475 | pass | FAIL | **FAIL** |

  So they are caused by `dispatch_fastpath`, which chaining merely happens to switch on as a side effect.
  Reading the test explains why: `guard_func`'s CFG holds only its own block at 0x3b0e, so without the
  fast path the dispatcher hits `find_block(func->cfg, 0x3b00) == NULL` and returns with pc=0x3b00, which
  is what line 15681 asserts. With the fast path it finds the body block at 0x3b00 in the *runtime-wide*
  block cache and correctly continues into it — the guest did jump to 0x3b00. The assertion encodes a
  non-fastpath dispatcher boundary, and **`MACRUNNER_HB_SINGLE_LOOKUP=1` is already the default in
  `laneA-run-hk.sh:288`**, so production has behaved this way all along.
- step: **failures attributable to chaining and to nothing else: exactly 8 — and all 8 are code-size
  budgets** (6891, 6943, 7350, 13695, 13753, 17080, 17278, 17595). Chaining adds ~44 bytes per block
  (`emit_block_chain_slot` 4 NOPs + `emit_block_counter_accounting` 7 instrs), so `ASSERT(code_buf->size
  <= N)` must fail. **The unit suite therefore shows ZERO correctness failures caused by chaining**, and
  the "seconds-long reproduction" I claimed last iteration does not exist. Corrected in
  [[project_hb_block_chaining_broken_20260730]].
- step: **so the 3/3 `exit=5` is once again unexplained**, and the honest reading is that I have no
  offline evidence against chaining. Next: measure it on HK directly — `avg_chain` is still the one
  number the brief's main lead has never produced.
- step: **[OPEN, named] why chaining lands the wrong `ctx->pc`** — withdrawn; see the retraction above.
  The live question is instead: why does HK die at ~+20 s with chaining armed, when the ISA-level suite
  is clean?
- step: **SPEEDDIG3 triage (`classify_run.py`): VERDICT BLOCKED, CLASS `GENERIC_GRAPHICS_FAULT`, rung 11
  (swapchain), `time_to_swapchain=122s`** — the same class SPEEDDIG2 produced, i.e. the post-menu graphics
  wall, not a regression from the import fix. The analyzer's `LADDER_REGRESSION` flag compares against a
  best-ever rung 13 from a *different* configuration (`laneA-BLACKFRAME-DRAWTRACE`), so it is not evidence
  about this change; the speed lane's own metric (the language marker) improved.
- step: **★ chaining is live, and the death is timed to it: `macrunner-hb-dispatch-gate: block_chain=1
  single_lookup=1 indirect_ic=0 legacy=0` at +50.750 s, `exit=5` at +51.242 s.** SPEEDCHAIN3 (blocking
  run) died **under 0.5 s after the JIT dispatcher first ran a block with chaining armed**, immediately
  after the second runtime creation (`rtmeter n=2`, cacheopen 17.34 ms). **Zero fault markers** — no
  SIGSEGV/SIGBUS/SIGILL, no quarantine, no interp fallback, no `code_cache_full`: the guest exits silently.
  `avg_chain` STILL unmeasured, because 50 000–100 000 dispatches are never reached before the death.
- step: **★ BISECT: the widening is INNOCENT — `MACRUNNER_HB_CHAIN_WIDE=0` dies too.** With chaining
  restricted to the original direct-`HB_IR_JMP` set, SPEEDCHAIN-NARROW died `exit=5` at +44.328 s, 0.2 s
  after the same gate line. So the fault is not in admitting `Jcc`/`CALL`/indirect jumps, and it is not in
  the PC guard either — CHAINONE died the same way on build 0ab0b84d, before both existed. **Tally: 5/5
  runs with `MACRUNNER_HB_BLOCK_CHAIN=1` die at exit=5; 3/3 with it off reach the menu.** Chaining has
  never worked in this engine, and that predates this lane.
- step: **[TRAP, mine] `timeout` does not exist on this box** — `timeout 400 scripts/...` returned exit=127
  instantly and measured nothing. Already recorded in
  [[lesson_guest_probe_vehicle_traps]]; I walked into it anyway. The run scripts take their own timeout
  argument, which is the only one to use.
### 2026-07-30 iteration 3 — the chaining exit=5 root-caused by bisection: a missing W^X bracket

- step: **SPEEDCHAIN-NARROW triage: `classify_run.py` returns `GENERIC_GRAPHICS_FAULT` with only
  prefix-sync lines as EVIDENCE** on a run that died at +44 s before any graphics existed. That is the
  documented generic fallback, not a finding; the informative signal is `flight.jsonl` `"status":5`. Noted
  so nobody mines that class for meaning.
- step: **★ BISECT, three arms, each decisive because these deaths are fast:**

  | arm | what is active | result |
  |---|---|---|
  | chaining off | — | boots (3/3) |
  | `MACRUNNER_HB_CHAIN_PATCH=0` | emit side only: 4-NOP slot + counter accounting | **boots** — `Initialize engine` +59.65 s, `Begin MonoManager` +61.29 s, 19.15 M dispatches, ended only on my own 300 s timeout (`exit=124`) |
  | `MACRUNNER_HB_CHAIN_WRITE=0` | + trampoline committed, tail write skipped | **exit=5 at +48.989 s** |
  | full chaining | + tail write | exit=5 |

  The emitted block shape is therefore **innocent**, and in the third arm **no trampoline is ever branched
  to** — nothing points at one — yet it still dies. So merely *building* a trampoline was fatal.
- step: **★ ROOT CAUSE: `chain_trampoline_for` wrote the trampoline into a re-protected JIT page.**
  `jit_commit_blob` makes the arena writable for its own `memcpy` of the zero block and then calls
  `hb_jit_buffer_commit`, which re-protects it; `chain_trampoline_build` then stored 20 instruction words
  straight into that page. A plain store to non-writable JIT memory takes the process down with no
  guest-visible exception — which is exactly the signature: `exit=5` within a second of the first
  dispatch, and **zero** SIGSEGV/SIGBUS/SIGILL/quarantine/interp-fallback lines in any of the five runs.
  `patch_block_tail` has always bracketed its own two stores with `make_writable`/`make_executable`; this
  path never did. Fixed by bracketing the build with `hb_jit_buffer_make_writable` /
  `hb_jit_buffer_commit`, releasing the bracket on the failure path too.
- step: **★ VERIFIED: the guest now survives the point that killed it 5/5.** Run CHAIN-WXFIX (ntdll
  f5aee510, full chaining, no sub-gates): dispatch gate at +49.661 s and **450 000 dispatches reported at
  +56.662 s** with no `exit=5`, where all five earlier chaining arms died within ~0.5 s of that same gate
  line. Stated precisely: the run was cut short at +56.7 s by MY OWN 10-minute wrapper timeout, not by the
  guest, so what is established is survival past the death point plus 450 k dispatches — not a completed
  boot. No orphan processes were left (checked: none live).
- step: **`avg_chain` is STILL exactly 1.0000, with `chainable_pct=5.21`.** So chaining is now safe but
  does **not engage**: had any chained block executed, the emitted `block_count` accounting would have
  made the delta > 1. That is the next question, and it is a different one from the crash — the crash is
  fixed, the mechanism is inert. Suspects, in order: `patch_block_tail`'s early
  `if (meta->target_code) return meta->target_code == next->native_code + 16;` (which compares a
  trampoline address against a block address and so can never be true — harmless for correctness because
  every caller discards the result, but it means a second attempt on the same predecessor is indistinguishable
  from a first), `entry_has_chain_slot` rejecting the tail, and `arm64_branch_reaches` failing across a
  128 MB arena whose `B` range is exactly ±128 MB.
### 2026-07-30 iteration 4 — chaining IS installed; `avg_chain` is blind to it by construction

- step: **CHAIN-WXFIX triage: rung 6 (mono-init)**, i.e. it lived well past the +44–51 s deaths; ended on
  my wrapper timeout. Chaining is stable now, not merely surviving: the follow-up run below ran to
  `exit=124` at +203 s (its own 180 s budget) with no `exit=5`.
- step: **★ counted the decline reasons instead of guessing them.** Added a per-reason histogram over
  every exit of `patch_block_tail` plus its call site (`macrunner-hb-chaindecline`). One run answers it:

  `site_called=389480  already=336061  slot_next=47054  PATCHED=6365`

  and **zero** for gate / invalid / nometa / terminal / slot_cur / tramp / reach / write_off / wprot /
  xprot. So the mechanism is not being refused anywhere I suspected last iteration: **6 365 block tails
  were successfully patched**, and `already=336061` proves those chained predecessors keep re-executing.
- step: **★ REFUTES my own "safe but INERT" conclusion from iteration 3, and explains `avg_chain=1.0000`
  as an INSTRUMENT DEFECT rather than a fact about chaining.** Proven by construction, not inference:
  the patch writes `MOV X0, X19` and `B <trampoline>` over the first two of the four NOPs
  (`hb_runtime.c:3158-3159`), while `entry_has_chain_slot` requires **all four** words at
  `native_size - 32` to still be NOP. So it returns false for every *patched* block — and that is exactly
  the predicate gating `native_accounting = chain_accounting && entry_has_chain_slot(cached, NULL)`
  (`:6011`, `:6234`). For precisely the blocks that got chained, the dispatcher therefore falls back to
  `blocks_executed++`, one per dispatch. **`avg_chain` can never exceed 1.0000 no matter how well chaining
  works.** The brief's headline metric is blind to the thing it was chosen to measure.
- step: **the same predicate is also throttling coverage: `slot_next=47054`.** A block that has already
  been patched as a predecessor no longer presents a slot, so it is rejected as a chain *target* — 47 054
  times here. Chaining is self-limiting for the same reason the metric is blind.
- step: **[COUNT] that is the third instrument defect this lane has found in its own measurements**, after
  the `eb89b54d` denominator (would have read ≈80 for 1.0000) and `dispatch_stats_register`'s
  `clock_gettime` per dispatch (3.9 % of the critical thread). The pattern is consistent enough to be worth
  stating: every one of them was found by checking the instrument against a case whose answer was known
  independently, never by reading the code alone.
### 2026-07-30 iteration 5 — chaining MEASURED at last: avg_chain 5.2, and it is a net loss that wedges

- step: **CHAIN-WHYDECLINE triage: rung 11 (swapchain)** — the same rung the non-chaining SPEEDDIG2/3
  runs reach, so chaining was boot-compatible at that point, not merely non-fatal.
- step: **fixed the blind predicate by accepting the PATCHED shape** (`MOV X0,X19` / `B` / NOP / NOP) beside
  the pristine four NOPs, keeping the byte-sniffing approach because the persistent cache restores blobs
  and anything derived from bytes survives a cache load for free where a struct field would need
  serialising.
- step: **★ PREDICTION CONFIRMED ON BOTH COUNTS.** Run CHAIN-SLOTFIX (ntdll 563d27f5):

  | quantity | before the fix | after |
  |---|---|---|
  | `avg_chain` | 1.0000 (pinned by construction) | **5.1998 – 5.9293** |
  | `slot_next` declines | 47 054 | **0 — gone from the histogram** |
  | `PATCHED` tails | 6 365 | **12 033** |

  So block chaining genuinely works: a dispatcher round trip now retires ~5.2 guest blocks instead of 1.
  That is the brief's main lead, measured for the first time in 218+ runs.
- step: **★ AND IT IS A NET LOSS, WHICH THE SAME RUN SHOWS.** Chaining bypasses the dispatcher 5× yet
  moves *fewer* guest blocks per second:

  | | blocks/s | dispatches/s |
  |---|---|---|
  | chaining off (SPEEDDIG2) | **2 243 438** | 2 243 438 |
  | chaining on (CHAIN-SLOTFIX) | **188 785** | 36 306 |

  a ~12× drop in guest throughput. Then the run **wedges**: the last real log line is +55.377 s, only 5
  dispatch-stats lines were ever emitted (250 000 dispatches in wall_s 6.886), `Mono path` printed but
  `Initialize engine version` and `Begin MonoManager` never did, and it sat until my 420 s budget
  (`exit=124`). CHAIN-EMITONLY, the same binary lineage with patching disabled, reached both markers by
  +61.3 s. **Chaining therefore stays default-off; nothing here justifies moving it.**
- step: **[HYPOTHESIS, unproven] the wedge is a chained cycle that stops returning to the dispatcher.**
  A guest loop A→B→A chained in both directions runs natively and correctly — that is the goal — but it
  only ever re-enters the dispatcher on a mispredict, so a hot loop whose exit edge is rare would spin in
  the arena while `ctx->block_count` climbs, which is exactly the shape observed (blocks high, dispatches
  low, then silence). Competing explanation not yet excluded: the 5× larger `steps`/`blocks` deltas now fed
  back into `out->steps_executed` may be tripping a budget in `macrunner_hb_run_x64`'s loop.
- step: **[NEXT STEP] sample the wedged process rather than re-running.** A live `sample` distinguishes the
  two hypotheses in one shot: a chained cycle shows samples inside the JIT arena with no symbol under
  `run_jit_block_with_signal_guard`, while a budget/loop problem shows them in `macrunner_hb_run_x64` or the
  dispatcher. The brief's own rule — sample a stalled process instead of paying for another run — applies
  directly, and the wedge is reproducible at ~+55 s.
- step: **the wedge is DETERMINISTIC, 2/2.** CHAIN-WEDGE repeated it: last real log line +52.745 s, then
  silence to the 150 s budget (`exit=124` at +173.205 s), `avg_chain=5.1886`, 5 dispatch-stats lines, only
  `Mono path`. And `PATCHED=12033` / `site_called=43810` are **the same counts as CHAIN-SLOTFIX** — so this
  is not a race but a repeatable state, which is the best possible starting point for the next bisect.
- step: **[NON-MEASUREMENT, mine] the wedge sample produced nothing.** The helper polled
  `ps -Ao pid,comm | grep -i hollow` and never matched, so it exited silently and no profile was written.
  Recorded as a failed instrument, not as evidence about the wedge — the run itself is unaffected, and no
  background process was left behind (verified 0 spike procs).
- step: **[NEXT STEP, cheap and decisive] refuse to chain BACKWARD edges.** If the wedge is a chained cycle
  that stops returning to the dispatcher, then declining to patch when `next->guest_addr <= cur->guest_addr`
  — the shape of a loop backedge — must make it disappear while leaving `avg_chain > 1` for forward chains.
  That is a two-line change plus one 150 s run, and it discriminates the cycle hypothesis from the
  `steps`/budget-feedback one without needing a profile. If the wedge survives forward-only chaining, the
  cause is the accounting feedback into `out->steps_executed`, not a cycle.
### 2026-07-30 iteration 6 — the cycle hypothesis is refuted, and so is my "chaining is a net loss"

- step: **CHAIN-WEDGE triage: rung 11 (swapchain)** on a run that wedged at +53 s and never printed
  `Initialize engine version`. So the analyzer's ladder rung is NOT a progress signal for these runs — it
  is satisfied by prefix/overlay markers laid down before the guest gets anywhere. Grade chaining runs on
  the log markers, never on `LADDER_RUNG`.
- step: **★ REFUTED: the wedge is not a chained cycle.** `MACRUNNER_HB_CHAIN_FORWARD_ONLY=1` declines any
  successor whose guest address is not strictly greater, i.e. every loop backedge. It fired hard —
  `backedge=258640` — and pulled `avg_chain` from 5.19 down to **1.3674–1.9611**, so it demonstrably
  removed the backward chains. **The wedge survived unchanged**: `Mono path` only, dispatch-stats stop at
  +57.05 s, `exit=124`. A chained loop that never returns to the dispatcher cannot be the mechanism.
- step: **★ RETRACTED — my own "chaining is a ~12× net loss" from iteration 5 was a window artifact.** I
  compared 188 785 blocks/s (chaining, 5th report line, `wall_s=6.886` of cold boot) against 2 243 438
  blocks/s (chaining off, LAST line, `wall_s=233` including steady state) — different windows AND
  different phases. Conditioning on comparable earliest windows instead:

  | run | window | blocks/s | avg_chain |
  |---|---|---|---|
  | SPEEDDIG2 (chain off) | 7.450 s | 268 456 | 1.0000 |
  | SPEEDDIG3 (chain off) | 0.117 s | 854 701 | 1.0000 |
  | CHAIN-EMITONLY (patch off) | 0.058 s | 862 069 | 1.0000 |
  | CHAIN-SLOTFIX (full chain) | 0.280 s | **1 041 814** | 5.8342 |
  | CHAIN-FWDONLY (forward only) | 0.072 s | 949 611 | 1.3674 |

  Chaining is if anything the fastest of these, not 12× slower. The windows still differ by two orders of
  magnitude, so **no throughput verdict on chaining is supportable either way** — the chaining arms wedge
  at ~+57 s and never reach the steady state the non-chaining headline rate is drawn from. The brief's own
  rule (condition on wall-clock before any rate stat) is what caught this; I had already violated it once
  today in the opposite direction.
- step: **what survives as established.** Chaining works (`avg_chain` 5.19 full / 1.37–1.96 forward-only,
  ~11.5–12 k tails patched); it wedges the boot deterministically at ~+53–57 s in BOTH configurations; the
  emit side alone boots fine; the cycle explanation is dead; and its speed effect is **unmeasured**, because
  no arm with chaining has ever reached steady state. `MACRUNNER_HB_BLOCK_CHAIN` stays default-off.
- step: **[NEXT STEP] bisect on HOW MUCH chaining is survivable, not on which kind.** Both kind-based
  splits (wide vs narrow, forward vs all) wedge, so the discriminator to try next is a cap: patch at most N
  tails, then stop. If N=64 boots and N=∞ wedges, the wedge is a cumulative/resource effect and the cap
  itself is a shippable subset; if even a handful of patches wedges, log those few `cur→next` guest address
  pairs and the culprit edge is named directly. Either outcome is progress, and each arm is one 240 s run.
### 2026-07-30 iteration 7 — ONE patched edge wedges the boot, and it is a self-chain

- step: **CHAIN-FWDONLY triage: rung 11 (swapchain)** again on a wedged run — third confirmation that
  `LADDER_RUNG` is satisfied by prefix/overlay markers and says nothing about guest progress here.
- step: **bisected on quantity with a new cap (`MACRUNNER_HB_CHAIN_MAX_PATCHES`) plus edge logging
  (`MACRUNNER_HB_TRACE_CHAIN_EDGE`), after both kind-based splits had failed.**
  - **cap=8 → wedges.** `capped=3003107 PATCHED=8`, `avg_chain=1.0000`, only `Mono path`, `exit=124`.
  - **cap=1 → wedges.** `capped=3003092 PATCHED=1`. So a **single** patched tail is enough.
- step: **★ the one fatal edge is a SELF-CHAIN, logged and deterministic:**
  `macrunner-hb-chainedge: n=1 cur=0x87ef3e43250 next=0x87ef3e43250` — identical address in both the cap=1
  and cap=8 runs, with `site_called` reproducing to within 20 attempts (3 003 092 / 3 003 107).
  The mechanism is immediate: the block's tail branches to its OWN trampoline, whose guard compares
  `ctx->pc` against that same `guest_addr` — which the block itself just stored, because the guest block is
  a self-loop. The guard therefore always matches and **control never returns to the dispatcher**, so every
  per-dispatch host action for that thread stops: the import-thunk check in `macrunner_hb_run_x64`, signal
  and event servicing, wineserver interaction. A spin-wait that previously re-entered the host on every
  iteration now spins in the arena forever. That is the wedge, and it needs no cycle of two blocks — one
  block suffices, which is why chaining *kind* filters never helped.
- step: **[UNRESOLVED, stated rather than glossed] self-chains are sufficient but probably not the only
  fatal shape.** `MACRUNNER_HB_CHAIN_FORWARD_ONLY=1` declines `next->guest_addr <= cur->guest_addr`, which
  **includes** the self case, yet that arm still wedged with 11 458 patches installed. So at least one
  non-self forward edge is also fatal, or the forward-only arm wedged for a different reason. I am not
  claiming self-chains explain every arm.
- step: **★ RAN IT: forward-only + cap=8 ALSO WEDGES — so the self-chain is NOT the cause.** `PATCHED=8`,
  `backedge=956801` (self and backward edges all refused), 8 strictly-forward distinct edges installed:
  `0x87ef3e42208→0x87ef3e4221b`, `→0x87ef3e4aa60`, `→0x87ef3e81c10`, `0x87ef3e42227→0x87ef3e4222b`,
  `→0x87ef3e42258`, `→0x87ef3e4225e`, `→0x87ef3e4aa90`, `→0x87ef3e81c30`. Same wedge: `Mono path` only,
  `exit=124`. The self-chain hypothesis from this same iteration is therefore **retracted as the cause** —
  it is a real hazard and still worth refusing, but it does not explain the wedge.
- step: **★ CONCLUSION the four cap/kind arms force: executing ANY chained transition wedges the guest.**
  No patch at all boots to `Begin MonoManager`; one self edge wedges; eight forward non-self edges wedge;
  thousands wedge. Edge *selection* is not the variable — the chained-execution path itself is broken on
  its first use. That is a much better-posed defect than "chaining is unsafe", and it retires three
  hypotheses (cycle, backedge, self-chain) with evidence rather than argument.
### 2026-07-30 iteration 8 — the chained transition is CLEAN; the wedge follows the arena write, not the jump

- step: **CHAIN-FWD8 triage: rung 11** — fourth wedged run classified at swapchain. Settled.
- step: **★ ran the transition trace with cap=1. Exactly one chained transition executed, and it was
  correct:** `macrunner-hb-chaintransit: from=0x87ef3e43250 pc_after=0x87ef3e4325c blocks=4 steps=16`.
  The self-chained block ran **4 iterations inside one native dispatch** and returned to the dispatcher at
  `0x87ef3e4325c` — twelve bytes past the block start, a perfectly sane guest address.
  **So the trampoline does not corrupt control flow**: the guard matched, the chain ran, the bail-out
  unwound the frame, and the dispatcher resumed normally. Combined with the clang-verified encodings and the
  verified `native_code + 12` entry, the whole trampoline mechanism is now positively confirmed working.
- step: **★ and yet that run wedges — with ONE patch and ONE chained transition, after which the guest kept
  dispatching ~3.4 M blocks and only then stalled.** So the wedge is not the chained jump. What separates
  every wedging arm from the booting one is narrower than that: `MACRUNNER_HB_CHAIN_PATCH=0` boots and
  performs **zero** extra `make_writable`/`commit` cycles on the JIT arena, while cap=1 (one tail patch) and
  `CHAIN_WRITE=0` (trampoline commit only, no tail patch at all) both wedge and both perform **exactly one
  extra cycle**. The correlation across five arms is with *touching the arena outside `jit_commit_blob`*,
  not with chaining.
- step: **[HYPOTHESIS, now specific and checkable] `hb_jit_buffer_make_writable` sets
  `buf->dirty_start = buf->used`, so the paired `commit` computes an EMPTY dirty range and skips
  `__builtin___clear_cache` entirely** (`hb_jit.c`: `dirty_end > dirty_start` is false when both equal
  `used`). That is correct for `jit_commit_blob`, which writes AT `used` and advances it — but both chaining
  writers target memory *below* `used` (an already-committed block tail, or a trampoline whose 128 bytes
  were already accounted). Their correctness therefore rests entirely on the explicit
  `block_cache_clear_icache` calls, and the surrounding W^X cycle contributes nothing but a
  `pthread_jit_write_protect_np(0)/(1)` toggle that is thread-global on Apple silicon. `make_executable` is
  a plain alias for `commit`, so that is not the difference.
- step: **[NEXT STEP] remove the extra cycle rather than tune it.** Build the trampoline into a LOCAL
  128-byte buffer and hand it to `jit_commit_blob` in one shot, so no writer outside `jit_commit_blob` ever
  toggles the arena; the layout can be computed from the reserved address before filling the local copy.
  If that boots with chaining armed, the arena bookkeeping was the wedge all along and chaining becomes
  measurable for real. It also predicts the `CHAIN_WRITE=0` arm should then boot, which is a free check.
### 2026-07-30 iteration 9 — W^X refuted, arena exhaustion refuted; isolated to the eviction unchain path

- step: **removed the extra W^X cycle properly**: `chain_trampoline_build_at` now assembles into a LOCAL
  buffer for a predicted address and `chain_trampoline_for` commits it in ONE `jit_commit_blob` call, with a
  `dest != predicted` guard so a layout computed for the wrong address refuses to chain rather than emitting
  silent corruption. Clean under `-Werror`.
- step: **★ REFUTED, my own W^X hypothesis, by its own prediction.** It predicted `CHAIN_WRITE=0` would now
  boot. Ran it (CHAIN-NOWX, ntdll b1bdee3d): **still wedges** — `write_off=3003120` so zero tail patches and
  zero extra arena toggles, `Mono path` only, dispatching stops at +53.3 s after 3.4 M dispatches,
  `exit=124`. The W^X bracket was never the cause.
- step: **★ REFUTED, arena exhaustion.** The trampoline commits could in principle fill the 128 MB arena and
  latch `code_cache_full`, which would drop the guest to the interpreter and look exactly like a wedge.
  Checked all four runs for `jit-buffer-full` / `code-cache-full` / `interp fallback`: **zero occurrences in
  every one**, including the booting arm. Not it either.
- step: **★ the isolated variable is now `cache->chain_meta` being ALLOCATED.** `MACRUNNER_HB_CHAIN_PATCH=0`
  boots and returns from `patch_block_tail` at the first gate, so `block_cache_chain_meta(..., create=true)`
  is never called and `cache->chain_meta` stays NULL. Every wedging arm calls it. The allocation itself is
  sound (`calloc(HB_BLOCK_CACHE_SIZE, ...)`, indexed by entry index), but it is also the **enable flag for
  the eviction-time unchain machinery**: `block_cache_chain_meta_const` and friends at `hb_runtime.c:516-590`
  all bail on `!cache->chain_meta`, so with it NULL they are inert, and with it allocated they run on every
  eviction — including the path that writes `arm64_nop` back over a block tail (`:525-527`) and the one that
  flips a trampoline literal to its bail-out. That is unchanged pre-existing code, which fits the fact that
  chaining has never worked, and it is consistent with a wedge rather than a crash: eviction quietly rewrites
  tails while chains still point at them.
### 2026-07-30 iteration 10 — a real latent bug fixed, and it is STILL not the wedge

- step: **CHAIN-NOWX triage: rung 11.** Fifth wedged run, same class. Settled.
- step: **★ found and fixed a genuine second instance of the unbracketed-arena-write bug.** In
  `block_cache_prepare_replace_entry` the eviction literal store sat in the FIRST `if` block while
  `hb_jit_buffer_make_writable` was only called in the SECOND — so
  `__atomic_store_n(slot, bail, RELEASE)` wrote into the JIT arena while it was mapped read+execute. It
  fires exactly when the evicted block has a trampoline, which is the variable the bisection had isolated.
  Both writes now share one bracket, preserving the ordering the original comment cared about (literal
  retired before the unchain walk). Worth keeping on its own merits.
- step: **★ REFUTED — it is not the wedge either.** CHAIN-EVICTFIX (ntdll a7de44ab, full chaining):
  `avg_chain=5.6543`, `PATCHED=11003`, and **still** `Mono path` only, dispatching stops at +53.1 s,
  `exit=124`. Three mechanisms have now been proposed and killed by their own predictions in two
  iterations: the trampoline W^X bracket, arena exhaustion, and the eviction literal store.
- step: **also worth recording: the W^X cycle per eviction was never a candidate.**
  `block_cache_prepare_replace_entry`'s `make_writable`/`make_executable` pair is gated only on
  `runtime_block_chain_enabled()`, which is TRUE in the booting `MACRUNNER_HB_CHAIN_PATCH=0` arm too. So
  that arm performs the same eviction W^X cycles and boots — which is what let me discard the cycle
  explanation rather than tuning it.
- step: **where this stands, stated without inflation.** The wedge is reproducibly isolated to *creating a
  trampoline at all* (`chain_meta` allocated + one `jit_commit_blob` of 128 bytes + `in_trampoline` set):
  zero patches still wedge, one patch wedges, 11 003 patches wedge, and no trampoline boots. The chained
  transitions themselves are proven correct at runtime. The mechanism is NOT yet found, and I have stopped
  guessing at it — three offline hypotheses in a row were wrong.
- step: **[NON-MEASUREMENT #2, mine — worse than the first because it produced confident-looking output.]**
  I sampled the wedged run with `pgrep -f extracted-hollow-knight` and got a clean-looking profile: one
  thread, 503/503 samples in unsymbolized `???`, no `hb_jit_runtime_run` frame — which reads exactly like
  "the thread is spinning in JIT arena code and never returns to C", the answer I was expecting. **It was
  `/bin/bash`.** The sample header says `Path: /bin/bash`, `Identifier: bash`, and the `???` addresses all
  sit inside bash's own load range (0x102608000–0x10268f45b per its Binary Images). `pgrep -f` matched a
  WRAPPER's command line containing that path — the exact trap already recorded in
  [[lesson_macos_has_no_setsid_and_pgrep_matches_wrappers]] and in the brief's "count on `comm`, never
  `ps -Awwo args | grep`". My previous entry recommended this matcher; that advice was wrong and is
  withdrawn. Nothing about the wedge was learned, and had I not checked the header I would have written a
  fabricated root cause into this journal.
### 2026-07-30 iteration 11 — the wedge is a LOST WAKEUP, not a spin (measured, target verified)

- step: **CHAIN-SAMPLE triage: rung 11, and markers confirm the wedge** — `Mono path` ×1,
  `Initialize engine version` ×0, `Begin MonoManager` ×0. Sixth reproduction.
- step: **sampled the wedged guest properly this time.** Matched on `comm` (a basename, so it cannot match a
  wrapper's arguments): `ps -Ao pid,comm | awk '/[Hh]ollow/ {print $1; exit}'` → pid 22923, `comm` = the
  game executable. Verified the sample header BEFORE reading any count: `Process: wine [22923]`,
  `Path: /Users/*/Documents/*/wine` — the real guest, not `/bin/bash` as last iteration.
- step: **★ THE WEDGED PROCESS IS NOT SPINNING — EVERY THREAD IS PARKED.** Two samples, 70 s apart, both
  stable:

  | | threads | states |
  |---|---|---|
  | A (+70 s) | 8 | 1 × `mach_msg2_trap`, 7 × `__ulock_wait2` |
  | B (+125 s) | 8 | identical |

  **Zero threads with `hb_jit_runtime_run` frames, zero in unsymbolized `???` code, zero running.** So the
  wedge is a **lost wakeup / deadlock**: nobody is executing, which is why dispatch counters simply stop and
  why no fault marker is ever emitted. It also explains the CPU dropping away rather than pinning.
- step: **this retires the spinning family of explanations outright** — including my original chained-cycle
  idea and the bash-profile reading I discarded last iteration, which had claimed exactly the opposite
  ("spinning in arena code, no dispatcher frame"). Two independent verified samples say parked, not looping.
  Discarding that non-measurement was the right call, and this is what it would have buried.
- step: **[CONNECTS TO KNOWN GROUND]** all-threads-in-`__ulock_wait2` is the signature family already
  documented for this project in [[project_hb_two_futex_queues_20260729]]: HyperBridge and wine maintain
  separate futex queues, and `sync.c` builds SRWLock/condvar/critsect on PE-INTERNAL calls HB cannot
  intercept, where `equal=0` proves a lost wakeup. Chaining plausibly removes the dispatcher visit at which
  some wake would otherwise be observed — but that is a **[HYPOTHESIS]**, and after three offline guesses in
  a row I am not spending a run on it before instrumenting.
### 2026-07-30 iteration 12 — the wedge NAMED: the main guest thread dies `c000007b` in Mono

- step: **[STALE MEMORY, corrected] the instrument my own note prescribed does not exist in this tree.**
  `WAIT_WAKE_TRACE` / `ARM_FILE` from [[project_hb_two_futex_queues_20260729]]: `grep -rn "WAIT_WAKE"` over
  `engine/` returns **nothing**, as do `TRACE_WAITADDR`, `rtl_wait_on_address` and `WakeByAddress`. That note
  named flags from another worktree or an unlanded change. Verified before use, per the standing rule about
  recalled memories naming files and flags.
- step: **read the stacks I already had instead of running anything.** The two verified samples were only
  summarised at the tip last iteration; the full spines answer it outright. All 8 threads:
  - 1 × Cocoa main thread, `__wine_main → CFRunLoopRun → __CFRunLoopServiceMachPort → mach_msg2_trap` — a
    normal idle run loop.
  - 7 × **`AssetGarbageCollectorHelper`**, every one identical:
    `macrunner_hb_run_x64 → macrunner_hb_call_import_thunk → macrunner_hb_try_kernel32_handle_semantic →
    NtWaitForSingleObject → inproc_wait → msync_wait_objs → msync_wait_single → __ulock_wait2`.
  - **No thread has an `hb_jit_runtime_run` frame. The guest's MAIN thread is simply absent.**
- step: **★ and the run log names why.** At **+48.076 s**:
  `macrunner-hb-run-exit: label=thread status=c000007b reason=runtime pc=0x87ef2469a30`, with the failure
  backtrace slots pointing into **`mono-2.0-bdwgc.dll` rva=0x3cc250**, plus `run-exit-indirect` recording
  `r15` and the q-registers. So with chaining armed the guest's main thread **dies with `c000007b` inside
  Mono**, and the seven helpers are innocent bystanders correctly parked on a producer that no longer exists.
  The process stays alive because the Cocoa thread and the helpers do, which is exactly why the harness
  reports `exit=124` rather than a crash.
- step: **[CORRECTION to my own previous entry] "lost wakeup / deadlock" was imprecise.** The waiters are not
  victims of a missed wake between peers — their producer thread died. The measurement (all threads parked)
  was right; the interpretation I attached to it was not. The corrected statement is: **a single thread dies,
  and everything else parks behind it.**
- step: **[CORRECTION] my W^X and eviction fixes did change the failure mode, and I described them as "still
  wedging identically".** The earliest chaining arms died `exit=5` (whole process gone within a second of the
  first dispatch); every arm after the trampoline W^X fix instead reaches +48 s, loses one thread to
  `c000007b`, and hangs at `exit=124`. That is a different and later failure, i.e. those fixes were real
  progress rather than no-ops. I should have separated the two failure modes by their exit codes instead of
  filing them together as "the wedge".
- step: **[CONNECTS, and this time the memory checks out] `c000007b` on a thread inside Mono is the signature
  in [[project_monojit_target_validation_unified_root]]** — the validator rejecting unregistered
  indirect-call targets — and [[project_hk_rung12_worker_thread_death]] records a thread dying `c000007b`
  before. **[HYPOTHESIS]** chaining skips the dispatcher visit at which an indirect-call target would be
  registered, so Mono's next indirect call lands on a target the validator has never seen. Not yet tested;
  `c000007b` is not raised literally in `hb_validate.c` or `macrunner_hb.c`, so the emitter of
  `status=... reason=runtime` has to be found first.
### 2026-07-30 iteration 13 — `c000007b` was a catch-all; the real failure is a JIT helper memory fault

- step: **`c000007b` carries no information — it is the generic bucket.** `macrunner_hb.c:39761` sets
  `status = STATUS_INVALID_IMAGE_FORMAT; status_reason = "runtime"` for *any* failed HyperBridge run. The
  informative line is the `ERR(...)` immediately above it, printing `ret`, `out.result` and
  `out.fault_reason` — and it was invisible because the run script sets `WINEDEBUG=-all`, which suppresses
  `ERR` while leaving raw `fprintf` markers like `run-exit` visible. So the Mono-validator reading I floated
  last iteration was built on a status code that means nothing. **Withdrawn.**
- step: **★ re-ran with `MACRUNNER_HK_WINEDEBUG='err+all'` and the failure names itself:**

  `err:module:macrunner_hb_run_x64 MacRunner HyperBridge run failed thread pc=0x87ef2469a30`
  **`ret=OK out=MEMORY_FAULT reason="JIT helper fault"`**
  `rax=0x462a147cddc6 rcx=0xf0e0993f rdx=0x320eec31 rdi=0xcd26c53d rsi=0x11bc1dbb0 rsp=0x11bc1d978`

  followed by `macrunner_hb_BaseThreadInitThunk ... thread callback failed`. So the main thread dies because
  a **guest memory access inside a JIT helper faults**, not because anything rejected an image or a call
  target. `rsi`/`rsp` look like plausible guest-stack addresses while `rax`/`rcx`/`rdx`/`rdi` do not, which
  is at least consistent with corrupted guest state by the time the helper runs — though "looks like garbage"
  is an impression, not a measurement, and is marked as such.
- step: **also visible in the same window, and not yet explained:** a first-chance `c0000005` at
  `addr=00000001119DA440` on thread 0060 half a second earlier (+47.811 s) reported as
  `Unhandled exception code c0000005`, and two `LdrLoadDll` failures with `status=c0000135`
  (module-not-found) against a path under the game directory. Whether the `c0000005` on another thread is
  upstream of the main thread's helper fault or an independent symptom is **[UNKNOWN]** — both appear only in
  chaining arms so far, but I have not compared against a booting arm with `err` enabled, which is the
  control this needs.
- step: **★ CONTROL RUN — the helper fault IS attributable to chaining, and the `c0000005` is not.** Same
  binary, same `err+all`, only `MACRUNNER_HB_CHAIN_PATCH` differing:

  | | markers reached | `HyperBridge run failed` | `c0000005` first-chance |
  |---|---|---|---|
  | control (emit side only, no patching) | `Mono path` + **`Initialize engine version`** + **`Begin MonoManager`** | **0** | 1 |
  | test (full chaining) | `Mono path` only | **1** (`out=MEMORY_FAULT reason="JIT helper fault"`) | 1 |

  So the JIT-helper memory fault occurs **only when tails are patched** — attribution established with a
  control rather than asserted. And the first-chance `c0000005` at `0x1119DA440` appears **once in both
  arms**, so it is pre-existing background and is now excluded as a suspect; the two `c0000135`
  `LdrLoadDll` failures likewise want no further attention until something depends on them.
- step: **[NOTE] `MACRUNNER_HB_TRACE_JIT_HELPER_FAIL=1` printed nothing** (`macrunner-hb-block-fault-pc`: 0
  lines in both arms) even though the failure reports `reason="JIT helper fault"`. So this fault does not
  reach the `ctx->last_result != HB_OK` branch that trace guards — it arrives another way, most likely the
  signal-guard path setting `out->faulted`. The next probe must print the faulting guest ADDRESS from
  wherever `set_helper_fault_result` is actually reached, not from that gate.
### 2026-07-30 iteration 14 — mid-block epilogue slots: fixed, refuted, and proven inapplicable

- step: **CTRL-NOPATCH-ERR triage: `GENERIC_ACCESS_VIOLATION`, rung 11** — note the class differs from the
  chaining arms' `GENERIC_GRAPHICS_FAULT` even though this is the arm that BOOTS further, which is one more
  reason not to read meaning into that field.
- step: **found a real latent hazard and removed it.** `emit_return_if_helper_failed` emits a full
  `emit_epilogue` *inside* the block for the helper-failure branch, and every epilogue carried a 4-NOP chain
  slot. Since `entry_has_chain_slot` identifies the slot purely by position (`native_size - 32`), any block
  whose last emitted epilogue was the failure one would have had its **failure path** patched — continuing
  into a successor instead of returning with `ctx->last_result` set. Mid-block epilogues now emit no slot
  (`emit_epilogue_ex(buf, false)`), so such blocks simply stop being chainable.
- step: **★ REFUTED, and the refutation is airtight because of one number: `PATCHED=11003`, bit-identical to
  the run before the change** (and `site_called=36018` vs 36028, `already=25015` vs 25025). If any block had
  ended with a helper-failure epilogue, removing its slot would have reduced the chainable population. It did
  not move at all — so **no block in this workload ends that way**, the change is a no-op here, and the
  hypothesis was inapplicable rather than merely wrong. Keeping the change anyway: it closes a hazard that
  would fire the moment a differently-shaped block appeared.
- step: **the failure is far more deterministic than I had appreciated, which is the useful lead.** Across
  CHAIN-ERRWHY and CHAIN-SLOTONLYFINAL the fault reproduces at the **same guest pc `0x87ef2469a30`** with
  **identical `rcx=0xf0e0993f` and `rdx=0x320eec31`**; only `rax` differs (0x462a147cddc6 vs 0x74d80e434565).
  Same `out=MEMORY_FAULT reason="JIT helper fault"`, same `PATCHED=11003`.
- step: **[TALLY] four mechanisms proposed and killed on this wedge:** chained cycle / backedge / self-chain,
  the trampoline W^X bracket, arena exhaustion, the eviction literal store, and now the mid-block epilogue
  slot. Every one was an offline guess about a mechanism; every one died. The two things that DID produce
  knowledge were both direct observations — the verified `sample` (all threads parked, main thread absent) and
  turning `err` back on (`out=MEMORY_FAULT reason="JIT helper fault"`). I am not proposing a fifth mechanism.
- step: **[NEXT STEP] identify `0x87ef2469a30` and its relationship to a chained edge.** Two concrete,
  observation-only questions: (a) which module and RVA is that pc — the fail-bt slots named
  `mono-2.0-bdwgc.dll rva=0x3cc250` but the failing pc is in the `0x87ef246…` region, not `0x87ef27c…`, so
  they are different modules and the pc must be resolved on its own; (b) is that block, or its predecessor,
  one of the 11 003 patched tails — answerable by logging chained edges filtered to that address instead of
  the first 64. That names the exact chained edge that breaks the guest, which no mechanism guess has managed.
### 2026-07-30 iteration 15 — the faulting block is a CHAIN TARGET, and its predecessors are the backtrace

- step: **CHAIN-SLOTONLYFINAL triage: `GENERIC_ACCESS_VIOLATION`, rung 11.**
- step: **[MY ERROR, corrected] the helper-fault trace "printing 0 lines" was my own doing.** I set
  `MACRUNNER_HB_TRACE_JIT_HELPER_FAIL=1` only in the CONTROL arm — which does not fault — and then reported
  the absence of output from the TEST arm, where the flag was never set. Set it in a faulting arm and it
  fires immediately:
  `macrunner-hb-block-fault-pc: pc=0x87ef2469a30 last=-8 guest_addr=0x87ef2469a30`.
  `pc == guest_addr` means the fault lands on the block's FIRST memory access, before any progress.
- step: **resolved the faulting pc from the fail-bt, no run needed.** `val=0x87ef246adc2` with
  `rva=0x6adc2` fixes the module base at `0x87ef2400000`, so the faulting pc is
  **`mono-2.0-bdwgc.dll+0x69a30`**, and every backtrace frame is in that same DLL (0x69a30, 0x6adxx, 0x6b2xx,
  0xb040c, 0xbc114, 0x546b9b …) — Mono's Boehm GC, which is also what the seven parked
  `AssetGarbageCollectorHelper` threads are.
- step: **★ replaced the first-64 edge cap with an address filter (`MACRUNNER_HB_CHAIN_EDGE_NEAR`) and the
  link is direct: the faulting block IS a chain target, three times, and never a source** (`as next: 3`,
  `as cur: 0`):

  | edge | predecessor | also appears as |
  |---|---|---|
  | n=11199 | `0x87ef246aee7 → 0x87ef2469a30` | **`fail-bt[3] val=0x87ef246aee7`** |
  | n=12842 | `0x87ef246adb6 → 0x87ef2469a30` | **`run-exit-prev pc=0x87ef246adb6`** |
  | n=12843 | `0x87ef246adc9 → 0x87ef2469a30` | — |

  Two of the three predecessors that chain into the dying block are the very frames the failure backtrace
  names. All three share one trampoline (`0x2000086a0840`), as expected for a per-target trampoline. This is
  the first evidence that ties the chain mechanism to the fault site rather than to a guess.
- step: **[OBSERVATION, unexplained] chained sequences are ending at SYNTHETIC IMPORT addresses.**
  `chaintransit` shows `from=0x87ef3e4aa60 pc_after=0x6f0000002240 blocks=2`,
  `from=0x87ef3e42227 pc_after=0x6f00000024e0 blocks=6`, and more — `0x6f0000000000` is
  `MACRUNNER_HB_IMPORT_BASE`. So a chain runs several blocks and exits with `ctx->pc` pointing at an import
  thunk. That should be benign (the thunk is not a cached block, so the guard mispredicts and the dispatcher
  handles the import), and the O(1) index from this lane's earlier fix resolves such addresses correctly —
  but it means chained runs routinely hand the dispatcher a synthetic PC, and I have not verified that path
  is equivalent to reaching the same thunk unchained.
### 2026-07-30 iteration 16 — PROVEN: chaining corrupts guest rcx/rdx before the faulting block

- step: **CHAIN-NEAREDGE triage: `GENERIC_ACCESS_VIOLATION`, rung 11.**
- step: **★ the chained transition that lands on the faulting block, with registers either side:**

  `chaintransit: NEAR from=0x87ef246adb6 pc_after=0x87ef2469a30 blocks=2 steps=71`
  **`rcx 5000000000000 → f0e0993f`   `rdx 11d66db60 → 320eec31`**

  Those post-values are *exactly* the ones the failure line reports (`rcx=0xf0e0993f rdx=0x320eec31`), in
  every run. So the corruption happens **during** a chained run of 2 blocks / 71 steps, and the very next
  dispatch of `0x2469a30` faults on its first memory access.
- step: **★ and the control settles what those registers SHOULD be.** Same binary, `CHAIN_PATCH=0`, entry
  state of that exact block over 8 dispatches:

  | | rcx | rdx | rsp |
  |---|---|---|---|
  | unchained (n=1…8) | `0x11dc1d9d0` / `0x11dc1dbb0` | `0x11dc1d9ec` / `0x11dc1dbcc` | `0x11dc1d9xx` |
  | chained | **`0xf0e0993f`** | **`0x320eec31`** | — |

  Unchained, `rcx` and `rdx` are **guest stack pointers** with `rdx = rcx + 0x1c` — the block is a function
  taking two pointers into its caller's frame. Chained, they are 32-bit values that are not addresses at all,
  which is precisely why the first memory access faults. The control run also reaches `Begin MonoManager`
  with **0** `HyperBridge run failed` lines.
- step: **the defect statement is now concrete and no longer about "wedging":** *chaining corrupts guest
  `rcx`/`rdx` across the chained run beginning at `0x87ef246adb6`, so the successor block
  `mono-2.0-bdwgc.dll+0x69a30` is entered with non-pointers and faults on its first access, killing the main
  thread and parking the seven GC helpers behind it.* Every element of that sentence is measured: the
  transition, the before/after registers, the unchained baseline, the fault site, the thread states.
### 2026-07-30 iteration 17 — refusing the suspect edges removes the FAULT but not the failure: chaining is general

- step: **CTRL-BLOCKENTRY triage: `GENERIC_ACCESS_VIOLATION`, rung 11** — that arm boots to
  `Begin MonoManager` with zero failures, so once again the classifier's class says nothing useful here.
- step: **★ ran the pre-registered discriminator.** `MACRUNNER_HB_CHAIN_REFUSE_NEAR=1` declined every edge in
  the ±64 KB window around the faulting block (`refused_near=130241`) while leaving chaining fully active
  elsewhere (`PATCHED=13933`, `avg_chain=3.5817`). Result:
  - **the memory fault VANISHED** — `HyperBridge run failed` count **0**, no `block-fault-pc` line at all;
  - **the boot still did not progress** — `Mono path` only, no `Initialize engine version`, `exit=124`.
- step: **★ and the dispatch counters show it is a STALL, not slow progress:**

  | | total dispatches | stats wall_s | stats lines |
  |---|---|---|---|
  | REFUSENEAR (chaining on) | **800 000** | **7.09** | 2 |
  | CONTROL (`CHAIN_PATCH=0`) | **482 000 000** | 172.6 | 1205 |

  Dispatching stopped about 7 s after it began and produced nothing for the remaining ~230 s, against 482 M
  dispatches in the control. So chaining has **two independent fatal effects**: register corruption on some
  edges (now shown removable in isolation) and execution stopping outright. My pre-registered reading of this
  arm was "vanishes ⇒ block-pair defect; relocates ⇒ general". It did neither cleanly — the fault vanished and
  a different failure remained — which is the general answer: **the defect is not that block pair.**
- step: **[DECISION] park chaining and go back to the profile.** The evidence for this is not fatigue, it is
  the balance of what is measured. On chaining: seven mechanism hypotheses proposed and refuted (cycle,
  backedge, self-chain, trampoline W^X, arena exhaustion, eviction literal store, mid-block epilogue slot),
  two distinct fatal modes still live, and its best measured value is `avg_chain` 5.2–5.9 whose *speed* effect
  has never been observable because no chaining arm has ever reached steady state. Meanwhile the post-import-fix
  profile has an untouched 15.1 % item — `_platform_memmove`, the 1592-byte per-dispatch context snapshot in
  `run_jit_block_with_signal_guard` — plus `hb_jit_runtime_run` 12.5 %, the guard itself 10.6 %,
  `_tlv_get_addr` 8.8 %, and the guest MMU ~10 %. Those are contained, single-mechanism targets of the same
  kind as the import scan that already delivered 828.7 s → 448.4/490.3 s. `MACRUNNER_HB_BLOCK_CHAIN` stays
  default-off with every gate and instrument built this week left in place, so resuming costs nothing.
### 2026-07-30 iteration 18 — ★ SECOND WIN: the per-dispatch snapshot, 2616 → 760 bytes

- step: **CHAIN-REFUSENEAR triage: `GENERIC_ACCESS_VIOLATION`, rung 11.**
- step: **[CORRECTION to my own previous entry] the snapshot was copying 2616 bytes, not 1592.** I wrote that
  `hb_ctx_snapshot_save` copies 1592 bytes per dispatch. It does — *when the gate is on*. But
  `MACRUNNER_HB_SNAPSHOT_SKIP_INTERP` is **default off**, so every run measured this week took the
  `*dst = *src` path and copied the **full 2616 bytes**. The profile's 15.1 % `_platform_memmove` was the cost
  of the whole struct. A pre-existing, already-reasoned optimisation had simply never been armed — the same
  pattern as `BLOCK_CHAIN` and `INDIRECT_IC` in the brief.
- step: **widened the skip window from `xmm_ext` to `ymm_hi`, after an exhaustive safety check.** The
  interpreter-only AVX region is three fields larger than the old window assumed:
  `ymm_hi [640,896)`, `zmm_hi [896,1408)`, opmask `k [1408,1472)`, `xmm_ext [1472,1728)`,
  `ymm_hi_ext [1728,1984)`, `zmm_hi_ext [1984,2496)` — one unbroken **1856-byte** run. Safe because the
  emitter cannot reach it: the COMPLETE set of `offsetof(hb_context_t, …)` in `hb_arm64_codegen.c` is
  `block_count, flags, guest32_base, indirect_ic_guest_addr, indirect_ic_native_code, last_result,
  lazy_flags, pc, step_count` plus the regs union, and every one lies outside [640,2496) —
  `regs [32,432)`, `flags [432,438)`, `lazy_flags [440,504)`, `step_count [512,520)`,
  `block_count [528,536)`, `pc [544,552)`, `last_result [584,588)`, `guest32_base [2496,2504)`,
  `codegen_flags [2504,2508)`, `indirect_ic_* [2512,2528)`. `ymm_hi`/`zmm_hi`/`k` are referenced **0** times
  in the emitter, only from `hb_interpreter.c` and `hb_context.c`. Copy becomes
  `[0,640) + [2496,2616) = 760 bytes`.
- step: **★ MEASURED, and the pre-registered internal metric confirms it within one run:**

  | critical-thread self time | before (2616 B copy) | after (760 B) |
  |---|---|---|
  | `_platform_memmove` | 81 / 536 = **15.1 %** | 16 / 498 = **3.2 %** |
  | `run_jit_block_with_signal_guard` | 57 = 10.6 % | 39 = **7.8 %** |

  and the wall-clock, all a new best for this lane:

  | | `Restored language` | Mono phase |
  |---|---|---|
  | pre-import-fix | 828.704 s | 284.2 s |
  | post-import-fix (n=2) | 448.399 / 490.330 s | 150.5 / 135.5 s |
  | **+ snapshot (SNAPSHOT760)** | **369.009 s** | **109.7 s** |
  | project baseline | 593 ± 78 s | 212.6 s best / 237.7 s healthy max |

  Zero `HyperBridge run failed`, 758 M dispatches over 478 s. **369.0 s is 1.61× under the 593 s baseline
  mean and 2.25× under this lane's starting point**, and the Mono phase is now half the brief's best figure.
- step: **flipped the gate's default ON** (`MACRUNNER_HB_SNAPSHOT_SKIP_INTERP` 0 → 1) and redeployed —
  ntdll **3b9ab74bb6e9aef4**. The brief's rule is to move a default only after measuring, and this is that
  measurement. Stated honestly: the marker time is **n=1**, so the load-bearing evidence is the profile share
  (a within-run before/after on the exact item), not the clock; `=0` restores the full-struct copy with one
  env var.
### 2026-07-30 iteration 19 — clean baseline profile, and a correction to my own snapshot claim

- step: **SNAPSHOT760 triage: rung 11, `time_to_swapchain=114s`.**
- step: **[CORRECTION, mine] the snapshot fix's WALL-CLOCK claim does not survive n=2.** Last iteration I
  reported 369.009 s as a new best and "1.61× under the baseline mean". A second post-snapshot run
  (CLEANPROF, same deployed binary, instruments OFF) reached `Restored language` at **457.049 s**. So
  post-snapshot is 369.0 / 457.0 (mean 413 s) against post-import-fix 448.4 / 490.3 (mean 469 s) — an ~12 %
  difference that sits **well inside the documented ±78 s spread**. I did label the marker time n=1 and name
  the profile share as load-bearing, but I also wrote "1.61× under the baseline", and that comparison was not
  supportable. The defensible statement is: **the snapshot change is proven by the profile
  (`_platform_memmove` 15.1 % → 3.2 %/5.1 %), and its effect on time-to-menu is not yet resolvable against
  run-to-run variance.** Mono phase likewise: 109.7 s then 139.0 s.
- step: **★ clean critical-thread profile — instruments off, snapshot fix deployed, 534 samples:**

  | item | self | % |
  |---|---|---|
  | `hb_jit_runtime_run` | 91 | **17.0 %** |
  | `hb_flags_read_operand_value` | 41 | **7.7 %** |
  | `hb_memory_read` | 29 | 5.4 % |
  | `run_jit_block_with_signal_guard` | 27 | 5.1 % |
  | `_platform_memmove` | 27 | 5.1 % |
  | translated guest code (`???`) | 27 | 5.1 % |
  | `_tlv_get_addr` | 26 | 4.9 % |
  | `_platform_memset` | 20 | 3.7 % |
  | `find_region_normalized` | 16 | 3.0 % |
  | `hb_memory_write` | 11 | 2.1 % |
  | `hb_context_write_reg_value` / `hb_context_read_reg_value` | 10 / 10 | 1.9 % each |
  | `hb_jit_helper_exec_extend_operand_lazy` | 9 | 1.7 % |

  Groups: dispatch machinery **30.9 %**, MMU **10.5 %**, my own `dispatch_stats*` **0** — the profile is
  finally uncontaminated, which is why it was worth a run on its own.
- step: **the previous profile was ~5 % my own tooling, now quantified.** In the instrumented run
  `_tlv_get_addr` was 8.2 % spread over 22 callers, of which **11 of 41 samples came from
  `dispatch_stats_add` / `_register` / `_flush_thread`** — my dispatch-stats instrument — plus
  `dispatch_stats_add`'s own 2.4 % self. Clean, `_tlv_get_addr` falls to 4.9 % and is still spread across
  ~20 callers including a long tail of `macrunner_hb_*_probe_*_block` functions, so there is no single fix
  there; it is the cost of many per-block env-gated probes each touching thread-locals.
- step: **[NEXT TARGET, chosen on clean data] the lazy-flags machinery, ~13 % as a group.**
  `hb_flags_read_operand_value` 7.7 % + `hb_context_read_reg_value` 1.9 % + `hb_context_write_reg_value` 1.9 %
  + `hb_jit_helper_exec_extend_operand_lazy` 1.7 % = **13.2 %**, and it is reached from
  `hb_jit_helper_eval_cond_lazy`, the C helper the emitter calls for **every** `Jcc` —
  which this lane measured at **71.1 % of all dispatched terminals**. That is the same finding the terminal
  histogram produced, arriving from the other direction. `hb_jit_runtime_run`'s own 17.0 % self is larger as a
  single symbol, but it is the dispatcher loop itself, so reducing it means fewer dispatches — which is
  chaining, now parked.
### 2026-07-30 iteration 20 — 560 M helper operand reads, 58.8 % of them through the software MMU

- step: **CLEANPROF triage: rung 11, `time_to_swapchain=110s`.**
- step: **★ instrumented `hb_flags_read_operand_value` by operand type and ran it.** 560 000 001 calls by the
  last report on the critical thread:

  | operand type | count | share |
  |---|---|---|
  | register | 189 184 228 | 33.8 % |
  | immediate | 41 781 127 | 7.5 % |
  | **memory** | **329 034 646** | **58.8 %** |

  Stable across reports (36.9 % mem at 64 M calls, then 58.7/58.73/58.76 % from 552 M on). So the majority of
  these operand reads go through `mem_read_size` → `hb_memory_read` → `find_region_normalized`, which is why
  the MMU shows up at 10.5 % right beside this function's 7.7 %.
- step: **[CORRECTION, mine, before it propagates] these are JIT HELPER operand reads, not flag
  re-materialisation.** I introduced the counter under the heading "size the lazy-flags re-read" and then
  wrote that 329 M memory reads "exist purely to re-materialize x86 flags". Checking the callers instead of
  assuming: **every** caller of `hb_flags_read_operand_value` outside `hb_flags.c` is in
  `hb_arm64_codegen.c` at lines 5339, 5349, 5642, 5821, 5847, 5900, 5903, 6299, 6354, 6382, 6405, 6434 — the
  `hb_jit_helper_exec_*` family, i.e. the helpers emitted code calls when the JIT could **not** emit an
  operation natively. Some of those helpers are flag-related (`cmp`/`test`, and `hb_lazy_flags_note` already
  takes VALUES `lhs, rhs, result`, so materialisation does not re-read), but the family is much broader. The
  number is real; the label I first gave it was wrong.
- step: **so the finding is larger than the one I was chasing: a very large number of guest instructions are
  still executed by C helpers rather than emitted ARM64.** That is the same conclusion the brief anticipates
  ("if `exec_instr_unlocked` is on top, hot loops are still interpreted — dig there"), arriving via the helper
  path instead of the interpreter proper. It also explains the shape of the clean profile without needing
  chaining: `hb_flags_read_operand_value` 7.7 % + MMU 10.5 % + `hb_context_read/write_reg_value` 3.8 % +
  `hb_jit_helper_exec_extend_operand_lazy` 1.7 % are all the cost of *not* having emitted the instruction.
### 2026-07-30 iteration 21 — the helper-op coverage matrix, and the direct-mem lever

- step: **FLAGOPS triage: rung 11.**
- step: **★ built the ranked coverage matrix the bulk rule asks for: 152 000 001 helper invocations on the
  critical thread, by IR op.** Six ops are **99.3 %** of them:

  | op | count | share | name |
  |---|---|---|---|
  | 46 | 64 164 260 | **42.2 %** | `HB_IR_STORE` |
  | 79 | 20 584 762 | 13.5 % | `HB_IR_SIGN_EXTEND` |
  | 183 | 20 525 952 | 13.5 % | `HB_IR_MULSS` |
  | 191 | 20 430 848 | 13.4 % | `HB_IR_CVTTSS2SI` |
  | 45 | 19 723 877 | 13.0 % | `HB_IR_LOAD` |
  | 174 | 5 136 561 | 3.4 % | `HB_IR_FADD` |
  | 30 / 86 / 9 / 51 / 10 / 6 | 447 540 … 62 529 | ≤0.3 % each | `CMP`, `ZERO_EXTEND`, `IMUL`, `CALL`, `DIV`, `SUB` |

  **`LOAD` + `STORE` = 55.2 %**, which matches the independently measured 58.8 % of operand reads coming from
  guest memory.
- step: **[MY ERROR, caught by compile-time check] my first decoding of these op numbers was WRONG.** I parsed
  the enum out of `hb_ir.h` with a regex and reported the head as `PUSH 42.3 %`, `CMPS`, `COMISS`,
  `VEC_PACKED`. Verifying against the compiler instead — `HB_IR_STORE=46`, `PUSH=47`, `CMPS=82`, `COMISS=186`,
  `VEC_PACKED=194`, `FDIV=177` — showed my indices drifted by 1–3 because the regex skipped entries. Rebuilt
  the map by emitting one `printf` per enum name and compiling it (271 entries), which is what the table above
  uses. The counts were always right; the names were not, and a whole pass could have been spent emitting
  native code for `PUSH`, an op that never appears.
- step: **★ and the head lands on a lever the harness explicitly disables:**
  `hk-run-try12-config.sh:100` sets `MACRUNNER_HB_JIT_DIRECT_MEM` to **0** unless
  `MACRUNNER_HK_FORCE_JIT_DIRECT_MEM` is set, which is exactly why `LOAD`/`STORE` reach a C helper. Armed it
  (run DIRECTMEM, `direct_mem=1` echoed by the runner, 0 faults) and **four helper op classes went to exactly
  zero**: `SIGN_EXTEND` 20 584 762 → **0**, `MULSS` 20 525 952 → **0**, `CVTTSS2SI` 20 430 848 → **0**,
  `FADD` 5 136 561 → **0**. That is 46.3 M helper calls' worth of the baseline eliminated as a *class*, which
  is a qualitative result and not a rate.
- step: **[CONFOUND, stated rather than glossed] the LOAD/STORE totals are NOT comparable between the two
  runs.** Totals went 152 M → 424 M with `LOAD` 19.7 M → 225.2 M, which reads like a regression — but this run
  progressed **further in less time** (`UnloadTime` +177.059 s against +262.313 s, Mono phase 120.5 s), so it
  executed far more guest code, and these counters are cumulative. Comparing them directly would repeat the
  window/phase error this lane already made twice. **No direction is claimed for LOAD/STORE.**
### 2026-07-30 iteration 22 — normalised the metric, and it REFUTES the direct-mem lever

- step: **DIRECTMEM triage: rung 11, `time_to_swapchain=102s`.**
- step: **★ ran both arms with the helper histogram AND dispatch stats, so helper calls divide by dispatches
  and phase drops out of the comparison:**

  | | helper calls | dispatches | **helpers / 1000 dispatches** | per 1000 guest steps |
  |---|---|---|---|---|
  | direct_mem **OFF** (harness default) | 160 000 001 | 486 000 000 | **329.2** | 68.1 |
  | direct_mem **ON** | 352 000 001 | 504 000 000 | **698.4** | 144.3 |

  **Arming direct-mem is 112 % WORSE** on the normalised metric. Per op, per 1000 dispatches:

  | op | OFF | ON | |
  |---|---|---|---|
  | `LOAD` | 42.7 | **367.4** | 8.6× worse |
  | `STORE` | 139.0 | **203.7** | 1.5× worse |
  | `CMP` | 1.0 | 63.5 | worse |
  | `IMUL` | 0.4 | 26.8 | worse |
  | `SIGN_EXTEND` | 44.6 | **0** | eliminated |
  | `MULSS` | 44.5 | **0** | eliminated |
  | `CVTTSS2SI` | 44.2 | **0** | eliminated |
  | `FADD` | 11.1 | **0** | eliminated |

  plus several ops appearing only with it on (`op4` 6.1, `TEST` 4.3, `op19` 2.8, `op156` 2.6). So it trades
  four SSE/extend helper classes for a large increase in integer load/store/cmp helpers, and loses on net.
- step: **[REFUTES a standing expectation] the "direct-mem lever" is not a win here.**
  [[project_hk_directmem_lever_20260628]] records that JIT direct-mem is default-off "so loads/stores hit the
  interpreter lazy-helper", which reads as an opportunity. Measured on this workload it is the opposite: the
  harness's `MACRUNNER_HB_JIT_DIRECT_MEM=0` (`hk-run-try12-config.sh:100`) is the **better** setting, by 2.1×
  on helpers per dispatch. Leaving the default alone. The Mono phase happened to be shorter in the ON arm
  (193.1 s vs 223.3 s) but that is n=1 per arm inside a ±78 s spread and is **not** treated as evidence —
  the normalised counter is.
- step: **and that settles the confound from last iteration honestly.** I had refused to claim a direction
  from the raw totals (152 M → 424 M) because the runs reached different phases. Normalised, the direction is
  real and it is the one the raw numbers suggested — but the point stands that it could not be known until the
  denominator existed.
- step: **★ so the native-emission target list, in the configuration that actually ships (direct_mem OFF), is
  now fixed and ranked** — per 1000 dispatches: `STORE` **139.0** (42 % of all helper calls),
  `SIGN_EXTEND` 44.6, `MULSS` 44.5, `CVTTSS2SI` 44.2, `LOAD` 42.7, `FADD` 11.1. Those six are 326.1 of 329.2,
  i.e. **99 %** of helper traffic. This is the coverage matrix the bulk rule wanted, with a verified op
  numbering and a phase-independent unit.
### 2026-07-30 iteration 23 — the gate behind 55 % of helper traffic exists, and it is BROKEN

- step: **NORM-DMON triage: rung 11.**
- step: **★ found why `STORE` and `LOAD` reach a helper: a fifth default-off gate.** The emitter *has* a native
  path for both — `hb_arm64_codegen.c:4563` for STORE and the same predicate in the LOAD case — but it is
  guarded by `jit_native_mem_ir_enabled()`, i.e. **`MACRUNNER_HB_JIT_NATIVE_MEM_IR`, default 0**, and no script
  in the tree sets it (the only mention anywhere is `docs/ACTIVE-INVESTIGATION.md` recording a run with it
  `=0`). The sibling predicate `direct_user_mem_allowed` → `jit_direct_scalar_mem_enabled()` IS on by default,
  so this one flag is what routes `STORE` 139.0 + `LOAD` 42.7 = **181.7 of 329.2 per 1000 dispatches, 55 % of
  all helper traffic**, into C.
- step: **★ armed it, and it BREAKS THE GUEST: `exit=29`, `c000001d` (illegal instruction) at +52.3 s**, with
  `macrunner-hb-seh-nonmod-stk` dumps and **zero** of the three boot markers. So unlike the snapshot window,
  this is not a correct-but-unarmed optimisation — the native memory emission produces bad code on this
  workload. That is presumably why it has been off, and it means the 55 % cannot be reclaimed by flipping a
  flag; the emitter has to be fixed.
- step: **[CORRECTION, caught immediately] the per-op "→ 0.0" column in my own comparison was a division
  artifact, not a result.** With the run dying before any dispatch report, `dispatches=0` and
  `helper_calls=1`, so every `per-1000-dispatches` figure came out 0.0 — which reads exactly like "all helper
  traffic eliminated". It is nothing of the kind. Any normalised metric needs its denominator checked for zero
  before the ratio is believed, which is the same discipline the `avg_chain` denominator needed.
- step: **[UNIT SUITE IS CLEAN — the defect needs the real guest.]** `hb_test_runner`, 3 runs per arm (the
  suite is ASLR-flaky, so stable sets only): `NATIVE_MEM_IR=0` → 449 passed / 35 stable fails;
  `NATIVE_MEM_IR=1` → 447 passed / 35 stable fails; **failures unique to the ON arm: 0**. So the ISA-level
  tests do not cover whatever operand shape Mono emits that breaks. Exactly the pattern block chaining showed,
  and worth stating so the next attempt does not start by re-running the suite.
- step: **[STATE OF THE LEVERS, five now catalogued]** `BLOCK_CHAIN` — works, measured `avg_chain` 5.2–5.9,
  breaks the guest two ways, parked. `INDIRECT_IC` — still never armed. `SNAPSHOT_SKIP_INTERP` — correct and
  unarmed, now widened and **default ON** (this lane's second win). `JIT_DIRECT_MEM` — armed and measured
  **worse** (698.4 vs 329.2 helpers/1000 dispatches), default correctly off. `JIT_NATIVE_MEM_IR` — the big one
  at 55 % of helper traffic, and **broken** (`c000001d`).
### 2026-07-30 iteration 24 — a real RIP defect found and fixed; it is NOT the blocker

- step: **NATIVEMEMIR triage: `GENERIC_INVALID_DISPOSITION_SEH`, rung 11.**
- step: **ruled out the unpatched-branch theory first.** `emit_b_deferred` emits `B #0` and
  `emit_bcond_deferred` a conditional to itself, so an unpatched placeholder would **hang**, not raise
  `c000001d`. (Worth noting anyway: `emit_direct_mem_store_from_x20_tso` patches its second branch only
  `if (done_branch)`, and `emit_b_deferred` returns `buf->size` — so offset 0 would silently skip the patch.
  Unreachable here because every block starts with a prologue, but it is a latent trap.) And the SEH dump
  bytes are x86 (`48 89 85 …`), so the illegal instruction is the **guest** executing garbage after
  corruption, not a malformed ARM64 stream.
- step: **★ found a genuine, statically-proven defect: RIP-relative operands took the direct path and used a
  STALE RIP.** `is_direct_user_mem_operand` admits any base below `HB_REG_XMM0`, and compile-time values are
  `HB_REG_RIP=16`, `HB_REG_XMM0=17` — so RIP passed and reached `emit_direct_mem_addr`, which has no RIP case
  and emitted `ldr x21, [x19, #reg_off(RIP)]`, reading `ctx->regs.x64.rip` as an ordinary GPR. That field is
  synced only at block boundaries (`sync_arch_pc_after_jit_block`), so mid-block it is stale, whereas x86
  RIP-relative addressing is defined against the next instruction's address — which the sibling
  `emit_direct_mem_addr_for_instr` computes correctly and which only **2 of 21** call sites use. Since
  RIP-relative access is how x86-64 reaches globals, this would fire constantly. Excluded RIP (base and index)
  from the direct path so those operands fall back to the helper; correct by construction and kept regardless
  of what follows.
- step: **★ REFUTED as the blocker: the run still dies identically.** NATIVEMEM-NORIP: `exit=29`, `c000001d`
  present, **no** boot markers, no dispatch report — indistinguishable from before the fix. So the native
  memory path has at least one more hole; RIP was real but not the one that kills the boot.
- step: **[PATTERN, worth naming] four times now a statically-proven defect has turned out not to be the
  blocker** — the mid-block epilogue chain slot, the eviction literal store outside the W^X bracket, the
  unbracketed trampoline build (that one WAS a blocker), and now RIP-as-GPR. Reading code finds real bugs
  here; it has a poor record at finding *the* bug. What has worked every time is bisection with the guest as
  oracle: cap the mechanism, halve the population, compare arms.
### 2026-07-31 iteration 25 — ★ SHAPE BISECT LOCALISES IT: sub-64-bit memory operands

- step: **NATIVEMEM-NORIP triage: `GENERIC_INVALID_DISPOSITION_SEH`, rung 11.**
- step: **added `MACRUNNER_HB_JIT_NATIVE_MEM_SHAPE` (0 = unrestricted, default) and walked the levels.**
  Three runs, one per level:

  | arm | boots? | `c000001d` | dispatches |
  |---|---|---|---|
  | SHAPE=1 — 64-bit, **no index**, disp ∈ [0,4096) | **yes** | 0 | 580 000 000 |
  | SHAPE=2 — + index register | **yes** | 0 | 576 000 000 |
  | SHAPE=3 — + arbitrary displacement | **yes** | 0 | 564 000 000 |
  | SHAPE=0/4 — unrestricted, i.e. **+ sub-64-bit sizes** | **NO** | 1 | 0 |

  All three restricted arms reach `Begin MonoManager` and `UnloadTime` and end on the timeout (`exit=124`)
  with zero illegal instructions. The single difference between SHAPE=3 and unrestricted is the
  `op->size != HB_SIZE_64` clause. **So the breaking shape is 8/16/32-bit memory operands**, and index
  registers and large displacements are innocent — the opposite of where the XMM path's `x22` comment pointed.
- step: **this is what the bisect was for.** Four rounds of code-reading each produced a real defect that was
  not the blocker; three runs of population-halving localised it. The mechanism now has an obvious shape:
  a narrow guest store must write only its low bytes and a narrow load must extend correctly, so
  `emit_direct_mem_store_from_x20(buf, dst->size)` and its load counterpart mishandling those sizes would put
  full 64-bit values into memory — writes landing wrong, which is exactly "guest executes garbage → c000001d".
  **[HYPOTHESIS]** until the emitted narrow-size code is read; the localisation itself is measured.
- step: **[CAVEAT on the helper metric from this bisect] my shape gate is broader than the NATIVE_MEM_IR path.**
  I put the filter in `direct_user_mem_allowed`, which has **32 call sites** — so SHAPE=1 narrowed every op
  that uses it, not just LOAD/STORE, which is why helper traffic rose 329.2 → 924.1 per 1000 dispatches with
  `LOAD` 42.7 → 427.4, `CMP` 1.0 → 107.5, `ZERO_EXTEND` 0.5 → 87.1. That rise is an artifact of my own gate's
  breadth and says nothing about NATIVE_MEM_IR's value. The crash/no-crash result is unaffected by it, which
  is why the bisect still works.
### 2026-07-31 iteration 26 — narrowed to the narrow-size native STORE; two more hypotheses refuted

- step: **SHAPE3 triage: rung 11.**
- step: **[REFUTED] "narrow stores emit a full 64-bit STR".** `emit_stlr_from_reg` honours the size exactly:
  `STLRB Wt` / `STLRH Wt` / `STLR Wt` / `STLR Xt` for 8/16/32/64. Narrow stores write only their own bytes.
- step: **[REFUTED] "narrow loads zero the destination's upper bits".** `emit_store_x20_to_gpr_sized` is also
  correct: `STRB W20` for 8-bit and `STRH W20` for 16-bit (both at `reg_off + reg_offset`, so AH-style
  sub-registers are handled), and 32-bit zero-extends via `UBFM` + `STR X`, which is exactly x86's 32-bit
  write semantics. So neither side's sized emission is naively wrong.
- step: **★ [RE-FRAMES the previous iteration's bisect] 64-bit LOADs have their OWN extra gate.** The LOAD case
  requires `src1.size != HB_SIZE_64 || jit_native_mem_ir_qword_loads_enabled()`, and
  `MACRUNNER_HB_JIT_NATIVE_MEM_IR_QWORD_LOADS` is **also default 0**. So in the SHAPE=1..3 arms 64-bit loads
  still went to the helper and those arms were effectively testing native **stores**. "Sub-64-bit breaks it"
  was therefore ambiguous between narrowness and the load side — which is exactly why the next cut had to be
  by side, not by size.
- step: **★ SIDE bisect settles it.** Added `MACRUNNER_HB_JIT_NATIVE_MEM_SIDE` (0 both, 1 stores only,
  2 loads only) and ran stores-only with sizes unrestricted:

  | arm | sizes | sides | boots? | `c000001d` |
  |---|---|---|---|---|
  | SHAPE=3 | 64-bit only | both | **yes** | 0 |
  | **SIDE=1** | **all** | **stores only** | **NO** | **1** |
  | unrestricted | all | both | no | 1 |
  | baseline (`NATIVE_MEM_IR=0`) | — | — | yes | 0 |

  Stores alone reproduce the crash, so **the defect is in the narrow-size (8/16/32-bit) native STORE path**,
  and the load side is exonerated. Two runs, no code-reading required for the conclusion.
- step: **[TALLY] six code-reading hypotheses refuted on this defect** (unpatched deferred branch, RIP-as-GPR —
  a real bug but not the blocker, full-64-bit narrow store, narrow-load upper-bit clobber, plus the two
  earlier chaining ones). Every localisation that stuck came from bisection. Recording this because it is now
  a reliable prior for this codebase, not an impression.
### 2026-07-31 iteration 27 — ★ FULLY LOCALISED: the inline BYTE store, address path exonerated

- step: **SIDE-STORE triage: `GENERIC_INVALID_DISPOSITION_SEH`, rung 11.**
- step: **★ width bisect (stores only): it is 8-bit.**

  | arm | boots? | `c000001d` | dispatches |
  |---|---|---|---|
  | stores, 64-bit only | yes | 0 | 564 000 000 |
  | stores, **everything except 8-bit** | **yes** | 0 | 364 000 000 |
  | stores, all widths (8-bit included) | **NO** | 1 | 0 |

  Excluding only `HB_SIZE_8` is enough to boot, so **byte stores are the defect**.
- step: **[REFUTED] "the 8-bit source register's `reg_offset` is ignored" (AH/BH/CH/DH).**
  `emit_load_gpr_sized_to_reg` computes `off = reg_off(op->reg) + op->reg_offset` and emits `LDRB` from there,
  exactly as the writeback side does. Seventh code-reading hypothesis, seventh refutation.
- step: **★ and the last cut separates the address from the store, using a diagnostic that already existed.**
  `MACRUNNER_HB_FORCE_LAZY_STORE=1` keeps `emit_direct_mem_addr` inline but routes the store itself through
  `hb_jit_helper_store_sized`:

  | arm | address | store | boots? | `c000001d` |
  |---|---|---|---|---|
  | all widths | inline | **inline STLR** | **no** | 1 |
  | all widths | inline | **helper** | **yes** (552 M dispatches, `Begin MonoManager` + `UnloadTime`) | 0 |

  So **the inline address computation is correct** — it is used in the arm that boots — and the **inline byte
  store instruction is what breaks the guest**. The defect is not arithmetic and not the encoding
  (`STLRB Wt,[Xn]` = `0x089ffc00|rn<<5|rt`, verified); it is that the inline store skips whatever
  `hb_jit_helper_store_sized` → `write_u64_tso` does around the write.
- step: **[HYPOTHESIS, now targeted rather than speculative] the helper performs bookkeeping the raw STLR
  cannot: SMC / translation-cache invalidation on writes into pages holding translated code**, and Mono's
  code patching and write barriers are exactly byte-store traffic — which would explain why only this width
  shows it while aligned 16/32/64-bit inline stores are fine. That is now a read of ONE function
  (`hb_jit_helper_store_sized`/`write_u64_tso`) with a named question, not a hunt.
- step: **[SHIPPABLE SUBSET IDENTIFIED] `NATIVE_MEM_IR=1` with 8-bit stores excluded boots** (364 M dispatches,
  no illegal instruction). Its speed value is still **unmeasured**, because the shape gate I used sits in
  `direct_user_mem_allowed` (32 call sites) and inflates the helper metric; measuring it needs the exclusion
  moved into the STORE case itself. Nothing is being defaulted on until that number exists.
- step: **[TALLY] seven code-reading hypotheses refuted; five bisection arms, five decisive answers.** The
  defect went from "arming a flag kills the boot" to "the inline byte store, address path fine" in five runs
  without reading the fix.
- step: **[SUPERSEDED] bisect WHICH narrow store size breaks.** Done: 8-bit.

### 2026-07-31 iteration 28 — the byte-store fix works on its own terms; my "loads exonerated" was a logic error

- step: **STORE-LAZY triage: rung 11.**
- step: **read what the helper does that the raw store does not.** `hb_jit_helper_store_sized` dispatches to
  `hb_jit_helper_write_u8_tso`, which calls `hb_memory_host_ptr(ctx->memory, addr, size, HB_PERM_WRITE)` — a
  guest→host translation **with a permission check** — then an Apple-specific `hb_jit_live_host_ptr` fallback,
  and finally the full software `hb_memory_write_u8`. The inline path does `STLRB W20,[X21]` where on x64 x21 is
  the guest address itself (`emit_x86_ea_to_host` returns immediately unless `arch == HB_ARCH_X86`), so it works
  only because guest VA == host VA and it performs **no permission check and no software fallback at all**.
  Byte stores are the only width that can never reach that helper, since their alignment mask is 0.
- step: **applied the exclusion and it does exactly what it should to store traffic:**
  `STORE` **139.0 → 39.5** per 1000 dispatches (−72 %), with `MULSS`/`CVTTSS2SI`/`FADD` still eliminated. The
  native store path demonstrably works for 16/32/64-bit.
- step: **★ but the run still crashes, and that exposes a LOGIC ERROR in my previous entry.** I wrote that the
  side bisect "exonerated" loads. It did not: `SIDE=1` (stores only) crashing shows stores are **sufficient** to
  crash, not that loads are safe. The arm I called proof-of-boot (SHAPE=6) *also* carried `SIDE=1`, so it had
  the native LOAD path switched off entirely — its success was "native stores, no bytes, **no loads**". With
  loads re-enabled and bytes excluded, the guest dies again (`exit=29`, `c000001d`, `Begin MonoManager` only,
  324 M dispatches). Sufficient ≠ necessary, and I conflated them.
- step: **[NEXT STEP] the arm I never ran: `SIDE=2`, loads only.** That tests the load path independently and is
  the missing cell of the table — stores-only crashes, stores-only-minus-bytes plus loads crashes, and
  stores-only-minus-bytes-without-loads boots, so loads are now the prime remaining suspect. One 300 s run.
  If loads alone crash, the native memory path has two independent defects and the honest summary is that
  `NATIVE_MEM_IR` needs the load side fixed too before any of the 55 % is reclaimable.
### 2026-07-31 iteration 29 — NATIVE_MEM_IR measured to exhaustion: the safe subset is worth 2.2 %, not 55 %

- step: **NATIVEMEM-PRED triage: rung 11.**
- step: **★ ran the missing cell, and the map is now complete — each side is safe ALONE, the pair is not:**

  | arm | boots? | `c000001d` | helpers/1000 disp |
  |---|---|---|---|
  | baseline, `NATIVE_MEM_IR` off | yes | 0 | 329.2 |
  | stores only, all widths | **no** | 1 | — |
  | stores only, no bytes | yes | 0 | — |
  | **loads only, no bytes** | **yes** | 0 | 338.0 |
  | **both sides, no bytes** | **no** | 1 | 790.1 |

  So this is an **interaction**, not a per-side defect: stores-minus-bytes boots, loads-minus-bytes boots, and
  enabling both together crashes. That is the shape of shared state — x20/x21/x22 or deferred lazy-flag
  materialisation that each side relies on the *other's* helper path to perform. I am not chasing it by reading
  after seven refutations; it is recorded as the open question.
- step: **★ and the decisive number: the best SAFE configuration is worth 2.2 %.** Arming the load side with
  byte operands excluded and `MACRUNNER_HB_JIT_NATIVE_MEM_IR_QWORD_LOADS=1` (a *further* never-armed gate,
  found while reading the LOAD case) boots cleanly and gives:

  | | total | `LOAD` | `STORE` |
  |---|---|---|---|
  | baseline | 329.2 | 42.7 | 139.0 |
  | loads only, no bytes, qword loads ON | **322.1** | **34.3** | 139.3 |

  −2.2 % total, `LOAD` −20 %. Helper traffic is ~13 % of the critical thread, so that is roughly **0.3 % of
  thread time** — measurable, and far too small to justify defaulting a gate that crashes in three of its five
  configurations.
- step: **[CONCLUSION on this avenue, stated plainly] the "55 % of helper traffic" was addressable only on
  paper.** The histogram was right that `LOAD`+`STORE` are 55 % of helper calls; what the runs establish is that
  the emitter can absorb only a fraction of them — byte operands must stay on the helper (no permission check
  or software fallback exists on the inline path), and the two sides cannot both be native. So the honest
  ranking of this lane's remaining levers does **not** include NATIVE_MEM_IR until the two-sided interaction is
  fixed. `MACRUNNER_HB_JIT_NATIVE_MEM_IR` stays default-off; the byte guard and the diagnostic gates stay in.
- step: **[NEXT] back to the clean profile, which is where the remaining size is.** Critical-thread self time
  with instruments off: `hb_jit_runtime_run` **17.0 %** (the dispatcher loop itself — reducing it means fewer
  dispatches, i.e. chaining, parked), the guest-MMU group `find_region_normalized` + `hb_memory_read` +
  `hb_memory_write` = **10.5 %**, `hb_flags_read_operand_value` 7.7 %, `_tlv_get_addr` 4.9 %. The MMU group is
  the largest *actionable* item left: `find_region_normalized` already has a treap plus a per-thread MRU hot
  cache, so the question to measure first is that cache's **hit rate** and how often `hot_cache_reset_tls`
  wipes it (it clears every slot whenever the region epoch changes, and Mono maps/unmaps constantly). One
  gated counter, one run — the same shape as every measurement that has worked in this lane.
- step: **[KEEPING] the byte-store guard and its comment stay in** — the mechanism is measured (139.0 → 39.5,
  and `FORCE_LAZY_STORE` booting proves the address arithmetic is fine), the reasoning is written down, and
  `NATIVE_MEM_IR` remains default-off so the tree's shipping behaviour is unchanged either way. Extend the shape levels to admit 64+32, then
  64+32+16, with `SIDE=1` to keep loads out of it: the first level that crashes names the width. `HB_SIZE_8`
  is the prime suspect on structural grounds — `direct_mem_alignment_mask(HB_SIZE_8)` returns 0, so byte
  stores are the ONLY size with no alignment check and no helper fallback path emitted at all, meaning they
  take the inline `STLRB` unconditionally. That is a hypothesis; the run decides it.
- step: **[SUPERSEDED] read the narrow-size store/load emission, fix it, then measure unrestricted.** Read:
  both sides' sized emission is correct, so the defect is elsewhere in the narrow store path. Two
  concrete things: `emit_direct_mem_store_from_x20(buf, size)` for `HB_SIZE_8/16/32` (does it use `STRB/STRH/STR
  W` or a full 64-bit `STR`?), and the load side's extension. With that fixed, run NATIVE_MEM_IR=1 with SHAPE=0
  and compare helpers/1000 dispatches against the 329.2 baseline — that is the number that decides whether the
  55 % of helper traffic is actually reclaimable. SHAPE=3 is meanwhile a known-good subset, though its value is
  unmeasured because of the gate-breadth caveat above.
- step: **[SUPERSEDED] bisect the operand SHAPES instead of hunting more defects.** Add a gate that admits the
  native path only for the narrowest useful shape — `size == HB_SIZE_64`, no index register, displacement in
  `[0,4096)` — and widen in steps (add index, add large disp, add sub-register sizes). Each arm is one 300 s
  run with the same normalised counter, and the first arm that survives gives both a working subset and, by
  difference, the shape that breaks. That converts an open-ended code hunt into a bounded search of maybe four
  arms, the technique that localised chaining to a single edge in two runs.
- step: **[SUPERSEDED] find the illegal instruction the native memory path emits.** Cheapest route, no full
  run: compile a block through the emitter with `NATIVE_MEM_IR=1` for the STORE/LOAD operand shapes the suite
  does not cover (indexed, large displacement, sub-register sizes, the `x22`-clobber case the XMM path already
  documents at `:4548`) and disassemble the output, comparing against the helper path's semantics. The
  `emit_direct_mem_store_from_x20_tso` / `emit_direct_mem_addr` pair is where to look, and the XMM store
  immediately above it already carries a comment about a real corruption bug from clobbering `x22` — a strong
  hint that the GPR path has an analogous hole.
- step: **[SUPERSEDED] emit `HB_IR_STORE` natively for the common shapes.** It is single-handedly 42 % of helper
  calls, and the emitter already has a native path for other memory forms
  (`MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM`/`_SCAN` are on by default), so the first question is *why* STORE falls
  through — read the `HB_IR_STORE` case in `hb_arm64_codegen.c` and identify which operand shapes it declines.
  Then the same normalised counter re-run measures the coverage shift directly, with no cross-run clock claim
  needed.
- step: **[SUPERSEDED] normalise before judging: re-run both arms with `MACRUNNER_HB_TRACE_DISPATCH_STATS=1`**
  so helper calls can be divided by dispatches, making the metric phase-independent — the same fix that turned
  `avg_chain` from a comparison into a single-run number. Then the direct-mem default is decidable on evidence,
  and the remaining helper head (`LOAD`/`STORE` under direct-mem) is the native-emission target.
- step: **[SUPERSEDED] count helper invocations by IR op,
  then emit the top ones natively.** The set is finite and externally specified — 163 ops in `hb_ir.h` — and a
  reference implementation exists (the interpreter implements all of them), which is exactly the condition the
  project's bulk rule names. One gated histogram keyed by `instr->op` inside the helper entry points gives the
  ranking from a single run; the top few ops then get native emission, and the same counter re-run measures the
  coverage shift. That is a coverage matrix, not a per-failure fix.
- step: **[SUPERSEDED] read what `hb_flags_read_operand_value` actually does per call before touching it**, and
  check whether the emitter can evaluate common conditions (ZF/SF from a preceding CMP/TEST) inline in ARM64
  instead of calling into C. Observation first: count how many Jcc helper calls occur per dispatch and which
  `cc` codes dominate, which is one gated counter and one run.
- step: **[SUPERSEDED] the profile has moved again, so re-read it before choosing.** Done, on clean data. After this change the
  critical thread is `hb_jit_runtime_run` 12.7 %, **`_tlv_get_addr` 8.2 %**, the guard 7.8 %,
  `find_region_normalized` 6.8 %, `hb_memory_read` 6.4 %, translated guest code 5.0 %,
  `hb_flags_read_operand_value` 4.4 %, `_platform_memmove` 3.2 %, and `dispatch_stats_add` 2.4 % (my own
  instrument — worth disabling for a clean baseline run). `_tlv_get_addr` is now the second item and is pure
  overhead: thread-local accesses in a dylib going through the lazy accessor. The guest MMU pair
  (`find_region_normalized` + `hb_memory_read` = 13.2 %) is the largest *group*.
- step: **[SUPERSEDED] the per-dispatch context snapshot.** Done, and it is the lane's second delivered win. `hb_ctx_snapshot_save` copies
  `sizeof(hb_context_t) - HB_CTX_INTERP_ONLY_END` = 1592 bytes on **every** dispatch, purely so a fault can
  restore and re-run the block in the interpreter. Two observations first, no code change: how many bytes of
  that are guest-visible state the fault path actually needs, and how often a fault actually consumes the
  snapshot (the brief records the critical thread taking **zero** faults, which would make it pure waste on
  that thread). Then narrow the copy behind a gate and A/B on the marker, which is the pattern that worked
  for the import scan.
- step: **[SUPERSEDED] bisect INSIDE that one chained run.** The refuse-near arm answered the question the
  bisect was meant to answer. It is `from=0x87ef246adb6`, 2 blocks, 71 steps —
  suspiciously many instructions for two blocks, and its `blocks=2` comes from the emitted counter rather than
  from anything I have verified. Two cheap observations: (a) log each block boundary within a chained run for
  that one edge, so the intermediate block is named; (b) refuse to chain from that single predecessor and see
  whether the fault vanishes or merely relocates — if it relocates to another chained edge, the defect is
  general to chaining rather than to this block pair. Both are single 220 s runs.
- step: **[SUPERSEDED] dump guest registers immediately before and after the specific chained transition into
  `0x87ef2469a30`.** Done, and it produced the proof above. `chain_edge_is_near` already isolates that address, and `trace_chain_transition` already
  fires on `block_delta > 1`; extending it to print `rcx`/`rdx` (the two registers that are identically
  garbage in every failing run) for that one edge says whether the chain corrupts them or inherits them
  already corrupt. That is one 220 s run and it distinguishes "chaining breaks this block" from "chaining
  merely reaches a block that was already going to fault".
- step: **[SUPERSEDED] name the faulting helper and the address it touched.** Done: `mono-2.0-bdwgc.dll+0x69a30`,
  fault on its first memory access, reached by a chain from three predecessors.
  `MACRUNNER_HB_TRACE_JIT_HELPER_FAIL=1` (`hb_runtime.c:2193`) prints the helper-fault block and pc via
  `trace_jit_helper_fault_block`, and `set_helper_fault_result` is what turned this into `MEMORY_FAULT`. Run
  that with chaining armed AND `err+all`, plus the same run with `MACRUNNER_HB_CHAIN_PATCH=0` as the control
  — two 200 s runs that together say whether the helper fault exists without chaining. Until that control
  exists I am not attributing the fault to chaining, only observing that it appears with it.
- step: **[SUPERSEDED] find what maps a runtime failure to `c000007b` on this path, then gate it.** Done: it
  maps everything, which is why it was the wrong thing to chase. Grep the
  `macrunner-hb-run-exit` emitter for how `status` is derived, identify the validator or IAT check behind it,
  and if it has a kill switch, run chaining with it relaxed. That is a static read plus one 220 s run, and
  unlike the last three attempts it starts from a named status code and a named module rather than a guess.
- step: **[SUPERSEDED] use the documented instrument for this exact symptom, not a new guess.**
  `WAIT_WAKE_TRACE` + `ARM_FILE` per that note, on a chaining run, and read whether a wake is lost
  (`equal=0`). The note also warns that `TRACE_WAITADDR` logs only waits `n<64 || n%8192` so it can never
  show the final wait — so `WAIT_WAKE_TRACE` is the one to use. One 220 s run, and the wedge is
  deterministic at ~+53 s so the window is known.
- step: **[SUPERSEDED] sample with the matcher that has actually worked here, and verify the target before
  trusting the output.** Done, and it produced the finding above. `ps -Ao pid,comm | grep -i hollow` is what successfully found the guest twice today
  (pids 10832 and 75046); `comm` is the basename, so it cannot match a wrapper's arguments. Then check the
  sample's own `Path:` line names the game executable BEFORE reading a single count — that one check is what
  caught this, and it is cheap enough to be unconditional.
- step: **[SUPERSEDED] gate the unchain machinery, not the chaining.** Add a switch that keeps `chain_meta`
  allocated (so chaining still works) but skips the eviction-time unchain writes; with the trampoline design
  those writes are arguably redundant anyway, since eviction is supposed to be one store to the trampoline
  literal rather than a rewrite of predecessor tails. If that boots with chaining armed, the defect is in the
  unchain path and the trampoline design's whole premise — "eviction becomes ONE 8-byte store" — is what
  should replace it. One 200 s run.
- step: **[SUPERSEDED] observe the guest state ACROSS one chained transition.** With `cap=1` the fatal edge
  is known and reproducible, so the missing instrument is a dispatcher-side trace fired when
  `run_block_delta > 1`, printing `ctx->pc`, the predecessor's `guest_addr`, and the expected successor. If
  the PC after a chained run is not a sane guest address, the trampoline is corrupting control flow (and the
  encodings being clang-verified would then point at the *entry offset* or the frame contract, not the
  bytes); if the PC is sane, the loss is on the host side — the per-dispatch work that a chained transition
  skips. One 200 s run, and `avg_chain` already proves the delta>1 path is taken.
- step: **[SUPERSEDED] stop sniffing bytes — record the slot at emit time.** Done differently and better:
  the predicate now recognises both shapes, so no emit-time bookkeeping was needed at all. `entry_has_chain_slot` should
  not infer a chain slot from instruction bytes that chaining itself overwrites; the offset is known when
  the block is emitted and belongs in the cache entry or in `hb_block_chain_meta`. That one change fixes
  the blindness and the `slot_next` self-throttling together, and only then does `avg_chain` become
  readable — so the payoff of chaining is still **unmeasured**, and no claim about its speed is made here.
- step: **[SUPERSEDED] the earlier next-step note below** — the emit-vs-patch split it proposed is what
  produced this iteration's result.
- step: **[NEXT STEP, named and cheap] split the chain flag into emit-side vs patch-side.** Arming
  chaining changes three things at once: `emit_block_chain_slot` (4 NOPs) and
  `emit_block_counter_accounting` (7 instrs) at emit time, and `patch_block_tail` at run time. A gate that
  keeps the emitted shape but skips the patching separates "the emitted block is wrong" from "modifying it
  at run time is wrong" in ONE 45-second run, because these deaths are fast. That is the next bisect, and
  it needs no new instrument. Note the encodings are verified against
  clang and the chain entry `native_code + 12` is verified against the prologue, so the suspects are
  elsewhere: the eviction/`meta->target_code` bookkeeping, or the interaction with
  `emit_block_counter_accounting`'s use of X20/X21, or `patch_block_tail` recording a successor whose
  entry is later replaced. `hb_test_runner.c:15681` is the entry point for finding out, and it costs
  seconds per attempt.

## 2026-07-31 — the guest MMU: the misses are *absences*, and the cache was keyed on a reusable address

Entered this pass with the MMU group as the largest actionable item in the clean profile (10.5%) and one
question: is `find_region_normalized`'s per-thread MRU cache being wiped by epoch churn? Mono maps and unmaps
constantly, and `hot_cache_reset_tls` clears every slot on an epoch change, so a high reset rate would mean the
cache is permanently cold no matter how good its locality is.

**Epoch churn: REFUTED, first measurement.** 808 M lookups, 67.7 % hits, **0.04 resets per 1 k lookups**
(32 621 resets total). The wipe is a non-event.

**Capacity: REFUTED, second measurement.** Extended the instrument with a hit-depth histogram and a 64-deep
true-LRU *shadow* of the same reference stream, which prices any cache size without rebuilding. A 32-slot cache
would reach **76.04 %** against the real 75.82 %; 64 slots, 76.12 %. More slots buy **0.3 points**. `avg_hit_depth`
is 4.11 with a near-flat tail (d0 52 %, then d1–d15 each 10–33 M), so the 16-wide scan is genuinely used and
cannot simply be shortened either.

**What the misses actually are.** 97.0–97.5 % of misses (replicated over three runs) resolve to **NULL** — the
treap contains no region for that address. ~166 M lookups per run, ~25 % of all lookups, each paying the full
16-slot scan *and* a failed treap walk to return nothing. No positive cache of any size can answer that,
because the thing worth caching is the *absence*.

**Where they live.** First sampler was first-come-first-served over 20 buckets: twenty cold startup addresses
took the table and 99.96 % of traffic fell into `other`. That measured nothing — replaced with Misra-Gries,
which guarantees any bucket above 1/32 of the stream survives. Result: `evicted=61` over 166 M events, i.e. the
whole stream fits in ~32 buckets of 16 MB — two clusters (`0x113–0x114`, `0x47b–0x4e6`), top-6 = 50 %. Live
Mono/Unity data the region list simply never had entries for.

**The fix, and why it is sound.** A failed treap walk already computes the exact answer for free: the last node
we turned left at is the successor, the last we turned right at is the predecessor, so `[lo,hi)` is a genuine
region-free gap (regions are non-overlapping, treap keyed by base). One entry covers a whole gap, so 8 slots
cover a stream spread over ~32 buckets. Gated `MACRUNNER_HB_NO_NEG_CACHE`.

Soundness needed one thing that was NOT true: **region adds never bumped `hot_gen`.** Only remove/split/rebuild
did. For the positive cache that was fine — every add is guarded by `any_overlap`, so an add can never
invalidate a cached region — but an add is *precisely* the event that turns "no region contains addr" into a
lie. `insert_region_head` now calls `clear_hot_cache`. Cost measured, not assumed: flushes went 33 k → 72 650,
still 0.1 per 1 k lookups.

**The bug this surfaced (independent of the perf work).** The unit suite showed one stable regression under my
gate: `hb_test_runner.c:19370`. Cause is not the gap logic — the TLS caches key validity on
`(mem pointer, hot_gen)`, `calloc` starts `hot_gen` at 0, so a destroyed `hb_memory_t` whose address malloc
later hands back produces a NEW instance that a stale TLS table matches exactly. Classic ABA. **This hazard
already existed for the positive cache, where the stale entries are pointers to regions freed with the previous
instance** — a use-after-free that only needed the right allocation pattern. `hb_memory_create` now seeds
`hot_gen` from a global counter (`<< 32` stride), so no reused address can alias. Re-ran 3+3 arms: stable
regressions **NONE**; 19370 now fails only in the *off* arm, i.e. it is back to being ASLR noise.

**Effect, measured on a live boot:** `gap_hits=154 984 532` against `miss_null=155 889 612` — **99.4 % of the
null stream now skips the treap walk**, 905 080 fills. Scale of what that removes: the run's own rebuild
counters (`rebuild_nodes=823871 / rebuilds=34`) put the tree at **~24 000 live regions**, so each avoided walk
is ~15–20 dependent, cache-cold pointer loads, replaced by an 8-entry MRU scan that hits entry 0–1 in one cache
line.

**Not yet established: wall-clock.** The instruments-off sample lands at t≈153 s (the sleep started before wine
launched), not the ~240 s of the clean baseline, and `find_region_normalized` is now a thin wrapper so the work
reports under `find_region_impl` — two reasons the 4.5 % vs baseline 3.0 % comparison is not decisive either
way. Critical thread at t≈153 s (99 % non-idle): `hb_jit_runtime_run` 16.4 %, `hb_memory_read` 7.9 %,
`_tlv_get_addr` 7.0 %, `hb_flags_read_operand_value` 6.6 %, guest 4.7 %, guard 4.6 %, `find_region_impl` 4.5 %,
`_platform_memmove` 4.4 %, MMU group 16.6 %.

[NEXT] the decisive A/B, which this pass deliberately did not fake: same build, `MACRUNNER_HB_NO_NEG_CACHE=1`
vs default, both sampled at the SAME guest-relative offset (anchor the sleep on the guest pid appearing, not on
run start), plus a nodes-visited-per-failed-walk counter so the saving is expressed in dependent loads rather
than inferred from region count.

## 2026-07-31 (later) — the negative cache is CORRECT and measured SLOWER; default flipped to opt-in

Promised the decisive A/B last pass and ran it. Both arms same build, same 300 s budget, sampler anchored on
the GUEST's launch rather than run start, so both sampled at **guest+164 s** exactly (02:19:09→02:21:53 and
02:25:51→02:28:35).

**Scale of the mechanism, measured in-run first.** `nodes_per_failed_walk=17.2` over 158 308 936 failed walks
on the busiest thread — **2.72 billion dependent, cache-cold pointer loads per run**, of which the gap cache
provably removes 99.4 %. (`nodes_per_ok_walk=15.4`.) So the thing being removed is real and large.

**The A/B says it is still a loss:**

| | ON | OFF | Δ |
|---|---|---|---|
| `find_region*` self | 4.05 % | 1.96 % | **+2.09 pts** |
| MMU group | 13.90 % | 12.09 % | +1.81 pts |
| `hb_memory_read` | 6.73 % | 6.71 % | +0.02 |
| dispatches at 300 s | 532 M | 560 M | **−5 %** |

Two independent signals agreeing, n=1 per arm. Not proof, but it is not a win, so it does not ship on.

**Why — and it is my own code, not the idea.** `hot_cache_epoch` is an `__atomic_load_n(ACQUIRE)` on
`mem->hot_gen`, a line every one of 54 threads reads on every lookup. I added a SECOND acquire load per miss
(`gap_lookup`), two more per fill, and — worse — made every region add **write** that line via
`clear_hot_cache`, so the line ping-pongs across cores exactly when Mono is mapping hardest. Plausibly enough
to eat a 17-node walk saving.

Fixed by splitting what the two caches actually depend on: **an add can never invalidate a cached region**
(guarded by `any_overlap`) and **a remove/split can never invalidate a cached gap** (it only makes more of the
space region-free). So `hot_gen` keeps its original meaning (positive cache, bumped on remove/split/rebuild —
the add-bump I introduced last pass is reverted) and a new `add_gen` serves the negative cache alone. Both
seeded per instance against the ABA. The resolve path now takes **one** acquire load total.

**Correctness settled separately, and it retracts my own reading.** The suite showed a stable-at-n=3 failure
only with the cache on (`19316`, an SSE store to a mapped stack buffer). Rather than argue the bracket is
right, I added `MACRUNNER_HB_VERIFY_NEG_CACHE`: every gap hit re-walks the treap and prints the entry that
lied. **0 violations across 3 full suite runs**, and 19316 turns out to appear in 2 of 3 runs — the suite's
known ASLR flakiness, not a regression. n=3 was too few to call it stable; that is the same trap the lane
already documented and I walked into it again.

**Default is now opt-in** (`MACRUNNER_HB_NEG_CACHE=1`). Shipping the arm the evidence favours, keeping the
mechanism and its instruments intact behind the gate.

[NEXT] re-run the same two-arm A/B on the split-epoch build — the whole point of the fix is that ON should now
stop paying for the coherence traffic. If it still loses, the negative cache is refuted for this workload and
the MMU group's remaining cost is the positive-cache scan itself, not the walk.

## 2026-07-31 (III) — split-epoch A/B: the regression is gone, and the result points somewhere else entirely

Re-ran the two-arm A/B on the split-epoch build (`hot_gen` positive / `add_gen` negative, one acquire load per
resolve). Both arms sampled at guest+163 s, same 300 s budget.

**The coherence diagnosis was right.** Every profile number reversed sign versus the previous build:

| | ON | OFF | Δ now | Δ before |
|---|---|---|---|---|
| `find_region*` self | 0.00 % | 0.63 % | **−0.63** | +2.09 |
| MMU group | 10.14 % | 11.08 % | −0.94 | +1.81 |
| `hb_memory_read` | 5.03 % | 6.77 % | −1.74 | +0.02 |

Caveat I have to state: the two arms' hottest emulation threads were *different named threads*
(`…: Unit…` vs `…: Load…`), so those shares compare different work. The one clean cross-arm number is the
process-wide dispatch rate, and `total_dispatches` is confirmed a GLOBAL atomic accumulator (the same line
carries `thread_dispatches` separately), so it is comparable.

**Dispatch rate — and the thing that kills the win.** Using the in-run `[laneA-ts +NNNs]` clock the runs
already stamp:

| arm | Begin MonoManager | UnloadTime | rate | dispatches burned to reach UnloadTime |
|---|---|---|---|---|
| ABON (old build, on) | 54.7 s | 245.4 s | 1933 k/s | 468 M |
| ABOFF (old build, off) | 54.4 s | 175.6 s | 2046 k/s | 440 M |
| AB2ON (split, on) | 58.3 s | **never** | **2355 k/s** | 644 M and still not there |
| AB2OFF (split, off) | 53.2 s | 212.7 s | 2052 k/s | 472 M |

Three arms need ~460 M dispatches to reach `UnloadTime` (440/468/472, ±3.5 %). The split-epoch ON arm burned
**644 M — 1.4× — and never arrived**, while running 15 % FASTER and still executing at 2355 k/s when the budget
expired. Not a crash: same triage class, one exception line, zero fault pins, no wedge.

**Reading it.** More dispatches, more rate, less progress is the signature of a spin, not of speed. The
coherent story: those 166 M null lookups are not one-shot probing — they are a probe/retry loop whose
iteration rate was being limited by the cost of the failed lookup. Make the lookup cheap and the loop simply
churns faster, converting saved time into more iterations of the same wait. That would also explain why a
quarter of ALL region lookups are for unmapped addresses, which was never a normal-looking number.

The alternative is plain variance — the two OFF arms alone put `UnloadTime` at 175.6 s and 212.7 s, a 37 s
spread, which is exactly the ±78 s the brief warns about. n=1 per arm cannot separate these. What it CAN say is
that the negative cache does not buy progress, so the default stays opt-in.

[NEXT] stop optimising the lookup and ask what issues it. One gated Misra-Gries over the GUEST PC at the moment
of a null lookup: if they concentrate in one or two blocks, we learn what the loop is, and a spin-wait on
unmapped memory is a far bigger prize than the walk ever was. Same one-counter shape as every measurement that
has worked in this lane.

## 2026-07-31 (IV) — the nulls are not a spin; they are US asking the same question twice

Two measurements, then a fix that follows from them.

**1. The spin hypothesis is REFUTED.** Histogrammed the guest block address at the moment of each null lookup
(Misra-Gries, 32 counters, gated). Over 112 882 264 nulls on the busiest thread only ONE block clears 1 % of
the stream, at 1.5 % — and with `evicted=3 357 850` each counter was decremented 3.36 M times, so even its true
count is at most ~4 %. Thirty-two counters consumed ~107 M credits against 112.9 M events: the stream is spread
thin over many blocks. The 166 M nulls are DIFFUSE, so last pass's "probe/retry loop spinning faster" story for
the 644 M-dispatch anomaly does not hold. Recorded as refuted.

**2. Call-site attribution found the real source, and it is not the guest.** Tagged all five resolve call sites
plus the three external `hb_memory_find_region` callers:

| site | nulls | share | null-rate |
|---|---|---|---|
| `hb_memory_find_region` | 63.0 M | 47.8 % | **94.7 %** |
| `hb_memory_read` | 49.9 M | 37.9 % | 14.1 % |
| `hb_memory_write` | 12.3 M | 9.4 % | 10.2 % |
| `jit_host_span` | 3.3 M | 2.5 % | 52.5 % |
| `hb_memory_host_ptr` | 3.2 M | 2.5 % | 27.4 % |

`jit_codegen` and `interp` were zero, so the external probes are not it. The arithmetic then closes exactly:
**63.0 M ≈ 49.9 M + 12.3 M = 62.2 M**. Every failed read/write resolves THE SAME ADDRESS TWICE — once directly,
then again inside `can_read_span`/`can_write_span` → `check_perm_span`. And `check_perm_span` bails on its first
null, so when `region` is already NULL that second lookup is provably NULL too (same address, normalize applied
before both; same epoch) and the span check can only return false.

**3. Fix: skip it.** `if ((region || null_span_recheck_enabled()) && hb_memory_can_read_span(...))`. Measured,
normalized per dispatch because the two runs did different amounts of work:

| | with recheck | skipped | Δ |
|---|---|---|---|
| `find_region` lookups/dispatch | 0.1868 | 0.0120 | **−93.6 %** |
| total region lookups/dispatch | 1.573 | 1.402 | **−10.9 %** |
| `find_region` null-rate | 94.7 % | — | nulls 63.0 M → 0.88 M |

Suite: 5 runs per arm this time (n=3 fooled me once already on this suite) — stable failures 33 in both,
**NONE introduced**. Default ON, gate `MACRUNNER_HB_NULL_SPAN_RECHECK=1` restores the old path.

**What I am NOT claiming.** Dispatch rate rose 9.4 %, and neither instrumented run reached `UnloadTime` inside
the 260 s budget, so there is no progress measurement here. After the negative-cache result — where +15 % rate
came with 644 M dispatches and no marker — rate alone is not evidence of speed. The defensible claim is
narrower and stronger: this deletes provably-dead work (10.9 % of all region lookups) rather than making a
failed operation cheaper, which is the category that has actually paid off in this lane (import scan, hot-miss
rescan).

[NEXT] the negative cache now has far less to do — its whole population was 99 % these same nulls, and 63 M of
them no longer happen. Re-run the ON/OFF A/B on this build before spending anything more on it; it may simply
be obsolete. Then re-profile: with `find_region` at 0.012 lookups/dispatch the MMU group should have collapsed,
and the next-largest item takes over.

## 2026-07-31 (V) — the negative cache was UNSOUND on the real workload; found, fixed, verified 2118 → 0

Re-A/B'd the negative cache on the span-recheck build and got the same signature a THIRD time — and this time
three replications made it a pattern rather than noise:

| arm | rate | dispatches | UnloadTime |
|---|---|---|---|
| ABON / AB2ON / AB3ON (cache ON) | 1933 / 2355 / 2346 k/s | 532 / 644 / 652 M | 245.4 s / **never** / **never** |
| ABOFF / AB2OFF / AB3OFF (OFF) | 2046 / 2052 / 2000 k/s | 560 / 560 / 548 M | 175.6 / 212.7 / 223.4 s |

3/3 OFF arms reach the marker; the two fast-cache ON arms burn ~650 M dispatches and never do. Higher rate,
more work, no progress — three times.

**The verify pass had a hole I put there: it had only ever run in the unit suite, which is single-threaded.**
Ran it on a live HK boot instead: **2118 VIOLATIONS, 267 distinct addresses** — the gap cache reporting mapped
memory as unmapped. Every violating address is a fresh 64 KB commit (`0x114c20000`, `0x114c30000`, …) sitting
INSIDE a cached gap.

**Root cause — my own epoch split.** Splitting `hot_gen` (positive) from `add_gen` (negative) was right in
principle but assumed every add goes through `insert_region_head`. It does not: the VM-map sync path appends
straight to `mem->regions` and calls `rebuild_region_tree`, which bumps only `hot_gen` via `clear_hot_cache`.
So regions materialised inside cached gaps with nothing to invalidate them. That is also the mechanism behind
the 3-run pattern above: those arms were not "spinning", they were **executing on wrong memory answers**.

**Fix:** `clear_hot_cache` now bumps `add_gen` as well. Deliberately conservative — removes and splits do not
strictly need to invalidate a gap — but that is the right trade against a stale gap declaring mapped memory
unmapped, and plain adds still bump only `add_gen`, so the positive cache keeps the hit rate the split bought.
**Re-verified on a live boot: 2118 → 0 violations**, and the arm reached `UnloadTime` at 227.9 s (vs 260.0 s
before the fix, in the same instrumented configuration).

**Corrections this forces on my own earlier entries.** The "spin" reading from pass (III) is wrong — it was
data corruption, not a spin. And the pass (II) conclusion "negative cache correct but slower" was based on a
verify that could not see the defect; the cache was never correct on this workload.

**Shipping-config profile** (recheck skipped, cache off), critical thread: `__ulock_wait2` 17.3 %,
`hb_jit_runtime_run` 10.6 %, `run_jit_block_with_signal_guard` 8.7 %, `_tlv_get_addr` **7.5 %**,
`hb_memory_read` 5.6 %, `_platform_memmove` 3.1 %, `find_region_impl` 2.0 %. MMU group is down to **10.10 %**
and `find_region*` to 1.98 % — the span-recheck removal did land where predicted.

[NEXT] `_tlv_get_addr` at 7.5 % is now the largest non-dispatch item and it is pure overhead: every `__thread`
access from a dylib goes through it on Darwin. (`tls_model` is already known to be silently ignored on
darwin-arm64, so the lever is hoisting TLS reads out of hot paths into locals, not an attribute.) Count the TLS
reads per dispatch first — same one-counter shape.

---

## ПОДСКАЗКА КООРДИНАТОРА (2026-07-31 04:0x) — прототип из QEMU для проблемы `jcc` 71 %

Не указание, а найденное прежнее решение. Проверь и применяй по своему усмотрению.

**Твоё измерение:** `jcc` = 71.1 % концов блоков, прямые переходы = 7.4 %. Связывание с
ОДНИМ слотом (наши 4 `NOP`) может накрыть только 7.4 %. Ты предложил связывание с проверкой PC —
QEMU решает ту же задачу иначе и проще.

**Как у QEMU** (docs/devel/tcg.rst, «Translator Internals»):

> В каждом блоке трансляции ровно **ДВА слота перехода** (индекс 0 или 1).
> `tcg_gen_goto_tb()` получает индекс слота параметром.
> «Чтобы воспользоваться связыванием при условных переходах, нужно (как минимум) **два набора
> метаданных связывания — по одному на каждую ветвь**.»

Механика в три шага: `goto_tb(slot)` прыгает вперёд на обновление состояния; затем пишется новый
PC; затем `exit_tb()` возвращает **помеченный указатель — адрес последнего исполненного блока и
индекс слота**. Главный цикл, найдя блок-назначение, патчит слот у ТОГО блока, который вернул
`exit_tb`.

**Три вещи, которые стоит взять:**

1. **Два слота вместо одного.** Каждая ветвь `jcc` получает свой. Это и есть переход с 7.4 % на
   ~78 % покрытия, без проверки PC во время исполнения.
2. **Блок для патча возвращает сам исполняемый код**, а не выводится диспетчером. В документации
   прямо оговорено: этот адрес «может отличаться от того, что был запущен из главного цикла, если
   последний уже был связан с другими блоками». **Это похоже на твой дефект `rcx`/`rdx`**: если
   предшественник определяется по тому, кого запустил диспетчер, а фактически исполнялась цепочка,
   патчится не тот блок. У тебя `blocks=2 steps=71` на один связанный проход — тот же признак.
3. **Условия, при которых связывать нельзя** (у нас таких проверок нет):
   - изменение состояния ЦП должно быть **постоянным** — то есть прямой переход, не косвенный;
   - **переход не должен пересекать границу страницы**: отображения памяти могут смениться, и код
     по адресу назначения станет другим.

Второе условие для нас особенно важно: гость — Mono, который **переписывает свой код**, и у нас
уже есть отдельная линия про устаревшие трансляции.

Источники: https://www.qemu.org/docs/master/devel/tcg.html ,
https://github.com/qemu/qemu/blob/master/docs/devel/tcg.rst

---

## ★ ПОДСКАЗКА КООРДИНАТОРА — разбор ВСЕЙ цепочки, а не звена

Полный документ: `reports/research/DBT-PIPELINE-GAP-ANALYSIS-20260731.md`.

Сверил нашу цепочку с box64 / FEX / QEMU / Prism целиком. **Главное найденное: у нас нет
распределения регистров.** `hb_arm64_codegen.c:668 x64_reg_off()` — каждый доступ к гостевому
регистру эмитится как обращение в память контекста. То есть `add rax, rbx` это загрузка,
загрузка, сложение, запись: **четыре обращения к памяти на одну арифметическую операцию**.

У ARM64 31 GPR, у x86-64 — 16. Весь гостевой регистровый файл влезает в хозяйские регистры;
сбрасывать нужно только на границе блока, перед вызовом хелпера и перед возможным отказом.

FEX делает это `RegisterAllocationPass`, box64 — отдельным проходом перед эмиссией (у него
всего 4 прохода: счёт → флаги и переходы → регистры → эмиссия).

**Почему это важнее связывания, которым ты занят:** связывание убирает круг диспетчера МЕЖДУ
блоками; распределение регистров убирает лишние обращения к памяти ВНУТРИ каждой инструкции,
а их на порядок больше. Первая версия не требует ни анализа живости, ни IR-проходов —
достаточно статического отображения 16 на 16 со сбросом на границах.

Ленивые флаги у нас, для сведения, **есть** (`hb_lazy_flags_t`) — это звено закрыто.

Порядок по убыванию отдачи: **распределение регистров → связывание с двумя слотами →
кеш непрямых переходов → общий кеш трансляций на модуль**.

Решай сам, но, по-моему, стоит отложить дальнейший бисект связывания и оценить
распределение регистров: сколько обращений к памяти в среднем блоке приходится на
чтение/запись гостевых регистров. Это меряется статически, без прогона.

## 2026-07-31 (VI) — STATIC step per the new brief: the register-allocation ceiling is 9.0 %, and it is not the biggest item

Measured with **zero HK runs**, on 1 096 350 real blocks / 37 825 816 emitted ARM64 instructions from a
persisted translation cache (`ntdll-5385710288b50361`, 221 MB). Full method and numbers:
`reports/research/SPEED-DIG-STATIC-REGISTER-TRAFFIC-20260731.md`.

Guest-register traffic is exactly identifiable in the emitted code: every access is `LDR/STR` with base
**x19** (the `hb_context_t` pointer) at a `reg_off()` displacement. Offsets taken from the compiler, not read
off the struct.

```
emitted = 11.8 + 7.2 × guest_instructions
```

- mean guest instructions per block **3.14** (44.8 % of blocks hold ≤2, and those are 32.8 % of all emitted code)
- **fixed per-block overhead 11.8 instrs = 34 % of ALL emitted code**
- **guest-GPR loads/stores = 9.0 % of all emitted**, i.e. **0.99 per guest instruction**
- XMM context traffic 7.0 %; `rip`/`rflags` context traffic **zero**

**The brief's premise does not hold.** "`add rax, rbx` = four memory accesses per arithmetic op" — the
emitted average is **one** guest-GPR access per guest instruction, 9.0 % of instructions overall. Register
allocation is worth at most 9 %.

**Fixed per-block overhead is 3.8× the entire register-allocation ceiling**, and it is paid once per dispatch
on 3.14-instruction blocks: 3.8 ARM64 instructions of pure overhead per guest instruction.

**Register allocation is gated on block size, not the reverse.** A static 16→16 mapping must flush at every
unchained block boundary; with 3.14-instruction blocks and ~1.0 GPR access per instruction, the mandatory
load-on-entry/store-on-exit traffic is comparable to what is removed. Chaining/larger blocks create the room
for RA — so the brief's ordering (RA, then chaining) is inverted by the data.

Method trap recorded: the first counter reported 0 load/stores in 37.8 M instructions. JS bitwise ops return
signed int32, so `w & 0xFFC00000` never matched the positive constant. A zero from a counter is a claim about
the counter first.

[NEXT] price the 11.8-instruction prologue/epilogue from the same cache (common prefix/suffix across blocks,
no run): that decides whether to chain past it or shrink it.

## 2026-07-31 (VII) — the 34 % fixed overhead is a callee-saved frame, and half the blocks never need it

Decomposed the per-block overhead from the same cache, still zero HK runs. Modal prologue is 4 instructions
(100 % of blocks), modal epilogue 5 (63.9 %):

```
STP x19,x20,[sp,#-48]! / STP x21,x22,[sp,#16] / STP x23,x30,[sp,#32] / MOV x19,x0
... STR x21,[x19,#544] / LDP x23,x30 / LDP x21,x22 / LDP x19,x20 / RET
```

Eight of those nine instructions are a **callee-saved register frame — 12 memory accesses per dispatch** for a
block averaging 3.14 guest instructions. All guest-register traffic together is 3.1 per block, so **the frame
costs ~4× what register allocation could save.**

The frame exists only because a block may `BL` a helper. Counted: **48.3 % of blocks (529 347) make ZERO
calls** — calls per block are only ever 0 or 1, mean 0.52. Eliding the frame there removes **6 352 164 memory
accesses = 1.87× the ENTIRE register-allocation ceiling**, and **4 234 776 instructions = 11.2 % of all
emitted code**.

Feasible because the frame only honours AAPCS: a call-free block can take its 5 scratch registers from the
caller-saved bank (x9–x15, 7 available), needing no save/restore and no x30 spill; the epilogue collapses to
`STR x21,[x19,#544]; RET`.

**Revised ordering: (1) elide the frame on call-free blocks — 11.2 %; (2) chaining/larger blocks; (3) register
allocation — 9.0 % ceiling, mostly eaten by boundary flushes until (2).**

[NEXT] implement (1) behind a gate: parameterise the hard-coded scratch register numbers per block, select the
caller-saved mapping when the IR block lowers to no helper call, and verify by re-dumping the cache — call-free
blocks must start with `MOV`, not `STP`.

## 2026-07-31 (VIII) — preconditions for the lean frame all verified; design settled, no run

Checked what a call-free block actually needs, over all 529 347 of them (frame words excluded): **0** use the
stack for anything else, **0** write x24–x28, **0** write x30, **0** end in anything but `RET`. Body register
usage: x19 100 %, x21 100 %, x20 86.4 %, x22 56.6 %, x23 32.0 % — at most five registers, no stack. AAPCS has
seven caller-saved (x9–x15), so **a call-free block can run with no frame at all.**

Design: the 596 hard-coded scratch literals mean the remap must live in the **42 encoder primitives** —
`uint8_t rmap[32]` on `hb_codegen_buffer_t`, identity by default, applied on primitive entry. Call sites stay
untouched. Two conditions: (a) relocations are recorded inside a primitive (`:259 relocs[].reg = rd`), so the
remap must be applied BEFORE that line and patching stays consistent for free — classification is already by
KIND after the earlier x23 incident; (b) call-free must be known before emission → two-pass emit (re-emit with
frame if a call appeared), costing ~0.35 s of the already-refuted 0.69 s codegen budget.

Instrument trap: the first precondition check said "100 % of call-free blocks touch SP outside the frame". That
was my window — the epilogue is 5 words and I excluded 3, so `LDP x23,x30,[sp,#32]` was counted once per block.
Exactly-one-per-block is what exposed it.

[NEXT] implement behind `MACRUNNER_HB_LEAN_FRAME`: rmap in the 42 primitives, skip frame for call-free blocks,
two-pass emit. Verify with no HK run by re-dumping the cache — call-free blocks must start with `MOV`, not
`STP`, and still end in `RET`; then unit suite 5 runs per arm before any timing claim.

## 2026-07-31 (IX) — lean frame implemented and gated; two self-inflicted bugs found, baseline restored

Implemented the frameless-block change behind `MACRUNNER_HB_LEAN_FRAME`:
- `rmap[32]` on `hb_codegen_buffer_t`, applied in the **31 leaf encoders** (those that build an encoding
  directly). Composites like `emit_ldr_gpr` are deliberately excluded — they delegate, and remapping both
  levels would substitute twice. None of the 596 call sites changed.
- `emit_prologue`/`emit_epilogue_ex` skip the frame when armed; `emit_blr` sets `emitted_call`.
- Two-pass emit in `hb_arm64_codegen_block_with_cfg`: emit lean, and if a call appeared, reset and re-emit
  with the frame.
- Relocation ordering verified: `emit_mov_imm64_kind` applies `rd = hb_rm(...)` **before**
  `codegen_note_reloc(buf, rd, …)`, so the recorded register is the remapped one and patching stays consistent.

**Static verification passed.** From a cache dumped by a real run: 81.8 % of blocks frameless, **0 frameless
blocks containing a call**, 0 frameless blocks referencing the old ctx register x19 as a base (136 use the
remapped x11; framed blocks still correctly use x19). The mechanism does exactly what it claims.

**Bug 1 — register collision (real, fixed).** First mapping was x19→x9. But `hb_arm64_codegen.c:3453` already
emits `MOV x9, x20` / `MOV x10, x21` as temporaries, so x19→x9 made that instruction overwrite the **ctx
pointer**. Moved the mapping to x11–x15, the five caller-saved registers nothing else claims.

**Bug 2 — uninitialised gate (real, fixed, and it broke the DEFAULT path).** `hb_codegen_buffer_t` is never
zero-initialised anywhere in the tree, so testing `buf->rmap_active` read uninitialised stack and armed the
remap at random **with the gate off**. The plain baseline regressed to no markers at all. Fixed with two
guards: a file-scope `g_lean_frame_on` that only `lean_frame_arm()` sets, plus an `0xA5` sentinel — with the
gate unset the remap is the identity by construction. **Baseline re-verified: `Begin MonoManager` 56.2 s,
`UnloadTime` 152.2 s** (the fastest UnloadTime measured today, vs 175.6/212.7/223.4/245.4 s).

**All lean-frame timing results from this pass are VOID** — every one of those runs executed on the build with
the uninitialised gate, so both the "lean" and "control" arms were randomly remapping. The 22-block caches and
absent markers measured the bug, not the change.

A second confound worth recording: I enabled `MACRUNNER_HB_TRANSLATION_CACHE=1` to obtain a dump, which
changed two variables at once. The control run (cache on, lean off) produced a byte-identical 254 KB log,
which is what exposed that the regression was not the lean frame.

Unit suite 5 runs per arm on the pre-sentinel build: stable failures 33 in both, none introduced.

[NEXT] redo the lean-frame measurement on the fixed build: unit suite 5×5, then one gated run vs one baseline
run, comparing `UnloadTime` on the in-run clock and the frameless share from a fresh cache dump. Only after
that does any speed claim mean anything.

## 2026-07-31 (X) — lean frame measured cleanly on the fixed build: it BREAKS the boot

With the uninitialised-gate bug fixed, the comparison is finally valid — same build, same config, only
`MACRUNNER_HB_LEAN_FRAME` differs:

| arm | log | Begin MonoManager | UnloadTime |
|---|---|---|---|
| gate OFF (baseline) | 0.50 MB | 56.2 s | **152.2 s** |
| gate ON (lean frame) | 0.28 MB | — | **never — no markers at all** |

Unit suite on the fixed build, 5 runs per arm: stable failures 33 in both, **none introduced**. So the defect
is on a path `hb_test_runner` does not exercise — which is consistent with it being the signal/fault or
dispatch machinery, the parts HK hammers and the suite barely touches.

**What is verified correct**, so the search can skip it: the emitted code itself. From a real cache dump,
81.8 % of blocks frameless, **0** frameless blocks containing a call, **0** frameless blocks still using x19
as a ctx base (136 use the remapped x11, while framed blocks correctly keep x19). Relocations are recorded
after the remap. The register collision with `MOV x9, x20` at `:3453` is fixed by mapping to x11–x15.

**One latent incompatibility found, but it is NOT the current cause:** `hb_runtime.c` recognises the block
tail by exact encoding — `entry_has_chain_slot` (`:3010`) keys on `MOV X0, X19` (`0xaa1303e0`) plus
`LDP X23,LR,[SP,#32]` / `LDP X19,X20,[SP],#48`, and `chain_trampoline_build_at` (`:3162`) does the same. A
frameless block has none of those bytes. Both sites are inside the chaining machinery, which is default-off,
so they cannot explain this failure — but they will have to be taught about frameless blocks before chaining
and the lean frame can ever be on together.

**Gate stays OFF and the tree is safe:** the baseline was re-verified on this exact build (UnloadTime 152.2 s,
the fastest measured today).

[NEXT] bisect the change into its two independent halves, which the gate makes trivial:
(a) **remap only** — apply x19–x23 → x11–x15 but KEEP emitting the frame. Semantically valid (the block still
    preserves the callee-saved registers it no longer uses), so if this breaks, the remap is at fault;
(b) **frame elision only** — impossible to test alone without violating AAPCS, so it is the residual: if (a)
    boots, the defect is in dropping the frame, and the next question is who depends on that stack layout.
One run per arm answers it, and the technique — bisect with the guest as oracle — is the one that has
localised every defect in this lane.

## 2026-07-31 (XI) — bisect: the REMAP is at fault, not the frame elision

Added `MACRUNNER_HB_LEAN_REMAP_ONLY` — apply the register remap but keep emitting the frame. That is
semantically valid (the block preserves callee-saved registers it has stopped using), so it splits the change
cleanly in two.

| arm | log | markers |
|---|---|---|
| baseline (same build) | 0.50 MB | MonoManager 56.2 s, **UnloadTime 152.2 s** |
| remap + frame elided | 0.28 MB | none |
| **remap only, frame kept** | **0.25 MB** | **none** |

Half A breaks too, so **the defect is the register remap itself** and frame elision is exonerated for now.
That is worth knowing precisely because the frame elision was the part carrying the 11.2 % — it is still
viable once the remap is sound.

**Mechanism, confirmed:** the remap lives in the 31 leaf encoders, but not every instruction goes through
them. `hb_arm64_codegen.c:4527` builds an encoding inline with literal register numbers —

```c
emit_u32(buf, 0x8b000000 | (21 << 16) | (shift << 10) | (20 << 5) | 20);   /* ADD x20, x20, x21, LSL #s */
```

— so `hb_rm` never sees it. With the remap armed, every other instruction in that block uses x12/x13 while
this one still writes x20: silent corruption of a live value. There are **57** expression-built `emit_u32`
sites in the file, and a source grep can only find the ones spelled with literal digits; a local variable
holding 21 would be invisible to it.

**So the audit must be empirical, not textual.** The method that already worked (checking the ctx base
register in emitted blocks, which came back clean at 0/136) generalises: dump a cache from a remap-only run
and scan **every register field of every instruction** in remapped blocks for any surviving reference to
x19–x23. That produces the exact list of bypassing instructions with no reliance on grep. My earlier check
only inspected the `Rn` field of loads and stores, which is why it reported clean while `Rd`/`Rm` bypasses
went unseen.

Build discipline note: this pass also caught a `make` failure where hyperbridge returned rc=2 and wine
relinked the previous library — the `strings` gate check on the shipped `ntdll.so` reported `gate=0` and
stopped a measurement that would have been meaningless. Cause was use-before-declaration
(`lean_remap_only()` called from `emit_prologue`, defined ~10 000 lines later); fixed with a file-scope flag.

[NEXT] run remap-only WITH `MACRUNNER_HB_TRANSLATION_CACHE=1`, dump, and scan all register fields of remapped
blocks for x19–x23 survivors. Fix each, re-run, and only when that scan returns zero re-test the full lean
frame.

## 2026-07-31 (XII) — the remap was incomplete because my own transform skipped attributed functions

Fixed the remap and it now boots. Two bypass classes, found by two different methods:

**1. Inline encoding (found by reading source).** `:4527` built `ADD x20,x20,x21,LSL#s` with `emit_u32` and
literal registers. Routed through `emit_add_reg_lsl`, which encodes identically and is remapped. Fixing this
alone changed nothing — still no markers.

**2. Twelve primitives my transform silently skipped (found by scanning emitted code).** The empirical scan —
decode every instruction in remapped blocks, extract every register field, look for x19–x23 survivors —
returned four offending encodings: `SUB x22,x20,x21`, `AND x22,x22,x23`, `AND x21,x21,x23`, `EOR x22,x20,x21`.
Tracing them to source found `emit_sub_reg`, `emit_and_reg`, `emit_ands_reg`, `emit_orr_reg`, `emit_eor_reg`,
`emit_cmp_reg`, `emit_tst_reg`, `emit_lslv`, `emit_lsrv`, `emit_asrv`, `emit_mvn`, `emit_neg` — **all declared
`static void __attribute__((unused)) emit_…`**, and my transform's regex character class excluded parentheses,
so it never matched them. Twelve leaf encoders, silently untouched, while the tool reported "31 transformed"
and looked successful.

**After fixing all 12:**

| | log | Begin MonoManager | UnloadTime |
|---|---|---|---|
| baseline | 0.50 MB | 56.2 s | **152.2 s** |
| remap-only (before) | 0.25 MB | — | none |
| **remap-only (after)** | **0.42 MB** | **55.4 s** | **210.5 s** |

**Scan now returns zero:** 47 705 remapped blocks, 1 098 656 body instructions decoded, **no x19–x23
references remain**. Caveat on that claim: 5.9 % of instruction forms are undecoded by my classifier, so
"zero" covers the 94.1 % it recognises.

210.5 s vs 152.2 s is slower, but n=1 per arm and today's `UnloadTime` spread is 152–245 s, so this is not yet
a speed verdict — only proof the remap is sound enough to run.

**Method note worth keeping:** the source-transform tool reported success and a plausible count (31 functions)
while missing a whole category. What caught it was scanning the *emitted artifact*, not re-reading the source
— the same lesson as the earlier "verify the artifact, not the build". A transform's own report of what it did
is not evidence that it did it.

[NEXT] now that the remap is clean, re-test the FULL lean frame (frame elision) — that is the half carrying the
11.2 %, and it was exonerated by the bisect but never ran on a sound remap. Then, if it boots, a proper
multi-run timing comparison rather than n=1.

## 2026-07-31 (XIII) — the lean frame WORKS end to end; timing inconclusive at n=2, as expected

With the remap sound, the full change was tested:

| arm | Begin MonoManager | UnloadTime |
|---|---|---|
| baseline | 56.2 s / — | 152.2 s, 218.3 s |
| remap only (frame kept) | 55.4 s | 210.5 s |
| **lean frame (full)** | 55.4 s, 56.7 s | **150.6 s, 229.0 s** |

**The mechanism works end to end**: frameless blocks execute, the guest boots, both markers are reached, no
new failure class, and the unit suite showed no stable regressions at 5 runs per arm.

**Timing is inconclusive and no claim is made.** Means are 185.3 s (baseline) vs 189.8 s (lean) at n=2 each,
while the within-arm spread is 66–78 s — the brief's ±78 s warning, exactly. Separating a hoped-for ~11 %
effect from that noise needs ~8 runs per arm, which is hours; cross-run timing is the last resort by Rule 1.

**So the next measurement is the within-run one.** The change's magnitude is a property of the emitted code,
not the clock: the static prediction is 4 234 776 instructions removed = 11.2 % of all emitted code, and the
same cache dump that verified the remap can verify the actual reduction directly — mean emitted instructions
per block, lean vs baseline, on the same corpus of guest addresses. That is a Rule-1 quantity, immune to the
±78 s, and it either confirms 11.2 % or says the two-pass re-emit is eating it.

Operational note: the Bash tool caps at 10 minutes, so a loop of four 260 s runs was killed mid-flight. One
run per call from now on; the killed run left no orphaned wine processes (`mr-clean` verified 0), and its
partial run dir was discarded rather than read.

[NEXT] dump caches for both arms on this build and compare mean emitted instructions per block over the
intersection of guest addresses — the within-run confirmation of the 11.2 %.

## 2026-07-31 (XIV) — within-run confirmation: 11.03 % fewer emitted instructions, predicted 11.2 %

Dumped a translation cache from each arm on the same build and intersected on identical 48-byte cache keys, so
the comparison is over the SAME guest blocks rather than whatever each run happened to reach.

| | baseline | lean |
|---|---|---|
| same guest blocks | 53 414 | 53 414 |
| mean emitted instrs/block | 36.03 | **32.06** |
| total instructions | 1 924 765 | 1 712 491 |
| **reduction** | — | **11.03 %** (predicted **11.2 %**) |
| smaller / identical / larger | — | 66.2 % / 33.8 % / **0** |

Two independent samples agree on block size: 3.10 guest instrs/block here vs 3.14 on the 1.1 M-block corpus.
Lean arm emits 48 055 frameless against 25 065 framed blocks.

This is the Rule-1 quantity for this change — a property of the emitted code, immune to the ±78 s. The static
analysis predicted the effect before the code was written and the built change hit it to within 0.2 points.

**Wall-clock still unproven and no claim made**: UnloadTime n=2 per arm, baseline 152.2 / 218.3 s vs lean
150.6 / 229.0 s, within-arm spread 66–78 s. Gate stays OFF until a multi-run campaign (~8 per arm) can
separate 11 % from that noise.

[NEXT] either fund the timing campaign (~8 runs/arm ≈ 70 min of slot) or go after the rest of the fixed
overhead, since the same corpus says the frame is only part of the 34 %: chaining removes the dispatch
round-trip that the remaining prologue/epilogue serves.

## 2026-07-31 (XV) — the cheap marker is not sensitive; the lean frame's timing verdict is not affordable

Tried to buy the timing verdict cheaply. `Begin MonoManager` lands at ~55 s with a ~1 s spread, so a 120 s
budget would cost half a run — if that marker responds to the change.

| arm | Begin MonoManager | mean |
|---|---|---|
| baseline | 56.2, 54.1 | 55.15 s |
| lean | 55.4, 56.7, 55.7 | 55.93 s |

**It does not respond** — lean is 0.8 s *slower*, i.e. noise. `Begin MonoManager` sits in the load-dominated
prefix, not a JIT-throughput-bound phase, so its low variance buys nothing. Recorded as a refuted experiment
design rather than a result about the change.

**And the expensive verdict is not worth funding, by arithmetic.** The frame is 6 instructions of a ~34.5
instruction block, all L1-resident stack accesses. Even assuming they cost their full share of block execution
(~15 %), block execution is only part of the run — the clean profile puts the dispatcher at 16.0 % and guest
code at ~5 %. A plausible wall-clock effect is therefore **3–6 %**, i.e. 6–11 s on a 185 s mean, against a
measured within-arm spread of 66–78 s. Detecting that needs on the order of **30 runs per arm**, not 8. At
~4 min per run that is hours of slot for a single-digit percentage.

**Decision: stop here on the lean frame.** It stays gated, correct, and documented with a measured 11.03 %
code-size reduction. Spending the slot on the same corpus's larger item is the better trade.

**What that larger item is, from the same measurements:** the frame was only part of the 34 % fixed overhead.
The rest is the dispatch round-trip itself — return to C, re-enter the dispatcher, look up the next block —
which chaining removes outright. The clean profile puts `hb_jit_runtime_run` at 16.0 % and
`run_jit_block_with_signal_guard` at 4.6 % of the critical thread, ~20 % in dispatch machinery, against the
frame's 11 % of emitted bytes. Chaining is also what makes register allocation worth doing (Part I): with
3.14-instruction blocks, boundary flushes eat the 9 % until blocks get longer.

[NEXT] chaining. It is already implemented and gated (`MACRUNNER_HB_BLOCK_CHAIN`) with a precisely localised
defect from 30.07 — guest `rcx`/`rdx` corrupted on the edge out of `0x87ef246adb6`, successor
`mono-2.0-bdwgc.dll+0x69a30` receiving non-pointers. Start by reproducing that single edge, not by re-running
the whole feature: the cache-dump tooling built this pass can decode the emitted trampoline and the
predecessor's tail directly, which is exactly the kind of evidence that localised every other defect here.

## 2026-07-31 (XVI) — chaining: the corrupted register is RBP, and the instrument was watching the wrong ones

Moved to chaining. Two cheap statics first, then one run.

**Static 1 — stack-leak hypothesis REFUTED for free.** The trampoline branches to `entry->native_code + 12`
(`hb_runtime.c:3276`), i.e. three words past the head, landing on `MOV X19, X0` and deliberately *skipping*
the successor's frame push. The predecessor's frame is reused and whichever block finally mispredicts runs the
one epilogue that pops it. No per-hop leak.

**Static 2 — that same line pins the lean-frame incompatibility exactly.** A frameless block's prologue is one
instruction, so its `+12` is not `MOV X19, X0` but the middle of the body. Chaining and
`MACRUNNER_HB_LEAN_FRAME` cannot be enabled together until the chain target is computed from the block's real
entry rather than a hard-coded offset. (Alongside the encoding-signature checks at `:3010`/`:3162`.)

**The run.** `MACRUNNER_HB_BLOCK_CHAIN=1` + `MACRUNNER_HB_TRACE_CHAIN_EDGE=1`, 150 s: 64 chain edges, 32
transits, then a **guest failure dump at +50.9 s** and a wedge — no further output until the timeout at
+171 s. Control: the same dump appears **0 times** in both the baseline and lean-frame runs, so it belongs to
chaining.

**What the dump says:**

```
macrunner-hb-fail-backtrace: rsp=0x11992d978 walk_from=0x11992d978 stack_top=0x119940000 rbp=0x8
macrunner-hb-fail-bt[2]: val=0x87ef246adc2 module=mono-2.0-bdwgc.dll rva=0x6adc2
```

**`rbp = 0x8`.** The guest frame pointer is garbage — which is why the walker had to start from `rsp`. And the
failing site sits in the same mono function the 30.07 work named (`0x87ef246adb6`, rva `0x6adb6`), so this is
the same defect, now with the corrupted register identified.

**Why 32 traced transitions looked clean:** `macrunner-hb-chaintransit` prints only `rcx` and `rdx`. Zero of
32 showed a pointer→non-pointer change, and I nearly recorded that as "corruption not reproducing". The
instrument was simply watching the wrong registers — the 30.07 framing named rcx/rdx, and the trace was built
to match that framing rather than to find whatever is actually wrong.

**One naming trap recorded:** `macrunner-hb-fail-rbp-chain` is NOT about block chaining — it is the
frame-pointer-chain section header of the generic failure backtrace in `macrunner_hb_run_x64`. I started to
follow it as a chaining lead before reading its source.

[NEXT] extend `trace_chain_transition` to print `rbp` and `rsp` alongside rcx/rdx, re-run, and find the exact
transition where `rbp` becomes 0x8. That single edge is the defect, and the 30.07 work already narrowed the
neighbourhood to mono rva 0x6adXX.

## 2026-07-31 (XVII) — rbp instrumented: 21 transitions land on exactly 0x8, but the control is missing

Extended `trace_chain_transition` to carry `rbp`/`rsp` alongside rcx/rdx, plus an alert that is deliberately
**not** subject to the existing 32-line cap — that cap is precisely why the first 32 transitions all looked
clean while whatever matters was never printed.

**Result:** **21 chain transitions land on `rbp = 0x8`**, the exact value the failure dump reports at
`mono-2.0-bdwgc.dll rva 0x6adc2`. So the corruption is observable at a chain boundary, not only in the
post-mortem.

**But the alert cannot convict yet.** It fired 7232 times across 171 distinct source blocks, 2524 of them
`rbp→0`, and a normal transit line shows `rbp 1->1` — small rbp values genuinely occur in this guest code.
The trace only exists when chaining is ON, so there is no unchained control and "chaining corrupts rbp" is
not yet separable from "the guest sets rbp small here anyway". Recorded as an unfinished measurement rather
than a finding.

**The fix for that is the same shape that has worked all along — an in-run control.** `trace_chain_transition`
is called only when `block_delta > 1`, i.e. when a chain actually executed. Sampling `block_delta == 1`
dispatches the same way gives the unchained rate of `rbp→small` inside the same run, same workload, same
thread. If chained and unchained rates match, rbp is a red herring and the 0x8 is downstream of something
else; if the chained rate is higher, the 21 hits on 0x8 are the defect and their 171 source blocks are the
search space.

[NEXT] add the `block_delta == 1` control arm to the same instrument and compare the two rates within one run.
