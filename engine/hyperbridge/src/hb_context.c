#include "hb_context.h"
#include "hb_memory.h"
#include "hb_trace.h"
#include "hb_cache.h"
#include "hb_thunk.h"
#include <stdlib.h>
#include <string.h>

hb_context_t* hb_context_create(hb_arch_t arch, hb_backend_t backend) {
    hb_context_t* ctx = calloc(1, sizeof(hb_context_t));
    if (!ctx) return NULL;
    ctx->arch = arch;
    ctx->mode = (arch == HB_ARCH_X64) ? HB_MODE_64BIT : HB_MODE_32BIT;
    ctx->backend = backend;
    ctx->config.arch = arch;
    ctx->config.backend = backend;
    ctx->step_limit = 1000000;
    ctx->block_limit = 10000;
    ctx->exit_code = 0;
    ctx->last_result = HB_OK;
    return ctx;
}

void hb_context_destroy(hb_context_t* ctx) {
    if (!ctx) return;
    if (ctx->memory) hb_memory_destroy(ctx->memory);
    if (ctx->trace) hb_trace_destroy(ctx->trace);
    if (ctx->cache) hb_cache_destroy(ctx->cache);
    if (ctx->thunks) hb_thunk_table_destroy(ctx->thunks);
    free(ctx);
}

hb_result_t hb_context_reset(hb_context_t* ctx) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    memset(&ctx->regs, 0, sizeof(ctx->regs));
    memset(&ctx->flags, 0, sizeof(ctx->flags));
    memset(&ctx->lazy_flags, 0, sizeof(ctx->lazy_flags));
    ctx->step_count = 0;
    ctx->block_count = 0;
    ctx->pc = 0;
    ctx->exit_code = 0;
    ctx->last_result = HB_OK;
    return HB_OK;
}

hb_result_t hb_context_set_pc(hb_context_t* ctx, hb_gva_t pc) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    ctx->pc = pc;
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rip = pc;
    else ctx->regs.x86.eip = (uint32_t)pc;
    return HB_OK;
}

hb_result_t hb_context_set_step_limit(hb_context_t* ctx, uint64_t limit) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    ctx->step_limit = limit;
    return HB_OK;
}

hb_result_t hb_context_set_block_limit(hb_context_t* ctx, uint64_t limit) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    ctx->block_limit = limit;
    return HB_OK;
}
