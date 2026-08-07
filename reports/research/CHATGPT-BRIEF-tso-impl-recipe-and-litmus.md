# ChatGPT brief — exact ARM64 recipe for x86 TSO+atomics (edge cases) + a litmus validation suite

Tight scope, one session. Public knowledge (ARM ARM, FEX-Emu, box64, QEMU, Intel SDM). This is a
CODE-LEVEL implementation request — we're about to write it and must not ship a subtly-broken
atomics/memory-order layer (it would corrupt our 20M-case-clean ISA correctness or cause rare hangs).

## Context
Custom x86-64→ARM64 JIT (HyperBridge) on Apple Silicon. We confirmed the boot-blocker root = x86
TSO not honored on weak ARM64 (spin-reader never sees a writer's store → livelock). Plan agreed:
**C = don't hoist guest loads; B = software TSO** (guest scalar load→LDAR, store→STLR, LOCK/XCHG/
MFENCE→DMB ISH). We already emit DMB for fences and made interpreter LOCK ops full barriers. Now we
implement the codegen LDAR/STLR + true atomic LOCK RMW. We need the EXACT recipe + the gotchas.

## Questions
1. **Exact ARM64 instruction selection for x86 TSO loads/stores** at each width (8/16/32/64):
   - load → `LDARB/LDARH/LDAR(w)/LDAR(x)`; store → `STLRB/STLRH/STLR(w)/STLR(x)` — confirm these are
     the right acquire/release primitives and that LDAR/STLR pairs give x86-TSO (allowing only the
     Store→Load relaxation x86 permits, without over-fencing). Is `LDAPR` (RCpc) preferable to `LDAR`
     for x86 (since x86 is RCsc-ish / actually stronger)? Which matches x86 TSO exactly?
   - Do LDAR/STLR require natural alignment? What about x86's allowed UNALIGNED loads/stores — can we
     still use LDAR/STLR, or must unaligned guest accesses fall back to plain LDR/STR + an explicit
     DMB? How does FEX handle unaligned (its "HalfBarrier" option)?
2. **True atomic LOCK RMW** (CMPXCHG/XADD/XCHG/LOCK ADD|OR|AND|XOR|BTS|BTR|BTC, CMPXCHG8B/16B):
   - LSE mapping (`CASAL`, `LDADDAL`, `SWPAL`, `LDSETAL`/`LDCLRAL`/`LDEORAL`) vs `LDAXR/STLXR` retry
     loop — which to use on Apple M-series (LSE is present), and the exact sequence incl. flags
     (x86 sets ZF/CF/OF/SF/PF/AF from the RMW result — how to recover them correctly).
   - CMPXCHG16B → `CASPAL` (pair) — correctness + alignment (16-byte).
   - **x86 allows UNALIGNED / split-lock LOCK ops** (a LOCK crossing a cacheline). ARM atomics
     can't do a split lock. What's the correct fallback (global lock / stop-the-world / a bus-lock
     emulation)? What do FEX/box64 do for split-lock?
3. **No-hoist (C) implementation:** the exact rule set so the JIT never CSE's/LICM's/hoists a guest
   memory load across a loop backedge, call, atomic, or thread safepoint — but still allows local
   reuse within a straight-line run. How do FEX/QEMU model guest loads as memory ops that the
   optimizer can't lift?
4. **Flags from atomic RMW:** for LOCK ADD/SUB/INC/DEC/XADD etc., x86 sets the full flag set from
   the result. When using LSE (which returns the OLD value) or a CAS loop, give the exact way to
   compute ZF/CF/OF/SF/PF/AF identical to the non-locked ALU op.

## Deliverable A — implementation recipe
A concrete table: x86 op → ARM64 sequence (instructions + alignment rule + flag recovery), covering
plain load/store (LDAR/STLR), each LOCK RMW (LSE preferred + LL/SC fallback), CMPXCHG8B/16B,
unaligned + split-lock fallback, and the no-hoist rule. Tag each [CONFIRMED + source] / [INFERRED].

## Deliverable B — litmus validation suite (so we can PROVE correctness fast)
Give ready-to-run **x86-64 test programs** (asm or C with inline asm) for 2 threads each, with the
EXPECTED x86-TSO outcome, that we run under the translator to validate:
- message-passing (data=42; flag=1 // wait flag; assert data==42) — must pass
- store-buffering (T0: x=1;r1=y // T1: y=1;r2=x — r1==0&&r2==0 ALLOWED on x86) — checks we don't
  over-fence to SC
- Dekker / spin-flag (must terminate)
- CAS loop (lock cmpxchg must eventually succeed when mem==expected)
- a LOCK XADD counter from N threads → final value exact (checks atomicity, no lost updates)
- an unaligned LOCK op (checks the split-lock fallback)
For each: the x86 code, the expected result, and what a FAILURE implies (which of LDAR/STLR / LSE /
no-hoist / split-lock is broken). This becomes our regression gate.
