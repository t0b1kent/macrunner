#ifndef HB_RUNTIME_H
#define HB_RUNTIME_H

#include "hb_result.h"
#include "hb_context.h"
#include "hb_ir.h"
#include "hb_codegen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Execution result */
typedef struct {
    int exit_code;
    hb_result_t result;
    uint64_t steps_executed;
    uint64_t blocks_executed;
    uint64_t duration_ns;
    bool timed_out;
    bool faulted;
    const char* fault_reason;
} hb_exec_result_t;

/* Interpreter */
typedef struct hb_interpreter hb_interpreter_t;

hb_interpreter_t* hb_interpreter_create(hb_context_t* ctx);
void hb_interpreter_destroy(hb_interpreter_t* interp);
hb_result_t hb_interpreter_run(hb_interpreter_t* interp, const hb_ir_func_t* func, hb_exec_result_t* out);

/* In-memory block cache entry */
typedef struct {
    uint64_t guest_addr;
    uint8_t* native_code;
    size_t native_size;
    uint32_t steps;
    uint64_t hit_count;
    const hb_ir_block_t* block;
    bool fused;
    bool valid;
} hb_block_cache_entry_t;

#define HB_BLOCK_CACHE_SIZE 16384

/* In-memory block cache */
typedef struct {
    hb_block_cache_entry_t entries[HB_BLOCK_CACHE_SIZE];
} hb_block_cache_t;

/* JIT executor */
typedef struct {
    hb_context_t* ctx;
    hb_jit_buffer_t* jit_mem;
    hb_block_cache_t* block_cache;
    uint64_t hot_trace_blocks;
    uint64_t hot_trace_next;
} hb_jit_runtime_t;

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx);
void hb_jit_runtime_destroy(hb_jit_runtime_t* rt);
hb_result_t hb_jit_runtime_compile(hb_jit_runtime_t* rt, const hb_ir_func_t* func);
hb_result_t hb_jit_runtime_run(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out);

/* Unified runtime entry */
hb_result_t hb_runtime_run(hb_context_t* ctx, const hb_ir_func_t* func, hb_backend_t backend, hb_exec_result_t* out);

#ifdef __cplusplus
}
#endif

#endif
