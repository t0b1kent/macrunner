# HB EH dispatch Patch C implemented

## Change

Implemented the root-cause fix direction from `docs/HB-EH-DISPATCH-ROOTCAUSE.md` for `kernel32/kernelbase!RaiseException` under HB.

Touched code:

- `engine/wine/dlls/ntdll/unix/macrunner_hb.c`

The old RaiseException semantic shortcut called only HB-stored vectored handlers and then normal-returned through the common import epilogue. For Galaxy C++ EH this resumed at `Galaxy64.dll+0x831682`, the compiler-emitted unreachable `int3` after `_CxxThrowException`.

The new path:

- Generalizes the existing AV delivery frame into `macrunner_hb_deliver_guest_exception_record()`.
- Keeps `macrunner_hb_deliver_guest_access_violation()` as a wrapper around the generic helper.
- Passes `ret_addr` into `macrunner_hb_try_vectored_exception_semantic()`.
- Sets `ExceptionAddress=ret_addr` for `RaiseException`, matching the guest `RtlRaiseException` return-address behavior.
- After vectored handlers return `EXCEPTION_CONTINUE_SEARCH`, builds a guest x64 exception frame and transfers control to guest `ntdll!KiUserExceptionDispatcher`.
- Uses post-call guest context for RaiseException delivery: `Rip=ret_addr`, `Rsp=ctx->regs.x64.rsp + 8`.
- Adds a `control_transferred` flag so `macrunner_hb_call_import_thunk()` skips the common import epilogue when guest EH delivery has already set `ctx->pc` to the dispatcher.
- If guest delivery fails, returns `HB_ERR_EXEC_FAULT` instead of normal-returning to a noncontinuable throw continuation.

## Non-goals

- Did not touch intercepted-indirect-call resume / ABZU `pc=0x60` path.
- Did not modify `hb_runtime.c` or `hb_arm64_codegen.c`.
- Did not run Hollow Knight or any game.
- Did not overwrite lane/main dist artifacts.

## Build

Targeted build completed with rc=0:

```sh
. ./config/env.sh && make -C engine/wine/build-arm64ec-spike dlls/ntdll/ntdll.so > artifacts/hb-eh-dispatch-patchc-ntdll-build.log 2>&1
```

SHA is recorded in `reports/phase4-hollow-knight/PATCHC-BUILD-SHA.txt`.

## Validation plan

Future run should enable the PATCHC `ntdll.so` artifact in the HK lane dist and check:

- Galaxy `RaiseException(0xe06d7363, EXCEPTION_NONCONTINUABLE, 4, args)` no longer returns to `Galaxy64.dll+0x831682`.
- Trace shows guest `KiUserExceptionDispatcher` / `dispatch_exception` frame-walk instead of HB `UNSUPPORTED_OPCODE reason=INT3` at the throw continuation.
- If Galaxy catches the `Error getting address info` exception, the swapchain-creator thread survives past the previous `+307s` death point.
- If Galaxy does not catch it, termination happens through guest unhandled-exception semantics, not through the unreachable `int3` tail.
