# LANE HK SPEED — progress journal

Goal: cold start to HK menu < 120 s (today 476 s), warm < 60 s.
Rule of this lane: measure, fix, measure. Register the prediction BEFORE the run.

---

## Iteration 1 — 2026-07-29

### Where the previous iteration left off

`05f3f3f9` registered the 24 missing helpers. `mh_unkhelper` went 16970 → 0 and retention did
not move (64 % → 62 %): those blocks now reach the next filter and fail there. Rejection mix
after that commit:

| reason | count | share |
|---|---|---|
| `mh_argshape` | 24585 | 46 % |
| `mh_widearg` | 11759 | 22 % |
| `mh_toomany` | 3202 | 6 % |
| `mh_unkhelper` | 0 | — |

The relocation table (`d36cf063`) is recorded at codegen time but nothing reads it. The store
and load paths still *scan* the emitted bytes and try to recognise the shape.

### What the code actually says — read before touching anything

`emit_mov_imm64()` (hb_arm64_codegen.c:248) is the only emitter of the full 4-instruction
MOVZ/MOVK form, and it is the only thing `arm64_mov_imm64_at()` matches. Its 103 call sites,
grouped by destination register and by whether the value is a host pointer:

- **x1** (48 sites): 38 host pointers (`instr`×20, `block`×13, `first`/`sort`/`entry`/`call`),
  and **10 plain constants** — `instr->cc`×5, `instr->op`×2, `guest_addr+guest_len`,
  `mem.disp`, `src1.imm`.
- **x2/x3/x4** (18 sites): 8 host pointers (`second`, `third`, `fourth`, `less`, `instr`),
  and **10 plain constants** — `instr->op`, `dst.reg`, `src1.reg`, `dst.size`, REG/IMM flags.
- **x5/x6/x20/x21/x22** (33 sites): **zero** host pointers — all guest-derived
  (`src1.imm`, `target`, `guest_addr`, `mem.disp`, `dst.size`).

Statically verified with a whole-file grep for `(uint64_t)(uintptr_t)`: 46 hits, distributed
x1=38, x2=5, x3=2, x4=1, and nothing anywhere else. Plus x23, helper addresses by construction.

**Invariant, and the foundation for everything below: a host pointer can only ever reach
x1, x2, x3, x4 or x23.** So a relocation table covering those five registers is COMPLETE — it
sees every value that changes between the run that stores a block and the run that loads it.

### Two of the three rejection reasons are matcher artefacts, and one of them contradicts the brief

- **`argshape` (46 %)** — the matcher demands *exactly one* recognised arg1 per helper window,
  where "recognised" means the value equals `block` or `&block->instrs[i]`. At
  hb_arm64_codegen.c:4646/4652/4662/4668 the lazy-flag and setcc paths put `instr->cc` — the
  integer condition code — in x1, and the pointer, when there is one, in **x2**. So arg1_count
  comes out 0 and the block is thrown away. These are the flag paths, i.e. the densest thing in
  Mono-generated code. That is what 24585 rejections look like.

- **`widearg` (22 %)** — the brief calls this "the only rejection that describes a real property
  of the code rather than a weakness of the matcher". **That is not what the code does.** The
  veto is `arm64_mov_imm64_at(code, size, off, 2|3|4, &value)` — it matches the *pattern*, and
  never looks at `value`. Ten of the eighteen x2/x3/x4 sites move a small constant
  (`instr->dst.reg`, `instr->op`, `dst.size`, a 0/1 flag). Those need no relocation at all and
  are being vetoed anyway. `widearg` is the same artefact as `argshape`, not a property of the
  guest code. Correcting this before spending an iteration on the other two.

- **`toomany` (6 %)** — a 16-site cap in a matcher, against a 256-entry table.

All three fall to one mechanism: stop recognising, read the table.

### Design

Store is driven by the table, load is driven by unmistakable sentinels — no structural
recognition on either side, and no on-disk format change.

1. `codegen_note_reloc()` records x1/x2/x3/x4/x23 (was x1/x23). Complete by the invariant above.
2. Store classifies each recorded site **by value**, register-agnostic:
   `block` → BLOCK sentinel · `&block->instrs[i]` → INSTR|i · known helper (x23) → HELPER|id ·
   value below the 4 GB `__PAGEZERO` floor → provably not a host pointer, leave as a literal ·
   anything else → decline the block (an unresolvable host pointer — `first`/`second`/`sort`,
   pointers into *other* IR blocks, which genuinely cannot be restored).
3. Load scans for the 4-instruction form whose immediate matches a sentinel mask and patches it
   back, taking the destination register from the encoded instruction. No windows, no pairing,
   no site cap.
4. Sentinel collision guard: at store time, if any value we are NOT patching matches a sentinel
   mask, decline the block. That makes the load-time scan unambiguous by construction.
5. The existing round-trip self-check stays and now exercises the new load path.

### PREDICTION — registered before the run

On HK with the new path on:

- `mh_toomany` → **0** (site cap gone).
- `mh_argshape` → **0** (nothing is paired any more).
- `mh_widearg` → **0** (constants no longer vetoed).
- A new counter `reloc_hostptr` becomes the *only* remaining decline reason, and I expect it to
  be **small** — 8 of 103 call sites emit a cross-block pointer.
- Retention 62 % → **> 90 %**.
- Cold start: strictly faster than 476 s. I am NOT predicting < 120 s from this alone — the
  232 s Mono phase is the target, but lever 2 (two threads parked in `__ulock_wait2`) is
  untouched and no cache can move it.

**What would refute me:** retention stays under 80 % with `reloc_hostptr` large — meaning
cross-block pointers are common, not rare, and the table cannot fix that. Or: retention goes
above 90 % and the cold start does NOT improve — meaning re-translation was never the cost and
the 232 s is somewhere else entirely, which would redirect this whole lane to lever 2.

### Built and verified so far

- `codegen_note_reloc()` now records x1/x2/x3/x4/x23 (hb_arm64_codegen.c:238).
- `native_blob_reloc_store()` / `native_blob_reloc_load()` (hb_runtime.c), gated on
  `MACRUNNER_HB_CACHE_RELOC=1`, with the round-trip self-check retained and now exercising the
  new load path, plus a sentinel-collision guard that makes the load scan unambiguous.
- Persistent key carries `HB_PERSIST_FLAG_RELOC` so the two A/B arms cannot read each other's
  entries — a reloc-stored blob can hold sentinels in x2/x3/x4 that the legacy load never
  restores.
- Nine `rl_*` counters. **The summary buffer had to go 1024 → 4096**: the line now carries 32
  `%llu` fields and would have overrun. The overflow branch used to `return 0` — the trap this
  lane already paid for twice — so it now prints `...-TRUNCATED: need=N have=M` instead of
  vanishing. `reset_for_test()` was also missing every `mh_*`/`reloc_*` counter; added.
- Builds clean under `-Wall -Wextra -Werror`.

**No regression, checked against HEAD rather than assumed.** A HEAD-baseline copy of the
hyperbridge tree (my four files reverted) was built and run beside the modified one: baseline
455/29, modified 457/27, and the failure sets differ by nothing that appears in mine and not in
baseline. The 27 failures are pre-existing, from other lanes' uncommitted work in the tree.
**The suite is flaky** — three runs gave 455/29, 456/28, 457/27 — so the sound claim is *no new
failures*, not "fixed two".

The unit suite is not a vehicle for proving the cache: only **one** block reaches the JIT
dispatch path in the whole run (`compile_count=1`). Two passes over a shared cache root proved
nothing. The mechanism can only be exercised by a real x86 workload, so the proof is the HK run.

`scripts/hb-check-reloc-invariant.sh` — the soundness of the whole design rests on "a host
pointer only ever reaches x1/x2/x3/x4/x23", which was a grep I ran by hand. It is now a
mechanical check, verified with a negative control (injecting
`emit_mov_imm64(buf, 5, (uintptr_t)p)` makes it exit 1; the clean tree exits 0, 46 sites). If
that invariant ever breaks, the cache would restore a stale pointer with nothing crashing at
either store or load time.

### A second thing the arithmetic already says: the warm target has a floor that is not translation

The given budget is prefix sync 17 s · wine start 18 s · to Unity init 21 s · Mono load 232 s ·
language 124 s · menu 61 s. The first three sum to **56 s before a single line of Mono runs**.
The warm goal is 60 s. So even a cache with 100 % retention and zero translation cost cannot
reach it — there would be 4 s left for Mono load, language and the menu scene.

The 17 s is not a user-visible cost at all: `mr-run.sh` builds a throwaway prefix per run and
syncs the wine DLL set into it (the prefix *template* is only 1.9 MB, so the copy is not the
cost — the DLL sync is). A user launching the game a second time does not pay it. So the warm
number this lane is graded on is partly a property of the measurement harness.

I am not adjusting the target on my own reading of it. The A/B records `unity_init` per run, so
the next iteration will have the floor measured rather than inferred, and I will report it
against the 60 s goal explicitly.

### Status — measuring

Slot was held by a foreign run (`laneA-BLACKFRAME-DRAWTRACE-a1-try1`, pid 10408) for the first
~18 min; compiled through it and waited to deploy rather than disturb it.

`ntdll.so` relinked against the new hyperbridge and deployed after the slot freed:
`589c6f2a…` → `6eaa1555…`, verified by SHA **and** by `strings` (gate string present), never by
build exit code — `build-wine-arm64ec-spike.sh` does not build hyperbridge at all.

`scripts/hk-reloc-cache-ab.sh` now running: arms base (multi-helper, reloc off) and reloc, each
cold (empty cache root) then warm (reused), 600 s cap. It also turns on
`MACRUNNER_HB_TRACE_SYNCMETER=1`, which already exists and prints every ~1 s, so **lever 2 gets
measured from the same four runs for free** — rate vs latency, and timed-out vs signalled, which
is what separates "the guest is polling" from "a genuine block or a lost wakeup".

### Lever 2 measured for free — and the brief's reading of it does not survive

Another lane (`laneA-SYNCBLOCK-a1-try1-183638`) took the slot 50 s after my deploy, so my A/B is
queued behind it on mr-run's slot mutex. Two things follow, and the second is the useful one.

First, the deploy timing is clean: I deployed at 18:35:48 and their run started 18:36:38, so no
binary was swapped under a live run. Their run does execute my `6eaa1555` ntdll — with
`MACRUNNER_HB_CACHE_RELOC` unset, which is the legacy path plus two inert additions (extra
registers recorded into a side table, extra counters). Behaviour with the flag off is unchanged.

Second, their run carries the syncmeter, so lever 2 is now measured rather than sampled — 220
one-second windows over 316 s:

| | |
|---|---|
| wait ops | 36 526 |
| total blocked, summed across threads | **378.3 s** (in a 316 s run ≈ 1.2 threads parked at all times) |
| avg latency per wait | 10.4 ms |
| **timed out / signalled** | **10 / 36 516** |
| **INFINITE / finite timeout** | **36 516 / 10** |

**Essentially every wait is an INFINITE-timeout wait that gets signalled.** That rules out the
two failure modes the meter was built to separate: the guest is not polling on short timeouts,
and these are not lost wakeups — a lost wakeup does not get signalled. The wait implementation
is not doing anything wasteful. The time goes to *whatever the signalling thread is doing before
it signals*.

So "not translation, not fixable by any cache" is an assumption, not a measurement, and it is
probably wrong: a consumer blocked on a producer inherits the producer's cost, and on a cold
start the producer's cost is dominated by translating code it has translated before. Lever 2 may
not be a second lever at all — it may be lever 1 seen from the waiting end.

**PREDICTION, registered before my arms run:** if that is right, the reloc arm — which retains
far more translations — shows materially *lower* total blocked seconds and lower avg wait
latency than the base arm, without anything in the wait path changing. **What refutes it:**
retention rises sharply while blocked seconds stay flat. That would mean the waits are bound by
something else entirely (I/O, the graphics path, a genuine serialisation), and lever 2 would
then deserve its own work rather than riding on the cache.

The harness already records `sync_blocked_ms`, `sync_avg_latency_us` and the
timed-out/signalled split per arm, so this costs no extra runs.

### Why a persistent cache helps a COLD start at all — the mechanism, from the same free run

This needed saying, because on the face of it lever 1 should do nothing for a cold start: an
empty cache has nothing to load. The foreign run says otherwise, and the reason is in two of its
counters.

`open_ok=55` — the cache was opened **55 times in one process**. HB creates a JIT runtime per
guest thread (115 of them have been counted on HK before), and each has its own in-memory block
cache. The persistent cache is therefore the *only* channel through which one thread can reuse a
translation another thread already paid for. That is confirmed by the same run reporting
**hits=25623 against a cache that started empty** — every one of those hits is one thread
reading another thread's work inside a single cold run.

So retention is not only about the second launch. On a cold start it decides how many of the
135 497 compiled blocks get translated once versus once per thread. That is what makes "cold
start under 120 s" reachable through this lever at all, and it is why the argshape/widearg
rejections — which throw away 68 % of exactly this shared material — are expensive twice over.

### Harness correction, and a second deploy

The first A/B attempt was stopped before it produced anything, because I found a flaw in my own
harness worth more than the runs it would have produced: `laneA-run-hk.sh` waits only 300 s for
the title slot and I had passed `max_tries=1`. Another lane holding the slot for longer would
have turned a delayed arm into a **missing** arm — and an A/B with one arm missing is worse than
none, because the surviving number still reads like a result. The wait now lives in my script,
polls up to 2 h, and retries twice (the `exit=53` xtajit64 boot flake).

Killed only my own three PIDs, verified by process tree, and confirmed the other lane's run was
still alive afterwards.

Redeployed with the sign-extension fix included: `6eaa1555` → **`e999d81b`**, verified by
`strings` for both the gate and the new `rl_highhalf` counter. Arm B therefore measures the
refined classifier, and `rl_highhalf` will say what the 2^48 ceiling was worth on its own.

### ⚠ Disk — for the operator, flagged per the standing rule

**29 GB free against the project's 30 GB floor.** `disk-guard.sh --check-only` reports
"BELOW THRESHOLD" but exits 0, so the rule's hard STOP (exit 2) has not tripped and I am
continuing; the A/B needs perhaps 1–2 GB.

I did **not** run the full `disk-guard.sh`. Its rolling report cleanup does guard against active
runs (`find "$d" -mmin -30` → skip), which is good, but nothing in it guards
`artifacts/_mr-run.*`, and another lane's run currently owns `artifacts/_mr-run.7M3Tzi`. Running
a blanket cleaner over a live foreign prefix is the one class of action this lane is told to
stop and escalate on rather than decide alone.

What I could clean safely, I did: three translation-cache roots keyed to ntdll builds that are
no longer deployed and untouched for 2+ days (133 MB). `reports/phase-h` had **zero** dirs older
than 7 days, so the usual reclaim is already exhausted. The 20 GB in `reports/` is mostly recent
run dirs belonging to other lanes — not mine to prune.

### State, and what the next thread should do

**Armed and waiting on the slot.** `scripts/hk-reloc-deploy-when-slot-free.sh` is running in the
background: it polls for a free title slot, deploys `e999d81b`, then runs the four-run A/B at a
**900 s** cap (raised from 600 — the cold baseline is 476 s, and a control arm that overruns its
cap reports `NOT_REACHED` and settles nothing). Another lane's run
(`laneA-SYNCBLOCK`, mr-run pid 35148) has held the slot since 18:36; it is legitimately live —
its `mr-run.sh` parent is alive and its run.log is still being written — just idle in a wait, so
I am queuing rather than touching it.

Results land in `reports/research/hk-reloc-cache-ab-<stamp>/*-phases.txt`, one file per arm/pass.

**Read these first, in this order:**
1. `rl_stores` in the reloc arm. If it is **0**, the flag did not reach the guest process and
   nothing else in that arm means anything — fix the plumbing before reading a single timing.
2. `RETENTION_PCT` per arm — the prediction is 62 % → >90 %.
3. `mh_toomany` / `mh_widearg` / `mh_argshape` in the reloc arm — all three should be **0**,
   because that path does not run at all any more.
4. `rl_hostptr` — the one decline that describes the guest code. If it is large, the next step is
   to split it: values inside a mapped guest region are stable and could be left as literals,
   which would need the guest memory map consulted at store time.
5. `rl_highhalf` — what the 2^48 sign-extension ceiling was worth on its own.
6. `MENU_SECONDS` cold and warm, and `unity_init` (the pre-Mono floor, expected ≈56 s — see the
   warm-target section above).
7. `sync_blocked_ms` per arm — tests the lever-2 prediction registered above at no extra cost.

**If retention rises but the cold start does not move**, the lane's premise is wrong and the next
iteration belongs on phase attribution, not on the cache: `unity_init` / `mono_begin` /
`language` / `menu` are all recorded per run, so the phase that actually holds the 476 s can be
named without another instrument.

---

## Iteration 2 — 2026-07-29

The A/B from iteration 1 is running (deployed `e999d81b` cleanly at 19:39 after an 11 min slot
wait; arms base/reloc × cold/warm, 900 s cap). While it was queued I mined a *foreign* lane's
live run — `laneA-SYNCBLOCK-a1-try1-183638`, which carries my `6eaa1555` ntdll and therefore my
counters — rather than spend a slot. It answered the lane's central question, and the answer is
not the one the brief assumes.

### Housekeeping first

Two `laneA-run-hk.sh RELOC-a1` shells from iteration 1's abandoned attempt were still alive at
ppid 1, 67 min after their run had exited 143. They would have raced my A/B for the title slot.
Killed by verified PID (55102/88989/88990) — mine, tagged RELOC, run dir dead since 17:49.

Iteration 1's deploy driver had been launched with a bare `&`, so it died with that thread.
Relaunched as a tracked background task; it did the deploy and started the A/B on its own.

### The measurement — a new tool, `scripts/hk-boot-phases.py`

I could not edit `hk-reloc-cache-ab.sh` (it was already running), so the analysis lives in a
standalone script. That turned out to be the right shape anyway, because it can be re-run over
any historical run.log — which is how the cross-run table below exists at zero slot cost.

It also fixes a flaw that would have wasted the whole A/B: **the harness grades arms on
`MENU_SECONDS`, and no recent HK run reaches a menu.** Triage `LADDER_BEST` tops out at 11
(swapchain) / 13 (rtv) across every recent run dir, and the two markers the A/B greps for —
`Loaded saved language code`, `Making UI menu lean` — appear in none of them. All four arms would
have reported `NOT_REACHED`. The replacement metrics are markers that actually fire (verified
against a real log, not guessed) plus a *rate*: `macrunner-hb-rcache` prints every 200 000 hits,
so its slope measures guest execution speed and exists in every run regardless of how far it got.

### Finding 1 — the boot has a deterministic 148 s prefix, and a nondeterministic tail

Eight independent runs, different lanes, different configs:

| | spread across 8 runs |
|---|---|
| `Begin MonoManager` | 51.9 – 62.1 s |
| PhysX-ready → `Using XInput` | **56.3 – 63.1 s** |
| `Using XInput` reached at | **144.3 – 156.0 s** |
| last guest activity | **148.7 · 193.1 · 218.9 · 588.6 · 841.3 · 1207.3 · 1356.9 · 1426.3 s** |

Everything up to XInput is reproducible to ±5 %. Everything after it is unbounded. The budget in
the brief does not survive this: measured `MonoManager ReloadAssembly` is **32.6 s**, not a 232 s
"Mono load phase". Decomposition of the deterministic part (SYNCBLOCK, representative):

| phase | s |
|---|---|
| prefix sync (harness only — a user relaunching does not pay it) | 13.7 |
| wine start + loader + xtajit64 → first guest output | 27.0 |
| Unity init incl. GfxDevice + D3D11 | 12.5 |
| MonoManager ReloadAssembly | 32.6 |
| **PhysX-ready → XInput** | **58.5** |
| **to XInput** | **144.3** |

### Finding 2 — translation is not the cold-start cost, and lever 1's ceiling is small

The progress line prints every 5000 compiles, so its spacing *is* the compile rate:

| window | compiles | rate |
|---|---|---|
| t=40 → 56 s | 82 574 | 5 000 – 25 000 /s |
| t=56 → 91 s | 26 344 | ~700 /s |
| t=91 → 244 s | 31 548 | ~200 /s |
| **t=244 → 492 s** | **4 977** | **20 /s** |

71.7 % of all 145 443 compiles land in the first 50 s. Retention is **flat at 71–75 % the entire
run** — the cache behaves identically throughout, so the collapse is demand, not the translator
degrading. `hits` freezes at 25 623 from t=156 onward: nothing after that is a repeat.

So the whole translation phase is ~50 s of a 476–590 s boot, and the cache can only recover the
duplicate fraction of it. **Lever 1's ceiling on a cold start is single-digit seconds.** That is a
bound on the thing this lane was told was the main lever.

### Finding 3 — lever 2 is an idle worker pool, not a stall

3540 logged blocks decompose exactly: **9 tids × 443 each, on 9 distinct handles × 443 each, from
3 caller addresses (one is 88 %)**. Every wait INFINITE-timeout and signalled (36 516 / 36 526).
Nine workers each parked on their own handle in lockstep is a thread pool with no work — not a
convoy, not a lost wakeup, not translation seen from the waiting end. Shrinking those waits cannot
move the boot. (The fix would live in `macrunner_hb.c` anyway, which is outside this lane.)

### Finding 4 — the reference run did not finish slowly, it deadlocked

`ps -M` on the live process: ~0.5 s CPU total across 21 min, log dead for 12 min. `sample`:
**49 threads, 0 runnable** — 43 in `__ulock_wait2`, 4 in `mach_msg2_trap` (main thread parked in
`[NSApplication run]` → `mach_msg2_trap`), 2 idle workqueue. Fully wedged at t≈589 s.

This matters for the goal: "476 s to menu" is a number measured inside a regime that can stop
dead. Reported for the SYNCBLOCK lane's attention; I did not touch their process.

### Finding 5 — guest file reads are slow, but are not the phase cost

`globalgamemanagers`: 232 reads × 7168 B = 1.66 MB over 38.1 s ≈ 42 KB/s, p50 inter-read gap
17 ms, p90 22 ms. The one 32.7 s gap is ReloadAssembly happening between two reads, so the reads
are interleaved with the real work rather than being it. Noted, not chased.

### PREDICTION — registered before the arms land, superseding iteration 1's

Iteration 1 predicted "cold start strictly faster than 476 s". On the evidence above I now expect:

- `rl_stores` > 0 and `mh_argshape`/`mh_widearg`/`mh_toomany` → **0** in the reloc arm (the
  mechanism works — this part of iteration 1's prediction stands).
- `RETENTION_PCT` 74.6 % → **> 90 %**.
- **`xinput` (the deterministic prefix) moves by less than 5 s between arms** — i.e. within the
  ±5 % that eight runs already show without any cache change at all.
- Warm pass: cache hits rise sharply, `to_guest_s` and `xinput` still ~unchanged.

**What would refute me:** the reloc arm reaches XInput materially sooner — say < 130 s against the
base arm's ~148 s. That would mean cross-thread translation sharing does gate the early boot after
all, and lever 1 deserves the rest of the lane. I do not expect it, and I would rather be wrong
here than keep spending slots on a lever whose ceiling I have now bounded.

### What the next thread should do

1. Run `scripts/hk-boot-phases.py` over the four arm logs in
   `reports/research/hk-reloc-cache-ab-20260729-193914/` (the A/B's own summary greps for
   `MENU_SECONDS`, which will read `NOT_REACHED` for every arm — that is expected, not a failure).
2. Read `RETENTION_PCT` and `rl_*` to confirm the mechanism, then `xinput` to test the prediction.
3. If the prediction holds, **the lane's target is the 148 s deterministic prefix**, whose largest
   block is the 58.5 s PhysX-ready → XInput stretch, followed by 32.6 s ReloadAssembly and 27 s of
   wine start. None of those are translation-cache work. The 58.5 s stretch contains a burst of
   **15 new JIT runtimes at t≈113** (15 `translation-cache-open` in 1.6 s) — per-thread runtime
   construction is in `hb_runtime.c`, which *is* this lane's territory, and is the first thing I
   would instrument.
4. Note for the goal: the deterministic prefix alone is 144 s against a 120 s target, and 130 s of
   it is not translation. Reaching 120 s means attacking wine start / ReloadAssembly / the
   post-PhysX stretch, or establishing that the 476 s baseline was measured in the
   nondeterministic regime and the real cold start is ~148 s plus a tail.

### Finding 6 — the guest throughput cliff, and then the actual bottleneck

The rcache counter prints every 200 000 hits, so its spacing is guest execution speed. Across the
reference run:

| phase | rcache hits/s |
|---|---|
| ReloadAssembly (t=53–86) | 528 000 |
| asset reads (t=86–112) | 324 000 |
| silent pre-XInput (t=112–144) | 527 000 |
| post-XInput (t=144–200) | ~380 000 |
| **t≈202 onward (386 s, to the deadlock)** | **30 000 – 55 000** |

**Guest execution falls ~8.8× at t≈202 and never recovers.** The `inproc_wait` rate falls only
1.48× over the same instant (190/s → 128/s, from the msync-diag counter's own spacing), so work
done per wait falls ~4.5× — the process is not simply blocking more.

I first read the `MULTIPLE-BLOCK (server-registered wait)` line as the trigger, because it lands
at t=201.5. It is not: it prints every 4000 waits, so it is a periodic counter report. Checked
before relying on it.

**Then I sampled a *live* foreign run (`INPUT-ACT`, pid 971) at 92.7 % CPU, 502 s in — inside the
slow regime rather than after it.** The critical-path thread:

```
2795  macrunner_hb_x64_thread_entry
 1009   macrunner_hb_call_import_thunk
  996     macrunner_hb_try_kernel32_handle_semantic
   918       macrunner_hb_sync_virtual_region + 532
    913         hb_memory_protect + 820,828,…     <- LEAF, no children
```

Self-sample totals for the whole process: **`hb_memory_protect` 913**, `hb_memory_read` 164,
`hb_memory_write` 158, `find_region_normalized` 102. The other two busy threads are parked in
`__ulock_wait2` — waiting on this one.

`hb_memory_protect` is **33 % of the critical-path thread, and more than every guest memory read,
write and region lookup in the entire process combined** — despite VirtualProtect being far rarer
than memory access. That ratio is only possible if the list being walked is long.

The leaf has **no child frames**, which identifies the line exactly: `split_all_regions_at` is
O(log N) through the treap and would show `find_region_normalized` as a child, and the treap
lookup at the head of the function shows up separately. What is left is the full-list walk at
`hb_memory.c:1073` — `for (r = mem->regions; r; r = r->next)` — whose body, for the regions it
skips, is a bare compare-and-continue with no calls. The file's own comment two paragraphs above
records that this same class of O(n) walk "dominated the swapchain creator thread before its
first Present" and was fixed *for the single-region case only*; the multi-region fallback kept
the walk.

### The fix, and how it was verified without the slot

`mem->region_tree` is a treap keyed by base, so the same set of regions is reachable in
O(log N + k). Replaced the walk with an in-order range traversal that prunes both subtrees
(`protect_range_visit`). The visitor only edits `r->perm` and host protection and never changes
the tree shape, and the two splits have already run, so no region straddles a boundary.

Builds clean under `-Wall -Wextra -Werror`. **Not deployed yet** — an A/B was live, and replacing
`ntdll.so` under a running game invalidates its numbers.

The repo suite could not verify this. Its memory tests map *raw stack addresses*
(`&code`, `&wide`, hb_test_runner.c:5301) as guest regions, so ASLR moves them every run: five
runs of the unmodified tree gave 26/28/28/28/29 failures, union 46, stable 27. Comparing single
runs — which is what I did first — is meaningless here. Over 5 runs each, the fix's stable
failure set (26) is a strict **subset** of baseline's (27). That is "no regression", not "fixed
one"; the difference is one of the ASLR-dependent tests.

So I wrote a deterministic differential test instead —
`engine/hyperbridge/tests/hb_memory_protect_range_test.c`, fixed guest addresses, no host
pointers — covering 64 single-page regions with a mid-range protect, splits at both boundaries,
a range spanning a **hole**, the not-found case, and **descending insertion order** (the
order-independence the list walk had by accident and the treap walk must keep). It prints a
per-page permission digest. Old and new produce **byte-identical output on all five cases**,
`failures=0` both.

### Slot contention, and a re-prioritisation

The cache A/B never ran: its first arm waited 50 min while another lane's run held the title slot,
and three lanes are competing for one slot. Killing it does not gain slot access — but it changes
what runs when the slot frees, and on the evidence the memory path is the much larger lever. So I
stopped my own two PIDs (verified the foreign run survived) and queued
`scripts/hk-memprotect-ab.sh` instead.

What that costs: the cache A/B's *base* arm. That is affordable, because a real HK run on the same
build family with reloc off already measured it — retention 74.6 %, xinput 144.3 s. Arm A below is
exactly the reloc-cold arm that A/B would have produced, so the cache question is still answered,
by one run instead of four.

Both arms differ by **one file**: A = `e999d81b` (walk), B = `76a8c330` (treap walk), both with the
reloc cache on and their own empty cache root. Artifacts staged and verified by SHA and by the
presence of the reloc gate string before either run starts.

### PREDICTION — registered before the memprotect arms run

- **Post-cliff throughput rises materially**: `cliff_rate_after` was ~41 000 rcache hits/s in the
  reference run; B should be well above A. This is the number the fix targets.
- `cliff_ratio` shrinks in B (8.8× in the reference run).
- The deterministic prefix (`xinput` ≈ 148 s) moves **little** — the sample was taken at t≈500 s
  and I have no evidence this path is hot before XInput. If it does improve, that is a bonus, not
  the prediction.
- `RETENTION_PCT` in both arms > 90 % (the reloc mechanism, inherited from iteration 1, against
  the 74.6 % measured with it off), and `rl_stores` > 0 — if `rl_stores` is 0 the flag never
  reached the guest and nothing else in either arm means anything.

**What would refute me:** B's throughput is indistinguishable from A's. That would mean the walk
is not the cost — that the 913 samples are many short calls rather than few long ones, and the fix
addresses per-call cost that is not there. The next step would then be to count calls and region
count directly rather than infer N from a sample ratio, which is the one inference in this chain
that is not a direct measurement.

### What the next thread should do

1. Read `reports/research/hk-memprotect-ab-<stamp>/{A,B}-phases.txt` (written by
   `scripts/hk-boot-phases.py`, which now reports `cliff_t` / `cliff_ratio` /
   `cliff_rate_before` / `cliff_rate_after` automatically).
2. If B wins, this is the lane's result and `hb_memory_protect` is worth a second pass — the same
   O(N) shape is still present in `hb_memory_sync_live_range` (hb_memory.c:718, a nested scan) and
   in `any_overlap` (:418).
3. If B does not win, count `hb_memory_protect` calls and `mem->regions` length directly before
   touching anything else.
4. `MENU_SECONDS` will read `NOT_REACHED` in both arms. That is expected — no recent HK run
   reaches a menu — not a harness failure.

---

## Iteration 3 — 2026-07-29

Iteration 2's A/B produced **nothing**. Both of its failures are now fixed at the root rather
than worked around, and the central claim it was built to test — that the `hb_memory_protect`
walk is O(N) and expensive — has been **measured off-slot**, so the scarce title slot now only
has to supply the one number a bench cannot: the real region count.

### Why iteration 2's A/B died, and both causes

**Cause 1 — the driver died with its thread.** It had been launched with `nohup … &`. On macOS
there is no `setsid`, so the process stayed in the session's process group and went down with it,
mid-A/B. Fixed by launching through Python's `start_new_session=True`, which really does call
`setsid(2)`; the driver now runs at ppid 1 in its own process group. Verified by `ps` showing
`pgid == pid`, not assumed.

**Cause 2 — arm A never reached wine, and still wrote a run dir.** `A.stdout` said
`try1 rc=2`, and the run dir's `run-contract.json` says it all:

```
status = BLOCKED
blockers[0] = application.save_snapshot_manifest_sha256 : path_absent
```

Without `MACRUNNER_MR_RUN_PREFIX_TEMPLATE`, `mr-run.sh:403` falls back to
`find_warm_prefix_template`, which picks `artifacts/warm-prefix/laneA-fullybooted-20260626-121640`
— a 1.9 MB registry-only template with **no `drive_c` at all**, hence no HK save directory, hence
a blocked contract and an exit at +24.7 s. The lane that was passing (`INPUT-ACT2`, which reached
`time_to_swapchain=153s` on the same binary) uses
`artifacts/hk-prefix-template-NOSERVICES`, exported by `scripts/hk-run-try12-config.sh:103`.

This is the trap worth recording: **a run that dies before wine still creates a run dir, a
run.log and a triage summary**, so it reads like a run that failed to get far rather than one that
never started. The new harness greps for `run-contract-ledger status=BLOCKED` and reports
`DEAD ARM … not a measurement` instead of handing the number on.

**And setting the template was not enough — the first relaunch died the same way.** With the
template set, the save blocker cleared and three more appeared:
`runner.branch_map.{actxprxy,crt_case_fusion,wwise_observer} = branch_input_absent`. These are
branch *decisions*: mr-run refuses to run unless they are passed explicitly, and "absent from a
working run's environment dump" does not mean "safe to omit". The guard did its job — the arm was
reported as a DEAD ARM rather than measured — but four arms would have died identically.

The real fix is to stop reproducing another script's environment by hand. `hk-run-try12-config.sh`
is the configuration of the run that reached the menu on 2026-07-28; it sets the template, all
three branch gates, the DXMT overlay wiring and the JIT flags, and **its own header documents this
exact failure** ("a ~1250-line log and never reaches wine … that is NOT the exit=53 boot flake and
retrying it cannot help" — my dead run.log was 1256 lines). The harness now delegates to it with
`max_tries=1`, so every arm is launched exactly the way the reference run was, and the arms cannot
differ in attempt count. The general lesson, which that file states and this lane just re-paid
for: **a run-contract records the final CHILD environment, including values the wrapper derives —
feeding derived values back in as inputs reproduces the symptom, not the run.**

The deploy gate now also treats a live `dist-arm64ec-spike` wineserver as a busy slot, not just a
live guest: swapping `ntdll.so` under any process that will map it invalidates that run. It stays
scoped to this dist — a sibling lane's wineserver under `engine/wine/dist` does not load this
artifact, and counting it would wait out the full timeout against a free slot.

### The A/B is now one binary and two environment variables

`MACRUNNER_HB_MEMPROTECT_WALK=list|tree` selects the traversal at runtime (default `tree`).
The per-region body was extracted into `protect_apply_region()` and is shared **verbatim** by
both traversals, so the arms cannot drift apart in anything except how regions are reached.

That removes the deploy from inside the A/B window — the one action this lane must never take
under a live run — and makes "which build produced this number" true by construction rather than
something to prove afterwards.

`tests/hb_memory_protect_range_test.c` gives **byte-identical** permission digests under both
arms on all five deterministic cases (`failures=0` both), so a timing difference is traversal
cost and nothing else.

### The instrument: `macrunner-hb-memmeter`, and three deliberate choices

Iteration 2 read the 913-of-2795 `sample` hit as "the walk is long". That is an inference with
two unmeasured factors in it — *which branch* was hot, and *how long the list is*. The memmeter
measures both. Three properties, each chosen against a specific way this lane has already lost
data:

- **Always on, not env-gated.** Three lanes compete for one HK slot, so every foreign lane's run
  must yield this data for free. Cost: relaxed atomic adds, and the clock read on 1 call in 64.
- **Time-based cadence (5 s), not count-based.** A count trigger has to guess the call rate:
  at 262144 calls a run calling protect 100×/s emits its first line 45 minutes in, i.e. never,
  while a run calling it a million times a second drowns the log. Both failures are silent.
- **`fprintf`, not `snprintf` into a fixed buffer** — the 1024-byte buffers in this tree return
  SILENTLY on overflow, and this lane has already paid for that twice.

One flaw in my own first version, found and fixed before it could distort anything: incrementing
`mm_mp_visit` per visited node charges the list arm N contended atomic RMWs per call against the
tree arm's ~log N, i.e. **the instrument would have manufactured part of the very difference it
was measuring**. Visits and applies now accumulate in the per-call stack struct and fold into the
globals once.

### The measurement that did NOT need the slot

`tests/hb_memory_walk_bench.c` builds N Wine-owned live regions with
`hb_memory_sync_live_range` — faithful because those are `allocated=false, is_guest32=false`,
the class HK's hot protect traffic actually walks; building them with `hb_memory_map` would make
every visited region issue a real `mprotect(2)` and the syscall would swamp the traversal.

3000 multi-region protects per point, both arms, same binary:

| regions | tree ns/call | list ns/call | ratio |
|---|---|---|---|
| 64 | 116.7 | 192.0 | 1.6× |
| 256 | 165.3 | 610.0 | 3.7× |
| 1 024 | 258.0 | 2 582 | **10.0×** |
| 4 096 | 305.7 | 11 536 | **37.7×** |
| 16 384 | 390.0 | 37 802 | **96.9×** |

The list arm is clean O(N) at **≈2.3 ns per node visited**; the tree arm is O(log N) (117 → 390 ns
across a 256× range in N). **So the fix's entire value is a function of one unknown: N.** The HK
run now only has to report the region count, not settle an argument about complexity.

### A second O(N) path the bench exposed for free — not yet touched, deliberately

The bench's own setup made `hb_memory_sync_live_range` print its counters: `slr_calls=64`,
`slr_scan=4032`. That is **63 nodes scanned per call at N=64** — it walks the entire region list
on every call — and it calls `rebuild_region_tree` (a full O(N log N) reinsertion of every node)
on the replace path. At N≈13 000 the bench measured **220 µs per call**.

I did **not** fix it, and the reason is a real one rather than caution: `sync_live_range` inserts
its replacement at the list head with **no `any_overlap` guard**, so unlike everywhere else in
this file its regions can genuinely overlap — which is exactly why its inner `other_overlap` loop
exists. A treap range query assumes non-overlap and would be subtly wrong here. The memmeter
reports `slr_calls`, `slr_fast`, `slr_repl` and `slr_scan`, so the next run says whether this path
is hot in HK at all before anyone writes that code.

### The mechanism, proved off-slot: the map shreds itself, and that is what arms the walk

Two more bench cases, run while the slot was held by another lane. Together they explain a
throughput cliff that never recovers, which a plain "this function is O(N)" story cannot.

**(a) The region map shreds itself into one region per page.** Start with ONE live region of
16 384 pages and issue single-page protects with alternating permissions:

| protects | regions | ns/protect (tree) | ns/protect (list) |
|---|---|---|---|
| 3 000 | 3 001 | 216 | 208 |
| 12 000 | 12 001 | 253 | 251 |
| 18 000 | **16 384 (saturated)** | 256 | 282 |

Exactly one new region per protect, and it never comes back down — it stops at 16 384 only
because every page is now its own region and there is nothing left to split. **N is bounded only
by the guest's page count.** And note both arms are flat here: a sub-range protect takes the
split-then-re-enter path and lands on the exact-fit return, so it never reaches the walk at all.
That alone would say the walk does not matter.

**(b) But the shredding changes what every LATER protect means.** Once the map is per-page
fragments, a protect covering k pages necessarily spans k regions, so it can no longer take the
exact-fit path — it falls through to the walk, over a list the shredding made enormous. Shred
first, then issue 8-page protects:

| regions before | tree ns/protect | list ns/protect | ratio |
|---|---|---|---|
| 1 | 374 | 6 973 | 18.6× |
| 1 001 | 397 | 10 369 | 26× |
| 4 001 | 433 | 27 381 | 63× |
| 16 000 | **446** | **105 519** | **236×** |

**105 µs per 8-page protect** in the shipped code, against 446 ns with the treap walk — and the
fixed cost is essentially flat across a 16 000× range in N (374 → 446 ns).

This is self-reinforcing: shredding raises N, a higher N means more protects span multiple
regions, and each of those now costs O(N). That is the shape a cliff that never recovers has to
have, and it is why the fix's value could not be read off the region count alone.

**It also names the next fix, and the measurement rather than a hunch justifies it.** The treap
walk makes the *lookup* cheap but leaves N growing to the guest's page count, which still charges
every remaining O(N) path in the file — `hb_memory_sync_live_range` (a full list scan on EVERY
call plus an O(N log N) `rebuild_region_tree` on the replace path), `any_overlap` (:418, on both
map paths), and `remove_region_node`. Coalescing adjacent regions with identical
perm/flags/backing would hold N down and fix all of them at once. It needs real treap deletion
(today only `rebuild_region_tree` can remove a node, which is O(N) and would defeat the point),
so it is a genuine change to a load-bearing structure — worth doing on evidence, not before it.
The memmeter's `slr_calls`/`slr_scan` and `regions` decide whether it is worth that risk.

### PREDICTION — registered before the arms land

1. **`regions` is in the thousands and grows monotonically across the run.** Nothing in
   `hb_memory.c` ever merges two adjacent regions — I checked the whole file for
   merge/coalesce/adjacent and got **zero hits** — while `split_region_at` callocs a new one on
   every sub-range protect. `splits` should climb steadily.
2. `WALK_LEN` ≈ `regions` in the list arm; far smaller in the tree arm.
3. `MP_NS_PER_CALL` list ≫ tree, by roughly the bench ratio at the observed N.
4. `EST_TOTAL_PROTECT_S` (summed across threads): tens of seconds in the list arm.
5. `cliff_rate_after` higher in the tree arm — the fix targets the post-cliff regime.
6. `xinput` (~144 s deterministic prefix) moves **little**: the original sample was taken at
   t≈500 s and there is no evidence this path is hot before XInput.

**What refutes me:** `WALK_SHARE ≈ 0` — i.e. almost no protect call reaches the multi-region walk
at all — or `regions` staying small (< 500). Either would mean the 913 leaf samples are something
else inside that function, and the whole memory-path framing needs re-deriving rather than
extending. This is a genuine possibility: the bench forces 100 % walk share by construction, and
HK's real mix of fast/re-enter/exact/walk paths is exactly what has never been measured.

### RESULT — arm `list` (the shipped code), first 66 s of a real HK boot

Neither refutation condition fired. The synthetic mechanism reproduces on the real workload, and
it does so **inside the deterministic prefix**, not in the post-cliff tail where it was found:

| | measured |
|---|---|
| `regions` | 18 → **16 221**, monotonic across 13 reports |
| `splits` | 19 → 8 880 |
| `WALK_SHARE` | **40.4 %** of protect calls reach the multi-region walk |
| `WALK_LEN` | **9 538** nodes per walk, and still climbing at t=66 s |
| `MP_NS_PER_CALL` | **9 571 ns** |
| `MP_CALLS_PER_S` | 11 007 |
| `EST_TOTAL_PROTECT_S` | **7.0 s within a 66 s span**, summed across threads |

The growth curve is the mechanism, visible directly:

| t (s) | regions | walk_len |
|---|---|---|
| 0.0 | 18 | 13.7 |
| 11.4 | 385 | 161 |
| 21.5 | 4 004 | 1 750 |
| 36.5 | 9 818 | 5 293 |
| 66.1 | 16 221 | 9 538 |

`walk_len` tracks `regions` almost exactly — that is the O(N) walk, measured rather than inferred,
and it disposes of the possibility that the 913 leaf samples were something else in that function.
Note the run had not finished: N and walk_len were **still rising** at t=66 s, so the t≈500 s
regime where the original `sample` was taken is worse than these numbers, which is consistent with
`hb_memory_protect` being 33 % of that thread.

**And it accelerates.** The same arm re-read at t=253.6 s, still mid-run:

| at t= | regions | WALK_LEN | WALK_SHARE | MP_NS_PER_CALL | est. total in protect |
|---|---|---|---|---|---|
| 66.1 s | 16 221 | 9 538 | 40.4 % | 9 571 ns | 7.0 s of 66 s (**10.6 %**) |
| 253.6 s | **43 050** | **19 263** | 49.2 % | **48 311 ns** | **59.2 s of 253.6 s (23 %)** |

**23 % of wall-clock inside `hb_memory_protect` alone, and every term still rising.** A single
protect call now averages 48 µs. This is the cliff, and it is not a metaphor for one: N doubles,
walk length doubles with it, per-call cost quintuples, and nothing brings N back down.

Against the bench's cost curve — the treap arm is **flat at ~450 ns from N=1 to N=16 000** — the
fix should turn 1 225 344 calls × 48 µs ≈ 59 s into roughly 1 225 344 × 450 ns ≈ **0.6 s**. The
`tree` arm measures exactly that, and the prediction is now quantitative rather than directional.

(`slr_calls=15` at t=253 s confirms the earlier reading at greater length: `sync_live_range` stays
cold even as its per-call scan length reaches 40 526 nodes. It is a loaded gun that is never
fired — worth a comment in the source, not a rewrite.)

**Two leads died here, cheaply, which is the point of putting all three instruments in one deploy:**

- **`hb_memory_sync_live_range` is NOT hot: `slr_calls=6`.** The O(N) scan and O(N log N) rebuild
  I declined to rewrite are called six times in 66 s. Iteration 2's "second pass" suggestion and my
  own interval-tree sketch are both **dead** — do not spend a slot on them. `any_overlap` is worth
  a glance for the same reason, but on this evidence the map-mutation paths are not the cost.
- **Per-thread JIT runtime construction is NOT the 58.5 s block: 46 runtimes, 0.29 s total**
  (mean 6.3 ms, max 11.6 ms). All of it is `hb_cache_open`; the 128 MB JIT buffer costs
  **0.0 ms** because the mmap is lazy. Iteration 2's "first thing I would instrument" is answered
  and negative. The 58.5 s PhysX→XInput block remains unexplained and is still the largest
  unattributed item in the prefix.

### No regression, checked rather than assumed

The unit suite is ASLR-flaky (it maps raw stack addresses as guest regions), so single runs are
meaningless. Over 5 runs the instrumented build's **stable** failure set is 26 — byte-identical to
the pre-instrument fix build's 26, and a strict subset of HEAD-baseline's 27, with **zero**
failures present in mine and absent from baseline. Builds clean under `-Wall -Wextra -Werror`.

### State

`scripts/hk-memwalk-ab.sh memwalk2` is running detached (ppid 1, own process group), waiting on
the slot. It deploys **`63c9691c`** once — verified by SHA **and** by `strings` for the memmeter
line, the rtmeter line, the walk selector and the reloc gate, never by build exit code — then runs
`list, tree, list, tree` at a 900 s cap, alternating so one run per arm cannot be confused with
machine drift. Results land in `reports/research/hk-memwalk-ab-<stamp>/*-memmeter.txt` and
`*-phases.txt`.

Three lanes (AUDIOCHECK, SYNCBLOCK, INPUT-ACT2) are competing for the same slot, so expect long
waits between arms. A partial series is still usable: each arm writes its own files as it
finishes.

**A third instrument rode along in the same deploy, because the slot was busy anyway and an extra
always-on counter is free once the binary is being replaced.** `macrunner-hb-rtmeter` times
`hb_jit_runtime_create` — HK builds one JIT runtime per guest thread (115 counted), each mmapping
a 128 MB JIT buffer and opening the persistent cache, and iteration 2 saw a burst of 15 inside the
**58.5 s PhysX-ready → XInput** stretch, the largest unexplained block in the deterministic prefix,
which nothing has ever timed. One line per construction (~115/run), reported by
`hk-memmeter.py` as `TOTAL_RUNTIME_CONSTRUCTION_S`.

### The arithmetic the operator should see, because it bounds what this lane can deliver

The goal is a cold start to menu under 120 s. The deterministic prefix measured over eight runs is
**144 s to XInput**, of which only ~13.7 s is prefix sync that a real user re-launching would not
pay. So ~130 s is spent before the nondeterministic tail even begins, and the menu is somewhere
past it. **No fix to the post-cliff regime can reach 120 s on its own** — the memory path, the
translation cache and the sync levers all live in the tail.

That is not an argument to stop; it is an argument about where the remaining work has to go. The
prefix decomposes as wine start + loader + xtajit64 27.0 s · Unity init 12.5 s · MonoManager
ReloadAssembly 32.6 s · **PhysX-ready → XInput 58.5 s**. The last is both the largest and the only
one with no explanation at all, which is why the rtmeter went in now rather than later. If region
shredding is already under way during ReloadAssembly — heap-heavy, and exactly the sub-range
protect traffic that shreds — the memory fix may reach into the prefix too; the memmeter's
per-report `regions` timeline is what says so, and it is printed for that reason.

**Read `WALK_SHARE` and `regions` first.** If `WALK_SHARE` is ~0, stop and re-derive — do not read
any timing below it. Everything else in that file is conditional on those two numbers. *(Both have
now been read on the `list` arm and both confirm the prediction — see the RESULT section above. The
outstanding number is the `tree` arm's `MP_NS_PER_CALL`.)*

⚠ **The working tree is AHEAD of the deployed binary, deliberately.** Deployed = `63c9691c`.
Since then the tree gained `mem=`/`maps=` on the memmeter line (the region map is per guest
thread, so a growth curve without a map identity interleaves dozens of lists) and an `any_overlap`
counter (the last unmeasured O(N) walk). **Do not relink and deploy while the A/B is running** —
that is the one action this lane must never take mid-series. Deploy them together after the four
arms land; `hk-memmeter.py` already reports `ovl_calls=ABSENT` rather than `0` for logs from
builds that predate the counter, so an old log cannot be mistaken for a measured zero.

Note for whoever reads a foreign lane's log after this deploy: the memmeter is always on, so
**every** HK run from now on carries region-map data. `scripts/hk-memmeter.py <run.log>` reads it,
and prints `memmeter=ABSENT` for logs that predate the instrument rather than silently reporting
zeros.

---

## Iteration 4 — 2026-07-29

### Housekeeping: the A/B was running twice, and half of it was dead on arrival

Two drivers were alive against one slot. `hk-memwalk-ab.sh` stamp **205651** had died, but left an
orphaned arm (`laneA-run-hk.sh memwalk-tree2`, pid 46315, reparented to init) sitting in a 300 s
slot-wait loop; stamp **210958** was the live, correct series. Two consequences, both bad:

1. The orphan competed for the slot with the live series, so each would have measured the other's
   machine load.
2. **Every arm the orphan produced was dead on arrival.** It invoked `laneA-run-hk.sh` *directly*,
   and that path does not set the three branch inputs the run contract requires. Its
   `run-contract.json` says exactly that:

   ```
   status = BLOCKED
   blockers = runner.branch_map.actxprxy        reason=branch_input_absent
              runner.branch_map.crt_case_fusion  reason=branch_input_absent
              runner.branch_map.wwise_observer   reason=branch_input_absent
   ```

   The run still wrote a run dir, a 226 KB `run.log` and a triage record, and still printed
   `try1 rc=2 / try2 rc=2 / ALL_TRIES_FLAKED` — i.e. **a dead arm is indistinguishable from a slow
   one by file presence alone.** That is the trap: `laneA-run-hk.sh` must always be launched via
   `scripts/hk-run-try12-config.sh`, never directly. The live 210958 series does this correctly.

Killed the orphan scoped by verified PID (`kill -TERM 46315`, process group enumerated first,
2 processes, both this lane's). No `pkill`. The live series was left untouched.

### The measurement this iteration adds: protect cost resolved in TIME, not just in total

Iteration 3 established `hb_memory_protect` costs 23 % of wall-clock and that every term was still
rising. It could not say *where in the boot* that cost falls, and that is the number that decides
whether the memory fix can touch the 120 s goal at all. The memmeter reports cumulative counters
every ~5 s, so the per-window sampled mean (`Δmp_ns / Δmp_ns_n`) times the window's `Δmp_calls`
gives a time-resolved cost. Applied to the live `list` arm (`laneA-memwalk2-list1-a1-try1-211310`),
with the boot markers overlaid:

| phase | wall | protect (summed across threads) | protect calls |
|---|---|---|---|
| pre-mono | 11.4 s | **0.0 s** | 60 352 |
| ReloadAssembly | 30.1 s | **2.8 s** | 201 728 |
| PhysX-ready → XInput | 59.8 s | **10.7 s** | 780 096 |
| **post-XInput** | **476.6 s** | **169.3 s** | 354 880 |
| whole run | 620 s | 182.9 s | 1 372 096 |

**Cumulative protect cost at XInput (t=148 s) is 13.5 s.** That is the entire budget the memory fix
can recover from the deterministic prefix. The other **169.3 s sits in the tail**, which is where
this fix pays and where it pays enormously.

Two independent instruments agree on the tail, which is worth stating because neither is obvious:
169.3 s of 476.6 s is **35.5 %** of post-XInput wall-clock, and iteration 2's `sample` of a stalled
run put `hb_memory_protect` at **33 %** of the critical-path thread. A counter-based estimate and a
stack sampler landing within 2.5 points of each other is the strongest evidence this lane has that
the attribution is real and not an artefact of the sampling mask.

Note the call distribution, which is the opposite of the cost distribution: **57 % of all protect
calls happen in the PhysX→XInput window** (780 096 of 1 372 096) and cost 10.7 s, while the tail's
354 880 calls cost 169.3 s. Same work, 34× the unit price, because `regions` grew underneath it.

### The walk visits 25 069 regions to apply 1.28 of them

From the same run: `mp_walk=745 224`, `mp_visit=18 682 481 606`, `mp_apply=950 588`.

- `mp_visit / mp_walk` = **25 069 regions visited per walk**
- `mp_apply / mp_walk` = **1.28 regions actually modified per walk**

So the protect ranges do *not* span thousands of regions — they span about one. The O(N) scan is
pure overhead: 18.7 **billion** node visits to perform 950 588 permission changes. This is the
single most important number for predicting the tree arm, because it says the treap's in-range
count `k` is ~1.3, not ~25 000.

### Static check first: is the walk the ONLY O(N) left on that path?

If the split path were also O(N), the treap would fix half the cost and the tree arm would
disappoint. Read rather than assumed:

- `split_all_regions_at` → `hb_memory_find_region` → **`find_region_normalized` is an O(log N)
  treap descent** (hb_memory.c:1901), with a hot cache in front of it.
- `split_region_at` → `find_region` (O(log N)) + `tree_insert` (O(log N)).
- `remove_region_node` is O(N) + an O(N log N) `rebuild_region_tree`, but `rebuilds=19` for the
  whole run — cold, as iteration 3 found.
- `any_overlap` (:569) guards the two map paths and is O(N), but it is not on the protect path.
  Its counter is in the tree and not yet deployed (`ovl_calls=ABSENT`).

**`protect_range_walk_list` is the only O(N) on the protect path.** The tree arm therefore isolates
exactly one variable.

I also checked the two traversals are semantically equivalent, because a pruning bug would make the
tree arm silently apply to fewer regions — fast *and wrong*. The treap is keyed by base; left
descendants are strictly below, right descendants at or above. `protect_range_visit` descends left
only when `node->base >= w->start` (if it were below, every left descendant is below `start` and
none can match `base >= start`) and descends right only when `node->base < w->top` (symmetric).
Both prunes are sound and neither can skip a matching node, and `protect_apply_region` is shared
verbatim by both arms. The traversal is correct.

### PREDICTION — registered before the tree arm lands

1. **`WALK_LEN`: 25 069 → tens, not thousands.** Expected ≈ the two search paths to `start` and
   `top` plus the in-range nodes: roughly `2 × 1.39 × log2(57 000) + 1.3` ≈ **30–70**.
2. **`MP_NS_PER_CALL`: 120 435 ns → < 2 000 ns** (the off-slot bench was flat at ~450 ns from N=1
   to N=16 000).
3. **`EST_TOTAL_PROTECT_S`: 183 s → < 5 s.**
4. **`mp_apply / mp_walk` stays 1.28 in BOTH arms.** This is the correctness check, not a
   performance one: the arms share `protect_apply_region`, so if the ratio moves, the traversals are
   not equivalent and the arm is invalid regardless of how fast it is.
5. **`regions` and `splits` follow the same trajectory in both arms** — the traversal does not
   mutate the map, so a divergence means something other than the selector differed between runs.
6. **`xinput` moves from ~148 s to ≥ 134 s — NOT under 120 s.** The fix can only recover the 13.5 s
   of prefix protect cost measured above.
7. **The tail is where it shows:** post-XInput wall should fall substantially, since 35.5 % of it is
   protect.

**What refutes me:** `WALK_LEN` in the tree arm staying in the thousands. That would mean protect
ranges genuinely span thousands of regions, `k` is large, the treap cannot help, and **coalescing
adjacent regions becomes the only fix** — the work iteration 3 deliberately deferred. The measured
`mp_apply/mp_walk = 1.28` is what says this will not happen, so if it does, my reading of
apply-vs-visit is wrong and the region model needs re-deriving before any more code is written.

### Coalescing: the evidence now points AWAY from it, and that is a saved slot

Iteration 3 named region coalescing as the next fix and gated it on evidence. The evidence has
arrived and it argues the other way. Coalescing was justified by "N grows without bound, and the
walk is O(N)" — but `mp_apply/mp_walk = 1.28` shows the walk's *useful* work is ~1 region, so once
the traversal is O(log N + k) the unbounded N stops mattering for protect. What N still costs is
memory (57 701 region structs) and `any_overlap` on the map paths, neither of which is on the
measured critical path. **Coalescing requires real treap deletion — a genuine change to a
load-bearing structure — and the number that would justify it now argues against it.** Not built.
Revisit only if refutation condition above fires, or if `ovl_calls` comes back hot once deployed.

### A side observation, recorded not chased: VirtualAlloc is failing

Seven `macrunner-hb-guest-alloc-fail: import=VirtualAlloc status=ffffffffc0000018` in one run
(t = 62.7, 68.3, 87.5, 134.0, 144.3, 159.4, 178.1 s), every one a 64 KB `MEM_COMMIT|MEM_RESERVE`
at `PAGE_EXECUTE_READWRITE`. `c0000018` is `STATUS_CONFLICTING_ADDRESSES`. These are Mono asking
for JIT code memory and being refused. Recorded here because it is cheap to note and may matter to
whoever owns the tail; it is not this lane's measured cost and I did not chase it.

### The arithmetic, updated — and it is honest about the goal

| | measured |
|---|---|
| to XInput | 148 s (of which ~14 s is harness prefix sync a real user would not pay) |
| protect cost inside that | 13.5 s |
| best case to XInput after the memory fix | **~134 s** |
| goal | **120 s to the MENU**, which is past XInput |

The memory fix cannot reach the goal, and nothing else measured in the prefix is close either:
wine start + loader ≈ 28 s, ReloadAssembly 32.8 s, PhysX→XInput 59.9 s (only 10.7 s of it protect).
Reaching 120 s *to the menu* needs roughly a 4–5× speedup of general emulation, not another point
fix. **That is not a reason to stop and it is not a blocker** — the memory fix is still the largest
single win this lane has found (≈180 s of a 620 s run, and growing with run length, because the
per-call cost rises with `regions`). It is a reason the goal line should be restated against what
is actually measurable.

One more caveat the operator should have: **`hk-boot-phases.py` reports the menu as `NOT_REACHED`
in every arm**, and no HK run in this series reaches a menu at all. The "476 s to menu" baseline in
the brief is therefore not currently reproducible as a menu measurement; this lane grades on
`xinput` (deterministic, ±5 % over 8 runs) and on guest throughput instead. Any claim of "menu in
N seconds" needs a run that actually reaches one first.

### The control arm ran to completion, and the cost is worse at length

The numbers above were read at t=620 s while the run was still going. It finished at 880 s:

| | at t=620 s | at completion (880 s) |
|---|---|---|
| `regions` | 57 701 | **68 494** |
| `WALK_LEN` | 25 069 | **28 587** |
| `MP_NS_PER_CALL` | 120 435 | **134 493** |
| `EST_TOTAL_PROTECT_S` | 183 | **240.9** |
| protect share of post-XInput wall | 35.5 % | 29.2 % |
| `APPLY_PER_WALK` | 1.28 | **1.27** |

Every term still rising at the end, and `APPLY_PER_WALK` dead flat at 1.27 across a 42 % growth in
N — the walk's useful work is constant while its cost is not, which is the O(N)-overhead claim in
one number. **240.9 s of an 880 s run (27 %) is spent in `hb_memory_protect` in the control arm.**

### Tooling: the phase attribution is now in the tool, not in this journal

`scripts/hk-memmeter.py` gained the time-resolved table (`--- protect cost by boot phase ---`,
`PROTECT_S_BEFORE_XINPUT` / `PROTECT_S_AFTER_XINPUT`) and `APPLY_PER_WALK`. Two reasons this
belongs in the tool rather than in a hand calculation:

- The A/B driver already pipes every arm through `hk-memmeter.py`, so **the tree arm gets graded
  the same way automatically** — no chance of the two arms being scored by different arithmetic.
- Summing per window matters: per-call cost rises ~34× across a boot, so one whole-run mean charges
  the cheap early calls at the expensive late price. `EST_TOTAL_PROTECT_S` (whole-run mean) and
  `TIME_RESOLVED_TOTAL_S` (per-window) agree at 240.9 s here, but only because the run is long;
  on a short run they diverge, and the time-resolved one is the honest number.

Markers and memmeter rows are aligned on the harness `+Ns` stamp, not on `epoch=` — the Unity
markers carry no epoch, and mixing the two clocks would silently offset every phase boundary.
`list1-memmeter.txt` was regenerated with the new tool so both arms are comparable.

### Regression check on the staged changes — the set, not just the count

The build is clean under `-Wall -Wextra -Werror`. The unit suite is ASLR-flaky, so 5 runs:

```
per-run failures: 27 29 29 27 28     UNION=30  STABLE=26  FLAKY=4
```

**STABLE = 26**, the same count iteration 3 measured for the pre-`mem=`/`any_overlap` build, and a
strict subset of HEAD-baseline's 27. The 4 flaky ones are all `out.result == HB_OK` at
hb_test_runner.c:5325 / 5400 / 19370 / 19429 — they appear and vanish with the address layout,
which is the documented behaviour of this suite (it maps raw stack addresses as guest regions).

One improvement over previous iterations, which recorded only the *count*: the 26 stable failures
are now written out by name (`code_buf->size <= N` codegen-size assertions ×17, `out.result ==
HB_OK` ×6, `hb_memory_guest32_map` ×2, one `expected 1, got 3` pair). A matching count is weak
evidence; a matching *set* is what actually rules out "one regression appeared and one pre-existing
failure vanished". Future iterations should diff the set, and it is in
`reports/research/hk-memwalk-stable-failures-20260729.txt`.

### RESULT — the tree arm, matched against the list arm point by point

Both arms measured on the same binary, same deploy, alternating, one env var apart. Cumulative
figures at matched elapsed time (`laneA-memwalk2-tree2-a1-try1-213234` vs
`laneA-memwalk2-list1-a1-try1-211310`):

| t (s) | regions list / tree | **WALK_LEN** list / tree | **ns/call** list / tree | APPLY/WALK list / tree |
|---|---|---|---|---|
| 45 | 18 / 18 | 13.7 / **5.4** | 334 / 375 | 1.29 / 1.29 |
| 55 | 385 / 385 | 161.3 / **10.4** | 109 / 43 | 1.31 / 1.31 |
| 65 | 4 004 / 4 214 | 1 750.0 / **13.5** | 2 219 / 110 | 1.30 / 1.29 |
| 75 | 7 967 / 8 620 | 3 488.4 / **15.1** | 5 226 / 164 | 1.28 / 1.28 |
| 85 | 10 822 / 12 398 | 6 558.4 / **16.9** | 10 842 / 184 | 1.29 / 1.30 |
| 95 | 12 451 / 14 194 | 7 463.4 / **16.8** | 12 524 / 151 | 1.30 / 1.31 |

**At t=125 s: the walk is 662× shorter and a protect call is 79× cheaper.** Against the completed
list arm's endpoint (`WALK_LEN` 28 587, 134 493 ns/call) the gap is larger still, and growing,
because the list arm's cost tracks N while the tree arm's does not.

Every registered prediction that can be read yet is confirmed:

1. **`WALK_LEN` → tens.** Predicted 30–70; measured **flat at 5.4 → 16.8** while N grew 18 → 14 194.
   Better than predicted, and the *shape* is the point: the list arm's walk length tracks `regions`
   almost exactly, the tree arm's is nearly independent of it. That is O(N) versus O(log N + k)
   measured rather than argued.
2. **`MP_NS_PER_CALL` < 2 000 ns.** Measured **151 ns**, in line with the off-slot bench's ~450 ns
   and below it. (The 375 ns at t=45 is one sample on a cold cache, not a trend.)
4. **`APPLY_PER_WALK` identical across arms — 1.28–1.31 at every matched point.** This is the one
   that makes the rest trustworthy: the arms share `protect_apply_region` verbatim, so equal apply
   counts at equal times mean the treap traversal reaches exactly the regions the list scan reached.
   The tree arm is not fast because it is skipping work. Checked at six points, not just at the end.
5. **`regions` trajectories match** (18 / 385 / 4 004 vs 18 / 385 / 4 214) — the traversal does not
   mutate the map, so the two runs are comparable and the difference is the selector alone.

### The payoff, in guest work rather than in counters

Counters can only show the walk got cheaper. What matters is whether the guest got further, so:
wall-clock to reach a fixed amount of guest translation work (`misses`, i.e. compiles).

| guest work | list | tree | gain |
|---|---|---|---|
| 60 000 compiles | 53.9 s | 55.2 s | −1.3 s |
| 80 000 | 58.4 s | 59.4 s | −1.0 s |
| 100 000 | 87.2 s | 84.2 s | +3.0 s |
| 110 000 | 116.4 s | 107.9 s | +8.5 s |
| **120 000** | **146.0 s** | **131.2 s** | **+14.9 s (10.2 %)** |

**The gap opens with time and starts at zero.** That is the signature the mechanism predicts and
the strongest evidence the win is real rather than run-to-run luck: early on, `regions` is small,
the list walk is short, and the arms are identical (the first two rows are a wash, tree marginally
behind). The advantage appears only once N has grown enough for O(N) to bite, and then compounds.

**`Using XInput`: list 148.0 s → tree 132.9 s, 15.1 s / 10.2 % faster.**

### Where the registered prediction was wrong, and by how much

Prediction 6 said `xinput` would land **≥ 134 s**, reasoning that the fix can only recover the
13.5 s of protect cost measured inside the prefix. Measured: **132.9 s** — 1.1 s below my floor,
recovering 15.1 s where 13.5 s was budgeted. The direction, the magnitude and the "not under 120 s"
call were all right; the floor was marginally too tight. The likely reason the fix over-delivers
slightly is that the protect timer charges only time inside `hb_memory_protect`, while an 18.7
billion-node scan also evicts cache and lengthens whatever holds the map — costs that are real,
are removed by the fix, and are invisible to the instrument that budgeted it.

Worth stating plainly: 132.9 s is **below the entire 144.3–156.0 s range** the deterministic prefix
showed across 8 earlier runs, so this is not the ±5 % band. But it is **one pair of arms**. The
driver alternates `list tree list tree` precisely so a second pair can confirm it, and that pair
has not run yet. Treat 10 % as provisional until it does.

Prediction 3 (`EST_TOTAL_PROTECT_S` < 5 s) needs the arm to finish.

### Cross-validated by a stack sampler, not just by the counters that were built to prove it

The counters and the fix were written by the same hand, so confirming one with the other is weak.
A `sample` of the live tree arm (pid 47868, 5 s, mid-tail — taken *after* XInput was already
recorded, so it cannot have influenced the 132.9 s) against iteration 2's sample of the old code:

| symbol | list arm (before) | tree arm (now) |
|---|---|---|
| `macrunner_hb_try_kernel32_handle_semantic` | 996 | **10** |
| `macrunner_hb_sync_virtual_region` | 918 | **0** |
| `hb_memory_protect` | **913** (top non-idle leaf) | **0** |
| `find_region_normalized` | 102 | 33 |

**The 33 %-of-critical-path spine is gone**, measured by an instrument that knows nothing about the
memmeter. `protect_range_visit` does not appear at all — it is inlined and too cheap to sample.

The new top-of-stack, once the pathology is removed, is what a healthy emulator looks like — actual
translated execution, not map maintenance:

```
macrunner_hb_run_x64            658     _platform_memset      586
hb_jit_runtime_run              346     _platform_memmove     531
try_promote_hot_block_families  237     hb_memory_read        214
```

(Idle dominates the raw totals — `__ulock_wait2` 162 708, `mach_msg2_trap` 14 386 — which is the
parked worker pool iteration 2 characterised, not a stall.)

### A lead opened and closed in one step: the memset is not ours

`_platform_memset` + `_platform_memmove` = 1 117 samples is the largest real-work item in that
profile, larger than `macrunner_hb_run_x64` itself, and `clear_hot_cache` does a `memset` on **every
split** — 35 220 of them in the control arm. That is a clean hypothesis and it is **wrong**:
`HB_MEMORY_HOT_CACHE_SLOTS` is **16**, so the memset clears **128 bytes**. 35 220 × 128 B is
nothing. The hot cache is also thread-local with an epoch counter, so the array clear is a
back-compat formality, not the invalidation mechanism.

The memset/memmove traffic is therefore the guest's own — Mono zeroing and copying heap — routed
through the normal paths. Healthy, and not this lane's to remove. Recorded so the next iteration
does not re-open it.

**The refutation condition did not fire.** `WALK_LEN` did not stay in the thousands, so the treap's
in-range span `k` really is ~1.3, coalescing is not needed to fix the protect path, and the decision
in the previous section to not build it stands on measurement rather than on my reading of it.

**This fix is already the shipped default** — `hb_memory_init_environment` sets
`memprotect_walk_list` only when `MACRUNNER_HB_MEMPROTECT_WALK=list` is passed, so `list` is the
artificial control arm and every other lane's runs on this dist are already getting the treap. There
is nothing to flip on; there is something to *commit*, which is the coordinator's call.

### State

- Live: `hk-memwalk-ab.sh memwalk2` (pid 84507), arms `list tree list tree`, 900 s cap, deploying
  `63c9691c` only. **Arm 1 (`list`) and arm 2 (`tree`) are both measured** — that is the result
  above. Arms 3–4 are the confirming second pair and have not run; sister lanes AUDIOCHECK and
  SYNCBLOCK hold the slot between arms, so expect long gaps.
- The orphaned duplicate series is dead; only one driver is now competing for the slot.
- ⚠ Still true and still the one thing this lane must not do: **the working tree is AHEAD of the
  deployed binary** (`mem=`/`maps=` on the memmeter line, the `any_overlap` counter). Do not relink
  and deploy until all four arms land.

### What the next thread should do

1. **Read arms 3–4 when they land** (`reports/research/hk-memwalk-ab-20260729-210958/*-memmeter.txt`
   — the phase table and `APPLY_PER_WALK` are now printed automatically). The one number that
   matters is whether `xinput` in the second `tree` arm again lands ~15 s under the second `list`
   arm. Until it does, the 10.2 % is one pair.
2. **Then deploy the staged instruments together** (`mem=`/`maps=`, `any_overlap` counter) and read
   `ovl_calls`. That is the last unmeasured O(N) walk in the file and the only surviving argument
   for region coalescing.
3. **Do not re-open**: coalescing (refuted — `APPLY_PER_WALK`=1.27), `sync_live_range` /
   `any_overlap` rewrites (cold: `slr_calls`=33 in 880 s), per-thread JIT runtime construction
   (0.60 s total), `clear_hot_cache` as the memset source (16 slots = 128 B).
4. **The honest framing for the goal.** With the treap fix the deterministic prefix is ~133 s to
   XInput and the menu is past it, so **120 s to the menu is not reachable by any remaining point
   fix in this lane's territory** — it needs general emulation throughput. The next real target is
   whatever the post-fix profile says, and that profile is now `macrunner_hb_run_x64` +
   guest memset/memmove + `hb_jit_runtime_run`, i.e. the translator itself. That is a different
   kind of work from the O(N) hunt that has paid out so far, and it should be scoped as such rather
   than attacked with another counter.

---

## Iteration 5 — 2026-07-29

The memwalk A/B's `tree` arm finished, so iteration 4's last open prediction is settled. Then a
sample of that same arm — taken while it was still live, because a running tail is a perishable
measurement — found something an order of magnitude larger than anything this lane has touched,
and it is **not** in the translator.

### Closing out the memory fix: the last prediction, and one that was wrong

`tree2` completed. Iteration 4's prediction 3 was `EST_TOTAL_PROTECT_S` < 5 s:

| | list arm (control) | tree arm (fix) |
|---|---|---|
| `EST_TOTAL_PROTECT_S` | **240.9 s** | **0.5 s** |
| `WALK_LEN` | 28 587 | **16.6** (flat while `regions` grew 18 → 77 604) |
| `MP_NS_PER_CALL` | 134 493 | **264** |
| `APPLY_PER_WALK` | 1.27 | **1.27** ← the correctness check, unchanged |
| `PROTECT_S_BEFORE_XINPUT` | 13.5 | **0.1** |
| `xinput` | 148.0 s | **132.9 s** |
| **`compiles` completed** | 165 873 | **217 915 (+31 %)** |

Confirmed at 0.5 s against a < 5 s prediction. `APPLY_PER_WALK` identical to two decimal places
across a 42 % difference in N is what says the treap reaches exactly the regions the scan reached
— the arm is not fast because it skips work.

**A registered prediction that FAILED, recorded because it changes what is still open.**
Iterations 1–2 predicted `RETENTION_PCT` 62 % → **> 90 %**. Measured: **72.6 %** (list 72.0 %).
The mechanism itself works exactly as designed — `mh_argshape`, `mh_widearg` and `mh_toomany` are
all **0** in both arms, so the matcher artefacts really are gone — but retention did not follow.
`rl_stores`=158 178 of 217 915 compiles, and the only counted decline, `rl_hostptr`=6 081,
explains just 10 % of the 59 737 blocks that did not store. **~53 700 declines have no counter at
all.** That is an unattributed gap, not a measured cause, and iteration 1's "retention → >90 %"
should be treated as refuted rather than pending. Not chased here — see the reprioritisation below.

### The finding: 71.6 % of all non-idle CPU is a fault loop inside the signal handler

A 10 s `sample` of the live `tree` arm (pid 47868, t≈760 s, 141 % CPU). Leaf attribution is
`sample(1)`'s own, not my parser's — I checked my first hand-rolled pass against it and used the
tool's numbers:

| leaf | samples | % of non-idle |
|---|---|---|
| **`_platform_memmove`** | **23 465** | **71.6 %** |
| `macrunner_hb_run_x64` | 1 462 | 4.5 % |
| `_platform_memset` | 973 | 3.0 % |
| `hb_jit_runtime_run` | 744 | 2.3 % |
| `try_promote_hot_block_families` | 648 | 2.0 % |

95.3 % of that memmove sits under exactly one spine:

```
_sigtramp
  macrunner_hb_primary_signal_handler
    macrunner_hb_route_x64_callback_fault
      macrunner_hb_pc_is_x64_guest_code_module_no_lock
        macrunner_hb_probe_image_bytes
          memcpy()            <- a PE header read that is ALLOWED TO FAULT
```

**Three threads had 7 472 of 7 472 samples in it — every single sample.** A 64-byte header copy
cannot be caught by every sample of a thread; a copy that faults into the kernel and is restarted
forever can. Three other threads call `probe_image_bytes` from ordinary context and are fine, so
the pathology is the fault path specifically, and grading on "100 % in the handler" cannot be
inflated by them.

Two independent facts agree. `ps -M` on the same process: one thread at 0:18 system / 0:02 user —
**86 % of its time in the kernel**, which is what a page-fault loop looks like and what a spin does
not. And `macrunner-hb-fault-reentry-break` fired **0** times, exactly as it must: that guard
watches for a nested *signal*, and this is a nested *kernel fault* inside the handler's own copy.

### What is NEW here and what was already known — stated before the claims, not after

**The wedge itself is not my discovery and I should not have framed it as one.** The master lane
diagnosed it this morning (ITER-4) in far more detail than the paragraphs above: the exact pinned
instruction (`pc_is_x64_guest_code_module_no_lock+264` = `ldrh w9,[x0]`, the `dos->e_magic` read in
`macrunner_hb_module_machine`, on a `DllBase` from the unlocked LDR walk), the `ps -M` Δsys-vs-Δusr
discriminator (+4.6 s sys / +0.38 s usr on pinned threads, inverted ~12× against healthy ones), the
reason the reentry guard structurally cannot see it (the faulting load runs with the signal masked,
so no second signal is ever delivered — exactly one `_sigtramp`), and the fix and its gate. My
"three threads at 7472/7472" is their result reproduced on a different run.

**What this lane adds is the cost attribution and the throughput experiment:**
- the wedge is **71.6 % of all non-idle CPU**, measured against a non-idle denominator — nobody had
  priced it, and a correctness-framed bug that turns out to be the single largest CPU consumer is a
  different decision for the coordinator than a hang;
- pins **accumulate** across a run and each one is permanent (table below);
- the **405 % → 141 % CPU** correlation, which suggests a global tax rather than N lost threads;
- the first A/B of the gate graded on **throughput** rather than on whether the wedge appears.

**And a prior refutation that cuts against my own prediction, recorded because it must not be
buried:** the master lane already established that **"a pinned thread causes the stall" is REFUTED —
the stall reproduced with 0 wedges.** So the wedge is not necessary for the boot stall. That does
not contradict a CPU-cost claim (a thread can burn a core without being the cause of a separate
stall), but it does mean prediction 4 below is **less likely than my CPU-collapse observation alone
would suggest**, and I am registering it knowing that. If the `on` arm frees three threads and
`compiles` does not move, the master lane's refutation simply extends to throughput as well, and
this lane should say so plainly rather than look for a reason the arm was unfair.

### The cure is already in the tree, default-OFF, and has never been run

`macrunner_hb.c:1158` — `MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE`. With it set,
`probe_image_bytes` routes fault-path header reads through `mach_vm_read_overwrite` and **refuses**
an unreadable one instead of faulting. Default off, so every run this lane has ever measured —
including all four memwalk arms — carried the faulting path.

Verified by content, not assumed: the shipped `ntdll.so` (`63c9691c`) contains both the gate string
and the refusal marker. **So this costs no build and no deploy** — it is one environment variable
on an already-deployed binary, which also means the two arms cannot differ by anything else.

**Territory, stated plainly.** That whole spine is in `macrunner_hb.c` / `signal_arm64.c`, both
outside this lane's territory to edit. I am not editing them. What is inside this lane is the run
configuration and the measurement, and the gate is a runtime flag — so the experiment is fully in
bounds even though the code is not. The sister master lane owns the file and already wrote
`scripts/hk-header-probe-ab2.sh` at 17:35 today; it has produced **no run dir**, so the A/B has
never landed. Theirs grades on whether the wedge appears (a correctness question about reaching a
menu). Mine grades on throughput. Same gate, different question — recorded so the coordinator can
see this is complementary, not duplicated work.

### Why this reorders the whole lane

Iteration 4 concluded the remaining cost was "the translator itself — `macrunner_hb_run_x64` +
guest memset/memmove + `hb_jit_runtime_run`", and proposed scoping general emulation throughput.
**That conclusion was built on a 5 s sample read as leaf counts, and it was wrong in its
attribution.** `macrunner_hb_run_x64` is 4.5 % of non-idle work, not the bulk; the memmove it saw
is not the guest copying its heap (iteration 4's explicit reading — "healthy, and not this lane's
to remove") but a fault loop in the signal handler. I am retracting that reading. The correct
next target is this gate, and it is a flag flip rather than a research programme.

It also gives iteration 2's Finding 4 a mechanism. That run "did not finish slowly, it deadlocked"
— 49 threads, 0 runnable, at t≈589 s. Three threads pinned in a kernel fault loop, starving
everything waiting on them, is a candidate explanation for exactly that, and it predicts the pins
should arrive *before* the wedge rather than after.

### PREDICTION — registered before the arms land

Launched `scripts/hk-hdrprobe-speed-ab.sh` at 22:00 (arms `off on off on`, 900 s cap, same binary,
gate the only difference). The slot was already held by a sister lane, so the first arm is queued;
no data exists at the time of writing and the first sample point is +420 s of guest clock away.

1. **`REFUSALS` > 0 in the `on` arm.** If it is 0 *and* pins are unchanged, the flag never reached
   the guest and nothing else in that arm means anything — read this first. This is the `rl_stores`
   lesson applied to a new gate.
2. **`PINNED` 3 → 0 in the `on` arm** at the late sample point, against ~1–3 in the `off` arm.
   This is the mechanism, read out of `sample(1)`, which knows nothing about MacRunner — so the arm
   is not graded by the instrument being tested.
3. **`MEMMOVE_PCT_OF_NONIDLE` falls from ~71 % to under 20 %.**
4. **`compiles` and `last_activity_s` rise materially in the `on` arm** — the tail is where a fault
   loop is paid for, and three freed cores should show up as guest work completed.
5. **`xinput` moves LITTLE** (≈133 s, unchanged). The wedge is late-arriving — the sister lane
   measured 0 pins at +5.5 min and I caught 3 at +12.7 min, both well past XInput at 133 s. If the
   prefix does improve, that is a bonus and not the prediction.

**AMENDMENT to prediction 2, registered 22:08 — still before any arm data exists.** The driver's own
log shows it waited for the slot from 21:52 to 22:08:07 and arm `off1` only started then, so nothing
below is retrofitted to a result. The amendment is forced by the second wedge site found at 22:07:

- **2a. At the `_platform_memmove` site: PINNED → 0 in the `on` arm.** This is the gate's target and
  the only part of prediction 2 that was ever defensible.
- **2b. At the `macrunner_hb_get_callback_exception_stack` (SIGBUS) site: PINNED unchanged.** The
  gate does not touch `bus_handler`. If this site *also* clears, my reading of the two spines is
  wrong and I would want to know why before believing the arm.
- **2c. A bare `PINNED` count is no longer a valid grade** and I am not grading on one. If the `on`
  arm shows `PINNED=1` at the SIGBUS site and 0 at the memmove site, that is a **success**, not the
  partial failure the original prediction 2 would have scored it as.

**What would refute me:** `PINNED` goes to 0 and throughput does **not** move. That would mean the
three pinned threads were not blocking anything — that the work they were failing to do was not on
anyone's critical path — and the 71.6 % would be real CPU burn with no wall-clock consequence.
That is a genuine possibility and it is exactly why the arm is graded on `compiles`, not on the
sample. The opposite refutation also matters: if the `on` arm is *slower*, the gate has traded a
cheap memcpy for a `mach_vm_read_overwrite` syscall on a hot path, and the right fix would be to
cache the classification rather than to make the probe safe.

### The loop, resolved to a line — for the master lane, whose file this is

The full stack of a pinned thread closes the loop exactly, and **both probe frames are the identical
site** (`probe_image_bytes+136`):

```
macrunner_hb_run_x64 +16472
  macrunner_hb_module_machine +36          <- ORDINARY context, depth == 0
    macrunner_hb_probe_image_bytes +136    <- memcpy of a PE header -> SIGSEGV
      _sigtramp
        macrunner_hb_primary_signal_handler +660
          macrunner_hb_route_x64_callback_fault +752
            macrunner_hb_pc_is_x64_guest_code_module_no_lock +304
              macrunner_hb_probe_image_bytes +136   <- SAME function, SAME offset
                _platform_memmove +144              <- faults AGAIN
```

`signal_arm64.c:1637-1647` classifies four addresses on the fault path, and one of them is
**`fault_addr` itself (line 1642)** — the address that just faulted. `pc_is_x64_guest_code_module_no_lock`
answers by reading a PE header *at that address*. So the router asks "is the address that just
faulted x64 guest code?" and answers it by dereferencing the address that just faulted. It faults
again, deterministically, at the same instruction.

The stack does **not** grow — it is exactly two probe levels, not runaway recursion — so this is the
kernel re-delivering one fault forever rather than a stack overflow. That is why `ps -M` shows 86 %
system time, and why `macrunner-hb-fault-reentry-break` (which watches for a nested *signal*) counts 0.

Note the outer memcpy faults with the gate ON too: `header_probe_is_safe()` requires
`fault_header_probe_depth > 0`, which is false in ordinary context. The gate protects only the
*inner*, in-handler probe — which is the right place, because that is the one that turns a single
recoverable fault into a permanent loop. **Recorded as a risk to the prediction, not hidden: the gate
may convert a pinned thread into a delivered exception, so the `on` arm could plausibly die
*earlier* rather than run faster.** `last_activity_s` will say, and that outcome would still be
progress — a fault that is handled is debuggable; a thread wedged forever is not.

### How the gate can break the loop at all, given the OUTER fault happens either way

This needed resolving, because on the face of it the gate should not work: the outer memcpy runs at
`depth == 0`, so it faults with the gate ON too, and the kernel then retries the same instruction —
which looks like the same infinite loop with a safer inner probe.

The answer is the same-pc guard at `signal_arm64.c:1569-1594`, and the ordering is the whole point.
`macrunner_hb_fault_same_pc_count` is incremented **at the top of the router**, before the
classification block that wedges. So:

- **Gate OFF:** the first handler entry bumps the counter to 1, then wedges inside the classifier
  and **never returns**. There is no second entry, so the counter can never reach
  `MACRUNNER_HB_FAULT_SAME_PC_LIMIT`. The guard is structurally unreachable — for the same reason
  the reentry guard is, one level up.
- **Gate ON:** the inner probe refuses instead of faulting, the handler **completes and returns**,
  the kernel retries, the next entry bumps the counter — and after the limit the guard fires
  `same-pc-break`, stops handling, and lets the fault propagate normally. The thread is freed.

So the gate does not prevent the fault; it restores the existing escape hatch that the wedge was
disabling. That is a much more believable mechanism than "the probe got cheaper", and it predicts
the freed thread ends in a delivered exception rather than in useful work — which is exactly why
`last_activity_s` is on the scorecard next to `compiles`.

**One measurement trap, found before relying on it:** `macrunner-hb-fault-same-pc-break` is behind a
`static int announced_pc`, so it prints **once per process no matter how often it fires**. It cannot
be used as a rate, and its presence does not distinguish the arms — my gate-OFF `tree2` run already
has exactly one. Grade on `PINNED` and `compiles`, not on that marker.

### Independently replicated on a foreign lane, and the pins ACCUMULATE

Sampled a sister lane's live guest (`MASTER-I9-DRAIN`, pid 52275) while my own arm queued — a
different lane, a different config, the same shipped binary, and no slot cost. At only **5:32** into
its run it already had **`PINNED=1`**. Placed beside the other specimens:

| specimen | guest age | pinned threads |
|---|---|---|
| master lane's own three specimens | +5.5 min / +15 min / +18 min | 0 / 1 / 3 |
| `MASTER-I9-DRAIN` (this sample) | +5.5 min | **1** |
| memwalk `tree2` (this sample) | +12.7 min | **3** |

**CORRECTION — I tracked that run over time and "pins accumulate" did NOT survive it.** I wrote the
table above after a single sample of each specimen and generalised across *different runs*, which
was sloppy. Sampling the same foreign run again 10 minutes later:

| `MASTER-I9-DRAIN`, one run tracked | +5.5 min | +16 min |
|---|---|---|
| PINNED | 1 | **1** |
| pinned tid | 61123361 | **61123361 (the same thread)** |
| CPU | 405 % | **144.7 %** |

**Within a single run, the pin count did not grow at all over ten minutes.** What *is* confirmed is
that a pinned thread never recovers — the identical tid is still wedged 10 minutes later. So the
sound claims are "a pin is permanent" and "different runs carry different pin counts (1–3)", not
"pins accumulate". The cross-run 0/1/3 progression I quoted may just be run-to-run variation.

**And that breaks my own throughput story, so it has to be said plainly: this run's CPU fell
405 % → 144.7 % while the pin count stayed at 1.** The collapse therefore is *not* explained by pins
accumulating, and the "three pinned threads hold something everyone needs" reading of the 405-vs-141
comparison was comparing two different runs and attributing the difference to the one variable I had
measured. The VM-map-lock hypothesis below is now supported by nothing and should be treated as
idle speculation until something measures it.

The 73.1 % system time is still real and still unexplained by one pinned thread — see the second
wedge below, and the broad signal traffic (`_sigtramp` 11.4 %, `__mprotect` 13.3 % of non-idle in
the same sample). This looks like the **signal storm** the master lane already characterised
(73 % of the running thread in `_sigtramp`), of which a pinned thread is one symptom rather than
the cause.

### A SECOND wedge, at a different site, which the gate cannot touch

The foreign run's pinned thread is not in the header probe at all:

```
hb_jit_helper_exec_two_block_loop          <- JIT-generated guest execution
  hb_jit_helper_exec_ir_block_once
    mem_write > hb_memory_write_u8 > hb_memory_write
      macrunner_hb_special_write +624      <- faults, and this one is a SIGBUS
        _sigtramp > macrunner_hb_primary_signal_handler +840
          bus_handler +588
            macrunner_hb_get_callback_exception_stack +52   <- PINNED 7243/7243, leaf
```

Different signal (`bus_handler`, not the SEGV router), different wedge site, and **the header-probe
gate does not touch it.** Two of my three specimens are the memmove wedge (3 threads) and one is
this (1 thread, at both sample points).

**This changes how the A/B must be graded, and I caught it before the arms landed.** `PINNED` is not
one phenomenon. An `on` arm that cures the header wedge but meets this one would read as a failure;
an arm that happens to meet neither would read as a success. `scripts/hk-pin-count.py` now reports
`PINNED_SITE` per thread, so each arm is scored per site rather than on a bare count. Verified
against all three specimens: `_platform_memmove n=3` for `tree2`, `macrunner_hb_get_callback_exception_stack n=1`
for both foreign points.

Worth noting for whoever owns it: the outer fault of this second wedge is a **guest memory write**
through `hb_memory_write` — which *is* this lane's territory — even though the wedge itself is in
the handler, which is not. The trigger is `hb_memory.c:1636`: on the **region-not-found** path,
`hb_memory_write` hands the address to `mem->special_write`, whose implementation writes it raw and
takes the SIGBUS. So HyperBridge is asking the embedder to write an address it has no region for,
and the embedder finds out by faulting.

**Noted, not chased** — deliberately, and the reason is the same discipline that killed the
coalescing work in iteration 3: whether that address *should* be writable is not answerable from
the call site, the wedge it leads to is in a file this lane cannot edit, and one arm of a live A/B
is worth more than a speculative fix. The `macrunner-hb-write-deny` trace at `hb_memory.c:1649`
already exists and would name the addresses if anyone wants to price it first.

### The 71.6 % nearly died to a sampling artefact, and the check made it stronger

I stopped to kill my own headline before publishing it. `sample(1)` records where a thread's stack
*is*, including while it is **blocked** in a kernel fault — so "71.6 % of non-idle samples" is not
by itself a statement about CPU consumed, and a thread stuck in a page fault would look identical
to one burning a core. A first pass at `ps -M` seemed to support the deflationary reading (a pinned
thread had accrued only ~21 s of CPU across 12 min).

So I measured CPU directly instead of arguing about it — `ps` utime/time deltas over a 30 s interval
on the live foreign run at +14:35, which is straight CPU accounting with no sampling in it:

| over 30 s wall | |
|---|---|
| Δ total CPU | **45.90 s** (153 %) |
| Δ **system** | **33.57 s** |
| Δ user | 12.33 s |
| **system share of all CPU consumed** | **73.1 %** |

**Nearly three quarters of every CPU-second this process burns is kernel time**, and under half a
core is doing user-space emulation. A translator executing guest code should be overwhelmingly user
time; this is not.

That is an instrument that knows nothing about stacks agreeing with one that knows nothing about
CPU accounting: **71.6 % of non-idle samples in the fault-path memmove, 73.1 % of CPU in the kernel
— within 1.5 points.** The deflationary reading is refuted and the cost claim stands, now on two
independent measurements rather than one.

**The honest caveat:** system time is not *by definition* all fault loop — `mach_vm_read_overwrite`,
`mprotect` and ordinary syscalls also land there. What ties the 73 % to the wedge is the sample's
spine, not the CPU number alone. The A/B settles it: if the gate removes the pins and system share
does not fall, the two measurements were coincidental and I will say so.

**[HYPOTHESIS, not measured]** the natural candidate is the task's VM map lock: a storm of kernel
faults serialises on it, and every guest `mmap`/`mprotect`/page-fault then queues behind it. That
would make the fault loop a *global* throughput tax rather than the loss of N threads, and it is
the mechanism by which prediction 4 could pay much more than 3/64 of the machine. I have not
measured it and it is not needed for the A/B to be valid — `compiles` is the outcome either way —
but it is the thing to test next if the `on` arm wins big.

### Instrument added

`scripts/hk-pin-count.py` — reads a `sample(1)` file and reports `PINNED` (threads whose *every*
sample is inside the signal handler), `MEMMOVE_PCT_OF_NONIDLE`, and the non-idle leaf table.
Verified against the specimen sample above (PINNED=3, 71.6 %, matching my hand analysis) and with a
negative control: a missing file prints `PINNED=ABSENT-NO-SAMPLE` and exits 2, never a zero.

### ⚠ NEW TRAP, paid for in a free slot: never launch a lane driver from inside the ctx sandbox

The first launch of this A/B produced **nothing**, and the cause is one I had not seen recorded
anywhere. Arm `off1` took the slot at 22:08, wrote a run dir, a `run.log` and a `flight.jsonl`, and
its log kept advancing for 8 minutes — while never reaching wine. The log said only:

```
[mr-run] breaking stale title-slot lock (owner=unreadable x3)     <- every 10 s, forever
```

`mr-run.sh:242` puts the title-slot lock at **`${TMPDIR:-/tmp}/macrunner-title-slot.lock`**. I had
launched the driver from inside a **context-mode sandbox**, which sets `TMPDIR` to an ephemeral
`.ctx-mode-XXXXXX` directory that is deleted when that sandbox exits. Every child therefore had a
`TMPDIR` pointing at a directory that no longer existed, and `acquire_title_slot()` degenerates:

| step | with a deleted TMPDIR |
|---|---|
| `mkdir "$TMPDIR/…lock"` | **ENOENT — fails forever** |
| `cat "$TMPDIR/…/pid"` | fails → reads as `owner=unreadable` |
| `rm -rf "$TMPDIR/…lock"` | no-op |
| `continue` | → back to a `mkdir` that can never succeed |

**Diagnosed by elimination, not by guessing:** the lock did not exist at *either* candidate path
across 70 polls at 0.2 s, so `mkdir` was failing against a directory that was not there — which can
only mean the parent was missing.

Two things make this worth writing down. First, **it wears the same disguise as the BLOCKED
contract**: a run dir, a growing `run.log` and a triage record all appear, so it reads as a slow run
rather than one that never started — `grep`ping for `status=BLOCKED` does *not* catch it, because the
contract was never even evaluated. Second, **the obvious fix is a worse bug**: pointing `TMPDIR` at
`/tmp` makes the livelock disappear while silently moving this lane's lock somewhere no sister lane
looks, destroying the mutual exclusion and putting two Hollow Knights on one machine. The guard added
to the script therefore **refuses to run** rather than re-point, and prints why. Verified with a
negative control (a `mktemp -d` that is then `rmdir`'d makes it abort; the real TMPDIR passes).

General rule for this repo: **anything long-lived — a driver, an autoloop, a background sampler —
must be launched from a shell whose `TMPDIR` outlives it.** The Bash tool's `TMPDIR` is the real
`getconf DARWIN_USER_TEMP_DIR`; a context-mode sandbox's is not.

### State, and what the next thread should do

**Live:** `scripts/hk-hdrprobe-speed-ab.sh` (pid **67358**, own process group, `TMPDIR` pinned to
`getconf DARWIN_USER_TEMP_DIR` and verified writable before anything else runs). Arms
`off on off on`, 900 s cap, **no deploy** — both arms are the already-shipped `63c9691c` and differ
by one environment variable. Started 22:17:40. Results land in
`reports/research/hk-hdrprobe-speed-ab-20260729-221740/`.

The first attempt (`…-215206`, pid 43001, tag `hdrspeed`) is **dead and its `off1` run dir is
garbage** — it never reached wine. Its process group was stopped by verified PID, sister lanes
confirmed alive afterwards, and one `mr-run` that survived `TERM` was `KILL`ed.

**Read it with `python3 scripts/hk-hdrprobe-readout.py <that dir>`**, which prints the four blocks in
the order they must be read. Do not read the timings before the first two:

1. **`GATE_IN_GUEST`** — read from each run's own `final-child.json`, i.e. the environment the child
   actually received, not the one the driver believed it exported. If the `on` arm says `<unset>`,
   stop: the flag never arrived and nothing else in that arm means anything. (The reader is
   controlled: it returns `<unset>` for the memwalk runs, which genuinely did not set it, and
   correctly returns `tree` for a variable those runs *did* set.)
2. **`PINNED` per `PINNED_SITE`** — never the bare count. There are at least two wedge sites and the
   gate addresses only the memmove one. See prediction 2a/2b/2c.
3. `compiles` / `last_activity_s` — the outcome the whole thing is graded on.
4. `xinput` — expected ~unchanged at ~133 s.

**Do not re-open** (each closed on evidence this iteration or the last): coalescing
(`APPLY_PER_WALK`=1.27), `sync_live_range` / `any_overlap` rewrites (cold), per-thread JIT runtime
construction (2.74 s of a 542 s run, all of it `hb_cache_open`), `clear_hot_cache` as the memset
source (16 slots = 128 B), `try_promote_hot_block_families` (7 per-block matchers with power-of-two
retry backoff — O(1) per call, ~2 % steady, not an O(N) walk).

**If the `on` arm wins**, the follow-through is in this lane's territory: add the gate to
`scripts/hk-run-try12-config.sh` beside the other `MACRUNNER_HB_*` exports at line 101. **Use
`${MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE:-1}`, and do not add it while any A/B is live** — my `off`
arm *unsets* the variable, so a bare `:-1` default would silently turn every control arm into a
treatment arm. That is the `MACRUNNER_WINE_BIN` trap in a new costume.

### Slot accounting — what I gave up, deliberately

I stopped my own memwalk driver (pid 84507, verified PID + process group, sister lanes confirmed
alive afterwards) while its arm 3 was still *waiting* for the slot, so no run was destroyed and no
data was lost. That forfeits the confirming `list`/`tree` pair iteration 4 asked for. The trade:
that pair would re-test a 10.2 % delta already backed by a 509× per-call mechanism and matching
trajectories at six points, against a first measurement of a lever consuming 71.6 % of process CPU.
The `off` arms of the new A/B are themselves default-`tree` runs, so the tree number does still get
a second and third independent reading — just without a paired `list` control.

---

## Iteration 6 — 2026-07-29

The A/B from iteration 5 held the slot for this whole iteration, so all of it is off-slot work on
lever 1. It produced a root cause, a correction to iteration 5, and a fix — none of which needed a
run, because **the number that settles it was already in the logs on disk.**

### Iteration 5's "unattributed gap" was attributed all along, in the same line

Iteration 5 wrote: *"`rl_stores`=158 178 of 217 915 compiles, and the only counted decline,
`rl_hostptr`=6 081 … ~53 700 declines have no counter at all."*

Parsing the last telemetry line of that same run (`laneA-memwalk2-tree2-a1-try1-213234/run.log`):

| tree2 arm | |
|---|---|
| `compile_count` | 255 000 |
| `reloc_blocks` | 217 914 |
| `rl_stores` | 158 178 |
| `store_skips` | 59 736 |
| `rl_hostptr` | 6 081 |
| **`rl_unkhelper`** | **53 655** |
| every other `rl_*` decline | 0 |

6 081 + 53 655 = 59 736 = `store_skips` **exactly**. The main-path accounting was already closed;
the residual is 0. The "unattributed 53 700" was `rl_unkhelper`, printed in the same line and
missed when reading it. The list arm agrees (4 606 + 41 899 = 46 505, residual 0).

Recorded plainly because it is the second time this lane has drawn a conclusion from a field it
did not read — and because the correction is what pointed at the actual bug.

### The bug: the store path reads a REGISTER where it needs a VALUE

`HB_RELOC_DECLINE_UNKNOWN_HELPER` had exactly one producer:

```c
if (rl->reg == 23) {                                  /* "x23 means helper address" */
    uint8_t id = helper_cache_id_for_addr(rl->value);
    if (!id) { *why = UNKNOWN_HELPER; goto decline; }  /* declines the WHOLE block */
```

x23 is not only the helper-call register. **`emit_mask_x_reg_to_size()` uses x23 as its scratch at
22 of its 25 call sites**, and for a 32-bit operand it emits `mov x23, 0xffffffff` — the
zero-extension every 32-bit x86 operation needs. `codegen_note_reloc()` records it, the store path
looked `0xffffffff` up in the helper id table, found nothing, and threw the block away.

Measured directly, not inferred — a probe over `hb_arm64_codegen_block`:

```
ADD size=32  ->  relocs=1 : [x23 kind=VALUE v=0xffffffff]     <- one relocation, and it is the mask
ADD size=64  ->  relocs=0                                      <- control: no mask, nothing recorded
XOR size=32  ->  relocs=1 : [x23 kind=VALUE v=0xffffffff]
MUL size=32  ->  relocs=2 : [x1 ...] [x23 kind=HELPER ...]     <- a real helper, correctly tagged
```

A plain 32-bit ADD produces exactly one relocation and it is the mask. That is what 53 655 blocks —
24.6 % of everything that reached the cache — look like.

**This also explains why `05f3f3f9` moved retention by nothing.** Registering the 24 missing helpers
drove `mh_unkhelper` 16 970 → 0 and retention stayed at 62 %: the old matcher only ever inspected
the real `blr x23` target, while the relocation table sees *every* x23 write. The table traded a
small artefact for a much larger one, and the counter named it correctly the whole time.

### Three hypotheses tested and killed before the right one

Stated because each was cheap and the wrong two would have burned a slot:

1. **"The gap is `mem.disp` parked in x23"** (hb_arm64_codegen.c:993). Refuted: that path is
   reachable only with direct-mem on, and `hk-run-try12-config.sh:96` sets
   `MACRUNNER_HB_JIT_DIRECT_MEM=0`. It cannot fire on Hollow Knight. I had already written this
   into three comments as the cause before checking — they are corrected.
2. **"Unregistered helpers"** — the bulk-over-reactive reading, and the obvious sequel to
   `05f3f3f9`. Enumerated all 69 `emit_call_helper` call sites: 57 distinct targets, 55 registered,
   and the 2 unregistered (`hb_jit_split_lock_acquire`/`release`) are gated behind
   `MACRUNNER_HB_LOCK_RMW_ATOMIC`, which no run config sets. The registry is complete; this was not
   it. The reverse lookup covers 1..55 and matches the table, so that was not it either.
3. **"`hb_cache_store()` is failing silently"** — a real uncounted path. Refuted by arithmetic:
   `stores` = `rl_stores` = 158 178, so nothing is lost between the two.

### The fix: state the kind at emission, where it is known

`hb_codegen_reloc_t` carries a `kind` (`VALUE` / `HELPER`), set in `emit_mov_imm64_kind()`.
`emit_call_helper()` is the only caller that passes `HELPER`; the 103 other call sites go through
an unchanged `emit_mov_imm64()` wrapper, so none of them moved. The store path switches on kind, so
the mask falls through to the value classification, lands below the 4 GB `__PAGEZERO` floor, and is
left alone as a literal. **The kind byte fits in the struct's existing padding — the table costs
nothing.**

This is the same correction iteration 1 made for `mh_widearg` and did not finish: that one stopped
vetoing x2/x3/x4 on the instruction pattern, this one stops reading x23 as a type.

A useful safety property fell out of the negative controls: if a helper address is ever left
untagged, it lands *above* the floor and is declined as an unresolvable host pointer. The failure
mode is a lost block, never a cached blob calling a stale address.

### The second finding: 14.5 % of "compiles" can never be cached, and that is the denominator

`record_compile()` fires at **nine** sites. `native_blob_prepare_cache_store()` is reachable from
**two**. The other seven are the `try_promote_*` hot-family fusions, which write straight into
`jit_mem` and `block_cache_put(fused)` and never touch `rt->persistent_cache`.

`compile_count - reloc_blocks` = **37 086, 14.5 %** in the tree2 arm (29 128, 14.9 % in list) — in
the retention denominator with no path to the numerator. So the two retention figures are:

| | tree2 |
|---|---|
| `rl_stores` / `compile_count` | 62.0 % |
| `rl_stores` / `reloc_blocks` (blocks actually offered) | **72.6 %** |

Both are honest; they answer different questions. Now counted as `promo_compiles`, so
`compile_count - reloc_blocks - promo_compiles == 0` is checkable instead of being a gap for
somebody to rediscover. Whether those fusions *should* be persistable is a real question — they are
keyed on one block but semantically depend on two — and is **not** opened here.

### PREDICTION — registered before any run, with what would refute it

With `MACRUNNER_HB_CACHE_RELOC=1` on Hollow Knight:

- `rl_unkhelper` 53 655 → **< 500**. Only genuinely unregistered helpers can remain, and both
  known ones are behind an unset flag, so I expect ~0. **This is the load-bearing prediction.**
- `rl_x23val` > 0, on the order of the blocks that used to decline — it is the same sites, counted
  where they now succeed.
- `rl_stores`/`reloc_blocks` 72.6 % → **> 95 %**; `rl_stores`/`compile_count` 62.0 % → **> 81 %**
  (ceilinged by the 14.5 % of compiles that structurally cannot store).
- `compile_count - reloc_blocks - promo_compiles == 0` exactly. If not, there is a tenth compile
  site and the identity is still open.
- **Cold start unchanged, within the ±5 % eight runs already show.** A cold run *stores*; it does
  not load. I am explicitly not predicting a cold-start win from this, and if `xinput` moves I will
  treat it as noise until a second run agrees. If anything it may be marginally *slower* — 40 % more
  blocks reach `hb_cache_store`.
- The payoff is the **warm** start, which is half the lane's goal (< 60 s) and which no arm has yet
  measured with a populated cache.

**What would refute me:** `rl_unkhelper` stays large → there is a third x23 producer I have not
found, and the enumeration above is incomplete. Or retention goes above 95 % and the warm run is no
faster → re-translation was never the warm cost either, and lever 1 is finished as a speed lever
regardless of how good its retention gets.

### Verified without the slot — and every check has a negative control

- **`tests/hb_reloc_kind_test.c`** (new). Deterministic, no host pointers in what it asserts, so it
  does not inherit the repo suite's ASLR flakiness. Pins: 32-bit ADD/XOR → x23 `kind=VALUE`
  `0xffffffff` below the floor; 64-bit ADD → no x23 site (the control that says it is the operand
  *size*); MUL/DIV → x23 `kind=HELPER`. **PASSED, identical across runs.** Two negative controls
  both fail it: dropping the tag in `emit_call_helper` (4 failures) and tagging by register the old
  way (2 failures).
- **`scripts/hb-check-reloc-invariant.sh`** extended with the new invariant: exactly one
  `HB_RELOC_KIND_HELPER` emitter, inside `emit_call_helper()`, and the store path never classifies
  on `rl->reg == 23` again. **Four negative controls, all fire**; the clean tree passes and both
  source files were byte-identical afterwards.
- **Repo suite: no regression.** 457/27, 457/27, 455/29, 455/29, 457/27 over five runs before the
  change and 457/27, 457/27, 455/29 after — the same known flaky band. Per the ASLR lesson, the
  sound claim is *no new failures*, not a count.
- Builds clean under `-Wall -Wextra -Werror`. Nothing deployed: an A/B was live the whole time.

### ⚠ A check that reported OK *because* it found the violation

The first version of the invariant check used `code_only < "$FILE" | grep -q PATTERN` under
`set -o pipefail`. `grep -q` exits at the first match, the upstream `grep -v` takes SIGPIPE (141),
and pipefail makes the pipeline non-zero — so the `if` never fired. **The check passed precisely
when it should have failed, and it is invisible in the passing case.** Caught only because the
negative control was run; the earlier `-q` in the same script had survived by luck, since its
upstream output was small enough to finish before the match.

Rule, now written into the script: never `| grep -q` under `pipefail` in a guard. Count with `-c`
and compare — it consumes all input and cannot race.

### State, and what the next thread should do

**Staged, built, NOT deployed** — the A/B (pid 67358, arms `off on off on`) held the slot
throughout and replacing `ntdll.so` under it would invalidate its numbers. Files touched, all in
this lane's territory: `include/hb_codegen.h`, `include/hb_contract_telemetry.h`,
`src/hb_arm64_codegen.c`, `src/hb_contract_telemetry.c`, `src/hb_runtime.c`,
`tests/hb_reloc_kind_test.c` (new), `scripts/hb-check-reloc-invariant.sh`.

1. **Read the hdrprobe A/B first** (`python3 scripts/hk-hdrprobe-readout.py <dir>`), in the order
   iteration 5 set out: `GATE_IN_GUEST`, then `PINNED` per `PINNED_SITE`, then `compiles`. It owns
   the slot until it finishes; do not deploy over it.
2. **Then A/B this fix, with `MACRUNNER_HB_CACHE_RELOC=1` on BOTH arms**, cold then warm on the same
   cache root. The interesting number is the warm pass, which nothing has measured yet.
3. ⚠ **`MACRUNNER_HB_CACHE_RELOC` is NOT set in `hk-run-try12-config.sh`.** The reloc path — and so
   this entire fix — is **inert in a default HK run**; only an arm that exports it is affected. Do
   not read a default-config run as evidence either way. Turning it on by default is a separate step
   and must not be taken while any A/B is live, for the reason iteration 5 recorded: an `off` arm
   that *unsets* the variable silently becomes a treatment arm the moment a `:-1` default appears.
4. Do not re-open: the helper registry (complete but for two flag-gated helpers), `hb_cache_store`
   failures (zero), `mem.disp` as the decline cause (unreachable with direct-mem off), and the
   main-path accounting residual (0, in both arms).

### Ready to launch — the measurement is written and the artifacts are staged

`scripts/hk-cachefix-ab.sh` (syntax-checked, not yet run). Arms `base` / `fix`, each **cold then
warm on the same cache root** — the warm pass is the point, and no arm in this lane has ever
measured a warm start with a populated cache.

Both ntdll builds are staged and, importantly, **distinguishable by content**:

| | sha | `rl_x23val` marker |
|---|---|---|
| `artifacts/hk-cachefix-ab/ntdll-base.so` | `63c9691c0f8af97f` | 0 |
| `artifacts/hk-cachefix-ab/ntdll-fix.so`  | `810c99a6de76bf20` | 1 |

The marker is a telemetry format-string field that exists only in the new build, so grading step 1
proves which binary produced the numbers rather than trusting what the driver believed it deployed.
The driver aborts if the two artifacts are not distinguishable, if a deployed SHA mismatches, or if
the base artifact carries the marker.

Relinked with `make -C engine/wine/build-arm64ec-spike dlls/ntdll/ntdll.so` after
`make -C engine/hyperbridge`; the link line shows `libhyperbridge.a` being consumed, and the
content check above is what confirms it landed. **Nothing deployed** — the iteration-5 A/B held the
slot throughout.

⚠ **Each arm gets its OWN cache root.** `HB_PERSIST_FLAG_RELOC` separates reloc from non-reloc
entries but NOT old-reloc from new-reloc, and both arms set `MACRUNNER_HB_CACHE_RELOC=1` — a shared
root would silently make the fix arm a warm start on the base arm's blobs.

**A fresh baseline arrived while this was being written.** The live hdrprobe A/B sets
`MACRUNNER_HB_CACHE_RELOC=1` on *both* its arms, so its `off1` arm is an independent
pre-fix measurement: `RETENTION_PCT=72.4`, `compiles=217790`, `xinput=133.8 s`. Against tree2's
72.6 % that makes the 72.x % baseline reproduced three times, which is what the >95 % prediction is
measured against.

**Disk: 31 GB free against disk-guard's 30 GB minimum.** Freed 9 finished roots of this lane's own
A/Bs. Deliberately NOT touched: the 22 `ntdll-<sha>` roots (sister lanes' warm caches) and the
880 MB `artifacts/_mr-run.*` prefix, which belongs to the live run. Worth the operator's attention
before a long cycle.

### ⚠ For whoever commits: one new file is INVISIBLE to `git status`

`.gitignore:60` ignores `engine/` wholesale, so **`engine/hyperbridge/tests/hb_reloc_kind_test.c`
does not appear in `git status` at all** — the tracked hyperbridge sources only show as modified
because they were force-added previously. It is the regression guard for a bug whose failure mode
is silent (a cached blob calling a stale helper), so losing it costs more than the diff.

Commit it explicitly: `git add -f engine/hyperbridge/tests/hb_reloc_kind_test.c`.

The two new scripts (`scripts/hk-cachefix-ab.sh`, and the extended
`scripts/hb-check-reloc-invariant.sh`) are outside `engine/` and show up normally.


---

## Iteration 7 — 2026-07-29 (23:05)

### Slot state on entry, and what was queued

The iteration-5 hdrprobe A/B (driver pid 67358) was live on arm 3 of 4 and owns the slot until
~23:35. It toggles a gate on the SHIPPED ntdll (`63c9691c0f8af97f`) and never redeploys;
`hk-cachefix-ab.sh` DOES deploy. Its own `wait_for_slot()` only watches for a live *guest*, so
between hdrprobe's arms the slot reads free and cachefix would have deployed `ntdll-fix.so`
under hdrprobe's next arm — voiding both A/Bs. Gated on the DRIVER PID instead
(`scripts/hk-chain-cachefix.sh`, launched 22:55, pid 61989, waits on `kill -0` then runs the
cachefix A/B). Both env var names it depends on were verified to exist in the source first:
`MACRUNNER_HB_TRANSLATION_CACHE_ROOT` and `MACRUNNER_HB_TRANSLATION_CACHE` at
`hb_runtime.c:2417-2418`, `MACRUNNER_HB_CACHE_RELOC` at `:1158`. If the root var had been wrong
both passes would have been cold and the whole A/B void.

### A claim in the brief that the arithmetic does not support

Cold start is **not** translation-bound, and no amount of retention reaches the < 120 s goal:

| | s |
|---|---|
| today | 476 |
| goal | 120 |
| **must remove** | **356** |
| all translation, everywhere (measured, iter "levers rescoped") | ~50 |

Even deleting translation entirely leaves 426 s. What is left is the ~73 % of CPU that is
**system** time in the fault/signal path — and that lives in `signal_arm64.c` / `macrunner_hb.c`,
outside this lane's territory. Lever 1 is a **warm**-start lever. That is still half the goal
(< 60 s) and it has never been measured, which is what the queued A/B closes.

### Two findings mined out of existing 69 MB run logs — no slot spent

New tools, all bounded-output streamers (never `cat`/`grep` a run.log):
`scripts/hk-telemetry-inventory.py` (every `macrunner-hb-*` channel + its last line, one pass),
`scripts/hk-rtmeter-readout.py`, `scripts/hb-cache-audit.py`.

**Finding A — the persistent cache is opened 77 times per process, and open is 99.9 % of the
cost of creating a JIT runtime.** `hb_jit_runtime_create()` (`hb_runtime.c:2391`) runs once per
guest thread and calls `hb_cache_open()` (`:2425`), which calls `load_entries()`
(`hb_aot_cache.c:255`) — a full read of the whole cache file into a **private** in-memory copy.
Reproduced on both hdrprobe arms:

| | off1 | on2 |
|---|---|---|
| runtimes | 77 | 77 |
| cumulative create cost | 3673 ms | 3533 ms |
| of which `cacheopen` | 3670 ms (**99.9 %**) | 3531 ms (99.9 %) |
| of which `jitbuf` (the 128 MB MAP_JIT arena) | 2 ms | 0 ms |
| `cacheopen_ms` first quarter → last quarter | 4.37 → 151.22 (**34.6x**) | 4.27 → 141.32 (33.1x) |
| max single open | 229 ms | 214 ms |

The 128 MB JIT arena everyone assumes is the expensive part costs **0.03 %** of runtime creation.
The cost is the cache load, and it grows with cache population.

**Finding B — the clobber hazard is REAL IN THE CODE and does NOT fire in these runs.**
`hb_cache_close()` rewrites the entire file from one runtime's private snapshot through a
`rename()` (`hb_aot_cache.c:213-229, 283-287`) while other runtimes `append_entry()` to the same
path in `"ab"` mode (`:231`). 77 writers, last-close-wins. **Predicted before looking:** the file
would hold far fewer records than the run's `stores=157665`.

**REFUTED.** Audit of the actual file: 158 873 records / 153 481 distinct keys / 52.6 MB,
parse clean to the last byte, no truncation. And the reason is exact — `rtmeter` (per create) = 77,
`translation-cache-summary` (per destroy) = **0**. The run is killed by its 900 s timeout, so
`hb_jit_runtime_destroy` never runs, `hb_cache_close` never runs, and the file is a pure append
log. **The bug is latent, not active** — it fires on a CLEAN exit, i.e. exactly what a user gets
when they quit the game normally. Recorded, not fixed here; it is not on the speed path.

### PREDICTION — registered before the queued warm run, with what would refute it

A cold run pays `cacheopen` against a file growing 0 → 52.6 MB, and measured 3.67 s. **A warm run
opens a FULL file all 77 times, from the first runtime.** Taking the cold run's own late-open cost
(96–229 ms, mean 151 ms in the last quarter, when the file was near final size):

- **warm `cum_total_ms` ≈ 77 x ~190 ms ≈ 11–17 s**, versus 3.7 s cold — a **3–4x rise**, and
  ~20–25 % of the entire < 60 s warm budget spent re-reading one file 77 times.
- **warm `cacheopen_ms` first-quarter mean rises from ~4 ms to > 100 ms** — this is the sharpest
  discriminator, because on a warm run there is no ramp: open #1 already sees the full file.
- warm RSS from the cache alone ≈ 77 x 42.4 MB of blob copies ≈ **3.3 GB**, feeding
  the already-recorded 48 GB attribution.

**What would refute me:** warm `cum_total_ms` stays near 3.7 s, or the warm first-quarter mean
stays single-digit ms. Either would mean `load_entries` is not re-reading the full file per open —
e.g. the OS page cache making it free, in which case the cost is malloc/index-rebuild bound and
NOT proportional to file size, and a shared-open fix buys memory but not time.

Both numbers come out of `scripts/hk-rtmeter-readout.py` on each arm's run.log, and the A/B
already runs cold-then-warm on the same root, so **the comparison needs no extra run**.

### RESULT — the prediction was half right, and the half it got wrong matters

The mechanism is **confirmed**; the magnitude I predicted is **refuted**, and I am recording the
number I measured rather than the one I registered.

`tests/hb_cache_open_bench.c` opens a REAL populated cache (the hdrprobe off1 arm's own file:
158 873 records, 52.6 MB) 77 times — HK's exact runtime count — keeping every handle live,
because that is what the runtime does: 77 JIT runtimes coexist. Unshared runs FIRST so it, not
the shared arm, gets the cold page cache; if shared still wins it is not a page-cache artefact.
Two runs:

| | total | per open | peak RSS |
|---|---|---|---|
| UNSHARED (today) | 2381 / 2487 ms | 30.9 / 32.3 ms | **5.08 GB** |
| SHARED | 45.6 / 52.2 ms | 0.59 / 0.68 ms | one handle (~66 MB) |
| | **-2.4 s (48-52x)** | | **-5.0 GB** |

**Where the prediction went wrong.** I predicted 11-17 s of warm cache-open cost, extrapolating
from the live run's own late-open cost (96-229 ms, mean 151 ms). The bench says 31 ms per open on
a full file. The live guest pays **5-7x more per open than a quiet process does for the identical
work.** So 2.4 s is a measured FLOOR for the warm saving, not the expected value, and the live
figure is plausibly 8-16 s — **but that is now an extrapolation and I am not claiming it.** The
queued cachefix A/B measures it directly on its warm passes at zero extra cost.

**The 5 GB is the finding I did not go looking for, and it probably explains the 5-7x.** 77
private copies of the same 42 MB of blobs plus 77 entry tables plus 77 hash indexes = 5.08 GB
measured, in a process already recorded at 48 GB RSS. Memory pressure at that scale is a very
plausible reason a 31 ms open costs 151-229 ms in the live guest — which makes the time and
memory results one finding, not two, and means fixing it should compound.

### The fix, and why sharing one handle is SAFE and not just faster

`hb_cache_open()` now returns a process-wide refcounted handle when
`MACRUNNER_HB_CACHE_SHARED=1` (default OFF — the live A/B's control arm must stay untouched, and
per iteration 5, a `:-1` default silently turns an `off` arm into a treatment arm).

The safety argument is a property of the existing API, not something the patch adds: **no pointer
into `entries` ever escapes the handle.** `hb_cache_get()` (`hb_aot_cache.c:372-384`) returns a
freshly malloc'd COPY including `native_code`, which the caller frees with
`hb_cache_entry_free()`. So the only requirement is mutual exclusion on the handle. The lock is
taken ONLY when shared, so the unshared path is byte-for-byte the code it was.

Public entry points were split into thin locking wrappers over `*_locked` internals, because
`hb_cache_lookup`->`hb_cache_get`, `hb_cache_store`->`hb_cache_put` and
`hb_cache_prune`->`hb_cache_clear` were all public-calling-public and would each have
self-deadlocked. INVARIANT: a `*_locked` never calls a public `hb_cache_*`.

Side effect worth naming: one shared handle is also **one writer**, which removes the latent
last-close-wins clobber (Finding B) rather than leaving it to be rediscovered.

### Verified — 11 assertions, and both negative controls fire

`tests/hb_cache_shared_test.c` (new): OFF gives two handles and a store is NOT cross-visible (the
control that says the test can tell the regimes apart, so a passing ON case is not vacuous); ON
gives one handle with immediate cross-visibility; **the refcount** — closing one holder leaves the
other usable, a later open still joins it; a different root is never aliased; and the reuse
counters report `opens=1 reuses=2` so a run can PROVE it took the path. **PASSED, 0 failures.**

Negative controls, each run in ISOLATION:
- refcount ignored (first close frees the shared handle) -> **3 failures**, exactly the three
  refcount assertions, and `opens=3 reuses=1` shows the handle being reloaded after each free.
- sharing forced off at the source -> **5 failures**, exactly the five ON assertions.

Repo suite **455 passed / 29 failed** — inside the recorded flaky band (457/27, 455/29 both seen
repeatedly). Per the ASLR lesson the sound claim is *no new failures*, not a count. Builds clean
under `-Wall -Wextra -Werror`. **Nothing deployed** — an A/B held the slot throughout.

### ⚠ A trap that nearly produced a false verdict, and the rule that catches it

Running both negative controls in ONE shell invocation reported the WRONG failure set for the
second (it showed the first control's 3 failures instead of its own 5). Re-running that control
alone gave 5, the correct ones. The batched loop had left a stale `libhyperbridge.a` in place, so
the test binary was linked against the previous control's object. A `touch` on the source plus
visible `make` output settled it.

**This is the same class as this repo's standing "make ≠ deploy" lesson, one level down: `make`
exiting 0 does not prove the artifact under test contains the change.** Rule for this lane: run
negative controls ONE PER INVOCATION, and confirm the rebuild happened by looking at the compile
line, never by the exit code. A control that reports the wrong failures is worse than no control —
it launders a broken build into a passing one.

### State

Built and unit-verified, **NOT deployed and NOT yet measured on Hollow Knight.** Files touched,
all in this lane's territory: `src/hb_aot_cache.c`, `include/hb_cache.h`,
`tests/hb_cache_shared_test.c` (new), `tests/hb_cache_open_bench.c` (new). New measurement tools:
`scripts/hk-telemetry-inventory.py`, `scripts/hk-rtmeter-readout.py`, `scripts/hb-cache-audit.py`,
`scripts/hk-chain-cachefix.sh`.

⚠ `.gitignore:60` ignores `engine/` wholesale, so **both new test files are invisible to
`git status`**. Commit explicitly:
`git add -f engine/hyperbridge/tests/hb_cache_shared_test.c engine/hyperbridge/tests/hb_cache_open_bench.c`

Because sharing is env-gated, its A/B needs **ONE binary with the env var toggled**, not a
two-binary deploy — no deploy-verification burden at all.


### The hdrprobe A/B finished — I read it, formed a hypothesis, and the fourth arm killed it

Iteration 5 left an instruction to read this A/B first. It completed at 23:25 (4 arms, gate
`MACRUNNER_HB_FAULT_SAFE_HEADER_PROBE`, `off on off on`). New tool `scripts/hk-compile-rate.py`
adds a throughput series the driver never produced: `translation-cache-progress` prints every 5000
compiles, so its SPACING is a compile-rate meter present in any run with the cache on — unlike
`macrunner-hb-rcache`, which needs `MACRUNNER_HB_TRACE_SYNCMETER=1` and is absent from all four.

| arm | gate | PINNED p2 | MEMMOVE_PCT p2 | tail compile rate | xinput |
|---|---|---|---|---|---|
| off1 | off | 1 | 30.0 | 252/s | 133.8 |
| on2 | **on** | 0 | **5.6** | 442/s | 131.5 |
| off3 | off | 3 | 52.9 | 183/s | 136.0 |
| on4 | **on** | 0 | **5.6** | **189/s** | 143.5 |

**The gate does exactly what it claims, replicated 2/2 each way and with no overlap:** PINNED 0 and
MEMMOVE 5.6 on both ON arms, PINNED 1/3 and MEMMOVE 30.0/52.9 on both OFF arms. As a wedge cure it
is settled.

**MY HYPOTHESIS, AND ITS REFUTATION.** From the first three arms the tail compile rate ordered
perfectly inversely with the wedge indicator (252/30.0, 442/5.6, 183/52.9 — 3/3), which said the
wedge suppresses late-run throughput and would have made the gate a cold-start lever. **arm on4
refuted it:** MEMMOVE 5.6, wedge-free by the same measure as on2, and a tail rate of **189/s** —
indistinguishable from off3's 183/s. Throughput does not follow the wedge.

I had the three-point correlation before on4 landed and waited for it rather than writing it up.
This is precisely the failure the lane brief warns about ("the worst hours came from reading
numbers after the fact") — a post-hoc correlation over 3 points, and the 4th killed it. It also
extends a recorded finding rather than contradicting it: the CPU collapse was already known to be
unexplained by pins; now late-run throughput is too.

**xinput is a null result and the within-arm spread proves it:** ON = 131.5 / 143.5, OFF = 133.8 /
136.0. The ON arms differ from EACH OTHER by 12.0 s while the arm means differ by 2.6 s. Any
reading of these four numbers as a treatment effect is noise.

**Verdict for the coordinator: the header-probe gate cures the wedge and buys no measurable
speed** — not xinput, not tail throughput. It remains worth having as a hang cure; it is not a
speed lever, and this lane should stop treating it as one.

### ⚠ CORRECTION — I read a format string as a measurement, and it reversed the conclusion

Retracted, in full: I wrote that `hk-pin-count.py` reported "100 % of samples inside
`macrunner_hb_primary_signal_handler`" for every arm including the wedge-free ones, and concluded
the cold start was ruled by a signal regime outside this lane's territory.

**That string is a literal in the script's own output** (`scripts/hk-pin-count.py:123`):

    print("PINNED=%d   (100%% of samples inside %s)" % (len(pinned), HANDLER))

It is the LABEL defining what PINNED counts — threads with 100 % of their samples in the handler —
not a measured quantity. On both ON arms PINNED=0, i.e. there were **zero** such threads. I had it
exactly backwards.

The raw `sample(1)` files were saved per arm, so the real profile was one command away. Non-idle
leaf composition at point 2:

| leaf | on2 (gate ON) | on4 (gate ON) | off3 (gate OFF) |
|---|---|---|---|
| `_sigtramp` | 22.8 % | 25.5 % | 12.7 % |
| `macrunner_hb_run_x64` | 9.8 % | 11.7 % | 4.7 % |
| `_platform_memmove` | 5.6 % | 5.6 % | **52.9 %** |
| `_platform_memset` | 5.0 % | 5.4 % | 4.3 % |
| `hb_jit_runtime_run` | 5.3 % | 4.7 % | 2.6 % |
| `try_promote_hot_block_families` | 5.1 % | 3.8 % | 1.5 % |
| `__wine_pe_x18_thunk` | 4.8 % | 5.2 % | 2.6 % |
| `hb_memory_read` | 4.4 % | 3.0 % | 1.3 % |
| `_tlv_get_addr` | 3.2 % | 3.9 % | 1.1 % |
| interpreter leaves (`exec_instr_unlocked`, `hb_flags_*`, `hb_context_read_reg_value`) | ~5 % | ~4 % | — |
| **NON-IDLE SAMPLES** | **20 685** | **20 560** | **45 639** |

**What this actually says, and it is close to the opposite of what I wrote.**

1. **Signal delivery is ~23-25 % of non-idle work on a healthy arm, not ~100 %.** The recorded
   "~73 % of CPU is system time" is a property of the **WEDGED** regime — visible here as off3's
   52.9 % memmove — and does NOT describe a wedge-free run.
2. **The wedge more than DOUBLES total non-idle work** (20.6k -> 45.6k samples) and nearly all of
   the excess is that one memmove. So the gate does remove a large real cost — while still moving
   neither xinput nor tail compile rate, which remains genuinely unexplained and is now a sharper
   question than before, not a vaguer one.
3. **Roughly three quarters of a healthy arm's non-idle time is hyperbridge's own user-space code,
   which IS this lane's territory.** I was one step from writing "the cold start is unreachable
   from `engine/hyperbridge/**`" into the journal as a finding. It would have been wrong, and the
   next iteration would have inherited it as settled.

**The lesson, and it is not "read more carefully".** I built a conclusion on a derived tool's
prose instead of on the measurement the tool was summarising, when the raw `sample` files sat
beside it in the same directory. Rule for this lane: **a claim about where time goes must cite a
leaf percentage from `sample(1)`'s own output, never a driver log's summary line.** Driver logs
carry labels; only the sample files carry data.

### In-territory cold-start levers, now that the profile is real

Sized off the two ON arms, worst-to-best confidence:

- **`_tlv_get_addr` 3.2-3.9 %** — macOS resolves every `__thread` access through a call.
  hyperbridge declares **14** thread-locals (`hb_runtime.c` 9, `hb_memory.c` 2, `hb_interpreter.c`
  2, `hb_arm64_codegen.c` 1) and uses `tls_model` on none. I called this the cheapest lever on the
  board; **both cheap routes to it are now refuted**, off-slot, in two minutes:

  1. **`tls_model` is SILENTLY IGNORED on Darwin arm64.** `plain`, `initial-exec` and `local-exec`
     compile to byte-identical code — the Mach-O TLV descriptor sequence
     `adrp @TLVPPAGE / ldr / ldr x8,[x0] / blr x8`. `initial-exec` and `local-exec` are ELF
     concepts. (The call is INDIRECT through the descriptor, which is why grepping the assembly
     for `tlv_get_addr` returns 0 and looks like success — check for `blr x8` after a
     `@TLVPPAGE`, never for the symbol name.)
  2. **Clang already CSEs the resolver within a function** — one call for two reads, and it stays
     one even across an opaque call, so it treats the address as invariant. Hoisting by hand buys
     nothing.

  So the 3.2-3.9 % is ~one resolver call per function-invocation that touches a thread-local, and
  cutting it means passing per-thread state explicitly through the hot paths — an invasive
  refactor, not a cheap flag. Re-rank it accordingly: real, but the most expensive of the four.
- **`try_promote_hot_block_families` 3.8-5.1 %** — the hot-family fusion, already known from this
  iteration to be 14.5 % of "compiles" that can never reach the persistent cache. It is being paid
  for twice.
- **`hb_memory_read`/`hb_memory_write` 4.7-6.6 %** plus `find_region_normalized` and
  `normalize_guest32_mirror_addr` ~2.5-3 %.
- **interpreter leaves ~4-5 %** — `exec_instr_unlocked`, `hb_flags_read/write_operand_value`,
  `hb_context_read_reg_value`. Blocks are still being interpreted, not just translated.

Out of territory and together the largest single block: `_sigtramp` (23-25 %),
`macrunner_hb_run_x64` (10-12 %), `__wine_pe_x18_thunk` (5 %).


### State at the end of iteration 7 — two A/Bs queued, nothing waiting on a human

Chained so the slot never idles; each gates on the PREVIOUS DRIVER'S PID, not on the slot, because
a deploying driver must not start between another driver's arms (between arms the slot legitimately
reads free):

| | driver | starts | measures |
|---|---|---|---|
| 1 | `hk-cachefix-ab.sh` (running, base/cold from 23:26) | after hdrprobe exited 23:25 | the reloc-KIND fix; **the first warm start this lane has ever measured** |
| 2 | `hk-sharedcache-ab.sh` (armed, pid 37043) | when 1 exits | the shared handle, ONE binary + env toggle |

Read them with `scripts/hk-rtmeter-readout.py` (cache-open cost) and `scripts/hk-compile-rate.py`
(throughput), in each arm's run dir. Grading order is in each driver's header. **The cachefix A/B's
warm passes also test iteration 7's registered cache-open prediction at zero extra cost** — warm
`cacheopen_ms` first-quarter mean should be > 100 ms where cold was ~4 ms, because a warm run has
no ramp.

**Not deployed, ready:** `artifacts/hk-sharedcache-ab/ntdll-shared.so` (`95d51e959aba3ccb`), carries
`MACRUNNER_HB_CACHE_SHARED` + `rl_x23val`, verified by content; `ntdll-fix.so` has `rl_x23val` only
and `ntdll-base.so` neither, so all three are distinguishable. Both new gates are default-OFF, so
this binary behaves as base for any sister lane that inherits it from `dist`.

**Scorecard for this iteration — three refutations, one fix, two corrections.**

| claim | outcome |
|---|---|
| cache open is 99.9 % of runtime creation, 77x, growing 34x | **confirmed**, both arms |
| shared handle: 48-52x faster opens, -5.0 GB RSS | **measured** off-slot on a real 52.6 MB cache |
| the on-disk cache is being clobbered by 77 writers | **refuted** — latent only; 0 destroys in timeout-killed runs |
| warm cache-open costs 11-17 s | **refuted on magnitude** — 2.4 s floor in a quiet process |
| the wedge suppresses late-run throughput (3/3 correlation) | **refuted by arm 4** |
| `tls_model` can remove `_tlv_get_addr` | **refuted** — silently ignored on Darwin; clang already CSEs it |
| "cold start is unreachable from `engine/hyperbridge/**`" | **RETRACTED** — I misread a format string; ~3/4 of a healthy arm's non-idle time is this lane's own code |

**★ For the operator — the disk pressure has ONE cause and I measured it.** Every HK run dir is
69 MB, and **68 MB of that is `dxmt-builtin-overlay`** — a full per-run COPY of the same DXMT DLL
build output (`aarch64-unix` 45 MB + `x86_64-windows` 13 MB + `aarch64-windows` 11 MB). run.log
itself is 0.5 MB; the evidence is not what fills the disk.

- **9.9 GB across 140 other run dirs**, and it grows by **71 MB per HK run**.
- I deleted only **this lane's own 4 finished** overlays (0.28 GB) plus its finished cache roots
  (0.21 GB). **Nothing of a sister lane's was touched** — that is the operator's call, and the
  overlay is a run's exact binaries, not just scratch.
- The systemic fix is to stage the overlay ONCE and symlink it per run instead of copying. That
  edit lands in the shared run harness and would change how every lane's runs are staged, so I am
  flagging it rather than doing it.
- Disk sat at **30-31 GB against disk-guard's 30 GB minimum** all iteration. The two queued A/Bs
  will add ~8 dirs (~0.6 GB) and will complete, but there is no headroom for a long cycle after
  them.

LOOP-STATUS: WORKING

---

## Iteration 8 — 2026-07-29 (23:40)

### Slot state on entry

Two A/Bs from iteration 7 are alive and correctly queued; nothing needed fixing.

| | driver | pid | state at 23:36 |
|---|---|---|---|
| 1 | `hk-cachefix-ab.sh cachefix 900 base fix` | 61989 | arm `base` pass `cold` launched 23:26, **waiting on the title slot** |
| 2 | `hk-chain-after.sh 61989 … hk-sharedcache-ab.sh` | 37043 | armed, gating on 61989 |

The slot is held by a **sister** lane (`MASTER-I11-WAITWAKE`, mr-run pid 69640, `timeout 1500`).
`laneA-run-hk.sh` waits up to `LANEA_SLOT_WAIT_MAX_ITERS=720` × 5 s = 1 h and explicitly does NOT
force-clean a live owner, so the queued arm is safe. Disk 30 GB free / 93 % — at disk-guard's floor,
unchanged from iteration 7 and still the binding constraint on a long cycle.

### The target: the 58.5 s in the DETERMINISTIC prefix that nobody has ever explained

Iteration 2 measured the prefix as wine+loader+xtajit64 27.0 s · Unity init 12.5 s ·
ReloadAssembly 32.6 s · **PhysX-ready → XInput 58.5 s**, and called the last "the largest and the
only one with no explanation at all". It is still unexplained, and it matters more than anything in
the tail: the prefix is ~144 s against a 120 s goal, so **no fix confined to the post-cliff tail can
reach the target**. This iteration attributes it, off-slot, out of logs already on disk.

**It reproduces.** Across the 26 run dirs on disk that reach XInput, the gap is 52.7–63.1 s
(n=26, mean ≈ 58). That is a stable, measurable block, not one run's bad luck.

### It is not a stall, and it is not translation — both from the logs, no slot spent

Measured on `laneA-SYNCBLOCK-a1-try1-183638/run.log` (1.0 MB, the most instrumented run on disk):

- **Not a stall.** The largest silent gap between consecutive stamped lines inside the whole 58.5 s
  window is **0.79 s**, and the ten largest gaps together are **11.3 %** of it. The window is
  continuous work, evenly spread — there is nothing here to unblock.
- **Not translation.** `macrunner-hb-rcache` moves `hits 17 799 499 → 40 999 046` while
  `miss 501 → 954`: **+23.2 M against +453**, i.e. **51 213 hits per miss**. Nothing is being
  compiled in this window; the persistent-cache lever (iteration 1–7's whole subject) cannot touch it.
- **Six threads are parked through all of it** — 944 `macrunner-hb-BLOCK` lines, 8 tids, six of them
  exactly 118 reports each, `pending_ms` p50 = 73 s. That is iteration 2's idle worker pool again,
  confirmed at a second point in the boot. Not a convoy, nothing to shrink.

Throughput by phase, same counter, same run:

| phase | wall | Δhits | rate | Δmiss |
|---|---|---|---|---|
| first line → PhysX | 38.0 s | 17.6 M | 463 k/s | 422 |
| **PhysX → XInput** | **57.9 s** | **23.2 M** | **401 k/s** | **453** |
| XInput → end | 444.9 s | 35.6 M | 80 k/s | 3102 |

### ⚠ What `rcache` actually counts — I nearly mis-framed this, and the name is the trap

`macrunner-hb-rcache` is **not** a translation/block cache. It is the per-thread **region cache for
`special_read`/`special_write`** (`macrunner_hb.c:7345`), whose comment states the mechanism
outright: *"With JIT_DIRECT_MEM off, EVERY guest memory access goes through special_read, which does
a mach_vm_region() syscall per access."* So the counter's unit is **one guest memory access**, and
the window performs **23.2 M of them at 401 k/s**.

Two things follow, and the second is the reason this is worth a run:

1. `hits` is the guest's memory-op rate, so the boot's cost is a *throughput* problem, not a
   blocked-on-something problem. 58.5 s / 23.2 M = **2.52 µs per guest memory access as an upper
   bound** — against ~1 ns for an inline load. Even a tenth of that is the window.
2. **`direct_mem=1` in every one of these runs.** `laneA-run-hk.sh` already sets
   `MACRUNNER_HB_JIT_DIRECT_MEM=1`, so the 2026-06-28 "root lever" is ON and these 23.2 M accesses
   are happening *anyway*. Whatever produces them is not the JIT load/store path.
   `hb_memory_read` (`hb_memory.c:1436`) reaches `special_read` only at :1466 — the branch taken when
   the address has **no covering region in hyperbridge's own region map**, or the region denies READ.
   That map is this lane's territory.

### PREDICTION — registered before the run, with what would refute it

The window is CPU-bound in the guest-memory helper path — `hb_memory_read`/`hb_memory_write` →
`macrunner_hb_special_read`/`_write` → `mach_vm_read_overwrite` — because the region map does not
cover the addresses the guest is touching, so a Mach syscall is paid per access.

Graded on a `sample(1)` taken INSIDE the window (t ≈ 92 s and t ≈ 118 s), plus exact counters from
`MACRUNNER_HB_TRACE_SPECIAL_IO_STATS=1`, in one ~240 s run:

1. **Memory-helper spine ≥ 30 %** of the busy thread's non-idle leaves
   (`special_read`/`special_write`/`hb_memory_read`/`hb_memory_write`/`mach_vm_read_overwrite`).
2. **`read_mach` + `write_mach` ≥ 50 % of chunks** — i.e. the syscall, not the direct path, is what
   services them. `read_direct_ok` is the counter that says otherwise.
3. **`_sigtramp` < 15 %**, materially below the 23–25 % iteration 7 measured in the tail: this window
   has no fault wedge, so if signals dominate here too, the memory framing is wrong.
4. Translation leaves < 5 %, consistent with 453 misses in 58 s.

**Refuted if:** memory-helper leaves < 10 % (the rcache counter is then incremented off the critical
path and its rate is a red herring); or `_sigtramp` ≥ 25 % (same signal regime as the tail, and this
window is not a distinct problem); or `read_direct_ok` dominates `read_mach` (the direct path already
works and the cost is elsewhere in the helper).

Per this lane's standing rule, the verdict must cite leaf percentages from `sample(1)`'s own output,
never a driver log's summary line.
LOOP-STATUS: BLOCKED — every backend in the failover chain (claude) retired after 2 consecutive no-work fast returns; quota or auth is exhausted and the operator must refresh it.
