/* ============================================================================
 * MacRunner — wait/signal review bundle for ChatGPT (2026-06-02)
 *
 * WHAT THIS IS
 * ------------
 * Extracted, self-contained slices of our ntdll-unix shim
 * (engine/wine/dlls/ntdll/unix/macrunner_hb.c). This shim intercepts Win32/NT
 * imports from an x86-64 guest (running under our x86->ARM64 binary translator)
 * and re-implements or forwards them to the HOST's ARM64 Wine ntdll.
 *
 * THE SYSTEM (so the code makes sense)
 * ------------------------------------
 * - An unmodified x64 Windows game (Hollow Knight, Unity + Mono) runs as
 *   emulated x86-64. System DLLs are NATIVE ARM64 Wine. When the guest calls an
 *   imported function, control reaches a "thunk" dispatcher; the functions below
 *   are part of that dispatcher (`*_try_*` return TRUE if they handled the call).
 * - `hb_context_t *ctx` = the emulated CPU state (ctx->regs.x64.rsp, ctx->pc...).
 * - `args[]` = the guest's call arguments already marshalled into host values.
 * - `*ret` = the value to hand back to the guest as the return value.
 * - Host primitives we call are REAL ARM64 Wine ntdll:
 *   NtWaitForAlertByThreadId / NtAlertThreadByThreadId /
 *   NtAlertMultipleThreadByThreadId (thread-id alert/futex primitives),
 *   NtWaitForSingleObject / NtWaitForMultipleObjects, InterlockedCompareExchange.
 * - We implement WaitOnAddress / WakeByAddressSingle / WakeByAddressAll
 *   (the Win32 futex API) OURSELVES via a hashed set of per-address wait queues,
 *   parking threads with NtWaitForAlertByThreadId and waking them by thread id.
 *   Mono and Unity's lock-free code use this API heavily during startup.
 *
 * THE PROBLEM WE NEED HELP WITH
 * -----------------------------
 * Running Hollow Knight, the guest MAIN thread parks before the graphics device
 * is ever created (D3D11CreateDevice is never reached). Observed at runtime:
 *   - main thread: a ZERO-timeout poll loop (spins, never progresses)
 *   - worker threads: INFINITE waits that never return
 *   - net effect: a classic "nobody ever wakes me" stall.
 * Unity's normal boot has the main thread hand work to a render/worker thread and
 * wait on a signal; if our wait/wake layer ever LOSES a wakeup, this is exactly
 * the symptom.
 *
 * WHAT TO REVIEW (be concrete, cite line numbers in THIS file)
 * ------------------------------------------------------------
 * 1. LOST-WAKEUP / RACE: In macrunner_hb_rtl_wait_on_address, is there any window
 *    where a concurrent WakeByAddress* can be missed? Consider: compare value ->
 *    add self to queue -> unlock -> NtWaitForAlertByThreadId. If a waker runs
 *    AFTER our compare but BEFORE we block, do we still wake? (Alerts are
 *    per-thread; does ordering guarantee the alert is pending when we wait, or can
 *    it be consumed/dropped?) Same for the entry.addr==NULL re-check.
 * 2. The per-address queue is hashed into 256 buckets by (addr>>4). Two distinct
 *    addresses can share a bucket. Wake walks the bucket filtering entry->addr==addr.
 *    Any correctness or fairness bug there? Spurious-wake handling? (WaitOnAddress
 *    callers must re-check, but do we re-loop on spurious return correctly? Note:
 *    we DON'T re-loop here — we return after one wait. Is that correct vs Win32
 *    WaitOnAddress semantics, which require looping until the value changes?)
 * 3. TIMEOUT semantics: zero-timeout vs NULL(infinite) — are we mapping guest
 *    timeouts correctly so an "infinite" wait isn't accidentally a 0-timeout poll
 *    (or vice-versa)? (See macrunner_hb_get_nt_timeout usage.)
 * 4. WaitForSingleObject/MultipleObjects forwarding: any handle/return-value/
 *    error-mapping bug that could make a wait return "signaled" when it isn't, or
 *    spin?
 * 5. Memory ordering: spin_lock/spin_unlock use InterlockedCompareExchange/
 *    InterlockedExchange. Is the lock release ordered correctly w.r.t. the queue
 *    mutation and the value compare? Any missing barrier that lets a waker observe
 *    a stale empty queue?
 *
 * Give: each finding as [CONFIRMED BUG] / [LIKELY] / [STYLE], the exact race
 * interleaving (thread A vs thread B timeline), and the minimal fix. If the code
 * is correct, say so and explain WHY the lost-wakeup symptom must be elsewhere
 * (e.g. the wake is never CALLED because the guest never reaches it).
 *
 * NOTE: helper functions referenced but not included (assume correct, standard):
 *   macrunner_hb_strieq (case-insensitive compare), macrunner_hb_get_nt_timeout
 *   (DWORD ms -> LARGE_INTEGER* relative timeout, returns NULL for INFINITE),
 *   macrunner_hb_trace_* (logging only, no logic), list_* (Wine intrusive list),
 *   hb_memory_read (copy guest memory -> host buffer).
 * ============================================================================ */

/* ---- helpers / logging are elided; see header note ---- */

/* ==== SLICE 1: macrunner_hb.c lines ~10474-10755 (WaitOnAddress impl) ==== */
struct macrunner_hb_wait_addr_entry
{
    struct list entry;
    const void *addr;
    DWORD tid;
};

struct macrunner_hb_wait_addr_queue
{
    struct list queue;
    LONG lock;
};

static struct macrunner_hb_wait_addr_queue macrunner_hb_wait_addr_queues[256];

static struct macrunner_hb_wait_addr_queue *macrunner_hb_get_wait_addr_queue( const void *addr )
{
    ULONG_PTR val = (ULONG_PTR)addr;

    return &macrunner_hb_wait_addr_queues[(val >> 4) % ARRAY_SIZE(macrunner_hb_wait_addr_queues)];
}

static void macrunner_hb_wait_addr_spin_lock( LONG *lock )
{
    while (InterlockedCompareExchange( lock, -1, 0 ))
        YieldProcessor();
}

static void macrunner_hb_wait_addr_spin_unlock( LONG *lock )
{
    InterlockedExchange( lock, 0 );
}

static BOOL macrunner_hb_compare_wait_addr( const void *addr, const void *cmp, SIZE_T size )
{
    if (!addr || !cmp) return FALSE;

    switch (size)
    {
    case 1:
        return *(const UCHAR *)addr == *(const UCHAR *)cmp;
    case 2:
        return *(const USHORT *)addr == *(const USHORT *)cmp;
    case 4:
        return *(const ULONG *)addr == *(const ULONG *)cmp;
    case 8:
        return *(const ULONG64 *)addr == *(const ULONG64 *)cmp;
    default:
        return FALSE;
    }
}

static NTSTATUS macrunner_hb_rtl_wait_on_address( const void *addr, const void *cmp, SIZE_T size,
                                                  const LARGE_INTEGER *timeout )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry entry;
    NTSTATUS status;

    if (size != 1 && size != 2 && size != 4 && size != 8)
        return STATUS_INVALID_PARAMETER;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    entry.addr = addr;
    entry.tid = GetCurrentThreadId();

    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!macrunner_hb_compare_wait_addr( addr, cmp, size ))
    {
        macrunner_hb_wait_addr_spin_unlock( &queue->lock );
        return STATUS_SUCCESS;
    }

    if (!queue->queue.next)
        list_init( &queue->queue );
    list_add_tail( &queue->queue, &entry.entry );
    macrunner_hb_wait_addr_spin_unlock( &queue->lock );

    status = NtWaitForAlertByThreadId( NULL, timeout );

    if (entry.addr)
    {
        macrunner_hb_wait_addr_spin_lock( &queue->lock );
        if (entry.addr)
            list_remove( &entry.entry );
        macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    }

    return status == STATUS_ALERTED ? STATUS_SUCCESS : status;
}

static void macrunner_hb_rtl_wake_address_all( const void *addr )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry *entry, *next;
    unsigned int count = 0;
    HANDLE tids[256];

    if (!addr) return;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!queue->queue.next)
        list_init( &queue->queue );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &queue->queue, struct macrunner_hb_wait_addr_entry, entry )
    {
        if (entry->addr == addr)
        {
            entry->addr = NULL;
            list_remove( &entry->entry );
            if (count == ARRAY_SIZE(tids))
            {
                NtAlertMultipleThreadByThreadId( tids, count, NULL, NULL );
                count = 0;
            }
            tids[count++] = ULongToHandle( entry->tid );
        }
    }

    macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    if (count)
        NtAlertMultipleThreadByThreadId( tids, count, NULL, NULL );
}

static void macrunner_hb_rtl_wake_address_single( const void *addr )
{
    struct macrunner_hb_wait_addr_queue *queue;
    struct macrunner_hb_wait_addr_entry *entry;
    DWORD tid = 0;

    if (!addr) return;

    queue = macrunner_hb_get_wait_addr_queue( addr );
    macrunner_hb_wait_addr_spin_lock( &queue->lock );
    if (!queue->queue.next)
        list_init( &queue->queue );

    LIST_FOR_EACH_ENTRY( entry, &queue->queue, struct macrunner_hb_wait_addr_entry, entry )
    {
        if (entry->addr == addr)
        {
            tid = entry->tid;
            entry->addr = NULL;
            list_remove( &entry->entry );
            break;
        }
    }

    macrunner_hb_wait_addr_spin_unlock( &queue->lock );
    if (tid)
        NtAlertThreadByThreadId( ULongToHandle( tid ) );
}

static BOOL macrunner_hb_try_wait_address_semantic( hb_context_t *ctx,
                                                    const struct macrunner_hb_import_thunk *thunk,
                                                    const uint64_t args[MACRUNNER_HB_IMPORT_ARG_MAX],
                                                    uint64_t *ret )
{
    LARGE_INTEGER timeout;
    LARGE_INTEGER *timeout_ptr = NULL;
    NTSTATUS status;

    if (!ctx || !thunk || !args || !ret) return FALSE;

    if (macrunner_hb_strieq( thunk->import_name, "WakeByAddressAll" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressAll" ))
    {
        macrunner_hb_rtl_wake_address_all( (const void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wake import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p mode=all\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0] );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WakeByAddressSingle" ) ||
        macrunner_hb_strieq( thunk->import_name, "RtlWakeAddressSingle" ))
    {
        macrunner_hb_rtl_wake_address_single( (const void *)(uintptr_t)args[0] );
        NtCurrentTeb()->LastStatusValue = STATUS_SUCCESS;
        *ret = 0;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wake import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p mode=single\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0] );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WaitOnAddress" ))
    {
        timeout_ptr = macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[3] );
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait-before import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout_ms=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (unsigned long)(DWORD)args[3] );
            fflush( stderr );
        }
        status = macrunner_hb_rtl_wait_on_address( (const void *)(uintptr_t)args[0],
                                                   (const void *)(uintptr_t)args[1],
                                                   (SIZE_T)args[2], timeout_ptr );
        NtCurrentTeb()->LastStatusValue = status;
        if (status == STATUS_SUCCESS)
        {
            NtCurrentTeb()->LastErrorValue = 0;
            *ret = 1;
        }
        else
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = 0;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout_ms=%lu "
                     "status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (unsigned long)(DWORD)args[3], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "RtlWaitOnAddress" ))
    {
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait-before import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout=%p\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (void *)(uintptr_t)args[3] );
            fflush( stderr );
        }
        status = macrunner_hb_rtl_wait_on_address( (const void *)(uintptr_t)args[0],
                                                   (const void *)(uintptr_t)args[1],
                                                   (SIZE_T)args[2],
                                                   (const LARGE_INTEGER *)(uintptr_t)args[3] );
        NtCurrentTeb()->LastStatusValue = status;
        *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: address-wait import=%s!%s pc=%p "
                     "caller=%p rsp=%p addr=%p cmp=%p size=%zu timeout=%p status=%08lx\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller, (void *)(uintptr_t)ctx->regs.x64.rsp,
                     (void *)(uintptr_t)args[0], (void *)(uintptr_t)args[1],
                     (size_t)args[2], (void *)(uintptr_t)args[3], (unsigned long)status );
            fflush( stderr );
        }
        return TRUE;
    }

    return FALSE;
}

/* ==== SLICE 2: macrunner_hb.c lines ~12928-13034 (WaitForSingleObject / WaitForMultipleObjects dispatch) ==== */

    if (macrunner_hb_strieq( thunk->import_name, "WaitForSingleObject" ) ||
        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ))
    {
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: before import=%s!%s pc=%p caller=%p rsp=%p "
                     "handle=%p timeout_ms=%lu alertable=%u\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)(DWORD)args[1],
                     (unsigned int)(macrunner_hb_strieq( thunk->import_name,
                                                         "WaitForSingleObjectEx" ) && args[2]) );
            fflush( stderr );
        }
        status = NtWaitForSingleObject( (HANDLE)(uintptr_t)args[0],
                                        macrunner_hb_strieq( thunk->import_name, "WaitForSingleObjectEx" ) &&
                                        args[2],
                                        macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[1] ) );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = WAIT_FAILED;
        }
        else *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: after import=%s!%s pc=%p caller=%p rsp=%p "
                     "handle=%p timeout_ms=%lu status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (void *)(uintptr_t)args[0],
                     (unsigned long)(DWORD)args[1], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

    if (macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjects" ) ||
        macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ))
    {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        DWORD count = (DWORD)args[0];

        if (!count || count > MAXIMUM_WAIT_OBJECTS ||
            hb_memory_read( ctx->memory, (hb_gva_t)args[1], handles, count * sizeof(handles[0]) ) != HB_OK)
        {
            if (macrunner_hb_trace_wait_semantic_budget_allows())
            {
                fprintf( stderr, "macrunner-hb-wait-semantic: invalid import=%s!%s pc=%p rsp=%p "
                         "count=%lu handles_gva=%p wait_all=%u timeout_ms=%lu\n",
                         thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                         (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                         (void *)(uintptr_t)args[1], (unsigned int)args[2],
                         (unsigned long)(DWORD)args[3] );
                fflush( stderr );
            }
            RtlSetLastWin32Error( ERROR_INVALID_PARAMETER );
            NtCurrentTeb()->LastStatusValue = STATUS_INVALID_PARAMETER;
            *ret = WAIT_FAILED;
            return TRUE;
        }
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: before import=%s!%s pc=%p caller=%p rsp=%p "
                     "count=%lu handles_gva=%p handles=%p,%p,%p,%p wait_all=%u timeout_ms=%lu alertable=%u\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                     (void *)(uintptr_t)args[1], count > 0 ? handles[0] : NULL,
                     count > 1 ? handles[1] : NULL, count > 2 ? handles[2] : NULL,
                     count > 3 ? handles[3] : NULL, (unsigned int)args[2],
                     (unsigned long)(DWORD)args[3],
                     (unsigned int)(macrunner_hb_strieq( thunk->import_name,
                                                         "WaitForMultipleObjectsEx" ) && args[4]) );
            fflush( stderr );
        }
        status = NtWaitForMultipleObjects( count, handles, args[2] ? WaitAll : WaitAny,
                                           macrunner_hb_strieq( thunk->import_name, "WaitForMultipleObjectsEx" ) &&
                                           args[4],
                                           macrunner_hb_get_nt_timeout( &timeout, (DWORD)args[3] ) );
        NtCurrentTeb()->LastStatusValue = status;
        if (NT_ERROR( status ))
        {
            RtlSetLastWin32Error( RtlNtStatusToDosError( status ) );
            *ret = WAIT_FAILED;
        }
        else *ret = status;
        if (macrunner_hb_trace_wait_semantic_budget_allows())
        {
            uint64_t caller = macrunner_hb_trace_return_address( ctx );
            fprintf( stderr, "macrunner-hb-wait-semantic: after import=%s!%s pc=%p caller=%p rsp=%p "
                     "count=%lu wait_all=%u timeout_ms=%lu status=%08lx ret=%p last_error=%lu\n",
                     thunk->dll_name, thunk->import_name, (void *)(uintptr_t)ctx->pc,
                     (void *)(uintptr_t)caller,
                     (void *)(uintptr_t)ctx->regs.x64.rsp, (unsigned long)count,
                     (unsigned int)args[2], (unsigned long)(DWORD)args[3], (unsigned long)status,
                     (void *)(uintptr_t)*ret, (unsigned long)NtCurrentTeb()->LastErrorValue );
            fflush( stderr );
        }
        return TRUE;
    }

/* ==== END OF BUNDLE ==== */
