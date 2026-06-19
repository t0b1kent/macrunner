#include <hb_codegen.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void) {
    const int N = 100;
    const size_t size = 128ULL * 1024ULL * 1024ULL;
    size_t i;
    double t0, t1;

    printf("JIT buffer create/destroy vs reset (N=%d, size=%zu MB)\n", N, size / (1024 * 1024));

    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_jit_buffer_t *buf = hb_jit_buffer_create(size);
        if (!buf) { fprintf(stderr, "create failed\n"); return 1; }
        hb_jit_buffer_destroy(buf);
    }
    t1 = now_sec();
    double create_destroy_ms = (t1 - t0) * 1000.0;
    printf("create+destroy: %.3f ms total, %.3f ms/iter\n",
           create_destroy_ms, create_destroy_ms / N);

    hb_jit_buffer_t *buf = hb_jit_buffer_create(size);
    if (!buf) { fprintf(stderr, "create failed\n"); return 1; }
    t0 = now_sec();
    for (i = 0; i < N; i++) {
        hb_jit_buffer_reset(buf);
    }
    t1 = now_sec();
    double reset_ms = (t1 - t0) * 1000.0;
    printf("reset only    : %.3f ms total, %.3f ms/iter\n",
           reset_ms, reset_ms / N);

    printf("reset/create-destroy ratio: %.4f\n", reset_ms / create_destroy_ms);

    hb_jit_buffer_destroy(buf);
    return 0;
}
