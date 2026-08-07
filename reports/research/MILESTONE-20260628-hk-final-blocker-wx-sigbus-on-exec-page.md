# MILESTONE 2026-06-28 — HK final blocker PINNED: W^X SIGBUS on an executable page (0x400000000), NOT a commit gap. Corrects the "legit reserved-tail / under-commit" framing.

After ~20 build/run cycles the fault path is fully traced. The producer's hot block (`0x87efdef9430`) is a
load+store SSE copy executed via the direct-mem JIT; on its over-write fault the **macrunner_hb signal handler calls
`macrunner_hb_try_grow_guard_page` directly** (commit + retry) — which is why every higher-level probe missed
(exec_ir_block_once interp-fail=0, hb_memory_write memcpy-w=0, special_write special-vm=0, vector_store_loop
vstore-fault=0; only `try_grow_guard_page` fires, 265×).

## Decisive probe (try_grow_guard_page FALSE-return page state)
```
reserved-commit: 265   (PROT_NONE data pages at 0x300000000–0x3e0000000, committed by the fix)
grow-fail: addr=0x400000000 kr=0 region=[0x400000000,0x408000000) prot=0x7 max_prot=0x7 covers=1
```
The fatal page at **`0x400000000` is already `prot=0x7` = R|W|**X** (executable, 128 MB region).** The direct-mem JIT
`str` to it **SIGBUSes due to Apple-Silicon W^X** (a `MAP_JIT`/executable page cannot be written directly without the
`pthread_jit_write_protect` toggle). `try_grow_guard_page` cannot help — the page is already writable; the fault is
**W^X, not a missing commit** → returns FALSE → c000007b.

## Corrected picture (supersedes the "legit reserved-tail, not runaway" verdict)
- The **265 reserved-commits (0x300–0x3e0)** ARE real reserved (PROT_NONE) **data** pages — the commit-coherence /
  under-commit story applies *there*, and commit-on-fault handles them correctly (advances producer 260× to rung 9).
- The **FATAL crash is separate**: the over-write reaches a **9th region at 0x400000000 that is EXECUTABLE (RWX)** —
  almost certainly the **Mono JIT code heap** (Mono reserves large RWX regions). A direct-mem write there is a W^X
  violation on Apple Silicon, where Windows (no W^X) would allow it. So the "stays within 8 reserved data regions"
  framing was incomplete: the producer's write reaches an exec region beyond them.

## The open fork (must be resolved before a fix)
Is the write to 0x400000000 **legitimate** (the region is RWX *data* that Windows writes directly) or an
**over-extension into executed Mono code** (a length/loop-bound over-run → corruption)?
- If LEGIT → fix = **W^X-safe direct-mem write to exec pages**: route the faulting store through the SMC/special path
  (toggle `pthread_jit_write_protect_np`, or mprotect→write→restore), the standard Apple-Silicon JIT pattern.
- If OVER-EXTENSION → forcing the W^X write would **corrupt Mono code** and crash later anyway; the real fix is the
  producer's copy length/bound (the original DIRECT_MEM-corruption / over-extend hypothesis).
DECIDER: check whether 0x400000000 is ever EXECUTED (it's Mono code) vs only written (RWX data) — e.g. an exec-region
registry lookup or a single winedbg/CrossOver check of what maps there. The RWX (exec) attribute strongly suggests
Mono code, which leans toward OVER-EXTENSION — i.e. the producer should NOT be writing there, and commit-on-fault was
masking a runaway that swept the data regions and finally hit code.

## Status — NO PIXEL this session (honest)
~20 cycles; the path bottoms out at Apple-Silicon W^X + an unresolved legitimacy question (data-in-RWX vs executed
Mono code), which is exactly the deep JIT/W^X rabbit hole the operator's standing guidance says to avoid blindly.
This needs the JIT/W^X owner (Codex) and/or a determination of the 0x400000000 region's nature — not more blind
MacRunner-side probing.

## Fixes left in source (all UNCOMMITTED; floor `.so` restored byte-exact, SIG_OK)
- fault-recovery (force_interp demote + interp re-run) — hb_runtime.c
- commit-on-fault (try_grow_guard_page reserved-page mprotect) — macrunner_hb.c — SAFE + advances 260×, KEEP
- commit-time mprotect (sync_virtual_region) — fires 0× for HK, sound general hardening
- diagnostic probes (interp-fail, vstore-fault, grow-fail, region/host probes) — env-gated, off by default

**Rails:** floor `.so` restored byte-exact (SIG_OK); no commit (no pixel); scoped `wineserver -k`; disk 50 GB.
