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

#if 0
#pragma makedep unix
#endif

#ifdef __aarch64__

#include "config.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __APPLE__
# include <dlfcn.h>
# include <mach/arm/thread_status.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else
# ifdef HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif
#ifdef HAVE_SYS_SIGNAL_H
# include <sys/signal.h>
#endif
#ifdef HAVE_SYS_UCONTEXT_H
# include <sys/ucontext.h>
#endif
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/mach_vm.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "wine/asm.h"
#include "unix_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(seh);

#define NTDLL_DWARF_H_NO_UNWINDER
#include "dwarf.h"

extern NTSTATUS macrunner_hb_get_x64_thread_context( HANDLE handle, AMD64_CONTEXT *context );
extern NTSTATUS macrunner_hb_set_x64_thread_context( HANDLE handle, const AMD64_CONTEXT *context );
extern void macrunner_hb_trace_nullcall_site( const char *source, uint64_t host_pc, uint64_t fault_addr );
extern void macrunner_hb_trace_hk_memcpy_fault( const char *source, uint64_t host_pc, uint64_t fault_addr );
extern int hb_jit_runtime_handle_signal_fault( ULONG_PTR pc, ULONG_PTR fault_addr, int signal );

/***********************************************************************
 * signal context platform-specific definitions
 */
#ifdef linux

/* All Registers access - only for local access */
# define REG_sig(reg_name, context) ((context)->uc_mcontext.reg_name)
# define REGn_sig(reg_num, context) ((context)->uc_mcontext.regs[reg_num])

/* Special Registers access  */
# define SP_sig(context)            REG_sig(sp, context)    /* Stack pointer */
# define PC_sig(context)            REG_sig(pc, context)    /* Program counter */
# define PSTATE_sig(context)        REG_sig(pstate, context) /* Current State Register */
# define FP_sig(context)            REGn_sig(29, context)    /* Frame pointer */
# define LR_sig(context)            REGn_sig(30, context)    /* Link Register */

static struct _aarch64_ctx *get_extended_sigcontext( const ucontext_t *sigcontext, unsigned int magic )
{
    struct _aarch64_ctx *ctx = (struct _aarch64_ctx *)sigcontext->uc_mcontext.__reserved;
    while ((char *)ctx < (char *)(&sigcontext->uc_mcontext + 1) && ctx->magic && ctx->size)
    {
        if (ctx->magic == magic) return ctx;
        ctx = (struct _aarch64_ctx *)((char *)ctx + ctx->size);
    }
    return NULL;
}

static struct fpsimd_context *get_fpsimd_context( const ucontext_t *sigcontext )
{
    return (struct fpsimd_context *)get_extended_sigcontext( sigcontext, FPSIMD_MAGIC );
}

static DWORD64 get_fault_esr( ucontext_t *sigcontext )
{
    struct esr_context *esr = (struct esr_context *)get_extended_sigcontext( sigcontext, ESR_MAGIC );
    if (esr) return esr->esr;
    return 0;
}

#elif defined(__APPLE__)

/* All Registers access - only for local access */
# define REG_sig(reg_name, context) ((context)->uc_mcontext->__ss.__ ## reg_name)
# define REGn_sig(reg_num, context) ((context)->uc_mcontext->__ss.__x[reg_num])

/* Special Registers access  */
# define SP_sig(context)            REG_sig(sp, context)    /* Stack pointer */
# define PC_sig(context)            REG_sig(pc, context)    /* Program counter */
# define PSTATE_sig(context)        REG_sig(cpsr, context)  /* Current State Register */
# define FP_sig(context)            REG_sig(fp, context)    /* Frame pointer */
# define LR_sig(context)            REG_sig(lr, context)    /* Link Register */

static DWORD64 get_fault_esr( ucontext_t *sigcontext )
{
    return sigcontext->uc_mcontext->__es.__esr;
}

#endif /* linux */

#define MACRUNNER_ARM64_CPSR_TRAP 0x00200000u

/* stack layout when calling KiUserExceptionDispatcher */
struct exc_stack_layout
{
    CONTEXT              context;        /* 000 */
    CONTEXT_EX           context_ex;     /* 390 */
    EXCEPTION_RECORD     rec;            /* 3b0 */
    ULONG64              align;          /* 448 */
    ULONG64              sp;             /* 450 */
    ULONG64              pc;             /* 458 */
    ULONG64              redzone[2];     /* 460 */
};
C_ASSERT( offsetof(struct exc_stack_layout, rec) == 0x3b0 );
C_ASSERT( sizeof(struct exc_stack_layout) == 0x470 );

/* stack layout when calling KiUserApcDispatcher */
struct apc_stack_layout
{
    void                *func;           /* 000 APC to call*/
    ULONG64              args[3];        /* 008 function arguments */
    ULONG64              alertable;      /* 020 */
    ULONG64              align;          /* 028 */
    CONTEXT              context;        /* 030 */
    ULONG64              redzone[2];     /* 3c0 */
};
C_ASSERT( offsetof(struct apc_stack_layout, context) == 0x30 );
C_ASSERT( sizeof(struct apc_stack_layout) == 0x3d0 );

/* stack layout when calling KiUserCallbackDispatcher */
struct callback_stack_layout
{
    void                *args;           /* 000 arguments */
    ULONG                len;            /* 008 arguments len */
    ULONG                id;             /* 00c function id */
    ULONG64              unknown;        /* 010 */
    ULONG64              lr;             /* 018 */
    ULONG64              sp;             /* 020 sp+pc (machine frame) */
    ULONG64              pc;             /* 028 */
    BYTE                 args_data[0];   /* 030 copied argument data*/
};
C_ASSERT( offsetof(struct callback_stack_layout, sp) == 0x20 );
C_ASSERT( sizeof(struct callback_stack_layout) == 0x30 );

struct syscall_frame
{
    ULONG64               x[29];          /* 000 */
    ULONG64               fp;             /* 0e8 */
    ULONG64               lr;             /* 0f0 */
    ULONG64               sp;             /* 0f8 */
    ULONG64               pc;             /* 100 */
    ULONG                 cpsr;           /* 108 */
    ULONG                 restore_flags;  /* 10c */
    struct syscall_frame *prev_frame;     /* 110 */
    void                 *syscall_cfa;    /* 118 */
    ULONG                 syscall_id;     /* 120 */
    ULONG                 align;          /* 124 */
    ULONG                 fpcr;           /* 128 */
    ULONG                 fpsr;           /* 12c */
    NEON128               v[32];          /* 130 */
};

C_ASSERT( sizeof( struct syscall_frame ) == 0x330 );


/***********************************************************************
 *           context_init_empty_xstate
 *
 * Initializes a context's CONTEXT_EX structure to point to an empty xstate buffer
 */
static inline void context_init_empty_xstate( CONTEXT *context, void *xstate_buffer )
{
    CONTEXT_EX *xctx;

    xctx = (CONTEXT_EX *)(context + 1);
    xctx->Legacy.Length = sizeof(CONTEXT);
    xctx->Legacy.Offset = -(LONG)sizeof(CONTEXT);
    xctx->XState.Length = 0;
    xctx->XState.Offset = (BYTE *)xstate_buffer - (BYTE *)xctx;
    xctx->All.Length = sizeof(CONTEXT) + xctx->XState.Offset + xctx->XState.Length;
    xctx->All.Offset = -(LONG)sizeof(CONTEXT);
}

void set_process_instrumentation_callback( void *callback )
{
    if (callback) FIXME( "Not supported.\n" );
}


/***********************************************************************
 *           syscall_frame_fixup_for_fastpath
 *
 * Fixes up the given syscall frame such that the syscall dispatcher
 * can return via the fast path if CONTEXT_INTEGER is set in
 * restore_flags.
 *
 * Clobbers the frame's X16 and X17 register values.
 */
static void syscall_frame_fixup_for_fastpath( struct syscall_frame *frame )
{
    frame->x[16] = frame->pc;
    frame->x[17] = frame->sp;
}

/***********************************************************************
 *           save_fpu
 *
 * Set the FPU context from a sigcontext.
 */
static void save_fpu( CONTEXT *context, const ucontext_t *sigcontext )
{
#ifdef linux
    struct fpsimd_context *fp = get_fpsimd_context( sigcontext );

    if (!fp) return;
    context->ContextFlags |= CONTEXT_FLOATING_POINT;
    context->Fpcr = fp->fpcr;
    context->Fpsr = fp->fpsr;
    memcpy( context->V, fp->vregs, sizeof(context->V) );
#elif defined(__APPLE__)
    context->ContextFlags |= CONTEXT_FLOATING_POINT;
    context->Fpcr = sigcontext->uc_mcontext->__ns.__fpcr;
    context->Fpsr = sigcontext->uc_mcontext->__ns.__fpsr;
    memcpy( context->V, sigcontext->uc_mcontext->__ns.__v, sizeof(context->V) );
#endif
}


/***********************************************************************
 *           restore_fpu
 *
 * Restore the FPU context to a sigcontext.
 */
static void restore_fpu( const CONTEXT *context, ucontext_t *sigcontext )
{
#ifdef linux
    struct fpsimd_context *fp = get_fpsimd_context( sigcontext );

    if (!fp) return;
    fp->fpcr = context->Fpcr;
    fp->fpsr = context->Fpsr;
    memcpy( fp->vregs, context->V, sizeof(fp->vregs) );
#elif defined(__APPLE__)
    sigcontext->uc_mcontext->__ns.__fpcr = context->Fpcr;
    sigcontext->uc_mcontext->__ns.__fpsr = context->Fpsr;
    memcpy( sigcontext->uc_mcontext->__ns.__v, context->V, sizeof(context->V) );
#endif
}


/***********************************************************************
 *           save_context
 *
 * Set the register values from a sigcontext.
 */
static void save_context( CONTEXT *context, const ucontext_t *sigcontext )
{
    DWORD i;

    context->ContextFlags = CONTEXT_FULL;
    context->Fp   = FP_sig(sigcontext);     /* Frame pointer */
    context->Lr   = LR_sig(sigcontext);     /* Link register */
    context->Sp   = SP_sig(sigcontext);     /* Stack pointer */
    context->Pc   = PC_sig(sigcontext);     /* Program Counter */
    context->Cpsr = PSTATE_sig(sigcontext); /* Current State Register */
    for (i = 0; i <= 28; i++) context->X[i] = REGn_sig( i, sigcontext );
    save_fpu( context, sigcontext );
}


/***********************************************************************
 *           restore_context
 *
 * Build a sigcontext from the register values.
 */
static void restore_context( const CONTEXT *context, ucontext_t *sigcontext )
{
    DWORD i;

    FP_sig(sigcontext)     = context->Fp;   /* Frame pointer */
    LR_sig(sigcontext)     = context->Lr;   /* Link register */
    SP_sig(sigcontext)     = context->Sp;   /* Stack pointer */
    PC_sig(sigcontext)     = context->Pc;   /* Program Counter */
    PSTATE_sig(sigcontext) = context->Cpsr; /* Current State Register */
    for (i = 0; i <= 28; i++) REGn_sig( i, sigcontext ) = context->X[i];
    restore_fpu( context, sigcontext );
}


/***********************************************************************
 *           signal_set_full_context
 */
NTSTATUS signal_set_full_context( CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (!status && (context->ContextFlags & CONTEXT_INTEGER) == CONTEXT_INTEGER)
        frame->restore_flags |= CONTEXT_INTEGER;

    if (is_arm64ec() && !is_ec_code( frame->pc ) && pKiUserEmulationDispatcher )
    {
        CONTEXT *user_context = (CONTEXT *)((frame->sp - sizeof(CONTEXT)) & ~15);

        user_context->ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), user_context );
        frame->sp = (ULONG_PTR)user_context;
        frame->pc = (ULONG_PTR)pKiUserEmulationDispatcher;
    }
    return status;
}


/***********************************************************************
 *              get_native_context
 */
void *get_native_context( CONTEXT *context )
{
    return context;
}


/***********************************************************************
 *              get_wow_context
 */
void *get_wow_context( CONTEXT *context )
{
    return get_cpu_area( main_image_info.Machine );
}


/***********************************************************************
 *              NtSetContextThread  (NTDLL.@)
 *              ZwSetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtSetContextThread( HANDLE handle, const CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    NTSTATUS ret = STATUS_SUCCESS;
    BOOL self = (handle == GetCurrentThread());
    const AMD64_CONTEXT *amd64_context = (const AMD64_CONTEXT *)context;
    DWORD arm64_flags = context->ContextFlags;
    DWORD amd64_flags = amd64_context->ContextFlags;
    DWORD flags = arm64_flags & ~CONTEXT_ARM64;

    if ((amd64_flags & CONTEXT_AMD64) && !(arm64_flags & CONTEXT_ARM64))
        return macrunner_hb_set_x64_thread_context( handle, (const AMD64_CONTEXT *)context );

    if (self && (flags & CONTEXT_DEBUG_REGISTERS)) self = FALSE;

    if (!self)
    {
        ret = set_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (ret || !self) return ret;
    }

    if (flags & CONTEXT_INTEGER)
    {
        memcpy( frame->x, context->X, sizeof(context->X[0]) * 18 );
        /* skip x18 */
        memcpy( frame->x + 19, context->X + 19, sizeof(context->X[0]) * 10 );
    }
    if (flags & CONTEXT_CONTROL)
    {
        frame->fp    = context->Fp;
        frame->lr    = context->Lr;
        frame->sp    = context->Sp;
        frame->pc    = context->Pc;
        frame->cpsr  = context->Cpsr;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        frame->fpcr = context->Fpcr;
        frame->fpsr = context->Fpsr;
        memcpy( frame->v, context->V, sizeof(frame->v) );
    }
    if (flags & CONTEXT_ARM64_X18)
    {
        frame->x[18] = context->X[18];
    }
    if (flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    frame->restore_flags |= flags & ~CONTEXT_INTEGER;
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              NtGetContextThread  (NTDLL.@)
 *              ZwGetContextThread  (NTDLL.@)
 */
NTSTATUS WINAPI NtGetContextThread( HANDLE handle, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM64;
    BOOL self = (handle == GetCurrentThread());

    if (((AMD64_CONTEXT *)context)->ContextFlags & CONTEXT_AMD64)
    {
        NTSTATUS ret = macrunner_hb_get_x64_thread_context( handle, (AMD64_CONTEXT *)context );
        if (!ret)
            set_context_exception_reporting_flags( &((AMD64_CONTEXT *)context)->ContextFlags,
                                                   CONTEXT_SERVICE_ACTIVE );
        return ret;
    }

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, context, &self, IMAGE_FILE_MACHINE_ARM64 );
        if (ret || !self) return ret;
    }

    if (needed_flags & CONTEXT_INTEGER)
    {
        memcpy( context->X, frame->x, sizeof(context->X[0]) * 29 );
        context->ContextFlags |= CONTEXT_INTEGER;
    }
    if (needed_flags & CONTEXT_CONTROL)
    {
        context->Fp   = frame->fp;
        context->Lr   = frame->lr;
        context->Sp   = frame->sp;
        context->Pc   = frame->pc;
        context->Cpsr = frame->cpsr;
        context->ContextFlags |= CONTEXT_CONTROL;
    }
    if (needed_flags & CONTEXT_FLOATING_POINT)
    {
        context->Fpcr = frame->fpcr;
        context->Fpsr = frame->fpsr;
        memcpy( context->V, frame->v, sizeof(context->V) );
        context->ContextFlags |= CONTEXT_FLOATING_POINT;
    }
    if (needed_flags & CONTEXT_DEBUG_REGISTERS) FIXME( "debug registers not supported\n" );
    set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              set_thread_wow64_context
 */
NTSTATUS set_thread_wow64_context( HANDLE handle, const void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = set_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame;
        const I386_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_i386;

        if (flags & CONTEXT_I386_INTEGER)
        {
            wow_frame->Eax = context->Eax;
            wow_frame->Ebx = context->Ebx;
            wow_frame->Ecx = context->Ecx;
            wow_frame->Edx = context->Edx;
            wow_frame->Esi = context->Esi;
            wow_frame->Edi = context->Edi;
        }
        if (flags & CONTEXT_I386_CONTROL)
        {
            WOW64_CPURESERVED *cpu = NtCurrentTeb()->TlsSlots[WOW64_TLS_CPURESERVED];

            wow_frame->Esp    = context->Esp;
            wow_frame->Ebp    = context->Ebp;
            wow_frame->Eip    = context->Eip;
            wow_frame->EFlags = context->EFlags;
            wow_frame->SegCs  = context->SegCs;
            wow_frame->SegSs  = context->SegSs;
            cpu->Flags |= WOW64_CPURESERVED_FLAG_RESET_STATE;
        }
        if (flags & CONTEXT_I386_SEGMENTS)
        {
            wow_frame->SegDs = context->SegDs;
            wow_frame->SegEs = context->SegEs;
            wow_frame->SegFs = context->SegFs;
            wow_frame->SegGs = context->SegGs;
        }
        if (flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            wow_frame->Dr0 = context->Dr0;
            wow_frame->Dr1 = context->Dr1;
            wow_frame->Dr2 = context->Dr2;
            wow_frame->Dr3 = context->Dr3;
            wow_frame->Dr6 = context->Dr6;
            wow_frame->Dr7 = context->Dr7;
        }
        if (flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( &wow_frame->ExtendedRegisters, context->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
        }
        if (flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &wow_frame->FloatSave, &context->FloatSave, sizeof(context->FloatSave) );
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame;
        const ARM_CONTEXT *context = ctx;
        DWORD flags = context->ContextFlags & ~CONTEXT_ARM;

        if (flags & CONTEXT_INTEGER)
        {
            wow_frame->R0  = context->R0;
            wow_frame->R1  = context->R1;
            wow_frame->R2  = context->R2;
            wow_frame->R3  = context->R3;
            wow_frame->R4  = context->R4;
            wow_frame->R5  = context->R5;
            wow_frame->R6  = context->R6;
            wow_frame->R7  = context->R7;
            wow_frame->R8  = context->R8;
            wow_frame->R9  = context->R9;
            wow_frame->R10 = context->R10;
            wow_frame->R11 = context->R11;
            wow_frame->R12 = context->R12;
        }
        if (flags & CONTEXT_CONTROL)
        {
            wow_frame->Sp = context->Sp;
            wow_frame->Lr = context->Lr;
            wow_frame->Pc = context->Pc & ~1;
            wow_frame->Cpsr = context->Cpsr;
            if (context->Cpsr & 0x20) wow_frame->Pc |= 1; /* thumb */
        }
        if (flags & CONTEXT_FLOATING_POINT)
        {
            wow_frame->Fpscr = context->Fpscr;
            memcpy( wow_frame->D, context->D, sizeof(context->D) );
        }
        break;
    }

    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *              get_thread_wow64_context
 */
NTSTATUS get_thread_wow64_context( HANDLE handle, void *ctx, ULONG size )
{
    BOOL self = (handle == GetCurrentThread());
    USHORT machine;
    void *frame;

    switch (size)
    {
    case sizeof(I386_CONTEXT): machine = IMAGE_FILE_MACHINE_I386; break;
    case sizeof(ARM_CONTEXT): machine = IMAGE_FILE_MACHINE_ARMNT; break;
    default: return STATUS_INFO_LENGTH_MISMATCH;
    }

    if (!self)
    {
        NTSTATUS ret = get_thread_context( handle, ctx, &self, machine );
        if (ret || !self) return ret;
    }

    if (!(frame = get_cpu_area( machine ))) return STATUS_INVALID_PARAMETER;

    switch (machine)
    {
    case IMAGE_FILE_MACHINE_I386:
    {
        I386_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_i386;

        if (needed_flags & CONTEXT_I386_INTEGER)
        {
            context->Eax = wow_frame->Eax;
            context->Ebx = wow_frame->Ebx;
            context->Ecx = wow_frame->Ecx;
            context->Edx = wow_frame->Edx;
            context->Esi = wow_frame->Esi;
            context->Edi = wow_frame->Edi;
            context->ContextFlags |= CONTEXT_I386_INTEGER;
        }
        if (needed_flags & CONTEXT_I386_CONTROL)
        {
            context->Esp    = wow_frame->Esp;
            context->Ebp    = wow_frame->Ebp;
            context->Eip    = wow_frame->Eip;
            context->EFlags = wow_frame->EFlags;
            context->SegCs  = wow_frame->SegCs;
            context->SegSs  = wow_frame->SegSs;
            context->ContextFlags |= CONTEXT_I386_CONTROL;
        }
        if (needed_flags & CONTEXT_I386_SEGMENTS)
        {
            context->SegDs = wow_frame->SegDs;
            context->SegEs = wow_frame->SegEs;
            context->SegFs = wow_frame->SegFs;
            context->SegGs = wow_frame->SegGs;
            context->ContextFlags |= CONTEXT_I386_SEGMENTS;
        }
        if (needed_flags & CONTEXT_I386_EXTENDED_REGISTERS)
        {
            memcpy( context->ExtendedRegisters, &wow_frame->ExtendedRegisters, sizeof(context->ExtendedRegisters) );
            context->ContextFlags |= CONTEXT_I386_EXTENDED_REGISTERS;
        }
        if (needed_flags & CONTEXT_I386_FLOATING_POINT)
        {
            memcpy( &context->FloatSave, &wow_frame->FloatSave, sizeof(context->FloatSave) );
            context->ContextFlags |= CONTEXT_I386_FLOATING_POINT;
        }
        if (needed_flags & CONTEXT_I386_DEBUG_REGISTERS)
        {
            context->Dr0 = wow_frame->Dr0;
            context->Dr1 = wow_frame->Dr1;
            context->Dr2 = wow_frame->Dr2;
            context->Dr3 = wow_frame->Dr3;
            context->Dr6 = wow_frame->Dr6;
            context->Dr7 = wow_frame->Dr7;
        }
        /* FIXME: CONTEXT_I386_XSTATE */
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    case IMAGE_FILE_MACHINE_ARMNT:
    {
        ARM_CONTEXT *wow_frame = frame, *context = ctx;
        DWORD needed_flags = context->ContextFlags & ~CONTEXT_ARM;

        if (needed_flags & CONTEXT_INTEGER)
        {
            context->R0  = wow_frame->R0;
            context->R1  = wow_frame->R1;
            context->R2  = wow_frame->R2;
            context->R3  = wow_frame->R3;
            context->R4  = wow_frame->R4;
            context->R5  = wow_frame->R5;
            context->R6  = wow_frame->R6;
            context->R7  = wow_frame->R7;
            context->R8  = wow_frame->R8;
            context->R9  = wow_frame->R9;
            context->R10 = wow_frame->R10;
            context->R11 = wow_frame->R11;
            context->R12 = wow_frame->R12;
            context->ContextFlags |= CONTEXT_INTEGER;
        }
        if (needed_flags & CONTEXT_CONTROL)
        {
            context->Sp   = wow_frame->Sp;
            context->Lr   = wow_frame->Lr;
            context->Pc   = wow_frame->Pc;
            context->Cpsr = wow_frame->Cpsr;
            context->ContextFlags |= CONTEXT_CONTROL;
        }
        if (needed_flags & CONTEXT_FLOATING_POINT)
        {
            context->Fpscr = wow_frame->Fpscr;
            memcpy( context->D, wow_frame->D, sizeof(wow_frame->D) );
            context->ContextFlags |= CONTEXT_FLOATING_POINT;
        }
        set_context_exception_reporting_flags( &context->ContextFlags, CONTEXT_SERVICE_ACTIVE );
        break;
    }

    }
    return STATUS_SUCCESS;
}


#if defined(__APPLE__)
/* macOS arm64 reserves x18 for kernel scratch and clears it on sigreturn.
 * Windows arm64 ABI uses x18 = TEB, so PE code reads TEB-relative fields
 * via x18 and crashes on first access if x18 is zero. We trampoline
 * through this stub: the kernel restores x10/x16 from sigcontext like
 * any other GP reg, the stub runs in user mode AFTER sigreturn, copies
 * TEB into x18 (kernel can no longer touch us), and branches to the
 * real PE entry. Used for "fresh" PE entries (KiUserExceptionDispatcher,
 * __wine_syscall_dispatcher_return) where x10/x16 don't carry caller
 * state. */
extern void __wine_pe_x18_thunk(void);
__ASM_GLOBAL_FUNC( __wine_pe_x18_thunk,
                   __ASM_CFI(".cfi_def_cfa 31,0\n\t")
                   __ASM_CFI(".cfi_same_value 30\n\t")
                   "mov x18, x10\n\t"  /* TEB */
                   "br  x16" )         /* real PE entry */

/* Resume thunk for the x18 self-heal path in segv_handler: PE code was
 * mid-function when xnu-induced x18=NULL faulted on a TEB-relative
 * deref. We must transparently retry the faulting instruction with x18
 * restored AND with x10/x16 untouched relative to PE's expectations.
 * segv_handler stashes the original x10/x16 and the retry PC into the
 * three apple_x18_save_* fields of ntdll_thread_data (TEB-relative);
 * this stub reads them back and jumps. x17 is sacrificed as a branch
 * target — it's an intra-procedure scratch register, not preserved
 * across BLR/RET in either ABI, so the PE compiler never relies on it
 * holding meaningful state across instructions. */
extern void __wine_pe_x18_resume_thunk(void);
__ASM_GLOBAL_FUNC( __wine_pe_x18_resume_thunk,
                   __ASM_CFI(".cfi_def_cfa 31,0\n\t")
                   __ASM_CFI(".cfi_same_value 30\n\t")
                   "mov x18, x10\n\t"           /* x18 = TEB */
                   "ldr x10, [x18, #0x3d8]\n\t" /* TEB_APPLE_X18_SAVE_X10_OFFSET */
                   "ldr x16, [x18, #0x3e0]\n\t" /* TEB_APPLE_X18_SAVE_X16_OFFSET */
                   "ldr x17, [x18, #0x3e8]\n\t" /* TEB_APPLE_X18_SAVE_PC_OFFSET */
                   "br  x17" )

TEB *__wine_get_current_teb_for_x18(void)
{
    return NtCurrentTeb();
}

extern int macrunner_hb_pc_is_x64_guest_code( void *pc );
extern int macrunner_hb_pc_is_x64_guest_code_no_lock( void *pc );
extern int macrunner_hb_pc_is_x64_guest_code_module_no_lock( void *pc );
extern void *macrunner_hb_pe_module_from_pc_no_lock( void *pc );
extern int macrunner_hb_pc_is_pe_code_module_no_lock( void *pc );
extern ULONG64 macrunner_hb_normalize_x64_callback_pc( ULONG64 pc );
extern ULONG64 macrunner_hb_normalize_x64_tls_callback_pc( ULONG64 pc, ULONG64 image_base,
                                                           ULONG64 reason );
extern ULONG64 macrunner_hb_dispatch_x64_callback( ULONG64 target, const ULONG64 args[8] );
extern void macrunner_hb_note_x64_guest_fault_handlers_ready(void);
/* MacRunner 2026-06-24 (HB-throughput direct-mem fast path): recover from a SIGSEGV/SIGBUS that
 * lands inside a gated direct guest-memory copy in special_read/write. siglongjmps back (does not
 * return) when the fault addr is inside the active copy's guest range; no-op otherwise. Must be
 * called at the TOP of the segv/bus handlers, before any lock. Async-signal-safe. */
extern void macrunner_hb_dmem_fault_recover( unsigned long long fault_addr );
/* MacRunner 2026-07-02: mprotect a PROT_NONE/PROT_READ page to RW when max_prot allows WRITE
 * (commit-on-fault for a reserved-but-committable or read-only-mapped guest page). Returns TRUE
 * if the page was upgraded and the faulting access should simply be retried by returning from
 * the signal handler. See macrunner_hb.c for the strict scope (never masks a genuine AV/OOB). */
extern BOOL macrunner_hb_try_commit_or_upgrade_page( unsigned long long addr );
extern void macrunner_hb_x64_callback_trampoline(void);
static BOOL macrunner_hb_x64_loader_enabled(void);
static BOOL macrunner_hb_trace_callback_route_enabled(void)
{
    const char *value = getenv( "MACRUNNER_HB_TRACE_CALLBACK_ROUTE" );
    return value && value[0] && value[0] != '0';
}

static BOOL macrunner_hb_x64_fault_routing_enabled(void)
{
    if (!macrunner_hb_x64_loader_enabled()) return FALSE;
    if (macrunner_hb_x64_guest_process()) return TRUE;

    /* PE32/WOW64 still executes the 64-bit Wine side through AMD64 guest
     * modules.  A native ARM64 ntdll thunk can therefore fault at a registered
     * x64 guest target even though the process main image is I386. */
    return is_wow64() && current_machine == IMAGE_FILE_MACHINE_ARM64 &&
           main_image_info.Machine == IMAGE_FILE_MACHINE_I386;
}

static BOOL macrunner_hb_trace_stack_setup_enabled(void)
{
    static int count;
    const char *value = getenv( "MACRUNNER_HB_TRACE_STACK_SETUP" );

    return value && value[0] && value[0] != '0' && count++ < 64;
}

static void macrunner_hb_trace_callback_target_module( const char *source, ULONG_PTR pc );

static ULONG_PTR macrunner_hb_normalize_x64_callback_target( ucontext_t *context,
                                                             ULONG_PTR target )
{
    ULONG_PTR tls_target;

    /* TLS callbacks carry the authoritative image base in x0 and reason in x1.
     * Use the image TLS table before falling back to instruction-boundary
     * heuristics, otherwise a fault PC in padding can be dispatched as code. */
    tls_target = macrunner_hb_normalize_x64_tls_callback_pc( target, REGn_sig(0, context),
                                                             REGn_sig(1, context) );
    if (tls_target != target) return tls_target;
    return macrunner_hb_normalize_x64_callback_pc( target );
}

static ULONG_PTR macrunner_hb_normalize_explicit_x64_callback_target( ucontext_t *context,
                                                                      ULONG_PTR target )
{
    /* x4 is the native caller's explicit indirect-call target, not a macOS
     * fault PC sampled from an ARM64 fetch.  Keep the instruction-boundary
     * heuristics for raw/fault PCs only; they can move valid MinGW TLS callback
     * entries such as __dyn_tls_dtor one byte backwards into padding. */
    return macrunner_hb_normalize_x64_tls_callback_pc( target, REGn_sig(0, context),
                                                       REGn_sig(1, context) );
}

static void macrunner_signal_copy_bytes( void *dst, const void *src, size_t size )
{
    volatile unsigned char *d = dst;
    const volatile unsigned char *s = src;

    while (size--) *d++ = *s++;
}

static BOOL macrunner_signal_read_memory( void *dst, const void *src, size_t size )
{
    if (!size) return TRUE;
    if (!dst || !src) return FALSE;
#ifdef __APPLE__
    {
        mach_vm_size_t out_size = 0;
        kern_return_t kr = mach_vm_read_overwrite( mach_task_self(), (mach_vm_address_t)src,
                                                   (mach_vm_size_t)size,
                                                   (mach_vm_address_t)dst, &out_size );
        return kr == KERN_SUCCESS && out_size == size;
    }
#else
    if (!virtual_is_valid_code_address( (void *)src, size )) return FALSE;
    macrunner_signal_copy_bytes( dst, src, size );
    return TRUE;
#endif
}

static BOOL macrunner_signal_write_memory( void *dst, const void *src, size_t size )
{
    if (!size) return TRUE;
    if (!dst || !src) return FALSE;
#ifdef __APPLE__
    if ((mach_msg_type_number_t)size != size) return FALSE;
    return mach_vm_write( mach_task_self(), (mach_vm_address_t)dst,
                          (vm_offset_t)(uintptr_t)src,
                          (mach_msg_type_number_t)size ) == KERN_SUCCESS;
#else
    macrunner_signal_copy_bytes( dst, src, size );
    return TRUE;
#endif
}

static BOOL macrunner_signal_read_u32_aligned( ULONG_PTR pc, ULONG *instr )
{
    if (!instr || (pc & 3)) return FALSE;
    return macrunner_signal_read_memory( instr, (void *)pc, sizeof(*instr) );
}

static void macrunner_signal_writef( const char *format, ... )
{
    char buffer[512];
    va_list args;
    int len;

    va_start( args, format );
    len = vsnprintf( buffer, sizeof(buffer), format, args );
    va_end( args );
    if (len <= 0) return;
    if ((size_t)len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    write( STDERR_FILENO, buffer, len );
}

struct macrunner_hb_signal_module_info
{
    void *base;
    ULONG_PTR rva;
    ULONG size;
    char name[64];
};

static void macrunner_hb_signal_copy_unicode_name( const UNICODE_STRING *src,
                                                   char *dst, size_t dst_size )
{
    size_t count, i;

    if (!dst || !dst_size) return;
    strcpy( dst, "unknown" );
    if (!src || !src->Buffer || !src->Length) return;

    count = src->Length / sizeof(WCHAR);
    if (count >= dst_size) count = dst_size - 1;
    for (i = 0; i < count; i++)
    {
        WCHAR ch = 0;

        if (!macrunner_signal_read_memory( &ch, src->Buffer + i, sizeof(ch) )) break;
        dst[i] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
    }
    dst[i] = 0;
    if (!i) strcpy( dst, "unknown" );
}

static BOOL macrunner_hb_signal_find_loader_module( ULONG_PTR pc,
                                                    struct macrunner_hb_signal_module_info *info )
{
    TEB *teb = NtCurrentTeb();
    PEB_LDR_DATA *ldr;
    PEB_LDR_DATA ldr_copy;
    LIST_ENTRY *head, *entry;
    unsigned int i;

    if (!info) return FALSE;
    memset( info, 0, sizeof(*info) );
    strcpy( info->name, "unknown" );
    if (!pc || !teb || !teb->Peb) return FALSE;
    if (!(ldr = teb->Peb->LdrData)) return FALSE;
    if (!macrunner_signal_read_memory( &ldr_copy, ldr, sizeof(ldr_copy) )) return FALSE;

    head = &ldr->InMemoryOrderModuleList;
    entry = ldr_copy.InMemoryOrderModuleList.Flink;
    for (i = 0; i < 256 && entry && entry != head; i++)
    {
        LDR_DATA_TABLE_ENTRY mod;
        LDR_DATA_TABLE_ENTRY *mod_ptr =
            CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks );
        ULONG_PTR base, size;
        LIST_ENTRY *next;

        if (!macrunner_signal_read_memory( &mod, mod_ptr, sizeof(mod) )) break;
        base = (ULONG_PTR)mod.DllBase;
        size = mod.SizeOfImage;
        if (base && size && pc >= base && pc - base < size)
        {
            info->base = mod.DllBase;
            info->rva = pc - base;
            info->size = mod.SizeOfImage;
            macrunner_hb_signal_copy_unicode_name( &mod.BaseDllName, info->name,
                                                   sizeof(info->name) );
            return TRUE;
        }
        next = mod.InMemoryOrderLinks.Flink;
        if (next == entry) break;
        entry = next;
    }
    return FALSE;
}

void macrunner_hb_trace_x64_callback_preserve( ULONG64 target, ULONG64 saved_x26,
                                               ULONG64 current_x26, ULONG64 saved_x27,
                                               ULONG64 current_x27 )
{
    if (!macrunner_hb_trace_callback_route_enabled()) return;
    ERR( "macrunner-hb-callback-preserve: target=%p saved_x26=%p current_x26=%p "
         "saved_x27=%p current_x27=%p\n",
         (void *)(ULONG_PTR)target, (void *)(ULONG_PTR)saved_x26,
         (void *)(ULONG_PTR)current_x26, (void *)(ULONG_PTR)saved_x27,
         (void *)(ULONG_PTR)current_x27 );
}

/* This trampoline is entered as an ARM64 PE callee when Wine native code calls
 * an x64 callback target.  Preserve the full AAPCS64 callee-saved set around
 * the HyperBridge dispatch; native callers commonly keep long-lived state in
 * x19-x28 (RtlProcessFlsData uses x26 for fls_data across FLS callbacks). */
__ASM_GLOBAL_FUNC( macrunner_hb_x64_callback_trampoline,
                   "stp x29, x30, [sp, #-0x100]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0x100\n\t")
                   __ASM_CFI(".cfi_offset 29,-0x100\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xf8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x0, x1, [x29, #0x10]\n\t"
                   "stp x2, x3, [x29, #0x20]\n\t"
                   "stp x4, x5, [x29, #0x30]\n\t"
                   "stp x6, x7, [x29, #0x40]\n\t"
                   "str x16, [x29, #0x58]\n\t"
                   "stp x19, x20, [x29, #0x60]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x60\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x68\n\t")
                   "stp x21, x22, [x29, #0x70]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x70\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x78\n\t")
                   "stp x23, x24, [x29, #0x80]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x80\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x88\n\t")
                   "stp x25, x26, [x29, #0x90]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x90\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x98\n\t")
                   "stp x27, x28, [x29, #0xa0]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0xa0\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0xa8\n\t")
                   "stp d8,  d9,  [x29, #0xb0]\n\t"
                   "stp d10, d11, [x29, #0xc0]\n\t"
                   "stp d12, d13, [x29, #0xd0]\n\t"
                   "stp d14, d15, [x29, #0xe0]\n\t"
                   "bl " __ASM_NAME("__wine_get_current_teb_for_x18") "\n\t"
                   "str x0, [x29, #0x50]\n\t"
                   "mov x18, x0\n\t"
                   "ldr x0, [x29, #0x58]\n\t"
                   "add x1, x29, #0x10\n\t"
                   "bl " __ASM_NAME("macrunner_hb_dispatch_x64_callback") "\n\t"
                   "str x0, [x29, #0xf0]\n\t"
                   "ldr x18, [x29, #0x50]\n\t"
                   "ldr x0, [x29, #0x58]\n\t"
                   "ldr x1, [x29, #0x98]\n\t"
                   "mov x2, x26\n\t"
                   "ldr x3, [x29, #0xa0]\n\t"
                   "mov x4, x27\n\t"
                   "bl " __ASM_NAME("macrunner_hb_trace_x64_callback_preserve") "\n\t"
                   "ldr x18, [x29, #0x50]\n\t"
                   "ldr x0, [x29, #0xf0]\n\t"
                   "ldp d14, d15, [x29, #0xe0]\n\t"
                   "ldp d12, d13, [x29, #0xd0]\n\t"
                   "ldp d10, d11, [x29, #0xc0]\n\t"
                   "ldp d8,  d9,  [x29, #0xb0]\n\t"
                   "ldp x27, x28, [x29, #0xa0]\n\t"
                   __ASM_CFI(".cfi_same_value 27\n\t")
                   __ASM_CFI(".cfi_same_value 28\n\t")
                   "ldp x25, x26, [x29, #0x90]\n\t"
                   __ASM_CFI(".cfi_same_value 25\n\t")
                   __ASM_CFI(".cfi_same_value 26\n\t")
                   "ldp x23, x24, [x29, #0x80]\n\t"
                   __ASM_CFI(".cfi_same_value 23\n\t")
                   __ASM_CFI(".cfi_same_value 24\n\t")
                   "ldp x21, x22, [x29, #0x70]\n\t"
                   __ASM_CFI(".cfi_same_value 21\n\t")
                   __ASM_CFI(".cfi_same_value 22\n\t")
                   "ldp x19, x20, [x29, #0x60]\n\t"
                   __ASM_CFI(".cfi_same_value 19\n\t")
                   __ASM_CFI(".cfi_same_value 20\n\t")
                   "ldp x29, x30, [sp], #0x100\n\t"
                   "ret" )

static BOOL macrunner_hb_redirect_arm64x_hexpthk_sigill( ucontext_t *context );

static BOOL macrunner_hb_route_x64_callback_fault( ucontext_t *context, ULONG_PTR fault_addr,
                                                   const char *source )
{
    ULONG_PTR pc, raw_pc, x4_target, x16_target;
    BOOL raw_is_guest, fault_is_guest, x4_is_guest, x16_is_guest, sigill_source;
    static int rejected_trace_count;

    /* A native ARM64 indirect call can land on an imported ARM64X x64 entry thunk
     * (optionally still carrying the CodeMap type tag in the low 2 bits).  Redirect
     * straight to the native ARM64 target rather than emulating the thunk as an x64
     * callback: the tag puts the PC mid-instruction (the recorded spin/OOM) and even
     * the aligned thunk cannot be JIT-executed.  This covers the SEGV/BUS fault
     * sources too (ill_handler already tries the redirect before reaching here). */
    if (macrunner_hb_redirect_arm64x_hexpthk_sigill( context )) return TRUE;

    if (!macrunner_hb_x64_fault_routing_enabled()) return FALSE;
    raw_pc = PC_sig(context);
    x4_target = REGn_sig(4, context);
    x16_target = REGn_sig(16, context);
    raw_is_guest = macrunner_hb_pc_is_x64_guest_code_no_lock( (void *)raw_pc );
    fault_is_guest = macrunner_hb_pc_is_x64_guest_code_no_lock( (void *)fault_addr );
    x4_is_guest = macrunner_hb_pc_is_x64_guest_code_no_lock( (void *)x4_target );
    x16_is_guest = macrunner_hb_pc_is_x64_guest_code_no_lock( (void *)x16_target );
    sigill_source = source && (!strcmp( source, "sigill" ) || !strcmp( source, "primary-ill" ));

    /* MacRunner diag: a NULL-pointer fault in the guest — either execute-at-0
     * (raw_pc==0, calling a NULL function pointer) or a NULL-page data deref
     * (fault_addr in the first page).  Pin the guest call/deref site from the
     * registered x64 context (guest RSP/RIP are not in an ARM64 reg here).  Use a
     * DEDICATED env (not the broad callback-route trace, which floods per-callback
     * stderr and starves the boot before graphics). */
    if (raw_pc == 0 || fault_addr < 0x1000)
    {
        static int nullcall_diag_count;
        const char *nv = getenv( "MACRUNNER_HB_TRACE_NULLCALL" );
        if (nv && nv[0] && nv[0] != '0' && nullcall_diag_count++ < 64)
            macrunner_hb_trace_nullcall_site( source, raw_pc, fault_addr );
    }

    if (sigill_source)
    {
        if (!raw_is_guest)
            raw_is_guest = macrunner_hb_pc_is_x64_guest_code_module_no_lock( (void *)raw_pc );
        if (fault_addr && !fault_is_guest)
            fault_is_guest = macrunner_hb_pc_is_x64_guest_code_module_no_lock( (void *)fault_addr );
        if (!x4_is_guest)
            x4_is_guest = macrunner_hb_pc_is_x64_guest_code_module_no_lock( (void *)x4_target );
        if (!x16_is_guest)
            x16_is_guest = macrunner_hb_pc_is_x64_guest_code_module_no_lock( (void *)x16_target );
    }

    /* Wine's ARM64EC-style indirect-call path can leave the real x64 target
     * in x4 while the architectural fault PC points at dispatch residue,
     * garbage, or a few bytes into x64 text after an ARM64 fetch attempt.
     * Prefer that explicit target only when the fault PC/address already
     * identifies x64 guest execution.  Otherwise a stale native x4 register
     * can manufacture a bogus callback from an ordinary ARM64 fault. */
    if (sigill_source && x16_is_guest && (raw_is_guest || fault_is_guest))
    {
        pc = macrunner_hb_normalize_explicit_x64_callback_target( context, x16_target );
        TRACE( "MacRunner Phase F using x16 x64 callback target raw_pc=%p x16=%p normalized=%p\n",
               (void *)raw_pc, (void *)x16_target, (void *)pc );
    }
    else if (x4_is_guest && (raw_is_guest || fault_is_guest))
    {
        pc = macrunner_hb_normalize_explicit_x64_callback_target( context, x4_target );
        TRACE( "MacRunner Phase F using x4 x64 callback target raw_pc=%p x4=%p normalized=%p\n",
               (void *)raw_pc, (void *)x4_target, (void *)pc );
    }
    else if (fault_is_guest)
        pc = macrunner_hb_normalize_x64_callback_target( context, fault_addr );
    else if (raw_is_guest)
        pc = macrunner_hb_normalize_x64_callback_target( context, raw_pc );
    else
    {
        if (macrunner_hb_trace_callback_route_enabled() && rejected_trace_count++ < 96)
            ERR( "macrunner-hb-callback-route-reject: source=%s raw_pc=%p fault=%p "
                 "lr=%p sp=%p x0=%p x1=%p x2=%p x3=%p x4=%p x16=%p x18=%p "
                 "x19=%p x20=%p x24=%p x26=%p x27=%p x28=%p\n",
                 source, (void *)raw_pc, (void *)fault_addr,
                 (void *)(ULONG_PTR)LR_sig(context), (void *)(ULONG_PTR)SP_sig(context),
                 (void *)(ULONG_PTR)REGn_sig(0, context),
                 (void *)(ULONG_PTR)REGn_sig(1, context),
                 (void *)(ULONG_PTR)REGn_sig(2, context),
                 (void *)(ULONG_PTR)REGn_sig(3, context),
                 (void *)(ULONG_PTR)x4_target,
                 (void *)(ULONG_PTR)REGn_sig(16, context),
                 (void *)(ULONG_PTR)REGn_sig(18, context),
                 (void *)(ULONG_PTR)REGn_sig(19, context),
                 (void *)(ULONG_PTR)REGn_sig(20, context),
                 (void *)(ULONG_PTR)REGn_sig(24, context),
                 (void *)(ULONG_PTR)REGn_sig(26, context),
                 (void *)(ULONG_PTR)REGn_sig(27, context),
                 (void *)(ULONG_PTR)REGn_sig(28, context) );
        return FALSE;
    }

    TRACE( "MacRunner Phase F routing %s x64 callback pc=%p lr=%p sp=%p "
           "x0=%p x1=%p x2=%p x3=%p x4=%p x5=%p\n",
           source, (void *)pc, (void *)(ULONG_PTR)LR_sig(context),
           (void *)(ULONG_PTR)SP_sig(context),
           (void *)(ULONG_PTR)REGn_sig(0, context),
           (void *)(ULONG_PTR)REGn_sig(1, context),
           (void *)(ULONG_PTR)REGn_sig(2, context),
           (void *)(ULONG_PTR)REGn_sig(3, context),
           (void *)(ULONG_PTR)REGn_sig(4, context),
           (void *)(ULONG_PTR)REGn_sig(5, context) );
    if (macrunner_hb_trace_callback_route_enabled())
    {
        macrunner_hb_trace_callback_target_module( source, pc );
        ERR( "macrunner-hb-callback-route: source=%s raw_pc=%p fault=%p normalized=%p "
             "lr=%p sp=%p x4=%p x16=%p x19=%p x26=%p x27=%p x28=%p\n",
             source, (void *)raw_pc, (void *)fault_addr, (void *)pc, (void *)(ULONG_PTR)LR_sig(context),
             (void *)(ULONG_PTR)SP_sig(context), (void *)(ULONG_PTR)x4_target,
             (void *)(ULONG_PTR)REGn_sig(16, context),
             (void *)(ULONG_PTR)REGn_sig(19, context),
             (void *)(ULONG_PTR)REGn_sig(26, context),
             (void *)(ULONG_PTR)REGn_sig(27, context),
             (void *)(ULONG_PTR)REGn_sig(28, context) );
    }
    REGn_sig(16, context) = pc;
    REGn_sig(18, context) = (ULONG_PTR)NtCurrentTeb();
    PC_sig(context) = (ULONG_PTR)macrunner_hb_x64_callback_trampoline;
    return TRUE;
}

static BOOL macrunner_hb_wow64_i386_execute_fault( ucontext_t *context,
                                                   const EXCEPTION_RECORD *rec )
{
    ULONG_PTR pc;

    if (!is_wow64() || current_machine != IMAGE_FILE_MACHINE_ARM64 ||
        main_image_info.Machine != IMAGE_FILE_MACHINE_I386)
        return FALSE;
    if (!rec || rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        rec->NumberParameters < 2 || rec->ExceptionInformation[0] != EXCEPTION_EXECUTE_FAULT)
        return FALSE;

    pc = rec->ExceptionInformation[1];
    return pc && pc == PC_sig(context) && pc <= 0xffffffffu;
}

static BOOL apple_x18_pc_is_resume_thunk( ULONG_PTR pc )
{
    ULONG_PTR resume = (ULONG_PTR)__wine_pe_x18_resume_thunk;
    return pc >= resume && pc < resume + 20;
}

static BOOL macrunner_hb_x64_loader_enabled(void)
{
    const char *value = getenv( "MACRUNNER_HB_X64_LOADER" );
    return value && value[0] && value[0] != '0';
}

#define WINE_RESTORE_X18_IF_ZERO                                            \
                   "cbnz x18, 9f\n\t"                                      \
                   "sub sp, sp, #0xb0\n\t"                                 \
                   "stp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "stp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "stp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "stp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "stp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "stp x10, x11, [sp, #0x50]\n\t"                         \
                   "stp x12, x13, [sp, #0x60]\n\t"                         \
                   "stp x14, x15, [sp, #0x70]\n\t"                         \
                   "stp x16, x17, [sp, #0x80]\n\t"                         \
                   "str x30, [sp, #0x90]\n\t"                              \
                   "mrs x17, NZCV\n\t"                                     \
                   "str x17, [sp, #0x98]\n\t"                              \
                   "bl " __ASM_NAME("__wine_get_current_teb_for_x18") "\n\t" \
                   "mov x18, x0\n\t"                                       \
                   "ldr x17, [sp, #0x98]\n\t"                              \
                   "msr NZCV, x17\n\t"                                     \
                   "ldr x30, [sp, #0x90]\n\t"                              \
                   "ldp x16, x17, [sp, #0x80]\n\t"                         \
                   "ldp x14, x15, [sp, #0x70]\n\t"                         \
                   "ldp x12, x13, [sp, #0x60]\n\t"                         \
                   "ldp x10, x11, [sp, #0x50]\n\t"                         \
                   "ldp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "ldp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "ldp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "ldp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "ldp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "add sp, sp, #0xb0\n"                                  \
                   "9:\n\t"

#define WINE_RESTORE_X18_FROM_TEB                                           \
                   "sub sp, sp, #0xb0\n\t"                                 \
                   "stp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "stp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "stp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "stp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "stp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "stp x10, x11, [sp, #0x50]\n\t"                         \
                   "stp x12, x13, [sp, #0x60]\n\t"                         \
                   "stp x14, x15, [sp, #0x70]\n\t"                         \
                   "stp x16, x17, [sp, #0x80]\n\t"                         \
                   "str x30, [sp, #0x90]\n\t"                              \
                   "mrs x17, NZCV\n\t"                                     \
                   "str x17, [sp, #0x98]\n\t"                              \
                   "bl " __ASM_NAME("__wine_get_current_teb_for_x18") "\n\t" \
                   "mov x18, x0\n\t"                                       \
                   "ldr x17, [sp, #0x98]\n\t"                              \
                   "msr NZCV, x17\n\t"                                     \
                   "ldr x30, [sp, #0x90]\n\t"                              \
                   "ldp x16, x17, [sp, #0x80]\n\t"                         \
                   "ldp x14, x15, [sp, #0x70]\n\t"                         \
                   "ldp x12, x13, [sp, #0x60]\n\t"                         \
                   "ldp x10, x11, [sp, #0x50]\n\t"                         \
                   "ldp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "ldp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "ldp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "ldp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "ldp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "add sp, sp, #0xb0\n\t"

#define WINE_LOAD_TEB_IN_X17                                                \
                   "sub sp, sp, #0xb0\n\t"                                 \
                   "stp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "stp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "stp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "stp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "stp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "stp x10, x11, [sp, #0x50]\n\t"                         \
                   "stp x12, x13, [sp, #0x60]\n\t"                         \
                   "stp x14, x15, [sp, #0x70]\n\t"                         \
                   "stp x16, x17, [sp, #0x80]\n\t"                         \
                   "str x30, [sp, #0x90]\n\t"                              \
                   "mrs x16, NZCV\n\t"                                     \
                   "str x16, [sp, #0x98]\n\t"                              \
                   "bl " __ASM_NAME("__wine_get_current_teb_for_x18") "\n\t" \
                   "mov x17, x0\n\t"                                       \
                   "mov x18, x0\n\t"                                       \
                   "ldr x16, [sp, #0x98]\n\t"                              \
                   "msr NZCV, x16\n\t"                                     \
                   "ldr x30, [sp, #0x90]\n\t"                              \
                   "ldr x16, [sp, #0x80]\n\t"                              \
                   "ldp x14, x15, [sp, #0x70]\n\t"                         \
                   "ldp x12, x13, [sp, #0x60]\n\t"                         \
                   "ldp x10, x11, [sp, #0x50]\n\t"                         \
                   "ldp x8,  x9,  [sp, #0x40]\n\t"                         \
                   "ldp x6,  x7,  [sp, #0x30]\n\t"                         \
                   "ldp x4,  x5,  [sp, #0x20]\n\t"                         \
                   "ldp x2,  x3,  [sp, #0x10]\n\t"                         \
                   "ldp x0,  x1,  [sp, #0x00]\n\t"                         \
                   "add sp, sp, #0xb0\n\t"

static void setup_x18_resume_from_sigcontext( ucontext_t *context )
{
    TEB *teb = NtCurrentTeb();
    struct ntdll_thread_data *thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    ULONG_PTR pc = PC_sig(context);

    /* The resume thunk itself is vulnerable to asynchronous signals on
     * macOS: xnu clears x18 on sigreturn, and Wine may suspend/debug a
     * thread while it is between "mov x18, x10" and "br x17".  Do not
     * overwrite apple_x18_save_pc in that window, otherwise x17 reloads
     * the thunk address and we spin forever in __wine_pe_x18_resume_thunk.
     * Re-enter the thunk from the beginning with x10=TEB and preserve the
     * original saved x10/x16/pc slots. */
    if (macrunner_hb_x64_loader_enabled() && apple_x18_pc_is_resume_thunk( pc ))
    {
        REGn_sig(10, context) = (ULONG_PTR)teb;
        REGn_sig(18, context) = (ULONG_PTR)teb;
        PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
        return;
    }

    thread_data->apple_x18_save_x10 = REGn_sig(10, context);
    thread_data->apple_x18_save_x16 = REGn_sig(16, context);
    thread_data->apple_x18_save_pc  = PC_sig(context);
    REGn_sig(10, context) = (ULONG_PTR)teb;
    REGn_sig(18, context) = (ULONG_PTR)teb;
    PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
}
#endif

static BOOL macrunner_hb_get_callback_exception_stack( ucontext_t *context, void **stack_ptr )
{
    static int report_count;
    TEB *teb = NtCurrentTeb();
    struct syscall_frame *frame = get_syscall_frame();
    char *sp = (char *)SP_sig( context );
    char *limit, *base, *saved_sp;

    if (!teb || !frame || !frame->sp) return FALSE;
    limit = teb->Tib.StackLimit;
    base = teb->Tib.StackBase;
    if (!limit || !base || limit >= base) return FALSE;

    if (sp >= limit && sp < base) return FALSE;
    saved_sp = (char *)(ULONG_PTR)frame->sp;
    if (saved_sp <= limit || saved_sp > base) return FALSE;

    if (stack_ptr) *stack_ptr = saved_sp;
    if (report_count++ < 16)
    {
        struct macrunner_hb_signal_module_info pc_info, lr_info;
        Dl_info sig_info;
        BOOL have_pc = macrunner_hb_signal_find_loader_module( (ULONG_PTR)frame->pc, &pc_info );
        BOOL have_lr = macrunner_hb_signal_find_loader_module( (ULONG_PTR)frame->lr, &lr_info );
        BOOL have_sig = dladdr( (void *)(ULONG_PTR)PC_sig( context ), &sig_info );

        macrunner_signal_writef( "macrunner-hb-callback-exception-stack: pid=%d "
                                 "pc=%p sp=%p delivery_sp=%p frame=%p frame_sp=%p "
                                 "sig_x10=%p sig_x16=%p sig_x18=%p frame_prev=%p "
                                 "frame_pc=%p frame_lr=%p kernel_stack=%p teb_stack=%p-%p\n",
                                 getpid(), (void *)(ULONG_PTR)PC_sig( context ), sp,
                                 saved_sp, frame, (void *)(ULONG_PTR)frame->sp,
                                 (void *)(ULONG_PTR)REGn_sig( 10, context ),
                                 (void *)(ULONG_PTR)REGn_sig( 16, context ),
                                 (void *)(ULONG_PTR)REGn_sig( 18, context ),
                                 frame->prev_frame,
                                 (void *)(ULONG_PTR)frame->pc,
                                 (void *)(ULONG_PTR)frame->lr,
                                 ntdll_get_thread_data()->kernel_stack, limit, base );
        macrunner_signal_writef( "macrunner-hb-callback-exception-sig-map: pid=%d "
                                 "sig_pc=%p sig_image=%s sig_base=%p sig_rva=0x%llx "
                                 "sig_symbol=%s sig_symbol_addr=%p sig_symbol_off=0x%llx "
                                 "sig_x10=%p sig_x16=%p sig_x18=%p\n",
                                 getpid(), (void *)(ULONG_PTR)PC_sig( context ),
                                 have_sig && sig_info.dli_fname ? sig_info.dli_fname : "(none)",
                                 have_sig ? sig_info.dli_fbase : NULL,
                                 have_sig && sig_info.dli_fbase ?
                                     (unsigned long long)(PC_sig( context ) - (ULONG_PTR)sig_info.dli_fbase) : 0,
                                 have_sig && sig_info.dli_sname ? sig_info.dli_sname : "(none)",
                                 have_sig ? sig_info.dli_saddr : NULL,
                                 have_sig && sig_info.dli_saddr ?
                                     (unsigned long long)(PC_sig( context ) - (ULONG_PTR)sig_info.dli_saddr) : 0,
                                 (void *)(ULONG_PTR)REGn_sig( 10, context ),
                                 (void *)(ULONG_PTR)REGn_sig( 16, context ),
                                 (void *)(ULONG_PTR)REGn_sig( 18, context ) );
        macrunner_signal_writef( "macrunner-hb-callback-exception-frame-map: pid=%d "
                                 "frame_pc=%p pc_module=%s pc_native=%p pc_rva=0x%llx "
                                 "frame_lr=%p lr_module=%s lr_native=%p lr_rva=0x%llx "
                                 "pc_found=%u lr_found=%u\n",
                                 getpid(), (void *)(ULONG_PTR)frame->pc,
                                 pc_info.name, pc_info.base,
                                 (unsigned long long)pc_info.rva,
                                 (void *)(ULONG_PTR)frame->lr,
                                 lr_info.name, lr_info.base,
                                 (unsigned long long)lr_info.rva,
                                 have_pc, have_lr );
    }
    return TRUE;
}

/***********************************************************************
 *           setup_raise_exception
 */
static void setup_raise_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct exc_stack_layout layout;
    struct exc_stack_layout *stack;
    void *stack_ptr = (void *)(SP_sig(sigcontext) & ~15);
    void *delivery_stack_ptr = stack_ptr;
    NTSTATUS status;

    if (macrunner_hb_trace_callback_route_enabled())
        ERR( "macrunner-hb-setup-raise: pid=%d code=%#lx flags=%#lx addr=%p "
             "pc=%p sp=%p stack_ptr=%p dispatcher=%p\n",
             getpid(), rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress,
             (void *)(ULONG_PTR)context->Pc, (void *)(ULONG_PTR)context->Sp,
             stack_ptr, pKiUserExceptionDispatcher );

    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP)
    {
        context->Cpsr &= ~MACRUNNER_ARM64_CPSR_TRAP;
        PSTATE_sig(sigcontext) &= ~MACRUNNER_ARM64_CPSR_TRAP;
    }

    status = send_debug_event( rec, context, TRUE, TRUE );
    if (status == DBG_CONTINUE || status == DBG_EXCEPTION_HANDLED)
    {
        restore_context( context, sigcontext );
        return;
    }

    /* fix up instruction pointer in context for EXCEPTION_BREAKPOINT */
    if (rec->ExceptionCode == EXCEPTION_BREAKPOINT) context->Pc -= 4;

    if (macrunner_hb_get_callback_exception_stack( sigcontext, &delivery_stack_ptr ))
    {
        static int callback_rec_count;
        struct macrunner_hb_signal_module_info context_info;
        BOOL have_context = macrunner_hb_signal_find_loader_module( context->Pc, &context_info );

        if (callback_rec_count++ < 16)
            macrunner_signal_writef( "macrunner-hb-callback-exception-record: pid=%d "
                                     "code=%#lx flags=%#lx addr=%p context_pc=%p context_sp=%p "
                                     "context_module=%s context_native=%p context_rva=0x%llx "
                                     "context_found=%u context_cpsr=%#lx sig_pstate=%#llx "
                                     "info0=%p info1=%p delivery_sp=%p teb_stack=%p-%p\n",
                                     getpid(), rec->ExceptionCode, rec->ExceptionFlags,
                                     rec->ExceptionAddress, (void *)(ULONG_PTR)context->Pc,
                                     (void *)(ULONG_PTR)context->Sp,
                                     context_info.name, context_info.base,
                                     (unsigned long long)context_info.rva, have_context,
                                     context->Cpsr, (unsigned long long)PSTATE_sig(sigcontext),
                                     rec->NumberParameters > 0 ? (void *)(ULONG_PTR)rec->ExceptionInformation[0] : NULL,
                                     rec->NumberParameters > 1 ? (void *)(ULONG_PTR)rec->ExceptionInformation[1] : NULL,
                                     delivery_stack_ptr,
                                     NtCurrentTeb() ? NtCurrentTeb()->Tib.StackLimit : NULL,
                                     NtCurrentTeb() ? NtCurrentTeb()->Tib.StackBase : NULL );
    }
    stack = virtual_setup_exception( delivery_stack_ptr, sizeof(*stack), rec );
    memset( &layout, 0, sizeof(layout) );
    macrunner_signal_copy_bytes( &layout.rec, rec, sizeof(layout.rec) );
    macrunner_signal_copy_bytes( &layout.context, context, sizeof(layout.context) );
    context_init_empty_xstate( &layout.context, layout.redzone );
    layout.sp = layout.context.Sp;
    layout.pc = layout.context.Pc;
    if (!macrunner_signal_write_memory( stack, &layout, sizeof(layout) ))
    {
        macrunner_signal_writef( "macrunner-hb-exception-stack-write-failed: pid=%d "
                                 "code=%#lx flags=%#lx addr=%p pc=%p sp=%p "
                                 "stack=%p stack_ptr=%p teb_stack=%p-%p\n",
                                 getpid(), rec->ExceptionCode, rec->ExceptionFlags,
                                 rec->ExceptionAddress, (void *)(ULONG_PTR)context->Pc,
                                 (void *)(ULONG_PTR)context->Sp, stack, stack_ptr,
                                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackLimit : NULL,
                                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackBase : NULL );
        abort_thread(1);
    }

    SP_sig(sigcontext) = (ULONG_PTR)stack;
#if defined(__APPLE__)
    REGn_sig(10, sigcontext) = (ULONG_PTR)NtCurrentTeb();
    REGn_sig(16, sigcontext) = (ULONG_PTR)pKiUserExceptionDispatcher;
    PC_sig(sigcontext) = (ULONG_PTR)__wine_pe_x18_thunk;
#else
    PC_sig(sigcontext) = (ULONG_PTR)pKiUserExceptionDispatcher;
#endif
    REGn_sig(18, sigcontext) = (ULONG_PTR)NtCurrentTeb();
    if (rec->ExceptionCode == STATUS_STACK_OVERFLOW && macrunner_hb_trace_stack_setup_enabled())
        ERR( "macrunner-hb-stack-setup: pid=%d stack=%p sig_sp=%p saved_sp=%p "
             "stack_sp=%p stack_pc=%p teb_stack=%p-%p dealloc=%p\n",
             getpid(), stack, (void *)(ULONG_PTR)SP_sig(sigcontext),
             (void *)(ULONG_PTR)layout.context.Sp, (void *)(ULONG_PTR)layout.sp,
             (void *)(ULONG_PTR)layout.pc, NtCurrentTeb() ? NtCurrentTeb()->Tib.StackLimit : NULL,
             NtCurrentTeb() ? NtCurrentTeb()->Tib.StackBase : NULL,
             NtCurrentTeb() ? NtCurrentTeb()->DeallocationStack : NULL );
}


/***********************************************************************
 *           setup_exception
 *
 * Modify the signal context to call the exception raise function.
 */
static void setup_exception( ucontext_t *sigcontext, EXCEPTION_RECORD *rec )
{
    CONTEXT context;

    rec->ExceptionAddress = (void *)PC_sig(sigcontext);
    save_context( &context, sigcontext );
    if (macrunner_hb_trace_callback_route_enabled())
        ERR( "macrunner-hb-setup-exception: pid=%d code=%#lx addr=%p pc=%p sp=%p "
             "x4=%p x16=%p x24=%p x26=%p\n",
             getpid(), rec->ExceptionCode, rec->ExceptionAddress,
             (void *)(ULONG_PTR)PC_sig(sigcontext), (void *)(ULONG_PTR)SP_sig(sigcontext),
             (void *)(ULONG_PTR)REGn_sig(4, sigcontext),
             (void *)(ULONG_PTR)REGn_sig(16, sigcontext),
             (void *)(ULONG_PTR)REGn_sig(24, sigcontext),
             (void *)(ULONG_PTR)REGn_sig(26, sigcontext) );
    setup_raise_exception( sigcontext, rec, &context );
}


/***********************************************************************
 *           call_user_apc_dispatcher
 */
NTSTATUS call_user_apc_dispatcher( CONTEXT *context, unsigned int flags, ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3,
                                   PNTAPCFUNC func, NTSTATUS status )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = context ? context->Sp : frame->sp;
    struct apc_stack_layout *stack;

    if (flags) FIXME( "flags %#x are not supported.\n", flags );

    sp &= ~15;
    stack = (struct apc_stack_layout *)sp - 1;
    if (context)
    {
        memmove( &stack->context, context, sizeof(stack->context) );
        NtSetContextThread( GetCurrentThread(), &stack->context );
    }
    else
    {
        stack->context.ContextFlags = CONTEXT_FULL;
        NtGetContextThread( GetCurrentThread(), &stack->context );
        stack->context.X0 = status;
    }
    stack->func      = func;
    stack->args[0]   = arg1;
    stack->args[1]   = arg2;
    stack->args[2]   = arg3;
    stack->alertable = TRUE;

    frame->sp = (ULONG64)stack;
    frame->pc = (ULONG64)pKiUserApcDispatcher;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 *           call_raise_user_exception_dispatcher
 */
void call_raise_user_exception_dispatcher(void)
{
    get_syscall_frame()->pc = (UINT64)pKiRaiseUserExceptionDispatcher;
}


/***********************************************************************
 *           call_user_exception_dispatcher
 */
NTSTATUS call_user_exception_dispatcher( EXCEPTION_RECORD *rec, CONTEXT *context )
{
    struct syscall_frame *frame = get_syscall_frame();
    struct exc_stack_layout *stack;
    NTSTATUS status = NtSetContextThread( GetCurrentThread(), context );

    if (status) return status;
    stack = (struct exc_stack_layout *)(context->Sp & ~15) - 1;
    memmove( &stack->context, context, sizeof(*context) );
    memmove( &stack->rec, rec, sizeof(*rec) );
    context_init_empty_xstate( &stack->context, stack->redzone );
    stack->sp = stack->context.Sp;
    stack->pc = stack->context.Pc;

    frame->pc = (ULONG64)pKiUserExceptionDispatcher;
    frame->sp = (ULONG64)stack;
    frame->restore_flags |= CONTEXT_CONTROL;
    syscall_frame_fixup_for_fastpath( frame );
    return status;
}


/***********************************************************************
 *           call_user_mode_callback
 */
extern NTSTATUS call_user_mode_callback( ULONG64 user_sp, void **ret_ptr, ULONG *ret_len,
                                         void *func, TEB *teb );
__ASM_GLOBAL_FUNC( call_user_mode_callback,
                   "stp x29, x30, [sp,#-0xd0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xd0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xd0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xc8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "stp d8,  d9,  [x29, #0x60]\n\t"
                   "stp d10, d11, [x29, #0x70]\n\t"
                   "stp d12, d13, [x29, #0x80]\n\t"
                   "stp d14, d15, [x29, #0x90]\n\t"
                   "stp x1, x2, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "mov x18, x4\n\t"              /* teb */
                   "mrs x1, fpcr\n\t"
                   "mrs x2, fpsr\n\t"
                   "bfi x1, x2, #0, #32\n\t"
                   "ldr x2, [x18]\n\t"            /* teb->Tib.ExceptionList */
                   "stp x1, x2, [x29, #0xb0]\n\t"

                   "ldr x7, [x18, #0x378]\n\t"    /* thread_data->syscall_frame */
                   "sub x1, sp, #0x330\n\t"       /* sizeof(struct syscall_frame) */
                   "str x1, [x18, #0x378]\n\t"    /* thread_data->syscall_frame */
                   "add x8, x29, #0xd0\n\t"
                   "stp x7, x8, [x1, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   "ldr w11, [x18, #0x380]\n\t"   /* thread_data->syscall_trace */
                   "cbnz x11, 1f\n\t"
                   /* switch to user stack */
                   "mov sp, x0\n\t"               /* user_sp */
                   "br x3\n"
                   "1:\tmov x19, x18\n\t"         /* teb */
                   "mov x20, x0\n\t"              /* user_sp */
                   "mov x21, x3\n\t"              /* func */
                   "mov sp, x1\n\t"
                   "ldr x1, [x20]\n\t"            /* args */
                   "ldp w2, w0, [x20, #8]\n\t"    /* len, id */
                   "str x0, [x29, #0xc0]\n\t"     /* id */
                   "bl " __ASM_NAME("trace_usercall") "\n\t"
                   "mov x18, x19\n\t"             /* teb */
                   "mov sp, x20\n\t"              /* user_sp */
                   "br x21" )


/***********************************************************************
 *           user_mode_callback_return
 */
extern void DECLSPEC_NORETURN user_mode_callback_return( void *ret_ptr, ULONG ret_len,
                                                         NTSTATUS status, TEB *teb );
__ASM_GLOBAL_FUNC( user_mode_callback_return,
                   "ldr x4, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "ldp x5, x29, [x4,#0x110]\n\t" /* prev_frame,syscall_cfa */
                   "str x5, [x3, #0x378]\n\t"     /* thread_data->syscall_frame */
                   "sub x29, x29, #0xd0\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   __ASM_CFI(".cfi_rel_offset 29,0x00\n\t")
                   __ASM_CFI(".cfi_rel_offset 30,0x08\n\t")
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "ldp x5, x6, [x29, #0xb0]\n\t"
                   "str x6, [x3]\n\t"             /* teb->Tib.ExceptionList */
                   "msr fpcr, x5\n\t"
                   "lsr x5, x5, #32\n\t"
                   "msr fpsr, x5\n\t"
                   "ldp x5, x6, [x29, #0xa0]\n\t" /* ret_ptr, ret_len */
                   "str x0, [x5]\n\t"             /* ret_ptr */
                   "str w1, [x6]\n\t"             /* ret_len */
                   "ldr w11, [x3, #0x380]\n\t"    /* thread_data->syscall_trace */
                   "cbz x11, 1f\n\t"
                   "ldr w3, [x29, #0xc0]\n\t"     /* id */
                   "mov x19, x2\n\t"
                   "bl " __ASM_NAME("trace_userret") "\n\t"
                   "mov x2, x19\n"                /* status */
                   "1:\tldp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_same_value 19\n\t")
                   __ASM_CFI(".cfi_same_value 20\n\t")
                   "ldp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_same_value 21\n\t")
                   __ASM_CFI(".cfi_same_value 22\n\t")
                   "ldp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_same_value 23\n\t")
                   __ASM_CFI(".cfi_same_value 24\n\t")
                   "ldp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_same_value 25\n\t")
                   __ASM_CFI(".cfi_same_value 26\n\t")
                   "ldp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_same_value 27\n\t")
                   __ASM_CFI(".cfi_same_value 28\n\t")
                   "ldp d8,  d9,  [x29, #0x60]\n\t"
                   "ldp d10, d11, [x29, #0x70]\n\t"
                   "ldp d12, d13, [x29, #0x80]\n\t"
                   "ldp d14, d15, [x29, #0x90]\n\t"
                   "mov x0, x2\n\t"               /* status */
                   "mov sp, x29\n\t"
                   "ldp x29, x30, [sp], #0xd0\n\t"
                   "ret" )


/***********************************************************************
 *           user_mode_abort_thread
 */
extern void DECLSPEC_NORETURN user_mode_abort_thread( NTSTATUS status, struct syscall_frame *frame );
__ASM_GLOBAL_FUNC( user_mode_abort_thread,
                   "ldr x1, [x1, #0x118]\n\t"    /* frame->syscall_cfa */
                   "sub x29, x1, #0xc0\n\t"
                   /* switch to kernel stack */
                   "mov sp, x29\n\t"
                   __ASM_CFI(".cfi_def_cfa 29,0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19,-0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20,-0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21,-0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22,-0x98\n\t")
                   __ASM_CFI(".cfi_offset 23,-0x90\n\t")
                   __ASM_CFI(".cfi_offset 24,-0x88\n\t")
                   __ASM_CFI(".cfi_offset 25,-0x80\n\t")
                   __ASM_CFI(".cfi_offset 26,-0x78\n\t")
                   __ASM_CFI(".cfi_offset 27,-0x70\n\t")
                   __ASM_CFI(".cfi_offset 28,-0x68\n\t")
                   "bl " __ASM_NAME("abort_thread") )


/***********************************************************************
 *           KeUserModeCallback
 */
NTSTATUS KeUserModeCallback( ULONG id, const void *args, ULONG len, void **ret_ptr, ULONG *ret_len )
{
    struct syscall_frame *frame = get_syscall_frame();
    ULONG64 sp = (frame->sp - offsetof( struct callback_stack_layout, args_data[len] ) - 16) & ~15;
    struct callback_stack_layout *stack = (struct callback_stack_layout *)sp;
    static int macrunner_callback_trace_count;

    if (getenv( "MACRUNNER_HB_TRACE_USER_CALLBACK" ) && macrunner_callback_trace_count++ < 160)
        fprintf( stderr, "macrunner-hb-user-callback: enter id=%lu len=%lu ret_ptr_slot=%p ret_len_slot=%p "
                 "frame=%p frame_sp=%p frame_pc=%p frame_lr=%p prev=%p kernel_stack=%p teb_stack=%p-%p "
                 "user_sp=%p\n",
                 (unsigned long)id, (unsigned long)len, ret_ptr, ret_len, frame,
                 (void *)(uintptr_t)frame->sp, (void *)(uintptr_t)frame->pc,
                 (void *)(uintptr_t)frame->lr, frame->prev_frame, ntdll_get_thread_data()->kernel_stack,
                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackLimit : NULL,
                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackBase : NULL, (void *)(uintptr_t)sp );

    if ((char *)ntdll_get_thread_data()->kernel_stack + min_kernel_stack > (char *)&frame)
    {
        TEB *teb = NtCurrentTeb();
        char *local = (char *)&frame;
        char *stack_limit = teb ? teb->Tib.StackLimit : NULL;
        char *stack_base = teb ? teb->Tib.StackBase : NULL;

        if (macrunner_hb_x64_loader_enabled() && stack_limit && stack_base &&
            stack_limit < stack_base && local >= stack_limit + min_kernel_stack &&
            local < stack_base)
        {
            TRACE( "MacRunner KeUserModeCallback accepting bridge stack id=%lu kernel_stack=%p local=%p "
                   "teb_stack=%p-%p frame=%p frame_sp=%p frame_pc=%p prev=%p\n",
                   (unsigned long)id, ntdll_get_thread_data()->kernel_stack, local,
                   stack_limit, stack_base, frame, (void *)(uintptr_t)frame->sp,
                   (void *)(uintptr_t)frame->pc, frame->prev_frame );
        }
        else
        {
            TRACE( "MacRunner KeUserModeCallback stack guard id=%lu kernel_stack=%p local=%p "
                   "teb_stack=%p-%p frame=%p frame_sp=%p frame_pc=%p prev=%p\n",
                   (unsigned long)id, ntdll_get_thread_data()->kernel_stack, local,
                   stack_limit, stack_base, frame, (void *)(uintptr_t)frame->sp,
                   (void *)(uintptr_t)frame->pc, frame->prev_frame );
            return STATUS_STACK_OVERFLOW;
        }
    }

    stack->args = stack->args_data;
    stack->len  = len;
    stack->id   = id;
    stack->lr   = frame->lr;
    stack->sp   = frame->sp;
    stack->pc   = frame->pc;
    memcpy( stack->args_data, args, len );
    return call_user_mode_callback( sp, ret_ptr, ret_len, pKiUserCallbackDispatcher, NtCurrentTeb() );
}


/***********************************************************************
 *           NtCallbackReturn  (NTDLL.@)
 */
NTSTATUS WINAPI NtCallbackReturn( void *ret_ptr, ULONG ret_len, NTSTATUS status )
{
    static int macrunner_callback_return_trace_count;
    struct syscall_frame *frame = get_syscall_frame();

    if (getenv( "MACRUNNER_HB_TRACE_USER_CALLBACK" ) && macrunner_callback_return_trace_count++ < 160)
    {
        fprintf( stderr, "macrunner-hb-user-callback: return ret_ptr=%p ret_len=%lu status=%08lx "
                 "frame=%p prev=%p syscall_cfa=%p frame_sp=%p frame_pc=%p frame_lr=%p "
                 "kernel_stack=%p teb_stack=%p-%p\n",
                 ret_ptr, (unsigned long)ret_len, (unsigned long)status, frame,
                 frame ? frame->prev_frame : NULL,
                 frame ? (void *)(uintptr_t)frame->syscall_cfa : NULL,
                 frame ? (void *)(uintptr_t)frame->sp : NULL,
                 frame ? (void *)(uintptr_t)frame->pc : NULL,
                 frame ? (void *)(uintptr_t)frame->lr : NULL,
                 ntdll_get_thread_data()->kernel_stack,
                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackLimit : NULL,
                 NtCurrentTeb() ? NtCurrentTeb()->Tib.StackBase : NULL );
    }

    if (!frame->prev_frame) return STATUS_NO_CALLBACK_ACTIVE;
    if (macrunner_hb_x64_loader_enabled() && ntdll_get_thread_data()->kernel_stack &&
        (char *)frame < (char *)ntdll_get_thread_data()->kernel_stack &&
        (char *)frame->prev_frame > (char *)ntdll_get_thread_data()->kernel_stack)
    {
        if (getenv( "MACRUNNER_HB_TRACE_USER_CALLBACK" ))
            fprintf( stderr, "macrunner-hb-user-callback: return-skip-bridge-frame frame=%p "
                     "prev=%p kernel_stack=%p syscall_cfa=%p\n",
                     frame, frame->prev_frame, ntdll_get_thread_data()->kernel_stack,
                     (void *)(uintptr_t)frame->syscall_cfa );
        ntdll_get_thread_data()->syscall_frame = frame->prev_frame;
    }
    user_mode_callback_return( ret_ptr, ret_len, status, NtCurrentTeb() );
}


/***********************************************************************
 *           handle_syscall_fault
 *
 * Handle a page fault happening during a system call.
 */
static BOOL handle_syscall_fault( ucontext_t *context, EXCEPTION_RECORD *rec )
{
    struct syscall_frame *frame = get_syscall_frame();
    DWORD i;

    if (!is_inside_syscall( SP_sig(context) )) return FALSE;

    TRACE( "code=%x flags=%x addr=%p pc=%p tid=%04x\n",
           rec->ExceptionCode, rec->ExceptionFlags, rec->ExceptionAddress,
           (void *)PC_sig(context), GetCurrentThreadId() );
    for (i = 0; i < rec->NumberParameters; i++)
        TRACE( " info[%d]=%016lx\n", i, rec->ExceptionInformation[i] );

    TRACE("  x0=%016lx  x1=%016lx  x2=%016lx  x3=%016lx\n",
          (DWORD64)REGn_sig(0, context), (DWORD64)REGn_sig(1, context),
          (DWORD64)REGn_sig(2, context), (DWORD64)REGn_sig(3, context) );
    TRACE("  x4=%016lx  x5=%016lx  x6=%016lx  x7=%016lx\n",
          (DWORD64)REGn_sig(4, context), (DWORD64)REGn_sig(5, context),
          (DWORD64)REGn_sig(6, context), (DWORD64)REGn_sig(7, context) );
    TRACE("  x8=%016lx  x9=%016lx x10=%016lx x11=%016lx\n",
          (DWORD64)REGn_sig(8, context), (DWORD64)REGn_sig(9, context),
          (DWORD64)REGn_sig(10, context), (DWORD64)REGn_sig(11, context) );
    TRACE(" x12=%016lx x13=%016lx x14=%016lx x15=%016lx\n",
          (DWORD64)REGn_sig(12, context), (DWORD64)REGn_sig(13, context),
          (DWORD64)REGn_sig(14, context), (DWORD64)REGn_sig(15, context) );
    TRACE(" x16=%016lx x17=%016lx x18=%016lx x19=%016lx\n",
          (DWORD64)REGn_sig(16, context), (DWORD64)REGn_sig(17, context),
          (DWORD64)REGn_sig(18, context), (DWORD64)REGn_sig(19, context) );
    TRACE(" x20=%016lx x21=%016lx x22=%016lx x23=%016lx\n",
          (DWORD64)REGn_sig(20, context), (DWORD64)REGn_sig(21, context),
          (DWORD64)REGn_sig(22, context), (DWORD64)REGn_sig(23, context) );
    TRACE(" x24=%016lx x25=%016lx x26=%016lx x27=%016lx\n",
          (DWORD64)REGn_sig(24, context), (DWORD64)REGn_sig(25, context),
          (DWORD64)REGn_sig(26, context), (DWORD64)REGn_sig(27, context) );
    TRACE(" x28=%016lx  fp=%016lx  lr=%016lx  sp=%016lx\n",
          (DWORD64)REGn_sig(28, context), (DWORD64)FP_sig(context),
          (DWORD64)LR_sig(context), (DWORD64)SP_sig(context) );

    if (ntdll_get_thread_data()->jmp_buf)
    {
        TRACE( "returning to handler\n" );
        REGn_sig(0, context) = (ULONG_PTR)ntdll_get_thread_data()->jmp_buf;
        REGn_sig(1, context) = 1;
        PC_sig(context)      = (ULONG_PTR)longjmp;
        ntdll_set_exception_jmp_buf( NULL );
    }
    else
    {
        TRACE( "returning to user mode ip=%p ret=%08x\n", (void *)frame->pc, rec->ExceptionCode );
        REGn_sig(0, context)  = rec->ExceptionCode;
        SP_sig(context)       = (ULONG_PTR)frame;
#if defined(__APPLE__)
        REGn_sig(10, context) = (ULONG_PTR)NtCurrentTeb();
        REGn_sig(16, context) = (ULONG_PTR)__wine_syscall_dispatcher_return;
        PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_thunk;
#else
        REGn_sig(18, context) = (ULONG_PTR)NtCurrentTeb();
        PC_sig(context)       = (ULONG_PTR)__wine_syscall_dispatcher_return;
#endif
    }
    return TRUE;
}


#if defined(__APPLE__)
static int get_memory_access_base_reg( DWORD insn )
{
    switch (insn & 0x3b000000)
    {
    case 0x38000000: /* load/store register: unscaled, pre/post-index, register offset */
    case 0x39000000: /* load/store register: unsigned immediate */
    case 0x28000000: /* load/store register pair: post-index */
    case 0x29000000: /* load/store register pair: offset/pre-index */
        return (insn >> 5) & 0x1f;
    default:
        break;
    }

    if ((insn & 0x3f000000) == 0x08000000) /* load/store exclusive */
        return (insn >> 5) & 0x1f;

    return -1;
}

static ULONG_PTR get_memory_access_offset( DWORD insn )
{
    if ((insn & 0x3b000000) == 0x39000000) /* load/store register: unsigned immediate */
        return ((insn >> 10) & 0xfff) << (insn >> 30);

    return 0;
}

static BOOL memory_access_base_copied_from_x18( DWORD *pc, int base_reg )
{
    unsigned int i;

    for (i = 1; i <= 32; i++)
    {
        DWORD *prev_pc = pc - i;
        DWORD prev;

        if (!macrunner_signal_read_memory( &prev, prev_pc, sizeof(prev) )) return FALSE;

        /* mov xN, x18 is encoded as orr xN, xzr, x18.  Compilers often
         * materialize the TEB base this way before a short branch and
         * later dereference [xN]. */
        if ((prev & 0xffe0ffe0) == 0xaa0003e0 &&
            ((prev >> 16) & 0x1f) == 18 && (prev & 0x1f) == base_reg)
            return TRUE;
    }

    return FALSE;
}

static BOOL memory_access_base_loaded_wow_teb_from_x18( DWORD *pc, int base_reg )
{
    unsigned int i, j;

    for (i = 1; i <= 8; i++)
    {
        DWORD *prev_pc = pc - i;
        DWORD prev;
        int offset_reg;

        if (!macrunner_signal_read_memory( &prev, prev_pc, sizeof(prev) )) return FALSE;

        /* add xBase, x18, xOffset */
        if ((prev & 0xffe0fc00) != 0x8b000000) continue;
        if ((prev & 0x1f) != base_reg || ((prev >> 5) & 0x1f) != 18) continue;
        offset_reg = (prev >> 16) & 0x1f;

        for (j = i + 1; j <= i + 16; j++)
        {
            DWORD *load_pc = pc - j;
            DWORD load;

            if (!macrunner_signal_read_memory( &load, load_pc, sizeof(load) )) return FALSE;

            /* ldrsw xOffset, [x18, #TEB.WowTebOffset] */
            if ((load & 0xffc00000) == 0xb9800000 &&
                (load & 0x1f) == offset_reg &&
                ((load >> 5) & 0x1f) == 18 &&
                get_memory_access_offset( load ) == offsetof( TEB, WowTebOffset ))
                return TRUE;
        }
    }

    return FALSE;
}

static void trace_apple_x18_heal( const char *kind, ucontext_t *context, ULONG_PTR fault_addr,
                                  int base_reg, ULONG_PTR base_value, ULONG_PTR mem_offset )
{
    /* Keep the signal handler async-signal-safe. Diagnostics here used to
     * call getenv()/fprintf(), which can recurse through libc while x18 is
     * already invalid and turn recovery into a CPU spin. */
    (void)kind;
    (void)context;
    (void)fault_addr;
    (void)base_reg;
    (void)base_value;
    (void)mem_offset;
}

static BOOL macrunner_hb_trace_native_faults_enabled(void)
{
    const char *val = getenv( "MACRUNNER_HB_TRACE_FAULTS" );
    return val && val[0] && val[0] != '0';
}

static IMAGE_NT_HEADERS *macrunner_hb_native_fault_nt_header( void *module )
{
    IMAGE_DOS_HEADER *dos = module;
    IMAGE_NT_HEADERS *nt;

    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000) return NULL;
    nt = (IMAGE_NT_HEADERS *)((BYTE *)module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return NULL;
    return nt;
}

static void *macrunner_hb_native_fault_module_from_pc( ULONG_PTR pc )
{
    uintptr_t p = pc & ~(uintptr_t)0xfff;
    unsigned int i;

    for (i = 0; i < 0x100000 && p; i++, p -= 0x1000)
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)p;
        IMAGE_NT_HEADERS *nt;

        if (!virtual_is_valid_code_address( dos, sizeof(*dos) ) ||
            dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            continue;
        nt = (IMAGE_NT_HEADERS *)(p + dos->e_lfanew);
        if (!virtual_is_valid_code_address( nt, sizeof(*nt) ) ||
            nt->Signature != IMAGE_NT_SIGNATURE)
            continue;
        return (void *)p;
    }
    return NULL;
}

static void macrunner_hb_native_fault_module_name( void *module, char *name, size_t name_size )
{
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_EXPORT_DIRECTORY *exports;
    IMAGE_NT_HEADERS *nt;

    if (!name || !name_size) return;
    strcpy( name, "unknown" );
    if (!module || !(nt = macrunner_hb_native_fault_nt_header( module ))) return;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size) return;
    exports = (IMAGE_EXPORT_DIRECTORY *)((BYTE *)module + dir->VirtualAddress);
    if (!exports->Name) return;
    snprintf( name, name_size, "%s", (const char *)module + exports->Name );
}

static void *macrunner_hb_readable_pe_module_from_pc( ULONG_PTR pc )
{
    uintptr_t p = pc & ~(uintptr_t)0xfff;
    unsigned int i;

    for (i = 0; i < 0x100000 && p; i++, p -= 0x1000)
    {
        IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)p;
        IMAGE_NT_HEADERS *nt;

        if (!virtual_check_buffer_for_read( dos, sizeof(*dos) ) ||
            dos->e_magic != IMAGE_DOS_SIGNATURE ||
            dos->e_lfanew <= 0 || dos->e_lfanew > 0x100000)
            continue;
        nt = (IMAGE_NT_HEADERS *)(p + dos->e_lfanew);
        if (!virtual_check_buffer_for_read( nt, sizeof(*nt) ) ||
            nt->Signature != IMAGE_NT_SIGNATURE)
            continue;
        return (void *)p;
    }
    return NULL;
}

static void macrunner_hb_trace_callback_target_module( const char *source, ULONG_PTR pc )
{
    unsigned char bytes[16] = {0};
    char hex[sizeof(bytes) * 2 + 1];
    void *module;
    char module_name[96];
    ULONG_PTR rva = 0;
    unsigned int i;

    if (!macrunner_hb_trace_callback_route_enabled()) return;
    module = macrunner_hb_readable_pe_module_from_pc( pc );
    macrunner_hb_native_fault_module_name( module, module_name, sizeof(module_name) );
    if (module) rva = pc - (ULONG_PTR)module;
    if (pc && virtual_check_buffer_for_read( (void *)pc, sizeof(bytes) ))
        memcpy( bytes, (void *)pc, sizeof(bytes) );
    for (i = 0; i < sizeof(bytes); i++) sprintf( hex + i * 2, "%02x", bytes[i] );
    hex[sizeof(hex) - 1] = 0;

    fprintf( stderr, "macrunner-hb-callback-target: source=%s pc=%p module=%s base=%p rva=0x%llx bytes=%s\n",
             source, (void *)pc, module_name, module, (unsigned long long)rva, hex );
}

static BOOL macrunner_hb_redirect_arm64x_hexpthk_sigill( ucontext_t *context )
{
    static const unsigned char thunk_prefix[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };
    unsigned char bytes[sizeof(thunk_prefix) + sizeof(LONG)];
    unsigned char pc_prefix[2];
    ULONG_PTR pc = PC_sig(context);
    ULONG_PTR candidates[5];
    unsigned int ci;
    ULONG_PTR thunk = 0, target;
    void *module;
    LONG rel;
    static int trace_candidates = -1;
    static int trace_count;

    if (trace_candidates < 0)
        trace_candidates = getenv( "MACRUNNER_HB_TRACE_HEXPTHK_CANDIDATE" ) ? 1 : 0;

    if (macrunner_hb_pc_is_x64_guest_code_module_no_lock( (void *)pc ))
    {
        if (macrunner_signal_read_memory( pc_prefix, (void *)pc, sizeof(pc_prefix) ) &&
            pc_prefix[0] == 0xff && pc_prefix[1] == 0x25)
        {
            if (macrunner_hb_trace_callback_route_enabled())
                fprintf( stderr, "macrunner-hb-arm64x-hexpthk-skip-import-jmp: pc=%p x4=%p x16=%p lr=%p\n",
                         (void *)pc, (void *)(ULONG_PTR)REGn_sig(4, context),
                         (void *)(ULONG_PTR)REGn_sig(16, context),
                         (void *)(ULONG_PTR)LR_sig(context) );
            return FALSE;
        }
    }

    /* The faulting ARM64X x64 entry-thunk address can arrive tagged with the CodeMap
     * type in the low 2 bits (entry_thunk | type) and/or in a register other than x16
     * (a native `blr x8` to an imported thunk leaves the target in another reg, and the
     * fault PC inside the thunk).  Probe x16 and x4 with the tag stripped, plus the
     * 16-aligned address the fault PC lies within; accept the first whose bytes are the
     * fast-forward entry-thunk prologue and that the fault PC falls within. */
    candidates[0] = REGn_sig(16, context) & ~(ULONG_PTR)3;
    candidates[1] = REGn_sig(8, context) & ~(ULONG_PTR)3;
    candidates[2] = REGn_sig(17, context) & ~(ULONG_PTR)3;
    candidates[3] = REGn_sig(4, context) & ~(ULONG_PTR)3;
    candidates[4] = pc & ~(ULONG_PTR)15;
    for (ci = 0; ci < ARRAY_SIZE(candidates); ci++)
    {
        ULONG_PTR c = candidates[ci];
        BOOL pe_code;

        if (!c || pc < c || pc >= c + sizeof(bytes)) continue;
        pe_code = macrunner_hb_pc_is_pe_code_module_no_lock( (void *)c );
        if (!pe_code)
        {
            if (trace_candidates && trace_count++ < 2048)
                fprintf( stderr, "macrunner-hb-hexpthk-candidate: pc=%p ci=%u c=%p pe_code=0 "
                         "x4=%p x8=%p x16=%p x17=%p lr=%p sp=%p\n",
                         (void *)pc, ci, (void *)c,
                         (void *)(ULONG_PTR)REGn_sig(4, context),
                         (void *)(ULONG_PTR)REGn_sig(8, context),
                         (void *)(ULONG_PTR)REGn_sig(16, context),
                         (void *)(ULONG_PTR)REGn_sig(17, context),
                         (void *)(ULONG_PTR)LR_sig(context),
                         (void *)(ULONG_PTR)SP_sig(context) );
            continue;
        }
        if (!macrunner_signal_read_memory( bytes, (void *)c, sizeof(bytes) )) continue;
        if (trace_candidates && trace_count++ < 2048)
            fprintf( stderr, "macrunner-hb-hexpthk-candidate: pc=%p ci=%u c=%p pe_code=1 "
                     "bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x "
                     "match=%d x4=%p x8=%p x16=%p x17=%p lr=%p sp=%p\n",
                     (void *)pc, ci, (void *)c,
                     bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
                     bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
                     !memcmp( bytes, thunk_prefix, sizeof(thunk_prefix) ),
                     (void *)(ULONG_PTR)REGn_sig(4, context),
                     (void *)(ULONG_PTR)REGn_sig(8, context),
                     (void *)(ULONG_PTR)REGn_sig(16, context),
                     (void *)(ULONG_PTR)REGn_sig(17, context),
                     (void *)(ULONG_PTR)LR_sig(context),
                     (void *)(ULONG_PTR)SP_sig(context) );
        if (memcmp( bytes, thunk_prefix, sizeof(thunk_prefix) )) continue;
        thunk = c;
        break;
    }
    if (!thunk) return FALSE;

    module = macrunner_hb_pe_module_from_pc_no_lock( (void *)thunk );
    if (!module || macrunner_hb_pe_module_from_pc_no_lock( (void *)pc ) != module) return FALSE;
    memcpy( &rel, bytes + sizeof(thunk_prefix), sizeof(rel) );
    target = thunk + sizeof(bytes) + rel;
    if ((target & 3) || macrunner_hb_pe_module_from_pc_no_lock( (void *)target ) != module) return FALSE;
    if (!macrunner_hb_pc_is_pe_code_module_no_lock( (void *)target )) return FALSE;

    if (macrunner_hb_trace_callback_route_enabled())
        fprintf( stderr, "macrunner-hb-arm64x-hexpthk-redirect: pc=%p thunk=%p target=%p\n",
                 (void *)pc, (void *)thunk, (void *)target );
    REGn_sig(16, context) = target;
    REGn_sig(18, context) = (ULONG_PTR)NtCurrentTeb();
    PC_sig(context) = target;
    return TRUE;
}

static BOOL macrunner_hb_native_fault_read_u64( ULONG_PTR addr, ULONG_PTR *out )
{
    if (!addr || !out) return FALSE;
    if (!virtual_check_buffer_for_read( (void *)addr, sizeof(*out) )) return FALSE;
    *out = *(ULONG_PTR *)addr;
    return TRUE;
}

static void macrunner_hb_native_fault_dump_qwords( const char *label, ULONG_PTR addr )
{
    ULONG_PTR values[4] = {0};
    unsigned int valid = 0, i;

    if (!macrunner_hb_trace_native_faults_enabled() || !addr) return;
    for (i = 0; i < ARRAY_SIZE(values); i++)
    {
        if (macrunner_hb_native_fault_read_u64( addr + i * sizeof(ULONG_PTR), &values[i] ))
            valid |= 1u << i;
    }
    fprintf( stderr,
             "macrunner-native-fault-mem: %s addr=0x%llx valid=0x%x q=[0x%llx,0x%llx,0x%llx,0x%llx]\n",
             label, (unsigned long long)addr, valid,
             (unsigned long long)values[0], (unsigned long long)values[1],
             (unsigned long long)values[2], (unsigned long long)values[3] );
}

static void macrunner_hb_native_fault_dump_vm_region( const char *label, ULONG_PTR addr )
{
#ifdef __APPLE__
    mach_vm_address_t region = (mach_vm_address_t)addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr;

    if (!macrunner_hb_trace_native_faults_enabled() || !addr) return;
    kr = mach_vm_region( mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object );
    if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
    fprintf( stderr,
             "macrunner-native-fault-vm: %s addr=0x%llx kr=%d region=0x%llx size=0x%llx prot=0x%x max=0x%x\n",
             label, (unsigned long long)addr, kr, (unsigned long long)region,
             (unsigned long long)size, kr == KERN_SUCCESS ? info.protection : 0,
             kr == KERN_SUCCESS ? info.max_protection : 0 );
#endif
}

static void macrunner_hb_trace_native_fault( const char *kind, ucontext_t *context,
                                             const EXCEPTION_RECORD *rec, DWORD64 esr )
{
    ULONG_PTR pc = PC_sig(context);
    ULONG_PTR fault_addr = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
    ULONG_PTR insn = 0;
    void *module = NULL;
    char module_name[96];
    const char *access = "unknown";
    BOOL have_insn = FALSE;
    TEB *teb;
    struct ntdll_thread_data *thread_data;

    if (!macrunner_hb_trace_native_faults_enabled()) return;

    if (rec->NumberParameters >= 1)
    {
        if (rec->ExceptionInformation[0] == EXCEPTION_WRITE_FAULT) access = "write";
        else if (rec->ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT) access = "execute";
        else if (rec->ExceptionInformation[0] == EXCEPTION_READ_FAULT) access = "read";
    }
    if (virtual_is_valid_code_address( (void *)pc, sizeof(DWORD) ))
    {
        insn = *(DWORD *)pc;
        have_insn = TRUE;
    }
    module = macrunner_hb_native_fault_module_from_pc( pc );
    macrunner_hb_native_fault_module_name( module, module_name, sizeof(module_name) );
    teb = NtCurrentTeb();
    thread_data = teb ? (struct ntdll_thread_data *)&teb->GdiTebBatch : NULL;

    fprintf( stderr,
             "macrunner-native-fault: kind=%s code=0x%08lx access=%s fault=0x%llx "
             "pid=%d module=%s base=%p rva=0x%llx pc=0x%llx sp=0x%llx lr=0x%llx esr=0x%llx pstate=0x%llx "
             "x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx "
             "x4=0x%llx x5=0x%llx x6=0x%llx x7=0x%llx "
             "x9=0x%llx x18=0x%llx x19=0x%llx x20=0x%llx x21=0x%llx x22=0x%llx x23=0x%llx x24=0x%llx x25=0x%llx "
             "x26=0x%llx x27=0x%llx x28=0x%llx teb=%p teb_stack=%p-%p dealloc=%p kernel=%p "
             "insn_valid=%u insn=0x%08llx\n",
             kind, rec->ExceptionCode, access, (unsigned long long)fault_addr,
             getpid(), module_name, module, module ? (unsigned long long)(pc - (ULONG_PTR)module) : 0,
             (unsigned long long)pc, (unsigned long long)SP_sig(context),
             (unsigned long long)REGn_sig(30, context), (unsigned long long)esr,
             (unsigned long long)PSTATE_sig(context),
             (unsigned long long)REGn_sig(0, context), (unsigned long long)REGn_sig(1, context),
             (unsigned long long)REGn_sig(2, context), (unsigned long long)REGn_sig(3, context),
             (unsigned long long)REGn_sig(4, context), (unsigned long long)REGn_sig(5, context),
             (unsigned long long)REGn_sig(6, context), (unsigned long long)REGn_sig(7, context),
             (unsigned long long)REGn_sig(9, context), (unsigned long long)REGn_sig(18, context),
             (unsigned long long)REGn_sig(19, context), (unsigned long long)REGn_sig(20, context),
             (unsigned long long)REGn_sig(21, context), (unsigned long long)REGn_sig(22, context),
             (unsigned long long)REGn_sig(23, context), (unsigned long long)REGn_sig(24, context),
             (unsigned long long)REGn_sig(25, context),
             (unsigned long long)REGn_sig(26, context), (unsigned long long)REGn_sig(27, context),
             (unsigned long long)REGn_sig(28, context), teb,
             teb ? teb->Tib.StackLimit : NULL, teb ? teb->Tib.StackBase : NULL,
             teb ? teb->DeallocationStack : NULL, thread_data ? thread_data->kernel_stack : NULL,
             have_insn, (unsigned long long)insn );

    macrunner_hb_native_fault_dump_qwords( "x19", REGn_sig(19, context) );
    macrunner_hb_native_fault_dump_qwords( "x20", REGn_sig(20, context) );
    macrunner_hb_native_fault_dump_qwords( "x24", REGn_sig(24, context) );
    macrunner_hb_native_fault_dump_vm_region( "x24", REGn_sig(24, context) );
    macrunner_hb_native_fault_dump_qwords( "x26", REGn_sig(26, context) );
    macrunner_hb_native_fault_dump_vm_region( "x26", REGn_sig(26, context) );
    macrunner_hb_native_fault_dump_qwords( "x28+0x890", REGn_sig(28, context) + 0x890 );
    {
        ULONG_PTR fls_global = REGn_sig(28, context) + 0x890;
        ULONG_PTR fls_cb_chunk = 0, fls_data_chunk = 0;
        ULONG_PTR fls_cb_current = 0, fls_data_current = 0;
        ULONG_PTR fls_index = REGn_sig(21, context);
        BOOL have_cb_chunk = macrunner_hb_native_fault_read_u64( fls_global, &fls_cb_chunk );
        BOOL have_data_chunk = macrunner_hb_native_fault_read_u64( REGn_sig(27, context), &fls_data_chunk );

        if (have_cb_chunk)
        {
            macrunner_hb_native_fault_dump_qwords( "fls_cb_chunk0", fls_cb_chunk );
            macrunner_hb_native_fault_dump_qwords( "fls_cb_chunk0+0x20", fls_cb_chunk + 0x20 );
            macrunner_hb_native_fault_read_u64( fls_cb_chunk + fls_index, &fls_cb_current );
        }
        if (have_data_chunk)
        {
            macrunner_hb_native_fault_dump_qwords( "fls_data_chunk0", fls_data_chunk );
            macrunner_hb_native_fault_dump_qwords( "fls_data_chunk0+0x20", fls_data_chunk + 0x20 );
            macrunner_hb_native_fault_read_u64( fls_data_chunk + fls_index, &fls_data_current );
        }
        fprintf( stderr,
                 "macrunner-native-fault-fls: global=0x%llx cb_chunk=0x%llx "
                 "data_root=0x%llx data_chunk=0x%llx index=0x%llx "
                 "cb_current=0x%llx data_current=0x%llx\n",
                 (unsigned long long)fls_global, (unsigned long long)fls_cb_chunk,
                 (unsigned long long)REGn_sig(27, context), (unsigned long long)fls_data_chunk,
                 (unsigned long long)fls_index, (unsigned long long)fls_cb_current,
                 (unsigned long long)fls_data_current );
    }
    macrunner_hb_native_fault_dump_qwords( "x27", REGn_sig(27, context) );
    macrunner_hb_native_fault_dump_qwords( "x9", REGn_sig(9, context) );
    macrunner_hb_native_fault_dump_qwords( "fault", fault_addr );
}

static void macrunner_hb_trace_low_stack_symbol( const char *label, ULONG_PTR addr )
{
    Dl_info info;

    if (!addr) return;
    if (dladdr( (void *)addr, &info ) && info.dli_fname)
        ERR( "macrunner-hb-low-stack-symbol: %s addr=%p image=%s image_base=%p "
             "symbol=%s symbol_addr=%p offset=0x%llx\n",
             label, (void *)addr, info.dli_fname, info.dli_fbase,
             info.dli_sname ? info.dli_sname : "unknown", info.dli_saddr,
             info.dli_saddr ? (unsigned long long)(addr - (ULONG_PTR)info.dli_saddr) : 0 );
}

static void macrunner_hb_trace_low_stack_vm_region( const char *label, ULONG_PTR addr )
{
    mach_vm_address_t region = (mach_vm_address_t)addr;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr;

    if (!addr) return;
    kr = mach_vm_region( mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object );
    if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
    ERR( "macrunner-hb-low-stack-vm: %s addr=%p kr=%d region=%p size=0x%llx prot=0x%x max=0x%x\n",
         label, (void *)addr, kr, (void *)(ULONG_PTR)region,
         (unsigned long long)size, kr == KERN_SUCCESS ? info.protection : 0,
         kr == KERN_SUCCESS ? info.max_protection : 0 );
}

static BOOL macrunner_hb_is_low_stack_access_fault( ucontext_t *context, const EXCEPTION_RECORD *rec )
{
    TEB *teb = NtCurrentTeb();
    char *sp = (char *)SP_sig(context);
    char *fault;

    if (!teb || !teb->DeallocationStack || !teb->Tib.StackLimit) return FALSE;
    if (rec->NumberParameters < 2) return FALSE;
    if (macrunner_hb_get_callback_exception_stack( context, NULL )) return FALSE;
    if (sp < (char *)teb->DeallocationStack || sp >= (char *)teb->Tib.StackLimit) return FALSE;

    fault = (char *)rec->ExceptionInformation[1];
    return (fault >= (char *)teb->DeallocationStack && fault < (char *)teb->Tib.StackLimit);
}

static void *macrunner_hb_virtual_fault_stack( ucontext_t *context, const EXCEPTION_RECORD *rec )
{
    TEB *teb = NtCurrentTeb();
    char *sp = (char *)SP_sig(context);
    char *fault;

    if (!teb || !teb->DeallocationStack || !teb->Tib.StackBase) return sp;
    if (rec->NumberParameters < 2) return sp;
    if (rec->ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT) return sp;

    fault = (char *)rec->ExceptionInformation[1];
    if (fault >= (char *)teb->DeallocationStack && fault < (char *)teb->Tib.StackBase &&
        (sp < (char *)teb->DeallocationStack || sp >= (char *)teb->Tib.StackBase))
        return fault;

    return sp;
}

static BOOL macrunner_hb_trace_low_stack_fault_enabled(void)
{
    static int count;

    return count++ < 32;
}

#if defined(__APPLE__)
static void macrunner_hb_trace_signal_exception_delivery( const char *kind, ucontext_t *context,
                                                          const EXCEPTION_RECORD *rec,
                                                          BOOL low_stack_fault,
                                                          BOOL stack_overflow_fault,
                                                          void *virtual_stack )
{
    static int report_count;
    TEB *teb = NtCurrentTeb();
    char *sp = (char *)SP_sig(context);
    BOOL near_stack = FALSE;

    if (teb && teb->DeallocationStack && teb->Tib.StackLimit && teb->Tib.StackBase)
        near_stack = sp >= (char *)teb->DeallocationStack &&
                     sp < (char *)teb->Tib.StackLimit + 0x20000;
    if (!rec || (rec->ExceptionCode != STATUS_STACK_OVERFLOW &&
                 !low_stack_fault && !stack_overflow_fault && !near_stack))
        return;
    if (report_count++ >= 32) return;

    macrunner_signal_writef( "macrunner-hb-signal-to-exception: kind=%s pid=%d "
                             "code=%#lx flags=%#lx pc=%p lr=%p sp=%p fault=%p "
                             "info0=0x%llx params=%lu low_stack=%u stack_overflow=%u "
                             "vstack=%p teb_stack=%p-%p dealloc=%p\n",
                             kind, getpid(), rec->ExceptionCode, rec->ExceptionFlags,
                             (void *)(ULONG_PTR)PC_sig(context),
                             (void *)(ULONG_PTR)LR_sig(context),
                             (void *)(ULONG_PTR)SP_sig(context),
                             rec->NumberParameters > 1 ?
                                 (void *)(ULONG_PTR)rec->ExceptionInformation[1] : NULL,
                             (unsigned long long)(rec->NumberParameters > 0 ?
                                 (ULONG_PTR)rec->ExceptionInformation[0] : 0),
                             rec->NumberParameters, (unsigned int)low_stack_fault,
                             (unsigned int)stack_overflow_fault,
                             virtual_stack,
                             teb ? teb->Tib.StackLimit : NULL,
                             teb ? teb->Tib.StackBase : NULL,
                             teb ? teb->DeallocationStack : NULL );
}
#endif

static BOOL emulate_apple_x18_teb_access( ucontext_t *context, TEB *teb, DWORD insn, ULONG_PTR fault_addr )
{
    ULONG_PTR offset, value = 0;
    unsigned int size, rt;
    void *addr;
    BOOL load;
    struct ntdll_thread_data *thread_data;

    if ((insn & 0x3b000000) != 0x39000000) return FALSE; /* unsigned immediate GPR load/store */
    if (((insn >> 5) & 0x1f) != 18) return FALSE;

    offset = get_memory_access_offset( insn );
    if (offset != fault_addr || offset >= 0x4000) return FALSE;

    size = 1u << (insn >> 30);
    if (size > sizeof(ULONG_PTR) || offset + size > 0x4000) return FALSE;

    rt = insn & 0x1f;
    load = (insn >> 22) & 1;
    addr = (char *)teb + offset;

    if (load)
    {
        switch (size)
        {
        case 1: value = *(BYTE *)addr; break;
        case 2: value = *(WORD *)addr; break;
        case 4: value = *(DWORD *)addr; break;
        case 8: value = *(ULONG_PTR *)addr; break;
        default: return FALSE;
        }
        if (rt != 31) REGn_sig(rt, context) = value;
    }
    else
    {
        if (rt != 31) value = REGn_sig(rt, context);
        switch (size)
        {
        case 1: *(BYTE *)addr = value; break;
        case 2: *(WORD *)addr = value; break;
        case 4: *(DWORD *)addr = value; break;
        case 8: *(ULONG_PTR *)addr = value; break;
        default: return FALSE;
        }
    }

    /* The memory operation has been completed, but we still need to return
     * through user-mode code that rehydrates x18.  If we simply advance PC in
     * the signal context, xnu clears x18 again on sigreturn and the next
    * non-faulting x18-derived instruction can materialize a bogus pointer. */
    thread_data = (struct ntdll_thread_data *)&teb->GdiTebBatch;
    if (macrunner_hb_x64_loader_enabled() && apple_x18_pc_is_resume_thunk( PC_sig(context) ))
    {
        REGn_sig(10, context) = (ULONG_PTR)teb;
        REGn_sig(18, context) = (ULONG_PTR)teb;
        PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
        return TRUE;
    }
    thread_data->apple_x18_save_x10 = REGn_sig(10, context);
    thread_data->apple_x18_save_x16 = REGn_sig(16, context);
    thread_data->apple_x18_save_pc  = PC_sig(context) + 4;
    trace_apple_x18_heal( load ? "direct-emulate-load" : "direct-emulate-store",
                          context, fault_addr, 18, 0, offset );
    REGn_sig(10, context) = (ULONG_PTR)teb;
    REGn_sig(18, context) = (ULONG_PTR)teb;
    PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
    return TRUE;
}

#else
static BOOL macrunner_hb_trace_low_stack_fault_enabled(void)
{
    return TRUE;
}

#endif


/**********************************************************************
 *		segv_handler
 *
 * Handler for SIGSEGV.
 */
static void segv_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    DWORD64 esr = get_fault_esr( context );
#if defined(__APPLE__)
    BOOL low_stack_fault;
    void *virtual_stack;
    TEB *teb = NtCurrentTeb();
#endif

    rec.NumberParameters = 2;
    if ((esr & 0xf0000000) == 0x80000000) rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
    else if (esr & 0x40) rec.ExceptionInformation[0] = EXCEPTION_WRITE_FAULT;
    else rec.ExceptionInformation[0] = EXCEPTION_READ_FAULT;
    rec.ExceptionInformation[1] = (ULONG_PTR)siginfo->si_addr;
#if defined(__APPLE__)
    /* recover a gated direct guest-mem copy fault BEFORE any other handling/locking */
    macrunner_hb_dmem_fault_recover( (unsigned long long)(ULONG_PTR)siginfo->si_addr );
    low_stack_fault = macrunner_hb_is_low_stack_access_fault( context, &rec );
    virtual_stack = macrunner_hb_virtual_fault_stack( context, &rec );
#endif

    if (macrunner_hb_trace_callback_route_enabled())
        ERR( "macrunner-hb-signal-entry: kind=segv pid=%d pc=%p fault=%p esr=0x%llx "
             "x4=%p x16=%p x24=%p x26=%p\n",
             getpid(), (void *)(ULONG_PTR)PC_sig(context),
             (void *)(ULONG_PTR)rec.ExceptionInformation[1], (unsigned long long)esr,
             (void *)(ULONG_PTR)REGn_sig(4, context), (void *)(ULONG_PTR)REGn_sig(16, context),
             (void *)(ULONG_PTR)REGn_sig(24, context), (void *)(ULONG_PTR)REGn_sig(26, context) );

#if defined(__APPLE__)
    /* MacRunner fault-time diagnostic (env-gated): the x64 JIT reported a clean MEMORY_FAULT
     * reading UnityPlayer .data (host ~0x87efe45xxxx) that the load-time probe showed RW-committed.
     * Dump the ACTUAL fault-time Mach region+protection for faults landing in the x64-guest high
     * window, to pin whether the page was reprotected (prot=0) / decommitted (region gap) at runtime. */
    if (getenv( "MACRUNNER_DIAG_FAULTVM" ))
    {
        ULONG_PTR fa = rec.ExceptionInformation[1];
        if (fa >= 0x87ef0000000ULL && fa < 0x87f00000000ULL)
        {
            static int faultvm_n;
            if (faultvm_n++ < 24)
            {
                mach_vm_address_t ra = (mach_vm_address_t)fa;
                mach_vm_size_t rs = 0;
                vm_region_basic_info_data_64_t info;
                mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                kern_return_t kr = mach_vm_region( mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                                   (vm_region_info_t)&info, &cnt, &obj );
                if (obj != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), obj );
                fprintf( stderr, "macrunner-diag-faultvm: pc=%p lr=%p fault=%p access=%lu kr=%d "
                         "region=%p end=%p size=0x%llx prot=0x%x max=0x%x\n",
                         (void *)(ULONG_PTR)PC_sig(context), (void *)(ULONG_PTR)LR_sig(context),
                         (void *)fa, (unsigned long)rec.ExceptionInformation[0], kr,
                         (void *)(uintptr_t)ra, (void *)(uintptr_t)(ra + rs),
                         (unsigned long long)rs, kr == KERN_SUCCESS ? info.protection : 0,
                         kr == KERN_SUCCESS ? info.max_protection : 0 );
            }
        }
    }
    /* MacRunner HK memcpy fault: reconcile + guest stack walk for the Unity/Mono
     * CRT stream-buffer range 0x320fxxxxx. */
    {
        ULONG_PTR fa = rec.ExceptionInformation[1];
        if (fa >= 0x320f00000ULL && fa < 0x321100000ULL)
        {
            static int hk_memcpy_n;
            if (hk_memcpy_n++ < 32)
                macrunner_hb_trace_hk_memcpy_fault( "segv", PC_sig(context), fa );
        }
    }
#endif
    /* MacRunner diag: a NULL-target execute fault (guest called a NULL function
     * pointer) is handled by hb_jit_runtime_handle_signal_fault below (it raises
     * the synthetic guest c0000005, so route_x64_callback_fault never sees it).
     * Pin the guest call site + TEB state from the registered x64 ctx FIRST. */
    if (rec.ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT &&
        (rec.ExceptionInformation[1] < 0x1000 || PC_sig(context) == 0))
    {
        static int nullcall_segv_n;
        const char *nv = getenv( "MACRUNNER_HB_TRACE_NULLCALL" );
        if (nv && nv[0] && nv[0] != '0' && nullcall_segv_n++ < 16)
            macrunner_hb_trace_nullcall_site( "segv-exec0", PC_sig(context),
                                              rec.ExceptionInformation[1] );
    }

    if (hb_jit_runtime_handle_signal_fault( PC_sig(context), rec.ExceptionInformation[1], signal ) ||
        hb_jit_runtime_handle_signal_fault( LR_sig(context), rec.ExceptionInformation[1], signal ))
        return;

    /* MacRunner Phase F: on macOS, executing x86_64 guest bytes on the
     * ARM64 CPU is not guaranteed to surface as EXCEPTION_EXECUTE_FAULT.
     * Some byte patterns decode as valid ARM64/SVE memory operations and
     * arrive as EXC_BAD_ACCESS while PC is still in the x64 PE .text.  Route
     * those guest-code traps through HyperBridge before normal ARM64 PE
     * fault handling, but keep the guard strictly scoped to known x64 guest
     * code pages. */
    if (macrunner_hb_route_x64_callback_fault( context, rec.ExceptionInformation[1], "segv-guest" )) return;

#if defined(__APPLE__)
    /* arm64 macOS xnu does not preserve x18 across thread context
     * switches AND wipes x18 on sigreturn even if we restore it in
     * sigcontext: when the scheduler preempts a Wine PE thread (or
     * delivers any signal), x18 — which Microsoft ARM64 ABI uses as
     * the TEB pointer — comes back as zero. The next TEB-relative load
     * in PE code then faults on a small immediate, e.g.
     * `ldr x*, [x18, #0x60]`. We MUST NOT dispatch this as a Windows
     * exception: doing so logs through the PE-side debug subsystem,
     * which itself derefs x18, recursively faults, and leaves
     * vectored_handlers_section orphaned — deadlocking the process.
     *
     * Self-heal patterns:
     *  - direct TEB deref: retry through __wine_pe_x18_resume_thunk so
     *    x18 is restored in user mode after sigreturn;
     *  - indirect TEB pointer: x18 was zero for an earlier non-faulting
     *    `add xN, x18, #off`, producing a small bogus pointer. Fix the
     *    current base register to TEB+off and retry this instruction.
     *    The faulting instruction may add its own immediate offset, e.g.
     *    `str xzr, [xN, #0x1480]`, so validate base+mem_offset rather
     *    than only matching the final fault address. */
    if (rec.ExceptionInformation[0] != EXCEPTION_EXECUTE_FAULT)
    {
        ULONG_PTR fault_addr = (ULONG_PTR)siginfo->si_addr;
        ULONG_PTR pc = PC_sig(context);
        DWORD insn;
        int base_reg;
        ULONG_PTR mem_offset;

        /* Only the Apple x18/TEB-loss patterns fault on low TEB-relative
         * addresses.  Avoid querying the virtual map before this cheap scope
         * check: SIGSEGV can arrive while virtual.c already owns virtual_mutex
         * (for example during delete_view()), and virtual_is_valid_code_address()
         * would deadlock inside the signal handler.  The CPU has already
         * fetched the current instruction for non-execute faults, so reading
         * the instruction word directly is the signal-safe path here. */
        if (!teb || fault_addr >= 0x4000 || pc < 0x10000) goto skip_apple_x18_heal;
        insn = *(DWORD *)pc;
        base_reg = get_memory_access_base_reg( insn );
        mem_offset = get_memory_access_offset( insn );

        /* TEB-relative offsets used by PE-side code fall in 0..teb_size
         * (~14 KB) and 0x3000..0x3a00 (TLS/GdiTebBatch).  Decode the
         * actual faulting memory instruction so unrelated low/null faults
         * are still dispatched as real Windows exceptions. */
        if (REGn_sig(18, context) == 0 && teb && fault_addr < 0x4000 && base_reg == 18)
        {
            struct ntdll_thread_data *thread_data =
                (struct ntdll_thread_data *)&teb->GdiTebBatch;
            if (emulate_apple_x18_teb_access( context, teb, insn, fault_addr )) return;
            if (macrunner_hb_x64_loader_enabled() && apple_x18_pc_is_resume_thunk( PC_sig(context) ))
            {
                REGn_sig(10, context) = (ULONG_PTR)teb;
                REGn_sig(18, context) = (ULONG_PTR)teb;
                PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
                return;
            }
            /* Stash the registers we are about to clobber in the
             * trampoline; the asm thunk reloads them from these slots
             * before branching to the retry PC. */
            thread_data->apple_x18_save_x10 = REGn_sig(10, context);
            thread_data->apple_x18_save_x16 = REGn_sig(16, context);
            thread_data->apple_x18_save_pc  = PC_sig(context);
            trace_apple_x18_heal( "direct", context, fault_addr, base_reg,
                                  REGn_sig(base_reg, context), mem_offset );
            REGn_sig(10, context) = (ULONG_PTR)teb;
            REGn_sig(18, context) = (ULONG_PTR)teb;
            PC_sig(context)       = (ULONG_PTR)__wine_pe_x18_resume_thunk;
            return;
        }
        if (REGn_sig(18, context) == 0 && teb && fault_addr < 0x4000 &&
            base_reg >= 0 && base_reg < 31 && REGn_sig(base_reg, context) < 0x4000 &&
            REGn_sig(base_reg, context) + mem_offset == fault_addr &&
            memory_access_base_copied_from_x18( (DWORD *)PC_sig(context), base_reg ))
        {
            ULONG_PTR base_addr = REGn_sig(base_reg, context);
            trace_apple_x18_heal( "copied", context, fault_addr, base_reg, base_addr, mem_offset );
            REGn_sig(base_reg, context) = (ULONG_PTR)teb + base_addr;
            setup_x18_resume_from_sigcontext( context );
            return;
        }
        if (REGn_sig(18, context) == 0 && teb && get_wow_teb( teb ) &&
            fault_addr < 0x400 && base_reg >= 0 && base_reg < 31 &&
            !REGn_sig(base_reg, context) && mem_offset == fault_addr &&
            memory_access_base_loaded_wow_teb_from_x18( (DWORD *)PC_sig(context), base_reg ))
        {
            trace_apple_x18_heal( "wow-teb-derived", context, fault_addr,
                                  base_reg, REGn_sig(base_reg, context), mem_offset );
            REGn_sig(base_reg, context) = (ULONG_PTR)get_wow_teb( teb );
            setup_x18_resume_from_sigcontext( context );
            return;
        }
        if (teb && fault_addr < 0x4000 && base_reg >= 0 && base_reg < 31 &&
            REGn_sig(base_reg, context) >= 0x1000 && REGn_sig(base_reg, context) < 0x4000 &&
            (REGn_sig(base_reg, context) + mem_offset == fault_addr ||
             (fault_addr >= REGn_sig(base_reg, context) &&
              fault_addr - REGn_sig(base_reg, context) < 0x1000)))
        {
            ULONG_PTR base_addr = REGn_sig(base_reg, context);
            trace_apple_x18_heal( "derived", context, fault_addr, base_reg, base_addr, mem_offset );
            REGn_sig(base_reg, context) = (ULONG_PTR)teb + base_addr;
            setup_x18_resume_from_sigcontext( context );
            return;
        }
        if (REGn_sig(18, context) == 0 && teb && fault_addr < 0x4000 &&
            base_reg >= 0 && base_reg < 31 && REGn_sig(base_reg, context) < 0x400 &&
            mem_offset >= 0x1400 && mem_offset < 0x1800 &&
            REGn_sig(base_reg, context) + mem_offset == fault_addr)
        {
            /* TlsAlloc/TlsSetValue compile as:
             *   add xN, x18, index, uxtw #3
             *   str/ldr ..., [xN, #0x1480]
             * If x18 was already cleared before the non-faulting add,
             * xN only contains index*8.  Rebase that register to the
             * real TEB and retry the faulting access. */
            ULONG_PTR base_addr = REGn_sig(base_reg, context);
            trace_apple_x18_heal( "tls-indexed", context, fault_addr, base_reg, base_addr, mem_offset );
            REGn_sig(base_reg, context) = (ULONG_PTR)teb + base_addr;
            setup_x18_resume_from_sigcontext( context );
            return;
        }
    }
skip_apple_x18_heal:
#endif
    if (rec.ExceptionInformation[0] == EXCEPTION_EXECUTE_FAULT &&
        macrunner_hb_route_x64_callback_fault( context, rec.ExceptionInformation[1], "segv-exec" ))
        return;
    if (macrunner_hb_wow64_i386_execute_fault( context, &rec ))
    {
        if (macrunner_hb_trace_callback_route_enabled())
            ERR( "macrunner-hb-wow64-i386-exec-route: pid=%d pc=%p sp=%p fault=%p lr=%p\n",
                 getpid(), (void *)(ULONG_PTR)PC_sig(context),
                 (void *)(ULONG_PTR)SP_sig(context),
                 (void *)(ULONG_PTR)rec.ExceptionInformation[1],
                 (void *)(ULONG_PTR)LR_sig(context) );
        setup_exception( context, &rec );
        return;
    }
    if (!virtual_handle_fault( &rec,
#if defined(__APPLE__)
                               virtual_stack
#else
                               (void *)SP_sig(context)
#endif
         )) return;
#if defined(__APPLE__)
    if (low_stack_fault)
    {
        if (macrunner_hb_trace_low_stack_fault_enabled())
            macrunner_signal_writef( "macrunner-hb-low-stack-as-overflow: pid=%d "
                                     "pc=%p sp=%p fault=%p info0=%Ix "
                                     "teb_stack=%p-%p dealloc=%p\n",
                                     getpid(), (void *)(ULONG_PTR)PC_sig(context),
                                     (void *)(ULONG_PTR)SP_sig(context),
                                     (void *)(ULONG_PTR)rec.ExceptionInformation[1],
                                     (ULONG_PTR)rec.ExceptionInformation[0],
                                     teb ? teb->Tib.StackLimit : NULL,
                                     teb ? teb->Tib.StackBase : NULL,
                                     teb ? teb->DeallocationStack : NULL );
        rec.ExceptionCode = STATUS_STACK_OVERFLOW;
        rec.NumberParameters = 0;
    }
    else
#endif
    if (handle_syscall_fault( context, &rec )) return;
    macrunner_hb_trace_native_fault( "segv", context, &rec, esr );
#if defined(__APPLE__)
    macrunner_hb_trace_signal_exception_delivery( "segv", context, &rec,
                                                  low_stack_fault, low_stack_fault,
                                                  virtual_stack );
#endif
    setup_exception( context, &rec );
}


/**********************************************************************
 *		ill_handler
 *
 * Handler for SIGILL.
 */
static void ill_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_ILLEGAL_INSTRUCTION };
    ucontext_t *context = sigcontext;
    static int macrunner_hb_ill_trace_count;
    ULONG instr = 0;

    macrunner_signal_read_u32_aligned( PC_sig( context ), &instr );

    if (macrunner_hb_trace_callback_route_enabled() && macrunner_hb_ill_trace_count++ < 16)
        ERR( "macrunner-hb-signal-entry: kind=ill pid=%d pc=%p sp=%p instr=%#lx "
             "si_code=%d pstate=%#llx x0=%p x1=%p x16=%p x20=%p x23=%p x26=%p\n",
             getpid(), (void *)(ULONG_PTR)PC_sig(context),
             (void *)(ULONG_PTR)SP_sig(context), instr, siginfo->si_code,
             (unsigned long long)PSTATE_sig(context),
             (void *)(ULONG_PTR)REGn_sig(0, context),
             (void *)(ULONG_PTR)REGn_sig(1, context),
             (void *)(ULONG_PTR)REGn_sig(16, context),
             (void *)(ULONG_PTR)REGn_sig(20, context),
             (void *)(ULONG_PTR)REGn_sig(23, context),
             (void *)(ULONG_PTR)REGn_sig(26, context) );

    if (macrunner_hb_redirect_arm64x_hexpthk_sigill( context )) return;
    if (hb_jit_runtime_handle_signal_fault( PC_sig(context), 0, signal ) ||
        hb_jit_runtime_handle_signal_fault( LR_sig(context), 0, signal )) return;
    if (macrunner_hb_route_x64_callback_fault( context, 0, "sigill" )) return;

    if (!(PSTATE_sig( context ) & 0x10) && /* AArch64 (not WoW) */
        !(PC_sig( context ) & 3))
    {
        /* emulate mrs xN, CurrentEL */
        if ((instr & ~0x1f) == 0xd5384240) {
            ULONG reg = instr & 0x1f;
            /* ignore writes to xzr */
            if (reg != 31) REGn_sig(reg, context) = 0;
            PC_sig(context) += 4;
            return;
        }
    }

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		bus_handler
 *
 * Handler for SIGBUS.
 */
static void bus_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_DATATYPE_MISALIGNMENT };
    ucontext_t *context = sigcontext;
    BOOL alignment_fault = FALSE;
    BOOL low_stack_fault = FALSE;
    BOOL stack_overflow_fault = FALSE;
    BOOL fault_unhandled;
    void *virtual_stack = (void *)SP_sig(context);
    TEB *teb = NtCurrentTeb();

#if defined(BUS_ADRALN)
    /* MacRunner (2026-07-02): si_code==BUS_ADRALN alone is NOT reliable on this kernel --
     * observed firing for a genuine ARM64 permission fault (ESR DFSC=0xf, write to a
     * mapped-but-read-only page) with insn=`str x0,[x20]`/`strb w11,[x9]`, neither of which
     * can fault from misalignment on ARM64. Cross-check against the ESR's own DFSC field
     * (bits [5:0] of the ISS): 0x21 is the dedicated "Alignment fault" code, distinct from
     * the permission-fault range 0x0d-0x0f and the translation-fault range 0x04-0x07. Only
     * classify as a real alignment fault when BOTH agree, so a permission fault is no longer
     * mislabeled STATUS_DATATYPE_MISALIGNMENT further down (macrunner-hb-bus-fault trace
     * confirmed this exact esr=0x9200004f / insn=0xf9000280 signature). */
    alignment_fault = (siginfo->si_code == BUS_ADRALN) &&
                       ((get_fault_esr( context ) & 0x3f) == 0x21);
#endif

    rec.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    rec.NumberParameters = 2;
    if ((get_fault_esr( context ) & 0xf0000000) == 0x80000000)
        rec.ExceptionInformation[0] = EXCEPTION_EXECUTE_FAULT;
    else if (get_fault_esr( context ) & 0x40)
        rec.ExceptionInformation[0] = EXCEPTION_WRITE_FAULT;
    else
        rec.ExceptionInformation[0] = EXCEPTION_READ_FAULT;
    rec.ExceptionInformation[1] = (ULONG_PTR)siginfo->si_addr;
    if (getenv("MACRUNNER_HB_TRACE_BUS_FAULT"))
    {
        static int bfc = 0;
        if (bfc < 24)
        {
            unsigned int insn = 0;
            memcpy( &insn, (void *)(ULONG_PTR)PC_sig(context), 4 );
            fprintf( stderr, "macrunner-hb-bus-fault: #%d pc=%p fault=%p esr=0x%llx align=%u kind=%lu insn=0x%08x\n",
                     bfc, (void *)(ULONG_PTR)PC_sig(context), (void *)(ULONG_PTR)siginfo->si_addr,
                     (unsigned long long)get_fault_esr( context ), (unsigned)alignment_fault,
                     (unsigned long)rec.ExceptionInformation[0], insn );
            fflush( stderr );
            bfc++;
        }
    }
#if defined(__APPLE__)
    /* recover a gated direct guest-mem copy fault BEFORE any other handling/locking */
    macrunner_hb_dmem_fault_recover( (unsigned long long)(ULONG_PTR)siginfo->si_addr );
    stack_overflow_fault = macrunner_hb_is_low_stack_access_fault( context, &rec );
    virtual_stack = macrunner_hb_virtual_fault_stack( context, &rec );
#endif
    if (teb && teb->DeallocationStack &&
        (char *)SP_sig(context) < (char *)teb->Tib.StackLimit + 0x10000)
    {
        low_stack_fault = TRUE;
        if (macrunner_hb_trace_low_stack_fault_enabled())
            ERR( "macrunner-hb-bus-low-stack: before-virtual pid=%d si_code=%d align=%u "
                 "pc=%p lr=%p fault=%p esr=0x%llx sp=%p teb_stack=%p-%p dealloc=%p "
                 "x0=%p x1=%p x2=%p x3=%p x4=%p x16=%p x18=%p x24=%p x26=%p\n",
                 getpid(), siginfo->si_code, alignment_fault,
                 (void *)(ULONG_PTR)PC_sig(context), (void *)(ULONG_PTR)LR_sig(context),
                 (void *)(ULONG_PTR)rec.ExceptionInformation[1],
                 (unsigned long long)get_fault_esr( context ), (void *)(ULONG_PTR)SP_sig(context),
                 teb->Tib.StackLimit, teb->Tib.StackBase, teb->DeallocationStack,
                 (void *)(ULONG_PTR)REGn_sig(0, context),
                 (void *)(ULONG_PTR)REGn_sig(1, context),
                 (void *)(ULONG_PTR)REGn_sig(2, context),
                 (void *)(ULONG_PTR)REGn_sig(3, context),
                 (void *)(ULONG_PTR)REGn_sig(4, context),
                 (void *)(ULONG_PTR)REGn_sig(16, context),
                 (void *)(ULONG_PTR)REGn_sig(18, context),
                 (void *)(ULONG_PTR)REGn_sig(24, context),
                 (void *)(ULONG_PTR)REGn_sig(26, context) );
#ifdef __APPLE__
        if (macrunner_hb_trace_native_faults_enabled())
        {
            macrunner_hb_trace_low_stack_symbol( "pc", PC_sig(context) );
            macrunner_hb_trace_low_stack_symbol( "lr", LR_sig(context) );
            macrunner_hb_trace_low_stack_vm_region( "fault", rec.ExceptionInformation[1] );
        }
#endif
    }

    if (macrunner_hb_trace_callback_route_enabled())
        ERR( "macrunner-hb-signal-entry: kind=bus pid=%d pc=%p fault=%p esr=0x%llx "
             "x4=%p x16=%p x24=%p x26=%p\n",
             getpid(), (void *)(ULONG_PTR)PC_sig(context),
             (void *)(ULONG_PTR)rec.ExceptionInformation[1],
             (unsigned long long)get_fault_esr( context ),
             (void *)(ULONG_PTR)REGn_sig(4, context), (void *)(ULONG_PTR)REGn_sig(16, context),
             (void *)(ULONG_PTR)REGn_sig(24, context), (void *)(ULONG_PTR)REGn_sig(26, context) );

#if defined(__APPLE__)
    /* MacRunner fault-time diagnostic (env-gated): the x64 JIT reported a clean MEMORY_FAULT
     * reading UnityPlayer .data (host ~0x87efe45xxxx) that the load-time probe showed RW-committed.
     * Dump the ACTUAL fault-time Mach region+protection for faults landing in the x64-guest high
     * window, to pin whether the page was reprotected (prot=0) / decommitted (region gap) at runtime. */
    if (getenv( "MACRUNNER_DIAG_FAULTVM" ))
    {
        ULONG_PTR fa = rec.ExceptionInformation[1];
        if (fa >= 0x87ef0000000ULL && fa < 0x87f00000000ULL)
        {
            static int faultvm_n;
            if (faultvm_n++ < 24)
            {
                mach_vm_address_t ra = (mach_vm_address_t)fa;
                mach_vm_size_t rs = 0;
                vm_region_basic_info_data_64_t info;
                mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                kern_return_t kr = mach_vm_region( mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                                   (vm_region_info_t)&info, &cnt, &obj );
                if (obj != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), obj );
                fprintf( stderr, "macrunner-diag-faultvm: pc=%p lr=%p fault=%p access=%lu kr=%d "
                         "region=%p end=%p size=0x%llx prot=0x%x max=0x%x\n",
                         (void *)(ULONG_PTR)PC_sig(context), (void *)(ULONG_PTR)LR_sig(context),
                         (void *)fa, (unsigned long)rec.ExceptionInformation[0], kr,
                         (void *)(uintptr_t)ra, (void *)(uintptr_t)(ra + rs),
                         (unsigned long long)rs, kr == KERN_SUCCESS ? info.protection : 0,
                         kr == KERN_SUCCESS ? info.max_protection : 0 );
            }
        }
    }
#endif
    if (hb_jit_runtime_handle_signal_fault( PC_sig(context), rec.ExceptionInformation[1], signal ) ||
        hb_jit_runtime_handle_signal_fault( LR_sig(context), rec.ExceptionInformation[1], signal ))
        return;

    /* Same Phase F rule as segv_handler: macOS can surface execution of
     * x86_64 guest bytes as SIGBUS/EXC_BAD_ACCESS when the byte pattern
     * decodes as a faulting ARM64/SVE memory instruction. */
    if (macrunner_hb_route_x64_callback_fault( context, rec.ExceptionInformation[1], "bus-guest" )) return;

    fault_unhandled = virtual_handle_fault( &rec, virtual_stack );
    if (low_stack_fault && macrunner_hb_trace_low_stack_fault_enabled())
        ERR( "macrunner-hb-bus-low-stack: after-virtual pid=%d unhandled=%u code=%#lx "
             "fault=%p sp=%p vstack=%p teb_stack=%p-%p dealloc=%p\n",
             getpid(), fault_unhandled, rec.ExceptionCode,
             (void *)(ULONG_PTR)rec.ExceptionInformation[1],
             (void *)(ULONG_PTR)SP_sig(context), virtual_stack,
             teb->Tib.StackLimit, teb->Tib.StackBase, teb->DeallocationStack );
    if (!fault_unhandled)
    {
#if defined(__APPLE__)
        if (virtual_is_valid_code_address( (void *)PC_sig(context), sizeof(DWORD) ))
            setup_x18_resume_from_sigcontext( context );
#endif
        return;
    }
#if defined(__APPLE__)
    if (stack_overflow_fault)
    {
        rec.ExceptionCode = STATUS_STACK_OVERFLOW;
        rec.NumberParameters = 0;
    }
    else if (macrunner_hb_try_commit_or_upgrade_page( (unsigned long long)rec.ExceptionInformation[1] ))
        return;
#endif
    if (handle_syscall_fault( context, &rec )) return;
    macrunner_hb_trace_native_fault( "bus", context, &rec, get_fault_esr( context ) );

    if (alignment_fault && !stack_overflow_fault)
    {
        memset( &rec, 0, sizeof(rec) );
        rec.ExceptionCode = EXCEPTION_DATATYPE_MISALIGNMENT;
    }

#if defined(__APPLE__)
    macrunner_hb_trace_signal_exception_delivery( "bus", context, &rec,
                                                  low_stack_fault, stack_overflow_fault,
                                                  virtual_stack );
#endif
    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		trap_handler
 *
 * Handler for SIGTRAP.
 */
static void trap_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };
    ucontext_t *context = sigcontext;
    CONTEXT ctx;

    rec.ExceptionAddress = (void *)PC_sig(context);
    save_context( &ctx, sigcontext );
    if (macrunner_hb_trace_callback_route_enabled())
        ERR( "macrunner-hb-signal-entry: kind=trap pid=%d pc=%p sp=%p si_code=%d "
             "x4=%p x16=%p x24=%p x26=%p\n",
             getpid(), (void *)(ULONG_PTR)PC_sig(context),
             (void *)(ULONG_PTR)SP_sig(context), siginfo->si_code,
             (void *)(ULONG_PTR)REGn_sig(4, context),
             (void *)(ULONG_PTR)REGn_sig(16, context),
             (void *)(ULONG_PTR)REGn_sig(24, context),
             (void *)(ULONG_PTR)REGn_sig(26, context) );

    switch (siginfo->si_code)
    {
    case TRAP_TRACE:
        rec.ExceptionCode = EXCEPTION_SINGLE_STEP;
        break;
    case TRAP_BRKPT:
        /* debug exceptions do not update ESR on Linux, so we fetch the instruction directly. */
        if (!(PSTATE_sig( context ) & 0x10) && /* AArch64 (not WoW) */
            !(PC_sig( context ) & 3))
        {
            ULONG instr;
            ULONG imm;

            if (!macrunner_signal_read_u32_aligned( PC_sig( context ), &instr )) break;
            imm = (instr >> 5) & 0xffff;
            switch (imm)
            {
            case 0xf000:
                ctx.Pc += 4;  /* skip the brk instruction */
                rec.ExceptionCode = EXCEPTION_BREAKPOINT;
                rec.NumberParameters = 1;
                break;
            case 0xf001:
                rec.ExceptionCode = STATUS_ASSERTION_FAILURE;
                break;
            case 0xf003:
                rec.ExceptionCode = STATUS_STACK_BUFFER_OVERRUN;
                rec.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
                rec.NumberParameters = 1;
                rec.ExceptionInformation[0] = ctx.X[0];
                NtRaiseException( &rec, &ctx, FALSE );
                break;
            case 0xf004:
                rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
                break;
            default:
                rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
                break;
            }
        }
        break;
    default:
        rec.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
        break;
    }

    setup_raise_exception( sigcontext, &rec, &ctx );
}

/**********************************************************************
 *		fpe_handler
 *
 * Handler for SIGFPE.
 */
static void fpe_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { 0 };

    switch (siginfo->si_code & 0xffff )
    {
#ifdef FPE_FLTSUB
    case FPE_FLTSUB:
        rec.ExceptionCode = EXCEPTION_ARRAY_BOUNDS_EXCEEDED;
        break;
#endif
#ifdef FPE_INTDIV
    case FPE_INTDIV:
        rec.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_INTOVF
    case FPE_INTOVF:
        rec.ExceptionCode = EXCEPTION_INT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTDIV
    case FPE_FLTDIV:
        rec.ExceptionCode = EXCEPTION_FLT_DIVIDE_BY_ZERO;
        break;
#endif
#ifdef FPE_FLTOVF
    case FPE_FLTOVF:
        rec.ExceptionCode = EXCEPTION_FLT_OVERFLOW;
        break;
#endif
#ifdef FPE_FLTUND
    case FPE_FLTUND:
        rec.ExceptionCode = EXCEPTION_FLT_UNDERFLOW;
        break;
#endif
#ifdef FPE_FLTRES
    case FPE_FLTRES:
        rec.ExceptionCode = EXCEPTION_FLT_INEXACT_RESULT;
        break;
#endif
#ifdef FPE_FLTINV
    case FPE_FLTINV:
#endif
    default:
        rec.ExceptionCode = EXCEPTION_FLT_INVALID_OPERATION;
        break;
    }
    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		int_handler
 *
 * Handler for SIGINT.
 */
static void int_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    HANDLE handle;

    if (!p__wine_ctrl_routine) return;
    if (!NtCreateThreadEx( &handle, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(),
                           p__wine_ctrl_routine, 0 /* CTRL_C_EVENT */, 0, 0, 0, 0, NULL ))
        NtClose( handle );
}


/**********************************************************************
 *		abrt_handler
 *
 * Handler for SIGABRT.
 */
static void abrt_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    EXCEPTION_RECORD rec = { EXCEPTION_WINE_ASSERTION, EXCEPTION_NONCONTINUABLE };

    setup_exception( sigcontext, &rec );
}


/**********************************************************************
 *		quit_handler
 *
 * Handler for SIGQUIT.
 */
static void quit_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    ucontext_t *context = sigcontext;

    if (!is_inside_syscall( SP_sig(context) )) user_mode_abort_thread( 0, get_syscall_frame() );
    abort_thread(0);
}


/**********************************************************************
 *		usr1_handler
 *
 * Handler for SIGUSR1, used to signal a thread that it got suspended.
 */
static void usr1_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    ucontext_t *ucontext = sigcontext;
    CONTEXT context;

    if (is_inside_syscall( SP_sig(ucontext) ))
    {
        context.ContextFlags = CONTEXT_FULL | CONTEXT_EXCEPTION_REQUEST;
        NtGetContextThread( GetCurrentThread(), &context );
        wait_suspend( &context );
        NtSetContextThread( GetCurrentThread(), &context );
    }
    else
    {
        save_context( &context, ucontext );
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING;
        wait_suspend( &context );
        restore_context( &context, ucontext );
#if defined(__APPLE__)
        setup_x18_resume_from_sigcontext( ucontext );
#endif
    }
}


/**********************************************************************
 *		usr2_handler
 *
 * Handler for SIGUSR2, used to set a thread context.
 */
static void usr2_handler( int signal, siginfo_t *siginfo, void *sigcontext )
{
    struct syscall_frame *frame = get_syscall_frame();
    ucontext_t *context = sigcontext;
    DWORD i;

    if (!is_inside_syscall( SP_sig(context) )) return;

    FP_sig(context)     = frame->fp;
    LR_sig(context)     = frame->lr;
    SP_sig(context)     = frame->sp;
    PC_sig(context)     = frame->pc;
    PSTATE_sig(context) = frame->cpsr;
    for (i = 0; i <= 28; i++) REGn_sig( i, context ) = frame->x[i];

#ifdef linux
    {
        struct fpsimd_context *fp = get_fpsimd_context( sigcontext );
        if (fp)
        {
            fp->fpcr = frame->fpcr;
            fp->fpsr = frame->fpsr;
            memcpy( fp->vregs, frame->v, sizeof(fp->vregs) );
        }
    }
#elif defined(__APPLE__)
    context->uc_mcontext->__ns.__fpcr = frame->fpcr;
    context->uc_mcontext->__ns.__fpsr = frame->fpsr;
    memcpy( context->uc_mcontext->__ns.__v, frame->v, sizeof(frame->v) );
    setup_x18_resume_from_sigcontext( context );
#endif
}


/**********************************************************************
 *           get_thread_ldt_entry
 */
NTSTATUS get_thread_ldt_entry( HANDLE handle, THREAD_DESCRIPTOR_INFORMATION *info, ULONG len )
{
    return STATUS_NOT_IMPLEMENTED;
}


/**********************************************************************
 *             signal_init_threading
 */
static void segv_handler( int signal, siginfo_t *siginfo, void *sigcontext );
static void ill_handler( int signal, siginfo_t *siginfo, void *sigcontext );
static void bus_handler( int signal, siginfo_t *siginfo, void *sigcontext );

#if defined(__APPLE__) && defined(__aarch64__)
static struct sigaction macrunner_hb_prev_segv_action;
static struct sigaction macrunner_hb_prev_ill_action;
static struct sigaction macrunner_hb_prev_bus_action;
static BOOL macrunner_hb_primary_signal_trace;
static BOOL macrunner_hb_primary_signal_pre_wine_installed;
static BOOL macrunner_hb_primary_signal_post_wine_installed;
static BOOL macrunner_hb_wine_signal_handlers_ready;

static struct sigaction *macrunner_hb_prev_action_for_signal( int sig )
{
    if (sig == SIGSEGV) return &macrunner_hb_prev_segv_action;
    if (sig == SIGILL) return &macrunner_hb_prev_ill_action;
    return &macrunner_hb_prev_bus_action;
}

static const char *macrunner_hb_signal_source( int sig )
{
    if (sig == SIGSEGV) return "primary-segv";
    if (sig == SIGILL) return "primary-sigill";
    return "primary-bus";
}

static void macrunner_hb_chain_signal( int sig, siginfo_t *siginfo, void *sigcontext )
{
    struct sigaction *prev = macrunner_hb_prev_action_for_signal( sig );

    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction)
    {
        prev->sa_sigaction( sig, siginfo, sigcontext );
        return;
    }
    if (prev->sa_handler == SIG_IGN) return;
    if (prev->sa_handler && prev->sa_handler != SIG_DFL)
    {
        prev->sa_handler( sig );
        return;
    }

    if (!macrunner_hb_wine_signal_handlers_ready)
    {
        if (sig == SIGBUS)
        {
            static unsigned int early_bus_count;

            if (early_bus_count++ < 4)
                macrunner_signal_writef( "macrunner-hb-early-native-bus: pid=%d pc=%p fault=%p "
                                         "routing-to-wine-bus-handler\n",
                                         getpid(), (void *)(ULONG_PTR)PC_sig((ucontext_t *)sigcontext),
                                         (void *)(ULONG_PTR)siginfo->si_addr );
            bus_handler( sig, siginfo, sigcontext );
            return;
        }
        macrunner_signal_writef( "macrunner-hb-early-nonx64-signal: pid=%d sig=%d pc=%p fault=%p "
                                 "prev_flags=%#x prev_handler=%p prev_sigaction=%p\n",
                                 getpid(), sig, (void *)(ULONG_PTR)PC_sig((ucontext_t *)sigcontext),
                                 (void *)(ULONG_PTR)(sig == SIGILL ? 0 : (ULONG_PTR)siginfo->si_addr),
                                 prev->sa_flags, prev->sa_handler, prev->sa_sigaction );
        signal( sig, SIG_DFL );
        raise( sig );
        return;
    }

    /* Once Wine owns its normal signal state, non-x64 faults still need Wine's
     * native handlers even if this primary handler was installed over a default
     * action in a bootstrap edge case. */
    if (sig == SIGSEGV)
    {
        segv_handler( sig, siginfo, sigcontext );
        return;
    }
    if (sig == SIGILL)
    {
        ill_handler( sig, siginfo, sigcontext );
        return;
    }
    if (sig == SIGBUS)
    {
        bus_handler( sig, siginfo, sigcontext );
        return;
    }

    signal( sig, SIG_DFL );
    raise( sig );
}

static void macrunner_hb_primary_signal_handler( int sig, siginfo_t *siginfo, void *sigcontext )
{
    ULONG_PTR fault_addr = (sig == SIGILL) ? 0 : (ULONG_PTR)siginfo->si_addr;

    /* MacRunner diag: does ANY signal handler see the NULL-target (pc=0) fault? */
    {
        ULONG_PTR pcv = PC_sig( (ucontext_t *)sigcontext );
        if ((pcv < 0x10000 || fault_addr < 0x10000) && pcv != fault_addr)
        {
            const char *nv = getenv( "MACRUNNER_HB_TRACE_NULLCALL" );
            static int prim_n;
            if (nv && nv[0] && nv[0] != '0' && prim_n++ < 16)
                fprintf( stderr, "macrunner-hb-primary-sig: sig=%d pc=%p fault=%p lr=%p\n",
                         sig, (void *)pcv, (void *)fault_addr,
                         (void *)(ULONG_PTR)LR_sig( (ucontext_t *)sigcontext ) ), fflush( stderr );
        }
    }

    if (macrunner_hb_route_x64_callback_fault( sigcontext, fault_addr,
                                               macrunner_hb_signal_source( sig ) ))
        return;

    if (macrunner_hb_primary_signal_trace && getenv( "MACRUNNER_HB_TRACE_SIGNAL_CHAIN" ))
        fprintf( stderr, "macrunner-hb-signal-chain: pid=%d sig=%d pc=%p fault=%p\n",
                 getpid(), sig, (void *)(ULONG_PTR)PC_sig((ucontext_t *)sigcontext),
                 (void *)(ULONG_PTR)fault_addr );
    macrunner_hb_chain_signal( sig, siginfo, sigcontext );
}

#if defined(__APPLE__) && defined(__aarch64__)
static void macrunner_hb_start_arm64ec_spin_watchdog(void);
#endif

static void macrunner_hb_install_primary_signal_handlers( const char *stage, BOOL x64_image_trigger )
{
    struct sigaction sig_act;
    BOOL macrunner_trace;
    int segv_rc, ill_rc, bus_rc;

    if (!macrunner_hb_x64_loader_enabled()) return;
    if (!x64_image_trigger && !macrunner_hb_x64_guest_process()) return;
    if (macrunner_hb_wine_signal_handlers_ready)
    {
        if (macrunner_hb_primary_signal_post_wine_installed) return;
    }
    else if (macrunner_hb_primary_signal_pre_wine_installed) return;

    macrunner_trace = macrunner_hb_trace_callback_route_enabled() ||
                      (getenv( "MACRUNNER_HB_TRACE_HOST_EXEC" ) &&
                       getenv( "MACRUNNER_HB_TRACE_HOST_EXEC" )[0] &&
                       getenv( "MACRUNNER_HB_TRACE_HOST_EXEC" )[0] != '0');
    macrunner_hb_primary_signal_trace = macrunner_trace;
    fprintf( stderr, "macrunner-hb-signal-init: pid=%d stage=%s-start trace=%d\n",
             getpid(), stage, macrunner_trace );

    /* MacRunner Lane A (nested-exception spin fix, 2026-06-12): mask SIGSEGV/SIGBUS
     * while this handler runs (drop SA_NODEFER).  On Apple, macOS wipes x18 on
     * sigreturn so PE code takes a dense storm of healable x18/TEB faults; if a
     * GENUINE guest access-violation (e.g. Mono-JIT deref of a NULL/garbage pointer)
     * fires WHILE the handler is still dispatching a preceding fault on the SA_ONSTACK
     * signal stack, SA_NODEFER let it pre-empt re-entrantly.  Its saved SP is then in
     * the signal-stack band, so virtual_setup_exception sees is_inside_signal_stack()
     * and calls abort_thread(1) -> the main thread dies and the boot spins to timeout
     * (stochastic ~1/2).  Masking defers the second fault until after sigreturn, when
     * SP is back on the thread stack, so it dispatches as an ordinary in-guest SEH AV
     * instead of aborting.  The x18-heal paths never deliberately re-fault (they only
     * touch already-mapped TEB + patch sigcontext), so masking is safe; this matches
     * upstream Wine's segv_handler posture (no SA_NODEFER). */
    sigemptyset( &sig_act.sa_mask );
    sigaddset( &sig_act.sa_mask, SIGSEGV );
    sigaddset( &sig_act.sa_mask, SIGBUS );
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sig_act.sa_sigaction = macrunner_hb_primary_signal_handler;

    segv_rc = sigaction( SIGSEGV, &sig_act, &macrunner_hb_prev_segv_action );
    /* Leave SIGILL directly owned by Wine.  Native ARM64 PE uses SIGILL in
     * startup/exception-dispatch paths, while x64 no-exec boundary faults are
     * delivered as SIGBUS/SIGSEGV on macOS.  Wine's own ill_handler still
     * routes guest x64 PCs through HyperBridge if that ever appears. */
    ill_rc = 0;
    bus_rc = sigaction( SIGBUS, &sig_act, &macrunner_hb_prev_bus_action );
    if (macrunner_hb_wine_signal_handlers_ready)
        macrunner_hb_primary_signal_post_wine_installed = TRUE;
    else
        macrunner_hb_primary_signal_pre_wine_installed = TRUE;
    macrunner_hb_note_x64_guest_fault_handlers_ready();

    fprintf( stderr, "macrunner-hb-signal-init: pid=%d stage=%s-primary-installed rc=%d/%d/%d\n",
             getpid(), stage, segv_rc, ill_rc, bus_rc );
#if defined(__APPLE__) && defined(__aarch64__)
    macrunner_hb_start_arm64ec_spin_watchdog();
#endif
}

void macrunner_hb_prepare_x64_guest_fault_handlers( const char *stage )
{
    macrunner_hb_install_primary_signal_handlers( stage, TRUE );
}
#endif

#if defined(__APPLE__) && defined(__aarch64__)
static void macrunner_hb_trace_arm64ec_watchdog_bytes( mach_vm_address_t addr, char *buf,
                                                       size_t buf_size )
{
    uint8_t bytes[32];
    mach_vm_size_t out_size = 0;
    kern_return_t kr;
    size_t i, pos = 0;

    if (!buf_size) return;
    buf[0] = 0;
    kr = mach_vm_read_overwrite( mach_task_self(), addr, sizeof(bytes),
                                 (mach_vm_address_t)(uintptr_t)bytes, &out_size );
    if (kr != KERN_SUCCESS)
    {
        snprintf( buf, buf_size, "read=%d", kr );
        return;
    }
    for (i = 0; i < out_size && pos + 3 < buf_size; i++)
        pos += snprintf( buf + pos, buf_size - pos, "%02x", bytes[i] );
}

static void macrunner_hb_trace_arm64ec_watchdog_ascii( mach_vm_address_t addr, char *buf,
                                                       size_t buf_size )
{
    uint8_t bytes[48];
    mach_vm_size_t out_size = 0;
    kern_return_t kr;
    size_t i, pos = 0;

    if (!buf_size) return;
    buf[0] = 0;
    if (!addr)
    {
        snprintf( buf, buf_size, "null" );
        return;
    }

    kr = mach_vm_read_overwrite( mach_task_self(), addr, sizeof(bytes),
                                 (mach_vm_address_t)(uintptr_t)bytes, &out_size );
    if (kr != KERN_SUCCESS)
    {
        snprintf( buf, buf_size, "read=%d", kr );
        return;
    }

    for (i = 0; i < out_size && pos + 2 < buf_size; i++)
    {
        uint8_t c = bytes[i];

        if (!c) break;
        buf[pos++] = (c >= 0x21 && c <= 0x7e) ? (char)c : '.';
    }
    buf[pos] = 0;
}

static unsigned int macrunner_hb_arm64ec_watchdog_env_u32( const char *name, unsigned int fallback,
                                                           unsigned int min_value, unsigned int max_value )
{
    const char *env = getenv( name );
    char *end;
    unsigned long value;

    if (!env || !env[0]) return fallback;
    value = strtoul( env, &end, 0 );
    if (end == env) return fallback;
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    return (unsigned int)value;
}

static void *macrunner_hb_arm64ec_spin_watchdog_thread( void *arg )
{
    unsigned int delay_ms = macrunner_hb_arm64ec_watchdog_env_u32(
        "MACRUNNER_HB_TRACE_ARM64EC_SPIN_WATCHDOG_DELAY_MS", 3000, 0, 600000 );
    unsigned int interval_ms = macrunner_hb_arm64ec_watchdog_env_u32(
        "MACRUNNER_HB_TRACE_ARM64EC_SPIN_WATCHDOG_INTERVAL_MS", 1000, 10, 60000 );
    unsigned int sample_count = macrunner_hb_arm64ec_watchdog_env_u32(
        "MACRUNNER_HB_TRACE_ARM64EC_SPIN_WATCHDOG_SAMPLES", 30, 1, 10000 );
    int sample, thread_index;

    (void)arg;
    usleep( (useconds_t)delay_ms * 1000 );
    for (sample = 0; sample < sample_count; sample++)
    {
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t thread_count = 0;
        kern_return_t kr = task_threads( mach_task_self(), &threads, &thread_count );

        if (kr != KERN_SUCCESS)
        {
            fprintf( stderr, "macrunner-hb-arm64ec-watchdog: sample=%d task_threads=%d\n",
                     sample, kr );
            fflush( stderr );
            usleep( (useconds_t)interval_ms * 1000 );
            continue;
        }
        for (thread_index = 0; thread_index < thread_count; thread_index++)
        {
            arm_thread_state64_t state;
            mach_msg_type_number_t state_count = ARM_THREAD_STATE64_COUNT;
            char bytes[80];
            char x0s[80], x1s[80], x14s[80], x20s[80];
            uint64_t pc;

            if (threads[thread_index] == mach_thread_self()) continue;
            kr = thread_get_state( threads[thread_index], ARM_THREAD_STATE64,
                                   (thread_state_t)&state, &state_count );
            if (kr != KERN_SUCCESS) continue;
            pc = state.__pc;
            if (pc < 0x0000080000000000ULL) continue;
            macrunner_hb_trace_arm64ec_watchdog_bytes( (mach_vm_address_t)(pc & ~3ULL),
                                                       bytes, sizeof(bytes) );
            macrunner_hb_trace_arm64ec_watchdog_ascii( (mach_vm_address_t)state.__x[0],
                                                       x0s, sizeof(x0s) );
            macrunner_hb_trace_arm64ec_watchdog_ascii( (mach_vm_address_t)state.__x[1],
                                                       x1s, sizeof(x1s) );
            macrunner_hb_trace_arm64ec_watchdog_ascii( (mach_vm_address_t)state.__x[14],
                                                       x14s, sizeof(x14s) );
            macrunner_hb_trace_arm64ec_watchdog_ascii( (mach_vm_address_t)state.__x[20],
                                                       x20s, sizeof(x20s) );
            fprintf( stderr, "macrunner-hb-arm64ec-watchdog: pid=%d sample=%d thread=%d "
                      "pc=%p lr=%p sp=%p x0=%p x1=%p x2=%p x3=%p x4=%p "
                      "x8=%p x9=%p x10=%p x11=%p x12=%p x13=%p x14=%p "
                      "x16=%p x18=%p x20=%p x0s=%s x1s=%s x14s=%s x20s=%s bytes=%s\n",
                      getpid(), sample, thread_index, (void *)(uintptr_t)pc,
                      (void *)(uintptr_t)state.__lr, (void *)(uintptr_t)state.__sp,
                      (void *)(uintptr_t)state.__x[0], (void *)(uintptr_t)state.__x[1],
                      (void *)(uintptr_t)state.__x[2], (void *)(uintptr_t)state.__x[3],
                      (void *)(uintptr_t)state.__x[4], (void *)(uintptr_t)state.__x[8],
                      (void *)(uintptr_t)state.__x[9], (void *)(uintptr_t)state.__x[10],
                      (void *)(uintptr_t)state.__x[11], (void *)(uintptr_t)state.__x[12],
                      (void *)(uintptr_t)state.__x[13], (void *)(uintptr_t)state.__x[14],
                      (void *)(uintptr_t)state.__x[16], (void *)(uintptr_t)state.__x[18],
                      (void *)(uintptr_t)state.__x[20], x0s, x1s, x14s, x20s, bytes );
            fflush( stderr );
        }
        if (threads)
            vm_deallocate( mach_task_self(), (vm_address_t)threads,
                           thread_count * sizeof(*threads) );
        usleep( (useconds_t)interval_ms * 1000 );
    }
    return NULL;
}

static void macrunner_hb_start_arm64ec_spin_watchdog(void)
{
    static BOOL started;
    pthread_t thread;
    const char *env = getenv( "MACRUNNER_HB_TRACE_ARM64EC_SPIN_WATCHDOG" );

    if (started || !env || !env[0] || env[0] == '0') return;
    started = TRUE;
    if (!pthread_create( &thread, NULL, macrunner_hb_arm64ec_spin_watchdog_thread, NULL ))
        pthread_detach( thread );
}
#endif

void signal_init_threading(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
    macrunner_hb_install_primary_signal_handlers( "early-threading", FALSE );
#endif
}


/**********************************************************************
 *             signal_alloc_thread
 */
NTSTATUS signal_alloc_thread( TEB *teb )
{
    return STATUS_SUCCESS;
}


/**********************************************************************
 *             signal_free_thread
 */
void signal_free_thread( TEB *teb )
{
}


/**********************************************************************
 *		signal_init_process
 */
void signal_init_process(void)
{
    struct sigaction sig_act;
    struct ntdll_thread_data *thread_data = ntdll_get_thread_data();
    void *kernel_stack = (char *)thread_data->kernel_stack + kernel_stack_size;
    BOOL macrunner_trace = macrunner_hb_trace_callback_route_enabled();

    if (macrunner_trace)
        fprintf( stderr, "macrunner-hb-signal-init: pid=%d stage=start\n", getpid() );

    thread_data->syscall_frame = (struct syscall_frame *)kernel_stack - 1;

    signal_alloc_thread( NtCurrentTeb() );
    if (macrunner_trace)
        fprintf( stderr, "macrunner-hb-signal-init: pid=%d stage=thread-allocated\n", getpid() );

    sig_act.sa_mask = server_block_set;
    sig_act.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;

    sig_act.sa_sigaction = int_handler;
    if (sigaction( SIGINT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = fpe_handler;
    if (sigaction( SIGFPE, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = abrt_handler;
    if (sigaction( SIGABRT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = quit_handler;
    if (sigaction( SIGQUIT, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = usr1_handler;
    if (sigaction( SIGUSR1, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = usr2_handler;
    if (sigaction( SIGUSR2, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = trap_handler;
    if (sigaction( SIGTRAP, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = segv_handler;
    if (sigaction( SIGSEGV, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = ill_handler;
    if (sigaction( SIGILL, &sig_act, NULL ) == -1) goto error;
    sig_act.sa_sigaction = bus_handler;
    if (sigaction( SIGBUS, &sig_act, NULL ) == -1) goto error;
#if defined(__APPLE__) && defined(__aarch64__)
    macrunner_hb_wine_signal_handlers_ready = TRUE;
    macrunner_hb_primary_signal_pre_wine_installed = FALSE;
    macrunner_hb_install_primary_signal_handlers( "wine-process", FALSE );
    macrunner_hb_start_arm64ec_spin_watchdog();
#else
    macrunner_hb_note_x64_guest_fault_handlers_ready();
#endif
    if (macrunner_trace)
        fprintf( stderr, "macrunner-hb-signal-init: pid=%d stage=installed-segv-ill-bus\n", getpid() );
    return;

 error:
    perror("sigaction");
    exit(1);
}


/***********************************************************************
 *           syscall_dispatcher_return_slowpath
 */
void syscall_dispatcher_return_slowpath(void)
{
    raise( SIGUSR2 );
}

/***********************************************************************
 *           init_syscall_frame
 */
void init_syscall_frame( LPTHREAD_START_ROUTINE entry, void *arg, BOOL suspend, TEB *teb )
{
    struct syscall_frame *frame = ((struct ntdll_thread_data *)&teb->GdiTebBatch)->syscall_frame;
    CONTEXT *ctx, context = { CONTEXT_ALL };
    I386_CONTEXT *i386_context;
    ARM_CONTEXT *arm_context;

    context.X0  = (DWORD64)entry;
    context.X1  = (DWORD64)arg;
    context.X18 = (DWORD64)teb;
    context.Sp  = (DWORD64)teb->Tib.StackBase;
    context.Pc  = (DWORD64)pRtlUserThreadStart;

    if ((i386_context = get_cpu_area( IMAGE_FILE_MACHINE_I386 )))
    {
        XMM_SAVE_AREA32 *fpu = (XMM_SAVE_AREA32 *)i386_context->ExtendedRegisters;
        i386_context->ContextFlags = CONTEXT_I386_ALL;
        i386_context->Eax = (ULONG_PTR)entry;
        i386_context->Ebx = (arg == peb ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        i386_context->Esp = get_wow_teb( teb )->Tib.StackBase - 16;
        i386_context->Eip = pLdrSystemDllInitBlock->pRtlUserThreadStart;
        i386_context->SegCs = 0x23;
        i386_context->SegDs = 0x2b;
        i386_context->SegEs = 0x2b;
        i386_context->SegFs = 0x53;
        i386_context->SegGs = 0x2b;
        i386_context->SegSs = 0x2b;
        i386_context->EFlags = 0x202;
        fpu->ControlWord = 0x27f;
        fpu->MxCsr = 0x1f80;
        fpux_to_fpu( &i386_context->FloatSave, fpu );
    }
    else if ((arm_context = get_cpu_area( IMAGE_FILE_MACHINE_ARMNT )))
    {
        arm_context->ContextFlags = CONTEXT_ARM_ALL;
        arm_context->R0 = (ULONG_PTR)entry;
        arm_context->R1 = (arg == peb ? (ULONG_PTR)wow_peb : (ULONG_PTR)arg);
        arm_context->Sp = get_wow_teb( teb )->Tib.StackBase;
        arm_context->Pc = pLdrSystemDllInitBlock->pRtlUserThreadStart;
        if (arm_context->Pc & 1) arm_context->Cpsr |= 0x20; /* thumb mode */
    }

    if (suspend)
    {
        context.ContextFlags |= CONTEXT_EXCEPTION_REPORTING | CONTEXT_EXCEPTION_ACTIVE;
        wait_suspend( &context );
    }

    ctx = (CONTEXT *)((ULONG_PTR)context.Sp & ~15) - 1;
    *ctx = context;
    ctx->ContextFlags = CONTEXT_FULL;
    signal_set_full_context( ctx );

    frame->sp    = (ULONG64)ctx;
    frame->pc    = (ULONG64)pLdrInitializeThunk;
    frame->x[0]  = (ULONG64)ctx;
    frame->x[18] = (ULONG64)teb;
    syscall_frame_fixup_for_fastpath( frame );

    pthread_sigmask( SIG_UNBLOCK, &server_block_set, NULL );
}


/***********************************************************************
 *           signal_start_thread
 */
__ASM_GLOBAL_FUNC( signal_start_thread,
                   "stp x29, x30, [sp,#-0xc0]!\n\t"
                   __ASM_CFI(".cfi_def_cfa_offset 0xc0\n\t")
                   __ASM_CFI(".cfi_offset 29,-0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30,-0xb8\n\t")
                   "mov x29, sp\n\t"
                   __ASM_CFI(".cfi_def_cfa_register 29\n\t")
                   "stp x19, x20, [x29, #0x10]\n\t"
                   __ASM_CFI(".cfi_rel_offset 19,0x10\n\t")
                   __ASM_CFI(".cfi_rel_offset 20,0x18\n\t")
                   "stp x21, x22, [x29, #0x20]\n\t"
                   __ASM_CFI(".cfi_rel_offset 21,0x20\n\t")
                   __ASM_CFI(".cfi_rel_offset 22,0x28\n\t")
                   "stp x23, x24, [x29, #0x30]\n\t"
                   __ASM_CFI(".cfi_rel_offset 23,0x30\n\t")
                   __ASM_CFI(".cfi_rel_offset 24,0x38\n\t")
                   "stp x25, x26, [x29, #0x40]\n\t"
                   __ASM_CFI(".cfi_rel_offset 25,0x40\n\t")
                   __ASM_CFI(".cfi_rel_offset 26,0x48\n\t")
                   "stp x27, x28, [x29, #0x50]\n\t"
                   __ASM_CFI(".cfi_rel_offset 27,0x50\n\t")
                   __ASM_CFI(".cfi_rel_offset 28,0x58\n\t")
                   "add x5, x29, #0xc0\n\t"     /* syscall_cfa */
                   /* set syscall frame */
                   "ldr x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "cbnz x4, 1f\n\t"
                   "sub x4, sp, #0x330\n\t"     /* sizeof(struct syscall_frame) */
                   "str x4, [x3, #0x378]\n\t"   /* thread_data->syscall_frame */
                   "1:\tstr wzr, [x4, #0x10c]\n\t" /* frame->restore_flags */
                   "stp xzr, x5, [x4, #0x110]\n\t" /* frame->prev_frame,syscall_cfa */
                   /* switch to kernel stack */
                   "mov sp, x4\n\t"
                   "bl " __ASM_NAME("init_syscall_frame") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/***********************************************************************
 *           __wine_syscall_dispatcher
 */
__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,
                   "hint 34\n\t" /* bti c */
                   WINE_LOAD_TEB_IN_X17
                   "ldr x10, [x17, #0x378]\n\t" /* thread_data->syscall_frame */
                   "stp x17, x19, [x10, #0x90]\n\t"
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "mov x19, sp\n\t"
                   "stp x9, x19, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "str w8, [x10, #0x120]\n\t"
                   "mrs x9, FPCR\n\t"
                   "str w9, [x10, #0x128]\n\t"
                   "mrs x9, FPSR\n\t"
                   "str w9, [x10, #0x12c]\n\t"
                   "stp q0,  q1,  [x10, #0x130]\n\t"
                   "stp q2,  q3,  [x10, #0x150]\n\t"
                   "stp q4,  q5,  [x10, #0x170]\n\t"
                   "stp q6,  q7,  [x10, #0x190]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "stp q16, q17, [x10, #0x230]\n\t"
                   "stp q18, q19, [x10, #0x250]\n\t"
                   "stp q20, q21, [x10, #0x270]\n\t"
                   "stp q22, q23, [x10, #0x290]\n\t"
                   "stp q24, q25, [x10, #0x2b0]\n\t"
                   "stp q26, q27, [x10, #0x2d0]\n\t"
                   "stp q28, q29, [x10, #0x2f0]\n\t"
                   "stp q30, q31, [x10, #0x310]\n\t"
                   "mov x22, x10\n\t"
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x22, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   "and x20, x8, #0xfff\n\t"    /* syscall number */
                   "ubfx x21, x8, #12, #2\n\t"  /* syscall table number */
                   "mov x18, x17\n\t"            /* x18 is Apple-volatile; x17 holds TEB */
                   "ldr x16, [x18, #0x370]\n\t" /* thread_data->syscall_table */
                   "add x21, x16, x21, lsl #5\n\t"
                   "ldr x16, [x21, #16]\n\t"    /* table->ServiceLimit */
                   "cmp x20, x16\n\t"
                   "bcs " __ASM_LOCAL_LABEL("bad_syscall") "\n\t"
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w9, [x16, x20]\n\t"
                   "subs x9, x9, #64\n\t"
                   "bls 2f\n\t"
                   "sub sp, sp, x9\n\t"
                   "tbz x9, #3, 1f\n\t"
                   "sub sp, sp, #8\n"
                   "1:\tsub x9, x9, #8\n\t"
                   "ldr x10, [x19, x9]\n\t"
                   "str x10, [sp, x9]\n\t"
                   "cbnz x9, 1b\n"
                   "2:\tldr x16, [x21]\n\t"     /* table->ServiceTable */
                   "ldr x23, [x16, x20, lsl 3]\n\t"
                   "mov x18, x17\n\t"            /* x18 is Apple-volatile; x17 holds TEB */
                   "ldr w11, [x18, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall") "\n\t"
                   "blr x23\n\t"
                   "mov sp, x22\n"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") ":\n\t"
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "tbz x16, #1, 2f\n\t"        /* CONTEXT_INTEGER */
                   "ldp x12, x13, [sp, #0x80]\n\t" /* frame->x[16..17] */
                   "ldp x14, x15, [sp, #0xf8]\n\t" /* frame->sp, frame->pc */
                   "cmp x12, x15\n\t"              /* frame->x16 == frame->pc? */
                   "ccmp x13, x14, #0, eq\n\t"     /* frame->x17 == frame->sp? */
                   "beq 1f\n\t"                    /* take slowpath if unequal */
                   "bl " __ASM_NAME("syscall_dispatcher_return_slowpath") "\n"
                   "1:\tldp x0, x1, [sp, #0x00]\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x8, x9, [sp, #0x40]\n\t"
                   "ldp x10, x11, [sp, #0x50]\n\t"
                   "ldp x12, x13, [sp, #0x60]\n\t"
                   "ldp x14, x15, [sp, #0x70]\n"
                   "2:\tldp x18, x19, [sp, #0x90]\n\t"
                   "ldp x20, x21, [sp, #0xa0]\n\t"
                   "ldp x22, x23, [sp, #0xb0]\n\t"
                   "ldp x24, x25, [sp, #0xc0]\n\t"
                   "ldp x26, x27, [sp, #0xd0]\n\t"
                   "ldp x28, x29, [sp, #0xe0]\n\t"
                   "tbz x16, #2, 1f\n\t"        /* CONTEXT_FLOATING_POINT */
                   "ldp q0,  q1,  [sp, #0x130]\n\t"
                   "ldp q2,  q3,  [sp, #0x150]\n\t"
                   "ldp q4,  q5,  [sp, #0x170]\n\t"
                   "ldp q6,  q7,  [sp, #0x190]\n\t"
                   "ldp q8,  q9,  [sp, #0x1b0]\n\t"
                   "ldp q10, q11, [sp, #0x1d0]\n\t"
                   "ldp q12, q13, [sp, #0x1f0]\n\t"
                   "ldp q14, q15, [sp, #0x210]\n\t"
                   "ldp q16, q17, [sp, #0x230]\n\t"
                   "ldp q18, q19, [sp, #0x250]\n\t"
                   "ldp q20, q21, [sp, #0x270]\n\t"
                   "ldp q22, q23, [sp, #0x290]\n\t"
                   "ldp q24, q25, [sp, #0x2b0]\n\t"
                   "ldp q26, q27, [sp, #0x2d0]\n\t"
                   "ldp q28, q29, [sp, #0x2f0]\n\t"
                   "ldp q30, q31, [sp, #0x310]\n\t"
                   "ldr w17, [sp, #0x128]\n\t"
                   "msr FPCR, x17\n\t"
                   "ldr w17, [sp, #0x12c]\n\t"
                   "msr FPSR, x17\n"
                   "1:\tldp x16, x17, [sp, #0x100]\n\t"
                   "msr NZCV, x17\n\t"
                   "ldp x30, x17, [sp, #0xf0]\n\t"
#if defined(__APPLE__)
                   WINE_RESTORE_X18_FROM_TEB
#endif
                   /* switch to user stack */
                   "mov sp, x17\n\t"
                   "ret x16\n"

                   __ASM_LOCAL_LABEL("trace_syscall") ":\n\t"
                   "stp x0, x1, [sp, #-0x40]!\n\t"
                   "stp x2, x3, [sp, #0x10]\n\t"
                   "stp x4, x5, [sp, #0x20]\n\t"
                   "stp x6, x7, [sp, #0x30]\n\t"
                   "mov x0, x8\n\t"             /* id */
                   "mov x1, sp\n\t"             /* args */
                   "ldr x16, [x21, #24]\n\t"    /* table->ArgumentTable */
                   "ldrb w2, [x16, x20]\n\t"    /* len */
                   "bl " __ASM_NAME("trace_syscall") "\n\t"
                   "ldp x2, x3, [sp, #0x10]\n\t"
                   "ldp x4, x5, [sp, #0x20]\n\t"
                   "ldp x6, x7, [sp, #0x30]\n\t"
                   "ldp x0, x1, [sp], #0x40\n\t"
                   "blr x23\n"
                   "mov sp, x22\n"

                   __ASM_LOCAL_LABEL("trace_syscall_ret") ":\n\t"
                   "mov x21, x0\n\t"            /* retval */
                   "ldr w0, [sp, #0x120]\n\t"   /* frame->syscall_id */
                   "mov x1, x21\n\t"            /* retval */
                   "bl " __ASM_NAME("trace_sysret") "\n\t"
                   "mov x0, x21\n\t"            /* retval */
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n"

                   __ASM_LOCAL_LABEL("bad_syscall") ":\n\t"
                   "mov x0, #0xc0000000\n\t"    /* STATUS_INVALID_SYSTEM_SERVICE */
                   "movk x0, #0x001c\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )

__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher_return,
                   WINE_RESTORE_X18_IF_ZERO
                   "ldr w11, [x18, #0x380]\n\t" /* thread_data->syscall_trace */
                   "cbnz x11, " __ASM_LOCAL_LABEL("trace_syscall_ret") "\n\t"
                   "b " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") )


/***********************************************************************
 *           __wine_unix_call_dispatcher
 */
__ASM_GLOBAL_FUNC( __wine_unix_call_dispatcher,
                   "hint 34\n\t" /* bti c */
                   WINE_RESTORE_X18_IF_ZERO
                   "ldr x10, [x18, #0x378]\n\t" /* thread_data->syscall_frame */
                   "stp x18, x19, [x10, #0x90]\n\t"
                   "stp x20, x21, [x10, #0xa0]\n\t"
                   "stp x22, x23, [x10, #0xb0]\n\t"
                   "stp x24, x25, [x10, #0xc0]\n\t"
                   "stp x26, x27, [x10, #0xd0]\n\t"
                   "stp x28, x29, [x10, #0xe0]\n\t"
                   "stp q8,  q9,  [x10, #0x1b0]\n\t"
                   "stp q10, q11, [x10, #0x1d0]\n\t"
                   "stp q12, q13, [x10, #0x1f0]\n\t"
                   "stp q14, q15, [x10, #0x210]\n\t"
                   "mov x9, sp\n\t"
                   "stp x30, x9, [x10, #0xf0]\n\t"
                   "mrs x9, NZCV\n\t"
                   "stp x30, x9, [x10, #0x100]\n\t"
                   "mov x19, x10\n\t"
                   /* switch to kernel stack */
                   "mov sp, x10\n\t"
                   /* we're now on the kernel stack, stitch unwind info with previous frame */
                   __ASM_CFI_CFA_IS_AT2(x19, 0x98, 0x02) /* frame->syscall_cfa */
                   __ASM_CFI(".cfi_offset 29, -0xc0\n\t")
                   __ASM_CFI(".cfi_offset 30, -0xb8\n\t")
                   __ASM_CFI(".cfi_offset 19, -0xb0\n\t")
                   __ASM_CFI(".cfi_offset 20, -0xa8\n\t")
                   __ASM_CFI(".cfi_offset 21, -0xa0\n\t")
                   __ASM_CFI(".cfi_offset 22, -0x98\n\t")
                   __ASM_CFI(".cfi_offset 23, -0x90\n\t")
                   __ASM_CFI(".cfi_offset 24, -0x88\n\t")
                   __ASM_CFI(".cfi_offset 25, -0x80\n\t")
                   __ASM_CFI(".cfi_offset 26, -0x78\n\t")
                   __ASM_CFI(".cfi_offset 27, -0x70\n\t")
                   __ASM_CFI(".cfi_offset 28, -0x68\n\t")
                   "ldr x16, [x0, x1, lsl 3]\n\t"
                   "mov x0, x2\n\t"             /* args */
                   "blr x16\n\t"
                   "ldr w16, [sp, #0x10c]\n\t"  /* frame->restore_flags */
                   "cbnz w16, " __ASM_LOCAL_LABEL("__wine_syscall_dispatcher_return") "\n\t"
                   __ASM_CFI_CFA_IS_AT2(sp, 0x98, 0x02) /* frame->syscall_cfa */
                   "ldp x18, x19, [sp, #0x90]\n\t"
                   "ldp x16, x17, [sp, #0xf8]\n\t"
                   /* switch to user stack */
                   "mov sp, x16\n\t"
                   "ret x17" )

#endif  /* __aarch64__ */
