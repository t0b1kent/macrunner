/*
 * ARM64 signal handling routines
 *
 * Copyright 2010-2013 André Hentschel
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef __aarch64__

#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <setjmp.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "wine/exception.h"
#include "ntdll_misc.h"
#include "wine/debug.h"
#include "ntsyscalls.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);
WINE_DECLARE_DEBUG_CHANNEL(relay);

#define MACRUNNER_HB_IMPORT_BASE 0x00006f0000000000ULL
#define MACRUNNER_HB_IMPORT_LIMIT (MACRUNNER_HB_IMPORT_BASE + 4096 * 0x10ULL)
#define MACRUNNER_HB_UNIX_DISPATCHER_WINDOW 0x20000ULL
#define MACRUNNER_HB_HOST_BOUNDARY_MIN 0x0000000100000000ULL
#define MACRUNNER_HB_HOST_BOUNDARY_MAX 0x0000008000000000ULL
#define MACRUNNER_HB_SYSCALL_FRAME_SIZE 0x330ULL

extern void *__wine_syscall_dispatcher;

static const EXCEPTION_RECORD *macrunner_hb_current_exception_record;

struct macrunner_hb_syscall_frame
{
    ULONG64 x[29];
    ULONG64 fp;
    ULONG64 lr;
    ULONG64 sp;
    ULONG64 pc;
    ULONG cpsr;
    ULONG restore_flags;
    struct macrunner_hb_syscall_frame *prev_frame;
    void *syscall_cfa;
};

static inline struct macrunner_hb_syscall_frame *macrunner_hb_current_syscall_frame(void)
{
    TEB *teb = NtCurrentTeb();

    if (!teb) return NULL;
    return *(struct macrunner_hb_syscall_frame **)((char *)teb + 0x378);
}

static inline BOOL macrunner_hb_is_import_thunk_pc( DWORD64 pc )
{
    return pc >= MACRUNNER_HB_IMPORT_BASE && pc < MACRUNNER_HB_IMPORT_LIMIT;
}

static inline DWORD64 macrunner_hb_normalize_arm64ec_host_pc( DWORD64 pc )
{
    if ((pc & 1) && pc >= MACRUNNER_HB_HOST_BOUNDARY_MIN && pc < MACRUNNER_HB_HOST_BOUNDARY_MAX)
        return pc & ~(DWORD64)1;
    return pc;
}

static inline BOOL macrunner_hb_is_x64_main_process(void)
{
    TEB *teb = NtCurrentTeb();
    IMAGE_NT_HEADERS *nt;

    if (!teb || teb->WowTebOffset || !teb->Peb || !teb->Peb->ImageBaseAddress)
        return FALSE;
    if (!(nt = RtlImageNtHeader( teb->Peb->ImageBaseAddress )))
        return FALSE;
    return nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ||
           nt->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64EC;
}

static inline BOOL macrunner_hb_near_unix_dispatcher( DWORD64 pc, const void *dispatcher )
{
    ULONG_PTR center = (ULONG_PTR)dispatcher;

    if (!center) return FALSE;
    if (center > MACRUNNER_HB_UNIX_DISPATCHER_WINDOW &&
        pc >= center - MACRUNNER_HB_UNIX_DISPATCHER_WINDOW &&
        pc < center + MACRUNNER_HB_UNIX_DISPATCHER_WINDOW)
        return TRUE;
    return pc >= center && pc < center + MACRUNNER_HB_UNIX_DISPATCHER_WINDOW;
}

static BOOL macrunner_hb_trace_arm64_seh_invalid_disposition(void)
{
    static unsigned int count;
    return count++ < 64;
}

static LONG CALLBACK macrunner_hb_pe_scan_fault( EXCEPTION_POINTERS *ep );
static BOOL macrunner_hb_find_pc_section( DWORD64 pc, char section_name[9],
                                          DWORD *section_characteristics );
static int macrunner_hb_arm64x_code_range_kind( ULONG_PTR base, DWORD64 pc );

static inline BOOL macrunner_hb_is_unix_dispatcher_boundary_pc( DWORD64 pc )
{
    if (!macrunner_hb_is_x64_main_process()) return FALSE;
    return macrunner_hb_near_unix_dispatcher( pc, __wine_syscall_dispatcher ) ||
           macrunner_hb_near_unix_dispatcher( pc, (const void *)__wine_unix_call_dispatcher );
}

static inline BOOL macrunner_hb_is_non_module_host_boundary_pc( DWORD64 pc )
{
    LDR_DATA_TABLE_ENTRY *module;

    if (!macrunner_hb_is_x64_main_process()) return FALSE;
    if (pc < MACRUNNER_HB_HOST_BOUNDARY_MIN || pc >= MACRUNNER_HB_HOST_BOUNDARY_MAX)
        return FALSE;
    return LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module ) != STATUS_SUCCESS;
}

static inline BOOL macrunner_hb_is_null_lr_boundary( DWORD64 pc, const CONTEXT *context )
{
    if (!macrunner_hb_is_x64_main_process() || !context) return FALSE;
    if (context->Lr) return FALSE;
    return pc == 0 || pc == ~(DWORD64)3;
}

static inline BOOL macrunner_hb_is_host_boundary_pc( DWORD64 pc, const CONTEXT *context )
{
    if (!macrunner_hb_is_x64_main_process()) return FALSE;
    if (!context) return FALSE;
    return pc == 0 || (context->Lr == 0 && (pc == ~(DWORD64)3 || pc == 0));
}

static inline BOOL macrunner_hb_current_exception_is_datatype_misalignment(void)
{
    const EXCEPTION_RECORD *rec = macrunner_hb_current_exception_record;

    return rec && rec->ExceptionCode == STATUS_DATATYPE_MISALIGNMENT && !rec->ExceptionFlags;
}

static BOOL macrunner_hb_is_plausible_recovered_pc( DWORD64 pc, const char **reason )
{
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = teb ? (ULONG_PTR)teb->Tib.StackLimit : 0;
    ULONG_PTR stack_hi = teb ? (ULONG_PTR)teb->Tib.StackBase : 0;
    char section_name[9];
    DWORD section_characteristics;

    if (!pc) { if (reason) *reason = "pc-null"; return FALSE; }
    if ((pc & 3) || pc == ~(DWORD64)3) { if (reason) *reason = "pc-unaligned"; return FALSE; }
    if (pc < 0x10000) { if (reason) *reason = "pc-low"; return FALSE; }
    if (stack_lo && stack_hi && pc >= stack_lo && pc < stack_hi)
    {
        if (reason) *reason = "pc-in-stack";
        return FALSE;
    }
    if (macrunner_hb_is_import_thunk_pc( pc )) return TRUE;
    if (pc >= MACRUNNER_HB_HOST_BOUNDARY_MIN &&
        macrunner_hb_find_pc_section( pc, section_name, &section_characteristics ))
    {
        LDR_DATA_TABLE_ENTRY *module = NULL;

        if (!(section_characteristics & IMAGE_SCN_MEM_EXECUTE))
        {
            if (reason) *reason = "pc-nonexec-section";
            return FALSE;
        }
        if (pc < MACRUNNER_HB_HOST_BOUNDARY_MAX)
        {
            if (reason) *reason = "pc-emulator-band";
            return FALSE;
        }
        if (LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module ) == STATUS_SUCCESS &&
            module && macrunner_hb_arm64x_code_range_kind( (ULONG_PTR)module->DllBase, pc ) == 0)
        {
            if (reason) *reason = "pc-x64-code-range";
            return FALSE;
        }
        return TRUE;
    }

    if (reason) *reason = "pc-not-code";
    return FALSE;
}

static BOOL macrunner_hb_find_pc_section( DWORD64 pc, char section_name[9],
                                          DWORD *section_characteristics )
{
    LDR_DATA_TABLE_ENTRY *module = NULL;
    IMAGE_NT_HEADERS *nt;
    ULONG_PTR base, rva;
    IMAGE_SECTION_HEADER *sec;
    WORD i;

    if (section_name) memset( section_name, 0, 9 );
    if (section_characteristics) *section_characteristics = 0;
    if (!pc) return FALSE;
    if (LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module ) != STATUS_SUCCESS || !module)
        return FALSE;

    __TRY
    {
        base = (ULONG_PTR)module->DllBase;
        nt = RtlImageNtHeader( module->DllBase );
        if (!base || !nt || pc < base || pc >= base + nt->OptionalHeader.SizeOfImage) return FALSE;
        rva = (ULONG_PTR)pc - base;
        sec = IMAGE_FIRST_SECTION( nt );
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
        {
            ULONG_PTR start = sec->VirtualAddress;
            ULONG_PTR end = start + max( sec->Misc.VirtualSize, sec->SizeOfRawData );

            if (!start || rva < start || rva >= end) continue;
            if (section_name) memcpy( section_name, sec->Name, 8 );
            if (section_characteristics) *section_characteristics = sec->Characteristics;
            return TRUE;
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY
    return FALSE;
}

static inline void macrunner_hb_stop_unwind_at_boundary( DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    dispatch->ImageBase = 0;
    dispatch->FunctionEntry = NULL;
    dispatch->HandlerData = NULL;
    dispatch->EstablisherFrame = 0;
    dispatch->LanguageHandler = NULL;
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
}

static inline BOOL macrunner_hb_unwind_leaf_via_lr( DISPATCHER_CONTEXT *dispatch, CONTEXT *context,
                                                    DWORD64 pc, DWORD64 lr )
{
    if (!lr || lr == pc || lr == pc + 4 || lr == ~(DWORD64)3) return FALSE;

    dispatch->ImageBase = 0;
    dispatch->FunctionEntry = NULL;
    dispatch->HandlerData = NULL;
    dispatch->EstablisherFrame = context->Sp;
    dispatch->LanguageHandler = NULL;
    context->Pc = lr;
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    return TRUE;
}

static inline BOOL macrunner_hb_unwind_made_no_progress( DWORD64 pc, DWORD64 prev_sp,
                                                         DWORD64 prev_fp, const CONTEXT *context )
{
    if (!context) return TRUE;
    if (context->Pc == pc || context->Pc == pc + 4) return TRUE;
    if (context->Sp == prev_sp) return TRUE;
    /* fp staying EQUAL is valid for a frameless function (it never establishes
     * its own frame pointer, so unwinding it leaves the caller's fp untouched);
     * only fp moving strictly BACKWARD (down-stack) signals a corrupt unwind.
     * Using <= here false-rejected genuine native unwinds of frameless DXMT
     * ARM64 frames (sp advanced, pc -> real caller, fp unchanged).
     * fp landing on exactly 0 is the ABI frame-chain terminator (outermost/
     * frameless-leaf frame with no caller fp to report), not a backward jump
     * into bogus memory -- do not flag it as corrupt. */
    if (prev_fp && context->Fp && context->Fp < prev_fp) return TRUE;
    return FALSE;
}

static inline BOOL macrunner_hb_pc_inside_syscall_frame( DWORD64 pc,
                                                         const struct macrunner_hb_syscall_frame *frame )
{
    ULONG_PTR base = (ULONG_PTR)frame;

    return base && pc >= base && pc < base + MACRUNNER_HB_SYSCALL_FRAME_SIZE;
}

static void macrunner_hb_restore_syscall_prev_frame_context( CONTEXT *context,
                                                             const struct macrunner_hb_syscall_frame *prev )
{
    context->Fp = prev->fp;
    context->Lr = prev->lr;
    context->Sp = prev->sp;
    context->Pc = prev->pc;
    context->Cpsr = prev->cpsr;
    memcpy( &context->X19, &prev->x[19], 10 * sizeof(prev->x[0]) );
    context->ContextFlags |= CONTEXT_CONTROL | CONTEXT_INTEGER;
}

static BOOL macrunner_hb_unwind_syscall_data_boundary( DISPATCHER_CONTEXT *dispatch,
                                                       CONTEXT *context,
                                                       struct macrunner_hb_syscall_frame *frame )
{
    struct macrunner_hb_syscall_frame *prev;
    static unsigned int report_count;
    DWORD tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );

    if (!frame || !macrunner_hb_pc_inside_syscall_frame( context->Pc, frame ))
        return FALSE;
    if (!(prev = frame->prev_frame) || !prev->pc || !prev->sp)
        return FALSE;
    if (macrunner_hb_pc_inside_syscall_frame( prev->pc, frame ))
        return FALSE;

    if (report_count++ < 64)
        MESSAGE( "macrunner-hb-seh-syscall-data-boundary: tid=%04lx pc=%p frame=%p "
                 "bad_frame_pc=%p bad_frame_lr=%p prev=%p prev_pc=%p prev_lr=%p "
                 "prev_sp=%p resume=prev-frame\n",
                 tid, (void *)(ULONG_PTR)context->Pc, frame,
                 (void *)(ULONG_PTR)frame->pc, (void *)(ULONG_PTR)frame->lr,
                 prev, (void *)(ULONG_PTR)prev->pc, (void *)(ULONG_PTR)prev->lr,
                 (void *)(ULONG_PTR)prev->sp );

    macrunner_hb_restore_syscall_prev_frame_context( context, prev );
    macrunner_hb_stop_unwind_at_boundary( dispatch, context );
    return TRUE;
}

static BOOL macrunner_hb_recover_native_dispatch_boundary( DISPATCHER_CONTEXT *dispatch,
                                                           CONTEXT *context, DWORD64 pc )
{
    struct macrunner_hb_syscall_frame *frame, *resume;
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = teb ? (ULONG_PTR)teb->Tib.StackLimit : 0;
    ULONG_PTR stack_hi = teb ? (ULONG_PTR)teb->Tib.StackBase : 0;
    const char *bad_pc = NULL, *source = "current-frame";
    static unsigned int report_count, reject_count;
    DWORD tid = teb ? HandleToULong( teb->ClientId.UniqueThread ) : 0;
    LDR_DATA_TABLE_ENTRY *module = NULL;
    IMAGE_NT_HEADERS *nt;
    char section_name[9];
    DWORD section_characteristics = 0;

    if (!context || !macrunner_hb_is_x64_main_process())
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p reason=%s\n",
                     tid, (void *)(ULONG_PTR)pc, NULL,
                     context ? "not-x64-main-process" : "no-context" );
        return FALSE;
    }
    if (!(frame = macrunner_hb_current_syscall_frame()))
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p reason=no-current-syscall-frame\n",
                     tid, (void *)(ULONG_PTR)pc, NULL );
        return FALSE;
    }

    resume = frame->prev_frame;
    if (resume && resume != frame && resume->pc && resume->sp)
        source = "prev-frame";
    else
        resume = frame;

    if (!resume->pc || !resume->sp)
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p frame_pc=%p frame_lr=%p prev=%p source=%s reason=%s\n",
                     tid, (void *)(ULONG_PTR)pc, frame, (void *)(ULONG_PTR)frame->pc,
                     (void *)(ULONG_PTR)frame->lr, frame->prev_frame, source,
                      !resume->pc ? "resume-pc-null" : "resume-sp-null" );
        return FALSE;
    }
    if (resume == frame && !frame->prev_frame &&
        LdrFindEntryForAddress( (void *)(ULONG_PTR)resume->pc, &module ) == STATUS_SUCCESS &&
        module && macrunner_hb_arm64x_code_range_kind( (ULONG_PTR)module->DllBase, resume->pc ) == 0)
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p frame_pc=%p frame_lr=%p prev=%p source=%s "
                     "reason=current-x64-frame-no-prev\n",
                     tid, (void *)(ULONG_PTR)pc, frame, (void *)(ULONG_PTR)frame->pc,
                     (void *)(ULONG_PTR)frame->lr, frame->prev_frame, source );
        return FALSE;
    }
    if (resume != frame && macrunner_hb_pc_inside_syscall_frame( resume->pc, frame ))
        return FALSE;

    if (!macrunner_hb_is_plausible_recovered_pc( resume->pc, &bad_pc ))
    {
        if (!macrunner_hb_find_pc_section( resume->pc, section_name, &section_characteristics ))
            bad_pc = "pc-not-code";
        else if (!(section_characteristics & IMAGE_SCN_MEM_EXECUTE))
            bad_pc = "pc-nonexec-section";
        else if (LdrFindEntryForAddress( (void *)(ULONG_PTR)resume->pc, &module ) != STATUS_SUCCESS ||
                 !module || !(nt = RtlImageNtHeader( module->DllBase )))
            bad_pc = "pc-module-missing";
        else if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ||
                 nt->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64EC)
            bad_pc = NULL;
        else if (bad_pc && !strcmp( bad_pc, "pc-x64-code-range" ))
            bad_pc = NULL;
        else if (bad_pc && strcmp( bad_pc, "pc-x64-code-range" ))
            bad_pc = "pc-not-recoverable-code";
    }

    if (bad_pc)
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p frame_pc=%p frame_lr=%p prev=%p prev_pc=%p "
                     "prev_lr=%p prev_sp=%p source=%s reason=%s\n",
                     tid, (void *)(ULONG_PTR)pc, frame, (void *)(ULONG_PTR)frame->pc,
                     (void *)(ULONG_PTR)frame->lr, frame->prev_frame,
                     (void *)(ULONG_PTR)resume->pc, (void *)(ULONG_PTR)resume->lr,
                     (void *)(ULONG_PTR)resume->sp, source,
                     bad_pc ? bad_pc : "bad-prev-pc" );
        return FALSE;
    }
    if (stack_lo && stack_hi && (resume->sp < stack_lo || resume->sp + 0x10 < resume->sp ||
                                 resume->sp + 0x10 > stack_hi))
    {
        if (reject_count++ < 16)
            MESSAGE( "macrunner-hb-native-dispatch-boundary-reject: tid=%04lx "
                     "pc=%p frame=%p frame_pc=%p prev=%p prev_pc=%p prev_sp=%p "
                     "stack=%p-%p source=%s reason=bad-prev-sp\n",
                     tid, (void *)(ULONG_PTR)pc, frame, (void *)(ULONG_PTR)frame->pc,
                     frame->prev_frame, (void *)(ULONG_PTR)resume->pc,
                     (void *)(ULONG_PTR)resume->sp, (void *)stack_lo, (void *)stack_hi,
                     source );
        return FALSE;
    }

    if (report_count++ < 32)
        MESSAGE( "macrunner-hb-native-dispatch-boundary-recovered: tid=%04lx "
                 "pc=%p image=%p frame=%p frame_pc=%p frame_lr=%p prev=%p "
                 "guest_pc=%p guest_lr=%p guest_sp=%p source=%s stack=%p-%p\n",
                 tid, (void *)(ULONG_PTR)pc, dispatch ? (void *)(ULONG_PTR)dispatch->ImageBase : NULL,
                 frame, (void *)(ULONG_PTR)frame->pc, (void *)(ULONG_PTR)frame->lr,
                 frame->prev_frame, (void *)(ULONG_PTR)resume->pc, (void *)(ULONG_PTR)resume->lr,
                 (void *)(ULONG_PTR)resume->sp, source, (void *)stack_lo, (void *)stack_hi );

    macrunner_hb_restore_syscall_prev_frame_context( context, resume );
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    dispatch->EstablisherFrame = context->Sp;
    dispatch->LanguageHandler = NULL;
    dispatch->HandlerData = NULL;
    return TRUE;
}

static BOOL macrunner_hb_fix_syscall_data_boundary_exception( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct macrunner_hb_syscall_frame *frame, *prev;
    static unsigned int report_count;
    DWORD tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );

    if (!rec || !context) return FALSE;
    if (rec->ExceptionCode != STATUS_DATATYPE_MISALIGNMENT || rec->ExceptionFlags)
        return FALSE;
    if (!macrunner_hb_is_x64_main_process()) return FALSE;
    if (!(frame = macrunner_hb_current_syscall_frame())) return FALSE;
    if (!macrunner_hb_pc_inside_syscall_frame( context->Pc, frame )) return FALSE;
    if (!(prev = frame->prev_frame) || !prev->pc || !prev->sp) return FALSE;
    if (macrunner_hb_pc_inside_syscall_frame( prev->pc, frame )) return FALSE;

    if (report_count++ < 64)
        MESSAGE( "macrunner-hb-arm64ec-syscall-data-repair: tid=%04lx pc=%p lr=%p "
                 "frame=%p bad_frame_pc=%p bad_frame_lr=%p prev=%p prev_pc=%p "
                 "prev_lr=%p prev_sp=%p resume=continue-prev-frame\n",
                 tid, (void *)(ULONG_PTR)context->Pc, (void *)(ULONG_PTR)context->Lr,
                 frame, (void *)(ULONG_PTR)frame->pc, (void *)(ULONG_PTR)frame->lr,
                 prev, (void *)(ULONG_PTR)prev->pc, (void *)(ULONG_PTR)prev->lr,
                 (void *)(ULONG_PTR)prev->sp );

    macrunner_hb_restore_syscall_prev_frame_context( context, prev );
    rec->ExceptionAddress = (void *)(ULONG_PTR)context->Pc;
    return TRUE;
}

static BOOL macrunner_hb_is_arm64ec_unwind_scaffold_overshoot( ULONG64 establisher_frame,
                                                               void *end_frame )
{
    struct macrunner_hb_syscall_frame *frame;
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = teb ? (ULONG_PTR)teb->Tib.StackLimit : 0;
    ULONG_PTR stack_hi = teb ? (ULONG_PTR)teb->Tib.StackBase : 0;
    ULONG_PTR end = (ULONG_PTR)end_frame;
    ULONG64 delta;

    if (!macrunner_hb_is_x64_main_process()) return FALSE;
    if (!end_frame || !stack_lo || !stack_hi) return FALSE;
    if (end < stack_lo || end > stack_hi) return FALSE;
    if (establisher_frame < stack_lo || establisher_frame > stack_hi) return FALSE;
    if (establisher_frame <= end) return FALSE;

    delta = establisher_frame - end;
    if (delta > 0x800) return FALSE;

    if (!(frame = macrunner_hb_current_syscall_frame())) return FALSE;
    if (!frame->pc || !frame->sp) return FALSE;
    if (frame->sp < stack_lo || frame->sp > stack_hi) return FALSE;
    return TRUE;
}

static void macrunner_hb_trace_tagged_exception_context( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    static unsigned int report_count;

    if (!rec || !context) return;
    if (rec->ExceptionCode != STATUS_DATATYPE_MISALIGNMENT && !(context->Pc & 1) && !(context->Lr & 1))
        return;
    if (report_count++ >= 64) return;

    MESSAGE( "macrunner-hb-arm64ec-exception-context: code=%08lx flags=%08lx pc=%p lr=%p "
             "sp=%016I64x x64main=%u\n",
             rec->ExceptionCode, rec->ExceptionFlags, (void *)context->Pc, (void *)context->Lr,
             context->Sp, macrunner_hb_is_x64_main_process() );
}

static void macrunner_hb_copy_unicode_ascii( char *dst, size_t dst_len, const UNICODE_STRING *src )
{
    unsigned int i, len;

    if (!dst_len) return;
    dst[0] = 0;
    if (!src || !src->Buffer) return;

    len = min( src->Length / sizeof(WCHAR), (dst_len - 1) );
    for (i = 0; i < len; i++)
    {
        WCHAR ch = src->Buffer[i];
        dst[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    dst[len] = 0;
}

static BOOL macrunner_hb_is_unityplayer_module( const LDR_DATA_TABLE_ENTRY *module )
{
    static const WCHAR nameW[] =
        {'U','n','i','t','y','P','l','a','y','e','r','.','d','l','l'};
    unsigned int i, len;

    if (!module || !module->BaseDllName.Buffer) return FALSE;
    if (module->SizeOfImage < 0x1f404a8 + sizeof(ULONG64)) return FALSE;
    len = module->BaseDllName.Length / sizeof(WCHAR);
    if (len != ARRAY_SIZE(nameW)) return FALSE;

    for (i = 0; i < len; i++)
    {
        WCHAR a = module->BaseDllName.Buffer[i];
        WCHAR b = nameW[i];

        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return FALSE;
    }
    return TRUE;
}

static BOOL macrunner_hb_query_env_uint( const WCHAR *nameW, unsigned int *value )
{
    WCHAR buffer[32];
    UNICODE_STRING name, val;
    unsigned int i, result = 0;

    RtlInitUnicodeString( &name, nameW );
    val.Length = 0;
    val.MaximumLength = sizeof(buffer);
    val.Buffer = buffer;
    if (RtlQueryEnvironmentVariable_U( NULL, &name, &val ) != STATUS_SUCCESS)
        return FALSE;

    buffer[min( val.Length / sizeof(WCHAR), ARRAY_SIZE(buffer) - 1 )] = 0;
    for (i = 0; buffer[i] >= '0' && buffer[i] <= '9'; i++)
        result = result * 10 + buffer[i] - '0';
    if (value) *value = result;
    return i > 0;
}

static BOOL macrunner_hb_trace_first_chance_enabled( unsigned int *budget )
{
    static BOOL initialized, enabled;
    static unsigned int trace_budget;
    static const WCHAR traceW[] =
        {'M','A','C','R','U','N','N','E','R','_','H','B','_','T','R','A','C','E','_',
         'F','I','R','S','T','_','C','H','A','N','C','E',0};
    static const WCHAR budgetW[] =
        {'M','A','C','R','U','N','N','E','R','_','H','B','_','T','R','A','C','E','_',
         'F','I','R','S','T','_','C','H','A','N','C','E','_','B','U','D','G','E','T',0};
    unsigned int value;

    if (!initialized)
    {
        if (macrunner_hb_query_env_uint( traceW, &value ) && value)
        {
            enabled = TRUE;
            trace_budget = 256;
            if (macrunner_hb_query_env_uint( budgetW, &value ) && value)
                trace_budget = value;
        }
        initialized = TRUE;
    }
    if (budget) *budget = trace_budget;
    return enabled;
}

static void macrunner_hb_trace_first_chance_exception( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    static unsigned int report_count;
    unsigned int budget;
    TEB *teb = NtCurrentTeb();
    LDR_DATA_TABLE_ENTRY *module = NULL;
    LDR_DATA_TABLE_ENTRY *lr_module = NULL;
    NTSTATUS ldr_status;
    NTSTATUS lr_ldr_status;
    ULONG_PTR pc, lr, module_base = 0, lr_module_base = 0, rva = 0, lr_rva = 0;
    char module_name[96] = "-";
    char lr_module_name[96] = "-";

    if (!rec || !context || !teb) return;
    if (!macrunner_hb_trace_first_chance_enabled( &budget )) return;
    if (report_count++ >= budget) return;
    if (!macrunner_hb_is_x64_main_process()) return;

    pc = (ULONG_PTR)context->Pc;
    lr = (ULONG_PTR)context->Lr;
    ldr_status = LdrFindEntryForAddress( (void *)pc, &module );
    if (ldr_status == STATUS_SUCCESS && module)
    {
        module_base = (ULONG_PTR)module->DllBase;
        rva = pc - module_base;
        macrunner_hb_copy_unicode_ascii( module_name, sizeof(module_name), &module->BaseDllName );
    }
    lr_ldr_status = LdrFindEntryForAddress( (void *)lr, &lr_module );
    if (lr_ldr_status == STATUS_SUCCESS && lr_module)
    {
        lr_module_base = (ULONG_PTR)lr_module->DllBase;
        lr_rva = lr - lr_module_base;
        macrunner_hb_copy_unicode_ascii( lr_module_name, sizeof(lr_module_name),
                                         &lr_module->BaseDllName );
    }

    MESSAGE( "macrunner-hb-seh-first-chance: tid=%04lx code=%08lx flags=%08lx "
             "addr=%p pc=%p lr=%p sp=%016I64x fp=%p ldr=%08lx module=%p "
             "rva=%08Ix name=%s lr_ldr=%08lx lr_module=%p lr_rva=%08Ix lr_name=%s "
             "stack=%p-%p params=%lu info0=%016I64x info1=%016I64x\n",
             HandleToULong( teb->ClientId.UniqueThread ), rec->ExceptionCode, rec->ExceptionFlags,
             rec->ExceptionAddress, (void *)pc, (void *)lr,
             context->Sp, (void *)(ULONG_PTR)context->Fp, ldr_status, (void *)module_base,
             rva, module_name, lr_ldr_status, (void *)lr_module_base, lr_rva, lr_module_name,
             teb->Tib.StackLimit, teb->Tib.StackBase, rec->NumberParameters,
             rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0,
             rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0 );

    /* MacRunner: any fault inside UnityPlayer -> dump its graphics IAT slots, to
     * see whether D3D11CreateDevice[1a02070] is the rebound thunk (non-NULL) or 0
     * (the create-fn guard at UnityPlayer 0x8e276f `cmpq $0,[1a02070]` bails E_FAIL
     * if NULL -> device @1f40460 stays NULL -> QI fault at 0x8e2acb). */
    if (module_base && rva && macrunner_hb_is_unityplayer_module( module ) && rva < module->SizeOfImage)
    {
        char *u = (char *)module_base;
        __TRY {
            MESSAGE( "macrunner-hb-unityiat: faultrva=0x%llx D3D11On12[1a02068]=%p D3D11CreateDevice[1a02070]=%p "
                     "CreateDXGIFactory2[1a02098]=%p CreateDXGIFactory[1a020a0]=%p device[1f40460]=%p factory[1f404a8]=%p\n",
                     (unsigned long long)rva,
                     (void *)*(ULONG64 *)(u + 0x1a02068), (void *)*(ULONG64 *)(u + 0x1a02070),
                     (void *)*(ULONG64 *)(u + 0x1a02098), (void *)*(ULONG64 *)(u + 0x1a020a0),
                     (void *)*(ULONG64 *)(u + 0x1f40460), (void *)*(ULONG64 *)(u + 0x1f404a8) );
        } __EXCEPT(macrunner_hb_pe_scan_fault) { } __ENDTRY
    }

    /* NULL/low call (execute AV at addr ~0): the immediate caller is lost
     * (lr=0 for a tail-br), so walk the saved fp-chain to recover the calling
     * frame's module/rva — pins which DXMT/D3D11 function jumped through a NULL
     * pointer. Also dump x8/x9 (likely-NULL call targets) + x19/x0 (object). */
    if (pc < 0x10000)
    {
        ULONG_PTR fp = (ULONG_PTR)context->Fp;
        int lvl;
        for (lvl = 0; lvl < 5 && fp && !(fp & 7); lvl++)
        {
            ULONG_PTR caller_fp = 0, caller_lr = 0, crva = 0, cbase = 0;
            LDR_DATA_TABLE_ENTRY *cmod = NULL;
            char cname[96] = "-";
            __TRY { caller_fp = ((const ULONG_PTR *)fp)[0]; caller_lr = ((const ULONG_PTR *)fp)[1]; }
            __EXCEPT(macrunner_hb_pe_scan_fault) { break; }
            __ENDTRY
            if (LdrFindEntryForAddress( (void *)caller_lr, &cmod ) == STATUS_SUCCESS && cmod)
            {
                cbase = (ULONG_PTR)cmod->DllBase;
                crva = caller_lr - cbase;
                macrunner_hb_copy_unicode_ascii( cname, sizeof(cname), &cmod->BaseDllName );
            }
            MESSAGE( "macrunner-hb-nullcall-frame: lvl=%d fp=%p caller_lr=%p module=%s base=%p rva=0x%llx "
                     "x8=%p x9=%p x19=%p x0=%p x1=%p\n",
                     lvl, (void *)fp, (void *)caller_lr, cname, (void *)cbase, (unsigned long long)crva,
                     (void *)(ULONG_PTR)context->X[8], (void *)(ULONG_PTR)context->X[9],
                     (void *)(ULONG_PTR)context->X[19], (void *)(ULONG_PTR)context->X[0],
                     (void *)(ULONG_PTR)context->X[1] );
            if (caller_fp <= fp) break;
            fp = caller_fp;
        }
        /* The execute-at-0 fault is in NATIVE ARM64EC code (a blr/br to a 0 target),
         * so dump the registers NATIVELY (the earlier x64-EC decode was garbage).
         * Map each to module/rva: r30=Lr (native caller after the blr), r16/r17 =
         * branch-target/veneer regs (which is 0?), r29=Fp, r31=Sp, r32=Pc. */
        {
            ULONG_PTR regs[33];
            int i;
            for (i = 0; i < 29; i++) regs[i] = (ULONG_PTR)context->X[i];
            regs[29] = (ULONG_PTR)context->Fp;
            regs[30] = (ULONG_PTR)context->Lr;
            regs[31] = (ULONG_PTR)context->Sp;
            regs[32] = (ULONG_PTR)context->Pc;
            for (i = 0; i < 33; i++)
            {
                ULONG_PTR v = regs[i], rva = 0; LDR_DATA_TABLE_ENTRY *m = NULL; char mn[64] = "-";
                if (v && LdrFindEntryForAddress( (void *)v, &m ) == STATUS_SUCCESS && m)
                { rva = v - (ULONG_PTR)m->DllBase; macrunner_hb_copy_unicode_ascii( mn, sizeof(mn), &m->BaseDllName ); }
                MESSAGE( "macrunner-hb-nullcall-natreg: r%d=%p %s+0x%llx\n",
                         i, (void *)v, mn, (unsigned long long)rva );
            }
        }
    }
}

static BOOL macrunner_hb_fix_tagged_arm64ec_misalignment( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    static unsigned int report_count;
    DWORD64 pc, fixed_pc;

    if (!rec || !context) return FALSE;
    if (rec->ExceptionCode != STATUS_DATATYPE_MISALIGNMENT) return FALSE;
    if (rec->ExceptionFlags) return FALSE;
    if (!macrunner_hb_is_x64_main_process()) return FALSE;

    pc = context->Pc;
    if (!(pc & 1)) return FALSE;
    if (pc < MACRUNNER_HB_HOST_BOUNDARY_MIN || pc >= MACRUNNER_HB_HOST_BOUNDARY_MAX)
        return FALSE;
    if (context->Lr && context->Lr != pc) return FALSE;

    fixed_pc = pc & ~(DWORD64)1;
    if (report_count++ < 64)
        MESSAGE( "macrunner-hb-arm64ec-tagged-pc: exception=%08lx pc=%p lr=%p fixed=%p sp=%016I64x\n",
                 rec->ExceptionCode, (void *)pc, (void *)context->Lr, (void *)fixed_pc, context->Sp );

    context->Pc = fixed_pc;
    if (context->Lr == pc) context->Lr = fixed_pc;
    rec->ExceptionAddress = (void *)(ULONG_PTR)fixed_pc;
    return TRUE;
}

/*******************************************************************
 *         syscalls
 */
#define SYSCALL_ENTRY(id,name,args) __ASM_SYSCALL_FUNC( id, name )
ALL_SYSCALLS
#undef SYSCALL_ENTRY


/**************************************************************************
 *		__chkstk (NTDLL.@)
 *
 * Supposed to touch all the stack pages, but we shouldn't need that.
 */
__ASM_GLOBAL_FUNC( __chkstk, "ret")


/***********************************************************************
 *		RtlCaptureContext (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( RtlCaptureContext,
                    "str xzr, [x0, #0x8]\n\t"        /* context->X0 */
                    "stp x1, x2, [x0, #0x10]\n\t"    /* context->X1,X2 */
                    "stp x3, x4, [x0, #0x20]\n\t"    /* context->X3,X4 */
                    "stp x5, x6, [x0, #0x30]\n\t"    /* context->X5,X6 */
                    "stp x7, x8, [x0, #0x40]\n\t"    /* context->X7,X8 */
                    "stp x9, x10, [x0, #0x50]\n\t"   /* context->X9,X10 */
                    "stp x11, x12, [x0, #0x60]\n\t"  /* context->X11,X12 */
                    "stp x13, x14, [x0, #0x70]\n\t"  /* context->X13,X14 */
                    "stp x15, x16, [x0, #0x80]\n\t"  /* context->X15,X16 */
                    "stp x17, x18, [x0, #0x90]\n\t"  /* context->X17,X18 */
                    "stp x19, x20, [x0, #0xa0]\n\t"  /* context->X19,X20 */
                    "stp x21, x22, [x0, #0xb0]\n\t"  /* context->X21,X22 */
                    "stp x23, x24, [x0, #0xc0]\n\t"  /* context->X23,X24 */
                    "stp x25, x26, [x0, #0xd0]\n\t"  /* context->X25,X26 */
                    "stp x27, x28, [x0, #0xe0]\n\t"  /* context->X27,X28 */
                    "stp x29, xzr, [x0, #0xf0]\n\t"  /* context->Fp,Lr */
                    "mov x1, sp\n\t"
                    "stp x1, x30, [x0, #0x100]\n\t"  /* context->Sp,Pc */
                    "stp q0,  q1,  [x0, #0x110]\n\t" /* context->V[0-1] */
                    "stp q2,  q3,  [x0, #0x130]\n\t" /* context->V[2-3] */
                    "stp q4,  q5,  [x0, #0x150]\n\t" /* context->V[4-5] */
                    "stp q6,  q7,  [x0, #0x170]\n\t" /* context->V[6-7] */
                    "stp q8,  q9,  [x0, #0x190]\n\t" /* context->V[8-9] */
                    "stp q10, q11, [x0, #0x1b0]\n\t" /* context->V[10-11] */
                    "stp q12, q13, [x0, #0x1d0]\n\t" /* context->V[12-13] */
                    "stp q14, q15, [x0, #0x1f0]\n\t" /* context->V[14-15] */
                    "stp q16, q17, [x0, #0x210]\n\t" /* context->V[16-17] */
                    "stp q18, q19, [x0, #0x230]\n\t" /* context->V[18-19] */
                    "stp q20, q21, [x0, #0x250]\n\t" /* context->V[20-21] */
                    "stp q22, q23, [x0, #0x270]\n\t" /* context->V[22-23] */
                    "stp q24, q25, [x0, #0x290]\n\t" /* context->V[24-25] */
                    "stp q26, q27, [x0, #0x2b0]\n\t" /* context->V[26-27] */
                    "stp q28, q29, [x0, #0x2d0]\n\t" /* context->V[28-29] */
                    "stp q30, q31, [x0, #0x2f0]\n\t" /* context->V[30-31] */
                    "mov w1, #0x400000\n\t"          /* CONTEXT_ARM64 */
                    "movk w1, #0x7\n\t"              /* CONTEXT_FULL */
                    "str w1, [x0]\n\t"               /* context->ContextFlags */
                    "mrs x1, NZCV\n\t"
                    "str w1, [x0, #0x4]\n\t"         /* context->Cpsr */
                    "mrs x1, FPCR\n\t"
                    "str w1, [x0, #0x310]\n\t"       /* context->Fpcr */
                    "mrs x1, FPSR\n\t"
                    "str w1, [x0, #0x314]\n\t"       /* context->Fpsr */
                    "ret" )


/**********************************************************************
 * ARM64EC frame fallback for the plain-ARM64 dispatcher.
 *
 * ARM64X hybrid builtins keep the unwind data of their ARM64EC ranges in
 * the CHPE ExtraRFETable (AMD64 RUNTIME_FUNCTION/UNWIND_INFO format); the
 * ARM64-side RtlLookupFunctionEntry only consults the native ARM64 .pdata,
 * so unwinding through an EC frame used to raise STATUS_INVALID_DISPOSITION
 * and recurse (the c0000026 storms).  Walk the frame here by interpreting
 * the AMD64 unwind ops directly on the ARM64 context through the fixed
 * ARM64EC register mapping.  EC handlers are not invoked (frame walk only).
 */

struct macrunner_ec_runtime_function
{
    DWORD BeginAddress;
    DWORD EndAddress;
    DWORD UnwindData;
};

struct macrunner_ec_opcode
{
    BYTE offset;
    BYTE code : 4;
    BYTE info : 4;
};

struct macrunner_ec_unwind_info
{
    BYTE version : 3;
    BYTE flags : 5;
    BYTE prolog;
    BYTE count;
    BYTE frame_reg : 4;
    BYTE frame_offset : 4;
    struct macrunner_ec_opcode opcodes[1];
};

#ifndef UNW_FLAG_CHAININFO
#define UNW_FLAG_CHAININFO 4
#endif

#define MACRUNNER_EC_UWOP_PUSH_NONVOL     0
#define MACRUNNER_EC_UWOP_ALLOC_LARGE     1
#define MACRUNNER_EC_UWOP_ALLOC_SMALL     2
#define MACRUNNER_EC_UWOP_SET_FPREG       3
#define MACRUNNER_EC_UWOP_SAVE_NONVOL     4
#define MACRUNNER_EC_UWOP_SAVE_NONVOL_FAR 5
#define MACRUNNER_EC_UWOP_EPILOG          6
#define MACRUNNER_EC_UWOP_SAVE_XMM128     8
#define MACRUNNER_EC_UWOP_SAVE_XMM128_FAR 9
#define MACRUNNER_EC_UWOP_PUSH_MACHFRAME  10

/* x64 register number -> ARM64 context slot per the ARM64EC mapping */
static DWORD64 *macrunner_ec_int_reg( CONTEXT *context, unsigned int reg )
{
    switch (reg)
    {
    case 0:  return &context->X8;   /* rax */
    case 1:  return &context->X0;   /* rcx */
    case 2:  return &context->X1;   /* rdx */
    case 3:  return &context->X27;  /* rbx */
    case 4:  return &context->Sp;   /* rsp */
    case 5:  return &context->Fp;   /* rbp */
    case 6:  return &context->X25;  /* rsi */
    case 7:  return &context->X26;  /* rdi */
    case 8:  return &context->X2;   /* r8 */
    case 9:  return &context->X3;   /* r9 */
    case 10: return &context->X4;   /* r10 */
    case 11: return &context->X5;   /* r11 */
    case 12: return &context->X19;  /* r12 */
    case 13: return &context->X20;  /* r13 */
    case 14: return &context->X21;  /* r14 */
    case 15: return &context->X22;  /* r15 */
    }
    return NULL;
}

static int macrunner_ec_opcode_size( struct macrunner_ec_opcode op )
{
    switch (op.code)
    {
    case MACRUNNER_EC_UWOP_ALLOC_LARGE:
        return 2 + (op.info != 0);
    case MACRUNNER_EC_UWOP_SAVE_NONVOL:
    case MACRUNNER_EC_UWOP_SAVE_XMM128:
    case MACRUNNER_EC_UWOP_EPILOG:
        return 2;
    case MACRUNNER_EC_UWOP_SAVE_NONVOL_FAR:
    case MACRUNNER_EC_UWOP_SAVE_XMM128_FAR:
        return 3;
    default:
        return 1;
    }
}

static LONG CALLBACK macrunner_hb_pe_scan_fault( EXCEPTION_POINTERS *ep );

static IMAGE_ARM64EC_METADATA *macrunner_ec_module_metadata( ULONG_PTR base )
{
    const IMAGE_NT_HEADERS *nt;
    const IMAGE_LOAD_CONFIG_DIRECTORY *cfg;
    ULONG size;

    if (!base) return NULL;
    if (!(nt = RtlImageNtHeader( (void *)base ))) return NULL;
    cfg = RtlImageDirectoryEntryToData( (void *)base, TRUE, IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, &size );
    if (!cfg || size <= offsetof( IMAGE_LOAD_CONFIG_DIRECTORY, CHPEMetadataPointer )) return NULL;
    if (cfg->CHPEMetadataPointer <= base ||
        cfg->CHPEMetadataPointer >= base + nt->OptionalHeader.SizeOfImage)
        return NULL;
    return (IMAGE_ARM64EC_METADATA *)(ULONG_PTR)cfg->CHPEMetadataPointer;
}

static int macrunner_hb_arm64x_code_range_kind( ULONG_PTR base, DWORD64 pc )
{
    IMAGE_ARM64EC_METADATA *metadata = macrunner_ec_module_metadata( base );
    IMAGE_NT_HEADERS *nt;
    const IMAGE_CHPE_RANGE_ENTRY *map;
    DWORD64 rva;
    ULONG i, max_count;

    if (!metadata || !metadata->CodeMap || !metadata->CodeMapCount || pc < base) return -1;
    if (!(nt = RtlImageNtHeader( (void *)base )) || !nt->OptionalHeader.SizeOfImage) return -1;
    if (pc >= base + nt->OptionalHeader.SizeOfImage) return -1;
    if (metadata->CodeMap >= nt->OptionalHeader.SizeOfImage) return -1;

    max_count = (nt->OptionalHeader.SizeOfImage - metadata->CodeMap) / sizeof(*map);
    if (metadata->CodeMapCount > max_count) return -1;
    rva = pc - base;
    map = (const IMAGE_CHPE_RANGE_ENTRY *)(base + metadata->CodeMap);

    __TRY
    {
        for (i = 0; i < metadata->CodeMapCount; i++)
        {
            IMAGE_CHPE_RANGE_ENTRY entry;
            ULONG start, end;

            memcpy( &entry, &map[i], sizeof(entry) );
            start = entry.StartOffset & ~1u;
            if (entry.Length > ~start) continue;
            end = start + entry.Length;
            if (rva >= start && rva < end) return entry.NativeCode ? 1 : 0;
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY
    return -1;
}

static BOOL macrunner_hb_pc_unsafe_for_arm64_unwind( DISPATCHER_CONTEXT *dispatch,
                                                     CONTEXT *context, DWORD64 pc )
{
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = teb ? (ULONG_PTR)teb->Tib.StackLimit : 0;
    ULONG_PTR stack_hi = teb ? (ULONG_PTR)teb->Tib.StackBase : 0;
    ULONG_PTR sp = context ? (ULONG_PTR)context->Sp : 0;

    if (pc < MACRUNNER_HB_HOST_BOUNDARY_MAX) return TRUE;
    if (dispatch && dispatch->ImageBase &&
        macrunner_hb_arm64x_code_range_kind( dispatch->ImageBase, pc ) == 0)
        return TRUE;
    if (stack_lo && stack_hi && (!sp || sp < stack_lo || sp + 0x10 > stack_hi))
        return TRUE;
    return FALSE;
}

static BOOL macrunner_ec_virtual_unwind_frame( DISPATCHER_CONTEXT *dispatch, CONTEXT *context,
                                               DWORD64 pc )
{
    /* lld-built ARM64X hybrids carry NO unwind data at all for their EC
     * ranges (ExtraRFETable=0, x64-view exception dir zeroed by the ARM64X
     * fixups), so unwind by EPILOGUE SCAN: interpret forward from pc a
     * strict whitelist of ARM64 epilogue instructions until ret/br x30.
     * Any other instruction bails out to the old invalid-disposition path. */
    CONTEXT walk = *context;
    const DWORD *insn;
    unsigned int steps;
    BOOL done = FALSE, lr_restored = FALSE;
    const char *bad_pc = NULL;

    /* pc was just executing (it is a live frame address) — instructions
     * there are mapped; no LDR registration required (guest-arena module
     * copies are not in the loader list). */
    if (pc < 0x10000 || (pc & 3)) return FALSE;
    insn = (const DWORD *)(ULONG_PTR)pc;

    for (steps = 0; steps < 64 && !done; steps++, insn++)
    {
        DWORD op = *insn;

        /* the frame's own call instruction (pc points AT the bl/blr that
         * called the faulting child) — step over it */
        if (steps == 0 && ((op & 0xfc000000u) == 0x94000000u ||   /* bl */
                           (op & 0xfffffc1fu) == 0xd63f0000u))    /* blr */
            continue;

        if ((op & 0xffc003e0u) == 0xa94003e0u)        /* ldp xA, xB, [sp, #imm] */
        {
            unsigned int rt = op & 0x1f, rt2 = (op >> 10) & 0x1f;
            int imm = ((int)((op >> 15) & 0x7f) << 25) >> 22;  /* signed imm7 * 8 */
            if (rt < 31)  walk.X[rt]  = *(DWORD64 *)(walk.Sp + imm);
            if (rt2 < 31) walk.X[rt2] = *(DWORD64 *)(walk.Sp + imm + 8);
            if (rt == 30 || rt2 == 30) lr_restored = TRUE;
        }
        else if ((op & 0xffc003e0u) == 0xa8c003e0u)   /* ldp xA, xB, [sp], #imm (post-index) */
        {
            unsigned int rt = op & 0x1f, rt2 = (op >> 10) & 0x1f;
            int imm = ((int)((op >> 15) & 0x7f) << 25) >> 22;
            if (rt < 31)  walk.X[rt]  = *(DWORD64 *)walk.Sp;
            if (rt2 < 31) walk.X[rt2] = *(DWORD64 *)(walk.Sp + 8);
            walk.Sp += imm;
            if (rt == 30 || rt2 == 30) lr_restored = TRUE;
        }
        else if ((op & 0xffc003e0u) == 0xf94003e0u)   /* ldr xA, [sp, #imm] */
        {
            unsigned int rt = op & 0x1f;
            DWORD64 imm = ((op >> 10) & 0xfff) * 8;
            if (rt < 31) walk.X[rt] = *(DWORD64 *)(walk.Sp + imm);
            if (rt == 30) lr_restored = TRUE;
        }
        else if ((op & 0xffe00fe0u) == 0xf84007e0u)   /* ldr xA, [sp], #imm (post-index) */
        {
            unsigned int rt = op & 0x1f;
            int imm = ((int)((op >> 12) & 0x1ff) << 23) >> 23;
            if (rt < 31) walk.X[rt] = *(DWORD64 *)walk.Sp;
            walk.Sp += imm;
            if (rt == 30) lr_restored = TRUE;
        }
        else if ((op & 0xff8003ffu) == 0x910003ffu)   /* add sp, sp, #imm[, lsl #12] */
        {
            DWORD64 imm = (op >> 10) & 0xfff;
            if (op & 0x400000) imm <<= 12;
            walk.Sp += imm;
        }
        else if ((op & 0xff8003ffu) == 0x910003bfu)   /* add sp, x29, #imm (incl. mov sp, x29) */
        {
            DWORD64 imm = (op >> 10) & 0xfff;
            if (op & 0x400000) imm <<= 12;
            walk.Sp = walk.Fp + imm;
        }
        else if (op == 0xd65f03c0u || op == 0xd61f03c0u ||  /* ret / br x30 */
                 (op & 0xfffffc1fu) == 0xd61f0000u)         /* br xN: tail thunk */
        {
            /* pc points at this frame's own call, so the live lr belongs to
             * the callee — the scan must have reloaded lr from the stack for
             * the walk to be valid */
            if (!lr_restored) return FALSE;
            done = TRUE;
            break;
        }
        else if ((op & 0xfffff01fu) == 0xd503201fu) ; /* hint family: nop/pac/bti */
        else return FALSE;                            /* not a clean epilogue */
    }
    if (!done) return FALSE;
    if (macrunner_hb_is_plausible_recovered_pc( walk.Lr, &bad_pc ) && walk.Lr == pc)
        bad_pc = "pc-no-progress";
    if (bad_pc)
    {
        static unsigned int bad_count;
        if (bad_count++ < 32)
            MESSAGE( "macrunner-hb-seh-ec-unwind-bail: reason=%s pc=%p image=%p "
                     "new_pc=%p sp=%016I64x stack=%p-%p\n",
                     bad_pc, (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)dispatch->ImageBase,
                     (void *)(ULONG_PTR)walk.Lr, walk.Sp,
                     NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
        return FALSE;
    }

    *context = walk;
    context->Pc = walk.Lr;
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;

    dispatch->EstablisherFrame = context->Sp;
    dispatch->LanguageHandler = NULL;
    dispatch->HandlerData = NULL;

    {
        static unsigned int trace_count;
        if (trace_count++ < 16)
            ERR( "macrunner-hb-seh-ec-unwind: epilogue-scan pc=%p image=%p -> new pc=%p sp=%016I64x\n",
                 (void *)pc, (void *)dispatch->ImageBase, (void *)context->Pc, context->Sp );
    }
    return TRUE;
}

/* Fallback unwinder for MID-FUNCTION EC frames with no unwind data, where the
 * epilogue-scan above bails because pc is not at an epilogue (e.g. rpcrt4 RVA
 * 0x3E600 in the 0x6ba RPC_S_SERVER_UNAVAILABLE SEH cascade — a `bl` site, not a
 * ret).  lld ARM64X has ExtraRFETable=0 so there is genuinely no metadata; the
 * only remaining signal is the ARM64 frame-pointer (x29) chain that ABI-compliant
 * EC prologues maintain: [x29]=caller x29, [x29+8]=return address.  Heavily
 * validated (16-aligned, in the thread stack, chain climbs upward, return addr
 * is plausible code) so a frameless function or garbage fp bails to the old
 * invalid-disposition path instead of unwinding into nonsense. */
static BOOL macrunner_ec_fp_chain_unwind( DISPATCHER_CONTEXT *dispatch, CONTEXT *context, DWORD64 pc )
{
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = (ULONG_PTR)teb->Tib.StackLimit;
    ULONG_PTR stack_hi = (ULONG_PTR)teb->Tib.StackBase;
    ULONG_PTR fp = (ULONG_PTR)context->Fp;
    ULONG_PTR sp = (ULONG_PTR)context->Sp;
    ULONG_PTR new_fp = 0, new_pc = 0;
    const char *bail = NULL;
    BOOL stop_unwind = FALSE;

    if (!fp || (fp & 0xf)) bail = "fp-null-or-unaligned";
    else if (fp < stack_lo || fp + 0x10 > stack_hi)
    {
        bail = "fp-out-of-stack";
        stop_unwind = TRUE;
    }
    else
    {
        const char *pc_reason = NULL;

        new_fp = ((const ULONG_PTR *)fp)[0];   /* saved caller x29 */
        new_pc = ((const ULONG_PTR *)fp)[1];   /* saved lr / return address */
        if (!new_pc || new_pc == pc || (new_pc & 3) || new_pc < 0x10000) bail = "bad-new-pc";
        else if (!macrunner_hb_is_plausible_recovered_pc( new_pc, &pc_reason ))
        {
            bail = pc_reason ? pc_reason : "bad-new-pc";
            stop_unwind = TRUE;
        }
        else if (new_fp && (new_fp <= fp || (new_fp & 0xf) || new_fp + 0x10 > stack_hi)) bail = "bad-new-fp";
    }
    if (bail)
    {
        static unsigned int fpb;
        if (fpb++ < 16)
            MESSAGE( "macrunner-hb-seh-ec-fpchain-bail: reason=%s pc=%p fp=%p sp=%p lr=%p "
                     "stack=%p-%p new_fp=%p new_pc=%p action=%s\n",
                     bail, (void *)pc, (void *)fp, (void *)sp, (void *)(ULONG_PTR)context->Lr,
                     (void *)stack_lo, (void *)stack_hi, (void *)new_fp, (void *)new_pc,
                     stop_unwind ? "stop-unwind" : "fallback" );
        if (stop_unwind)
        {
            macrunner_hb_stop_unwind_at_boundary( dispatch, context );
            return TRUE;
        }
        return FALSE;
    }

    context->Sp  = fp + 0x10;
    context->Fp  = new_fp;
    context->Lr  = new_pc;
    context->Pc  = new_pc;
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
    dispatch->EstablisherFrame = context->Sp;
    dispatch->LanguageHandler = NULL;
    dispatch->HandlerData = NULL;
    {
        static unsigned int fp_trace;
        if (fp_trace++ < 16)
            MESSAGE( "macrunner-hb-seh-ec-fpchain: pc=%p fp=%p -> new pc=%p new fp=%p sp=%016I64x\n",
                     (void *)pc, (void *)fp, (void *)new_pc, (void *)new_fp, context->Sp );
    }
    return TRUE;
}

static BOOL macrunner_hb_arm64_no_pdata_frameless_unwind( DISPATCHER_CONTEXT *dispatch,
                                                          CONTEXT *context, DWORD64 pc );

static BOOL macrunner_hb_try_arm64_unwind_methods( DISPATCHER_CONTEXT *dispatch,
                                                   CONTEXT *context, DWORD64 pc )
{
    if (macrunner_hb_pc_unsafe_for_arm64_unwind( dispatch, context, pc ))
    {
        static unsigned int unsafe_count;
        TEB *teb = NtCurrentTeb();
        ULONG_PTR stack_lo = teb ? (ULONG_PTR)teb->Tib.StackLimit : 0;
        ULONG_PTR stack_hi = teb ? (ULONG_PTR)teb->Tib.StackBase : 0;
        int kind = dispatch && dispatch->ImageBase ?
            macrunner_hb_arm64x_code_range_kind( dispatch->ImageBase, pc ) : -1;

        if (macrunner_hb_recover_native_dispatch_boundary( dispatch, context, pc ))
            return TRUE;

        if (unsafe_count++ < 32)
            MESSAGE( "macrunner-hb-arm64-unwind-unsafe-boundary: pc=%p image=%p "
                     "kind=%d sp=%016I64x stack=%p-%p action=stop-unwind\n",
                     (void *)(ULONG_PTR)pc,
                     dispatch ? (void *)(ULONG_PTR)dispatch->ImageBase : NULL,
                     kind, context ? context->Sp : 0, (void *)stack_lo, (void *)stack_hi );
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return TRUE;
    }

    return macrunner_ec_virtual_unwind_frame( dispatch, context, pc ) ||
           macrunner_hb_arm64_no_pdata_frameless_unwind( dispatch, context, pc ) ||
           macrunner_ec_fp_chain_unwind( dispatch, context, pc );
}


/**********************************************************************
 *           virtual_unwind
 */
static LONG CALLBACK macrunner_hb_pe_scan_fault( EXCEPTION_POINTERS *ep );

/* Arena exception-data index consult (operator (c1)->(c2)): recover the
 * RUNTIME_FUNCTION for a pc in a mapped-but-UNREGISTERED guest-arena module
 * alias.  The arena alias is an execution-view (deliberately not in the loader
 * list, to preserve module identity), but it is a full contiguous image copy
 * carrying the real ARM64 .pdata — so we locate the image base (MZ-scan down
 * from pc; [base,pc] is wholly mapped, the scan never reads below base) and
 * binary-search its exception directory exactly as RtlLookupFunctionEntry would
 * for a registered module.  This is the native RtlAddFunctionTable semantics for
 * out-of-list code; a map-time-populated sorted range index will later replace
 * the per-call scan with O(log n) lookup (same RUNTIME_FUNCTION result). */
static void macrunner_hb_arena_consult_function_entry( DWORD64 pc, RUNTIME_FUNCTION **entry_out,
                                                       ULONG_PTR *base_out )
{
    ULONG_PTR img_floor = pc & ~0x0fffffffULL;   /* 256MB-aligned backstop */
    ULONG_PTR base = 0, p;
    RUNTIME_FUNCTION *table;
    ULONG size = 0, rva, count;
    LONG lo, hi, found = -1;

    *entry_out = NULL;
    if (pc < 0x10000 || (pc & 3)) return;
    if (pc < MACRUNNER_HB_HOST_BOUNDARY_MAX) return;

    __TRY
    {
        for (p = pc & ~0xfffULL; p >= img_floor; p -= 0x1000)
            if (*(const USHORT *)p == 0x5a4d) { base = p; break; }   /* 'MZ' header */
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) { base = 0; }
    __ENDTRY
    if (!base || !RtlImageNtHeader( (void *)base )) return;
    table = RtlImageDirectoryEntryToData( (void *)base, TRUE, IMAGE_DIRECTORY_ENTRY_EXCEPTION, &size );
    if (!table || size < sizeof(*table)) return;
    count = size / sizeof(*table);
    rva = (ULONG)(pc - base);
    /* largest BeginAddress <= rva (the function containing pc) */
    lo = 0; hi = (LONG)count - 1;
    while (lo <= hi)
    {
        LONG mid = lo + (hi - lo) / 2;
        if (table[mid].BeginAddress <= rva) { found = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (found < 0) return;
    *entry_out = &table[found];
    *base_out  = base;
    {
        static unsigned int t;
        if (t++ < 16)
            ERR( "macrunner-hb-arena-fde: pc=%p base=%p rva=%#x begin=%#x count=%lu\n",
                 (void *)pc, (void *)base, rva, table[found].BeginAddress, (unsigned long)count );
    }
}

/* MacRunner Lane A (2026-06-12): WOW64/i386 ARM64-PE pdata unwind machinery, RESTORED
 * from stash@{1} (lane-a-forward-fixes) in its delta-#4 form — aaf425d had deleted it
 * (246 lines) while consolidating bulk-CFI, regressing the i386 unwind path (80000002).
 * Gated !macrunner_hb_is_x64_main_process() at the call site so HK/x64 keeps using
 * arena-fde (the f69cdbc cascade fix is untouched).  ARM64 PE modules loaded by the
 * WOW64 layer above HOST_BOUNDARY_MAX are not tracked by the PE-side LDR; ARM64X
 * counterparts (ucrtbase) also have DataDirectory[3] -> .reloc, so scan section
 * headers for a ".pdata" section by name. */
static LONG CALLBACK macrunner_hb_pe_scan_fault( EXCEPTION_POINTERS *ep )
{
    (void)ep;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ARM64 RUNTIME_FUNCTION end-RVA: use FunctionLength (in 4-byte units) from the packed
 * header (Flag != 0) or from the XDATA block (Flag == 0). */
#define MACRUNNER_HB_ARM64_FUNC_END(entry, base_ptr) \
    ((entry)->Flag \
     ? (entry)->BeginAddress + 4u * (entry)->FunctionLength \
     : (entry)->BeginAddress + 4u * \
       ((const IMAGE_ARM64_RUNTIME_FUNCTION_ENTRY_XDATA *)((base_ptr) + (entry)->UnwindData))->FunctionLength)

static PRUNTIME_FUNCTION macrunner_hb_pdata_lookup_at_base( DWORD64 pc, DWORD64 known_base,
                                                            DWORD64 *out_image_base )
{
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    PRUNTIME_FUNCTION funcs, lo, hi, mid;
    char *base = (char *)(ULONG_PTR)known_base;
    DWORD count, rva;
    static unsigned int report_count;

    __TRY
    {
        nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
        if (macrunner_hb_arm64x_code_range_kind( known_base, pc ) == 0)
        {
            static unsigned int x64_range_count;

            if (out_image_base) *out_image_base = known_base;
            if (x64_range_count++ < 16)
                MESSAGE( "macrunner-hb-wow64-arm64-pdata: skip-x64-range base=%p rva=%08lx\n",
                         base, (DWORD)(pc - known_base) );
            return NULL;
        }
        dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir->VirtualAddress || !dir->Size) return NULL;
        funcs = (PRUNTIME_FUNCTION)(base + dir->VirtualAddress);
        count = dir->Size / sizeof(*funcs);

        rva = (DWORD)(pc - known_base);
        if (report_count++ < 16)
            MESSAGE( "macrunner-hb-wow64-arm64-pdata: direct base=%p rva=%08lx count=%lu\n",
                     base, rva, (unsigned long)count );

        lo = funcs; hi = funcs + count;
        while (lo < hi)
        {
            mid = lo + (hi - lo) / 2;
            if (rva < mid->BeginAddress) { hi = mid; }
            else if (rva < MACRUNNER_HB_ARM64_FUNC_END(mid, base))
            {
                if (out_image_base) *out_image_base = known_base;
                return mid;
            }
            else { lo = mid + 1; }
        }

        /* DataDirectory[3] may point to wrong section (ARM64X ucrtbase: .reloc, not .pdata).
         * Scan section headers for a ".pdata" section and retry only if it differs. */
        {
            IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION( nt );
            WORD i;
            for (i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
            {
                if (memcmp( sec->Name, ".pdata\0\0", 8 ) != 0) continue;
                if (!sec->VirtualAddress || !sec->SizeOfRawData) continue;
                if (sec->VirtualAddress == dir->VirtualAddress) break; /* already tried */
                funcs = (PRUNTIME_FUNCTION)(base + sec->VirtualAddress);
                count = sec->SizeOfRawData / sizeof(*funcs);
                if (report_count <= 32)
                    MESSAGE( "macrunner-hb-wow64-arm64-pdata: section-fallback base=%p"
                             " rva=%08lx count=%lu\n", base, rva, (unsigned long)count );
                lo = funcs; hi = funcs + count;
                while (lo < hi)
                {
                    mid = lo + (hi - lo) / 2;
                    if (rva < mid->BeginAddress) { hi = mid; }
                    else if (rva < MACRUNNER_HB_ARM64_FUNC_END(mid, base))
                    {
                        if (out_image_base) *out_image_base = known_base;
                        return mid;
                    }
                    else { lo = mid + 1; }
                }
                break;
            }
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY
    return NULL;
}

static PRUNTIME_FUNCTION macrunner_hb_register_wow64_arm64_pe_pdata( DWORD64 pc, DWORD64 *out_image_base )
{
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_SECTION_HEADER *sec;
    PRUNTIME_FUNCTION funcs, lo, hi, mid;
    char *base;
    DWORD i, count, rva;
    WORD j;
    static unsigned int report_count;

    if (!out_image_base) return NULL;
    if (!pc || pc <= MACRUNNER_HB_HOST_BOUNDARY_MAX) return NULL;

    base = (char *)((ULONG_PTR)pc & ~(ULONG_PTR)0xfff);
    __TRY
    {
        for (i = 0; i <= 256; i++, base -= 0x1000)
        {
            dos = (IMAGE_DOS_HEADER *)base;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
            if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x800) continue;
            nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
            if (nt->OptionalHeader.SizeOfImage == 0) continue;
            if ((ULONG_PTR)base + nt->OptionalHeader.SizeOfImage <= (ULONG_PTR)pc) continue;
            if (macrunner_hb_arm64x_code_range_kind( (ULONG_PTR)base, pc ) == 0)
            {
                static unsigned int x64_range_count;

                *out_image_base = (DWORD64)(ULONG_PTR)base;
                if (x64_range_count++ < 16)
                    MESSAGE( "macrunner-hb-wow64-arm64-pdata: scan-skip-x64-range "
                             "base=%p machine=%04x pc=%p rva=%08lx\n",
                             base, nt->FileHeader.Machine, (void *)(ULONG_PTR)pc,
                             (DWORD)(pc - (DWORD64)(ULONG_PTR)base) );
                return NULL;
            }

            if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64)
            {
                /* Native ARM64 PE — use DataDirectory[3] directly. */
                dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
                if (!dir->VirtualAddress || !dir->Size) return NULL;
                funcs = (PRUNTIME_FUNCTION)(base + dir->VirtualAddress);
                count = dir->Size / sizeof(*funcs);
            }
            else if (nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64)
            {
                /* ARM64X counterpart: machine=AMD64 in memory after update_arm64x_mapping().
                 * Verify this is really an ARM64X module by checking CHPE metadata pointer. */
                IMAGE_DATA_DIRECTORY *lc_dir =
                    &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
                BOOL is_arm64x = FALSE;
                if (lc_dir->VirtualAddress && lc_dir->Size)
                {
                    IMAGE_LOAD_CONFIG_DIRECTORY *cfg =
                        (IMAGE_LOAD_CONFIG_DIRECTORY *)(base + lc_dir->VirtualAddress);
                    DWORD lc_size = min(lc_dir->Size, cfg->Size);
                    if (lc_size > offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, CHPEMetadataPointer)
                        && cfg->CHPEMetadataPointer > (ULONG_PTR)base
                        && cfg->CHPEMetadataPointer <
                               (ULONG_PTR)base + nt->OptionalHeader.SizeOfImage)
                        is_arm64x = TRUE;
                }
                if (!is_arm64x) continue;
                /* ARM64X: DataDirectory[3] may point to wrong section (ucrtbase: .reloc).
                 * Always use section-header scan for .pdata. */
                funcs = NULL; count = 0;
                sec = IMAGE_FIRST_SECTION( nt );
                for (j = 0; j < nt->FileHeader.NumberOfSections; j++, sec++)
                {
                    if (memcmp( sec->Name, ".pdata\0\0", 8 ) != 0) continue;
                    if (!sec->VirtualAddress || !sec->SizeOfRawData) continue;
                    funcs = (PRUNTIME_FUNCTION)(base + sec->VirtualAddress);
                    count = sec->SizeOfRawData / sizeof(*funcs);
                    break;
                }
                if (!funcs || !count) return NULL;
            }
            else continue;

            if (report_count++ < 16)
                MESSAGE( "macrunner-hb-wow64-arm64-pdata: scan base=%p machine=%04x size=%08lx "
                         "pc=%p rva=%08lx count=%lu\n",
                         base, nt->FileHeader.Machine, nt->OptionalHeader.SizeOfImage,
                         (void *)(ULONG_PTR)pc,
                         (DWORD)(pc - (DWORD64)(ULONG_PTR)base),
                         (unsigned long)count );

            rva = (DWORD)(pc - (DWORD64)(ULONG_PTR)base);
            lo = funcs; hi = funcs + count;
            while (lo < hi)
            {
                mid = lo + (hi - lo) / 2;
                if (rva < mid->BeginAddress) { hi = mid; }
                else if (rva < MACRUNNER_HB_ARM64_FUNC_END(mid, base))
                {
                    *out_image_base = (DWORD64)(ULONG_PTR)base;
                    return mid;
                }
                else { lo = mid + 1; }
            }
            return NULL;
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY
    return NULL;
}

static BOOL macrunner_hb_arm64_call_insn( DWORD op )
{
    return (op & 0xfc000000u) == 0x94000000u ||   /* bl */
           (op & 0xfffffc1fu) == 0xd63f0000u;     /* blr xN */
}

static BOOL macrunner_hb_arm64_lr_preindex_slot( DWORD op, DWORD *slot, DWORD *frame_size )
{
    if ((op & 0xffe00fffu) == 0xf8000ffeu)        /* str x30, [sp, #imm]! */
    {
        int imm = ((int)((op >> 12) & 0x1ff) << 23) >> 23;
        if (imm >= 0 || imm < -0x1000 || (imm & 0xf)) return FALSE;
        *slot = 0;
        *frame_size = (DWORD)-imm;
        return TRUE;
    }

    if ((op & 0xffc003e0u) == 0xa98003e0u)        /* stp xA, xB, [sp, #imm]! */
    {
        unsigned int rt = op & 0x1f, rt2 = (op >> 10) & 0x1f;
        int imm = (((int)((op >> 15) & 0x7f) << 25) >> 25) * 8;
        if (imm >= 0 || imm < -0x1000 || (imm & 0xf)) return FALSE;
        if (rt == 30) *slot = 0;
        else if (rt2 == 30) *slot = 8;
        else return FALSE;
        *frame_size = (DWORD)-imm;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_arm64_sp_preindex_frame( DWORD op, DWORD *frame_size )
{
    if ((op & 0xffc003e0u) == 0xa98003e0u)        /* stp xA, xB, [sp, #imm]! */
    {
        int imm = (((int)((op >> 15) & 0x7f) << 25) >> 25) * 8;
        if (imm >= 0 || imm < -0x1000 || (imm & 0xf)) return FALSE;
        *frame_size = (DWORD)-imm;
        return TRUE;
    }
    return FALSE;
}

static BOOL macrunner_hb_arm64_sp_sub_imm( DWORD op, DWORD *size )
{
    if ((op & 0xff8003ffu) == 0xd10003ffu)        /* sub sp, sp, #imm */
    {
        DWORD imm = (op >> 10) & 0xfff;
        if (op & 0x00400000u) imm <<= 12;
        if (!imm || imm > 0x20000 || (imm & 0xf)) return FALSE;
        *size = imm;
        return TRUE;
    }
    return FALSE;
}

static BOOL macrunner_hb_arm64_lr_sp_offset_slot( DWORD op, DWORD *slot )
{
    if ((op & 0xffc003e0u) == 0xa90003e0u)        /* stp xA, xB, [sp, #imm] */
    {
        unsigned int rt = op & 0x1f, rt2 = (op >> 10) & 0x1f;
        int imm = (((int)((op >> 15) & 0x7f) << 25) >> 25) * 8;
        if (imm < 0 || imm > 0x1000) return FALSE;
        if (rt == 30) *slot = (DWORD)imm;
        else if (rt2 == 30) *slot = (DWORD)(imm + 8);
        else return FALSE;
        return TRUE;
    }

    if ((op & 0xffc003e0u) == 0xf90003e0u && (op & 0x1f) == 30) /* str x30, [sp, #imm] */
    {
        *slot = ((op >> 10) & 0xfff) * 8;
        return TRUE;
    }

    return FALSE;
}

static BOOL macrunner_hb_arm64_no_pdata_frameless_unwind( DISPATCHER_CONTEXT *dispatch,
                                                          CONTEXT *context, DWORD64 pc )
{
    ULONG_PTR stack_lo = (ULONG_PTR)NtCurrentTeb()->Tib.StackLimit;
    ULONG_PTR stack_hi = (ULONG_PTR)NtCurrentTeb()->Tib.StackBase;
    ULONG_PTR sp = (ULONG_PTR)context->Sp;
    DWORD64 saved_lr = 0;
    DWORD slot = 0, frame_size = 0, scan;
    BOOL call_site = FALSE;

    if (pc < 0x10000 || (pc & 3)) return FALSE;
    if (!sp || (sp & 0xf) || sp < stack_lo || sp + 0x10 > stack_hi) return FALSE;

    __TRY
    {
        if (macrunner_hb_arm64_call_insn( *(const DWORD *)(ULONG_PTR)pc ))
            call_site = TRUE;
        else if (pc >= 4 && macrunner_hb_arm64_call_insn( *(const DWORD *)(ULONG_PTR)(pc - 4) ))
            call_site = TRUE;
        if (!call_site) return FALSE;

        for (scan = 1; scan <= 256 && pc >= scan * 4; scan++)
        {
            DWORD64 insn_pc = pc - scan * 4;
            DWORD op = *(const DWORD *)(ULONG_PTR)insn_pc;
            DWORD fwd, alloc;

            if (!macrunner_hb_arm64_lr_preindex_slot( op, &slot, &frame_size ))
            {
                BOOL found_lr_save = FALSE;

                if (!macrunner_hb_arm64_sp_preindex_frame( op, &frame_size ) &&
                    !macrunner_hb_arm64_sp_sub_imm( op, &frame_size ))
                    continue;
                for (fwd = 4; fwd <= 64 && insn_pc + fwd < pc; fwd += 4)
                {
                    DWORD fop = *(const DWORD *)(ULONG_PTR)(insn_pc + fwd);
                    if (macrunner_hb_arm64_lr_sp_offset_slot( fop, &slot ))
                    {
                        found_lr_save = TRUE;
                        break;
                    }
                }
                if (!found_lr_save || slot + sizeof(saved_lr) > frame_size) continue;
            }
            for (fwd = 4; fwd <= 1024 && insn_pc + fwd < pc; fwd += 4)
            {
                DWORD fop = *(const DWORD *)(ULONG_PTR)(insn_pc + fwd);
                if (macrunner_hb_arm64_sp_sub_imm( fop, &alloc ))
                {
                    if (frame_size > 0x20000 - alloc || slot > 0x20000 - alloc)
                        return FALSE;
                    frame_size += alloc;
                    slot += alloc;
                }
            }

            if (sp + frame_size > stack_hi) return FALSE;
            if (sp + slot + sizeof(saved_lr) > stack_hi) return FALSE;
            saved_lr = *(const DWORD64 *)(sp + slot);
            {
                const char *bad_pc = NULL, *same_reason = NULL;
                if (!macrunner_hb_is_plausible_recovered_pc( saved_lr, &bad_pc ))
                {
                    static unsigned int bad_count;
                    if (bad_count++ < 32)
                        MESSAGE( "macrunner-hb-wow64-arm64-frameless-bail: reason=%s "
                                 "pc=%p image=%p saved_lr=%p sp=%016I64x frame=%lu slot=%lu "
                                 "stack=%p-%p\n",
                                 bad_pc, (void *)(ULONG_PTR)pc,
                                 (void *)(ULONG_PTR)dispatch->ImageBase,
                                 (void *)(ULONG_PTR)saved_lr, (DWORD64)sp,
                                 (unsigned long)frame_size, (unsigned long)slot,
                                 (void *)stack_lo, (void *)stack_hi );
                    return FALSE;
                }
                if (saved_lr == pc || saved_lr == context->Lr)
                {
                    static unsigned int same_count;
                    same_reason = saved_lr == pc ? "pc-no-progress" : "pc-same-live-lr";
                    if (same_count++ < 32)
                        MESSAGE( "macrunner-hb-wow64-arm64-frameless-bail: reason=%s "
                                 "pc=%p image=%p saved_lr=%p sp=%016I64x frame=%lu slot=%lu "
                                 "stack=%p-%p\n",
                                 same_reason, (void *)(ULONG_PTR)pc,
                                 (void *)(ULONG_PTR)dispatch->ImageBase,
                                 (void *)(ULONG_PTR)saved_lr, (DWORD64)sp,
                                 (unsigned long)frame_size, (unsigned long)slot,
                                 (void *)stack_lo, (void *)stack_hi );
                    return FALSE;
                }
            }

            context->Sp = sp + frame_size;
            context->Lr = saved_lr;
            context->Pc = saved_lr;
            context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
            dispatch->EstablisherFrame = context->Sp;
            dispatch->LanguageHandler = NULL;
            dispatch->HandlerData = NULL;

            {
                static unsigned int trace_count;
                if (trace_count++ < 16)
                    MESSAGE( "macrunner-hb-wow64-arm64-frameless: pc=%p image=%p "
                             "saved_lr=%p sp=%016I64x frame=%lu slot=%lu\n",
                             (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)dispatch->ImageBase,
                             (void *)(ULONG_PTR)saved_lr, (DWORD64)sp,
                             (unsigned long)frame_size, (unsigned long)slot );
            }
            return TRUE;
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY

    return FALSE;
}

static NTSTATUS virtual_unwind( ULONG type, DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 *nonvol_regs;
    DWORD64 pc;
    DWORD64 raw_pc;
    DWORD64 lookup_pc;
    DWORD64 unwind_lr, unwind_sp, unwind_fp;
    CONTEXT unwind_context;
    void *unwind_entry;
    NTSTATUS status;
    int i;

restart:
    pc = context->Pc;
    raw_pc = pc;
    dispatch->ScopeIndex = 0;
    dispatch->ControlPc  = pc;
    dispatch->ControlPcIsUnwound = (context->ContextFlags & CONTEXT_UNWOUND_TO_CALL) != 0;
    if (dispatch->ControlPcIsUnwound) pc -= 4;
    lookup_pc = macrunner_hb_normalize_arm64ec_host_pc( pc );

    nonvol_regs = (DISPATCHER_CONTEXT_NONVOLREG_ARM64 *)dispatch->NonVolatileRegisters;
    memcpy( nonvol_regs->GpNvRegs, &context->X19, sizeof(nonvol_regs->GpNvRegs) );
    for (i = 0; i < 8; i++) nonvol_regs->FpNvRegs[i] = context->V[i + 8].D[0];

    if (lookup_pc != pc)
    {
        dispatch->FunctionEntry = RtlLookupFunctionEntry( lookup_pc, &dispatch->ImageBase, dispatch->HistoryTable );
        if (dispatch->FunctionEntry || dispatch->ImageBase)
        {
            dispatch->ControlPc = lookup_pc;
            pc = lookup_pc;
            goto unwind_with_function_entry;
        }
    }

    if (macrunner_hb_is_import_thunk_pc( pc ))
    {
        TRACE( "stopping at MacRunner HyperBridge import thunk pc %p lr %p\n",
               (void *)pc, (void *)context->Lr );
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    if (macrunner_hb_is_unix_dispatcher_boundary_pc( pc ))
    {
        TRACE( "stopping at MacRunner HyperBridge Unix dispatcher boundary pc %p lr %p syscall=%p unix=%p\n",
               (void *)pc, (void *)context->Lr, __wine_syscall_dispatcher, __wine_unix_call_dispatcher );
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    if (macrunner_hb_is_non_module_host_boundary_pc( pc ))
    {
        static unsigned int report_count;
        const EXCEPTION_RECORD *rec = macrunner_hb_current_exception_record;
        struct macrunner_hb_syscall_frame *frame = macrunner_hb_current_syscall_frame();
        LDR_DATA_TABLE_ENTRY *module = NULL;
        NTSTATUS ldr_status = LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module );
        DWORD tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );

        if (report_count++ < 64)
        {
            MESSAGE( "macrunner-hb-seh-host-boundary: pc=%p lr=%p sp=%016I64x\n",
                     (void *)pc, (void *)context->Lr, context->Sp );
            MESSAGE( "macrunner-hb-seh-host-boundary-detail: side=arm64 tid=%04lx pc=%p lr=%p "
                     "sp=%016I64x exception=%08lx flags=%08lx ldr_status=%08lx module=%p "
                     "exc_addr=%p info0=%Ix info1=%p "
                     "resume=stop-unwind frame=%p frame_pc=%p frame_lr=%p frame_sp=%p "
                     "frame_prev=%p frame_cfa=%p frame_flags=%08lx\n",
                     tid, (void *)pc, (void *)context->Lr, context->Sp,
                     rec ? rec->ExceptionCode : 0, rec ? rec->ExceptionFlags : 0,
                     ldr_status, module ? module->DllBase : NULL,
                     rec ? rec->ExceptionAddress : NULL,
                     (ULONG_PTR)(rec && rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0),
                     (void *)(ULONG_PTR)(rec && rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0),
                     frame,
                     frame ? (void *)(ULONG_PTR)frame->pc : NULL,
                     frame ? (void *)(ULONG_PTR)frame->lr : NULL,
                     frame ? (void *)(ULONG_PTR)frame->sp : NULL,
                      frame ? frame->prev_frame : NULL,
                      frame ? frame->syscall_cfa : NULL,
                      frame ? frame->restore_flags : 0 );
            /* MacRunner diag: this boundary is a native libsystem_platform call (e.g.
             * _platform_memmove) faulting on a guest pointer.  Dump x0..x8 (memmove
             * dst=x0 src=x1 len=x2) + lr + a stack window so the bad buffer/length and
             * the Wine caller can be recovered from the run log. */
            {
                const ULONG64 *stk = (const ULONG64 *)(ULONG_PTR)context->Sp;
                unsigned si;
                MESSAGE( "macrunner-hb-seh-nonmod-regs: x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p "
                         "x6=%p x7=%p x8=%p x9=%p x16=%p x17=%p x18=%p lr=%p fp=%p sp=%p\n",
                         (void *)context->X[0], (void *)context->X[1], (void *)context->X[2],
                         (void *)context->X[3], (void *)context->X[4], (void *)context->X[5],
                         (void *)context->X[6], (void *)context->X[7], (void *)context->X[8],
                         (void *)context->X[9], (void *)context->X[16], (void *)context->X[17],
                         (void *)context->X[18], (void *)context->Lr, (void *)context->Fp,
                         (void *)context->Sp );
                for (si = 0; si < 24; si += 4)
                    MESSAGE( "macrunner-hb-seh-nonmod-stk: +%02x %p %p %p %p\n",
                             si * 8, (void *)stk[si], (void *)stk[si+1],
                             (void *)stk[si+2], (void *)stk[si+3] );
            }
        }
        if (macrunner_hb_unwind_syscall_data_boundary( dispatch, context, frame ))
            return STATUS_SUCCESS;
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    if (macrunner_hb_is_host_boundary_pc( raw_pc, context ) ||
        macrunner_hb_is_null_lr_boundary( raw_pc, context ) ||
        macrunner_hb_is_non_module_host_boundary_pc( pc ))
    {
        static unsigned int report_count;
        const EXCEPTION_RECORD *rec = macrunner_hb_current_exception_record;
        LDR_DATA_TABLE_ENTRY *module = NULL;
        LDR_DATA_TABLE_ENTRY *lr_module = NULL;
        LDR_DATA_TABLE_ENTRY *x17_module = NULL;
        NTSTATUS ldr_status = LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module );
        NTSTATUS lr_ldr_status = LdrFindEntryForAddress( (void *)(ULONG_PTR)context->Lr, &lr_module );
        NTSTATUS x17_ldr_status =
            LdrFindEntryForAddress( (void *)(ULONG_PTR)context->X[17], &x17_module );
        DWORD tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );

        if (report_count++ < 64)
        {
            MESSAGE( "macrunner-hb-seh-host-boundary: pc=%p lr=%p sp=%016I64x\n",
                     (void *)pc, (void *)context->Lr, context->Sp );
            MESSAGE( "macrunner-hb-seh-host-boundary-detail: side=arm64 tid=%04lx pc=%p lr=%p "
                     "sp=%016I64x exception=%08lx flags=%08lx ldr_status=%08lx module=%p "
                     "exc_addr=%p info0=%Ix info1=%p resume=stop-unwind\n",
                     tid, (void *)pc, (void *)context->Lr, context->Sp,
                     rec ? rec->ExceptionCode : 0, rec ? rec->ExceptionFlags : 0,
                     ldr_status, module ? module->DllBase : NULL,
                     rec ? rec->ExceptionAddress : NULL,
                     (ULONG_PTR)(rec && rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0),
                     (void *)(ULONG_PTR)(rec && rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0) );
            if (lr_module)
            {
                const ULONG *insn = (const ULONG *)(ULONG_PTR)context->Lr;
                MESSAGE( "macrunner-hb-seh-nullcall-native: lr_status=%08lx module=%s base=%p "
                         "lr_rva=%Ix insn[-4..0]=%08lx/%08lx/%08lx/%08lx/%08lx "
                         "x17_status=%08lx x17_module=%s x17_base=%p x17_rva=%Ix x17_value=%p\n",
                         lr_ldr_status, debugstr_w( lr_module->BaseDllName.Buffer ),
                         lr_module->DllBase,
                         context->Lr - (ULONG64)(ULONG_PTR)lr_module->DllBase,
                         insn[-4], insn[-3], insn[-2], insn[-1], insn[0],
                         x17_ldr_status,
                         x17_module ? debugstr_w( x17_module->BaseDllName.Buffer ) : "(none)",
                         x17_module ? x17_module->DllBase : NULL,
                         x17_module ? context->X[17] - (ULONG64)(ULONG_PTR)x17_module->DllBase : 0,
                         x17_module && context->X[17] + sizeof(ULONG64) <=
                             (ULONG64)(ULONG_PTR)x17_module->DllBase + x17_module->SizeOfImage
                             ? (void *)*(const ULONG64 *)(ULONG_PTR)context->X[17] : NULL );
            }
            /* MacRunner diag: a NULL-call boundary (exc_addr=0, EXECUTE) carries the
             * original fault registers here (first unwind step).  Dump them + a stack
             * window so the guest return address (a guest-range value, the call site)
             * can be recovered from the run log. */
            if (rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                !rec->ExceptionAddress)
            {
                const ULONG64 *stk = (const ULONG64 *)(ULONG_PTR)context->Sp;
                unsigned i;
                MESSAGE( "macrunner-hb-seh-host-boundary-regs: x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p "
                         "x6=%p x7=%p x8=%p x9=%p x16=%p x17=%p x18=%p x19=%p x20=%p\n",
                         (void *)context->X[0], (void *)context->X[1], (void *)context->X[2],
                         (void *)context->X[3], (void *)context->X[4], (void *)context->X[5],
                         (void *)context->X[6], (void *)context->X[7], (void *)context->X[8],
                         (void *)context->X[9], (void *)context->X[16], (void *)context->X[17],
                         (void *)context->X[18], (void *)context->X[19], (void *)context->X[20] );
                MESSAGE( "macrunner-hb-seh-host-boundary-regs2: x21=%p x22=%p x23=%p x24=%p x25=%p "
                         "x26=%p x27=%p x28=%p fp=%p lr=%p sp=%p\n",
                         (void *)context->X[21], (void *)context->X[22], (void *)context->X[23],
                         (void *)context->X[24], (void *)context->X[25], (void *)context->X[26],
                         (void *)context->X[27], (void *)context->X[28], (void *)context->Fp,
                         (void *)context->Lr, (void *)context->Sp );
                for (i = 0; i < 32; i += 4)
                    MESSAGE( "macrunner-hb-seh-host-boundary-stk: +%02x %p %p %p %p\n",
                             i * 8, (void *)stk[i], (void *)stk[i+1],
                             (void *)stk[i+2], (void *)stk[i+3] );
            }
            /* The NULL call is in UnityPlayer's WndProc (rva 0x7d5170), which calls
             * USER32 imports through its IAT.  Resolve the UnityPlayer module from a
             * guest pointer (x1/x5/x19 hold guest addresses) and dump the suspect
             * IAT slots so a NULL (unresolved) import is identified directly. */
            if (rec && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !rec->ExceptionAddress)
            {
                ULONG64 probes[3] = { context->X[5], context->X[1], context->X[19] };
                PEB_LDR_DATA *pldr = NtCurrentTeb()->Peb->LdrData;
                void *upbase = NULL;
                unsigned pi;
                for (pi = 0; pi < 3 && !upbase && pldr; pi++)
                {
                    LIST_ENTRY *le;
                    for (le = pldr->InLoadOrderModuleList.Flink;
                         le != &pldr->InLoadOrderModuleList; le = le->Flink)
                    {
                        LDR_DATA_TABLE_ENTRY *m =
                            CONTAINING_RECORD( le, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );
                        ULONG_PTR b = (ULONG_PTR)m->DllBase;
                        if (probes[pi] >= b && probes[pi] < b + m->SizeOfImage)
                        { upbase = m->DllBase; break; }
                    }
                }
                if (upbase)
                {
                    char *u = (char *)upbase;
                    ULONG64 base64 = (ULONG64)(ULONG_PTR)upbase;
                    unsigned ri;
                    static const int rix[5] = { 4, 3, 28, 25, 6 };
                    MESSAGE( "macrunner-hb-nullcall-iat: module_base=%p "
                             "GetParent[1a01c18]=%p ValidateRect[1a01c20]=%p "
                             "GetWindowRect[1a01cc8]=%p IsIconic[1a01ce8]=%p\n",
                             upbase,
                             (void *)*(ULONG64 *)(u + 0x1a01c18),
                             (void *)*(ULONG64 *)(u + 0x1a01c20),
                             (void *)*(ULONG64 *)(u + 0x1a01cc8),
                             (void *)*(ULONG64 *)(u + 0x1a01ce8) );
                    /* The fault is call [dxgi!CreateDXGIFactory2 IAT slot 0x1a02098].
                     * Dump the runtime values of the critical dxgi/d3d11 import slots
                     * to confirm which graphics import is NULL (unresolved). */
                    MESSAGE( "macrunner-hb-nullcall-gfximports: D3D11On12CreateDevice[1a02068]=%p "
                             "D3D11CreateDevice[1a02070]=%p CreateDXGIFactory2[1a02098]=%p "
                             "CreateDXGIFactory[1a020a0]=%p\n",
                             (void *)*(ULONG64 *)(u + 0x1a02068),
                             (void *)*(ULONG64 *)(u + 0x1a02070),
                             (void *)*(ULONG64 *)(u + 0x1a02098),
                             (void *)*(ULONG64 *)(u + 0x1a020a0) );
                    /* The IAT slots are x64->EC thunks; read each thunk's code words +
                     * its embedded EC target (typ. thunk+8) — a 0 target = unbound EC
                     * export => the execute-at-0 in the EC dispatch. */
                    {
                        ULONG64 ts[4];
                        unsigned ti;
                        ts[0] = *(ULONG64 *)(u + 0x1a02068); ts[1] = *(ULONG64 *)(u + 0x1a02070);
                        ts[2] = *(ULONG64 *)(u + 0x1a02098); ts[3] = *(ULONG64 *)(u + 0x1a020a0);
                        for (ti = 0; ti < 4; ti++)
                        {
                            const ULONG64 *t = (const ULONG64 *)(ULONG_PTR)ts[ti];
                            static const char *nm[4] = { "D3D11On12", "D3D11Create",
                                                          "CreateDXGIFactory2", "CreateDXGIFactory" };
                            if (ts[ti] < 0x10000) { MESSAGE( "macrunner-hb-gfxthunk: %s thunk=%p (low)\n", nm[ti], (void *)(ULONG_PTR)ts[ti] ); continue; }
                            MESSAGE( "macrunner-hb-gfxthunk: %s thunk=%p w0=%016llx w1=%016llx w2=%016llx w3=%016llx\n",
                                     nm[ti], (void *)(ULONG_PTR)ts[ti],
                                     (unsigned long long)t[0], (unsigned long long)t[1],
                                     (unsigned long long)t[2], (unsigned long long)t[3] );
                            /* Follow this thunk's EC target (w1 = thunk+8 literal):
                             * identify its module+rva and disasm the first insns.
                             * If the EC entry itself br's 0 (or is a bad address),
                             * this names the broken x64->EC forward target. */
                            {
                                ULONG64 tgt = t[1];
                                LDR_DATA_TABLE_ENTRY *tm = NULL; ULONG_PTR trva = 0; char tn[64] = "-";
                                unsigned int ins[8] = {0};
                                if (tgt && LdrFindEntryForAddress( (void *)(ULONG_PTR)tgt, &tm ) == STATUS_SUCCESS && tm)
                                { trva = (ULONG_PTR)tgt - (ULONG_PTR)tm->DllBase;
                                  macrunner_hb_copy_unicode_ascii( tn, sizeof(tn), &tm->BaseDllName ); }
                                __TRY { const unsigned int *c = (const unsigned int *)(ULONG_PTR)tgt;
                                        ins[0]=c[0];ins[1]=c[1];ins[2]=c[2];ins[3]=c[3];
                                        ins[4]=c[4];ins[5]=c[5];ins[6]=c[6];ins[7]=c[7]; }
                                __EXCEPT(macrunner_hb_pe_scan_fault) { ins[0]=0xdeadbeef; }
                                __ENDTRY
                                MESSAGE( "macrunner-hb-gfxtarget: %s ec_target=%p module=%s rva=0x%llx "
                                         "insns=%08x %08x %08x %08x %08x %08x %08x %08x\n",
                                         nm[ti], (void *)(ULONG_PTR)tgt, tn, (unsigned long long)trva,
                                         ins[0],ins[1],ins[2],ins[3],ins[4],ins[5],ins[6],ins[7] );
                            }
                        }
                    }
                    /* Scan candidate guest stack pointers (x3/x4/x28/...) for the
                     * return address into UnityPlayer's WndProc -> the exact call
                     * site.  A guest-range qword (module_base..+0x2200000) with the
                     * WndProc rva is the return after the faulting indirect call. */
                    for (ri = 0; ri < 5; ri++)
                    {
                        ULONG64 p = context->X[rix[ri]];
                        const ULONG64 *w;
                        unsigned k;
                        if (p < 0x10000 || p >= 0x900000000000ULL || (p & 7)) continue;
                        w = (const ULONG64 *)(ULONG_PTR)p;
                        for (k = 0; k < 48; k++)
                        {
                            ULONG64 v = w[k];
                            if (v > base64 && v < base64 + 0x2200000)
                                MESSAGE( "macrunner-hb-nullcall-ret: x%d=%p +0x%x val=%p rva=0x%llx\n",
                                         rix[ri], (void *)(ULONG_PTR)p, k * 8,
                                         (void *)(ULONG_PTR)v,
                                         (unsigned long long)(v - base64) );
                        }
                    }
                    /* The WM_PAINT handler (rva 0x7d520a) calls vtable methods on a
                     * thread-local object (rbx = x27 in the ARM64EC map; from 0x6c7860
                     * TlsGetValue).  Read [obj]=vtable and the called slots to find the
                     * NULL method + the non-null methods' rva (identifies the class). */
                    {
                        static const int orx[6] = { 27, 0, 25, 26, 3, 19 };
                        static const unsigned voff[5] = { 0x678, 0x680, 0x690, 0x698, 0x6b0 };
                        unsigned oi, vi;
                        for (oi = 0; oi < 6; oi++)
                        {
                            ULONG64 obj = context->X[orx[oi]], vt;
                            if (obj < 0x10000 || obj >= 0x900000000000ULL || (obj & 7)) continue;
                            vt = *(ULONG64 *)(ULONG_PTR)obj;
                            if (vt < 0x10000 || vt >= 0x900000000000ULL || (vt & 7)) continue;
                            for (vi = 0; vi < 5; vi++)
                            {
                                ULONG64 mp = *(ULONG64 *)(ULONG_PTR)(vt + voff[vi]);
                                MESSAGE( "macrunner-hb-nullcall-vtable: obj=x%d=%p vtable=%p rva=0x%llx "
                                         "+0x%x=%p%s%s\n",
                                         orx[oi], (void *)(ULONG_PTR)obj, (void *)(ULONG_PTR)vt,
                                         (vt > base64 && vt < base64 + 0x2200000) ?
                                             (unsigned long long)(vt - base64) : 0ULL,
                                         voff[vi], (void *)(ULONG_PTR)mp,
                                         mp ? "" : " <<NULL",
                                         (mp > base64 && mp < base64 + 0x2200000) ?
                                             " UP" : "" );
                            }
                        }
                    }
                }
                else
                    MESSAGE( "macrunner-hb-nullcall-iat: UnityPlayer module not found "
                             "(probes %p %p %p)\n",
                             (void *)probes[0], (void *)probes[1], (void *)probes[2] );
            }
        }
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    dispatch->FunctionEntry = RtlLookupFunctionEntry( pc, &dispatch->ImageBase, dispatch->HistoryTable );
    if (!dispatch->FunctionEntry && pc < MACRUNNER_HB_HOST_BOUNDARY_MAX &&
        macrunner_hb_try_arm64_unwind_methods( dispatch, context, pc ))
        return STATUS_SUCCESS;

    /* ARM64 PE modules loaded above HOST_BOUNDARY_MAX are not always tracked by the
     * PE-side LDR, so RtlLookupFunctionEntry can miss valid ARM64/ARM64X pdata. */
    if (!dispatch->FunctionEntry && pc >= MACRUNNER_HB_HOST_BOUNDARY_MAX)
    {
        if (dispatch->ImageBase)
            dispatch->FunctionEntry = macrunner_hb_pdata_lookup_at_base(
                pc, dispatch->ImageBase, &dispatch->ImageBase );
        if (!dispatch->FunctionEntry)
            dispatch->FunctionEntry = macrunner_hb_register_wow64_arm64_pe_pdata(
                pc, &dispatch->ImageBase );
        if (!dispatch->FunctionEntry && pc >= MACRUNNER_HB_HOST_BOUNDARY_MAX + 4)
        {
            DWORD64 return_pc = pc - 4;
            DWORD64 return_image = dispatch->ImageBase;

            if (return_image)
                dispatch->FunctionEntry = macrunner_hb_pdata_lookup_at_base(
                    return_pc, return_image, &return_image );
            if (!dispatch->FunctionEntry)
                dispatch->FunctionEntry = macrunner_hb_register_wow64_arm64_pe_pdata(
                    return_pc, &return_image );
            if (dispatch->FunctionEntry)
            {
                static unsigned int return_lookup_count;
                if (return_lookup_count++ < 16)
                    MESSAGE( "macrunner-hb-wow64-arm64-pdata: return-address pc=%p "
                             "lookup_pc=%p image=%p function=%p\n",
                             (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)return_pc,
                             (void *)(ULONG_PTR)return_image, dispatch->FunctionEntry );
                pc = return_pc;
                dispatch->ControlPc = pc;
                dispatch->ImageBase = return_image;
            }
        }
    }
    /* Leaf function with no .pdata entry (normal for thunks).  Stop unwind via the
     * ordered fallback chain rather than letting RtlVirtualUnwind2 raise c0000026. */
    if (!dispatch->FunctionEntry && pc >= MACRUNNER_HB_HOST_BOUNDARY_MAX)
    {
        static unsigned int wow64_leaf_count;
        static unsigned int wow64_x64_range_count;
        BOOL resumed;

        if (dispatch->ImageBase &&
            macrunner_hb_arm64x_code_range_kind( dispatch->ImageBase, pc ) == 0 &&
            macrunner_hb_current_exception_is_datatype_misalignment())
        {
            if (wow64_x64_range_count++ < 16)
                MESSAGE( "macrunner-hb-wow64-arm64-leaf: pc=%p image=%p "
                         "x64-range stop-unwind\n",
                         (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)dispatch->ImageBase );
            macrunner_hb_stop_unwind_at_boundary( dispatch, context );
            return STATUS_SUCCESS;
        }
        if (macrunner_hb_try_arm64_unwind_methods( dispatch, context, pc ))
            return STATUS_SUCCESS;
        resumed = macrunner_hb_unwind_leaf_via_lr( dispatch, context, pc, context->Lr );
        if (wow64_leaf_count++ < 16)
            MESSAGE( "macrunner-hb-wow64-arm64-leaf: pc=%p lr=%p image=%p "
                     "%s\n",
                     (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)context->Lr,
                     (void *)(ULONG_PTR)dispatch->ImageBase,
                     resumed ? "resume=lr" : "stopping-unwind" );
        if (resumed) return STATUS_SUCCESS;
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    if (!dispatch->FunctionEntry)
    {
        /* (c) EC-unwind: guest-arena module aliases are execution-views NOT in the
         * loader list (by design — registering would dup module identity), so
         * RtlLookupFunctionEntry can't find their exception data even though it is
         * present in the image.  Consult the arena exception index (native
         * RtlAddFunctionTable-style dynamic function tables, populated at arena
         * map-time) to recover the real RUNTIME_FUNCTION + image base; then the
         * normal RtlVirtualUnwind2 below unwinds the frame with genuine .pdata. */
        macrunner_hb_arena_consult_function_entry( pc, (RUNTIME_FUNCTION **)&dispatch->FunctionEntry,
                                                   &dispatch->ImageBase );
    }

unwind_with_function_entry:
    unwind_lr = context->Lr;
    unwind_sp = context->Sp;
    unwind_fp = context->Fp;
    unwind_context = *context;
    unwind_entry = dispatch->FunctionEntry;
    status = RtlVirtualUnwind2( type, dispatch->ImageBase, pc, dispatch->FunctionEntry, context,
                                NULL, &dispatch->HandlerData, &dispatch->EstablisherFrame,
                                NULL, NULL, NULL, &dispatch->LanguageHandler, 0 );
    /* Native .pdata is ground truth: a successful RtlVirtualUnwind2 driven by a
     * genuine native FunctionEntry must NOT be discarded merely because the
     * ARM64X CodeMap misclassifies pc as EC (kind=0) — a known runtime defect of
     * lld-built ARM64X hybrids (e.g. DXMT dxgi: a native rva reads kind=0 at
     * runtime even though it sits in the ARM64/native CodeMap range with valid
     * .pdata). Only fall to the heuristic EC fallbacks when there was no native
     * entry or the unwind did not succeed; the no-progress check just below
     * stays as the safety net for a genuinely bogus result. */
    if (macrunner_hb_pc_unsafe_for_arm64_unwind( dispatch, &unwind_context, pc ) &&
        !(unwind_entry && status == STATUS_SUCCESS))
    {
        *context = unwind_context;
        macrunner_hb_try_arm64_unwind_methods( dispatch, context, pc );
        return STATUS_SUCCESS;
    }
    if (macrunner_hb_unwind_made_no_progress( pc, unwind_sp, unwind_fp, context ))
    {
        static unsigned int no_progress_count;
        BOOL resumed;
        char sec_name[9];
        DWORD sec_chars;

        macrunner_hb_find_pc_section( context->Pc, sec_name, &sec_chars );
        if (no_progress_count++ < 32)
            MESSAGE( "macrunner-hb-unwind-no-progress: pc=%p after_pc=%p lr=%p "
                     "status=%08lx image=%p function=%p before_sp=%016I64x "
                     "after_sp=%016I64x before_fp=%p after_fp=%p section=%s chars=%08lx\n",
                     (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)context->Pc,
                     (void *)(ULONG_PTR)unwind_lr, status, (void *)(ULONG_PTR)dispatch->ImageBase,
                     dispatch->FunctionEntry, unwind_sp, context->Sp,
                     (void *)(ULONG_PTR)unwind_fp, (void *)(ULONG_PTR)context->Fp,
                     sec_name, sec_chars );

        if (macrunner_hb_try_arm64_unwind_methods( dispatch, context, pc ))
            return STATUS_SUCCESS;
        resumed = macrunner_hb_unwind_leaf_via_lr( dispatch, context, pc, unwind_lr );
        if (resumed) return STATUS_SUCCESS;
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }
    if (status != STATUS_SUCCESS)
    {
        if (!dispatch->FunctionEntry &&
            macrunner_hb_try_arm64_unwind_methods( dispatch, context, pc ))
            return STATUS_SUCCESS;
        if (macrunner_hb_trace_arm64_seh_invalid_disposition())
            ERR( "macrunner-hb-seh-invalid: reason=unwind-metadata-missing pc=%p lr=%p "
                 "type=%lu image=%p function=%p sp=%016I64x stack=%p-%p\n",
                 (void *)pc, (void *)context->Lr, (unsigned long)type,
                 (void *)dispatch->ImageBase, dispatch->FunctionEntry,
                 context->Sp, NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
        WARN( "exception data not found for pc %p, lr %p\n", (void *)pc, (void *)context->Lr );
        return STATUS_INVALID_DISPOSITION;
    }
    return STATUS_SUCCESS;
}


/**********************************************************************
 *           unwind_exception_handler
 *
 * Handler for exceptions happening while calling an unwind handler.
 */
EXCEPTION_DISPOSITION WINAPI unwind_exception_handler( EXCEPTION_RECORD *record, void *frame,
                                                       CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    DISPATCHER_CONTEXT *orig_dispatch = ((DISPATCHER_CONTEXT **)frame)[-2];

    /* copy the original dispatcher into the current one, except for the TargetPc */
    dispatch->ControlPc          = orig_dispatch->ControlPc;
    dispatch->ImageBase          = orig_dispatch->ImageBase;
    dispatch->FunctionEntry      = orig_dispatch->FunctionEntry;
    dispatch->EstablisherFrame   = orig_dispatch->EstablisherFrame;
    dispatch->LanguageHandler    = orig_dispatch->LanguageHandler;
    dispatch->HandlerData        = orig_dispatch->HandlerData;
    dispatch->HistoryTable       = orig_dispatch->HistoryTable;
    dispatch->ScopeIndex         = orig_dispatch->ScopeIndex;
    dispatch->ControlPcIsUnwound = orig_dispatch->ControlPcIsUnwound;
    *dispatch->ContextRecord     = *orig_dispatch->ContextRecord;
    memcpy( dispatch->NonVolatileRegisters, orig_dispatch->NonVolatileRegisters,
            sizeof(DISPATCHER_CONTEXT_NONVOLREG_ARM64) );
    TRACE( "detected collided unwind\n" );
    return ExceptionCollidedUnwind;
}


/**********************************************************************
 *           call_unwind_handler
 */
DWORD WINAPI call_unwind_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                                  CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler );
__ASM_GLOBAL_FUNC( call_unwind_handler,
                   "stp x29, x30, [sp, #-32]!\n\t"
                   ".seh_save_fplr_x 32\n\t"
                   ".seh_endprologue\n\t"
                   ".seh_handler unwind_exception_handler, @except\n\t"
                   "str x3, [sp, #16]\n\t"    /* frame[-2] = dispatch */
                   "blr x4\n\t"
                   "ldp x29, x30, [sp], #32\n\t"
                   "ret" )




/*******************************************************************
 *         nested_exception_handler
 */
EXCEPTION_DISPOSITION WINAPI nested_exception_handler( EXCEPTION_RECORD *rec, void *frame,
                                                       CONTEXT *context, void *dispatch )
{
    if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) return ExceptionContinueSearch;
    return ExceptionNestedException;
}

static BOOL macrunner_hb_stack_overflow_repeat_guard( EXCEPTION_RECORD *rec, CONTEXT *orig_context )
{
    struct macrunner_hb_stack_overflow_seen
    {
        DWORD tid;
        DWORD64 pc;
        DWORD64 sp;
        DWORD64 addr;
        unsigned int repeat;
    };
    static struct macrunner_hb_stack_overflow_seen seen[32];
    static unsigned int report_count;
    TEB *teb = NtCurrentTeb();
    ULONG_PTR stack_lo = (ULONG_PTR)teb->Tib.StackLimit;
    ULONG_PTR stack_hi = (ULONG_PTR)teb->Tib.StackBase;
    DWORD tid = HandleToULong( teb->ClientId.UniqueThread );
    DWORD64 pc = orig_context->Pc;
    DWORD64 sp = orig_context->Sp;
    DWORD64 addr = (DWORD64)(ULONG_PTR)rec->ExceptionAddress;
    DWORD64 bt[4] = { orig_context->Lr, 0, 0, 0 };
    LDR_DATA_TABLE_ENTRY *module = NULL;
    ULONG_PTR module_base = 0, rva = 0;
    unsigned int i;
    struct macrunner_hb_stack_overflow_seen *slot = &seen[tid % ARRAY_SIZE(seen)];

    if (slot->tid == tid && slot->pc == pc && slot->sp == sp && slot->addr == addr) slot->repeat++;
    else
    {
        slot->tid = tid;
        slot->pc = pc;
        slot->sp = sp;
        slot->addr = addr;
        slot->repeat = 1;
    }

    if (LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module ) == STATUS_SUCCESS && module)
    {
        module_base = (ULONG_PTR)module->DllBase;
        rva = (ULONG_PTR)pc - module_base;
    }

    __TRY
    {
        ULONG_PTR fp = (ULONG_PTR)orig_context->Fp;
        for (i = 1; i < ARRAY_SIZE(bt); i++)
        {
            if (!fp || (fp & 0xf) || fp < stack_lo || fp + 0x10 > stack_hi) break;
            bt[i] = ((const DWORD64 *)fp)[1];
            fp = ((const DWORD64 *)fp)[0];
        }
    }
    __EXCEPT(macrunner_hb_pe_scan_fault) {}
    __ENDTRY

    if (report_count++ < 32)
        MESSAGE( "macrunner-hb-seh-stack-overflow-record: repeat=%u tid=%04lx addr=%p "
                 "flags=%08lx orig_pc=%p orig_lr=%p orig_sp=%016I64x fp=%p "
                 "module=%p rva=%08Ix stack=%p-%p bt=%p,%p,%p,%p params=%lu "
                 "info0=%016I64x info1=%016I64x%s\n",
                 slot->repeat, tid, rec->ExceptionAddress, rec->ExceptionFlags,
                 (void *)(ULONG_PTR)pc, (void *)(ULONG_PTR)orig_context->Lr, sp,
                 (void *)(ULONG_PTR)orig_context->Fp, (void *)module_base, rva,
                 (void *)stack_lo, (void *)stack_hi, (void *)(ULONG_PTR)bt[0],
                 (void *)(ULONG_PTR)bt[1], (void *)(ULONG_PTR)bt[2],
                 (void *)(ULONG_PTR)bt[3], rec->NumberParameters,
                 rec->NumberParameters > 0 ? rec->ExceptionInformation[0] : 0,
                 rec->NumberParameters > 1 ? rec->ExceptionInformation[1] : 0,
                 slot->repeat >= 2 ? " bail=repeat" : "" );

    return slot->repeat >= 2;
}

/***********************************************************************
 *		call_seh_handler
 */
DWORD WINAPI call_seh_handler( EXCEPTION_RECORD *rec, ULONG_PTR frame,
                               CONTEXT *context, void *dispatch, PEXCEPTION_ROUTINE handler );
__ASM_GLOBAL_FUNC( call_seh_handler,
                   "stp x29, x30, [sp, #-16]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   ".seh_endprologue\n\t"
                   ".seh_handler nested_exception_handler, @except\n\t"
                   "blr x4\n\t"
                   "ldp x29, x30, [sp], #16\n\t"
                   "ret" )


/**********************************************************************
 *           call_seh_handlers
 *
 * Call the SEH handlers.
 */
NTSTATUS call_seh_handlers( EXCEPTION_RECORD *rec, CONTEXT *orig_context )
{
    struct macrunner_hb_unwind_seen
    {
        DWORD64 pc;
        DWORD64 fp;
        DWORD64 sp;
    };
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    const EXCEPTION_RECORD *old_record = macrunner_hb_current_exception_record;
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 nonvol_regs;
    UNWIND_HISTORY_TABLE table;
    DISPATCHER_CONTEXT dispatch;
    struct macrunner_hb_unwind_seen unwind_seen[32];
    unsigned int unwind_seen_count = 0;
    CONTEXT *context;
    NTSTATUS status;
    ULONG_PTR frame;
    DWORD res;
    unsigned int i;

    macrunner_hb_trace_tagged_exception_context( rec, orig_context );
    macrunner_hb_trace_first_chance_exception( rec, orig_context );
    if (rec->ExceptionCode == STATUS_STACK_OVERFLOW)
    {
        if (macrunner_hb_stack_overflow_repeat_guard( rec, orig_context ))
            return STATUS_UNHANDLED_EXCEPTION;
    }
    if (macrunner_hb_fix_tagged_arm64ec_misalignment( rec, orig_context ))
        return STATUS_SUCCESS;
    if (macrunner_hb_fix_syscall_data_boundary_exception( rec, orig_context ))
        return STATUS_SUCCESS;

    if (!(context = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(*context) )))
        return STATUS_NO_MEMORY;
    *context = *orig_context;
    macrunner_hb_current_exception_record = rec;
    dispatch.TargetPc      = 0;
    dispatch.ContextRecord = context;
    dispatch.HistoryTable  = &table;
    dispatch.NonVolatileRegisters = nonvol_regs.Buffer;

    for (;;)
    {
        for (i = 0; i < unwind_seen_count; i++)
        {
            if (unwind_seen[i].pc == context->Pc &&
                unwind_seen[i].fp == context->Fp &&
                unwind_seen[i].sp == context->Sp)
            {
                if (macrunner_hb_trace_arm64_seh_invalid_disposition())
                    ERR( "macrunner-hb-seh-boundary: reason=unwind-no-progress pc=%p "
                         "fp=%p sp=%016I64x rec_code=%08lx stack=%p-%p\n",
                         (void *)(ULONG_PTR)context->Pc, (void *)(ULONG_PTR)context->Fp,
                         context->Sp, rec->ExceptionCode, NtCurrentTeb()->Tib.StackLimit,
                         NtCurrentTeb()->Tib.StackBase );
                macrunner_hb_stop_unwind_at_boundary( &dispatch, context );
                goto unwind_done;
            }
        }
        if (unwind_seen_count < ARRAY_SIZE(unwind_seen))
        {
            unwind_seen[unwind_seen_count].pc = context->Pc;
            unwind_seen[unwind_seen_count].fp = context->Fp;
            unwind_seen[unwind_seen_count].sp = context->Sp;
            unwind_seen_count++;
        }
        status = virtual_unwind( UNW_FLAG_EHANDLER, &dispatch, context );
        if (status != STATUS_SUCCESS) goto done;

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %I64x (%p-%p)\n", dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler)
        {
            TRACE( "calling handler %p (rec=%p, frame=%I64x context=%p, dispatch=%p)\n",
                   dispatch.LanguageHandler, rec, dispatch.EstablisherFrame, orig_context, &dispatch );
            res = call_seh_handler( rec, dispatch.EstablisherFrame, orig_context,
                                    &dispatch, dispatch.LanguageHandler );
            rec->ExceptionFlags &= EXCEPTION_NONCONTINUABLE;
            TRACE( "handler at %p returned %lu\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                status = (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? STATUS_NONCONTINUABLE_EXCEPTION : STATUS_SUCCESS;
                goto done;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  context, &dispatch.HandlerData, &frame, NULL );
                goto unwind_done;
            default:
                if (macrunner_hb_trace_arm64_seh_invalid_disposition())
                    ERR( "macrunner-hb-seh-invalid: reason=language-handler-bad-disposition "
                         "res=%lu handler=%p control_pc=%p establisher=%I64x rec_code=%08lx "
                         "sp=%016I64x stack=%p-%p\n",
                         res, dispatch.LanguageHandler, (void *)dispatch.ControlPc,
                         dispatch.EstablisherFrame, rec->ExceptionCode, context->Sp,
                         NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
                status = STATUS_INVALID_DISPOSITION;
                goto done;
            }
        }
        /* hack: call wine handlers registered in the tib list */
        else while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG64)teb_frame < context->Sp)
        {
            TRACE( "calling TEB handler %p (rec=%p frame=%p context=%p dispatch=%p) sp=%I64x\n",
                   teb_frame->Handler, rec, teb_frame, orig_context, &dispatch, context->Sp );
            res = call_seh_handler( rec, (ULONG_PTR)teb_frame, orig_context,
                                    &dispatch, (PEXCEPTION_ROUTINE)teb_frame->Handler );
            TRACE( "TEB handler at %p returned %lu\n", teb_frame->Handler, res );

            switch (res)
            {
            case ExceptionContinueExecution:
                status = (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? STATUS_NONCONTINUABLE_EXCEPTION : STATUS_SUCCESS;
                goto done;
            case ExceptionContinueSearch:
                break;
            case ExceptionNestedException:
                rec->ExceptionFlags |= EXCEPTION_NESTED_CALL;
                TRACE( "nested exception\n" );
                break;
            case ExceptionCollidedUnwind:
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  context, &dispatch.HandlerData, &frame, NULL );
                teb_frame = teb_frame->Prev;
                goto unwind_done;
            default:
                if (macrunner_hb_trace_arm64_seh_invalid_disposition())
                    ERR( "macrunner-hb-seh-invalid: reason=teb-handler-bad-disposition "
                         "res=%lu handler=%p frame=%p rec_code=%08lx sp=%016I64x "
                         "stack=%p-%p\n",
                         res, teb_frame->Handler, teb_frame, rec->ExceptionCode,
                         context->Sp, NtCurrentTeb()->Tib.StackLimit,
                         NtCurrentTeb()->Tib.StackBase );
                status = STATUS_INVALID_DISPOSITION;
                goto done;
            }
            teb_frame = teb_frame->Prev;
        }

        if (context->Sp == (ULONG64)NtCurrentTeb()->Tib.StackBase) break;
    }
    status = STATUS_UNHANDLED_EXCEPTION;

done:
    macrunner_hb_current_exception_record = old_record;
    RtlFreeHeap( GetProcessHeap(), 0, context );
    return status;
}


/*******************************************************************
 *		KiUserExceptionDispatcher (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( KiUserExceptionDispatcher,
                   ".seh_context\n\t"
                   ".seh_endprologue\n\t"
                   "adrp x16, pWow64PrepareForException\n\t"
                   "ldr x16, [x16, #:lo12:pWow64PrepareForException]\n\t"
                   "cbz x16, 1f\n\t"
                   "add x0, sp, #0x3b0\n\t"     /* rec */
                   "mov x1, sp\n\t"             /* context */
                   "blr x16\n"
                   "1:\tadd x0, sp, #0x3b0\n\t" /* rec */
                   "mov x1, sp\n\t"             /* context */
                   "bl dispatch_exception\n\t"
                   "brk #1" )


/*******************************************************************
 *		KiUserApcDispatcher (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( KiUserApcDispatcher,
                   ".seh_context\n\t"
                   "nop\n\t"
                   ".seh_stackalloc 0x30\n\t"
                   ".seh_endprologue\n\t"
                   "ldp x16, x0, [sp]\n\t"        /* func, arg1 */
                   "ldp x1, x2, [sp, #0x10]\n\t"  /* arg2, arg3 */
                   "add x3, sp, #0x30\n\t"        /* context (FIXME) */
                   "blr x16\n\t"
                   "add x0, sp, #0x30\n\t"        /* context */
                   "ldr w1, [sp, #0x20]\n\t"      /* alertable */
                   "bl NtContinue\n\t"
                   "brk #1" )


/*******************************************************************
 *		KiUserCallbackDispatcher (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( KiUserCallbackDispatcher,
                   ".seh_pushframe\n\t"
                   "nop\n\t"
                   ".seh_stackalloc 0x20\n\t"
                   "nop\n\t"
                   ".seh_save_reg lr, 0x18\n\t"
                   ".seh_endprologue\n\t"
                   ".seh_handler user_callback_handler, @except\n\t"
                   "ldr x0, [sp]\n\t"             /* args */
                   "ldp w1, w2, [sp, #0x08]\n\t"  /* len, id */
                   "ldr x3, [x18, 0x60]\n\t"      /* peb */
                   "ldr x3, [x3, 0x58]\n\t"       /* peb->KernelCallbackTable */
                   "ldr x15, [x3, x2, lsl #3]\n\t"
                   "blr x15\n\t"
                   ".globl KiUserCallbackDispatcherReturn\n"
                   "KiUserCallbackDispatcherReturn:\n\t"
                   "mov x2, x0\n\t"               /* status */
                   "mov x1, #0\n\t"               /* ret_len */
                   "mov x0, x1\n\t"               /* ret_ptr */
                   "bl NtCallbackReturn\n\t"
                   "bl RtlRaiseStatus\n\t"
                   "brk #1" )


/**********************************************************************
 *           consolidate_callback
 *
 * Wrapper function to call a consolidate callback from a fake frame.
 * If the callback executes RtlUnwindEx (like for example done in C++ handlers),
 * we have to skip all frames which were already processed. To do that we
 * trick the unwinding functions into thinking the call came from somewhere
 * else.
 */
void WINAPI DECLSPEC_NORETURN consolidate_callback( CONTEXT *context,
                                                    void *(CALLBACK *callback)(EXCEPTION_RECORD *),
                                                    EXCEPTION_RECORD *rec );
__ASM_GLOBAL_FUNC( consolidate_callback,
                   "stp x29, x30, [sp, #-16]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   "sub sp, sp, #0x390\n\t"
                   ".seh_stackalloc 0x390\n\t"
                   ".seh_endprologue\n\t"
                   "mov x4, sp\n\t"
                   /* copy the context onto the stack */
                   "mov x5, #0x390/16\n"
                   "1:\tldp x6, x7, [x0], #16\n\t"
                   "stp x6, x7, [x4], #16\n\t"
                   "subs x5, x5, #1\n\t"
                   "b.ne 1b\n\t"
                   "mov x0, x2\n\t"
                   "b invoke_callback" )
__ASM_GLOBAL_FUNC( invoke_callback,
                   ".seh_context\n\t"
                   ".seh_endprologue\n\t"
                   "blr x1\n\t"
                   "str x0, [sp, #0x108]\n\t" /* context->Pc */
                   "mov x0, sp\n\t"
                   "mov w1, #0\n\t"
                   "b NtContinue" )


/*******************************************************************
 *              RtlRestoreContext (NTDLL.@)
 */
void CDECL RtlRestoreContext( CONTEXT *context, EXCEPTION_RECORD *rec )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;

    if (rec && rec->ExceptionCode == STATUS_LONGJUMP && rec->NumberParameters >= 1)
    {
        struct _JUMP_BUFFER *jmp = (struct _JUMP_BUFFER *)rec->ExceptionInformation[0];
        int i;

        context->X19  = jmp->X19;
        context->X20  = jmp->X20;
        context->X21  = jmp->X21;
        context->X22  = jmp->X22;
        context->X23  = jmp->X23;
        context->X24  = jmp->X24;
        context->X25  = jmp->X25;
        context->X26  = jmp->X26;
        context->X27  = jmp->X27;
        context->X28  = jmp->X28;
        context->Fp   = jmp->Fp;
        context->Pc   = jmp->Lr;
        context->Sp   = jmp->Sp;
        context->Fpcr = jmp->Fpcr;
        context->Fpsr = jmp->Fpsr;

        for (i = 0; i < 8; i++)
            context->V[8+i].D[0] = jmp->D[i];
    }
    else if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1)
    {
        PVOID (CALLBACK *consolidate)(EXCEPTION_RECORD *) = (void *)rec->ExceptionInformation[0];
        TRACE( "calling consolidate callback %p (rec=%p)\n", consolidate, rec );
        consolidate_callback( context, consolidate, rec );
    }

    /* hack: remove no longer accessible TEB frames */
    while (is_valid_frame( (ULONG_PTR)teb_frame ) && (ULONG64)teb_frame < context->Sp)
    {
        TRACE( "removing TEB frame: %p\n", teb_frame );
        teb_frame = __wine_pop_frame( teb_frame );
    }

    TRACE( "returning to %I64x stack %I64x\n", context->Pc, context->Sp );
    NtContinue( context, FALSE );
}

/*******************************************************************
 *		RtlUnwindEx (NTDLL.@)
 */
void WINAPI RtlUnwindEx( PVOID end_frame, PVOID target_ip, EXCEPTION_RECORD *rec,
                         PVOID retval, CONTEXT *context, UNWIND_HISTORY_TABLE *table )
{
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 nonvol_regs;
    EXCEPTION_RECORD record;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT new_context;
    NTSTATUS status;
    static unsigned int scaffold_skip_count;
    ULONG_PTR frame;
    DWORD i, res;

    RtlCaptureContext( context );
    new_context = *context;

    /* build an exception record, if we do not have one */
    if (!rec)
    {
        record.ExceptionCode    = STATUS_UNWIND;
        record.ExceptionFlags   = 0;
        record.ExceptionRecord  = NULL;
        record.ExceptionAddress = (void *)context->Pc;
        record.NumberParameters = 0;
        rec = &record;
    }

    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (end_frame ? 0 : EXCEPTION_EXIT_UNWIND);

    TRACE( "code=%lx flags=%lx end_frame=%p target_ip=%p\n",
           rec->ExceptionCode, rec->ExceptionFlags, end_frame, target_ip );
    for (i = 0; i < min( EXCEPTION_MAXIMUM_PARAMETERS, rec->NumberParameters ); i++)
        TRACE( " info[%ld]=%016I64x\n", i, rec->ExceptionInformation[i] );
    TRACE_CONTEXT( context );

    dispatch.TargetPc         = (ULONG64)target_ip;
    dispatch.ContextRecord    = context;
    dispatch.HistoryTable     = table;
    dispatch.NonVolatileRegisters = nonvol_regs.Buffer;

    for (;;)
    {
        status = virtual_unwind( UNW_FLAG_UHANDLER, &dispatch, &new_context );
        if (status != STATUS_SUCCESS) raise_status( status, rec );

    unwind_done:
        if (!dispatch.EstablisherFrame) break;

        if (!is_valid_frame( dispatch.EstablisherFrame ))
        {
            ERR( "invalid frame %I64x (%p-%p)\n", dispatch.EstablisherFrame,
                 NtCurrentTeb()->Tib.StackLimit, NtCurrentTeb()->Tib.StackBase );
            rec->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }

        if (dispatch.LanguageHandler)
        {
            if (end_frame && (dispatch.EstablisherFrame > (ULONG64)end_frame))
            {
                if (macrunner_hb_is_arm64ec_unwind_scaffold_overshoot( dispatch.EstablisherFrame, end_frame ))
                {
                    if (scaffold_skip_count++ < 64)
                        MESSAGE( "macrunner-hb-rtlunwind-scaffold-skip: establisher=%p "
                                 "end_frame=%p control_pc=%p handler=%p target=%p\n",
                                 (void *)(ULONG_PTR)dispatch.EstablisherFrame, end_frame,
                                 (void *)(ULONG_PTR)dispatch.ControlPc,
                                 dispatch.LanguageHandler, target_ip );
                    new_context.Pc = (ULONG64)target_ip;
                    new_context.Sp = (ULONG64)end_frame;
                    *context = new_context;
                    rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;
                    break;
                }
                ERR( "invalid end frame %I64x/%p\n", dispatch.EstablisherFrame, end_frame );
                raise_status( STATUS_INVALID_UNWIND_TARGET, rec );
            }
            if (dispatch.EstablisherFrame == (ULONG64)end_frame) rec->ExceptionFlags |= EXCEPTION_TARGET_UNWIND;

            TRACE( "calling handler %p (rec=%p, frame=%I64x context=%p, dispatch=%p)\n",
                   dispatch.LanguageHandler, rec, dispatch.EstablisherFrame,
                   dispatch.ContextRecord, &dispatch );
            res = call_unwind_handler( rec, dispatch.EstablisherFrame, dispatch.ContextRecord,
                                       &dispatch, dispatch.LanguageHandler );
            TRACE( "handler %p returned %lx\n", dispatch.LanguageHandler, res );

            switch (res)
            {
            case ExceptionContinueSearch:
                rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                break;
            case ExceptionCollidedUnwind:
                new_context = *context;
                RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                  dispatch.ControlPc, dispatch.FunctionEntry,
                                  &new_context, &dispatch.HandlerData, &frame,
                                  NULL );
                rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                goto unwind_done;
            default:
                raise_status( STATUS_INVALID_DISPOSITION, rec );
                break;
            }
        }
        else  /* hack: call builtin handlers registered in the tib list */
        {
            while (is_valid_frame( (ULONG_PTR)teb_frame ) &&
                   (ULONG64)teb_frame < new_context.Sp &&
                   (ULONG64)teb_frame < (ULONG64)end_frame)
            {
                TRACE( "calling TEB handler %p (rec=%p, frame=%p context=%p, dispatch=%p)\n",
                       teb_frame->Handler, rec, teb_frame, dispatch.ContextRecord, &dispatch );
                res = call_unwind_handler( rec, (ULONG_PTR)teb_frame, dispatch.ContextRecord, &dispatch,
                                           (PEXCEPTION_ROUTINE)teb_frame->Handler );
                TRACE( "handler at %p returned %lu\n", teb_frame->Handler, res );
                teb_frame = __wine_pop_frame( teb_frame );

                switch (res)
                {
                case ExceptionContinueSearch:
                    rec->ExceptionFlags &= ~EXCEPTION_COLLIDED_UNWIND;
                    break;
                case ExceptionCollidedUnwind:
                    new_context = *context;
                    RtlVirtualUnwind( UNW_FLAG_NHANDLER, dispatch.ImageBase,
                                      dispatch.ControlPc, dispatch.FunctionEntry,
                                      &new_context, &dispatch.HandlerData,
                                      &frame, NULL );
                    rec->ExceptionFlags |= EXCEPTION_COLLIDED_UNWIND;
                    goto unwind_done;
                default:
                    raise_status( STATUS_INVALID_DISPOSITION, rec );
                    break;
                }
            }
            if ((ULONG64)teb_frame == (ULONG64)end_frame && (ULONG64)end_frame < new_context.Sp) break;
        }

        if (dispatch.EstablisherFrame == (ULONG64)end_frame) break;
        *context = new_context;
    }

    if (rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE)
        context->Pc = (ULONG64)target_ip;
    else if (rec->ExceptionInformation[10] == -1)
        rec->ExceptionInformation[10] = (ULONG_PTR)&nonvol_regs;

    context->X0 = (ULONG64)retval;
    RtlRestoreContext(context, rec);
}


/*************************************************************************
 *		RtlGetNativeSystemInformation (NTDLL.@)
 */
NTSTATUS WINAPI RtlGetNativeSystemInformation( SYSTEM_INFORMATION_CLASS class,
                                               void *info, ULONG size, ULONG *ret_size )
{
    return NtQuerySystemInformation( class, info, size, ret_size );
}


static ULONGLONG cpu_features_bitmap[2];
static RTL_RUN_ONCE init_once = RTL_RUN_ONCE_INIT;

static DWORD WINAPI init_cpu_features( RTL_RUN_ONCE *once, void *param, void **context )
{
    return !NtQuerySystemInformation( SystemProcessorFeaturesBitMapInformation,
                                      cpu_features_bitmap, sizeof(cpu_features_bitmap), NULL );
}


/***********************************************************************
 *           RtlIsProcessorFeaturePresent [NTDLL.@]
 */
BOOLEAN WINAPI RtlIsProcessorFeaturePresent( UINT feature )
{
    static const ULONGLONG arm64_features =
        (1ull << PF_COMPARE_EXCHANGE_DOUBLE) |
        (1ull << PF_NX_ENABLED) |
        (1ull << PF_ARM_VFP_32_REGISTERS_AVAILABLE) |
        (1ull << PF_ARM_NEON_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_SECOND_LEVEL_ADDRESS_TRANSLATION) |
        (1ull << PF_FASTFAIL_AVAILABLE) |
        (1ull << PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE) |
        (1ull << PF_ARM_64BIT_LOADSTORE_ATOMIC) |
        (1ull << PF_ARM_EXTERNAL_CACHE_AVAILABLE) |
        (1ull << PF_ARM_FMAC_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V8_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V83_JSCVT_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE2_1_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_AES_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_PMULL128_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_BITPERM_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_BF16_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_EBF16_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_B16B16_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_SHA3_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_SM4_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_I8MM_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_F32MM_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_SVE_F64MM_INSTRUCTIONS_AVAILABLE) |
        (1ull << PF_ARM_LSE2_AVAILABLE);

    if (feature < PROCESSOR_FEATURE_MAX)
        return (arm64_features & (1ull << feature)) && user_shared_data->ProcessorFeatures[feature];

    feature -= PROCESSOR_FEATURE_MAX;
    if (feature >= 8 * sizeof(cpu_features_bitmap)) return FALSE;

    RtlRunOnceExecuteOnce( &init_once, init_cpu_features, NULL, NULL );
    return !!(cpu_features_bitmap[feature / 64] & (1ull << (feature % 64)));
}


/*************************************************************************
 *		RtlWalkFrameChain (NTDLL.@)
 */
ULONG WINAPI RtlWalkFrameChain( void **buffer, ULONG count, ULONG flags )
{
    UNWIND_HISTORY_TABLE table;
    RUNTIME_FUNCTION *func;
    PEXCEPTION_ROUTINE handler;
    ULONG_PTR pc, frame, base;
    CONTEXT context;
    void *data;
    ULONG i, skip = flags >> 8, num_entries = 0;

    RtlCaptureContext( &context );

    for (i = 0; i < count; i++)
    {
        pc = context.Pc;
        if (context.ContextFlags & CONTEXT_UNWOUND_TO_CALL) pc -= 4;
        pc = macrunner_hb_normalize_arm64ec_host_pc( pc );
        func = RtlLookupFunctionEntry( pc, &base, &table );
        if (RtlVirtualUnwind2( UNW_FLAG_NHANDLER, base, pc, func, &context, NULL,
                               &data, &frame, NULL, NULL, NULL, &handler, 0 ))
            break;
        if (!context.Pc) break;
        if (!frame || !is_valid_frame( frame )) break;
        if (context.Sp == (ULONG_PTR)NtCurrentTeb()->Tib.StackBase) break;
        if (i >= skip) buffer[num_entries++] = (void *)context.Pc;
    }
    return num_entries;
}


/***********************************************************************
 *		__C_ExecuteExceptionFilter
 */
__ASM_GLOBAL_FUNC( __C_ExecuteExceptionFilter,
                   "stp x29, x30, [sp, #-96]!\n\t"
                   ".seh_save_fplr_x 96\n\t"
                   "stp x19, x20, [sp, #16]\n\t"
                   ".seh_save_regp x19, 16\n\t"
                   "stp x21, x22, [sp, #32]\n\t"
                   ".seh_save_regp x21, 32\n\t"
                   "stp x23, x24, [sp, #48]\n\t"
                   ".seh_save_regp x23, 48\n\t"
                   "stp x25, x26, [sp, #64]\n\t"
                   ".seh_save_regp x25, 64\n\t"
                   "stp x27, x28, [sp, #80]\n\t"
                   ".seh_save_regp x27, 80\n\t"
                   ".seh_endprologue\n\t"
                   "ldp x19, x20, [x3, #0]\n\t" /* nonvolatile regs */
                   "ldp x21, x22, [x3, #16]\n\t"
                   "ldp x23, x24, [x3, #32]\n\t"
                   "ldp x25, x26, [x3, #48]\n\t"
                   "ldp x27, x28, [x3, #64]\n\t"
                   "ldr x1, [x3, #80]\n\t"      /* x29 = frame */
                   "blr x2\n\t"                 /* filter */
                   "ldp x19, x20, [sp, #16]\n\t"
                   "ldp x21, x22, [sp, #32]\n\t"
                   "ldp x23, x24, [sp, #48]\n\t"
                   "ldp x25, x26, [sp, #64]\n\t"
                   "ldp x27, x28, [sp, #80]\n\t"
                   "ldp x29, x30, [sp], #96\n\t"
                   "ret")


/***********************************************************************
 *		RtlRaiseException (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( RtlRaiseException,
                   "sub x17, sp, #0x4000\n\t"
                   ".seh_nop\n\t"
                   "ldr x16, [x18, #0x10]\n\t"       /* TEB.StackLimit */
                   ".seh_nop\n\t"
                   "cmp x17, x16\n\t"
                   ".seh_nop\n\t"
                   "b.hs 1f\n\t"
                   ".seh_nop\n\t"
                   "ldr x15, [x18, #0x08]\n\t"       /* TEB.StackBase */
                   ".seh_nop\n\t"
                   "add x17, x16, #0x100000\n\t"
                   ".seh_nop\n\t"
                   "cmp x17, x15\n\t"
                   ".seh_nop\n\t"
                   "b.lo 2f\n\t"
                   ".seh_nop\n\t"
                   "mov x17, x15\n"
                   ".seh_nop\n\t"
                   "2:\tmov sp, x17\n"
                   ".seh_nop\n\t"
                   "1:\n\t"
                   "sub sp, sp, #0x3b0\n\t" /* 0x390 (context) + 0x20 */
                   ".seh_stackalloc 0x3b0\n\t"
                   "stp x29, x30, [sp]\n\t"
                   ".seh_save_fplr 0\n\t"
                   ".seh_endprologue\n\t"
                   "mov x29, sp\n\t"
                   "str x0,  [sp, #0x10]\n\t"
                   "add x0,  sp, #0x20\n\t"
                   "bl RtlCaptureContext\n\t"
                   "add x1,  sp, #0x20\n\t"      /* context pointer */
                   "add x2,  sp, #0x3b0\n\t"     /* orig stack pointer */
                   "str x2,  [x1, #0x100]\n\t"   /* context->Sp */
                   "ldr x0,  [sp, #0x10]\n\t"    /* original first parameter */
                   "str x0,  [x1, #0x08]\n\t"    /* context->X0 */
                   "ldp x4, x5, [sp]\n\t"        /* frame pointer, return address */
                   "stp x4, x5, [x1, #0xf0]\n\t" /* context->Fp, Lr */
                   "str  x5, [x1, #0x108]\n\t"   /* context->Pc */
                   "str  x5, [x0, #0x10]\n\t"    /* rec->ExceptionAddress */
                   "ldr w2, [x1]\n\t"            /* context->ContextFlags */
                   "orr w2, w2, #0x20000000\n\t" /* CONTEXT_UNWOUND_TO_CALL */
                   "str w2, [x1]\n\t"
                   "ldr x3, [x18, #0x60]\n\t"    /* peb */
                   "ldrb w2, [x3, #2]\n\t"       /* peb->BeingDebugged */
                   "cbnz w2, 1f\n\t"
                   "bl dispatch_exception\n"
                   "1:\tmov  x2, #1\n\t"
                   "bl NtRaiseException\n\t"
                   "bl RtlRaiseStatus\n\t"
                   "brk #1" )


/***********************************************************************
 *           _setjmpex (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( NTDLL__setjmpex,
                   ".seh_endprologue\n\t"
                   "str x1,       [x0]\n\t"        /* jmp_buf->Frame */
                   "stp x19, x20, [x0, #0x10]\n\t" /* jmp_buf->X19, X20 */
                   "stp x21, x22, [x0, #0x20]\n\t" /* jmp_buf->X21, X22 */
                   "stp x23, x24, [x0, #0x30]\n\t" /* jmp_buf->X23, X24 */
                   "stp x25, x26, [x0, #0x40]\n\t" /* jmp_buf->X25, X26 */
                   "stp x27, x28, [x0, #0x50]\n\t" /* jmp_buf->X27, X28 */
                   "stp x29, x30, [x0, #0x60]\n\t" /* jmp_buf->Fp,  Lr  */
                   "mov x2,  sp\n\t"
                   "str x2,       [x0, #0x70]\n\t" /* jmp_buf->Sp */
                   "mrs x2,  fpcr\n\t"
                   "mrs x3,  fpsr\n\t"
                   "stp w2, w3,   [x0, #0x78]\n\t" /* jmp_buf->Fpcr,Fpsr */
                   "stp d8,  d9,  [x0, #0x80]\n\t" /* jmp_buf->D[0-1] */
                   "stp d10, d11, [x0, #0x90]\n\t" /* jmp_buf->D[2-3] */
                   "stp d12, d13, [x0, #0xa0]\n\t" /* jmp_buf->D[4-5] */
                   "stp d14, d15, [x0, #0xb0]\n\t" /* jmp_buf->D[6-7] */
                   "mov x0, #0\n\t"
                   "ret" )


/*******************************************************************
 *		longjmp (NTDLL.@)
 */
void __cdecl NTDLL_longjmp( _JUMP_BUFFER *buf, int retval )
{
    EXCEPTION_RECORD rec;

    if (!retval) retval = 1;

    rec.ExceptionCode = STATUS_LONGJUMP;
    rec.ExceptionFlags = 0;
    rec.ExceptionRecord = NULL;
    rec.ExceptionAddress = NULL;
    rec.NumberParameters = 1;
    rec.ExceptionInformation[0] = (DWORD_PTR)buf;
    RtlUnwind( (void *)buf->Frame, (void *)buf->Lr, &rec, IntToPtr(retval) );
}


/***********************************************************************
 *           RtlUserThreadStart (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( RtlUserThreadStart,
                   "stp x29, x30, [sp, #-16]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   ".seh_endprologue\n\t"
                   "adrp x8, pBaseThreadInitThunk\n\t"
                   "ldr x8, [x8, #:lo12:pBaseThreadInitThunk]\n\t"
                   "mov x2, x1\n\t"
                   "mov x1, x0\n\t"
                   "mov x0, #0\n\t"
                   "blr x8\n\t"
                   "brk #1\n\t"
                   ".seh_handler call_unhandled_exception_handler, @except" )

/******************************************************************
 *		LdrInitializeThunk (NTDLL.@)
 */
void WINAPI LdrInitializeThunk( CONTEXT *context, ULONG_PTR unk2, ULONG_PTR unk3, ULONG_PTR unk4 )
{
    static unsigned int macrunner_trace_count;
    unsigned int trace_index = macrunner_trace_count++;

    if (trace_index < 64)
        MESSAGE( "macrunner-hb-ldr-init: phase=before index=%u ctx=%p pc=%p lr=%p sp=%p "
                 "x0=%p x1=%p x2=%p x3=%p unk=%p/%p/%p\n",
                 trace_index, context, (void *)context->Pc, (void *)context->Lr,
                 (void *)context->Sp, (void *)context->X0, (void *)context->X1,
                 (void *)context->X2, (void *)context->X3,
                 (void *)unk2, (void *)unk3, (void *)unk4 );
    loader_init( context, (void **)&context->X0 );
    if (trace_index < 64)
        MESSAGE( "macrunner-hb-ldr-init: phase=after-loader index=%u ctx=%p pc=%p lr=%p "
                 "sp=%p entry=%p arg=%p\n",
                 trace_index, context, (void *)context->Pc, (void *)context->Lr,
                 (void *)context->Sp, (void *)context->X0, (void *)context->X1 );
    TRACE_(relay)( "\1Starting thread proc %p (arg=%p)\n", (void *)context->X0, (void *)context->X1 );
    {
        NTSTATUS status = NtContinue( context, TRUE );
        MESSAGE( "macrunner-hb-ldr-init: NtContinue returned status=%08lx index=%u ctx=%p pc=%p sp=%p\n",
                 status, trace_index, context, (void *)context->Pc, (void *)context->Sp );
    }
}


/***********************************************************************
 *           process_breakpoint
 */
__ASM_GLOBAL_FUNC( process_breakpoint,
                   ".seh_endprologue\n\t"
                   ".seh_handler process_breakpoint_handler, @except\n\t"
                   "brk #0xf000\n\t"
                   "ret\n"
                   "process_breakpoint_handler:\n\t"
                   "ldr x4, [x2, #0x108]\n\t" /* context->Pc */
                   "add x4, x4, #4\n\t"
                   "str x4, [x2, #0x108]\n\t"
                   "mov w0, #0\n\t"           /* ExceptionContinueExecution */
                   "ret" )

/***********************************************************************
 *		DbgUiRemoteBreakin   (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( DbgUiRemoteBreakin,
                   "stp x29, x30, [sp, #-16]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   ".seh_endprologue\n\t"
                   ".seh_handler DbgUiRemoteBreakin_handler, @except\n\t"
                   "ldr x0, [x18, #0x60]\n\t"       /* NtCurrentTeb()->Peb */
                   "ldrb w0, [x0, 0x02]\n\t"        /* peb->BeingDebugged */
                   "cbz w0, 1f\n\t"
                   "bl DbgBreakPoint\n"
                   "1:\tmov w0, #0\n\t"
                   "bl RtlExitUserThread\n"
                   "DbgUiRemoteBreakin_handler:\n\t"
                   "mov sp, x1\n\t"                 /* frame */
                   "b 1b" )

/**********************************************************************
 *              DbgBreakPoint   (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( DbgBreakPoint, "brk #0xf000; ret"
                    "\n\tnop; nop; nop; nop; nop; nop; nop; nop"
                    "\n\tnop; nop; nop; nop; nop; nop" );

/**********************************************************************
 *              DbgUserBreakPoint   (NTDLL.@)
 */
__ASM_GLOBAL_FUNC( DbgUserBreakPoint, "brk #0xf000; ret"
                    "\n\tnop; nop; nop; nop; nop; nop; nop; nop"
                    "\n\tnop; nop; nop; nop; nop; nop" );

#endif  /* __aarch64__ */
