# HK+ABZU ARM64EC exception dispatch upstream comparison

Date: 2026-07-06

Scope: read-only source comparison against `AndreRH/wine` branch `arm64ec`,
commit `2e8ad15d5b7b87ef0e1520bf9fec2ecfb31fd1b1`.

## Verdict

The class bug is not "bad guest stack" first. The old HK AV delivery proves the
manual x64 frame was good enough to enter x64 `KiUserExceptionDispatcher` and
reach x64 `ntdll` `virtual_unwind`. It fails because MacRunner enters the AMD64
Wine/ntdll exception path under HyperBridge, where `virtual_unwind` calls
`__wine_unix_call_dispatcher`. That Unix-call boundary is not a valid guest x64
execution target, so HB reports `EXEC_FAULT` / `JIT helper fault` before a real
guest language handler or C++ EH handler can run.

Upstream ARM64EC avoids this by keeping exception dispatch in ARM64EC ntdll:
ARM64 context is converted to `AMD64_Context`, then ARM64EC-side
`dispatch_exception` / `call_seh_handlers` / `RtlVirtualUnwind2` walks x64 frames
without executing x64 Wine Unix-call glue as guest code.

## Upstream reference path

- `/tmp/AndreRH-wine-arm64ec-codex/dlls/ntdll/unix/signal_arm64.c:365-372`
  `signal_set_full_context()` redirects non-EC code to `pKiUserEmulationDispatcher`
  with a captured user context on the frame stack.
- `/tmp/AndreRH-wine-arm64ec-codex/dlls/ntdll/unix/signal_arm64.c:749-772`
  `setup_raise_exception()` builds `exc_stack_layout`, copies `CONTEXT` and
  `EXCEPTION_RECORD`, then sets native `SP` and `PC` to `pKiUserExceptionDispatcher`.
- `/tmp/AndreRH-wine-arm64ec-codex/dlls/ntdll/signal_arm64ec.c:1008-1045`
  ARM64EC `virtual_unwind()` fills ARM64EC non-volatiles, then calls
  `RtlVirtualUnwind2(..., &context->AMD64_Context, ...)`.
- `/tmp/AndreRH-wine-arm64ec-codex/dlls/ntdll/signal_arm64ec.c:1137-1236`
  `call_seh_handlers()` loops x64 language handlers and TIB handlers using the
  ARM64EC dispatcher context.
- `/tmp/AndreRH-wine-arm64ec-codex/dlls/ntdll/signal_arm64ec.c:1278-1314`
  `prepare_exception_arm64ec()` does `context_arm_to_x64()`,
  `pResetToConsistentState()`, optional patched x64 dispatcher thunk, then the
  ARM64EC `KiUserExceptionDispatcher` calls `dispatch_exception`.

Note: this upstream ARM64EC path does not rely on `unix_unwind_builtin_dll` in
the ARM64EC dispatcher path. The failing HK path is the x64 ntdll path that still
tries to cross `__wine_unix_call_dispatcher` from emulated guest code.

## Local deltas

- `engine/wine/dlls/ntdll/unix/signal_arm64.c:145-156`: local native
  `exc_stack_layout` adds `sp` and `pc`; size is `0x470`, upstream is `0x460`.
  This is an ABI-sensitive MacRunner delta, but not the primary HK `EXEC_FAULT`
  cause because the observed failure reaches x64 `virtual_unwind`.
- `engine/wine/dlls/ntdll/unix/signal_arm64.c:1423-1506`: local setup adds
  callback-exception stack routing, signal-safe memory writes, and Apple
  `__wine_pe_x18_thunk` dispatch.
- `engine/wine/dlls/ntdll/signal_arm64ec.c:1116-1174`: local ARM64EC
  `virtual_unwind()` has a MacRunner-only host-boundary early stop at
  `1142-1163`; upstream has no equivalent and proceeds directly to
  `RtlVirtualUnwind2`.
- `engine/wine/dlls/ntdll/signal_arm64ec.c:1271-1377`: local
  `call_seh_handlers()` tracks `macrunner_hb_current_exception_record` and
  restores it through `done:`.
- `engine/wine/dlls/ntdll/signal_arm64ec.c:1416-1430`: local
  `prepare_exception_arm64ec()` is broadly upstream-equivalent, with extra x18
  restore and `__os_arm64x_dispatch_call_no_redirect` guard.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:7793-7858`: MacRunner manual
  guest AV delivery finds AMD64 `ntdll.dll!KiUserExceptionDispatcher`, writes an
  `AMD64_CONTEXT`/record frame to guest stack, then sets `ctx->regs.x64.rip` and
  `ctx->pc` directly to that AMD64 dispatcher. This bypasses the ARM64EC
  dispatcher path above.
- `engine/wine/dlls/ntdll/signal_arm64.c:351-357`: the native-dispatch boundary
  recovery rejects with `reason=not-x64-main-process`; ABZU's repeated
  not-x64-main reports are consistent with this guard being too process-shape
  narrow.

## Evidence tie-in

- `reports/phase4-hollow-knight/HK-RUNG12-GC-EH-DISPATCH-REPORT.md` confirms:
  old Unity AV was delivered to guest x64 `KiUserExceptionDispatcher`, then
  failed at x64 `ntdll.dll` RVA `0x6229c` in `virtual_unwind` at
  `__wine_unix_call_dispatcher`.
- The same report rejects the Mono-GC write-barrier theory for current evidence:
  TLS-shadow removes the old fatal `UnityPlayer+0x2b5605` worker death.
- Galaxy `rcx=0xe06d7363` fits the same class: MSVC C++ EH requires the same
  language-handler unwind path, so broken dispatch/unwind can fall through to
  INT3 padding.
- `reports/abzu/rebaseline-20260702-rsi-producer/abzu-boundary-map-clean-180-20260704-230356/BOUNDARY-MAP-180-SUMMARY.json:21-31`
  shows ABZU `c0000005` stopped at `side=arm64` host-boundary: PC/LR are in a
  non-module host range, while exception address and frame PCs are in ntdll
  `.text`. That is adjacent to the same dispatch/unwind boundary class.

## Root cause

MacRunner currently has two exception delivery worlds:

1. Upstream ARM64EC world: native signal -> ARM64EC `KiUserExceptionDispatcher`
   -> `context_arm_to_x64()` -> ARM64EC `dispatch_exception` ->
   `RtlVirtualUnwind2` over `AMD64_Context`.
2. MacRunner HB manual AV world: runtime fault -> write AMD64 frame -> jump to
   AMD64 ntdll `KiUserExceptionDispatcher` under HB -> x64 `virtual_unwind` ->
   x64 `__wine_unix_call_dispatcher` -> `EXEC_FAULT`.

The second path is the bug. It sets an x64 context, but it enters the wrong
dispatcher domain. The stack is not the first failure; the dispatcher reaches
x64 unwind code. The missing piece is an ARM64EC/HB bridge for guest exceptions
that keeps Unix-call/unwind execution on the native ARM64EC side.

## Fix spec

1. Replace or gate `macrunner_hb_deliver_guest_access_violation()` at
   `engine/wine/dlls/ntdll/unix/macrunner_hb.c:7793-7858`. Do not jump directly
   to AMD64 `ntdll.dll!KiUserExceptionDispatcher` for x64 guest faults in the
   ARM64EC build.
2. Add a bridge that delivers the same `EXCEPTION_RECORD` and `AMD64_CONTEXT`
   into the ARM64EC dispatcher path:
   - target ARM64EC ntdll `#KiUserExceptionDispatcher` / `prepare_exception_arm64ec`;
   - preserve `CONTEXT_EXCEPTION_ACTIVE`, `Rip`, `Rsp`, `EFlags`, and exception
     parameters currently built in `macrunner_hb.c:7807-7825`;
   - let `signal_arm64ec.c:1116-1174` call `RtlVirtualUnwind2` over
     `context->AMD64_Context`.
3. Audit the local host-boundary early stop at
   `engine/wine/dlls/ntdll/signal_arm64ec.c:1142-1163`. If retained, it must
   synthesize/resume the correct x64 frame, not simply stop before guest handlers.
4. Broaden the native-dispatch recovery guard at
   `engine/wine/dlls/ntdll/signal_arm64.c:351-357` from "main image is AMD64" to
   "this thread has a valid HB/x64 guest context or AMD64 frame". That addresses
   ABZU `not-x64-main-process` rejects without making a global catch-all.
5. Non-placebo gate:
   - `MACRUNNER_HB_GUEST_AV_DELIVERY=1` old HK Unity AV path no longer reaches
     `ntdll+0x6229c EXEC_FAULT`;
   - trace reaches language handler / SEH handler or clean `NtContinue`;
   - Galaxy C++ EH `0xe06d7363` no longer falls through to INT3;
   - ABZU host-boundary summary no longer stops at non-module ARM64 PC without
     mapping to a guest frame.
