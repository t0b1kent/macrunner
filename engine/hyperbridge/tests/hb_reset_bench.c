#include <hb_context.h>
#include <hb_runtime.h>
#include <hb_codegen.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void) {
    const int N = 100;
    double t0, t1;
    size_t i;

    printf("HyperBridge reset-vs-create/destroy micro-bench (N=%d)\n", N);

    /* --- create/destroy path (old per-callback cost) --- */
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_context_t *ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
        hb_jit_runtime_t *rt = hb_jit_runtime_create(ctx);
        if (!rt) { fprintf(stderr, "create failed\n"); return 1; }
        hb_jit_runtime_destroy(rt);
        hb_context_destroy(ctx);
    }
    t1 = now_sec();
    double create_destroy_ms = (t1 - t0) * 1000.0;
    printf("create+destroy: %.3f ms total, %.3f ms/iter\n",
           create_destroy_ms, create_destroy_ms / N);

    /* --- reset path (new per-callback cost) --- */
    hb_context_t *ctxs[N];
    for (i = 0; i < N; i++)
        ctxs[i] = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_context_t *base_ctx = hb_context_create(HB_ARCH_X64, HB_BACKEND_JIT);
    hb_jit_runtime_t *rt = hb_jit_runtime_create(base_ctx);
    if (!rt) { fprintf(stderr, "base create failed\n"); return 1; }

    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_jit_runtime_reset(rt, ctxs[i]);
    }
    t1 = now_sec();
    double reset_ms = (t1 - t0) * 1000.0;
    printf("reset only    : %.3f ms total, %.3f ms/iter\n",
           reset_ms, reset_ms / N);

    printf("reset/create-destroy ratio: %.4f\n", reset_ms / create_destroy_ms);

    hb_jit_runtime_destroy(rt);
    hb_context_destroy(base_ctx);
    for (i = 0; i < N; i++)
        hb_context_destroy(ctxs[i]);
    return 0;
}
