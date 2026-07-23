#ifndef HB_RUNTIME_H
#define HB_RUNTIME_H

#include "hb_result.h"
#include "hb_context.h"
#include "hb_ir.h"
#include "hb_codegen.h"
#include "hb_cache.h"

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
    bool owns_block;
    bool fused;
    bool valid;
} hb_block_cache_entry_t;

typedef struct {
    uint64_t guest_addr;
    uint8_t* target_code;
    size_t patch_offset;
} hb_block_chain_meta_t;

/* MacRunner (2026-06-17 — FIX#2a, HK rank-7 livelock): the block-cache hash table was
 * the JIT limiter (filled 65536/65536 during Mono ReloadAssembly while the 128MB JIT
 * exec buffer was only ~18% used), tripping the sticky code_cache_full latch -> JIT
 * permanently disabled -> everything fell to the slow cache-full re-dispatch fallback.
 * Sized so the 128MB exec buffer (~347K blocks @ ~377B) is the real limiter, not this
 * table. calloc'd per-thread (~56B/entry => ~29MB virtual/thread, lazy zero-fill so
 * physical is pay-as-touched; idle worker threads touch almost none). */
#define HB_BLOCK_CACHE_SIZE 524288

/* In-memory block cache */
typedef struct {
    hb_block_cache_entry_t entries[HB_BLOCK_CACHE_SIZE];
    size_t count;
    /* MacRunner 2026-06-22 (lever #3, ABZU first-frame): hb_jit_runtime_reset runs once per
     * nested run_x64 frame (13777x in ABZU's _initterm grind). The old block_cache_reset
     * memset the whole entries[] array (~29MB) and looped all 524288 slots every frame,
     * faulting in + writing every page and defeating the lazy zero-fill (~11.3% self-time).
     * Track the slots actually occupied this generation so reset clears ONLY those (== count).
     * used_overflow falls back to the full memset if the tracking array can't grow (OOM-safe);
     * correctness invariant preserved: after reset every slot has valid==false. */
    uint32_t* used_slots;
    size_t used_count;
    size_t used_cap;
    bool used_overflow;
    /* Block chaining is experimental/env-gated. Keep the hot cache entry at
     * baseline size when the flag is off; allocate side metadata only on use. */
    hb_block_chain_meta_t* chain_meta;
} hb_block_cache_t;

/* JIT executor */
typedef struct {
    hb_context_t* ctx;
    hb_jit_buffer_t* jit_mem;
    hb_block_cache_t* block_cache;
    uint64_t hot_trace_blocks;
    uint64_t hot_trace_next;
    uint64_t code_cache_full_reports;
    hb_cache_t* persistent_cache;
    uint8_t persistent_cache_flags;
    bool code_cache_full;
    /* Default-off native-signal recovery quarantine, shared by the SIGBUS
     * invalidation and SIGILL ownership paths. This deliberately survives
     * hb_jit_runtime_reset(): retrying a native block that already faulted would
     * recreate the same signal loop after the code cache is reset. */
    uint64_t* jit_signal_quarantine;
    size_t jit_signal_quarantine_count;
    size_t jit_signal_quarantine_capacity;
    bool jit_signal_disable;
} hb_jit_runtime_t;

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx);
void hb_runtime_init_environment(void);
void hb_jit_runtime_destroy(hb_jit_runtime_t* rt);
/* MacRunner: reset a runtime for reuse by another callback on the same thread
 * (per-thread pool) instead of destroy+recreate per callback. Eagerly frees the
 * block_cache's owned blocks + clears it, rewinds the jit arena bump pointer, and
 * re-points ctx — translations are fully regenerated (SMC-safe, no stale code). */
void hb_jit_runtime_reset(hb_jit_runtime_t* rt, hb_context_t* ctx);
hb_result_t hb_jit_runtime_compile(hb_jit_runtime_t* rt, const hb_ir_func_t* func);
hb_result_t hb_jit_runtime_run(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out);
int hb_jit_runtime_handle_signal_fault(uint64_t pc, uint64_t fault_addr, int signal,
                                       const void* host_context);
/* SIGILL ownership is selected by ntdll's cached, default-off gate.  Unlike the
 * generic range-based path this claims any active TLS JIT guard: generated code
 * can branch through a stale/corrupt native target outside the current slab.
 * The signal handler supplies the already-probed instruction word so the
 * runtime never dereferences an arbitrary fault PC after siglongjmp(). */
int hb_jit_runtime_handle_owned_sigill(uint64_t pc, uint32_t native_word,
                                       int native_word_valid,
                                       const void* host_context);
/* Resolve an address inside the owning thread's live JIT block cache.  This is
 * read-only diagnostic metadata: callers must use the runtime on its owner
 * thread and must not retain the result across hb_jit_runtime_reset(). */
int hb_jit_runtime_native_block_info(hb_jit_runtime_t* rt, uint64_t native_pc,
                                     uint64_t* guest_addr, uint64_t* native_start,
                                     size_t* native_size);

/* Unified runtime entry */
hb_result_t hb_runtime_run(hb_context_t* ctx, const hb_ir_func_t* func, hb_backend_t backend, hb_exec_result_t* out);

#ifdef __cplusplus
}
#endif

#endif
