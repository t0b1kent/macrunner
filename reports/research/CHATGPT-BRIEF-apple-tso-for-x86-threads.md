# ChatGPT brief — enabling x86 TSO memory ordering for translated guest threads on Apple Silicon

Tight scope, one session. Public knowledge (Apple Rosetta, Asahi, FEX-Emu, box64, QEMU, ARM docs).
This is a concrete ENGINEERING question, not theory — we need implementable mechanisms.

## Context
We have a custom x86-64→ARM64 binary translator (NOT Rosetta) running an unmodified Unity/Mono
game under ARM64 Wine on Apple Silicon (macOS, M-series). CONFIRMED root cause of our boot
livelock: guest x86 code assumes **TSO (Total Store Ordering)**, but our translated code runs with
ARM64's weak memory model. A spin-reader's plain `mov [flag]` load never observes another guest
thread's store → infinite livelock before the graphics device is created. We already made x86 LOCK
ops full barriers (publisher side). We now need the **reader side**: give translated guest
loads/stores x86 ordering.

## Questions (concrete, implementable)
1. **Apple Silicon hardware TSO toggle:** M-series CPUs have a per-thread TSO mode that Rosetta 2
   uses. What is the actual mechanism to ENABLE it for a thread in a user-space process on macOS?
   - Is there a usable API / sysctl / thread flag / `pthread` call / `__builtin` / register write
     (e.g. an `ACTLR_EL1`/`hidden` MSR, or a macOS-specific `thread_policy`/`os_` call)?
   - What ENTITLEMENT is required (e.g. `com.apple.security.cs.allow-jit`,
     `com.apple.private.*`)? Is it available to non-Apple/third-party binaries at all, or
     Rosetta-only?
   - Cite: Apple docs, Asahi Linux findings (they documented the M1 TSO bit / `AMX`-style ACTLR),
     FEX-Emu and box64 macOS discussions, any open-source code that flips TSO on macOS.
2. **If the hardware TSO toggle is NOT usable by third parties**, what is the correct
   barrier-insertion strategy in a JIT to emulate x86 TSO on weak ARM64?
   - The standard approach: x86 load → ARM64 `ldar` (load-acquire); x86 store → `stlr`
     (store-release); x86 LOCK/`mfence` → `dmb ish` full barrier. Confirm this is sufficient for
     x86 TSO, and the exact cost/placement (every guest load/store, or only synchronizing ones?).
   - What do FEX-Emu / box64 / QEMU actually do on weak hosts to preserve x86 ordering? Do they use
     ldar/stlr for ALL guest memory ops, or the hardware TSO mode when available and fall back to
     barriers otherwise? Cite their approach.
3. **JIT correctness pitfall:** can a JIT that caches/hoists a guest memory load into a register
   across a loop turn a legitimate spin-wait into an infinite loop EVEN on TSO? (i.e. must guest
   memory loads be treated as volatile / re-loaded each iteration regardless of ordering?) How do
   the above projects prevent load hoisting of guest memory?
4. **Asahi/Linux note:** Asahi documented enabling the M-series TSO bit for their x86 emulation.
   What exactly do they set, and is the macOS equivalent reachable from user space?

## Deliverable
A concrete, ranked set of options to give translated guest threads x86 TSO on Apple Silicon:
- **Option A: hardware TSO mode** — exact enable mechanism + entitlement + whether third-party-usable
  (with sources). If usable, this is preferred (near-zero perf cost, fixes all loads/stores).
- **Option B: JIT barrier emulation** — exact ldar/stlr/dmb mapping for x86 loads/stores/LOCK/fences,
  what FEX/box64/QEMU do, and the perf tradeoff.
- **Option C: load-volatility** — preventing the JIT from hoisting guest loads.
For each: is it usable by a non-Rosetta translator on macOS, the entitlement/risk, and the one-line
"do this first." End with your single recommendation for fastest correct fix.
