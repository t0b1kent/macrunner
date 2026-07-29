#ifndef HB_CODEGEN_H
#define HB_CODEGEN_H

#include "hb_result.h"
#include "hb_ir.h"
#include "hb_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MacRunner 2026-07-29 — RELOCATION TABLE.
 *
 * The persistent cache has to turn absolute host addresses in emitted code into something a
 * later process can restore. Until now it did that by RE-DISCOVERING the sites afterwards:
 * scanning for `blr x23`, matching the shape of the surrounding movs, and rejecting anything it
 * did not recognise. That is why 89 % of unstored blocks were multi-helper ones, and why four
 * separate rejection reasons exist at all.
 *
 * Codegen already knows every one of these offsets at the instant it writes them. Recording them
 * here removes the search — and with it the site cap, the x2/x3/x4 veto, the helper-id lookup
 * and the arg1 window matching, which are all artefacts of guessing after the fact rather than
 * properties of the code. This is the shape the literature uses for persistent code caches:
 * emit relocatable host code, keep the relocation list beside it.
 *
 * Recorded centrally in emit_mov_imm64() for x1 and x23, so none of the 62 helper-call sites or
 * 37 pointer-mov sites needs to change — a migration that size is where mistakes hide.
 */
/* 256, not 64. Measured on Hollow Knight: 39434 blocks produced 194322 sites — 4.9 per block on
 * average — and 127 blocks overflowed a 64-entry table. The old single-stub matcher handled
 * exactly ONE site, which is why it rejected almost everything: a block with one relocation is
 * the exception here, not the rule. A table that silently truncates is worse than no table, so
 * the cap is set well clear of the measured distribution and overflow stays counted. */
#define HB_CODEGEN_MAX_RELOCS 256

/* What the recorded value MEANS, stated by the emitter rather than guessed from the register.
 *
 * The store path used to read `reg == 23` as "this is a helper address", because emit_call_helper()
 * is the obvious producer of x23. It is not the only one. emit_mask_x_reg_to_size() uses x23 as its
 * scratch register at 22 of its 25 call sites, and for a 32-bit operand it emits
 * `mov x23, 0xffffffff` — the zero-extension every 32-bit x86 operation needs. A plain 32-bit ADD
 * produces exactly one relocation, and it is that. The store path looked 0xffffffff up in the
 * helper id table, found nothing, and declined the whole block: 53 655 of them on Hollow Knight,
 * 90 % of all declines and 24.6 % of every block that reached the cache.
 *
 * It is also why registering the 24 missing helpers (05f3f3f9) moved retention by nothing — the
 * old matcher only ever inspected the real `blr x23` target, while the table sees every x23 write.
 * (A large `mem.disp` is parked in x23 too, at hb_arm64_codegen.c:993, but only when direct-mem is
 * enabled, which it is not on Hollow Knight. The mask is the one that fires.)
 *
 * That is the same mistake `mh_widearg` was: matching the register instead of the value. Codegen
 * knows which one it is emitting, so it says so here and nothing downstream has to infer it. */
typedef enum {
    HB_RELOC_KIND_VALUE = 0,  /* an ordinary immediate — classified by value at store time */
    HB_RELOC_KIND_HELPER = 1  /* a C helper entry address — only ever from emit_call_helper() */
} hb_codegen_reloc_kind_t;

typedef struct {
    size_t off;      /* byte offset of the 4-instruction MOVZ/MOVK sequence */
    uint8_t reg;     /* destination register: 1/2/3/4 (arguments) or 23 (helper target, scratch) */
    uint8_t kind;    /* hb_codegen_reloc_kind_t — fits in existing padding, so the table is free */
    uint64_t value;  /* absolute value written, resolved against the block at store time */
} hb_codegen_reloc_t;

/* Code generation result */
typedef struct {
    uint8_t* code;
    size_t size;
    size_t capacity;
    hb_codegen_reloc_t relocs[HB_CODEGEN_MAX_RELOCS];
    size_t reloc_count;
    bool reloc_overflow;  /* more sites than the table holds — do not trust it for this block */
    hb_arch_t arch;  /* guest architecture — defaults to HB_ARCH_X64 (=0) */
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
    uint64_t magic;
} hb_jit_buffer_t;

hb_jit_buffer_t* hb_jit_buffer_create(size_t size);
void hb_jit_buffer_destroy(hb_jit_buffer_t* buf);
/* MacRunner: reset the bump pointer to reuse the (already-mmap'd) arena for a
 * fresh round of codegen without munmap/mmap. Translations are regenerated, so
 * callers MUST also clear any cache that points into this arena (block_cache). */
hb_result_t hb_jit_buffer_reset(hb_jit_buffer_t* buf);
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
