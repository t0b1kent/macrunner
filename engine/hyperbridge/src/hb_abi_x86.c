#include "hb_abi.h"
#include "hb_memory.h"
#include <stdlib.h>
#include <string.h>

hb_result_t hb_abi_x86_cdecl_call(hb_context_t* ctx, uint32_t target, hb_abi_x86_cdecl_t* call, uint32_t* out) {
    if (!ctx || !call) return HB_ERR_INVALID_ARG;
    uint32_t esp = ctx->regs.x86.esp;

    /* Push args right-to-left */
    for (size_t i = call->stack_arg_count; i > 0; i--) {
        esp -= 4;
        hb_memory_write_u32(ctx->memory, esp, call->stack_args[i - 1]);
    }

    /* Push return address sentinel */
    esp -= 4;
    hb_memory_write_u32(ctx->memory, esp, 0xFFFF0000);

    ctx->regs.x86.esp = esp;
    ctx->pc = target;
    ctx->regs.x86.eip = target;

    if (out) *out = 0;
    return HB_OK;
}

hb_result_t hb_abi_x86_stdcall_call(hb_context_t* ctx, uint32_t target, hb_abi_x86_stdcall_t* call, uint32_t* out) {
    if (!ctx || !call) return HB_ERR_INVALID_ARG;
    uint32_t esp = ctx->regs.x86.esp;

    /* Push args right-to-left */
    for (size_t i = call->stack_arg_count; i > 0; i--) {
        esp -= 4;
        hb_memory_write_u32(ctx->memory, esp, call->stack_args[i - 1]);
    }

    /* Push return address sentinel */
    esp -= 4;
    hb_memory_write_u32(ctx->memory, esp, 0xFFFF0000);

    ctx->regs.x86.esp = esp;
    ctx->pc = target;
    ctx->regs.x86.eip = target;

    if (out) *out = 0;
    return HB_OK;
}
