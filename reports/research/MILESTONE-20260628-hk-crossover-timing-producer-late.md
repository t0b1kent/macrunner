# MILESTONE 2026-06-28 — HK CrossOver timing oracle: producer is EARLY under CrossOver (~6 s) vs LATE under MacRunner (~30 s)

**Verdict (evidence):** Under CrossOver (renders correctly) the render **producer publishes its first ready frame at
~6.4 s** from launch and the **consumer reaches present at ~7 s** — essentially the instant the engine is up. Under
MacRunner the producer does not publish (+0x40=1 @rva 0x586da3) until **~30 s**. Per the disambiguation, this means
**MacRunner's producer is abnormally slow/late (throughput/scheduling) — NOT a consumer-doesn't-block bug.** The consumer
is a **pure poll on both** sides (same UnityPlayer binary), so the only divergence is *when a frame becomes available*.

Harness: CrossOver `wine winedbg`, UnityPlayer base 0x6ffffc620000 (deterministic), perl-alarm self-timeout, scoped
`wineserver -k`. HK MacRunner lane (Codex) + golden + tree untouched; CrossOver bottle only.

## Q1 — producer first-publish time (wall-clock from launch, first-hit of each marker; separate runs)

| marker (rva) | meaning | CrossOver first-hit | MacRunner |
|---|---|---|---|
| 0x586da3 `movl $1,0x40(%rsi)` | **producer publishes ready frame** | **~6.4 s** | **~30 s** |
| 0x5872dc `cmpl $1,%esi` | consumer gate (engine running) | ~7.8 s | (spins, esi=0) |
| 0x587381 `callq *0x68(%rax)` | **consumer reaches present** | **~7.0 s** | never |

All three cluster at ~6–8 s (separate runs → ~1–2 s boot variance), i.e. the producer publishes and the consumer
presents as soon as the engine boots. There is **no ~24 s gap** under CrossOver. (CrossOver times include winedbg +
Rosetta Wine boot of a few seconds; the producer-latency-after-engine-start is ≈0.) ⇒ **CrossOver producer ≪ 30 s.**

## Q2 — does the CrossOver consumer block-wait on the +0x1f8 semaphore, or poll? → **POLLS (no block-wait)**

Disassembly of the render-decision function rva 0x5871b0 (and its entry helper 0x587500):
- `0x587500` (called at func entry 0x5871d8): checks `[mgr+0x20]` and `pending_count [mgr+0x310]`; on no-pending it
  jumps to **0x5876fe = `add rsp,0x50; pop r14; ret`** — it simply **returns. No wait.**
- `0x5871b0` body: dequeue one item (`call 0x586a80` @0x587244) → read `esi=[item+0x40]` @0x587273 →
  `call kernel32+0xfb50` @0x587283 (**= QueryPerformanceCounter**; thunk `jmpq → ntdll+0x30660`, `rcx=&local` —
  a timestamp, NOT a wait) → gate `cmp esi,1` @0x5872dc → present (esi==1) or `mov byte [r15+0x348],0; xor al,al; ret`
  (esi!=1, returns FALSE) @0x5874d8.
- **No `WaitForSingleObject`/`WaitOnAddress`/semaphore-acquire on +0x1f8 in the dequeue/present-decision path.** It is a
  single-shot *try-dequeue-one*; the outer loop calls it repeatedly = a **poll**. The +0x1f8 semaphore is signaled by the
  producer (0x5841f0) but the consumer's dequeue decision does not block on it. Same binary ⇒ MacRunner polls identically.

## Q3 — does the CrossOver consumer dequeue a ready item and reach present? → **YES, at ~7 s**

The present slot 0x587381 is hit at ~7.0 s (and earlier oracle 2026-06-27 confirmed all of 0x587381/738a/73ab execute
on the same item the producer published, esi=1). So under CrossOver the full pipeline producer→gate(esi=1)→present runs
within ~7 s of launch.

## Disambiguation result

- ✅ **CrossOver producer EARLY (~6.4 s ≪ 30 s) ⇒ MacRunner producer is abnormally slow/late** = a **producer-side
  throughput/scheduling** problem.
- ❌ NOT "CrossOver producer also ~30 s but consumer blocks": the CrossOver producer is early, and the consumer **polls**
  on both sides (it does not block-wait). The consumer is correct/identical; under MacRunner it spins on `item+0x40==0`
  for ~30 s simply because **the producer hasn't published a frame yet** (matches the ~572 reads / non-overlapping probe
  windows in RCA-CORRECTION-20260628-... ; consumer reads 0 because nothing is produced, not because of incoherence).

## Reconciliation + fix direction

This closes the loop with the prior findings: there is **no mapping split / coherence bug** (both threads resolve the
item identity to the same host page; the "two host backings" was a `mem=` probe misread). The real gate to HK's first
frame is **producer throughput** — UnityPlayer's frame-producer (and the scene-load / engine work it depends on) takes
~30 s to reach first-publish under MacRunner vs ~6 s under CrossOver/Rosetta. This is the **same double-emulation /
cold-cache JIT-compute throughput class** documented in the ABZU CrossOver oracle (per-callback JIT-runtime + cache
churn; runtime-pool / lift-cache levers). **Fix lever = producer-path throughput**, not memory coherence and not the
consumer's wait/poll behavior.

Next: profile the MacRunner producer thread (tid 8000, rva 0x586ca0 and upstream scene-load) over 0→30 s with native
`sample` + warm translation cache to localize where the ~30 s goes (JIT lift/exec of the producer's path, cache I/O,
per-callback overhead, or a serializing wait it does block on), then attack that with the established throughput levers.

**Rails:** READ-ONLY (winedbg, no edits/build/run on the MacRunner side); Codex HK lane + golden + tree untouched;
scoped CrossOver `wineserver -k` only (dist-arm64ec-spike wineserver verified alive throughout).
