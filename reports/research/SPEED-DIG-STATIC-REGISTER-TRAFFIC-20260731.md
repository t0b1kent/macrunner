# Static measurement: what the emitted code actually spends itself on

**2026-07-31 — zero HK runs.** The brief's first step was to bound the register-allocation win before writing
anything. The bound is now measured, on **1 096 350 real translated blocks / 37 825 816 emitted ARM64
instructions** taken from a persisted translation cache of a real HK run
(`artifacts/hb-translation-cache/ntdll-5385710288b50361/translation-cache.bin`, 221 MB).

## Method

The cache is `HBTC` v2: a 16-byte header, then per entry `meta(16) + key(48) + native_code(native_size)` —
the emitted ARM64 verbatim. Guest-register traffic is identifiable exactly, because every guest register
access is emitted as a load/store based on **x19** (the `hb_context_t` pointer) at a `reg_off()` displacement:
`emit_ldr_gpr(buf, <scratch>, 19, reg_off(buf, <guest reg>))`. Offsets came from the compiler, not from
reading the struct (`regs.x64.rax=32 … r15=152, rip=160, rflags=168, xmm=176`), so GPR traffic is exactly
`LDR/STR` with `Rn=19` and displacement in `[32,152]`.

Counted forms: `LDR/STR (unsigned offset)` 64-bit (`0xF9400000`/`0xF9000000`) and 32-bit
(`0xB9400000`/`0xB9000000`).

> Trap worth recording: the first version of the counter reported **0 loads/stores in 37.8 M instructions**.
> JavaScript bitwise operators yield a *signed* int32, so `w & 0xFFC00000` was negative and never compared
> equal to the positive constant. The debug dump looked correct only because it applied `>>>0`. A zero from a
> counter is a claim about the counter first.

## Result

```
emitted = 11.8 + 7.2 × guest_instructions          (weighted LS over steps 1..8, 1.08 M blocks)
```

| quantity | value |
|---|---|
| mean guest instructions per block | **3.14** |
| mean emitted ARM64 per block | 34.5 |
| emitted ARM64 per guest instruction | 11.0 |
| **fixed per-block overhead** | **11.8 instrs = 34 % of all emitted code** |
| marginal cost per guest instruction | 7.2 instrs |
| **guest-GPR loads/stores** | **3 397 744 = 9.0 % of all emitted**, 0.99 per guest instruction |
| XMM context traffic | 2 652 302 = 7.0 % of emitted |
| `rip` / `rflags` context traffic | **0** (lazy flags; RIP not spilled per access) |

Block size distribution: 1 step 9.0 %, 2 steps 35.8 %, 3 steps 22.4 %, 4 steps 16.8 %, ≥5 steps 15.9 %.
**44.8 % of blocks hold ≤2 guest instructions and account for 32.8 % of all emitted code.**

## What this changes

**1. The brief's premise does not match the emitted code.** The stated model was "`add rax, rbx` = four
memory accesses per arithmetic operation". Measured, the average is **0.99 guest-GPR memory accesses per
guest instruction**, and all guest-GPR traffic together is **9.0 %** of emitted instructions. Register
allocation is real, but its ceiling is 9 %, not a large multiple.

**2. Fixed per-block overhead is 3.8× larger than the entire register-allocation ceiling** — 34 % of every
byte of emitted code is the per-block prologue/epilogue, paid once per dispatch on blocks that average 3.14
guest instructions. That is 3.8 ARM64 instructions of pure overhead *per guest instruction*.

**3. Register allocation is gated on block size, not the reverse.** A static 16→16 mapping must flush live
registers at every unchained block boundary. With blocks of 3.14 guest instructions and ~1.0 GPR access per
instruction, the mandatory load-on-entry / store-on-exit traffic is comparable to the traffic being removed —
the allocator would spend most of its budget on flushes. Chaining and larger blocks are what create the room
for it.

So the ordering in the brief (1. register allocation, 2. chaining) is inverted by the data. The evidence says:
**enlarge/chain blocks first — it attacks 34 % and it is the precondition for the 9 %.**

## Next

Decompose the 11.8-instruction fixed prologue/epilogue: how much is the signal-guard frame, how much is the
snapshot, how much is dispatch bookkeeping. The same cache answers it — the prologue is a near-identical
byte sequence at the head of every block, so the common prefix/suffix can be extracted and priced without a
run. That number decides whether the win is chaining (skip the round-trip) or shrinking the prologue itself.

---

# Part II — what the 34 % fixed overhead actually is (same cache, still no run)

Extracted the modal leading/trailing word sequences across 300 000 blocks.

**Prologue — 4 instructions, identical in 100 % of blocks:**
```
STP x19,x20,[sp,#-48]!
STP x21,x22,[sp,#16]
STP x23,x30,[sp,#32]
MOV x19,x0                  ; ctx pointer
```
**Epilogue — 5 instructions, 63.9 % of blocks:**
```
STR x21,[x19,#544]          ; ctx->pc  (offset 544 = pc, confirmed against hb_context.h)
LDP x23,x30,[sp,#32]
LDP x21,x22,[sp,#16]
LDP x19,x20,[sp],#48
RET
```

Nine instructions, of which **eight are a callee-saved register frame**: six registers saved and restored =
**12 memory accesses on every single dispatch**, for a block that averages 3.14 guest instructions. All
guest-register traffic together is 3.1 accesses per block. **The frame costs about 4× what register
allocation could ever save.**

## The frame is only needed for blocks that call

x19–x23/x30 are preserved because the block may `BL` into a helper. A block that calls nothing has nothing to
preserve. Counted over the corpus:

| | |
|---|---|
| blocks with **zero** calls | **529 347 = 48.3 %** of blocks |
| — their share of emitted code | 39.9 % |
| — their share of guest instructions | 38.3 % |
| calls per block | only ever 0 or 1; mean 0.52 |
| frame accesses removable on call-free blocks | **6 352 164** |
| entire register-allocation ceiling, for comparison | 3 397 744 |
| **ratio** | **1.87×** |
| instructions removed | 4 234 776 = **11.2 % of all emitted code** |

**Eliding the frame on call-free blocks is worth 1.87× the whole register-allocation ceiling, and it is a
strictly simpler change.**

## Why it is feasible

The frame exists to honour AAPCS: x19–x23 are callee-saved, so a block clobbering them must preserve them for
its C caller, and x30 must be preserved only because `BL` overwrites it. A call-free block can instead take
its scratch from the **caller-saved** bank: it needs 5 registers (ctx + 4 scratch) and AAPCS gives 7 free
ones in x9–x15. No save, no restore, no `x30` spill, and the epilogue collapses to
`STR x21,[x19,#544]; RET`.

Implementation shape: decide before emission — scan the IR block for any op that lowers to a helper call; if
none, select the caller-saved scratch mapping and skip the frame. The scratch register numbers are currently
hard-coded (`emit_ldr_gpr(buf, 21, 19, …)`), so this needs them parameterised per block, which is mechanical
but touches every emitter. Gate it, and verify against the same cache: re-dump after the change and confirm
call-free blocks start with `MOV`, not `STP`.

## Revised ordering for the lane

1. **Elide the frame on call-free blocks** — 11.2 % of emitted code, 6.35 M memory accesses, 48.3 % of blocks, no
   correctness subtlety beyond register selection.
2. **Chaining / larger blocks** — attacks the remaining fixed overhead and is the precondition for (3).
3. **Register allocation** — ceiling 9.0 %, and with 3.14-instruction blocks most of it is eaten by mandatory
   boundary flushes until (2) lands.

---

# Part III — preconditions verified, implementation designed (still no run)

Before writing a wide change, checked what a call-free block actually requires. Over all **529 347**
call-free blocks, with the frame words excluded:

| precondition | result |
|---|---|
| blocks using the stack for anything but the frame | **0** (0.000 %) |
| blocks writing x24–x28 | **0** (0.000 %) |
| blocks whose last word is not `RET` | **0** |
| blocks writing x30 | **0** — LR is spilled *only* because `BL` clobbers it |

Register usage inside call-free blocks: x19 100 %, x21 100 %, x20 86.4 %, x22 56.6 %, x23 32.0 %. So the body
needs at most five registers and no stack at all. AAPCS gives seven caller-saved (x9–x15). **A call-free block
can run with no frame whatsoever.**

> A first version of this check reported "100 % of blocks touch SP outside the frame". That was the window,
> not a finding: the epilogue is five words and only three were excluded, so `LDP x23,x30,[sp,#32]` at n−4 was
> counted every time — exactly one hit per block, which is what gave it away.

## Implementation design

596 call sites hard-code the scratch registers (`emit_ldr_gpr(buf, 21, 19, …)`), so the remap must not live at
call sites. It belongs in the **42 encoder primitives**, which are the only places register numbers become
instruction fields: add `uint8_t rmap[32]` to `hb_codegen_buffer_t` (identity by default) and apply it on entry
to each primitive. Every call site is then untouched.

Two details that make or break it:

1. **Relocations must be recorded after the remap.** `hb_arm64_codegen.c:259` stores
   `relocs[...].reg = (uint8_t)rd` inside a primitive; if the remap is applied at primitive entry the recorded
   register is already the remapped one and patching stays consistent. Ordering is the whole fix — no separate
   reloc pass. (Classification itself is already by KIND, not by register number, after the earlier x23
   incident where `mov x23, 0xffffffff` was misread as a helper address and cost 24.6 % of cached blocks.)
2. **Call-free must be known before emission.** Simplest correct approach is two-pass: emit with the lean
   mapping, and if a call was emitted, reset and re-emit with the identity mapping plus the frame. Codegen is
   2.2 µs/block (0.69 s per run total, already measured and refuted as a cost), so re-emitting ~52 % of blocks
   costs ~0.35 s — acceptable, and replaceable later with an IR pre-scan.

Gate `MACRUNNER_HB_LEAN_FRAME`, default off. Verification needs no HK run: re-dump the translation cache and
assert that call-free blocks begin with `MOV`, not `STP`, and still end in `RET`.

---

# Part IV — the change was built, and it delivers the predicted size reduction

Implemented behind `MACRUNNER_HB_LEAN_FRAME` and measured **within-run**, the Rule-1 way: dump a translation
cache from each arm on the SAME build, intersect on identical 48-byte cache keys (same guest block, same
conditions), and compare emitted instructions.

| | baseline | lean |
|---|---|---|
| same guest blocks in both caches | 53 414 | 53 414 |
| mean emitted instructions per block | 36.03 | **32.06** |
| total instructions over those blocks | 1 924 765 | 1 712 491 |
| **reduction** | — | **11.03 %** |
| blocks smaller / identical / larger | — | 66.2 % / 33.8 % / **0** |

**Predicted 11.2 %, measured 11.03 %.** No block grew, and mean guest instructions per block is 3.10 against
the 3.14 measured on the 1.1 M-block corpus — the two independent samples agree.

Lean-arm composition: 48 055 frameless vs 25 065 framed blocks, i.e. ~66 % of translated blocks carry no
frame, consistent with the 66.2 % that shrank.

**Wall-clock remains unproven.** UnloadTime at n=2 per arm: baseline 152.2 / 218.3 s, lean 150.6 / 229.0 s —
the within-arm spread (66–78 s) is far larger than any 11 % effect, exactly the ±78 s the brief warns about.
Resolving it needs ~8 runs per arm. The gate therefore stays OFF: the mechanism is verified correct and its
size effect is measured, but no speed claim is made.

(Counting note: the per-arm block totals above are deduplicated by cache key, while the framed/frameless
tallies count raw entries, so the latter can exceed the former when a key is re-emitted. The intersection
analysis uses the deduplicated maps.)
