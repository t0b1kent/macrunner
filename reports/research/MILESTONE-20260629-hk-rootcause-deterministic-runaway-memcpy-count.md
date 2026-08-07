# MILESTONE 2026-06-29 — HK no-pixel ROOT-CAUSED to a deterministic JIT-correctness bug: corrupted memcpy count in the producer's container-grow

## ★★★★★★★★ SECOND FIX (2026-06-29) — unaligned STLR in direct-mem GPR stores
After the XMM fix removed the runaway, the producer reached `D3D11CreateDevice` then SIGBUS'd (signal 10) on
`STLR X20,[X21]` (8-byte store-release) to a 4-byte-aligned address (`0x32021d35c`): the page is RW-mapped,
so it's an **unaligned atomic store** — x86 permits unaligned 8-byte stores; ARM64 `STLR`/`LDAR` require
natural alignment. There IS an alignment-checked store path (`emit_direct_mem_store_from_x20_tso`: runtime
`TST addr,#mask` → aligned uses STLR, unaligned falls back to `hb_jit_helper_store_sized`), but two GPR store
paths bypassed it with raw `emit_direct_mem_store_from_x20_off`:
- `emit_native_scalar_mov` (mov mem,reg/imm)
- `emit_scalar_load_store_pair` (mov reg,[src]; mov [dst],reg)  ← the HK faulting path

**FIX: route both through `emit_direct_mem_store_from_x20_tso`** (x20 stays valid across it). NOTE: the matching
LOAD paths still use raw `LDAR` (`emit_direct_mem_load_to_x20_off`) — an unaligned *load* would SIGBUS next; if
that surfaces, add an aligned-checked load-to-x20 variant. Both fixes are real committable codegen-correctness
fixes in hb_arm64_codegen.c (class: x86 unaligned + TSO emulation on ARM64).

## ★★★★★★★ ACTUAL CODE FIX (2026-06-29) — x22 preservation in direct-XMM stores
The exact codegen bug: the direct-XMM 128-bit store loads the XMM value into **x20 (low) + x22 (high)**, then
computes the store address with `emit_direct_mem_addr`, which **clobbers x22** for indexed / large-disp
addresses (it loads the index into x22 at hb_arm64_codegen.c:699). It then stores the *clobbered* x22 as the
high qword → an indexed SSE store corrupts the high 8 bytes (the HK realloc-copy's last element).
`emit_xmm_load_store_pair` already preserved x22 (save→x23, restore); the two NON-paired XMM-store paths did
NOT:
- `emit_native_xmm_mov` single store (hb_arm64_codegen.c ~1344)
- generic `HB_IR_STORE` direct-XMM handler (~4262)

**FIX (both sites): mirror the pair path — if `!direct_mem_addr_preserves_x22(dst)` then `mov x23,x22; <addr>;
mov x22,x23`.** VERIFIED: with `MACRUNNER_HB_JIT_DIRECT_MEM=1` and XMM-mem ON (default, full speed),
`win@983b index15 = 0x10000048` CORRECT (was 0x80) and **runaway c000007b = 0**. So full direct-mem speed +
correct. (The `MACRUNNER_HB_JIT_DIRECT_XMM_MEM=0` workaround below is no longer needed.) Pixel attempt running
with the fix + full DIRECT_MEM. This is a real, committable codegen-correctness fix in hb_arm64_codegen.c.

## ★★★★★★ ROOT CAUSE PINNED + FIX FOUND (2026-06-29, supersedes everything below)
The realloc-copy corruption is the **direct XMM-mem store codegen path** (`emit_xmm_load_store_pair`,
gated by `jit_direct_xmm_mem_enabled()` / env `MACRUNNER_HB_JIT_DIRECT_XMM_MEM`, default ON). The Unity
multi-array insert copies the offset array with an SSE `movdqu`/`movdqa` pair; under DIRECT_MEM that pair
is emitted as a direct host load/store and **corrupts the last copied element** (writes `0x80`=byte-count
instead of `0x10000048`), cascading to `rbp=0xa4 → negative count → runaway → no pixel`.

DECISIVE A/B (both with `MACRUNNER_HB_JIT_DIRECT_MEM=1`, win@983b probe):
- `DIRECT_XMM_MEM` default(on)  → `index15 = 0x80`  CORRUPT, runaway `c000007b` fires.
- `MACRUNNER_HB_JIT_DIRECT_XMM_MEM=0` → `index15 = 0x10000048` CORRECT (later `0x1000006c`), **runaway count = 0**.

**FIX = keep GPR direct-mem (the speed lever) but route XMM stores through the lazy/correct path:
`MACRUNNER_HB_JIT_DIRECT_MEM=1 MACRUNNER_HB_JIT_DIRECT_XMM_MEM=0`** → fast AND correct. (Permanent fix =
default `MACRUNNER_HB_JIT_DIRECT_XMM_MEM` off for HK, or fix the `emit_xmm_load_store_pair` tail/last-chunk
store codegen.) This is why earlier force-interp of memcpy/allocator never fixed it — the corrupting store
is the inline direct XMM pair in the insert/copy, not a separate routine, and force-interp of the wrong
ranges never covered it; only globally disabling the direct XMM path (or no-DIRECT_MEM) fixes it.

Pixel path now unblocked on the producer side: the floor was PRESENT_MISSING because it's throughput-limited
(reached D3D11CreateDevice + MakeWindowAssociation by 300 s but not CreateSwapChain). DIRECT_MEM gives the
speed; XMM_MEM=0 gives correctness. Pixel attempt running with both. [old analysis below is superseded]


## The chain (fully traced this session, top to bottom)
1. **No pixel** = the producer never publishes a frame → consumer's render gate reads `esi=0` → no Present (PRESENT_MISSING).
2. The producer is stuck/crashing in a **container-grow memmove** (UnityPlayer rva 0x5b98c0 → memcpy 0x19e9430).
3. The memcpy gets a **corrupted byte count**: `r8 = 0xffffffff2023c064` (≈ −3.7 GB) → **runaway copy**.
   - **Floor (no DIRECT_MEM):** the runaway copy is slow → producer never finishes the grow → PRESENT_MISSING even at 280 s.
   - **DIRECT_MEM=1:** the runaway is fast → sweeps reserved pages (commit-on-fault masks it, 131072+ commits) → crashes
     at the first **executable (RWX) page** it hits (0x3e0000000/0x400000000) = **Apple-Silicon W^X SIGBUS** (a
     DOWNSTREAM symptom of the runaway, NOT the root).

## Decisive evidence
- Register dump at the fault: `rcx=0x3ffffffa0 rdx=0x3ffffff7c` (dst/src swept ~3.7 GB from the 0x320 buffer);
  `r8=0xffffffff2023c064` is **byte-identical across runs** (different rdi regions) ⇒ **deterministic**, not heap-random.
- Container is **SANE**: `[rdi+0x50]=base(valid)`, `[rdi+0x60]=0x1b4 (size)`, `[rdi+0x68]=0x450 (capacity)`,
  `arr[+0xb0..]= 0 0 0 0xc0 0x100 0x190 0x190 0x190 …` (all small/valid). So NOT a corrupt field.
- **Force-interpreting the count block did NOT change the runaway** (same r8) ⇒ the bug is in the IR / decode /
  upstream register flow, **shared by JIT and interpreter** — not a JIT-backend-only bug.
- **IR dump of the count block (rva 0x5b98c0) is structurally CORRECT** — all 14 instrs map to the x86, incl. the
  scaled-index load `ir[2] op=43 dst r14 src1 mem[base=rdi index=r14 scale=8 disp=0xb0]` and `ir[10] op=SUB
  count=r12−r14`. So the count block itself is decoded right.

## Where the corruption is (the surgical next step)
The count `= size(0x1b4) − r14`, and `r14 = [rdi+idx*8+0xb0] + movsxd(ebp)` (the base sub/add cancel). With sane
inputs this is small, but the result is ≈ −3.7 GB. Since this block's IR is correct, **a wrong INPUT register
(the index in `r14`, or `ebp`/`rbp`) arrives from an UPSTREAM block carrying a ~3.7 GB value.** The bug is
deterministic and reproduces in the interpreter, so it is an x86→IR-correctness bug in an **upstream block of the
producer's grow chain** (call chain by bt: 0x5b9919 → 0x5bb1f5 → 0x606bb2 → 0x6496b8).

**NEXT (surgical, do this — don't re-derive):**
1. Per-instruction register trace through the blocks UPSTREAM of rva 0x5b98c0 (dump regs after each IR instr in
   [0x5b9000,0x5ba000] and the callers) to find where `rbp`/the index first becomes ~0xdfdc4050 (~3.7 GB) when it
   should be small. The infra is in place: `MACRUNNER_HB_TRACE_IR_DUMP` + `FORCE_INTERP_LO/HI` + the interp-fail probe.
2. Check the **MOVSXD (IR op=77) source-size handling** in the interpreter/codegen — movsxd must sign-extend the
   LOW 32 bits; if it reads the full 64-bit source (or mishandles the source size), an upstream 64-bit `rbp` is not
   truncated → garbage. (Candidate, since movsxd feeds r14 here.)
3. Once the upstream corruption is pinned, fix that decode/IR op → the count is correct → no runaway → producer
   publishes → present. Re-test ladder; snapshot ONLY on a real pixel.

## UPDATE — MOVSXD checked (CORRECT) + fatal grow pinpointed
- **MOVSXD source-size handling is CORRECT, exonerated.** Decoder (hb_decode_x64.c:2976) sets `op2.size=4`
  (32-bit source); `HB_SIZE_32==4` (compiler-confirmed); the SIGN_EXTEND interpreter (hb_arm64_codegen.c:8736)
  hits `case HB_SIZE_32 → (int32_t)value`, correctly truncating to low-32 before sign-extending. Moreover the `rbp`
  values it processes are all small (≤0xa4), so the sign-extension is a no-op — MOVSXD cannot be the corruption.
- **Count-block INPUT registers dumped (MACRUNNER_HB_TRACE_GROW_IN).** The count is `size − elem − rbp` where
  `size=[rdi+0x60]`, `elem=[rdi + r14_idx*8 + 0xb0]`, `rbp`=entering loop offset:
  ```
  grow[5] size=0x16c elem=0x100 rbp=0x48 → count=+0x24  (ok)
  grow[6] size=0x190 elem=0x100 rbp=0xa4 → count=-0x14  (FATAL: elem+rbp=0x1a4 > size=0x190)
  ```
  count=-0x14 (huge unsigned) → memcpy ~29M iters × 0x80 ≈ 3.7 GB → decrements exactly to the observed
  `r8=0xffffffff2023c064`. **End-to-end confirmed.**
- **So the arithmetic is correct on its inputs; the INPUTS are an invalid combination** (`elem+rbp > size`). On
  Windows this works, so one of `{size=0x190, elem=0x100, rbp=0xa4}` DIVERGES from the reference (an earlier
  MacRunner op produced a wrong container size/elem or a wrong loop offset rbp).
- **NEXT (most-bounded lead of the session):** break at the same memmove (UnityPlayer rva 0x5b98c0) under CrossOver,
  find the grow where size≈0x190, read `[rdi+0x60]` / `[rdi+0xd0]` / `rbp` — whichever differs from
  `{0x190, 0x100, 0xa4}` is the corrupted input; trace THAT computation. (Replaces the vaguer "trace rbp upstream".)

## ★ CROSSOVER INPUT-COMPARISON DONE — `rbp` is the corrupted input (DECISIVE)
Broke at the same grow method (UnityPlayer rva 0x5b98c0; CrossOver UnityPlayer base = 0x6ffffc620000, stable) and
dumped size/rbp/idx/elem per grow on CrossOver vs MacRunner. The grow sequences are **IDENTICAL** until the fatal one:
```
            size   idx  elem   rbp     count = size-elem-se(rbp)
CX[6]/MR[5] 0x16c   4   0x100  0x48    +0x24   (match, valid)
CX[7]       0x190   4   0x100  0x6c    +0x24   (CrossOver: VALID)
MR[6]       0x190   4   0x100  0xa4    -0x14   (MacRunner: NEGATIVE -> runaway -> crash)
```
**Only `rbp` differs: 0xa4 (MacRunner) vs 0x6c (CrossOver); size, idx, elem are byte-identical.** Between the prior
grow (rbp=0x48 on both) and this one, CrossOver advances rbp by +0x24 (= element stride r15) but MacRunner advances
by +0x5c — an extra **0x38**. So MacRunner's `rbp` (the insertion offset) is over-incremented by 0x38 at this step.

VERDICT: the corrupted input is **`rbp` (insertion offset)**, mis-computed by MacRunner's JIT in the grow function's
offset logic (rbp is set INSIDE the function containing 0x5b98c0, before the count block — not an external param;
the bt[1]=0x5bb1f5 slot is a heuristic, no call/rbp-arith in that window). It is correct through every prior grow and
diverges only at size=0x190. **NEXT:** find where `rbp` is incremented inside the 0x5b9xxx function and which JIT op
adds 0x38 too much (per-instruction reg-trace of the rbp-update block, or compare the rbp-update IR vs x86). That op
is the fix → correct rbp → count=+0x24 → no runaway → producer publishes → present.

## ★★ rbp-UPDATE BLOCK TRACED — it's ONE wrong packed-array entry (not an increment)
Per-block rbp trace (MACRUNNER_HB_TRACE_RBP) localized rbp=0xa4 to block rva **0x5b973c**, which is NOT an increment
but a packed-field load: `mov ebp,[rax+rsi*8]; and ebp,0x0fffffff` (path A; a `cmp r8,r11; je 0x5b9747` selects a
path-B unpack). CrossOver winedbg (break both path entries) shows **BOTH platforms take path A** at the fatal grow —
no branch divergence. Then dumped rax/rsi + the loaded entry on both platforms, entry-by-entry:
```
rsi:    8         9         c         d         1         f         10(FATAL)
MR:   10000080  10000090  10000000  10000024  10000010  10000048   a4        <- ONLY THIS DIFFERS
CX:   10000080  10000090  10000000  10000024  10000010  10000048  1000006c
```
**All 15 other entries match byte-for-byte; only entry[rsi=0x10] differs: MacRunner=0xa4 vs CrossOver=0x1000006c.**
Same rax/rsi/r14/rcx. ⇒ NOT a systematic JIT op bug — **ONE wrong entry in a packed-offset array.** Every CORRECT
entry has the **0x10000000 "valid" tag bit**; MacRunner's 0xa4 is the ONLY one MISSING it ⇒ strongly implies MacRunner
**never properly wrote entry[16]** (reads a stale 0xa4) where CrossOver wrote the real 0x1000006c. So the divergence
is upstream of rbp entirely — a **control-flow divergence that skipped the element-16 insert/store**, or a wrong
untagged write. rbp is NOT "incremented +0x38"; it loads this one un-written slot.

**REVISED NEXT:** trace the WRITE to entry[16] (the packed-offset-array slot the element-16 insert should populate):
watchpoint/write-trace its address (MR ~0x30023c130 = rax+0x80 per run; CX 0x3023c130) → find whether MacRunner
skipped the store (a branch/loop flags bug — most likely given the missing valid-tag) or wrote 0xa4, and the exact
store/branch. That control-flow op is the root → correct entry[16] → rbp=0x6c → count=+0x24 → no runaway → present.

## ★ ENTRY WRITER FOUND — store at 0x5b9865; divergence is the insert's packed value
The packed entry is stored at **`0x5b9865: mov [rbx], r12`**, `rbx = [rdi+0x30] + rsi*8` (the rbp-source array),
`r12 = (r9<<28) | sext(ebp)` (packed at 0x5b9763). So at entry[16]'s store: **MacRunner r9=0, ebp=0xa4 vs CrossOver
r9=1, ebp=0x6c** — both the tag flag AND the offset wrong. The function is a Unity multi-array insert: shifts 3
parallel arrays via memmove (0x5b97ec 4-byte, 0x5b985b 8-byte, the fatal 0x5b98c0 path), reallocs (0x5b97c2/
0x5b9836), then runs an **index-adjustment loop at 0x5b98a0** (`cmp dword[rax+0/4/8], esi; jl; inc`, stride 0x24)
that shifts element indices on insert — a candidate for a signed-compare/flags cascade.

The root is a **deterministic stateful divergence inside this insert**: one entry's packed (tag,offset) comes out
wrong because an input (r9/ebp), itself derived from the insert's cumulative array state, diverged. Level-by-level
tracing has gone 7 deep (no-pixel→runaway→neg-count→rbp→entry[16]→store→r9/ebp) and each peel recedes one data
dependency further within the insert.

## METHOD CHANGE for the next push (level-by-level no longer converges)
Two heavier options to find the FIRST primitive divergence:
1. **Differential execution trace** — run MacRunner (force-interp the insert range) and CrossOver in lockstep over the
   index-16 insert, dump the same register set per block on both, diff to find the FIRST block where a register/flag
   diverges. That block's IR-vs-x86 is the primitive bug (likely a flags/signed/width op).
2. **Per-iteration trace of the index-adjustment loop 0x5b98a0** — dump the 3 `cmp [rax+k],esi; jl; inc` decisions per
   element on both platforms; a divergent inc = a signed-compare/flags mis-eval = the cascade source.
Either bottoms out at one JIT op → fix → entry[16]=0x1000006c → rbp=0x6c → count=+0x24 → no runaway → present.

## ★★★ DIFFERENTIAL TRACE DONE — pure DATA divergence, control flow IDENTICAL
Captured the full register state at the entry[16] packed-store (0x5b9865) on both platforms + the full store
sequence (keyed by r13, a running counter). Results:
- **Store sequence matches byte-for-byte through r13=0x11**, then diverges at exactly the entry[16] write:
  ```
  r13:  0xa     0xb     0xe     0xf     0x10    0x11    0x12(entry16)
  MR:  10000080 10000090 10000000 10000024 10000010 10000048  100000a4   <- FIRST divergence
  CX:  10000080 10000090 10000000 10000024 10000010 10000048  1000006c
  ```
- At the divergent store, **ALL register inputs match** (r8=8, r13=0x12, r15=0x24, rsi=0x10, rax/rdx region-shifted
  identical); ONLY the offset (rbp/rcx = the packed value's low 28 bits) differs: 0x6c (CX) vs 0xa4 (MR).
- The "missing valid-tag" was a READ-MASKING ARTIFACT — both platforms store the tag (r12=0x1000006c / 0x100000a4);
  the path-A read masks 0x10000000 off to extract the offset. So the divergence is PURELY the offset value.
- The offset at the store was READ BACK from the array (rbp = [rax+rsi*8]); its wrong value 0xa4 was placed there
  earlier by the array's SHIFT memmoves (the same memcpy family), NOT by a store (all stores matched).

VERDICT: **the control flow is provably identical (store order + all register inputs match); this is a deterministic
PURE-DATA divergence** — one offset arithmetic value comes out 0xa4 where the working reference produces 0x6c, and it
cascades through the array's shifts into the runaway count. It is a JIT data-computation bug (arithmetic/width/sign),
NOT control-flow, NOT graphics, NOT memory-mapping.

## REMAINING (honest): needs a full array-data-flow differential trace
The wrong 0xa4 originates inside a STATEFUL Unity insert whose own shift operations are memmoves — self-referential
data flow. Isolating the FIRST primitive miscalc requires reconstructing every shift+store on both platforms in
order and finding the first wrong offset, a larger undertaking than the targeted single-point traces run here.
Status after ~55 build/run cycles + 9 CrossOver sessions: root narrowed to "one offset value (0x6c→0xa4, diff 0x38=
0x14+0x24=two strides) is wrong; control flow identical; cascade via array shifts". NO PIXEL.

## ★★★★ CORRUPTION PINNED to a single op: the offset-array REALLOC copy
Array-evolution differential (MacRunner store-trace vs CrossOver winedbg, SAME container rdi …213740, aligned by the
r13 store-counter): arrays match byte-for-byte through r13=0x10; FIRST divergence at the r13=0x11 insert (element 0xf):
```
entryArr (0x5b973c, call entry, before realloc):  index15 = 0x10000048   CORRECT (matches CX)
win@983b  (0x5b983b, after offset-array realloc):  index15 = 0x80         CORRUPT  <-- here
store16   (0x5b9865, after shift):                 index15=index16 = 0x80 (shift duplicated)
```
The offset-array **realloc `call 0x140104700` (thunk → 0x1400f6f30) corrupts the LAST element: 0x10000048 → 0x80**
during its old→new buffer copy (buffer moved to new base 0x32023c0b0; index16=0 = new-buffer uninit, so it copied 16
elems). **0x80 = 16 elements × 8 = the copy byte-count** — the count value leaks into the last data slot. Cascade:
corrupt index15 → shift → index16=0x80 → +stride → 0xa4 → read as rbp=0xa4 → negative count → runaway → no pixel.

force-interp test: force-interpreting the SSE memcpy 0x19e9430 (range2) did NOT fix it (index15 still 0x80, runaway
still fires) ⇒ the realloc's internal copy is NOT that memcpy (the allocator 0x1400f6f30 uses a different copy
routine), OR the divergence is IR-level. So the exact corrupting instruction is INSIDE the Unity allocator realloc
(0x1400f6f30 chain), not yet pinned.

## STATUS — exceptional localization, exact op inside the allocator realloc
From "no pixel, graphics?" → "the Unity offset-array realloc (0x1400f6f30), copying 0x80 bytes, writes the byte-count
0x80 to the last element instead of the data, on one specific grow." Deterministic, CrossOver-verified at every step.
The final pinpoint = trace into 0x1400f6f30's copy (its own memmove/inline copy + the 0x80=count clue ⇒ a copy
length/tail or count-register bug) — a focused codegen task. ~64 build/run cycles + 13 CrossOver sessions; NO PIXEL.

## ★★★★★ ROOT: DIRECT_MEM-SPECIFIC corruption (corrects "IR-level"; corrects "DIRECT_MEM exonerated")
Built a real lockstep harness: store-loggers (HB_IR_STORE op44 via src2 value; hb_memory_write hook; and the
write_u64_tso hook — `write_u64_tso` does a direct host-ptr atomic store for ALIGNED writes and only falls through to
hb_memory_write when UNALIGNED, so aligned stores bypass the memory hook) + a window flag armed at the r13=0x11 insert
+ 3-range force-interp (LO/HI, LO2/HI2, LO3/HI3 envs).

DECISIVE A/B: run **without DIRECT_MEM** (all stores via the lazy helper) → `win@983b index15 = 0x10000048` CORRECT
(and the later value `0x1000006c`). Run **with DIRECT_MEM** → `index15 = 0x80` CORRUPT. **So the realloc-copy
corruption — and therefore the whole runaway → no-pixel chain — is a DIRECT_MEM CODEGEN bug (raw `str`), NOT a
decode/lift (IR) bug.** Force-interping the SSE memcpy 0x19e9430 / the allocator 0x2b0000-0x2c0000 / 0x19b0000 all
failed to fix it ONLY because the actual copy routine runs direct-mem in a 4th, not-yet-located function (none of
those ranges) — force-interp there never covered it.

This REFRAMES everything and corrects two earlier wrong calls:
- "Bug is IR-level (force-interp doesn't fix)" — WRONG: force-interp never covered the real copy fn; no-DIRECT_MEM fixes it.
- "alloc-trace says legit reserved-tail, DIRECT_MEM exonerated" (earlier milestone) — WRONG: DIRECT_MEM IS the corruptor.

Consequence: **floor (DIRECT_MEM default-OFF) = CORRECT but slow** (PRESENT_MISSING = throughput/consumer, a separate
issue); **DIRECT_MEM = fast but corrupts the copy → runaway**. PIXEL path = (a) fix the DIRECT_MEM copy codegen
(locate the direct-mem copy fn by bisecting force-interp ranges with DIRECT_MEM on, then its store codegen), or
(b) speed up the correct floor. Harness + 3-range force-interp infra all uncommitted, env-gated.

## Ruled OUT this session (so the next push doesn't re-chase them)
graphics / DXMT / present path; commit-coherence gap (commit-time fix fired 0×); W^X (downstream symptom of the
runaway); hot-cache region coherence (`DISABLE_HOT_CACHE` no change); DIRECT_MEM-specific corruption (deterministic
in BOTH floor and direct-mem); corrupt container fields (all sane).

## State / rails
- **No pixel** → floor `.so` restored byte-exact (SIG_OK); **no commit**; scoped `wineserver -k` only; disk 50 GB.
- Diagnostic infra retained UNCOMMITTED in source (all env-gated, off by default): `force_interp` guest-range
  (`MACRUNNER_HB_FORCE_INTERP_LO/HI`, hb_runtime.c), IR dump + interp-fail/container/array probes (hb_arm64_codegen.c),
  grow-fail / commit-on-fault / commit-time (macrunner_hb.c), fault-recovery (hb_runtime.c).
- ★ DIRECT_MEM is confirmed UNSAFE for HK (it makes the runaway fatal); the bug is NOT DIRECT_MEM's fault but
  DIRECT_MEM turns the latent runaway into a crash. Keep DIRECT_MEM default-OFF until the count bug is fixed.
