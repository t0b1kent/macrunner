# HB EH dispatch root cause: RaiseException returns to throw-continuation

## Symptom

HK Galaxy64.dll throws MSVC C++ EH from its local `_CxxThrowException`-style helper:

- `Galaxy64.dll+0x662784..0x662824`: helper prepares `RaiseException`.
- Runtime registers at the later trap: `rcx=0xe06d7363`, `rdx=1`, `r8=4`.
- Expected: guest SEH/C++ EH dispatch handles/unwinds, or guest unhandled-exception path terminates.
- Observed: execution returns to the caller continuation `Galaxy64.dll+0x831682`, which is the compiler-emitted unreachable `int3` after the throw call.

Primary evidence is in `reports/phase4-hollow-knight/HK-GALAXY-0x831682-CAUSE.md`.

## Where RaiseException is intercepted

`engine/hyperbridge/src/` has no direct `RaiseException` / `NtRaiseException` / `RtlRaiseException` implementation. The interception is in the Wine/HB import layer:

- `engine/wine/dlls/ntdll/loader.c:1643-1645` marks `AddVectoredExceptionHandler`, `RemoveVectoredExceptionHandler`, and `RaiseException` as HB-handled imports.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:21575-21578` declares `kernel32/kernelbase!RaiseException` as a 4-argument semantic import.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:22558-22560` includes `RaiseException` in the native/export local-semantic allowlist.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:22011-22049` routes imports through semantic shortcuts before falling back to the native PE import call.
- `engine/wine/dlls/ntdll/unix/macrunner_hb.c:20740-20814` is the actual `RaiseException` shortcut.

The shortcut builds a host-side `EXCEPTION_RECORD`, copies exception arguments from guest memory, calls only handlers registered in `macrunner_hb_vectored_handlers`, and then returns as if `RaiseException` completed normally:

- `macrunner_hb.c:20754-20756`: fills `ExceptionCode`, `ExceptionFlags`, `ExceptionAddress`.
- `macrunner_hb.c:20780-20810`: iterates HB's vectored-handler table.
- `macrunner_hb.c:20812-20814`: sets `LastStatusValue=STATUS_UNHANDLED_EXCEPTION`, `*ret=0`, `return TRUE`.
- `macrunner_hb.c:22219-22222`: common import epilogue writes `rax=rc`, `rsp+=8`, `ctx->pc=ret_addr`.

For Galaxy, `ret_addr` is `Galaxy64.dll+0x831682`, the unreachable `int3` after the throw helper call. So the observed trap is a direct consequence of the semantic import returning.

## Does it reach guest RtlDispatchException?

No.

The normal x64 Wine guest path is in `engine/wine/dlls/ntdll/signal_x86_64.c`, not `rtl.c`:

- `signal_x86_64.c:844-868` implements guest `RtlRaiseException`; it captures context, sets `rec->ExceptionAddress` from the return address, calls `dispatch_exception`, then calls `NtRaiseException` only if still unhandled/debugged.
- `signal_x86_64.c:359-394` implements `KiUserExceptionDispatcher`; it calls `dispatch_exception` then `int3` as an unreachable tail.
- `signal_x86_64.c:300-352` shows `dispatch_exception` walking frame handlers / TEB handlers and returning `STATUS_UNHANDLED_EXCEPTION` only after real guest search.

The HB `kernelbase!RaiseException` shortcut bypasses all of this. It does not enter guest `RtlRaiseException`, guest `NtRaiseException`, guest `KiUserExceptionDispatcher`, or the `dispatch_exception` frame-walk. `engine/wine/dlls/ntdll/rtl.c` only has unrelated users such as debug-print raising; it is not the active dispatcher for this case.

## Root cause

`kernel32/kernelbase!RaiseException` is implemented as a vectored-handler-only semantic shortcut under HB. When no HB-stored vectored handler returns `EXCEPTION_CONTINUE_EXECUTION`, the shortcut still reports the import as handled and returns normally to guest code.

That violates Windows/C++ EH semantics for `RaiseException(0xe06d7363, EXCEPTION_NONCONTINUABLE, 4, args)`: a noncontinuable C++ throw must transfer into the guest exception dispatcher and unwind/search handlers. It must not return to the throw helper's caller.

Additional semantic mismatch: the shortcut sets `record.ExceptionAddress = ctx->pc` (`macrunner_hb.c:20756`), which is the import thunk/current PC, not the would-be guest continuation/raise site. Guest `RtlRaiseException` would derive the exception address from the return address (`signal_x86_64.c:857-859`).

## ABZU comparison

This is not the exact ABZU `pc=0x60` intercepted-indirect-call resume loop.

ABZU evidence in `docs/HB-INDIRECT-CALL-RESUME-ROOTCAUSE.md`:

- import/unixlib dispatch returns successfully but exact resume at the guest return-site is lost;
- control re-enters the unix-call path and later falls into low-PC `pc=0x60` syscall recovery;
- the defect is centered on intercepted indirect-call return-site resume and low-PC recovery.

Galaxy evidence:

- PC is a valid Galaxy code address, `Galaxy64.dll+0x831682`;
- no `pc=0x60` low-PC recovery appears in this evidence;
- the immediate cause is that HB's `RaiseException` semantic import deliberately returns to `ret_addr` after only vectored-handler emulation.

Common family: both are HB non-local-control-transfer bugs where a path that should not resume as an ordinary call does resume at a normal continuation. Mechanism differs: ABZU is intercepted indirect-call/native-dispatch resume; Galaxy is exception-dispatch shortcut returning from a noncontinuable EH raise.

## Minimal patch candidate, not applied

Do not just special-case `0xe06d7363` by swallowing or skipping the `int3`. The root fix is to stop treating `RaiseException` as a normal returning semantic import.

Candidate A, preferred root direction:

- Extend `macrunner_hb_try_vectored_exception_semantic()` to accept/read the import `ret_addr`.
- For `RaiseException`, after any vectored handlers return `EXCEPTION_CONTINUE_SEARCH`, build a guest exception delivery instead of `return TRUE` with `ret=0`.
- Construct the exception context as the would-be post-call state: `Rip=ret_addr`, `Rsp=ctx->regs.x64.rsp + 8`, exception args copied from guest memory, `ExceptionAddress=ret_addr`.
- Transfer control to the guest x64 exception machinery, preferably by routing through guest `ntdll!RtlRaiseException` with a guest `EXCEPTION_RECORD` on guest stack, or by a dedicated HB guest-exception-delivery path that sets up `KiUserExceptionDispatcher` exactly as Wine's signal path does.
- Only if a guest/vectored handler returns `EXCEPTION_CONTINUE_EXECUTION` should the import resume normally.
- For noncontinuable exceptions, returning normally must be treated as invalid/noncontinuable, not as `STATUS_UNHANDLED_EXCEPTION` plus normal return.

Candidate B, short diagnostic gate:

- Env-gate a narrow path for `RaiseException` code `0xe06d7363` that logs whether guest dispatch is entered and never normal-returns to `ret_addr` on `EXCEPTION_NONCONTINUABLE`.
- This is diagnostic scaffolding only; final behavior must be class-correct for all `RaiseException` users.

Avoid candidate C:

- Do not simply `return FALSE` from the semantic shortcut and fall through to `macrunner_hb_call_arm64_pe_import12_for_ctx()`. That would call the native/host PE import path, not necessarily the guest x64 SEH path, and risks moving the same bug across the host/guest boundary.

## Validation target for a future patch

A valid fix should show, for HK Galaxy:

- `kernelbase!RaiseException(0xe06d7363, 1, 4, args)` does not return to `Galaxy64.dll+0x831682`.
- Trace proves entry into guest `RtlRaiseException`/`dispatch_exception`/handler search, or into a correctly constructed guest `KiUserExceptionDispatcher` frame.
- If Galaxy catches the `Error getting address info` exception, the swapchain-creator thread survives past the current `+307s` death point and can proceed toward `GetBuffer`.
- If Galaxy does not catch it, termination should happen through guest unhandled-exception semantics, not HB `UNSUPPORTED_OPCODE reason=INT3` at the throw continuation.
