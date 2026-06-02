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
hb_result_t hb_arm64_codegen_block_with_cfg(hb_arm64_codegen_t* cg, const hb_ir_block_t* block,
                                            const hb_ir_cfg_t* cfg, hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_copy_scan_counted_loop(hb_arm64_codegen_t* cg,
                                                    const hb_ir_block_t* body,
                                                    const hb_ir_block_t* guard,
                                                    hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_bounded_scan_loop(hb_arm64_codegen_t* cg,
                                               const hb_ir_block_t* guard,
                                               const hb_ir_block_t* body,
                                               hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_two_block_loop_helper(hb_arm64_codegen_t* cg,
                                                   const hb_ir_block_t* first,
                                                   const hb_ir_block_t* second,
                                                   hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_four_block_loop_helper(hb_arm64_codegen_t* cg,
                                                    const hb_ir_block_t* first,
                                                    const hb_ir_block_t* second,
                                                    const hb_ir_block_t* third,
                                                    const hb_ir_block_t* fourth,
                                                    hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_i32_less_tiebreaker_helper(hb_arm64_codegen_t* cg,
                                                        const hb_ir_block_t* entry,
                                                        const hb_ir_block_t* equal,
                                                        const hb_ir_block_t* less,
                                                        hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_unity_sort_inner_loop_helper(hb_arm64_codegen_t* cg,
                                                          const hb_ir_block_t* sort,
                                                          hb_codegen_buffer_t* out);
hb_result_t hb_arm64_codegen_instr(hb_arm64_codegen_t* cg, const hb_ir_instr_t* instr, hb_codegen_buffer_t* out);

/* JIT buffer management */
typedef struct {
    uint8_t* writable;
    uint8_t* executable;
    size_t size;
    size_t used;
    size_t dirty_start;
    bool is_executable;
    bool thread_jit_write_protect;
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
