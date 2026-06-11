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

static inline void macrunner_hb_stop_unwind_at_boundary( DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    dispatch->ImageBase = 0;
    dispatch->FunctionEntry = NULL;
    dispatch->HandlerData = NULL;
    dispatch->EstablisherFrame = 0;
    dispatch->LanguageHandler = NULL;
    context->ContextFlags |= CONTEXT_UNWOUND_TO_CALL;
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
    if (!walk.Lr || walk.Lr == pc) return FALSE;

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

    if (!fp || (fp & 0xf)) bail = "fp-null-or-unaligned";
    else if (fp < stack_lo || fp + 0x10 > stack_hi) bail = "fp-out-of-stack";
    else
    {
        new_fp = ((const ULONG_PTR *)fp)[0];   /* saved caller x29 */
        new_pc = ((const ULONG_PTR *)fp)[1];   /* saved lr / return address */
        if (!new_pc || new_pc == pc || (new_pc & 3) || new_pc < 0x10000) bail = "bad-new-pc";
        else if (new_fp && (new_fp <= fp || (new_fp & 0xf) || new_fp + 0x10 > stack_hi)) bail = "bad-new-fp";
    }
    if (bail)
    {
        static unsigned int fpb;
        if (fpb++ < 16)
            ERR( "macrunner-hb-seh-ec-fpchain-bail: reason=%s pc=%p fp=%p sp=%p lr=%p stack=%p-%p new_fp=%p new_pc=%p\n",
                 bail, (void *)pc, (void *)fp, (void *)sp, (void *)(ULONG_PTR)context->Lr,
                 (void *)stack_lo, (void *)stack_hi, (void *)new_fp, (void *)new_pc );
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
            ERR( "macrunner-hb-seh-ec-fpchain: pc=%p fp=%p -> new pc=%p new fp=%p sp=%016I64x\n",
                 (void *)pc, (void *)fp, (void *)new_pc, (void *)new_fp, context->Sp );
    }
    return TRUE;
}


/**********************************************************************
 *           virtual_unwind
 */
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
    for (p = pc & ~0xfffULL; p >= img_floor; p -= 0x1000)
        if (*(const USHORT *)p == 0x5a4d) { base = p; break; }   /* 'MZ' header */
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

static NTSTATUS virtual_unwind( ULONG type, DISPATCHER_CONTEXT *dispatch, CONTEXT *context )
{
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 *nonvol_regs;
    DWORD64 pc = context->Pc;
    DWORD64 raw_pc = pc;
    DWORD64 lookup_pc;
    int i;

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
                     "resume=stop-unwind frame=%p frame_pc=%p frame_lr=%p frame_sp=%p "
                     "frame_prev=%p frame_cfa=%p frame_flags=%08lx\n",
                     tid, (void *)pc, (void *)context->Lr, context->Sp,
                     rec ? rec->ExceptionCode : 0, rec ? rec->ExceptionFlags : 0,
                     ldr_status, module ? module->DllBase : NULL, frame,
                     frame ? (void *)(ULONG_PTR)frame->pc : NULL,
                     frame ? (void *)(ULONG_PTR)frame->lr : NULL,
                     frame ? (void *)(ULONG_PTR)frame->sp : NULL,
                      frame ? frame->prev_frame : NULL,
                      frame ? frame->syscall_cfa : NULL,
                      frame ? frame->restore_flags : 0 );
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
        NTSTATUS ldr_status = LdrFindEntryForAddress( (void *)(ULONG_PTR)pc, &module );
        DWORD tid = HandleToULong( NtCurrentTeb()->ClientId.UniqueThread );

        if (report_count++ < 64)
        {
            MESSAGE( "macrunner-hb-seh-host-boundary: pc=%p lr=%p sp=%016I64x\n",
                     (void *)pc, (void *)context->Lr, context->Sp );
            MESSAGE( "macrunner-hb-seh-host-boundary-detail: side=arm64 tid=%04lx pc=%p lr=%p "
                     "sp=%016I64x exception=%08lx flags=%08lx ldr_status=%08lx module=%p "
                     "resume=stop-unwind\n",
                     tid, (void *)pc, (void *)context->Lr, context->Sp,
                     rec ? rec->ExceptionCode : 0, rec ? rec->ExceptionFlags : 0,
                     ldr_status, module ? module->DllBase : NULL );
        }
        macrunner_hb_stop_unwind_at_boundary( dispatch, context );
        return STATUS_SUCCESS;
    }

    dispatch->FunctionEntry = RtlLookupFunctionEntry( pc, &dispatch->ImageBase, dispatch->HistoryTable );
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
    if (RtlVirtualUnwind2( type, dispatch->ImageBase, pc, dispatch->FunctionEntry, context,
                           NULL, &dispatch->HandlerData, &dispatch->EstablisherFrame,
                           NULL, NULL, NULL, &dispatch->LanguageHandler, 0 ))
    {
        if (!dispatch->FunctionEntry &&
            (macrunner_ec_virtual_unwind_frame( dispatch, context, pc ) ||
             macrunner_ec_fp_chain_unwind( dispatch, context, pc )))
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
    EXCEPTION_REGISTRATION_RECORD *teb_frame = NtCurrentTeb()->Tib.ExceptionList;
    const EXCEPTION_RECORD *old_record = macrunner_hb_current_exception_record;
    DISPATCHER_CONTEXT_NONVOLREG_ARM64 nonvol_regs;
    UNWIND_HISTORY_TABLE table;
    DISPATCHER_CONTEXT dispatch;
    CONTEXT *context;
    NTSTATUS status;
    ULONG_PTR frame;
    DWORD res;

    macrunner_hb_trace_tagged_exception_context( rec, orig_context );
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
