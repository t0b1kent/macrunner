# What we can take from ARM64 Boot Camp (Mihocka), checked against our tree

Date: 2026-07-27. Coordinator extraction, offline, no run consumed.

Source: Darek Mihocka's ARM64 Boot Camp, "Exiting ARM64 to emulated x64"
(http://www.emulators.com/docs/abc_exit_xta.htm) and the surrounding series. Mihocka is one
of the Microsoft engineers who built the four emulators shipping in Windows 11 — TTD,
xtajit, xtajit64, xtabase. This is not third-party commentary; it is the design we are
reimplementing, described by an author of it. Our `xtajit64.dll` is named after his
component.

Platform is Windows on ARM, not macOS. That is fine and arguably better for us: our upper
half is ARM64 Wine with ARM64EC, i.e. we implement Microsoft's interfaces. His description
is effectively the specification of the contract we have to satisfy.

## 1. Our architecture is the sanctioned one — this is load-bearing reassurance

> "Most of the heavy lifting of the emulation is actually handled by the Windows kernel and
> OS system DLLs such as NTDLL … What most people think of as 'the emulator' (the various
> XTA\*.DLL and .EXE files) are merely the x86/x64-To-ARM code translation binaries; and
> these are pluggable components. Any clever third party could develop an alternate
> translator without needing to make any changes to the existing emulation plumbing wired in
> to Windows."

That is exactly our split: Wine/ntdll is the plumbing, HyperBridge is the pluggable
translator. We are not fighting the design.

## 2. The icache-flush callback is REAL, LOAD-BEARING, and he MEASURES it

> "Flushing the instruction cache sends a callback to the x64 translator to flush any cached
> translations it may have for that target function. Flushing the i-cache is costly enough
> for native code, it's extra costly when a dynamic translator is involved."

He proves it with a micro-benchmark: an indirect call in a hot loop costs 187 cycles
emulated / 9 cycles native. Adding one `FlushInstructionCache()` per iteration takes it to
**10812 ms vs 187 ms per round — over 50× slower for emulated x64 and 300× for native
ARM64.**

**This is the independent confirmation of our finding in
`HK-ICACHE-FLUSH-CALLBACK-IS-A-STUB-20260727.md`.** A correctly wired translator does
*substantial* work on that callback. Ours returns `STATUS_SUCCESS` and does nothing, after
discarding the address range. We are not paying that cost — and we are not getting the
correctness it buys.

## 3. The cost of JIT, quantified — and why JIT-on-JIT is the known-hard case

> "the 'cost of JIT' is on the order of 10000 clock cycles for even the most trivial block of
> x64 code. This has always been especially problematic for JIT-on-JIT scenarios."

Mono is precisely a JIT-on-JIT scenario. **This gives a second, competing explanation for
our ~15-minute "spin" that does not require a livelock at all:** if freshly generated Mono
code is being re-translated repeatedly at ~10⁴ cycles a block, the thread is doing real work
the whole time — it is a re-translation storm, not a stuck loop.

Note that the missing invalidation can produce *either* failure mode, and they are opposites:

- invalidation never happens → the guest re-runs a **stale** translation (silent wrong
  execution, permanent non-advancement);
- or something else forces re-translation constantly → a **translation storm** (huge but
  finite cost, thread busy, progress glacial).

Our evidence — finite ~15 min spin, then presents resume but managed state never advances —
is compatible with the storm followed by stale execution. **The lane must discriminate these,
not pick one.** The instrument is the same: after Mono writes code into an executable page,
is a previously translated block re-executed, or re-translated, and how often?

## 4. Microsoft's own workaround for JIT-on-JIT: interpret one of the two levels

> "One workaround for JIT-on-JIT is to interpret at one of those two levels. Flipping the
> registry key to use xtabase.dll instead of xtajit64.dll now gives a JIT-on-interpreter
> scenario, which after re-running the test interpreted the run time drops and performance
> goes up!"

Measured: 8726 ms vs 10812 ms. **The interpreter beat the JIT.** He adds that he wishes
xtabase had been built as a hybrid JIT-interpreter (hotspot-style: interpret everything,
promote only hot blocks), and believes Rosetta 2 does something like that.

**Directly applicable to us:** we already have an interpreter/helper path
(`MACRUNNER_HB_JIT_DIRECT_MEM=0` selects it). Running Mono-generated code interpreted rather
than translated is a cheap experiment that is *both* a correctness workaround (no stale
translation to go stale) *and*, per his measurement, potentially faster. This is a candidate
fix, not just a diagnostic.

## 5. Checked against our tree — one gap, one non-gap

| Item | Our state |
|---|---|
| `BTCpu64FlushInstructionCache` → translation invalidation | **GAP.** `xtajit64/cpu.c:198` discards addr/size; `xtajit64/unixlib.c:710` is `return STATUS_SUCCESS;`. |
| `VirtualAlloc2` with `MEM_EXTENDED_PARAMETER_EC_CODE` | **NOT a gap.** Mihocka writes *"it would be very nice if Wine supported the VirtualAlloc2() function"* — ours does: `kernelbase/memory.c:449`, and the EC attribute is honoured in production at `ntdll/unix/virtual.c:6481,6544`, not merely in tests. |
| EC bitmap | Present: `ntdll/unix/virtual.c:3498` sets `peb->EcCodeBitMap`; machinery in `loader.c`, `signal_arm64ec.c`. |

## 6. Things worth taking later (not now)

- **Co-simulation as a validation method.** The four execution modes (interpreter, JIT,
  cached, ARM64EC) "can be (and have been) used to double check each other's correctness …
  This technique can also be used to bootstrap and check the correctness of any new alternate
  translator." Running our interpreter against our JIT on the same guest code is a
  ready-made correctness harness we have not built.
- **XtaCache is post-JIT and cannot cover dynamic code.** *"XtaCache can only work on static
  code backed by an on-disk .EXE or .DLL binary."* So Mono's generated code can never be
  served from an AOT cache — it must go through JIT or interpreter every single time. Our
  `hb_aot_cache` inherits that limitation by construction; do not expect it to help here.
- **ARM64EC register pressure and the varargs trap.** X13/X14/X23/X24 are unavailable;
  varargs pass the stack-arg pointer in X4 and the count in X5 (mapping to x64 R10/R11), and
  a mis-declared varargs signature makes the compiler optimise those away, losing arguments
  or crashing the marshalling. Mihocka hit this himself. Worth remembering when our thunks
  misbehave.
- **CPUID and soft intrinsics.** `softintrin.h` / `widemath.h` / `softintrin.lib` in the
  Windows SDK implement 500+ x64 intrinsics; some (CPUIDEX, RDTSC, RDTSCP, FXSAVE, FXRSTOR)
  are deliberately left as x64 bytecode and emulated because their behaviour is
  process-dependent. Relevant to the ARM64EC/ARM64X blocker, not to the black frame.

## Honest limits

Everything in sections 1–4 is Mihocka's measurement on Windows on ARM hardware, not ours.
Section 5 is our code, checked first-hand today. The reframing in section 3 is my inference
from combining the two and is `[HYPOTHESIS]` until the lane instruments it.
