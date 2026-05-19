#ifndef HB_CODEGEN_H
#define HB_CODEGEN_H

#include "hb_result.h"
#include "hb_ir.h"
#include "hb_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Code generation result */
typedef struct {
    uint8_t* code;
    size_t size;
    size_t capacity;
} hb_codegen_buffer_t;

/* ARM64 codegen */
typedef struct hb_arm64_codegen hb_arm64_codegen_t;

hb_arm64_codegen_t* hb_arm64_codegen_create(hb_context_t* ctx);
void hb_arm64_codegen_destroy(hb_arm64_codegen_t* cg);

hb_result_t hb_arm64_codegen_func(hb_arm64_codegen_t* cg, const hb_ir_func_t* func, hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_block(hb_arm64_codegen_t* cg, const hb_ir_block_t* block, hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_instr(hb_arm64_codegen_t* cg, const hb_ir_instr_t* instr, hb_codegen_buffer_t* out);

/* JIT buffer management */
typedef struct {
    uint8_t* writable;
    uint8_t* executable;
    size_t size;
    size_t used;
    bool is_executable;
} hb_jit_buffer_t;

hb_jit_buffer_t* hb_jit_buffer_create(size_t size);
void hb_jit_buffer_destroy(hb_jit_buffer_t* buf);
hb_result_t hb_jit_buffer_commit(hb_jit_buffer_t* buf);
hb_result_t hb_jit_buffer_make_writable(hb_jit_buffer_t* buf);
hb_result_t hb_jit_buffer_make_executable(hb_jit_buffer_t* buf);
void hb_jit_buffer_flush_icache(hb_jit_buffer_t* buf);

/* Generic codegen helpers */
hb_codegen_buffer_t* hb_codegen_buffer_create(size_t cap);
void hb_codegen_buffer_destroy(hb_codegen_buffer_t* buf);
hb_result_t hb_codegen_buffer_append(hb_codegen_buffer_t* buf, const uint8_t* bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif
