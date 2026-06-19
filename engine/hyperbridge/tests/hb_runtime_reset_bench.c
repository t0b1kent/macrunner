#include <hb_context.h>
#include <hb_runtime.h>
#include <hb_codegen.h>
#include <hb_decoder.h>
#include <hb_lifter.h>
#include <hb_memory.h>
#include <hb_ir.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static hb_ir_func_t *make_ret_func_x86(void) {
    const uint32_t code_base = 0x7bdc5e90u;
    uint8_t code[] = {0xc2, 0x18, 0x00};
    hb_decoder_t *dec = hb_decoder_create(HB_ARCH_X86, code, sizeof(code), code_base);
    hb_ir_func_t *func = NULL;
    if (!dec) return NULL;
    if (hb_lift_func_x86(dec, &func) != HB_OK) func = NULL;
    hb_decoder_destroy(dec);
    return func;
}

static hb_context_t *make_ctx_x86(hb_ir_func_t *func, uint32_t code_base) {
    hb_context_t *ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_JIT);
    if (!ctx) return NULL;
    ctx->memory = hb_memory_create(0);
    if (!ctx->memory) { hb_context_destroy(ctx); return NULL; }
    if (hb_memory_guest32_map(ctx->memory, code_base & ~0xfffu, 4096,
                              HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC) != HB_OK ||
        hb_memory_write(ctx->memory, code_base, (uint8_t*)func->cfg->blocks[0]->guest_addr ? (uint8_t*)0 : (uint8_t*)0, 0) != HB_OK) {
        hb_context_destroy(ctx);
        return NULL;
    }
    ctx->pc = code_base;
    ctx->regs.x86.eip = code_base;
    ctx->regs.x86.esp = 0x0140fb68u;
    return ctx;
}

int main(void) {
    const int N = 100;
    size_t i;
    double t0, t1;
    const uint32_t code_base = 0x7bdc5e90u;

    hb_ir_func_t *func = make_ret_func_x86();
    if (!func) { fprintf(stderr, "lift failed\n"); return 1; }

    printf("JIT runtime reset-vs-create/destroy with JIT run (N=%d)\n", N);

    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_context_t *ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_JIT);
        ctx->memory = hb_memory_create(0);
        hb_memory_guest32_map(ctx->memory, code_base & ~0xfffu, 4096,
                              HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC);
        ctx->pc = code_base; ctx->regs.x86.eip = code_base; ctx->regs.x86.esp = 0x0140fb68u;
        hb_jit_runtime_t *rt = hb_jit_runtime_create(ctx);
        hb_exec_result_t out;
        hb_jit_runtime_run(rt, func, &out);
        hb_jit_runtime_destroy(rt);
        hb_context_destroy(ctx);
    }
    t1 = now_sec();
    double create_destroy_ms = (t1 - t0) * 1000.0;
    printf("create+destroy+run: %.3f ms total, %.3f ms/iter\n",
           create_destroy_ms, create_destroy_ms / N);

    hb_context_t *base_ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_JIT);
    base_ctx->memory = hb_memory_create(0);
    hb_memory_guest32_map(base_ctx->memory, code_base & ~0xfffu, 4096,
                          HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC);
    base_ctx->pc = code_base; base_ctx->regs.x86.eip = code_base; base_ctx->regs.x86.esp = 0x0140fb68u;
    hb_jit_runtime_t *rt = hb_jit_runtime_create(base_ctx);
    hb_exec_result_t warmup;
    hb_jit_runtime_run(rt, func, &warmup);

    hb_context_t *ctxs[N];
    for (i = 0; i < N; i++) {
        ctxs[i] = hb_context_create(HB_ARCH_X86, HB_BACKEND_JIT);
        ctxs[i]->memory = hb_memory_create(0);
        hb_memory_guest32_map(ctxs[i]->memory, code_base & ~0xfffu, 4096,
                              HB_PERM_READ | HB_PERM_WRITE | HB_PERM_EXEC);
        ctxs[i]->pc = code_base; ctxs[i]->regs.x86.eip = code_base; ctxs[i]->regs.x86.esp = 0x0140fb68u;
    }

    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_jit_runtime_reset(rt, ctxs[i]);
        hb_exec_result_t out;
        hb_jit_runtime_run(rt, func, &out);
    }
    t1 = now_sec();
    double reset_ms = (t1 - t0) * 1000.0;
    printf("reset+run      : %.3f ms total, %.3f ms/iter\n",
           reset_ms, reset_ms / N);
    printf("reset/create-destroy ratio: %.4f\n", reset_ms / create_destroy_ms);

    hb_jit_runtime_destroy(rt);
    hb_context_destroy(base_ctx);
    for (i = 0; i < N; i++) hb_context_destroy(ctxs[i]);
    hb_ir_func_destroy(func);
    return 0;
}
