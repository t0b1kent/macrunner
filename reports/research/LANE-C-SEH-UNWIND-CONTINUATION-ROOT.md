# Lane C SEH unwind continuation root

Date: 2026-06-05

Constraint: no new Wine, HK, or heavy PE32 runs were executed for this note. This
is a consolidation of existing Lane C logs and static source anchors only.

## Verdict

The repeated tail is not a Lane C loader, `virtual.c`, or real-DLL API gap. It is
a Lane A-owned ARM64EC/HyperBridge SEH continuation contract failure at the
boundary between x64 guest execution/callback thunks and ARM64 Wine SEH unwind.

Owner files:

- Primary consumer/fix point: `engine/wine/dlls/ntdll/signal_arm64.c`
- Producer/contract partner: `engine/wine/dlls/ntdll/unix/macrunner_hb.c`
- Not indicated by current evidence: `engine/wine/dlls/ntdll/unix/virtual.c`,
  `engine/wine/dlls/ntdll/unix/loader.c`, ordinary `engine/wine/dlls/**` bodies.

Lane C mission ownership explicitly forbids editing `macrunner_hb.c`,
`signal_arm64.c`, or `engine/hyperbridge/**`; route the source patch to Lane A.

## Repeating evidence

Existing logs show the same terminal family across minimal x64, WOW64, GUI, and
HK-adjacent probes:

- 2026-06-06: Focused re-run of extracted `Diablo.exe` and `Terraria.exe`
  (`reports/lane-c-real-software-matrix-fresh-ict/summary.tsv`) stays in the same
  boundary-tail class: both reach `macrunner-hb-ldr-init: phase=after-loader` with
  zero `missing_import`, `unsupported`, `vm_fail`, and `dll_fail`, and then loop
  on in-module continuation `pc=0000087FFF80C374 lr=0000087FFF80C378 image=0000087FFF7A0000 function=0`
  (`virtual_unwind macrunner-hb-seh-invalid: reason=unwind-metadata-missing` +
  `RtlRaiseStatus status=c0000026`).

- `reports/lane-c-cli-hello-x64-20260604095922/summary.txt`: `missing_imports=0`,
  `fixme_stub=0`, `hb_runtime_fail=0`, then repeated
  `macrunner-hb-seh-invalid: reason=unwind-metadata-missing` followed by
  `RtlRaiseStatus status=c0000026` and `EXCEPTION_INVALID_DISPOSITION`.
- `reports/lane-c-wow64-memory-post-arm64-id-reg-20260604095647/summary.txt`:
  `wow64=0`, `missing_imports=0`, `hb_runtime_fail=0`; the first real tail is
  SEH `unwind-metadata-missing`, not WOW64 guest execution.
- `reports/lane-c-hollow-knight-post-setupapi-20260604111854/summary.txt`:
  `d3d_runtime_reached=no`, `missing_imports=0`, `unsupported=0`,
  `fixme_stub=0`, `vm_fail=0`, `hb_runtime_fail=0`; terminal blocker is repeated
  `virtual_unwind exception data not found` / `EXCEPTION_INVALID_DISPOSITION`.
- `reports/lane-c-pe32-api-breadth-20260605085530/analysis.txt`: x86 fixtures
  that reach WOW64/BTCpu have all Lane C markers zero; other bounded fixtures
  either stop before handoff or repeat the already triaged unwind tail.
- `reports/lane-c-dialog-shell-breadth-20260605144125/analysis.txt` and
  `reports/lane-c-x64-realapp-breadth-20260605142245/analysis.txt` classify
  `LANE_C_FIXABLE_GAPS=none`, route `seh_tail=64` to A/B, and show the canonical
  continuation form:
  `pc=FFFFFFFFFFFFFFFC lr=0000000000000000 image=0000000000000000 function=0000000000000000`.

## Where it breaks

The visible failure is inside `engine/wine/dlls/ntdll/signal_arm64.c`:

- `virtual_unwind()` takes `pc = context->Pc`.
- If `CONTEXT_UNWOUND_TO_CALL` is set, it decrements `pc` by 4 before lookup.
- It has explicit MacRunner boundary stops for import thunks, Unix dispatcher
  boundaries, and non-module host boundary PCs.
- Otherwise it calls `RtlLookupFunctionEntry()` and `RtlVirtualUnwind2()`.
- On bad unwind metadata it logs
  `macrunner-hb-seh-invalid: reason=unwind-metadata-missing ...` and returns
  `STATUS_INVALID_DISPOSITION`.

The `FFFFFFFFFFFFFFFC` form is therefore diagnostic: the unwind loop is seeing a
zero continuation PC with `CONTEXT_UNWOUND_TO_CALL` already set, then subtracting
4 and trying to unwind `-4` as code. Since `image=0` and `function=0`, this is
not a loaded module missing unwind data; it is a missing/sentinel continuation
contract at the x64-to-ARM64 boundary.

There is an earlier related form in x64 CLI/WOW64 setup:
`pc=000007FFD0796640 lr=000007FFD0796644 image=000007FFD0720000 function=0`.
That shows ARM64 code in an ntdll image without a function entry, then the same
`c0000026` loop. The later `-4` form is the stronger continuation sentinel
failure and explains the recurring tail after prior Lane C fixes removed loader,
VM, setupapi, and DLL-body markers.

## Producer side

`engine/wine/dlls/ntdll/unix/macrunner_hb.c` owns the x64 guest/callback
producer side:

- `macrunner_hb_dispatch_x64_callback()` rejects non-x64 callback targets in
  Phase F and dispatches valid x64 callbacks through `macrunner_hb_run_x64()`.
- `macrunner_hb_run_x64()` creates a HyperBridge x64 context, installs a bridge
  stack into the TEB, runs the guest, and restores the original TEB stack.
- HyperBridge ABI setup (`engine/hyperbridge/src/hb_abi_x64.c`) writes the x64
  synthetic return-address sentinel `0xFFFF0000`.
- `macrunner_hb_run_x64()` treats `ctx->pc == 0xffff0000` as a successful guest
  return.

The SEH failure is not that x64 guest return sentinel itself. The failing unwind
PC is ARM64-side `0`/`-4`, meaning the ARM64 SEH continuation that should stop
unwind at the HyperBridge boundary is absent or not recognized.

## Fix hypothesis

Lane A should close the boundary contract in `signal_arm64.c` and
`macrunner_hb.c`, not patch Lane C DLL bodies:

1. In `signal_arm64.c`, add an explicit MacRunner stop for empty/unwound host
   continuation before `RtlLookupFunctionEntry()`:
   if the process is an x64 HyperBridge main process and `context->Pc == 0`
   or the adjusted `pc == ~(DWORD64)3` with `lr == 0`, call
   `macrunner_hb_stop_unwind_at_boundary()` and return `STATUS_SUCCESS`.
   Gate this narrowly to MacRunner/HyperBridge contexts so ordinary ARM64 Wine
   SEH still reports real invalid dispositions.
2. In `macrunner_hb.c`, audit the callback/thread/dll entry return path so every
   x64 guest-to-host callback leaves a recognizable ARM64 unwind boundary
   sentinel, not a zero PC/LR pair. If a real boundary PC exists, expose it
   through the existing `macrunner_hb_is_*_boundary_pc()` predicates instead of
   special-casing random module ranges.
3. Add a focused regression that exercises x64 callback/SEH unwinding without HK:
   trigger an exception across `macrunner_hb_dispatch_x64_callback()` and assert
   the unwind stops at the HyperBridge boundary, with no
   `unwind-metadata-missing`, no `RtlRaiseStatus c0000026`, and no `pc=-4`.

## Phase 4 routing rule

Do not resume Phase 4/HK validation while Lane A is running HK. When CPU is free,
each VM-related commit must be followed by the HK after-loader smoke before it is
treated as a stable checkpoint.
