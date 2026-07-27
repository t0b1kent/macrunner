# The guest icache-flush callback reaches a no-op stub

Date: 2026-07-27. Coordinator code audit (offline, no run consumed).
Question posed by the prior-art brief: does a guest `NtFlushInstructionCache` actually reach
our translation-cache invalidation?

**Answer: no. The chain is wired correctly right up to the last step, where it does nothing.**

## The chain, link by link

1. Wine's ARM64EC syscall wrapper does the right thing —
   `engine/wine/dlls/ntdll/signal_arm64ec.c:771`:

```c
NTSTATUS SYSCALL_API NtFlushInstructionCache( HANDLE process, const void *addr, SIZE_T size )
{
    NTSTATUS status = syscall_NtFlushInstructionCache( process, addr, size );
    if (!status && enter_syscall_callback())
    {
        if (!RtlIsCurrentProcess( process ))
            send_cross_process_notification( process, CrossProcessFlushCache, addr, size, 0 );
        else if (pBTCpu64FlushInstructionCache)
            pBTCpu64FlushInstructionCache( addr, size );      /* <- correct: range is passed */
        leave_syscall_callback();
    }
    return status;
}
```

   There is a second call site at `signal_arm64ec.c:1094-1095` for the cross-process
   notification path. Both pass `addr` and `size`.

2. Our binary translator exports the callback —
   `engine/wine/dlls/xtajit64/xtajit64.spec:1` and `engine/wine/dlls/xtajit64/cpu.c:198`:

```c
void WINAPI BTCpu64FlushInstructionCache( void *addr, SIZE_T size )
{
    TRACE( "%p %Ix\n", addr, size );
    (void)xtajit64_unix_call( unix_flush_instruction_cache, NULL );   /* addr/size DISCARDED */
}
```

   **The range is dropped here.** `NULL` is passed to the unix side, so nothing downstream
   can know which pages were modified. (Same pattern at `cpu.c:253`.)

3. The unix-side handler — `engine/wine/dlls/xtajit64/unixlib.c:710`:

```c
static NTSTATUS unix_flush_instruction_cache_impl( void *args )
{
    return STATUS_SUCCESS;
}
```

   **A no-op stub.** It reports success and invalidates nothing.

4. HyperBridge's invalidation APIs exist but have **no production caller**.
   `hb_cache_invalidate` (`engine/hyperbridge/src/hb_aot_cache.c:452`) and
   `hb_cache_invalidate_module` (`:473`) are referenced only from
   `engine/hyperbridge/tests/hb_test_runner.c:8388,8684` and
   `engine/hyperbridge/cache/hb_test_cache_ext.c:201`. They are also the wrong granularity
   for this job — version and `module_id`, i.e. AOT-cache bookkeeping for module reload, not
   address-range invalidation for self-modifying code.

5. Nothing on the ntdll/HyperBridge side fills the gap either: `macrunner_hb.c` contains no
   match for `hb_cache_invalidate*`, `smc`, or `self_modif*`.

6. The neighbouring notification is not a substitute:
   `unix_notify_memory_protect_impl` (`unixlib.c:633`) just calls `map_native_range(args)` —
   it maps the range, it does not invalidate translations. So an RWX flip does not clean the
   cache either.

## Is this path live for Hollow Knight?

Yes. `xtajit64` appears 286 times in the captured HK run logs (`xtajit64.dll` ×10), so the
translator whose flush callback is a stub is the one loaded in our runs.

## What is proven, and what is not

**Proven (code reading, first-hand):** a guest instruction-cache flush performs no
translation-cache invalidation anywhere in our stack, and the modified address range is
discarded before it could.

**NOT proven, and the lane must establish it:** that Hollow Knight actually re-executes a
stale translation as a result. The gap is certain; the consequence is inference. Deciding it
needs one instrument, not a guess:

> after Mono writes machine code into an executable page, is a *previously translated* block
> covering that page executed again, rather than re-translated?

If yes, the shape matches our symptom exactly and better than the spin does: Mono compiles a
method, the write is invisible to the translator, the guest re-runs the old translation, and
the thread stays alive, on-CPU, and permanently unable to advance — which is what we see
after `Performing automatic level start.`, and what explains why a *finite* ~15 min spin is
followed by *permanent* non-advancement.

This is the same defect class FEX-Emu reported fixing on their Wine path — "handling of
self-modifying code … could fix some spurious hangs or incorrect invalidations" — and the
same one box64 works around by shrinking translation blocks for Mono.

## Suggested fix shape (design note, not a patch)

1. Stop discarding the range in `BTCpu64FlushInstructionCache` — pass `addr`/`size` through
   the unix call.
2. Implement `unix_flush_instruction_cache_impl` to invalidate translations overlapping that
   range.
3. Give HyperBridge an address-range invalidation entry point; the existing
   `hb_cache_invalidate*` pair is module/version-granular and is test-only today.
4. Note that a correct implementation must also handle writes that never call flush at all.
   Windows guests are *supposed* to call `FlushInstructionCache`, and Mono does on ARM — but
   coverage should be verified rather than assumed, which is why step 4 is a question for the
   lane, not a line of code.

**Do not treat any of this as fixed until the ordinal-200 readback goes non-black.**
