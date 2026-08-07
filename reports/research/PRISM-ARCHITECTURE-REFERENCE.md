# Public technical reference: Prism / Windows-on-Arm x86/x64 → Arm64 emulation
(ChatGPT deep-dive, 2026-06-05. Source brief: CHATGPT-BRIEF-prism-full-architecture-reference.md.
Standing reference for MacRunner engine design. Confidence labels: [CONFIRMED + source] /
[INFERRED] / [UNKNOWN/not public].)

## Scope
Public-source engineering reference, NOT an internal Microsoft dump. Microsoft documents the broad
shape of Prism and ARM64EC but NOT Prism's exact IR, block scheduler, in-memory code-cache layout,
SMC invalidation, or memory-order implementation. Where not public → marked [UNKNOWN] with the
closest public analogue (QEMU TCG, FEX-Emu, Box64, Rosetta/Asahi, published RE work).

Strongest public facts:
- WoA emulation JIT-compiles x86/x64 blocks to Arm64 and uses a service to cache translated blocks
  per module. [CONFIRMED]
- Win11 on Arm supports x86 AND x64; Win10 was x86-only (x64 preview 2020 → GA with Win11).[CONFIRMED]
- Prism = Win11 24H2-era improvement; added x64 AVX/AVX2/BMI/FMA/F16C in Insider Build 27744.[CONFIRMED]
- ARM64EC lets x64 + Arm64EC interoperate in one process: x64 under emulation, EC native. [CONFIRMED]

## 1. History & lineage
- Win10-on-Arm: x86 emulation via **XTA** stack — `xtajit.dll` (x86→Arm64 JIT), `xtac.exe`
  (XTA compiler), `XtaCache.exe` (persistent cache service). [CONFIRMED, Black Hat/FFRI]
- **CHPE** (Compiled Hybrid PE): x86-looking system DLLs carrying Arm64-translated bodies to cut JIT
  work (predecessor to ARM64EC). `SysChpe32`. [CONFIRMED, FFRI]
- x64 emulation: Insider preview Build 21277 (Dec 2020) → GA with Win11. [CONFIRMED]
- **Prism** (Win11 24H2): perf improvement; later x64 AVX/AVX2/BMI/FMA/F16C (Build 27744). Only x64
  apps get the new CPU features, not 32-bit. [CONFIRMED]
- [UNKNOWN] whether Prism fully replaced XTA binaries or is an evolution/umbrella; researchers still
  see `xtajit.dll`/`xtajit64.dll`.

**MacRunner lesson:** the right model is a STACK — translator + cache manager + ABI boundary +
native system-DLL side + fallback interpreter — not "one interpreter." Don't advertise AVX/AVX2/BMI/
FMA/F16C/AVX-512 until decoder+lifter+JIT+XSAVE/XGETBV state+tests are real.

## 2. Architecture
- Transparent emulation; JIT x86 blocks→Arm64, cache optimized blocks. [CONFIRMED]
- ARM64EC: one process holds x64 (emulated) + Arm64EC (native); EC follows x64 calling convention/
  stack/data layout to interoperate. [CONFIRMED]
- [INFERRED] flow: x64 app → emulator runs translated Arm64 blocks → x64 calls EC/Arm64X system DLL
  **entry thunks** → native EC runs → if native calls x64 callback, **exit thunk/call checker**
  routes back to emulator.
- Components: `xtajit.dll` (native Arm64 emulator DLL for x86, replaces wow64cpu.dll) [CONFIRMED];
  `xtajit64.dll`/`xtajit64se.dll` seen but role-split not documented [CONFIRMED-observed/UNKNOWN-role];
  `XtaCache.exe` runs `xtac.exe` to build cache files [CONFIRMED]; most OS code loaded by x64 apps is
  ARM64EC [CONFIRMED]; Arm64X binaries hold both classic Arm64 + EC, loadable by both [CONFIRMED].

**MacRunner equiv:** ARM64 Wine builtins + EC-like ABI glue = native side. x64 guest must call only
x64-callable entry thunks, never raw ARM64 bodies. A persistent cache daemon should be logically
separate from the runtime translator.

## 3. Translation pipeline
- [CONFIRMED] JIT-compiles blocks of x86→Arm64, optimizes emitted Arm64.
- [UNKNOWN] IR, block formation, regalloc, flag-lowering, chaining, inline-cache, return prediction,
  tiering, hot-block reopt — not public.
- Closest analogues to adopt:
  - **QEMU TCG**: TBs, direct chaining via `goto_tb`, NO chaining across page boundaries (mappings
    change), per-page TB linked lists for invalidation. [CONFIRMED] → page-bounded TBs are the safe
    baseline; don't chain across guest pages unless generation/invalidation covers both.
  - **FEX**: JIT + memory-model emulation + SMC + AVX/XSAVE + Wine integration; config split
    (normal TSO / vector TSO / memcpy-REP TSO / split locks / inline SMC). [CONFIRMED]
  - **Box64**: dynarec; knobs for dirty/modified blocks, hot-page detect, aligned atomics, weak
    barriers, return-to-dirty-block, persistent DynaCache, PAUSE policy. [CONFIRMED]

## 4. Code cache
- Persistent: service caches translated blocks **per module** for reuse across launches/apps.
  [CONFIRMED] Black Hat "Jack-in-the-Cache": x86 app load → notify `XtaCache.exe` → search cache →
  map matching `.jc` into process → transfer control. Filenames include exe/dll name + content hash
  + path hash + update count; `xtac.exe` adds newly translated blocks. FFRI: cache file = header,
  BLCK, stub, translated code, address pairs, NT path; ARM64 code unobfuscated. [CONFIRMED]
- **Security**: default full perms only for `XtaCache.exe`, but changeable with admin; modified cache
  could be loaded/executed ("XTA cache hijacking"); integrity NOT checked on tested system.[CONFIRMED]
- [UNKNOWN] in-memory cache organization, eviction, anonymous-JIT handling.

**MacRunner persistent-cache key** = module canonical path + content hash + size/timestamp + machine
type + image base/section-layout hash + translator version + CPU-feature policy + TSO policy +
codegen version + ABI mode. **Trust:** signature/HMAC, translator-version match, file-hash match,
no world-writable cache dir, admin-only mutation, debug mode disables persistent cache.

## 5. Self-modifying code (SMC) & coherence  ← CURRENT MACRUNNER GATE
- [UNKNOWN] Prism's write-to-translated-page detection / granularity / inline-SMC / trap-vs-instrument.
- [CONFIRMED Windows contract] apps generating/modifying code MUST call `FlushInstructionCache`;
  `VirtualProtect` docs: caller ensures cache coherency before executing after write/protect.
- Public SMC designs to copy:
  - **QEMU**: host pages write-protected when code translated; write → SIGSEGV → invalidate all TBs
    in page + re-enable writes; per-page TB lists + lists to undo direct chaining. [CONFIRMED]
  - **FEX**: was page/block SMC (invalidate touched page, run to block end); later **inline-SMC**:
    reconstruct x86 context, regenerate 1-instruction block for the modifying instruction, continue.
    [CONFIRMED]
  - **Box64**: dirty/hot-page policies (`BOX64_DYNAREC_DIRTY`), return-to-dirty handling. [CONFIRMED]

**Required MacRunner SMC contract** (Unity/Mono/Boehm): generated-code path = alloc → write →
VirtualProtect RW→RX (or RWX stays exec) → FlushInstructionCache → execute. Implement:
1. page→TB list  2. TB generation snapshots  3. invalidate on guest write to translated page
4. invalidate on FIC/NtFIC  5. invalidate on VirtualProtect/NtProtectVirtualMemory→EXECUTE
6. direct-chain unlinking  7. inline-SMC escape  8. anonymous JIT cache separate from PE module cache.

**Decisive test:** log every guest write to a page with TBs, every FIC/NtFIC, every VirtualProtect
exec-transition; at fault print target page generation vs TB generation. If faulting page was
written/protected/flushed AFTER translation and TB generation is stale → root confirmed.

## 6. ARM64EC / Arm64X
- EC follows x64 conventions; EC code runs native. [CONFIRMED]
- Call checkers: indirect call target in **x11**, exit-thunk in **x10**; if target is x64, control
  goes to emulation scaffolding first. [CONFIRMED]
- Entry thunks: x64 callers → EC functions. Exit thunks: EC → x64, set up frame, copy args EC→x64,
  call `__os_arm64x_dispatch_call_no_redirect`, transfer results back. [CONFIRMED + Corsix]
- Arm64X PE: both Arm64 + EC in one file; OS transforms loaded view per process arch; x64 sees it as
  x64. `IMAGE_DYNAMIC_RELOCATION_ARM64X`. [CONFIRMED + FFRI]
- [INFERRED] Runtime-generated x64 (Mono JIT) has NO static EC entrypoint → treat as ordinary x64
  guest; can enter native EC helpers ONLY via x64-callable entry/import thunks or emulation dispatch.

**MacRunner invariant:** generated x64 → x64 target: translate; → EC target: entry/import thunk only;
→ raw ARM64 body: invalid, route or reject. **Helper-fault diagnostic:** caller PC arch (x64 JIT /
x64 PE / EC), target PC arch (x64 / entry thunk / raw EC / `__os_arm64x*`), RSP align, shadow space,
RCX/RDX/R8/R9, return address.

## 7. Memory model
- [UNKNOWN] Prism's ordering algorithm / Snapdragon TSO-like assist.
- Apple: ACTLR_ELx<1> = TSO enable, AIDR_EL1<9> = support (Asahi/Marcan). macOS enables TSO for
  Rosetta on user-mode return; arbitrary-process TSO needs KEXT/sysctl hack — NOT 3rd-party API
  (TSOEnabler). FEX: x86 memory-model emulation is one of the largest costs; without HW TSO must use
  atomics/LRCPC. [CONFIRMED]
- **MacRunner:** do NOT rely on Apple HW TSO. Software TSO: acquire/release (or stronger) loads/
  stores, LOCK/XCHG/MFENCE full barriers, no load hoisting across loops/calls/atomics/safepoints,
  strict split-lock fallback. (Already implemented + litmus-validated.)

## 8. Syscall / API boundary
- x86-32: WOW64 user-mode emulator between 32-bit ntdll and kernel; on ARM uses `xtajit.dll` instead
  of wow64cpu.dll + xtac/XtaCache. [CONFIRMED]
- x64: most OS code = ARM64EC; x64 runs under emulator, EC native. [CONFIRMED] [INFERRED] user32/
  gdi32/d3d11/dxgi calls land in EC system DLLs via x64-visible entrypoints.
- [UNKNOWN] no public special Prism graphics-translation layer; D3D/DXGI/drivers are normal Windows.
- **MacRunner:** x64 guest should call x64 PE frontends/stubs bridging to native ARM64 DXMT/Metal,
  not WineD3D/GL fallback unless selected.

## 9. Performance
- [CONFIRMED] Prism improves perf; per-module cache reuse; expanded x64 ISA features unblock games.
- [SECONDARY] press quotes ~10–20% uplift; not an official universal number.
- [INFERRED, common DBT] direct chaining, deferred flags, indirect-target cache, return prediction,
  fast syscall/API thunks, SIMD lowering, hot-block/tiered retranslation.

## 10. Comparison
| Engine | Unit | Persistent cache | SMC | Memory model | Interop | OS |
|---|---|---|---|---|---|---|
| Prism/WoA | blocks | yes, per-module svc | [UNKNOWN] | [UNKNOWN] | ARM64EC/Arm64X | Windows |
| XTA/old WoA | blocks | XTA .jc | [UNKNOWN] | [UNKNOWN] | CHPE | Windows |
| Rosetta 2 | blocks AOT+JIT | yes | [not public] | Apple HW TSO | macOS ABI | macOS |
| QEMU TCG | TBs | no default | page write-protect + TB invalidation | explicit barriers | syscall emu | many |
| FEX | blocks | optional | page SMC + inline SMC | HW TSO else software | Linux/Wine | Linux |
| Box64 | dynablocks | DynaCache | dirty/hot-page | configurable | ELF/Wine | Linux |
| **MacRunner target** | TBs/pages | should add | page+range+FIC+inline | software TSO | Wine ARM64 + EC-like thunks | macOS |

## 11. Prioritized lessons for MacRunner
- **P0 Code-cache coherence** (page→TB, generation snapshot, invalidate on write/FIC/NtFIC/
  VirtualProtect-exec, unlink chains, anonymous-JIT separate, inline-SMC). Most likely root of the
  Mono JIT helper fault.
- **P0 Treat Mono/.NET/JS/game JITs as first-class**: trace generated-code lifecycle (alloc→write→
  protect→flush→execute); execute without prior invalidate = stale cache.
- **P0 ARM64EC thunk classifier** before blaming helper pointers: generated x64 must never jump to
  raw ARM64 bodies.
- **P0 Keep software TSO/no-hoist default.**
- **P1 Persistent cache only after correctness** (hash/version keys, signed/HMAC, debugger
  invalidates).
- **P1 Deconflict Boehm/Mono mprotect with SMC**: per-page protection-OWNER STACK (SMC_CODE_WATCH /
  GC_DIRTY_TRACKING / GUARD_PAGE / USER_PROTECTION), never one "protected" bool.
- **P1 Engine knobs** (FEX/Box64-style): MACRUNNER_STRICT_TSO / VECTOR_TSO / SMC_STRICT /
  SPLIT_LOCK_STRICT / DIRTY_BLOCK_POLICY / PERSISTENT_CACHE.

## 12. Target architecture
ProcessCodeCache { PE ModuleCache (key=hash/path/RVA, persistent optional); AnonymousJitCache
(key=process+guest page+generation, never persistent by default); PageState{ generation, TB list,
direct links, last_writer, last_FIC, last protection transition, protection owners } }.
Invalidation sources: guest store to translated page; FIC/NtFIC; VirtualProtect/NtProtectVirtualMemory;
unmap/free; section remap; debugger breakpoint write; inline-SMC write to current TB.
EC boundary: x64→x64 translate; x64→EC entry thunk only; EC→x64 exit thunk/`__os_arm64x` dispatch;
runtime x64 JIT → native helper requires x64-callable thunk.

### Decisive probe for the current Mono helper fault
For `mono-2.0-bdwgc.dll rva=0x4fe309 MEMORY_FAULT reason=JIT helper fault` print: faulting guest PC/
module/RVA; PC in PE image vs anonymous JIT vs EC thunk; guest target pointer if indirect; page
generation; TB generation; last guest write to page; last FIC range; last VirtualProtect transition;
last direct-chain target update; caller RSP align/shadow space; target arch classification.
→ page_generation > tb_generation = stale code cache; raw-EC-from-x64-JIT = EC-thunk routing;
page with both GC-mprotect and SMC owner = protection deconfliction.

## Final
Prism's lesson isn't one magic translator — it's an INTEGRATED SYSTEM: JIT block translator +
per-module cache service + hybrid ARM64EC system-DLL ABI + strong code-cache coherence + correct x86
memory model + safe generated-code handling.
