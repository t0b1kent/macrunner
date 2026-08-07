# ABZU ABBA source audit — 2026-07-13

Scope: source-only; no build or game run. Baseline retained: `e7cca2e3`. Golden was not touched.

## Verdict

The deadlock is real, but the proposed attribution to allocations inside MacRunner
`xtajit64 ThreadInit` is contradicted by the sealed ordering: that call has already
returned (`ThreadInit status=0`) before worker `0xa8` waits for the process heap.
The first source-proven loader-to-heap edge is common Wine initialization:

```
worker 0xa8
  LdrInitializeThunk
    loader_init                         loader.c:9404
      RtlEnterCriticalSection(loader_section)  loader.c:9415
      macrunner_hb_xtajit64_unix_thread_init   loader.c:9587-9593 (returns)
      fls_alloc_data                    loader.c:9623
        RtlAllocateHeap(GetProcessHeap) thread.c:545-556
```

The same held loader lock later covers `alloc_thread_tls()`
(`loader.c:9689`, implementation `loader.c:5216`, heap call at 5222) and
`thread_attach()` / `DLL_THREAD_ATTACH` (`loader.c:9691-9692`). It is released
only at `loader.c:9695`. Therefore moving only `ThreadInit` or only
`fls_alloc_data` cannot remove the loader-to-heap lock family.

## Main-thread edge

Sealed executable disassembly identifies the guest chain:

```
0x14006b640 static initializer
  allocation call 0x140918000 (returns at 0x14006b64e)
  constructor 0x140598690
    KERNEL32!CreateThread via IAT 0x141ebf360
      entry 0x14059d330 -> 0x1405b0950
```

Thus the visible guest allocation returns before `CreateThread`; this source path
does not intentionally retain the process-heap critical section across thread
creation. Wine's `RtlAllocateHeap` also enters/exits the heap lock locally at
`heap.c:2075-2077`. The sealed diagnostic proves that `0xa4` owns the heap lock,
but neither the checkpoint nor source-only analysis identifies its exact acquire
caller. Claiming that exact line would be speculation; a missed/corrupted release
on the translated/mixed-view heap path remains possible.

MacRunner nevertheless changes the lifecycle at a concrete point. `_initterm` is
executing the initializer through a nested `macrunner_hb_run_x64`
(`unix/macrunner_hb.c:19504-19596`). The CreateThread semantic then bypasses
kernelbase and performs `NtCreateThreadEx(CREATE_SUSPENDED)` plus immediate resume
inside that nested callback (`macrunner_hb.c:21351-21488`; dispatch at 26595).
Native Wine instead uses `kernelbase/thread.c:86-174`, including activation-context
propagation, before resume. `RtlCreateUserThread` (`ntdll/thread.c:284-331`) is not
this call path.

## ARM64EC/FEX comparison

Wine ARM64EC also invokes `arm64ec_thread_init()` while holding `loader_section`
(`loader.c:9584-9585`; callback wrapper `signal_arm64ec.c:338-345`). The local FEX
mirror initializes its thread under `ThreadCreationMutex` and uses `VirtualAlloc`,
context creation and C++ allocations (`cache/FEX/Source/Windows/ARM64EC/Module.cpp:916-965`).
After return, Wine still takes the process heap for FLS/TLS. Hence loader-to-runtime
initialization is not uniquely MacRunner; the unique local deviation is the manual
CreateThread semantic and its resume timing/bypass of kernelbase.

## Minimal fix candidate

At `unix/macrunner_hb.c:21351`, for **same-process `CreateThread` only**, return
`FALSE` when the semantic thunk has a callable native ARM64/ARM64EC builtin target.
The existing fallback at `macrunner_hb.c:26612` then invokes the real kernelbase
export through `macrunner_hb_call_arm64_pe_import12_for_ctx`. Keep the manual
semantic as fallback when no valid native target exists. Do not blanket-disable
`CreateRemoteThread{,Ex}` without a separate cross-process/attribute audit.

Why this is minimal: it deletes the MacRunner-specific lifecycle substitution from
the trigger while retaining the existing HB entry routing
(`macrunner_hb_BaseThreadInitThunk`, `loader.c:469-506`) and Wine's native activation
context/thread bootstrap. No loader or heap lock is weakened.

Risk: **medium**. The gate must prove `thunk->target` is the native ARM64/ARM64EC
builtin, not an x64/disk view. Handle/TID/argument and suspended/resume behavior
need family regression coverage. Because the exact main heap-acquire caller is not
sealed, this is a strongly evidenced minimal correction, not yet proof that it is
sufficient. If it does not remove the ABBA, the next valid probe is heap-CS
enter/leave caller capture on `0xa4`; moving FLS/TLS outside the loader lock is not
a safe fallback.

## Acceptance for the later authorized run

- no reciprocal `blocked by` pair for `0xa4`/`0xa8`;
- worker completes loader initialization and reaches guest `0x1405b0950`;
- main returns past the nested thread initializer;
- no downstream change is credited unless the run contract is complete.

