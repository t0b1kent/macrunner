#include "hb_context.h"
#include "hb_memory.h"
#include "hb_x87.h"
#include "hb_trace.h"
#include "hb_cache.h"
#include "hb_thunk.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static uint64_t read_x64_block_limit_env(void) {
    const char* value = getenv("MACRUNNER_HB_X64_BLOCK_LIMIT");
    if (!value || !*value) return 0;

    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value) return 0;
    return (uint64_t)parsed;
}

static uint64_t read_step_limit_env(hb_arch_t arch) {
    const char* name = (arch == HB_ARCH_X86) ? "MACRUNNER_HB_X86_STEP_LIMIT" : "MACRUNNER_HB_STEP_LIMIT";
    const char* value = getenv(name);
    uint64_t fallback = (arch == HB_ARCH_X86) ? 10000000ULL : 1000000ULL;

    if (!value || !*value) return fallback;

    errno = 0;
    char* end = NULL;
    unsigned long long parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value) return fallback;
    return (uint64_t)parsed;
}

hb_context_t* hb_context_create(hb_arch_t arch, hb_backend_t backend) {
    hb_context_t* ctx = calloc(1, sizeof(hb_context_t));
    if (!ctx) return NULL;
    ctx->arch = arch;
    ctx->mode = (arch == HB_ARCH_X64) ? HB_MODE_64BIT : HB_MODE_32BIT;
    ctx->backend = backend;
    ctx->config.arch = arch;
    ctx->config.backend = backend;
    ctx->step_limit = read_step_limit_env(arch);
    ctx->block_limit = (arch == HB_ARCH_X64) ? read_x64_block_limit_env() : 0;
    ctx->exit_code = 0;
    ctx->last_result = HB_OK;
    if (arch == HB_ARCH_X86) hb_x87_reset(&ctx->regs.x86.x87);
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
    if (ctx->arch == HB_ARCH_X86) hb_x87_reset(&ctx->regs.x86.x87);
    memset(ctx->ymm_hi, 0, sizeof(ctx->ymm_hi));
    memset(ctx->zmm_hi, 0, sizeof(ctx->zmm_hi));
    memset(ctx->k, 0, sizeof(ctx->k));
    memset(ctx->xmm_ext, 0, sizeof(ctx->xmm_ext));
    memset(ctx->ymm_hi_ext, 0, sizeof(ctx->ymm_hi_ext));
    memset(ctx->zmm_hi_ext, 0, sizeof(ctx->zmm_hi_ext));
    memset(&ctx->flags, 0, sizeof(ctx->flags));
    memset(&ctx->lazy_flags, 0, sizeof(ctx->lazy_flags));
    ctx->step_count = 0;
    ctx->block_count = 0;
    ctx->pc = 0;
    ctx->exit_code = 0;
    ctx->last_result = HB_OK;
    ctx->indirect_ic_guest_addr = 0;
    ctx->indirect_ic_native_code = 0;
    ctx->codegen_module_base = 0;
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
