# Present-stall diagnosis — "UnityPlayer stops post-swapchain, before Present" (HK)

**Date:** 2026-06-27  **Mode:** BURST-BRAIN, read-only diagnosis (no edits/build/deploy/run).
**Binary disassembled:** `game-hollow.knight-(89718)/extracted-hollow-knight-1.5.12620/UnityPlayer.dll`
(Unity 2020.3, PE32+ x86-64, ImageBase 0x180000000). Every live-run RVA below lands exactly on an
instruction boundary in this image → addresses verified against the **actual HK build**.

> ⚠️ Note for anyone re-running the disasm: `artifacts/ai-war2-unity-corpus/extracted/UnityPlayer.dll`
> is a **different game** (AI War 2) — its RVAs do NOT match the HK markers (they land mid-instruction).
> Use the HK path above. (This cost the first disasm pass; flagged so it isn't repeated.)

---

## TL;DR — the operator's frame-latency-waitable hypothesis is REFUTED. The stall is NOT graphics.

The window is black because **the render/producer thread never reaches its first `Present`** — it is
stuck *upstream* of any DXGI/Metal call, inside a **Unity allocator + growable-container path**
(`vfunc58` → `+0x0f6c30` → `0xf6550` → allocator `0x2b3cf0`). This is a **HyperBridge
direct-memory / live-VM-write coherence defect**, the same class as the already-recorded TLS-allocator
bad-object finding — **not** a DXMT/Metal/waitable problem. Three independent lines of evidence below.

---

## (a) What is `UnityPlayer+0x0f6c30`? — disassembly verdict

**It is a `dynamic_array` grow/reserve thunk — an ALLOCATOR path. Not a wait, not DXGI, not Present.**

```
; +0x0f6c30 : compute new capacity, tail-jump to the real grow
0x0f6c30: mov   rdx, [rcx+0x18]          ; capacity (low bit = in-place flag)
0x0f6c39: and   rdx, ~1
0x0f6c40: cmovb rdx, 1                   ; max(cap,1)
0x0f6c44: jmp   0xf6550                  ; --> real grow/realloc

; 0xf6550 : double capacity, call Unity allocator realloc
0x0f6571: lea   rax, [rdx+rdx]           ; new_cap = old*2
0x0f657d: mov   [rcx+0x18], rax          ; store new capacity
0x0f6581: lea   r8,  [rdx*4]             ; byte size = count*4  (int32 elements)
0x0f6591: mov   rdx, [rbx]               ; old buffer ptr
0x0f65a0: mov   rcx, [rip+0x1e15099]     ; --> global allocator object (rva 0x1f0b640)
0x0f65b0: call  0x2b3cf0                 ; --> Unity allocator realloc dispatch
0x0f65b5: mov   [rbx], rax               ; store new buffer ptr

; 0x2b3cf0 : allocator realloc/alloc dispatch (tag+size+align)
0x2b3d09: test  rdx, rdx                 ; rdx == old ptr
0x2b3d0c: jne   0x2b3d53                 ; realloc path (ptr != 0)
0x2b3d40: call  0x2b3a60                 ; alloc path (ptr == 0)
```

So the frontier call `0x58571e → 0xf6c30 → 0xf6550 → 0x2b3cf0` is **purely "grow this int32 array:
double capacity, realloc, copy"** against Unity's MemoryManager (global allocator at rva `0x1f0b640`).
This is the same allocator subsystem whose per-thread fast path produced the earlier
`*0x181a01800 = KERNEL32!TlsGetValue` bad-object (see `LANE-A-PROGRESS.md` 01:52 / memory note
`project_hk_groundtruth_72a4f84`).

### What `vfunc58` (rva 0x584d50) actually does
A method on `this=r15` (vtable; calls own slot `+0x40` at entry, gated on `[r15+0x3f8]==6`). The stall
is in its inner loop (`0x5856d0..0x585748`):

```
0x5856d0: rax = [r15+0x3d8]             ; base of array-of-structs (stride 0x18, count [r15+0x3e8])
0x5856f4: rdi = [r14+rax]               ; element -> an object pointer
0x5856fb: call 0x73c390                 ; weak-ref LOCK: bool "is object still alive?"  (marker 'after-73c390')
0x585700: test al,al                    ; <-- deepest confirmed live marker
0x585704: r13d = [rdi+8]                ; the object's id/value
0x585708: rdi = [rbx+0x10]              ; result_array.size
0x58570c: rax = [rbx+0x18]              ; result_array.capacity (flag in low bit)
0x585716: cmp rdi, rax(>>1)
0x585719: jbe no_grow
0x58571e: call 0xf6c30                  ; <-- FRONTIER: grow result array  (allocator)
0x585723: store r13d into array[size]; size++
0x585741: cmp r12, [r15+0x3e8]          ; loop while r12 < count
0x585748: jne 0x5856d0
```

`vfunc58` = **"walk a collection, weak-ref-lock each entry, and append the live ones' instance-IDs into
a growable array."** The **only** non-trivial call in the loop body is the allocator grow. `0x73c390` is
a bounded atomic refcount/weak-ref acquire (no wait — it returns). So the loop can only "not return
before Present" two ways, both memory-coherence:

- **the realloc `0x2b3cf0` does not return** (allocator free-list / heap metadata corrupted by an elided
  or stale copy), or
- **the loop bound `[r15+0x3e8]` (or the iterator/array metadata) is incoherent/corrupt** → unbounded
  loop that keeps hammering the grow path (which is exactly why the chase keeps landing on "the grow
  call" — it is re-entered every iteration).

Neither is a wait, a fence, a frame-latency object, or any DXGI/Metal call.

---

## Why the frame-latency-waitable hypothesis is wrong (3 independent refutations)

1. **Flag-value error.** In `engine/dxmt/include/native/directx/dxgi.h:253,258`:
   `FRAME_LATENCY_WAITABLE_OBJECT = 0x40`, **`ALLOW_TEARING = 0x800`**. If triage saw `0x800` and read
   it as "waitable," that is ALLOW_TEARING — DXMT then creates **no** waitable and
   `GetFrameLatencyWaitableObject` returns `nullptr` (`d3d11_swapchain.cpp:996`). Unity isn't blocking on
   a DXMT waitable.
2. **DXMT's waitable IS signaled; Present IS real.** Even with `0x40` set, DXMT creates
   `present_semaphore_` pre-signaled (`CreateSemaphore` initial count = frame_latency,
   `d3d11_swapchain.cpp:150`) and re-signals it every Present (`ReleaseSemaphore` `:798/:808`). `Present1`
   drives a real `layer_.nextDrawable()` + blit + `presentDrawable` (`dxmt_presenter.cpp:154`,
   `dxmt_context.cpp:1051-1053`). The only weakness is the pre-signal count is hard-coded to 1 — a narrow
   secondary risk, not a first-frame deadlock. (Full DXMT audit on file; the present path is complete.)
3. **The Metal layer is wired and non-blocking.** A real `CAMetalLayer` (`WineMetalLayer`) is created with
   `MTLCreateSystemDefaultDevice()` and added as a subview of the HWND's content view on the main thread
   (`winemac.drv/cocoa_window.m:707-720,982,1000-1013,4047-4070`; bind sequence
   `winemac.drv/d3dmetal.c:107-111`). `nextDrawable` only posts an async present event
   (`d3dmetal_objc.m:43-71`); nothing waits on vsync/CVDisplayLink. It cannot block the guest.

> **And the decisive one:** the live stall PC is in `vfunc58`/`+0x0f6c30`, an allocator+container grow
> that runs **before the thread ever issues a Present/DXGI/waitable call**. You cannot be blocked on a
> frame-latency waitable you never reached.

### Live markers corroborating (from `reports/research/LANE-A-PROGRESS.md`, runs `laneA-…-20260627-*`)
- `CreateSwapChainForHwnd … rc=0x0 hwnd=0x3003e` (S_OK) — **create=1, Present=0, fault_class=0** at every
  heartbeat (04:09 … 08:10); CG window exists and is **black** (03:11).
- `0x584105` = `WaitOnAddress` import then a `lock cmpxchg` **semaphore-acquire counter loop** + QPC
  timeout (my disasm 0x5840ff-0x584121) — matches "WaitOnAddress then atomic counter loop, 1ms timeouts."
- `0x586d65` = `call [vtable+0x58]` (producer executes a virtual "job"); the wait helper at `0x586daa`
  /`0x5878ab` calls `0x5841f0`/`0x584040` = the **semaphore RELEASE/signal** on object+0x1f8
  (`lock add` + `WakeByAddressSingle` loop, my disasm 0x584236-0x584251). Wake primitive works; the
  counter just never advances because the producer is stuck in `vfunc58`.
- ~16 Unity worker threads sit in **INFINITE** `WaitOnAddress` (normal idle — no frame work is produced
  because the producer is stuck).
- Earlier same-day narrowing: `MACRUNNER_HB_LIVE_VM_WRITE_MACH=1` **removed an allocator fault** (01:44),
  isolating it to the **hb_memory live-VM-write memcpy / direct-mem coherence path** — i.e. the allocator,
  not graphics. `vfunc58` is the next layer of that same path.

---

## (b) Ranked root hypotheses

| # | Hypothesis | Likelihood | Notes |
|---|-----------|-----------|-------|
| **H1** | **HyperBridge direct-mem / live-VM-write coherence** corrupts Unity allocator/container metadata → realloc `0x2b3cf0` hangs **or** corrupt `[r15+0x3e8]` count drives an unbounded `vfunc58` loop | **HIGH (primary)** | Matches: frontier IS the allocator grow; `MACRUNNER_HB_LIVE_VM_WRITE_MACH=1` already cleared a sibling allocator fault; recent commits 3bcace4 / 0719cb5 made "direct memcpy with cache-hit gating" default-on |
| H2 | A guest pointer/object copied incoherently (the TLS-allocator `*0x181a01800` non-descriptor bad-object class) makes `0x73c390`'s weak-ref or the element pointer garbage → loop logic diverges | MEDIUM | Same root family as H1 (direct-mem copy elision); distinguishable only by which datum is stale |
| H3 | DXMT frame-latency-waitable never signaled | **REFUTED** | wrong flag (0x800=tearing); waitable is signaled; **stall is pre-Present** |
| H4 | Metal drawable/CAMetalLayer not wired / blocks on vsync | **REFUTED** | layer wired, present path async/non-blocking; never reached |
| H5 | Degenerate swapchain (S_OK with no backbuffer) | LOW | real `CAMetalLayer` always backs it; `ResizeBuffers` HRESULT is unchecked (`d3d11_swapchain.cpp:203` `// FIXME`) — a latent correctness gap worth fixing, but not this stall (we never reach GetBuffer/Present) |

## (c) Strongest hypothesis + evidence — **H1 (direct-mem coherence in the allocator/container path)**

Evidence stack, strongest first:
1. **The frontier is literally the allocator grow** — disasm proves `0x58571e → 0xf6c30 → 0xf6550 →
   0x2b3cf0` is doubling+realloc against Unity's MemoryManager. The deepest marker `0x585700` is one
   instruction before the loop's append/grow.
2. **A direct-mem env flag already moved the needle** — `MACRUNNER_HB_LIVE_VM_WRITE_MACH=1` cleared an
   allocator fault in this exact subsystem (LANE-A 01:44/01:52). That is a direct causal handle on the
   memory path, none on the graphics path.
3. **Recent regression surface** — `3bcace4` "default-on direct memcpy with cache-hit gating & signal
   fallback" and `0719cb5` "direct-mem cache-hit gated copy" turned copy-elision ON by default. Eliding /
   deferring a copy that the allocator's free-list or the container's `size/capacity/count` words depend
   on yields exactly this signature: a hang in realloc or an unbounded grow loop.
4. **Graphics is exonerated three ways** (above) and the thread provably never reaches Present.

Open sub-question (cheap to resolve, see verify plan): is it **(i)** the realloc `0x2b3cf0` not
returning, or **(ii)** a corrupt loop bound `[r15+0x3e8]` causing unbounded iteration? Both are H1; the
fix is the same coherence fix, but the instrumentation tells Codex which datum is stale.

---

## (d) Fix-shape — for Codex/Lane-A hands (HyperBridge memory, NOT DXMT)

**Do not touch DXMT/Metal — they are correct.** The fix is in `engine/hyperbridge/src/hb_memory.c` /
`engine/wine/dlls/ntdll/unix/macrunner_hb.c` / `virtual.c` (the direct-mem / live-VM-write path).

1. **Pin the failure mode first (1 instrumented run, no code change needed):**
   - Log on entry to `vfunc58` (VA = ImageBase + 0x584d50): the loop bound `[r15+0x3e8]` and base
     `[r15+0x3d8]`; and a per-iteration counter.
   - Log enter/exit of `0xf6550` and `0x2b3cf0` (VA = base + rva).
   - **If iteration count explodes** (≫ plausible object count) → corrupt `[r15+0x3e8]`/iterator =
     stale-read coherence (sub-case ii).
   - **If `0x2b3cf0` enters and never returns** → free-list/heap-metadata corruption or a lock token
     written via the elided copy (sub-case i).
2. **A/B the direct-mem optimization (the prime regression):** re-run HK floor with the direct
   memcpy / cache-hit-gated copy **disabled** (safe full-copy path) and separately with
   `MACRUNNER_HB_LIVE_VM_WRITE_MACH=1`. If the stall disappears in either → commits `3bcace4` / `0719cb5`
   are the regression; that localizes the bug to the cache-hit gating predicate.
3. **The real fix (coherence invariant):** the cache-hit gating that elides a memcpy must **not** elide /
   must invalidate when the destination region is later read through a *different* mapping (the
   live-VM-write Mach path does this; the default direct path apparently doesn't for allocator-owned
   pages). Either make the Mach-coherent behavior the default for guest heap/allocator regions, or widen
   the gate so writes to allocator/container metadata pages are always materialized before a guest read.
   The cache-hit predicate is the thing to harden — an elided copy that leaves a stale `size`/`capacity`/
   free-list word is the mechanism.
4. **Secondary hardening (not this bug, but log while here):** check the unchecked `ResizeBuffers`
   HRESULT (`d3d11_swapchain.cpp:203`); and raise DXMT's hard-coded frame-latency pre-signal count
   (`d3d11_swapchain.cpp:33/150`) toward `BufferCount` — both are latent, neither is the stall.

## (e) Verify plan

1. **Primary signal:** after the fix, the HK floor run advances **create=1 → Present ≥ 1** and the window
   shows non-black pixels. The producer's `vtable+0x58` (`0x586d65`) returns, `vfunc58` exits past
   `0x585748`, the object+0x1f8 semaphore is signaled, and the ~16 worker threads stop sitting in INFINITE
   `WaitOnAddress` (they get frame work).
2. **Intermediate (before pixels):** the deepest live block-watch marker moves **out of `vfunc58`**
   (past 0x585748) — i.e. the allocator grow loop completes. If using the instrumentation from (d.1),
   `0x2b3cf0` enter/exit counts must balance and the `vfunc58` iteration count must be bounded/plausible.
3. **A/B confirmation:** stall presence must correlate with the direct-mem optimization toggle (d.2). If
   disabling copy-elision (or Mach=1) removes the stall while leaving it on reproduces it, root cause is
   confirmed as the cache-hit-gated copy coherence gap — independent of any graphics change.
4. **Regression guard:** keep the A/B toggle in the run matrix until DXMT is actually exercised; only then
   does the (refuted) graphics path get re-examined.

---

### Appendix — provenance
- Disasm: capstone 5.0.7 over the HK `UnityPlayer.dll` (script in session scratchpad); IAT-resolved import
  annotations; all RVAs verified on instruction boundaries.
- DXMT audit: `engine/dxmt/src/d3d11/d3d11_swapchain.cpp`, `dxgi_factory.cpp`, `dxmt_presenter.cpp`,
  `dxmt_context.cpp`, `include/native/directx/dxgi.h` (read-only).
- macdrv/Metal: `engine/wine/dlls/winemac.drv/{cocoa_window.m,d3dmetal.c,d3dmetal_objc.m,window.c}`
  (read-only).
- Markers: `reports/research/LANE-A-PROGRESS.md` + run dirs `reports/phase4-hollow-knight/laneA-…-20260627-*`.
- Memory: `project_hk_groundtruth_72a4f84`, `project_floor_reach_20260625`.
- Regression surface: commits `3bcace4`, `0719cb5`, `3bcace4`/`0719cb5` (direct-mem cache-hit gated copy).
- **Rails honored:** read-only; no edits to ntdll/dxmt; no build/deploy/HK run; this file is the only write.
