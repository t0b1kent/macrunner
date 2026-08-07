# MILESTONE 2026-06-28 — HK producer bottleneck = JIT direct-mem is default-OFF; enabling it faults (the real, scoped fix)

**Method:** terminal-empirical (profile → A/B → code-trace), READ-ONLY to source (no edits, no rebuild — deployed
`dist-arm64ec-spike/.../aarch64-unix/ntdll.so` unchanged @09:14, floor intact). Every link verified by a live run.

## 1. FLOOR is already clean — there is nothing to revert (premise corrected)

- Commit `0e258e03` does not exist in this repo/worktrees. The deployed build (built today 09:14 from the working-tree
  source, which HAS `emit_native_punpck_qdq` wired) runs HK to **rung 9 (dxgi-factory), PRESENT_MISSING, no fault/BUS,
  rc=143 clean 90 s timeout, DXMT FL 11_1.** So the "PUNPCKQDQ regression" is NOT live; reverting it would delete a
  working narrow lever. Step 1 of the task is moot.
- (The current `emit_native_punpck_qdq` only handles the reg-reg 8-byte-lane case — `lane!=8 → return false` — which is
  why it no longer BUS-errors: it sidesteps the memory-operand 128-bit load that was the original regression.)

## 2. PROFILE (native `sample`, the real wine producer, 40 s of the busy window) — bottleneck = interpreter memory path

Heaviest symbols (samples), all in `ntdll.so`:
| samples | symbol | role |
|---|---|---|
| 1209 | `hb_jit_helper_exec_two_block_loop` | interpreter fast-path for the 2-block vector loop |
| 1193+176+88 | `hb_memory_read` | per-op guest read |
| 1177+70 | `hb_jit_helper_exec_ir_block_once` | **interpreter executing IR blocks (the fallback)** |
| 1169 | `macrunner_hb_special_read` | mach/region read per access |
| 657 / 420 | `hb_jit_helper_exec_load/store_operand_lazy` | lazy interpreter operand access |
| ~961 | `hb_flags_read_operand_value` (×6) | interpreter flag eval |
| 341+334 | `hb_memory_write` + `macrunner_hb_special_write` | per-op guest store |

→ The producer's cost is **per-operand memory access through the interpreter** (`exec_*_lazy`/`exec_ir_block_once` →
`hb_memory_read/write` → `special_read/write`, ~3400 samples), because the hot vector blocks run in the **interpreter
fallback**, not native code (native codegen would use direct loads/stores + registers, eliminating the `special_*` calls).

## 3. ROOT LEVER — `MACRUNNER_HB_JIT_DIRECT_MEM` (default OFF) gates ALL direct-mem load/store codegen

`hb_arm64_codegen.c:444` `jit_direct_mem_enabled()` → env `MACRUNNER_HB_JIT_DIRECT_MEM`, **default 0**.
`jit_direct_mem_codegen_enabled(buf)` = arch && that flag. Every `HB_IR_LOAD/STORE` native path (incl. the XMM 128-bit
path at `:4225`/`:4250`) is gated on it; when OFF they fall to `hb_jit_helper_exec_load/store_operand_lazy` = the hot path.
So the producer is slow because **JIT direct-mem codegen is disabled by default** — a far bigger lever than punpck (it
governs *all* memory accesses in JIT'd code, not just vector ops). It is default-OFF for a correctness reason (below).

## 4. A/B (zero code change) — enabling it CONFIRMS the lever AND reproduces the exact fault

Run with `MACRUNNER_HB_JIT_DIRECT_MEM=1`: HK faults **`c000007b reason=runtime` at pc `0x87efdef9430`**, faulting block =
an SSE vector memcpy:
```
f3 0f 6f 0a   movdqu xmm1,[rdx]    f3 0f 6f 52 10  movdqu xmm2,[rdx+0x10]
f3 0f 6f 5a 20 movdqu xmm3,[rdx+0x20] f3 0f 6f 62 30 movdqu xmm4,[rdx+0x30]
66 0f 7f 09   movdqa [rcx],xmm1 ...     (rdx=0x300ffff7c, rcx=0x300ffffa0)
```
i.e. OFF → this block runs the safe slow lazy helper; ON → the direct 128-bit load/store faults. **This is the lever and
the bug, both confirmed on one run.**

## 5. ROOT CAUSE of the fault — fault recovery is named but NOT wired

- The direct 128-bit path is two plain `ldr x` (`emit_direct_mem128_load_to_x20_x22`, `:1203`); `emit_x86_ea_to_host`
  (`:389`) is a **no-op for x64** (identity) — so no address/alignment bug. The raw `ldr` reads guest VA `0x300ffff7c`
  directly, which faults on a host page the lazy helper would handle (commit/mach), but the direct load cannot.
- The signal guard `run_jit_block_with_signal_guard` (`hb_runtime.c:1293`) DOES catch the SIGSEGV/SIGBUS
  (`sigsetjmp`/`siglongjmp`), restore the context snapshot, and call
  `set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE, … "JIT native signal fault; interpreter fallback")`.
- **BUT** `set_jit_interp_fallback_result` only sets `out->faulted = true` (`:1250`); the caller
  (`hb_jit_runtime_run:2444`) does `if (run_result != HB_OK || out->faulted) return run_result;` — it **returns**, it does
  **not** re-execute the faulted block via the pure interpreter. So the faulted result propagates to `macrunner_hb_run_x64`
  → **`c000007b reason=runtime`**. The "interpreter fallback" is named but never actually re-runs the block safely.

## 6. THE FIX (precise, scoped) + plan

Wire the signal-fault fallback to actually recover, then turn the lever on:
1. **`hb_runtime.c`:** on a `run_jit_block_with_signal_guard` fault, RE-EXECUTE that one block through the pure IR
   interpreter (the path that uses `hb_memory_read/write` with proper PROT_NONE/commit/fault handling), instead of
   returning `out->faulted`. **Mark the block force-interpret** (don't re-JIT it) so it can't re-JIT→re-fault in a loop.
   (The interpreter executors already exist — `hb_jit_helper_exec_ir_block_once` etc.; the gap is dispatching to them on a
   guard fault rather than aborting.)
2. **Then** enable `MACRUNNER_HB_JIT_DIRECT_MEM` (default-on, or for HK): fast direct loads/stores in the common case,
   safe per-block interpreter recovery on the rare faulting access → the ~3400-sample `special_read/write` cost collapses
   → producer first-publish should drop from ~30 s toward CrossOver's ~6 s → Present fires.
3. Verify: re-profile (the `hb_memory_read`/`special_read`/`exec_*_lazy` symbols should fall away), confirm no `c000007b`,
   and check truePresent>0 / non-black capture.

**Caveats for whoever implements:** (a) the fix is correctness-critical to the *whole* JIT runtime (all guest exec), so
it needs no-regression runs beyond HK; (b) the working-tree source has **875 uncommitted codegen lines + 123 in
hb_runtime.c that are NEWER than the deployed 09:14 floor** — do NOT blind-rebuild; reconcile/rebuild deliberately and
re-confirm the floor first; (c) punpck NEON lowering is a *minor* sub-case — the real win is direct-mem-for-all + safe
fault recovery, not vector-specific codegen.

**Stale-doc note:** `docs/ACTIVE-INVESTIGATION.md` @17:38 still says "coherence" — superseded twice now
(RCA-CORRECTION-20260627 split-mapping REFUTED; this report = producer-throughput via direct-mem). Recommend updating it.

**Rails:** no source edits, no rebuild, deployed floor `.so` unchanged (09:14); scoped `wineserver -k` only; disk cleaned;
Codex lane source untouched by me.
