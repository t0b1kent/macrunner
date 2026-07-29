#include "hb_runtime.h"
#include "hb_codegen.h"
#include "hb_memory.h"
#include "hb_contract_telemetry.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#define HB_RUNTIME_PERSISTENT_CACHE_VERSION 21u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_MEM   0x01u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_STACK 0x02u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_SCAN 0x04u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_MEM  0x08u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_BLOCK_CHAIN 0x10u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_INDIRECT_IC 0x20u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_NATIVE_MEMMOVE 0x40u

#define HB_RUNTIME_CACHE_BLOCK_SENTINEL  0x48425254424c4b31ull /* HBRTBLK1 */
#define HB_RUNTIME_CACHE_HELPER_SENTINEL 0x48425254484c5000ull /* HBRTHLP + id */
#define HB_RUNTIME_CACHE_HELPER_MASK     0xfffffffffffff000ull
#define HB_RUNTIME_CACHE_INSTR_SENTINEL  0x48425254494e0000ull /* HBRTIN + index */
#define HB_RUNTIME_CACHE_INSTR_MASK      0xffffffffffff0000ull

extern void hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_xfg_dispatch_call(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_ir_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_load_cmp_jcc_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_cmp_setcc_ret_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_string_bsearch_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_freelist_fill_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_u32_ptr_compare(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_bsearch_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_string_hash(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_string_equal(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_rowptr_entry(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_row_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_row_entry(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_col(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_coded_index_search(hb_context_t* ctx, const hb_ir_block_t* block);
extern uint64_t hb_jit_helper_try_native_memmove(hb_context_t* ctx, const hb_ir_block_t* block);

typedef struct hb_cached_helper_stub {
    size_t arg1_mov_off;
    size_t helper_mov_off;
    uint8_t helper_id;
    uint16_t instr_index;
    bool arg1_is_instr;
} hb_cached_helper_stub_t;

typedef struct hb_jit_signal_fault_frame {
    struct hb_jit_signal_fault_frame* prev;
    hb_jit_runtime_t* rt;
    hb_context_t* ctx;
    hb_block_cache_entry_t* entry;
    hb_context_t snapshot;
    uint64_t steps;
    uint64_t blocks_executed;
    uint64_t host_pc;
    uint64_t fault_addr;
    uint64_t host_gpr[31];
    uint64_t host_sp;
    uint64_t host_fault_pc;
    uint64_t host_pstate;
    uint64_t dispatched_guest;
    uint64_t dispatched_native;
    size_t dispatched_native_size;
    uint64_t fault_guest_pc;
    uint64_t fault_arch_pc;
    uint64_t fault_indirect_ic_guest;
    uint64_t fault_indirect_ic_native;
    uint32_t native_word;
    bool native_word_valid;
    bool active_guard_claim;
    uint8_t aa_dst_pre[32];
    uint8_t aa_src_pre[32];
    uint8_t aa_dst_post[32];
    uint8_t aa_src_post[32];
    uint32_t aa_dst_pre_valid;
    uint32_t aa_src_pre_valid;
    uint32_t aa_dst_post_valid;
    uint32_t aa_src_post_valid;
    bool aa_enabled;
    bool host_context_valid;
    int signal;
    sigjmp_buf env;
} hb_jit_signal_fault_frame_t;

static __thread hb_jit_signal_fault_frame_t* g_jit_signal_fault_frame;
static unsigned int g_jit_signal_fault_reports;

/* Defined next to block_guest_span below; block_cache_put tracks every newly
 * cached translation for SMC reverify (HK Mono/JIT stale-translation fix). */
static void smc_track_entry(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry,
                            const hb_ir_block_t* block);
static unsigned int g_jit_aa_sigbus_reports;
static unsigned int g_jit_sigill_ownership_reports;

static uint64_t g_dispatch_stats_blocks;
static uint64_t g_dispatch_stats_dispatches;
static uint64_t g_dispatch_stats_steps;
static uint64_t g_dispatch_stats_start_ns;
static int g_dispatch_stats_atexit_registered;
static __thread uint64_t t_dispatch_stats_blocks;
static __thread uint64_t t_dispatch_stats_dispatches;
static __thread uint64_t t_dispatch_stats_steps;
static __thread uint64_t t_dispatch_stats_flushed_blocks;
static __thread uint64_t t_dispatch_stats_flushed_dispatches;
static __thread uint64_t t_dispatch_stats_flushed_steps;
static __thread uint64_t t_dispatch_stats_next_report;

static uint32_t jit_block_step_count(const hb_ir_block_t* block);
static void trace_jit_code_cache_full_once(hb_jit_runtime_t* rt,
                                           const char* reason,
                                           size_t needed);
static int runtime_block_chain_enabled(void);
static int runtime_single_lookup_enabled(void);
static int runtime_indirect_ic_enabled(void);

static int translation_cache_trace_enabled(void) {
    return hb_contract_telemetry_enabled();
}

static void translation_cache_trace_summary(void) {
    (void)hb_contract_telemetry_emit_summary(stderr);
}

static void translation_cache_register_atexit(void) {
    hb_contract_telemetry_register_atexit();
}

static uint64_t runtime_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int trace_dispatch_stats_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_DISPATCH_STATS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static void dispatch_stats_flush_thread(int force);

static void dispatch_stats_summary(void) {
    uint64_t blocks, dispatches, steps, start, now, elapsed;
    double seconds;
    if (!trace_dispatch_stats_enabled()) return;
    dispatch_stats_flush_thread(1);
    blocks = __atomic_load_n(&g_dispatch_stats_blocks, __ATOMIC_RELAXED);
    dispatches = __atomic_load_n(&g_dispatch_stats_dispatches, __ATOMIC_RELAXED);
    steps = __atomic_load_n(&g_dispatch_stats_steps, __ATOMIC_RELAXED);
    start = __atomic_load_n(&g_dispatch_stats_start_ns, __ATOMIC_RELAXED);
    now = runtime_now_ns();
    elapsed = (start && now > start) ? now - start : 0;
    seconds = elapsed ? (double)elapsed / 1000000000.0 : 0.0;
    fprintf(stderr,
            "macrunner-hb-dispatch-stats: wall_s=%.3f dispatches=%llu blocks=%llu steps=%llu "
            "dispatches_per_s=%.1f blocks_per_s=%.1f steps_per_s=%.1f\n",
            seconds, (unsigned long long)dispatches, (unsigned long long)blocks,
            (unsigned long long)steps,
            seconds > 0.0 ? (double)dispatches / seconds : 0.0,
            seconds > 0.0 ? (double)blocks / seconds : 0.0,
            seconds > 0.0 ? (double)steps / seconds : 0.0);
    fflush(stderr);
}

static void dispatch_stats_register(void) {
    uint64_t now;
    int expected = 0;
    if (!trace_dispatch_stats_enabled()) return;
    now = runtime_now_ns();
    if (now) {
        uint64_t zero = 0;
        (void)__atomic_compare_exchange_n(&g_dispatch_stats_start_ns, &zero, now, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
    if (__atomic_compare_exchange_n(&g_dispatch_stats_atexit_registered, &expected, 1,
                                    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        atexit(dispatch_stats_summary);
    }
}

static uint64_t dispatch_stats_report_interval(void) {
    static int parsed;
    static uint64_t interval;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_DISPATCH_STATS_INTERVAL");
        interval = env && *env ? strtoull(env, NULL, 0) : 1000000ull;
        if (interval < 1000ull) interval = 1000ull;
        parsed = 1;
    }
    return interval;
}

static void dispatch_stats_flush_thread(int force) {
    uint64_t add_blocks, add_dispatches, add_steps;
    uint64_t total_blocks, total_dispatches, total_steps, start, now, elapsed;
    double seconds;
    if (!trace_dispatch_stats_enabled()) return;
    if (!force && t_dispatch_stats_dispatches < t_dispatch_stats_next_report) return;

    add_blocks = t_dispatch_stats_blocks - t_dispatch_stats_flushed_blocks;
    add_dispatches = t_dispatch_stats_dispatches - t_dispatch_stats_flushed_dispatches;
    add_steps = t_dispatch_stats_steps - t_dispatch_stats_flushed_steps;
    if (!add_blocks && !add_dispatches && !add_steps && !force) return;

    total_blocks = add_blocks
        ? __atomic_add_fetch(&g_dispatch_stats_blocks, add_blocks, __ATOMIC_RELAXED)
        : __atomic_load_n(&g_dispatch_stats_blocks, __ATOMIC_RELAXED);
    total_dispatches = add_dispatches
        ? __atomic_add_fetch(&g_dispatch_stats_dispatches, add_dispatches, __ATOMIC_RELAXED)
        : __atomic_load_n(&g_dispatch_stats_dispatches, __ATOMIC_RELAXED);
    total_steps = add_steps
        ? __atomic_add_fetch(&g_dispatch_stats_steps, add_steps, __ATOMIC_RELAXED)
        : __atomic_load_n(&g_dispatch_stats_steps, __ATOMIC_RELAXED);
    t_dispatch_stats_flushed_blocks = t_dispatch_stats_blocks;
    t_dispatch_stats_flushed_dispatches = t_dispatch_stats_dispatches;
    t_dispatch_stats_flushed_steps = t_dispatch_stats_steps;

    if (!t_dispatch_stats_next_report)
        t_dispatch_stats_next_report = dispatch_stats_report_interval();
    while (t_dispatch_stats_next_report <= t_dispatch_stats_dispatches)
        t_dispatch_stats_next_report += dispatch_stats_report_interval();

    start = __atomic_load_n(&g_dispatch_stats_start_ns, __ATOMIC_RELAXED);
    now = runtime_now_ns();
    elapsed = (start && now > start) ? now - start : 0;
    seconds = elapsed ? (double)elapsed / 1000000000.0 : 0.0;
    fprintf(stderr,
            "macrunner-hb-dispatch-stats: wall_s=%.3f total_dispatches=%llu total_blocks=%llu total_steps=%llu "
            "dispatches_per_s=%.1f blocks_per_s=%.1f steps_per_s=%.1f "
            "thread_dispatches=%llu thread_blocks=%llu thread_steps=%llu\n",
            seconds, (unsigned long long)total_dispatches, (unsigned long long)total_blocks,
            (unsigned long long)total_steps,
            seconds > 0.0 ? (double)total_dispatches / seconds : 0.0,
            seconds > 0.0 ? (double)total_blocks / seconds : 0.0,
            seconds > 0.0 ? (double)total_steps / seconds : 0.0,
            (unsigned long long)t_dispatch_stats_dispatches,
            (unsigned long long)t_dispatch_stats_blocks,
            (unsigned long long)t_dispatch_stats_steps);
    fflush(stderr);
}

static void dispatch_stats_add(uint64_t dispatches, uint64_t blocks, uint64_t steps) {
    hb_contract_telemetry_record_dispatch(dispatches, blocks, steps);
    if (!trace_dispatch_stats_enabled()) return;
    dispatch_stats_register();
    if (!t_dispatch_stats_next_report)
        t_dispatch_stats_next_report = dispatch_stats_report_interval();
    t_dispatch_stats_dispatches += dispatches;
    t_dispatch_stats_blocks += blocks;
    t_dispatch_stats_steps += steps;
    dispatch_stats_flush_thread(0);
}

/* --- In-memory block cache helpers --- */
static size_t block_cache_hash(uint64_t addr) {
    return (size_t)((addr ^ (addr >> 32)) & (HB_BLOCK_CACHE_SIZE - 1));
}

static hb_block_cache_t* block_cache_create(void) {
    return calloc(1, sizeof(hb_block_cache_t));
}

static size_t block_cache_entry_index(const hb_block_cache_t* cache,
                                      const hb_block_cache_entry_t* entry) {
    if (!cache || !entry || entry < cache->entries ||
        entry >= cache->entries + HB_BLOCK_CACHE_SIZE)
        return SIZE_MAX;
    return (size_t)(entry - cache->entries);
}

static hb_block_chain_meta_t* block_cache_chain_meta(hb_block_cache_t* cache,
                                                     hb_block_cache_entry_t* entry,
                                                     bool create) {
    size_t idx;
    if (!cache || !entry) return NULL;
    idx = block_cache_entry_index(cache, entry);
    if (idx == SIZE_MAX) return NULL;
    if (!cache->chain_meta && create)
        cache->chain_meta = calloc(HB_BLOCK_CACHE_SIZE, sizeof(*cache->chain_meta));
    return cache->chain_meta ? &cache->chain_meta[idx] : NULL;
}

static const hb_block_chain_meta_t* block_cache_chain_meta_const(
    const hb_block_cache_t* cache, const hb_block_cache_entry_t* entry) {
    size_t idx;
    if (!cache || !entry || !cache->chain_meta) return NULL;
    idx = block_cache_entry_index(cache, entry);
    if (idx == SIZE_MAX) return NULL;
    return &cache->chain_meta[idx];
}

static hb_ir_block_t* block_clone_for_cache(const hb_ir_block_t* block) {
    hb_ir_block_t* copy;
    hb_ir_instr_t* instrs;

    if (!block) return NULL;
    copy = hb_ir_block_create(block->id, block->guest_addr);
    if (!copy) return NULL;
    if (block->instr_count > copy->instr_cap) {
        instrs = calloc(block->instr_count, sizeof(hb_ir_instr_t));
        if (!instrs) {
            hb_ir_block_destroy(copy);
            return NULL;
        }
        free(copy->instrs);
        copy->instrs = instrs;
        copy->instr_cap = block->instr_count;
    }
    if (block->instr_count)
        memcpy(copy->instrs, block->instrs, block->instr_count * sizeof(hb_ir_instr_t));
    copy->instr_count = block->instr_count;
    return copy;
}

static void block_cache_release_owned_block(hb_block_cache_entry_t* entry,
                                            const hb_ir_block_t* replacement) {
    if (!entry || !entry->owns_block || !entry->block || entry->block == replacement)
        return;
    hb_ir_block_destroy((hb_ir_block_t*)entry->block);
    entry->owns_block = false;
}

static void arm64_store_u32(uint8_t* p, uint32_t insn) {
    if (!p) return;
    p[0] = (uint8_t)(insn & 0xffu);
    p[1] = (uint8_t)((insn >> 8) & 0xffu);
    p[2] = (uint8_t)((insn >> 16) & 0xffu);
    p[3] = (uint8_t)((insn >> 24) & 0xffu);
}

static void block_cache_clear_icache(uint8_t* start, size_t len) {
    if (!start || !len) return;
    __builtin___clear_cache((char*)start, (char*)start + len);
}

static void block_cache_unchain_entry(hb_block_cache_t* cache,
                                      hb_block_cache_entry_t* entry) {
    static const uint32_t arm64_nop = 0xd503201fu;
    hb_block_chain_meta_t* meta;
    uint8_t* patch;

    meta = block_cache_chain_meta(cache, entry, false);
    if (!entry || !entry->native_code || !meta || !meta->target_code)
        return;
    if (meta->patch_offset + sizeof(uint32_t) > entry->native_size)
        return;
    patch = entry->native_code + meta->patch_offset;
    arm64_store_u32(patch, arm64_nop);
    if (meta->patch_offset + 2 * sizeof(uint32_t) <= entry->native_size)
        arm64_store_u32(patch + sizeof(uint32_t), arm64_nop);
    block_cache_clear_icache(patch, 2 * sizeof(uint32_t));
    memset(meta, 0, sizeof(*meta));
}

static void block_cache_unchain_references(hb_block_cache_t* cache, const uint8_t* target_code) {
    if (!cache || !target_code) return;
    if (cache->used_overflow) {
        for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
            hb_block_cache_entry_t* entry = &cache->entries[i];
            const hb_block_chain_meta_t* meta = block_cache_chain_meta_const(cache, entry);
            if (entry->valid && meta && meta->target_code == target_code)
                block_cache_unchain_entry(cache, entry);
        }
        return;
    }
    for (size_t i = 0; i < cache->used_count; i++) {
        hb_block_cache_entry_t* entry = &cache->entries[cache->used_slots[i]];
        const hb_block_chain_meta_t* meta = block_cache_chain_meta_const(cache, entry);
        if (entry->valid && meta && meta->target_code == target_code)
            block_cache_unchain_entry(cache, entry);
    }
}

static void block_cache_prepare_replace_entry(hb_jit_runtime_t* rt, hb_block_cache_t* cache,
                                              hb_block_cache_entry_t* entry) {
    int made_writable = 0;

    if (!entry || !entry->valid) return;
    if (runtime_block_chain_enabled() && rt && rt->jit_mem &&
        hb_jit_buffer_make_writable(rt->jit_mem) == HB_OK) {
        made_writable = 1;
        block_cache_unchain_references(cache, entry->native_code);
        block_cache_unchain_entry(cache, entry);
        (void)hb_jit_buffer_make_executable(rt->jit_mem);
    }
    if (!made_writable) {
        hb_block_chain_meta_t* meta = block_cache_chain_meta(cache, entry, false);
        if (meta) memset(meta, 0, sizeof(*meta));
    }
}

static void block_cache_destroy(hb_block_cache_t* cache) {
    if (!cache) return;
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++)
        block_cache_release_owned_block(&cache->entries[i], NULL);
    free(cache->used_slots);
    free(cache->chain_meta);
    free(cache);
}

/* MacRunner: eager reset for per-thread runtime reuse. Free every owned cloned
 * block (zero UAF risk — no cross-generation lazy free) and clear all entries so
 * the next callback regenerates translations from current guest code.
 *
 * Lever #3: only the slots occupied this generation (tracked in used_slots) can have
 * valid==true / owns_block, so clearing just those is equivalent to the old full-table
 * memset but O(count) instead of O(524288). Post-condition is identical: every slot
 * valid==false, no owned block leaked, count==0. used_overflow keeps the old full clear
 * as a safety net when the tracking array could not grow. */
static void block_cache_reset(hb_block_cache_t* cache) {
    bool unchain;
    if (!cache) return;
    unchain = runtime_block_chain_enabled();
    if (cache->used_overflow) {
        for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
            if (unchain) block_cache_unchain_entry(cache, &cache->entries[i]);
            block_cache_release_owned_block(&cache->entries[i], NULL);
        }
        memset(cache->entries, 0, sizeof(cache->entries));
        if (cache->chain_meta)
            memset(cache->chain_meta, 0,
                   HB_BLOCK_CACHE_SIZE * sizeof(*cache->chain_meta));
        cache->used_overflow = false;
    } else {
        for (size_t i = 0; i < cache->used_count; i++) {
            size_t slot = cache->used_slots[i];
            hb_block_cache_entry_t* e = &cache->entries[slot];
            if (unchain) block_cache_unchain_entry(cache, e);
            block_cache_release_owned_block(e, NULL);
            memset(e, 0, sizeof(*e));
            if (cache->chain_meta)
                memset(&cache->chain_meta[slot], 0, sizeof(cache->chain_meta[slot]));
        }
    }
    cache->used_count = 0;
    cache->count = 0;
}

static hb_block_cache_entry_t* block_cache_find(hb_block_cache_t* cache, uint64_t addr) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) return NULL;
        if (cache->entries[probe].guest_addr == addr) return &cache->entries[probe];
    }
    return NULL;
}

/* A chained block may fault after leaving the entry protected by the guard.
 * Resolve its actual owner after siglongjmp, outside signal context. */
static hb_block_cache_entry_t* block_cache_find_native_pc(hb_block_cache_t* cache,
                                                          uint64_t native_pc) {
    if (!cache || !native_pc) return NULL;
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        hb_block_cache_entry_t* entry = &cache->entries[i];
        uintptr_t start, end;
        if (!entry->valid || !entry->native_code || !entry->native_size) continue;
        start = (uintptr_t)entry->native_code;
        end = start + entry->native_size;
        if (end >= start && (uintptr_t)native_pc >= start && (uintptr_t)native_pc < end)
            return entry;
    }
    return NULL;
}

int hb_jit_runtime_native_block_info(hb_jit_runtime_t* rt, uint64_t native_pc,
                                     uint64_t* guest_addr, uint64_t* native_start,
                                     size_t* native_size) {
    hb_block_cache_entry_t* entry;

    if (guest_addr) *guest_addr = 0;
    if (native_start) *native_start = 0;
    if (native_size) *native_size = 0;
    if (!rt || !native_pc) return 0;
    entry = block_cache_find_native_pc(rt->block_cache, native_pc);
    if (!entry) return 0;
    if (guest_addr) *guest_addr = entry->guest_addr;
    if (native_start) *native_start = (uint64_t)(uintptr_t)entry->native_code;
    if (native_size) *native_size = entry->native_size;
    return 1;
}

static bool block_cache_is_full(const hb_block_cache_t* cache) {
    return cache && cache->count >= HB_BLOCK_CACHE_SIZE;
}

static hb_block_cache_entry_t* block_cache_put(hb_jit_runtime_t* rt, hb_block_cache_t* cache,
                                               uint64_t addr, uint8_t* code,
                                               size_t size, uint32_t steps,
                                               const hb_ir_block_t* block, bool fused,
                                               bool owns_block) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) {
            cache->entries[probe].guest_addr = addr;
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            cache->entries[probe].hit_count = 0;
            if (cache->chain_meta)
                memset(&cache->chain_meta[probe], 0, sizeof(cache->chain_meta[probe]));
            cache->entries[probe].block = block;
            cache->entries[probe].owns_block = owns_block;
            cache->entries[probe].fused = fused;
            cache->entries[probe].valid = true;
            cache->count++;
            /* lever #3: remember this newly-occupied slot so block_cache_reset clears only
             * used slots. Only this new-insert branch sets valid=true, so recording here
             * captures every occupied slot exactly once per generation. */
            if (cache->used_count >= cache->used_cap) {
                size_t ncap = cache->used_cap ? cache->used_cap * 2 : 256;
                uint32_t* n = realloc(cache->used_slots, ncap * sizeof(*n));
                if (n) { cache->used_slots = n; cache->used_cap = ncap; }
            }
            if (cache->used_count < cache->used_cap)
                cache->used_slots[cache->used_count++] = (uint32_t)probe;
            else
                cache->used_overflow = true;  /* tracking full -> reset does the safe full memset */
            smc_track_entry(rt, &cache->entries[probe], block);
            hb_contract_telemetry_record_translation(true);
            return &cache->entries[probe];
        }
        if (cache->entries[probe].guest_addr == addr) {
            /* Update existing entry */
            bool keep_existing_owner = cache->entries[probe].owns_block &&
                                       cache->entries[probe].block == block;
            if (runtime_block_chain_enabled())
                block_cache_prepare_replace_entry(rt, cache, &cache->entries[probe]);
            block_cache_release_owned_block(&cache->entries[probe], block);
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            if (cache->chain_meta)
                memset(&cache->chain_meta[probe], 0, sizeof(cache->chain_meta[probe]));
            cache->entries[probe].block = block;
            cache->entries[probe].owns_block = owns_block || keep_existing_owner;
            cache->entries[probe].fused = fused;
            smc_track_entry(rt, &cache->entries[probe], block);
            hb_contract_telemetry_record_translation(false);
            return &cache->entries[probe];
        }
    }
    return NULL;
}

static int runtime_env_enabled(const char* name) {
    const char* val = getenv(name);
    return val && *val && *val != '0';
}

static int runtime_env_flag_cached(int* cache, const char* name, int default_value) {
    int value = __atomic_load_n(cache, __ATOMIC_RELAXED);
    if (value < 0) {
        const char* env = getenv(name);
        value = (env && *env) ? (*env != '0') : default_value;
        __atomic_store_n(cache, value, __ATOMIC_RELAXED);
    }
    return value;
}

static int runtime_env_enabled_default_on(const char* name) {
    const char* val = getenv(name);
    return !val || !*val || *val != '0';
}

static int runtime_block_chain_enabled(void) {
    static int cached = -1;
    /* UNSAFE: single-slot tail patch, needs chain-entry trampoline — see report.
     * Keep MACRUNNER_HB_BLOCK_CHAIN default-off until the redesign lands. */
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_BLOCK_CHAIN", 0);
}

static int runtime_single_lookup_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SINGLE_LOOKUP", 0);
}

static int runtime_indirect_ic_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_INDIRECT_IC", 0);
}

static int trace_dispatch_gate_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_TRACE_DISPATCH_GATE", 0);
}

static int runtime_direct_scalar_scan_enabled_for_key(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN");
    if (val && *val)
        return *val != '0';
    return runtime_env_enabled_default_on("MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN");
}

static int runtime_direct_scalar_mem_enabled_for_key(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_SCALAR_MEM");
    if (val && *val)
        return *val != '0';
    return runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_MEM");
}

static uint8_t runtime_jit_flags(void) {
    uint8_t flags = 0;
    if (runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_MEM"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_MEM;
    if (runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_STACK"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_STACK;
    if (runtime_direct_scalar_scan_enabled_for_key())
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_SCAN;
    if (runtime_direct_scalar_mem_enabled_for_key())
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_MEM;
    if (runtime_block_chain_enabled())
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_BLOCK_CHAIN;
    if (runtime_indirect_ic_enabled())
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_INDIRECT_IC;
    if (runtime_env_enabled("MACRUNNER_HB_NATIVE_MEMMOVE"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_NATIVE_MEMMOVE;
    return flags;
}

static uint8_t macrunner_hb_runtime_persistent_cache_flags;

void hb_runtime_init_environment(void) {
    macrunner_hb_runtime_persistent_cache_flags = runtime_jit_flags();
}

static bool native_blob_has_helper_call(const uint8_t* code, size_t size) {
    if (!code) return true;
    for (size_t i = 0; i + sizeof(uint32_t) <= size; i += sizeof(uint32_t)) {
        uint32_t insn;
        memcpy(&insn, code + i, sizeof(insn));
        if ((insn & 0xfffffc1fu) == 0xd63f0000u) return true; /* BLR Xn */
    }
    return false;
}

static void* helper_addr_for_cache_id(uint8_t id) {
    switch (id) {
        case 1: return (void*)hb_jit_helper_exec_ir_block;
        case 2: return (void*)hb_jit_helper_exec_load_cmp_jcc_block;
        case 3: return (void*)hb_jit_helper_exec_cmp_setcc_ret_block;
        case 4: return (void*)hb_jit_helper_exec_unity_string_bsearch_loop;
        case 5: return (void*)hb_jit_helper_exec_unity_freelist_fill_loop;
        case 6: return (void*)hb_jit_helper_exec_unity_u32_ptr_compare;
        case 7: return (void*)hb_jit_helper_exec_mono_metadata_bsearch_loop;
        case 8: return (void*)hb_jit_helper_exec_mono_string_hash;
        case 9: return (void*)hb_jit_helper_exec_mono_string_equal;
        case 10: return (void*)hb_jit_helper_exec_mono_metadata_rowptr_entry;
        case 11: return (void*)hb_jit_helper_exec_mono_metadata_decode_row_loop;
        case 12: return (void*)hb_jit_helper_exec_mono_metadata_decode_row_entry;
        case 13: return (void*)hb_jit_helper_exec_mono_metadata_decode_col;
        case 14: return (void*)hb_jit_helper_exec_mono_metadata_coded_index_search;
        case 15: return (void*)hb_jit_helper_exec_interp_ir;
        case 16: return (void*)hb_jit_helper_exec_load_operand_lazy;
        case 17: return (void*)hb_jit_helper_exec_store_operand_lazy;
        case 18: return (void*)hb_jit_helper_exec_call_operand;
        case 19: return (void*)hb_jit_helper_exec_xfg_dispatch_call;
        case 20: return (void*)hb_jit_helper_exec_jmp_operand;
        case 21: return (void*)hb_jit_helper_exec_cmp_test_operand_lazy;
        case 22: return (void*)hb_jit_helper_exec_binop_operand_lazy;
        case 23: return (void*)hb_jit_helper_exec_mul_div_operand;
        case 24: return (void*)hb_jit_helper_exec_double_shift_operand;
        case 25: return (void*)hb_jit_helper_exec_extend_operand_lazy;
        case 26: return (void*)hb_jit_helper_exec_mov_operand_lazy;
        case 27: return (void*)hb_jit_helper_exec_not_operand_lazy;
        case 28: return (void*)hb_jit_helper_exec_neg_operand_lazy;
        case 29: return (void*)hb_jit_helper_exec_bit_scan;
        case 30: return (void*)hb_jit_helper_exec_loop_branch;
        case 31: return (void*)hb_jit_helper_try_native_memmove;
        default: return NULL;
    }
}

static uint8_t helper_cache_id_for_addr(uint64_t addr) {
    for (uint8_t id = 1; id <= 31; id++) {
        if ((uintptr_t)helper_addr_for_cache_id(id) == (uintptr_t)addr) return id;
    }
    return 0;
}

static bool helper_cache_id_uses_instr_arg1(uint8_t id) {
    return id >= 15 && id <= 30;
}

static bool arm64_mov_imm64_at(const uint8_t* code, size_t size, size_t off,
                               int rd, uint64_t* value) {
    uint32_t insn[4];
    uint64_t v;
    if (!code || off + sizeof(insn) > size || rd < 0 || rd > 31) return false;
    memcpy(&insn[0], code + off, 4);
    memcpy(&insn[1], code + off + 4, 4);
    memcpy(&insn[2], code + off + 8, 4);
    memcpy(&insn[3], code + off + 12, 4);
    if ((insn[0] & 0xffe0001fu) != (0xd2800000u | (uint32_t)rd) ||
        (insn[1] & 0xffe0001fu) != (0xf2a00000u | (uint32_t)rd) ||
        (insn[2] & 0xffe0001fu) != (0xf2c00000u | (uint32_t)rd) ||
        (insn[3] & 0xffe0001fu) != (0xf2e00000u | (uint32_t)rd))
        return false;
    v = ((uint64_t)((insn[0] >> 5) & 0xffffu)) |
        ((uint64_t)((insn[1] >> 5) & 0xffffu) << 16) |
        ((uint64_t)((insn[2] >> 5) & 0xffffu) << 32) |
        ((uint64_t)((insn[3] >> 5) & 0xffffu) << 48);
    if (value) *value = v;
    return true;
}

static void arm64_patch_mov_imm64_at(uint8_t* code, size_t size, size_t off,
                                     int rd, uint64_t value) {
    uint32_t insn[4];
    if (!code || off + sizeof(insn) > size || rd < 0 || rd > 31) return;
    insn[0] = 0xd2800000u | (uint32_t)(((value >> 0) & 0xffffu) << 5) | (uint32_t)rd;
    insn[1] = 0xf2a00000u | (uint32_t)(((value >> 16) & 0xffffu) << 5) | (uint32_t)rd;
    insn[2] = 0xf2c00000u | (uint32_t)(((value >> 32) & 0xffffu) << 5) | (uint32_t)rd;
    insn[3] = 0xf2e00000u | (uint32_t)(((value >> 48) & 0xffffu) << 5) | (uint32_t)rd;
    memcpy(code + off, &insn[0], 4);
    memcpy(code + off + 4, &insn[1], 4);
    memcpy(code + off + 8, &insn[2], 4);
    memcpy(code + off + 12, &insn[3], 4);
}

/* MacRunner 2026-07-29: count `blr x23` sites — helper calls — in emitted code.
 *
 * native_blob_single_arg_helper_stub() below bails on the SECOND one, so a block with two
 * helper calls can never be persisted. On HK that restriction shows up as stores=117 against
 * store_skips=40495: the persistent cache retains 0.3 % of what it compiles, and a cold start
 * therefore needs 476 s to reach the menu where Rosetta needs under 45 s. This exists purely to
 * attribute the skips, so the decision to generalise the patcher rests on a number rather than
 * on the assumption that multi-helper blocks are the common case. */
static size_t native_blob_helper_call_count(const uint8_t* code, size_t size) {
    size_t count = 0;
    if (!code) return 0;
    for (size_t off = 0; off + 4 <= size; off += 4) {
        uint32_t insn;
        memcpy(&insn, code + off, sizeof(insn));
        if (insn == (0xd63f0000u | (23u << 5))) count++;
    }
    return count;
}

static bool native_blob_single_arg_helper_stub(const uint8_t* code, size_t size,
                                               const hb_ir_block_t* block,
                                               bool canonical,
                                               hb_cached_helper_stub_t* out) {
    size_t blr_off = SIZE_MAX;
    size_t arg1_off = SIZE_MAX;
    uint8_t helper_id = 0;
    size_t arg1_count = 0;
    uint64_t helper_value = 0;
    uint16_t instr_index = 0;
    bool arg1_is_instr = false;

    if (!code || !size || !block) return false;
    for (size_t off = 0; off + 4 <= size; off += 4) {
        uint32_t insn;
        memcpy(&insn, code + off, sizeof(insn));
        if (insn == (0xd63f0000u | (23u << 5))) {
            if (blr_off != SIZE_MAX) return false;
            blr_off = off;
        }
    }
    if (blr_off == SIZE_MAX || blr_off < 16) return false;
    if (!arm64_mov_imm64_at(code, size, blr_off - 16, 23, &helper_value)) return false;
    if (canonical) {
        if ((helper_value & HB_RUNTIME_CACHE_HELPER_MASK) != HB_RUNTIME_CACHE_HELPER_SENTINEL)
            return false;
        helper_id = (uint8_t)(helper_value & 0xffu);
        if (!helper_addr_for_cache_id(helper_id)) return false;
    } else {
        helper_id = helper_cache_id_for_addr(helper_value);
        if (!helper_id) return false;
    }

    for (size_t off = 0; off + 16 <= size; off += 4) {
        uint64_t value = 0;
        if (arm64_mov_imm64_at(code, size, off, 2, &value) ||
            arm64_mov_imm64_at(code, size, off, 3, &value) ||
            arm64_mov_imm64_at(code, size, off, 4, &value))
            return false;
        if (arm64_mov_imm64_at(code, size, off, 1, &value)) {
            if (helper_cache_id_uses_instr_arg1(helper_id)) {
                if (canonical) {
                    if ((value & HB_RUNTIME_CACHE_INSTR_MASK) == HB_RUNTIME_CACHE_INSTR_SENTINEL) {
                        uint64_t idx = value & ~HB_RUNTIME_CACHE_INSTR_MASK;
                        if (idx >= block->instr_count || idx > UINT16_MAX) return false;
                        instr_index = (uint16_t)idx;
                        arg1_is_instr = true;
                        arg1_off = off;
                        arg1_count++;
                    }
                } else {
                    for (uint32_t idx = 0; idx < block->instr_count; idx++) {
                        if (value == (uint64_t)(uintptr_t)&block->instrs[idx]) {
                            instr_index = (uint16_t)idx;
                            arg1_is_instr = true;
                            arg1_off = off;
                            arg1_count++;
                            break;
                        }
                    }
                }
            } else {
                uint64_t expected = canonical ? HB_RUNTIME_CACHE_BLOCK_SENTINEL
                                              : (uint64_t)(uintptr_t)block;
                if (value == expected) {
                    arg1_off = off;
                    arg1_count++;
                }
            }
        }
    }
    if (arg1_count != 1 || arg1_off == SIZE_MAX) return false;
    if (out) {
        out->arg1_mov_off = arg1_off;
        out->helper_mov_off = blr_off - 16;
        out->helper_id = helper_id;
        out->instr_index = instr_index;
        out->arg1_is_instr = arg1_is_instr;
    }
    return true;
}

static bool native_blob_prepare_cache_store(const uint8_t* code, size_t size,
                                            const hb_ir_block_t* block,
                                            const uint8_t** out_code,
                                            uint8_t** owned_code) {
    hb_cached_helper_stub_t stub;
    uint8_t* patched;
    if (!code || !size || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;
    if (!native_blob_has_helper_call(code, size)) return true;
    if (!native_blob_single_arg_helper_stub(code, size, block, false, &stub))
        return false;
    patched = malloc(size);
    if (!patched) return false;
    memcpy(patched, code, size);
    arm64_patch_mov_imm64_at(patched, size, stub.arg1_mov_off, 1,
                             stub.arg1_is_instr
                                 ? (HB_RUNTIME_CACHE_INSTR_SENTINEL | stub.instr_index)
                                 : HB_RUNTIME_CACHE_BLOCK_SENTINEL);
    arm64_patch_mov_imm64_at(patched, size, stub.helper_mov_off, 23,
                             HB_RUNTIME_CACHE_HELPER_SENTINEL | stub.helper_id);
    *out_code = patched;
    *owned_code = patched;
    return true;
}

static bool native_blob_prepare_cache_load(const uint8_t* code, size_t size,
                                           const hb_ir_block_t* block,
                                           const uint8_t** out_code,
                                           uint8_t** owned_code) {
    hb_cached_helper_stub_t stub;
    uint8_t* patched;
    if (!code || !size || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;
    if (!native_blob_has_helper_call(code, size)) return true;
    if (!native_blob_single_arg_helper_stub(code, size, block, true, &stub))
        return false;
    patched = malloc(size);
    if (!patched) return false;
    memcpy(patched, code, size);
    arm64_patch_mov_imm64_at(patched, size, stub.arg1_mov_off, 1,
                             stub.arg1_is_instr
                                 ? (uint64_t)(uintptr_t)&block->instrs[stub.instr_index]
                                 : (uint64_t)(uintptr_t)block);
    arm64_patch_mov_imm64_at(patched, size, stub.helper_mov_off, 23,
                             (uint64_t)(uintptr_t)helper_addr_for_cache_id(stub.helper_id));
    *out_code = patched;
    *owned_code = patched;
    return true;
}

static bool block_guest_span(const hb_ir_block_t* block, uint64_t* start, size_t* len) {
    uint64_t lo, hi;
    uint32_t steps;
    if (!block || !block->instr_count || !start || !len) return false;
    steps = jit_block_step_count(block);
    if (!steps || steps > block->instr_count) return false;
    lo = block->instrs[0].guest_addr;
    hi = lo;
    for (uint32_t i = 0; i < steps; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        uint64_t end = instr->guest_addr + instr->guest_len;
        if (instr->guest_addr < lo) lo = instr->guest_addr;
        if (end > hi) hi = end;
    }
    if (hi <= lo || hi - lo > 4096u) return false;
    *start = lo;
    *len = (size_t)(hi - lo);
    return true;
}

/* ---- SMC (self-modifying code) translation reverify ----------------------
 * MacRunner 2026-07-27 — HK Mono/JIT stale-translation fix.
 * The in-memory block cache was keyed purely by guest address: a cached
 * translation kept executing even after the guest (Mono's JIT) rewrote the
 * underlying bytes, because no path — guest write, NtProtectVirtualMemory,
 * NtFlushInstructionCache — ever invalidated it (see
 * reports/phase4-hollow-knight/MONO-SMC-STALE-TRANSLATION-MECHANISM-20260727.md).
 * Fix: for blocks whose guest span is writable+executable (the only spans
 * that CAN change — Mono/JIT code heaps are RWX), hash the guest bytes at
 * translation time and re-verify on every cache hit; a mismatch evicts the
 * entry so the dispatch loop retranslates from current bytes.  Static RX
 * code stays untracked and pays nothing.  Default ON; kill switch
 * MACRUNNER_HB_SMC_REVERIFY=0; diagnostics MACRUNNER_HB_TRACE_SMC_REVERIFY=1. */

static int smc_reverify_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SMC_REVERIFY", 1);
}

static int smc_trace_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_TRACE_SMC_REVERIFY", 0);
}

static uint64_t smc_fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static uint64_t g_smc_tracked, g_smc_reverified, g_smc_evicted, g_smc_unreadable;

void hb_jit_smc_reverify_stats(uint64_t* tracked, uint64_t* reverified,
                               uint64_t* evicted, uint64_t* unreadable) {
    if (tracked) *tracked = g_smc_tracked;
    if (reverified) *reverified = g_smc_reverified;
    if (evicted) *evicted = g_smc_evicted;
    if (unreadable) *unreadable = g_smc_unreadable;
}

/* Hash the CURRENT guest bytes of [start, start+len).  Returns 0 when the
 * span is unreadable — treated as unverifiable, never evicts blind. */
static uint64_t smc_hash_current(hb_jit_runtime_t* rt, uint64_t start, size_t len) {
    uint8_t bytes[4096];
    if (!rt || !rt->ctx || !rt->ctx->memory || !len || len > sizeof(bytes)) return 0;
    if (hb_memory_read(rt->ctx->memory, start, bytes, len) != HB_OK) return 0;
    return smc_fnv1a(bytes, len);
}

static void smc_track_entry(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry,
                            const hb_ir_block_t* block) {
    uint64_t start = 0;
    size_t len = 0;
    hb_region_t* region;
    uint64_t h;
    if (!entry) return;
    entry->smc_hash = 0;
    entry->smc_span_start = 0;
    entry->smc_span_len = 0;
    if (!smc_reverify_enabled() || !rt || !rt->ctx || !rt->ctx->memory || !block)
        return;
    if (!block_guest_span(block, &start, &len)) return;
    region = hb_memory_find_region(rt->ctx->memory, start);
    if (!region || start + len > region->base + region->size) return;
    if ((region->perm & (HB_PERM_WRITE | HB_PERM_EXEC)) != (HB_PERM_WRITE | HB_PERM_EXEC))
        return;  /* static RX code cannot change under us: leave untracked */
    h = smc_hash_current(rt, start, len);
    if (!h) { g_smc_unreadable++; return; }
    entry->smc_span_start = start;
    entry->smc_span_len = (uint32_t)len;
    entry->smc_hash = h;
    g_smc_tracked++;
}

static void block_cache_evict_entry(hb_jit_runtime_t* rt, hb_block_cache_t* cache,
                                    hb_block_cache_entry_t* entry) {
    size_t idx;
    if (!cache || !entry || !entry->valid) return;
    idx = block_cache_entry_index(cache, entry);
    if (runtime_block_chain_enabled())
        block_cache_prepare_replace_entry(rt, cache, entry);
    block_cache_release_owned_block(entry, NULL);
    memset(entry, 0, sizeof(*entry));
    if (cache->chain_meta && idx != SIZE_MAX)
        memset(&cache->chain_meta[idx], 0, sizeof(cache->chain_meta[idx]));
    if (cache->count) cache->count--;
}

/* Returns the entry to dispatch, or NULL when the cached translation no
 * longer matches the current guest bytes (entry evicted; caller falls
 * through to the translate path). */
/* MacRunner 2026-07-28 — SMC re-lift gate.  Evicting the block-cache entry is only
 * HALF the invalidation: the lifted IR that the dispatch loop falls back to
 * (find_block(func->cfg, pc)) comes from macrunner_hb.c's address-keyed ir_cache,
 * which has no byte validation, so retranslating in place recompiles the SAME stale
 * IR and re-tracks it against the NEW bytes — permanently stale, silently.  See
 * reports/phase4-hollow-knight/HK-SMC-STALE-IR-CACHE-SECOND-CACHE-GAP-20260728.md.
 * With this gate an eviction EXITS the dispatch (like the "no block for PC" exit) so
 * the caller re-lifts from current bytes.  Kill switch MACRUNNER_HB_SMC_RELIFT=0. */
static int smc_relift_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SMC_RELIFT", 1);
}

static uint64_t g_smc_relift_exits;
static uint64_t g_smc_relift_suppressed;

uint64_t hb_jit_smc_relift_exits(void) { return g_smc_relift_exits; }
uint64_t hb_jit_smc_relift_suppressed(void) { return g_smc_relift_suppressed; }

/* MacRunner 2026-07-28 — LIVELOCK GUARD on the re-lift exit.
 * An eviction is only *believed* to mean "the guest rewrote this code".  A hash
 * mismatch can also arise without any guest write (the span's host backing moved,
 * an overlapping remap, an unstable read).  If such a mismatch reproduces after the
 * re-lift, the outer loop would exit → re-lift → exit forever with ZERO steps
 * executed: a thread that is alive, running and never advancing — precisely the bug
 * this fix exists to remove.  NOT observed; this is insurance against an unproven
 * failure mode, and it is deliberately cheap.  hb_test_runner cannot exercise it at
 * all (whole-suite totals: evicted=0 relift_exits=0), so the exit path is unvalidated
 * by unit test — see HK-SMC-RELIFT-UNIT-CONTROL-INCONCLUSIVE-20260728.md.
 *
 * Guard: only ZERO-PROGRESS exits accumulate a streak.  Any exit that follows real
 * forward progress (steps > 0) resets it, so a legitimate rewrite-heavy workload —
 * Mono patching call sites between blocks — is never throttled.  After
 * SMC_RELIFT_MAX_CONSECUTIVE zero-progress exits we stop exiting and fall through to
 * the pre-fix in-place retranslate: degraded (possibly stale) rather than wedged. */
#define SMC_RELIFT_MAX_CONSECUTIVE 8
static __thread unsigned g_smc_relift_streak;

static bool smc_relift_should_exit(uint64_t steps) {
    if (!smc_relift_enabled()) return false;
    if (steps > 0) {              /* progress was made: this is not a livelock */
        g_smc_relift_streak = 0;
        g_smc_relift_exits++;
        return true;
    }
    if (g_smc_relift_streak >= SMC_RELIFT_MAX_CONSECUTIVE) {
        g_smc_relift_suppressed++;
        return false;
    }
    g_smc_relift_streak++;
    g_smc_relift_exits++;
    return true;
}

static hb_block_cache_entry_t* smc_reverify_entry(hb_jit_runtime_t* rt,
                                                  hb_block_cache_entry_t* entry,
                                                  bool* evicted) {
    uint64_t now;
    if (evicted) *evicted = false;
    if (!entry || !entry->smc_hash) return entry;
    g_smc_reverified++;
    /* Positive-liveness aggregate: proves the instrument is executing even when
     * nothing is ever evicted ("not logged" != "did not happen").  Bounded:
     * first track, 1000th track, then every 2^24 reverifications. */
    if (smc_trace_enabled() &&
        (g_smc_reverified == 1 || g_smc_reverified == 1000 ||
         (g_smc_reverified & 0xffffffu) == 0))
        fprintf(stderr,
                "macrunner-hb-smc-reverify: progress tracked=%llu reverified=%llu "
                "evicted=%llu unreadable=%llu\n",
                (unsigned long long)g_smc_tracked, (unsigned long long)g_smc_reverified,
                (unsigned long long)g_smc_evicted, (unsigned long long)g_smc_unreadable);
    now = smc_hash_current(rt, entry->smc_span_start, entry->smc_span_len);
    if (!now) { g_smc_unreadable++; return entry; }
    if (now == entry->smc_hash) return entry;
    g_smc_evicted++;
    if (smc_trace_enabled() &&
        (g_smc_evicted <= 16 || (g_smc_evicted & 0xffffu) == 0))
        fprintf(stderr,
                "macrunner-hb-smc-reverify: evict guest=0x%llx span=%u old=%016llx "
                "new=%016llx evicted=%llu reverified=%llu\n",
                (unsigned long long)entry->guest_addr, entry->smc_span_len,
                (unsigned long long)entry->smc_hash, (unsigned long long)now,
                (unsigned long long)g_smc_evicted, (unsigned long long)g_smc_reverified);
    block_cache_evict_entry(rt, rt->block_cache, entry);
    if (evicted) *evicted = true;
    return NULL;
}

static hb_result_t persistent_cache_key_for_block(hb_jit_runtime_t* rt,
                                                  const hb_ir_block_t* block,
                                                  hb_cache_key_t* key) {
    uint64_t start = 0;
    size_t len = 0;
    uint8_t bytes[4096];
    hb_result_t r;
    if (!rt || !rt->ctx || !block || !key) return HB_ERR_INVALID_ARG;
    if (!block_guest_span(block, &start, &len)) return HB_ERR_UNSUPPORTED_FEATURE;
    r = hb_memory_read(rt->ctx->memory, start, bytes, len);
    if (r != HB_OK) return r;
    r = hb_cache_key_compute(bytes, len, rt->ctx->arch, HB_RUNTIME_PERSISTENT_CACHE_VERSION, key);
    if (r != HB_OK) return r;
    key->guest_addr = block->guest_addr;
    key->mode = (uint8_t)rt->ctx->mode;
    key->backend = (uint8_t)HB_BACKEND_JIT;
    key->flags = rt->persistent_cache_flags;
    return HB_OK;
}

static hb_result_t jit_commit_blob(hb_jit_runtime_t* rt, const uint8_t* code, size_t size,
                                   uint8_t** out_dest) {
    hb_result_t r;
    uint8_t* dest;
    if (!rt || !rt->jit_mem || !code || !size || !out_dest) return HB_ERR_INVALID_ARG;
    if (rt->jit_mem->used + size > rt->jit_mem->size) {
        rt->code_cache_full = true;
        trace_jit_code_cache_full_once(rt, "jit-buffer-full", size);
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    r = hb_jit_buffer_make_writable(rt->jit_mem);
    if (r != HB_OK) return r;
    dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code, size);
    rt->jit_mem->used += size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    r = hb_jit_buffer_commit(rt->jit_mem);
    if (r != HB_OK) return r;
    *out_dest = dest;
    return HB_OK;
}

static int trace_jit_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static int trace_jit_helper_fault_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HELPER_FAIL");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static int trace_x86_low_pc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_X86_LOW_PC");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_native_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_native_range_start(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_RANGE_START");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_native_range_end(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_RANGE_END");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_addr2(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_ADDR2");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_range_start(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_START");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_range_end(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_END");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static int trace_jit_blocks_budget_allows(int force) {
    static int count;
    static int exhausted;
    const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCK_BUDGET");
    int limit = env && *env ? atoi(env) : 2000;
    if (!trace_jit_blocks_enabled()) return 0;
    if (force || limit <= 0) return 1;
    if (count < limit) {
        count++;
        return 1;
    }
    if (!exhausted) {
        exhausted = 1;
        fprintf(stderr, "macrunner-hb-jit-block: trace budget exhausted at %d entries, silencing\n", limit);
        fflush(stderr);
    }
    return 0;
}

static void trace_jit_block(uint64_t guest_pc, const uint8_t* native, size_t native_size,
                            const hb_ir_block_t* block) {
    uint64_t watch = trace_jit_native_addr();
    uint64_t range_start = trace_jit_native_range_start();
    uint64_t range_end = trace_jit_native_range_end();
    uint64_t guest_watch = trace_jit_guest_addr();
    uint64_t guest_watch2 = trace_jit_guest_addr2();
    uint64_t guest_range_start = trace_jit_guest_range_start();
    uint64_t guest_range_end = trace_jit_guest_range_end();
    int matched = watch && (uintptr_t)native <= (uintptr_t)watch &&
                  (uintptr_t)watch < (uintptr_t)native + native_size;
    int range_matched = range_start && range_end && range_start < range_end &&
                        (uintptr_t)native < (uintptr_t)range_end &&
                        (uintptr_t)native + native_size > (uintptr_t)range_start;
    int guest_range_matched = guest_range_start && guest_range_end &&
                              guest_range_start < guest_range_end &&
                              guest_pc < guest_range_end;
    int guest_matched = (guest_watch && guest_pc == guest_watch) ||
                        (guest_watch2 && guest_pc == guest_watch2);
    const hb_ir_instr_t* first = (block && block->instr_count) ? &block->instrs[0] : NULL;
    const hb_ir_instr_t* last = (block && block->instr_count) ?
                                &block->instrs[block->instr_count - 1] : NULL;

    if (!guest_matched && (guest_watch || guest_watch2) && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            uint64_t start = instr->guest_addr;
            uint64_t end = start + instr->guest_len;
            if ((guest_watch && (guest_watch == start || (instr->guest_len && guest_watch >= start && guest_watch < end))) ||
                (guest_watch2 && (guest_watch2 == start || (instr->guest_len && guest_watch2 >= start && guest_watch2 < end)))) {
                guest_matched = 1;
                break;
            }
        }
    }
    if (guest_range_matched && block) {
        guest_range_matched = 0;
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            uint64_t start = instr->guest_addr;
            uint64_t end = start + (instr->guest_len ? instr->guest_len : 1);
            if (start < guest_range_end && end > guest_range_start) {
                guest_range_matched = 1;
                break;
            }
        }
    }
    matched = matched || guest_matched || range_matched || guest_range_matched;
    if (!trace_jit_blocks_budget_allows(matched)) return;
    fprintf(stderr, "macrunner-hb-jit-block: guest=%p native=%p-%p size=%zu instrs=%zu "
            "first_op=%u first_guest=%p last_op=%u last_guest=%p last_target=%p%s\n",
            (void*)(uintptr_t)guest_pc, native, native + native_size, native_size,
            block ? block->instr_count : 0,
            first ? (unsigned)first->op : 0, first ? (void*)(uintptr_t)first->guest_addr : NULL,
            last ? (unsigned)last->op : 0, last ? (void*)(uintptr_t)last->guest_addr : NULL,
            last ? (void*)(uintptr_t)last->target : NULL, matched ? " match=1" : "");
    if (matched && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            fprintf(stderr, "macrunner-hb-jit-block-ir: guest=%p op=%u target=%p len=%u\n",
                    (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->op,
                    (void*)(uintptr_t)instr->target, (unsigned)instr->guest_len);
            fprintf(stderr,
                    "macrunner-hb-jit-block-ir-operands: guest=%p "
                    "dst{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src1{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src2{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} cc=%u\n",
                    (void*)(uintptr_t)instr->guest_addr,
                    (unsigned)instr->dst.type, (unsigned)instr->dst.size,
                    (unsigned)instr->dst.reg, (unsigned)instr->dst.reg_offset,
                    (long long)instr->dst.imm, (unsigned)instr->dst.mem.base,
                    (unsigned)instr->dst.mem.index, (unsigned)instr->dst.mem.scale,
                    (long long)instr->dst.mem.disp, (unsigned)instr->dst.mem.segment,
                    (unsigned)instr->dst.mem.addr32,
                    (unsigned)instr->src1.type, (unsigned)instr->src1.size,
                    (unsigned)instr->src1.reg, (unsigned)instr->src1.reg_offset,
                    (long long)instr->src1.imm, (unsigned)instr->src1.mem.base,
                    (unsigned)instr->src1.mem.index, (unsigned)instr->src1.mem.scale,
                    (long long)instr->src1.mem.disp, (unsigned)instr->src1.mem.segment,
                    (unsigned)instr->src1.mem.addr32,
                    (unsigned)instr->src2.type, (unsigned)instr->src2.size,
                    (unsigned)instr->src2.reg, (unsigned)instr->src2.reg_offset,
                    (long long)instr->src2.imm, (unsigned)instr->src2.mem.base,
                    (unsigned)instr->src2.mem.index, (unsigned)instr->src2.mem.scale,
                    (long long)instr->src2.mem.disp, (unsigned)instr->src2.mem.segment,
                    (unsigned)instr->src2.mem.addr32,
                    (unsigned)instr->cc);
        }
    }
    fflush(stderr);
}

static bool trace_runtime_read_x64_reg(const hb_context_t* ctx, hb_reg_t reg, uint64_t* value) {
    if (!ctx || !value || ctx->mode != HB_MODE_64BIT) return false;
    switch (reg) {
        case HB_REG_RAX: *value = ctx->regs.x64.rax; return true;
        case HB_REG_RCX: *value = ctx->regs.x64.rcx; return true;
        case HB_REG_RDX: *value = ctx->regs.x64.rdx; return true;
        case HB_REG_RBX: *value = ctx->regs.x64.rbx; return true;
        case HB_REG_RSP: *value = ctx->regs.x64.rsp; return true;
        case HB_REG_RBP: *value = ctx->regs.x64.rbp; return true;
        case HB_REG_RSI: *value = ctx->regs.x64.rsi; return true;
        case HB_REG_RDI: *value = ctx->regs.x64.rdi; return true;
        case HB_REG_R8:  *value = ctx->regs.x64.r8; return true;
        case HB_REG_R9:  *value = ctx->regs.x64.r9; return true;
        case HB_REG_R10: *value = ctx->regs.x64.r10; return true;
        case HB_REG_R11: *value = ctx->regs.x64.r11; return true;
        case HB_REG_R12: *value = ctx->regs.x64.r12; return true;
        case HB_REG_R13: *value = ctx->regs.x64.r13; return true;
        case HB_REG_R14: *value = ctx->regs.x64.r14; return true;
        case HB_REG_R15: *value = ctx->regs.x64.r15; return true;
        case HB_REG_RIP: *value = ctx->pc; return true;
        default: return false;
    }
}

static bool trace_runtime_mem_addr(const hb_context_t* ctx, const hb_ir_instr_t* instr,
                                   const hb_ir_operand_t* op, uint64_t* addr) {
    uint64_t base = 0, index = 0;
    if (!ctx || !op || !addr || op->type != HB_OP_MEM) return false;
    if (op->mem.base < HB_REG_COUNT &&
        !trace_runtime_read_x64_reg(ctx, op->mem.base, &base))
        return false;
    if (op->mem.index < HB_REG_COUNT &&
        !trace_runtime_read_x64_reg(ctx, op->mem.index, &index))
        return false;
    if (op->mem.base == HB_REG_RIP && instr)
        base = instr->guest_addr + instr->guest_len;
    *addr = base + index * (op->mem.scale ? op->mem.scale : 1) + op->mem.disp;
    if (op->mem.addr32) *addr = (uint32_t)*addr;
    return true;
}

static void trace_jit_helper_fault_operand(const hb_context_t* ctx, const hb_ir_instr_t* instr,
                                           const char* role, const hb_ir_operand_t* op) {
    uint64_t addr = 0, value = 0;
    hb_result_t read = HB_ERR_INVALID_ARG;
    size_t read_size;

    if (!ctx || !instr || !role || !op || op->type != HB_OP_MEM) return;
    if (!trace_runtime_mem_addr(ctx, instr, op, &addr)) return;
    read_size = op->size && op->size < sizeof(value) ? op->size : sizeof(value);
    if (ctx->memory && read_size)
        read = hb_memory_read(ctx->memory, addr, &value, read_size);
    fprintf(stderr,
            "macrunner-hb-jit-helper-fail-mem: guest=%p role=%s addr=%p size=%u "
            "read=%s value=%p base=%u index=%u scale=%u disp=%lld\n",
            (void*)(uintptr_t)instr->guest_addr, role, (void*)(uintptr_t)addr,
            (unsigned)op->size, hb_result_string(read), (void*)(uintptr_t)value,
            (unsigned)op->mem.base, (unsigned)op->mem.index, (unsigned)op->mem.scale,
            (long long)op->mem.disp);
}

static void trace_jit_helper_fault_block(const hb_context_t* ctx, const hb_ir_block_t* block) {
    static unsigned reports;
    if (!ctx || !block || reports++ >= 32) return;
    fprintf(stderr,
            "macrunner-hb-jit-helper-fail: result=%s block=%p instrs=%zu pc=%p "
            "rax=%p rcx=%p rdx=%p rbx=%p rsp=%p rbp=%p rsi=%p rdi=%p "
            "r8=%p r9=%p r10=%p r11=%p r12=%p r13=%p r14=%p r15=%p\n",
            hb_result_string(ctx->last_result), (void*)(uintptr_t)block->guest_addr,
            block->instr_count, (void*)(uintptr_t)ctx->pc,
            (void*)(uintptr_t)ctx->regs.x64.rax, (void*)(uintptr_t)ctx->regs.x64.rcx,
            (void*)(uintptr_t)ctx->regs.x64.rdx, (void*)(uintptr_t)ctx->regs.x64.rbx,
            (void*)(uintptr_t)ctx->regs.x64.rsp, (void*)(uintptr_t)ctx->regs.x64.rbp,
            (void*)(uintptr_t)ctx->regs.x64.rsi, (void*)(uintptr_t)ctx->regs.x64.rdi,
            (void*)(uintptr_t)ctx->regs.x64.r8, (void*)(uintptr_t)ctx->regs.x64.r9,
            (void*)(uintptr_t)ctx->regs.x64.r10, (void*)(uintptr_t)ctx->regs.x64.r11,
            (void*)(uintptr_t)ctx->regs.x64.r12, (void*)(uintptr_t)ctx->regs.x64.r13,
            (void*)(uintptr_t)ctx->regs.x64.r14, (void*)(uintptr_t)ctx->regs.x64.r15);
    for (size_t i = 0; i < block->instr_count; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        fprintf(stderr,
                "macrunner-hb-jit-helper-fail-ir: index=%zu guest=%p len=%u op=%u target=%p "
                "dst{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} "
                "src1{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} "
                "src2{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} cc=%u\n",
                i, (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->guest_len,
                (unsigned)instr->op, (void*)(uintptr_t)instr->target,
                (unsigned)instr->dst.type, (unsigned)instr->dst.size,
                (unsigned)instr->dst.reg, (long long)instr->dst.imm,
                (unsigned)instr->dst.mem.base, (unsigned)instr->dst.mem.index,
                (unsigned)instr->dst.mem.scale, (long long)instr->dst.mem.disp,
                (unsigned)instr->dst.mem.addr32,
                (unsigned)instr->src1.type, (unsigned)instr->src1.size,
                (unsigned)instr->src1.reg, (long long)instr->src1.imm,
                (unsigned)instr->src1.mem.base, (unsigned)instr->src1.mem.index,
                (unsigned)instr->src1.mem.scale, (long long)instr->src1.mem.disp,
                (unsigned)instr->src1.mem.addr32,
                (unsigned)instr->src2.type, (unsigned)instr->src2.size,
                (unsigned)instr->src2.reg, (long long)instr->src2.imm,
                (unsigned)instr->src2.mem.base, (unsigned)instr->src2.mem.index,
                (unsigned)instr->src2.mem.scale, (long long)instr->src2.mem.disp,
                (unsigned)instr->src2.mem.addr32, (unsigned)instr->cc);
        trace_jit_helper_fault_operand(ctx, instr, "dst", &instr->dst);
        trace_jit_helper_fault_operand(ctx, instr, "src1", &instr->src1);
        trace_jit_helper_fault_operand(ctx, instr, "src2", &instr->src2);
    }
    fflush(stderr);
}

static void trace_x86_low_pc_after_block(const hb_context_t* ctx,
                                         const hb_ir_block_t* block,
                                         uint64_t steps,
                                         uint64_t blocks_executed) {
    static unsigned reports;
    if (!ctx || !block || !trace_x86_low_pc_enabled()) return;
    if (ctx->arch != HB_ARCH_X86 && ctx->mode != HB_MODE_32BIT) return;
    if (ctx->pc >= 0x10000u || reports++ >= 16) return;

    fprintf(stderr,
            "macrunner-hb-x86-low-pc: pc=%p block=%p instrs=%zu steps=%llu blocks=%llu "
            "eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x eflags=%08x\n",
            (void*)(uintptr_t)ctx->pc, (void*)(uintptr_t)block->guest_addr,
            block->instr_count, (unsigned long long)steps,
            (unsigned long long)blocks_executed, ctx->regs.x86.eax,
            ctx->regs.x86.ebx, ctx->regs.x86.ecx, ctx->regs.x86.edx,
            ctx->regs.x86.esi, ctx->regs.x86.edi, ctx->regs.x86.ebp,
            ctx->regs.x86.esp, ctx->regs.x86.eflags);
    for (size_t i = 0; i < block->instr_count; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        fprintf(stderr,
                "macrunner-hb-x86-low-pc-ir: index=%zu guest=%p len=%u op=%u target=%p "
                "dst{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} "
                "src1{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} "
                "src2{t=%u sz=%u reg=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld addr32=%u)} cc=%u\n",
                i, (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->guest_len,
                (unsigned)instr->op, (void*)(uintptr_t)instr->target,
                (unsigned)instr->dst.type, (unsigned)instr->dst.size,
                (unsigned)instr->dst.reg, (long long)instr->dst.imm,
                (unsigned)instr->dst.mem.base, (unsigned)instr->dst.mem.index,
                (unsigned)instr->dst.mem.scale, (long long)instr->dst.mem.disp,
                (unsigned)instr->dst.mem.addr32,
                (unsigned)instr->src1.type, (unsigned)instr->src1.size,
                (unsigned)instr->src1.reg, (long long)instr->src1.imm,
                (unsigned)instr->src1.mem.base, (unsigned)instr->src1.mem.index,
                (unsigned)instr->src1.mem.scale, (long long)instr->src1.mem.disp,
                (unsigned)instr->src1.mem.addr32,
                (unsigned)instr->src2.type, (unsigned)instr->src2.size,
                (unsigned)instr->src2.reg, (long long)instr->src2.imm,
                (unsigned)instr->src2.mem.base, (unsigned)instr->src2.mem.index,
                (unsigned)instr->src2.mem.scale, (long long)instr->src2.mem.disp,
                (unsigned)instr->src2.mem.addr32, (unsigned)instr->cc);
    }
    fflush(stderr);
}

static bool trace_jit_block_contains_guest(const hb_ir_block_t* block, uint64_t guest) {
    if (!block || !guest) return false;
    if (block->guest_addr == guest) return true;
    for (size_t i = 0; i < block->instr_count; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        uint64_t start = instr->guest_addr;
        uint64_t end = start + instr->guest_len;
        if (guest == start || (instr->guest_len && guest >= start && guest < end))
            return true;
    }
    return false;
}

#define JIT_WATCH_RING_N 16
typedef struct {
    uint64_t guest, fire, rax, rcx, rdx, rsp;
} jit_watch_ring_t;
static jit_watch_ring_t jit_watch_ring[JIT_WATCH_RING_N];
static uint64_t jit_watch_ring_idx;

static void trace_jit_cached_watch_block_once(hb_jit_runtime_t* rt, const hb_block_cache_entry_t* entry) {
    static uint64_t dumped_guest;
    static uint64_t fires;
    uint64_t guest = trace_jit_guest_addr();
    uint64_t guest2 = trace_jit_guest_addr2();
    uint64_t f;
    hb_context_t* ctx = rt ? rt->ctx : NULL;
    int matched;
    if (!entry || !entry->valid || !entry->block || !trace_jit_blocks_enabled())
        return;
    matched = (guest && trace_jit_block_contains_guest(entry->block, guest)) ||
              (guest2 && trace_jit_block_contains_guest(entry->block, guest2));
    if (!matched)
        return;
    /* MacRunner 2026-07-28 (HK Mono/JIT [B] bisection): the watched dispatch must
     * carry the LIVE registers — the OK-arm's rcx is the manufactured object and
     * the wrapper post-call block's rax is the checked function's return; without
     * values the watch can only say "reached", not "with what".  Two sinks:
     * (1) bounded live prints (first 128 fires + power-of-ten milestones) for the
     * boot baseline; (2) a 16-entry ring of the MOST RECENT fires, dumped at
     * run-exit — the faulting call's dispatch is exactly what the live cap would
     * otherwise lose (measured: ~25 fires/min at boot, invoke at ~+30 min). */
    f = ++fires;
    {
        jit_watch_ring_t* slot = &jit_watch_ring[jit_watch_ring_idx % JIT_WATCH_RING_N];
        slot->guest = entry->guest_addr;
        slot->fire = f;
        slot->rax = ctx ? ctx->regs.x64.rax : 0;
        slot->rcx = ctx ? ctx->regs.x64.rcx : 0;
        slot->rdx = ctx ? ctx->regs.x64.rdx : 0;
        slot->rsp = ctx ? ctx->regs.x64.rsp : 0;
        jit_watch_ring_idx++;
    }
    if (f > 128 && f != 1000 && f != 10000 && f != 100000 && f != 1000000)
        return;
    if (f > 128 && dumped_guest == entry->block->guest_addr)
        return;
    dumped_guest = entry->block->guest_addr;
    fprintf(stderr, "macrunner-hb-jit-watch: guest=%p fire=%llu rax=%p rcx=%p rdx=%p rsp=%p\n",
            (void*)(uintptr_t)entry->guest_addr, (unsigned long long)f,
            ctx ? (void*)(uintptr_t)ctx->regs.x64.rax : NULL,
            ctx ? (void*)(uintptr_t)ctx->regs.x64.rcx : NULL,
            ctx ? (void*)(uintptr_t)ctx->regs.x64.rdx : NULL,
            ctx ? (void*)(uintptr_t)ctx->regs.x64.rsp : NULL);
    fflush(stderr);
    trace_jit_block(entry->guest_addr, entry->native_code, entry->native_size, entry->block);
}

void hb_jit_watch_ring_dump(void) {
    uint64_t n = jit_watch_ring_idx < JIT_WATCH_RING_N ? jit_watch_ring_idx : JIT_WATCH_RING_N;
    uint64_t start = jit_watch_ring_idx - n;
    uint64_t k;
    fprintf(stderr, "macrunner-hb-jit-watch-ring: total=%llu shown=%llu\n",
            (unsigned long long)jit_watch_ring_idx, (unsigned long long)n);
    for (k = 0; k < n; k++) {
        const jit_watch_ring_t* slot = &jit_watch_ring[(start + k) % JIT_WATCH_RING_N];
        fprintf(stderr, "macrunner-hb-jit-watch-ring: guest=%p fire=%llu rax=%p rcx=%p rdx=%p rsp=%p\n",
                (void*)(uintptr_t)slot->guest, (unsigned long long)slot->fire,
                (void*)(uintptr_t)slot->rax, (void*)(uintptr_t)slot->rcx,
                (void*)(uintptr_t)slot->rdx, (void*)(uintptr_t)slot->rsp);
    }
    fflush(stderr);
}

static int trace_jit_hot_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_hot_interval(void) {
    static int parsed;
    static uint64_t interval;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCK_INTERVAL");
        interval = env && *env ? strtoull(env, NULL, 0) : 500000ULL;
        if (interval < 1000ULL) interval = 1000ULL;
        parsed = 1;
    }
    return interval;
}

static int trace_jit_hot_bytes_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static size_t trace_jit_hot_bytes_len(void) {
    static int parsed;
    static size_t len;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES_LEN");
        len = env && *env ? (size_t)strtoull(env, NULL, 0) : 16;
        if (len < 1) len = 16;
        if (len > 128) len = 128;
        parsed = 1;
    }
    return len;
}

static void trace_jit_hot_guest_bytes(hb_context_t* ctx, uint64_t guest_addr) {
    uint8_t byte;
    size_t len;
    if (!trace_jit_hot_bytes_enabled() || !ctx || !ctx->memory) return;
    len = trace_jit_hot_bytes_len();
    fprintf(stderr, " bytes=");
    for (size_t i = 0; i < len; i++) {
        if (hb_memory_read_u8(ctx->memory, (hb_gva_t)(guest_addr + i), &byte) != HB_OK) {
            fprintf(stderr, "%s??", i ? " " : "");
            break;
        }
        fprintf(stderr, "%s%02x", i ? " " : "", byte);
    }
}

static void trace_jit_hot_block_tick(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry) {
    const size_t top_count = 12;
    hb_block_cache_entry_t* top[12] = {0};

    if (!rt || !entry || !trace_jit_hot_blocks_enabled()) return;
    entry->hit_count++;
    rt->hot_trace_blocks++;
    if (!rt->hot_trace_next)
        rt->hot_trace_next = trace_jit_hot_interval();
    if (rt->hot_trace_blocks < rt->hot_trace_next) return;
    rt->hot_trace_next += trace_jit_hot_interval();

    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        hb_block_cache_entry_t* candidate = &rt->block_cache->entries[i];
        if (!candidate->valid || !candidate->hit_count) continue;
        for (size_t j = 0; j < top_count; j++) {
            if (!top[j] || candidate->hit_count > top[j]->hit_count) {
                for (size_t k = top_count - 1; k > j; k--) top[k] = top[k - 1];
                top[j] = candidate;
                break;
            }
        }
    }

    fprintf(stderr, "macrunner-hb-jit-hot-blocks: total=%llu interval=%llu used=%zu\n",
            (unsigned long long)rt->hot_trace_blocks,
            (unsigned long long)trace_jit_hot_interval(),
            rt->jit_mem ? rt->jit_mem->used : 0);
    for (size_t i = 0; i < top_count && top[i]; i++) {
        fprintf(stderr, "macrunner-hb-jit-hot-block: rank=%zu guest=%p hits=%llu native=%p "
                "size=%zu steps=%u",
                i + 1, (void*)(uintptr_t)top[i]->guest_addr,
                (unsigned long long)top[i]->hit_count,
                top[i]->native_code, top[i]->native_size, top[i]->steps);
        trace_jit_hot_guest_bytes(rt->ctx, top[i]->guest_addr);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx) {
    hb_jit_runtime_t* rt = calloc(1, sizeof(hb_jit_runtime_t));
    const char* size_env;
    size_t jit_size = 128u * 1024u * 1024u;
    if (!rt) return NULL;
    hb_runtime_init_environment();
    rt->ctx = ctx;
    rt->persistent_cache_flags = macrunner_hb_runtime_persistent_cache_flags;
    size_env = getenv("MACRUNNER_HB_JIT_BUFFER_SIZE");
    if (size_env && *size_env) {
        unsigned long long parsed = strtoull(size_env, NULL, 0);
        if (parsed >= 65536ULL && parsed <= 512ULL * 1024ULL * 1024ULL)
            jit_size = (size_t)parsed;
    }
    rt->jit_mem = hb_jit_buffer_create(jit_size);
    if (!rt->jit_mem) { free(rt); return NULL; }
    rt->block_cache = block_cache_create();
    if (!rt->block_cache) {
        hb_jit_buffer_destroy(rt->jit_mem);
        free(rt);
        return NULL;
    }
    const char* cache_root = getenv("MACRUNNER_HB_TRANSLATION_CACHE_ROOT");
    const char* cache_env = getenv("MACRUNNER_HB_TRANSLATION_CACHE");
    int cache_enabled = (cache_env && *cache_env) ? (*cache_env != '0') :
                        (cache_root && *cache_root);
    if (cache_enabled) {
        hb_cache_options_t options;
        memset(&options, 0, sizeof(options));
        rt->persistent_cache = hb_cache_open(cache_root && *cache_root ? cache_root : NULL, &options);
        hb_contract_telemetry_record_open(rt->persistent_cache != NULL);
        translation_cache_register_atexit();
        if (translation_cache_trace_enabled()) {
            fprintf(stderr, "macrunner-hb-translation-cache-open: root=%s status=%s\n",
                    cache_root && *cache_root ? cache_root : "build/hyperbridge-cache",
                    rt->persistent_cache ? "ok" : "failed");
            fflush(stderr);
        }
    }
    return rt;
}

void hb_jit_runtime_destroy(hb_jit_runtime_t* rt) {
    if (!rt) return;
    if (rt->persistent_cache) translation_cache_trace_summary();
    if (smc_trace_enabled() && (g_smc_tracked || g_smc_evicted || g_smc_unreadable))
        fprintf(stderr,
                "macrunner-hb-smc-reverify: summary tracked=%llu reverified=%llu "
                "evicted=%llu unreadable=%llu relift_exits=%llu relift_suppressed=%llu\n",
                (unsigned long long)g_smc_tracked, (unsigned long long)g_smc_reverified,
                (unsigned long long)g_smc_evicted, (unsigned long long)g_smc_unreadable,
                (unsigned long long)g_smc_relift_exits,
                (unsigned long long)g_smc_relift_suppressed);
    hb_cache_close(rt->persistent_cache);
    hb_jit_buffer_destroy(rt->jit_mem);
    block_cache_destroy(rt->block_cache);
    free(rt->jit_signal_quarantine);
    free(rt);
}

/* MacRunner: reset for per-thread reuse instead of destroy+recreate per callback.
 * Reuses the 128MB MAP_JIT arena (no munmap/mmap) + the block_cache allocation;
 * eagerly frees owned blocks and rewinds the arena so translations regenerate
 * from current guest code (SMC-safe). Re-points ctx to the new per-callback ctx
 * (generated code embeds ctx state, so a full regenerate against the live ctx is
 * required — which the cleared caches + rewound arena guarantee). */
void hb_jit_runtime_reset(hb_jit_runtime_t* rt, hb_context_t* ctx) {
    if (!rt) return;
    rt->ctx = ctx;
    if (rt->jit_mem) hb_jit_buffer_reset(rt->jit_mem);
    if (rt->block_cache) block_cache_reset(rt->block_cache);
    rt->hot_trace_blocks = 0;
    rt->hot_trace_next = 0;
    rt->code_cache_full = false;
    rt->code_cache_full_reports = 0;
}

/* Find block by guest address */
static hb_ir_block_t* find_block(const hb_ir_cfg_t* cfg, uint64_t addr) {
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

static bool is_control_transfer_op(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

static const hb_ir_instr_t* first_control_transfer_instr(const hb_ir_block_t* block) {
    if (!block) return NULL;
    for (size_t i = 0; i < block->instr_count; i++) {
        if (is_control_transfer_op(block->instrs[i].op)) return &block->instrs[i];
    }
    return NULL;
}

static bool block_terminal_is_chainable(const hb_ir_block_t* block) {
    const hb_ir_instr_t* terminal;
    if (!block || block->instr_count == 0) return false;
    terminal = &block->instrs[block->instr_count - 1];
    return terminal->op == HB_IR_JMP;
}

static bool entry_has_chain_slot(const hb_block_cache_entry_t* entry, size_t* offset) {
    static const uint32_t arm64_nop = 0xd503201fu;
    static const uint32_t epilogue[4] = {
        0xa9427bf7u, /* LDP X23, LR,  [SP, #32] */
        0xa9415bf5u, /* LDP X21, X22, [SP, #16] */
        0xa8c353f3u, /* LDP X19, X20, [SP], #48 */
        0xd65f03c0u  /* RET */
    };
    uint32_t insn;

    if (!entry || !entry->native_code || entry->native_size < 32) return false;
    size_t pos = entry->native_size - 32;
    for (size_t i = 0; i < 4; i++) {
        memcpy(&insn, entry->native_code + pos + i * 4, sizeof(insn));
        if (insn != arm64_nop) return false;
    }
    for (size_t i = 0; i < 4; i++) {
        memcpy(&insn, entry->native_code + pos + 16 + i * 4, sizeof(insn));
        if (insn != epilogue[i]) return false;
    }
    if (offset) *offset = pos;
    return true;
}

static bool arm64_branch_reaches(const uint8_t* from, const uint8_t* to) {
    intptr_t off;
    if (!from || !to) return false;
    off = (intptr_t)(to - from);
    return (off % 4) == 0 && off >= -(intptr_t)0x08000000 && off < (intptr_t)0x08000000;
}

static uint32_t arm64_b_to(const uint8_t* from, const uint8_t* to) {
    intptr_t off = (intptr_t)(to - from);
    return 0x14000000u | (uint32_t)(((off / 4) & 0x03ffffffu));
}

static uint32_t arm64_mov_reg_u32(int rd, int rn) {
    return 0xaa0003e0u | ((uint32_t)rn << 16) | (uint32_t)rd;
}

static bool patch_block_tail(hb_jit_runtime_t* rt, hb_block_cache_entry_t* cur,
                             hb_block_cache_entry_t* next) {
    hb_block_chain_meta_t* meta;
    uint8_t* target;
    uint8_t* patch;
    size_t patch_offset;
    bool ok;

    if (!runtime_block_chain_enabled() || !rt || !rt->jit_mem || !cur || !next)
        return false;
    if (!cur->valid || !next->valid || !cur->native_code || !next->native_code)
        return false;
    meta = block_cache_chain_meta(rt->block_cache, cur, true);
    if (!meta) return false;
    if (meta->target_code) return meta->target_code == next->native_code + 16;
    if (!block_terminal_is_chainable(cur->block))
        return false;
    if (!entry_has_chain_slot(cur, &patch_offset) || !entry_has_chain_slot(next, NULL))
        return false;

    target = next->native_code + 12; /* Reuse current frame, then run callee MOV X19, X0. */
    patch = cur->native_code + patch_offset;
    if (!arm64_branch_reaches(patch + sizeof(uint32_t), target))
        return false;

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK)
        return false;
    arm64_store_u32(patch, arm64_mov_reg_u32(0, 19)); /* MOV X0, X19 (ctx) */
    arm64_store_u32(patch + sizeof(uint32_t),
                    arm64_b_to(patch + sizeof(uint32_t), target));
    block_cache_clear_icache(patch, 2 * sizeof(uint32_t));
    ok = hb_jit_buffer_make_executable(rt->jit_mem) == HB_OK;
    if (!ok) return false;

    meta->guest_addr = next->guest_addr;
    meta->target_code = target;
    meta->patch_offset = patch_offset;
    return true;
}

static void update_indirect_ic(hb_context_t* ctx, hb_block_cache_entry_t* target,
                               bool enabled) {
    if (!ctx || !enabled || !target || !target->valid || !target->native_code ||
        !block_terminal_is_chainable(target->block) || !entry_has_chain_slot(target, NULL)) {
        if (ctx) {
            ctx->indirect_ic_guest_addr = 0;
            ctx->indirect_ic_native_code = 0;
        }
        return;
    }
    ctx->indirect_ic_guest_addr = target->guest_addr;
    ctx->indirect_ic_native_code = (uint64_t)(uintptr_t)(target->native_code + 16);
}

static uint32_t jit_block_step_count(const hb_ir_block_t* block) {
    const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
    if (!block) return 0;
    if (!transfer) return (uint32_t)block->instr_count;
    return (uint32_t)((size_t)(transfer - block->instrs) + 1);
}

static void sync_arch_pc_after_jit_block(hb_context_t* ctx) {
    if (!ctx) return;
    if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
    else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
}

static void set_helper_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                    uint64_t steps, uint64_t blocks_executed) {
    out->result = ctx->last_result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = "JIT helper fault";
}

static hb_result_t set_runtime_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                            hb_result_t result, uint64_t steps,
                                            uint64_t blocks_executed,
                                            const char* reason) {
    if (ctx) ctx->last_result = result;
    out->result = result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = reason;
    return result;
}

static hb_result_t set_jit_interp_fallback_result(hb_exec_result_t* out,
                                                  hb_result_t result,
                                                  uint64_t steps,
                                                  uint64_t blocks_executed,
                                                  const char* reason) {
    out->result = result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = reason;
    return HB_OK;
}

static bool jit_sigbus_invalidate_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_JIT_SIGBUS_INVALIDATE");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_sigill_ownership_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_JIT_SIGILL_OWNERSHIP");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_signal_quarantine_enabled(void) {
    return jit_sigbus_invalidate_enabled() || jit_sigill_ownership_enabled();
}

static bool jit_signal_quarantine_enabled_for(int signal) {
    if (signal == SIGBUS) return jit_sigbus_invalidate_enabled();
    if (signal == SIGILL) return jit_sigill_ownership_enabled();
    return false;
}

static bool jit_aa_sigbus_probe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_AA_SIGBUS_PROBE");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_aa_force_mono_simd_copy_interp_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_AA_FORCE_MONO_SIMD_COPY_INTERP");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_aa_mono_simd_copy_gate_probe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_AA_MONO_SIMD_COPY_GATE_PROBE");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_aa_mono_4ee14b_transparency_probe_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char* env = getenv("MACRUNNER_HB_AA_MONO_4EE14B_TRANSPARENCY_PROBE");
        enabled = env && env[0] && env[0] != '0';
    }
    return enabled != 0;
}

static bool jit_aa_is_mono_simd_copy_block(hb_context_t* ctx,
                                           const hb_block_cache_entry_t* entry,
                                           uint8_t bytes[16],
                                           uint64_t* rva_out) {
    static const uint8_t sig_4ee14b[16] = {
        0x0f, 0x1f, 0x44, 0x00, 0x00, /* nop dword ptr [rax+rax] */
        0xf3, 0x0f, 0x6f, 0x0a,       /* movdqu xmm1, xmmword ptr [rdx] */
        0xf3, 0x0f, 0x6f, 0x52, 0x10, /* movdqu xmm2, xmmword ptr [rdx+0x10] */
        0xf3, 0x0f                    /* next movdqu */
    };
    static const uint8_t sig_4ee150[16] = {
        0xf3, 0x0f, 0x6f, 0x0a,
        0xf3, 0x0f, 0x6f, 0x52, 0x10,
        0xf3, 0x0f, 0x6f, 0x5a, 0x20,
        0xf3, 0x0f
    };
    uint64_t rva;
    if (!ctx || !ctx->memory || !entry || !entry->guest_addr)
        return false;
    if (!(ctx->codegen_flags & HB_CONTEXT_CODEGEN_MONO_MODULE) ||
        !ctx->codegen_module_base || entry->guest_addr < ctx->codegen_module_base)
        return false;
    rva = entry->guest_addr - ctx->codegen_module_base;
    if (rva != 0x4ee14bull && rva != 0x4ee150ull)
        return false;
    for (size_t i = 0; i < 16; i++) {
        if (hb_memory_read_u8(ctx->memory, (hb_gva_t)(entry->guest_addr + i),
                              &bytes[i]) != HB_OK)
            return false;
    }
    if (rva_out) *rva_out = rva;
    if (rva == 0x4ee14bull)
        return memcmp(bytes, sig_4ee14b, sizeof(sig_4ee14b)) == 0;
    return memcmp(bytes, sig_4ee150, sizeof(sig_4ee150)) == 0;
}

static void jit_aa_probe_mono_simd_copy_gate(hb_jit_runtime_t* rt,
                                             hb_block_cache_entry_t* cached,
                                             uint64_t steps,
                                             uint64_t blocks_executed) {
    static unsigned int reports;
    hb_context_t* ctx;
    uint8_t bytes[16] = {0};
    uint64_t rva = 0;

    if (!jit_aa_mono_simd_copy_gate_probe_enabled() || !rt || !cached)
        return;
    ctx = rt->ctx;
    if (!jit_aa_is_mono_simd_copy_block(ctx, cached, bytes, &rva))
        return;
    if (reports++ < 16) {
        fprintf(stderr,
                "macrunner-hb-aa-mono-simd-gate-probe: would_force=1 module_base=%p "
                "rva=0x%llx guest=%p native=%p-%p steps=%llu blocks=%llu "
                "dst=%p src=%p len=%llu "
                "bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                (void*)(uintptr_t)ctx->codegen_module_base,
                (unsigned long long)rva,
                (void*)(uintptr_t)cached->guest_addr,
                cached->native_code, cached->native_code + cached->native_size,
                (unsigned long long)steps, (unsigned long long)blocks_executed,
                (void*)(uintptr_t)ctx->regs.x64.rcx,
                (void*)(uintptr_t)ctx->regs.x64.rdx,
                (unsigned long long)ctx->regs.x64.r8,
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
        fflush(stderr);
    }
}

static bool jit_aa_force_mono_simd_copy_interp(hb_jit_runtime_t* rt,
                                               hb_block_cache_entry_t* cached,
                                               hb_exec_result_t* out,
                                               uint64_t steps,
                                               uint64_t blocks_executed) {
    static unsigned int reports;
    hb_context_t* ctx;
    uint8_t bytes[16] = {0};
    uint64_t rva = 0;

    if (!jit_aa_force_mono_simd_copy_interp_enabled() || !rt || !cached || !out)
        return false;
    if (!cached->block)
        return false;
    ctx = rt->ctx;
    if (!jit_aa_is_mono_simd_copy_block(ctx, cached, bytes, &rva))
        return false;

    ctx->pc = cached->guest_addr;
    sync_arch_pc_after_jit_block(ctx);
    if (reports++ < 16) {
        fprintf(stderr,
                "macrunner-hb-aa-force-mono-simd-interp: module_base=%p rva=0x%llx "
                "guest=%p native=%p-%p "
                "steps=%llu blocks=%llu dst=%p src=%p len=%llu "
                "bytes=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
                (void*)(uintptr_t)ctx->codegen_module_base,
                (unsigned long long)rva,
                (void*)(uintptr_t)cached->guest_addr,
                cached->native_code, cached->native_code + cached->native_size,
                (unsigned long long)steps, (unsigned long long)blocks_executed,
                (void*)(uintptr_t)ctx->regs.x64.rcx,
                (void*)(uintptr_t)ctx->regs.x64.rdx,
                (unsigned long long)ctx->regs.x64.r8,
                bytes[0], bytes[1], bytes[2], bytes[3],
                bytes[4], bytes[5], bytes[6], bytes[7],
                bytes[8], bytes[9], bytes[10], bytes[11],
                bytes[12], bytes[13], bytes[14], bytes[15]);
        fflush(stderr);
    }
    hb_jit_helper_exec_ir_block(ctx, cached->block);
    if (ctx->last_result != HB_OK) {
        out->result = ctx->last_result;
        out->steps_executed = steps;
        out->blocks_executed = blocks_executed;
        out->faulted = true;
        out->fault_reason = "A/B forced Mono RVA-scoped SIMD-copy inline interpreter fault";
    }
    return true;
}

static uint32_t jit_aa_capture_window(hb_memory_t* memory, uint64_t address,
                                      uint8_t bytes[32]) {
    uint32_t valid = 0;
    if (!memory || !address) return 0;
    for (unsigned int i = 0; i < 32; i++) {
        uint8_t byte = 0;
        if (hb_memory_read_u8(memory, (hb_gva_t)(address + i), &byte) == HB_OK) {
            bytes[i] = byte;
            valid |= (uint32_t)1u << i;
        }
    }
    return valid;
}

static void jit_aa_format_window(const uint8_t bytes[32], uint32_t valid,
                                 char text[65]) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 32; i++) {
        if (valid & ((uint32_t)1u << i)) {
            text[i * 2] = hex[bytes[i] >> 4];
            text[i * 2 + 1] = hex[bytes[i] & 15];
        } else {
            text[i * 2] = '?';
            text[i * 2 + 1] = '?';
        }
    }
    text[64] = 0;
}

#define JIT_AA_MONO_4EE14B_WINDOW 256u

typedef struct jit_aa_mono_4ee14b_window {
    uint8_t bytes[JIT_AA_MONO_4EE14B_WINDOW];
    uint8_t valid[JIT_AA_MONO_4EE14B_WINDOW];
    size_t size;
    uint64_t hash;
    unsigned int valid_count;
} jit_aa_mono_4ee14b_window_t;

static uint64_t jit_aa_hash_bytes(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static void jit_aa_mono_4ee14b_capture_window(hb_memory_t* memory, uint64_t address,
                                              size_t size,
                                              jit_aa_mono_4ee14b_window_t* out) {
    memset(out, 0, sizeof(*out));
    if (!memory || !address) return;
    if (size > JIT_AA_MONO_4EE14B_WINDOW) size = JIT_AA_MONO_4EE14B_WINDOW;
    out->size = size;
    out->hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; i++) {
        uint8_t byte = 0;
        if (hb_memory_read_u8(memory, (hb_gva_t)(address + i), &byte) == HB_OK) {
            out->bytes[i] = byte;
            out->valid[i] = 1;
            out->valid_count++;
        }
        out->hash ^= (uint64_t)out->valid[i];
        out->hash *= 1099511628211ull;
        if (out->valid[i]) {
            out->hash ^= (uint64_t)byte;
            out->hash *= 1099511628211ull;
        }
    }
}

static bool jit_aa_mono_4ee14b_restore_window(hb_memory_t* memory, uint64_t address,
                                              const jit_aa_mono_4ee14b_window_t* in) {
    bool ok = true;
    if (!memory || !address || !in) return false;
    for (size_t i = 0; i < in->size; i++) {
        if (!in->valid[i]) continue;
        if (hb_memory_write(memory, (hb_gva_t)(address + i), &in->bytes[i], 1) != HB_OK)
            ok = false;
    }
    return ok;
}

static bool jit_aa_mono_4ee14b_windows_equal(const jit_aa_mono_4ee14b_window_t* a,
                                             const jit_aa_mono_4ee14b_window_t* b) {
    if (!a || !b || a->size != b->size || a->valid_count != b->valid_count ||
        a->hash != b->hash)
        return false;
    for (size_t i = 0; i < a->size; i++) {
        if (a->valid[i] != b->valid[i]) return false;
        if (a->valid[i] && a->bytes[i] != b->bytes[i]) return false;
    }
    return true;
}

static void jit_aa_mono_4ee14b_format32(const jit_aa_mono_4ee14b_window_t* w,
                                        char text[65]) {
    uint8_t bytes[32] = {0};
    uint32_t valid = 0;
    if (w) {
        size_t n = w->size < 32 ? w->size : 32;
        for (size_t i = 0; i < n; i++) {
            bytes[i] = w->bytes[i];
            if (w->valid[i]) valid |= (uint32_t)1u << i;
        }
    }
    jit_aa_format_window(bytes, valid, text);
}

static bool jit_aa_mono_4ee14b_gpr_equal(const hb_regs_x64_t* a,
                                         const hb_regs_x64_t* b) {
    return a->rax == b->rax && a->rbx == b->rbx &&
           a->rcx == b->rcx && a->rdx == b->rdx &&
           a->rsi == b->rsi && a->rdi == b->rdi &&
           a->rsp == b->rsp && a->rbp == b->rbp &&
           a->r8  == b->r8  && a->r9  == b->r9  &&
           a->r10 == b->r10 && a->r11 == b->r11 &&
           a->r12 == b->r12 && a->r13 == b->r13 &&
           a->r14 == b->r14 && a->r15 == b->r15 &&
           a->rip == b->rip && a->rflags == b->rflags;
}

static void jit_aa_mono_4ee14b_transparency_probe(hb_jit_runtime_t* rt,
                                                  hb_block_cache_entry_t* cached,
                                                  uint64_t steps,
                                                  uint64_t blocks_executed) {
    typedef void (*jit_block_t)(hb_context_t*);
    static unsigned int reports;
    hb_context_t* ctx;
    hb_context_t pre_ctx, jit_ctx, interp_ctx;
    hb_jit_signal_fault_frame_t frame;
    jit_aa_mono_4ee14b_window_t pre_dst, pre_src, jit_dst, jit_src, interp_dst, interp_src;
    uint8_t bytes[16] = {0};
    uint64_t rva = 0;
    uint64_t dst, src, len;
    size_t window;
    int jit_signal = 0;
    hb_result_t interp_result;
    bool restore_pre_ok;
    char pre_dst_text[65], jit_dst_text[65], interp_dst_text[65];
    char pre_src_text[65], jit_src_text[65], interp_src_text[65];

    if (!jit_aa_mono_4ee14b_transparency_probe_enabled() || !rt || !cached ||
        !cached->native_code || !cached->block || reports >= 8)
        return;
    ctx = rt->ctx;
    if (!jit_aa_is_mono_simd_copy_block(ctx, cached, bytes, &rva) ||
        rva != 0x4ee14bull)
        return;

    reports++;
    pre_ctx = *ctx;
    dst = pre_ctx.regs.x64.rcx;
    src = pre_ctx.regs.x64.rdx;
    len = pre_ctx.regs.x64.r8;
    window = len < 128 ? 128 : (size_t)len;
    if (window > JIT_AA_MONO_4EE14B_WINDOW) window = JIT_AA_MONO_4EE14B_WINDOW;
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, dst, window, &pre_dst);
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, src, window, &pre_src);

    memset(&frame, 0, sizeof(frame));
    frame.prev = g_jit_signal_fault_frame;
    frame.rt = rt;
    frame.ctx = ctx;
    frame.entry = cached;
    frame.snapshot = pre_ctx;
    frame.steps = steps;
    frame.blocks_executed = blocks_executed;
    frame.aa_enabled = false;
    g_jit_signal_fault_frame = &frame;
    if (sigsetjmp(frame.env, 0) == 0) {
        jit_block_t exec = (jit_block_t)(void*)cached->native_code;
        exec(ctx);
    } else {
        jit_signal = frame.signal ? frame.signal : -1;
        *ctx = pre_ctx;
    }
    g_jit_signal_fault_frame = frame.prev;
    jit_ctx = *ctx;
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, dst, window, &jit_dst);
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, src, window, &jit_src);

    *ctx = pre_ctx;
    (void)jit_aa_mono_4ee14b_restore_window(pre_ctx.memory, dst, &pre_dst);
    hb_jit_helper_exec_ir_block(ctx, cached->block);
    interp_result = ctx->last_result;
    interp_ctx = *ctx;
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, dst, window, &interp_dst);
    jit_aa_mono_4ee14b_capture_window(pre_ctx.memory, src, window, &interp_src);

    *ctx = pre_ctx;
    restore_pre_ok = jit_aa_mono_4ee14b_restore_window(pre_ctx.memory, dst, &pre_dst);
    jit_aa_mono_4ee14b_format32(&pre_dst, pre_dst_text);
    jit_aa_mono_4ee14b_format32(&jit_dst, jit_dst_text);
    jit_aa_mono_4ee14b_format32(&interp_dst, interp_dst_text);
    jit_aa_mono_4ee14b_format32(&pre_src, pre_src_text);
    jit_aa_mono_4ee14b_format32(&jit_src, jit_src_text);
    jit_aa_mono_4ee14b_format32(&interp_src, interp_src_text);

    fprintf(stderr,
            "macrunner-hb-mono-4ee14b-diff: seq=%u module_base=%p rva=0x%llx "
            "guest=%p native=%p-%p instrs=%zu steps=%llu blocks=%llu "
            "pre_pc=%p pre_rip=%p dst=%p src=%p len=%llu window=%zu "
            "jit_signal=%d interp_result=%s restore_pre=%u "
            "pc_equal=%u rip_equal=%u gpr_equal=%u regs_equal=%u xmm_hash_equal=%u "
            "dst_equal=%u src_equal=%u "
            "jit_pc=%p interp_pc=%p jit_rip=%p interp_rip=%p "
            "jit_rcx=%p interp_rcx=%p jit_rdx=%p interp_rdx=%p "
            "jit_r8=%p interp_r8=%p jit_r15=%p interp_r15=%p "
            "jit_last=%s interp_last=%s "
            "pre_dst=%s jit_dst=%s interp_dst=%s "
            "pre_src=%s jit_src=%s interp_src=%s "
            "dst_hash_pre=%016llx dst_hash_jit=%016llx dst_hash_interp=%016llx "
            "src_hash_pre=%016llx src_hash_jit=%016llx src_hash_interp=%016llx\n",
            reports,
            (void*)(uintptr_t)pre_ctx.codegen_module_base,
            (unsigned long long)rva,
            (void*)(uintptr_t)cached->guest_addr,
            cached->native_code, cached->native_code + cached->native_size,
            cached->block->instr_count,
            (unsigned long long)steps, (unsigned long long)blocks_executed,
            (void*)(uintptr_t)pre_ctx.pc,
            (void*)(uintptr_t)pre_ctx.regs.x64.rip,
            (void*)(uintptr_t)dst, (void*)(uintptr_t)src,
            (unsigned long long)len, window,
            jit_signal, hb_result_string(interp_result),
            restore_pre_ok ? 1u : 0u,
            jit_ctx.pc == interp_ctx.pc ? 1u : 0u,
            jit_ctx.regs.x64.rip == interp_ctx.regs.x64.rip ? 1u : 0u,
            jit_aa_mono_4ee14b_gpr_equal(&jit_ctx.regs.x64, &interp_ctx.regs.x64) ? 1u : 0u,
            memcmp(&jit_ctx.regs.x64, &interp_ctx.regs.x64, sizeof(jit_ctx.regs.x64)) == 0 ? 1u : 0u,
            jit_aa_hash_bytes(jit_ctx.regs.x64.xmm, sizeof(jit_ctx.regs.x64.xmm)) ==
                jit_aa_hash_bytes(interp_ctx.regs.x64.xmm, sizeof(interp_ctx.regs.x64.xmm)) ? 1u : 0u,
            jit_aa_mono_4ee14b_windows_equal(&jit_dst, &interp_dst) ? 1u : 0u,
            jit_aa_mono_4ee14b_windows_equal(&jit_src, &interp_src) ? 1u : 0u,
            (void*)(uintptr_t)jit_ctx.pc, (void*)(uintptr_t)interp_ctx.pc,
            (void*)(uintptr_t)jit_ctx.regs.x64.rip, (void*)(uintptr_t)interp_ctx.regs.x64.rip,
            (void*)(uintptr_t)jit_ctx.regs.x64.rcx, (void*)(uintptr_t)interp_ctx.regs.x64.rcx,
            (void*)(uintptr_t)jit_ctx.regs.x64.rdx, (void*)(uintptr_t)interp_ctx.regs.x64.rdx,
            (void*)(uintptr_t)jit_ctx.regs.x64.r8, (void*)(uintptr_t)interp_ctx.regs.x64.r8,
            (void*)(uintptr_t)jit_ctx.regs.x64.r15, (void*)(uintptr_t)interp_ctx.regs.x64.r15,
            hb_result_string(jit_ctx.last_result), hb_result_string(interp_ctx.last_result),
            pre_dst_text, jit_dst_text, interp_dst_text,
            pre_src_text, jit_src_text, interp_src_text,
            (unsigned long long)pre_dst.hash, (unsigned long long)jit_dst.hash,
            (unsigned long long)interp_dst.hash,
            (unsigned long long)pre_src.hash, (unsigned long long)jit_src.hash,
            (unsigned long long)interp_src.hash);
    fflush(stderr);
}

typedef struct hb_jit_aa_vm_region {
    uint64_t start;
    uint64_t end;
    int protection;
    int max_protection;
    int status;
} hb_jit_aa_vm_region_t;

static hb_jit_aa_vm_region_t jit_aa_query_vm_region(uint64_t address) {
    hb_jit_aa_vm_region_t result;
    memset(&result, 0, sizeof(result));
#if defined(__APPLE__)
    {
        mach_vm_address_t region = (mach_vm_address_t)address;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        memory_object_name_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size,
                                          VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
        result.status = kr;
        if (kr == KERN_SUCCESS) {
            result.start = (uint64_t)region;
            result.end = (uint64_t)(region + size);
            result.protection = info.protection;
            result.max_protection = info.max_protection;
        }
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    }
#else
    (void)address;
    result.status = -1;
#endif
    return result;
}

static bool jit_signal_quarantine_contains(const hb_jit_runtime_t* rt, uint64_t guest_addr) {
    if (!rt || !guest_addr) return false;
    for (size_t i = 0; i < rt->jit_signal_quarantine_count; i++)
        if (rt->jit_signal_quarantine[i] == guest_addr) return true;
    return false;
}

static bool jit_signal_quarantine_add(hb_jit_runtime_t* rt, uint64_t guest_addr) {
    uint64_t* entries;
    size_t capacity;
    if (!rt || !guest_addr) return false;
    if (jit_signal_quarantine_contains(rt, guest_addr)) return true;
    if (rt->jit_signal_quarantine_count == rt->jit_signal_quarantine_capacity) {
        capacity = rt->jit_signal_quarantine_capacity ?
                   rt->jit_signal_quarantine_capacity * 2 : 16;
        if (capacity < rt->jit_signal_quarantine_capacity) return false;
        entries = realloc(rt->jit_signal_quarantine, capacity * sizeof(*entries));
        if (!entries) return false;
        rt->jit_signal_quarantine = entries;
        rt->jit_signal_quarantine_capacity = capacity;
    }
    rt->jit_signal_quarantine[rt->jit_signal_quarantine_count++] = guest_addr;
    return true;
}

static int jit_signal_fault_claim(hb_jit_signal_fault_frame_t* frame,
                                  uint64_t pc, uint64_t fault_addr, int signal,
                                  uint32_t native_word, bool native_word_valid,
                                  bool active_guard_claim,
                                  const void* host_context) {
    if (!frame || !frame->rt || !frame->rt->jit_mem || !frame->entry ||
        !frame->entry->native_code || !frame->entry->native_size)
        return 0;

    frame->host_pc = pc;
    frame->fault_addr = fault_addr;
    frame->signal = signal;
    frame->native_word = native_word;
    frame->native_word_valid = native_word_valid;
    frame->active_guard_claim = active_guard_claim;
    if (frame->ctx) {
        frame->fault_guest_pc = frame->ctx->pc;
        if (frame->ctx->arch == HB_ARCH_X64)
            frame->fault_arch_pc = frame->ctx->regs.x64.rip;
        else if (frame->ctx->arch == HB_ARCH_X86)
            frame->fault_arch_pc = frame->ctx->regs.x86.eip;
        frame->fault_indirect_ic_guest = frame->ctx->indirect_ic_guest_addr;
        frame->fault_indirect_ic_native = frame->ctx->indirect_ic_native_code;
    }
#if defined(__APPLE__) && defined(__aarch64__)
    if (host_context) {
        const ucontext_t* context = (const ucontext_t*)host_context;
        if (context->uc_mcontext) {
            for (unsigned int i = 0; i < 29; i++)
                frame->host_gpr[i] = context->uc_mcontext->__ss.__x[i];
            frame->host_gpr[29] = context->uc_mcontext->__ss.__fp;
            frame->host_gpr[30] = context->uc_mcontext->__ss.__lr;
            frame->host_sp = context->uc_mcontext->__ss.__sp;
            frame->host_fault_pc = context->uc_mcontext->__ss.__pc;
            frame->host_pstate = context->uc_mcontext->__ss.__cpsr;
            frame->host_context_valid = true;
        }
    }
#else
    (void)host_context;
#endif
    {
        static int traced;
        if (traced++ < 8 && getenv("MACRUNNER_HB_TRACE_JIT_HELPER_FAIL"))
            fprintf(stderr, "macrunner-hb-jit-native-sigfault: host_pc=0x%llx fault=0x%llx sig=%d active_guard=%u\n",
                    (unsigned long long)pc, (unsigned long long)fault_addr, signal,
                    active_guard_claim ? 1u : 0u);
    }
    siglongjmp(frame->env, 1);
    return 1;
}

int hb_jit_runtime_handle_signal_fault(uint64_t pc, uint64_t fault_addr, int signal,
                                       const void* host_context) {
    hb_jit_signal_fault_frame_t* frame = g_jit_signal_fault_frame;
    uintptr_t native_start, native_end, slab_start, slab_end;

    if (!frame || !frame->rt || !frame->rt->jit_mem || !frame->entry ||
        !frame->entry->native_code || !frame->entry->native_size)
        return 0;

    native_start = (uintptr_t)frame->entry->native_code;
    native_end = native_start + frame->entry->native_size;
    slab_start = (uintptr_t)frame->rt->jit_mem->executable;
    slab_end = slab_start + frame->rt->jit_mem->used;
    if (native_end < native_start || slab_end < slab_start) return 0;
    if (!((uintptr_t)pc >= native_start && (uintptr_t)pc < native_end) &&
        !((uintptr_t)pc >= slab_start && (uintptr_t)pc < slab_end))
        return 0;

    return jit_signal_fault_claim(frame, pc, fault_addr, signal, 0, false,
                                  false, host_context);
}

int hb_jit_runtime_handle_owned_sigill(uint64_t pc, uint32_t native_word,
                                       int native_word_valid,
                                       const void* host_context) {
    hb_jit_signal_fault_frame_t* frame = g_jit_signal_fault_frame;

    /* The caller has already applied MACRUNNER_HB_JIT_SIGILL_OWNERSHIP.  An
     * active guard is the ownership authority here, not Mach VM membership:
     * the observed failure is a generated tail branch into an old zero RX page
     * outside both the guarded entry and the current JIT slab. */
    return jit_signal_fault_claim(frame, pc, 0, SIGILL, native_word,
                                  native_word_valid != 0, true, host_context);
}

static void jit_aa_report_sigbus(const hb_jit_signal_fault_frame_t* frame,
                                 const hb_block_cache_entry_t* faulted) {
    const hb_context_t* entry_ctx;
    const hb_block_cache_entry_t* native_entry;
    hb_jit_aa_vm_region_t fault_region, dst_region, src_region;
    char dst_pre[65], src_pre[65], dst_post[65], src_post[65], guest_text[65];
    uint8_t guest_bytes[32] = {0};
    uint32_t guest_valid;
    uint32_t native_words[5] = {0};
    unsigned int native_valid = 0;
    uint64_t dst, src, length, dst_end, src_end, fault_guest;
    unsigned int seq;

    if (!frame || !frame->aa_enabled || frame->signal != SIGBUS) return;
    seq = __sync_add_and_fetch(&g_jit_aa_sigbus_reports, 1);
    if (seq > 8) return;

    entry_ctx = &frame->snapshot;
    native_entry = faulted ? faulted : frame->entry;
    fault_guest = faulted ? faulted->guest_addr : frame->entry->guest_addr;
    dst = entry_ctx->regs.x64.rcx;
    src = entry_ctx->regs.x64.rdx;
    length = entry_ctx->regs.x64.r8;
    dst_end = length > UINT64_MAX - dst ? UINT64_MAX : dst + length;
    src_end = length > UINT64_MAX - src ? UINT64_MAX : src + length;

    jit_aa_format_window(frame->aa_dst_pre, frame->aa_dst_pre_valid, dst_pre);
    jit_aa_format_window(frame->aa_src_pre, frame->aa_src_pre_valid, src_pre);
    jit_aa_format_window(frame->aa_dst_post, frame->aa_dst_post_valid, dst_post);
    jit_aa_format_window(frame->aa_src_post, frame->aa_src_post_valid, src_post);
    guest_valid = jit_aa_capture_window(entry_ctx->memory, fault_guest, guest_bytes);
    jit_aa_format_window(guest_bytes, guest_valid, guest_text);

    if (native_entry && native_entry->native_code && native_entry->native_size) {
        uintptr_t start = (uintptr_t)native_entry->native_code;
        uintptr_t end = start + native_entry->native_size;
        uintptr_t center = (uintptr_t)frame->host_pc & ~(uintptr_t)3;
        for (int i = -2; i <= 2; i++) {
            uintptr_t at = center + (intptr_t)i * 4;
            if (at >= start && at + sizeof(uint32_t) <= end) {
                memcpy(&native_words[i + 2], (const void*)at, sizeof(uint32_t));
                native_valid |= 1u << (i + 2);
            }
        }
    }

    fault_region = jit_aa_query_vm_region(frame->fault_addr);
    dst_region = jit_aa_query_vm_region(dst);
    src_region = jit_aa_query_vm_region(src);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-state: seq=%u signal=%d block_entry=%p fault_guest=%p "
            "native_entry=%p hostpc=%p fault=%p steps=%llu blocks=%llu "
            "dst=%p-%p src=%p-%p len=%llu\n",
            seq, frame->signal, (void*)(uintptr_t)frame->entry->guest_addr,
            (void*)(uintptr_t)fault_guest,
            native_entry ? (void*)native_entry->native_code : NULL,
            (void*)(uintptr_t)frame->host_pc, (void*)(uintptr_t)frame->fault_addr,
            (unsigned long long)frame->steps,
            (unsigned long long)frame->blocks_executed,
            (void*)(uintptr_t)dst, (void*)(uintptr_t)dst_end,
            (void*)(uintptr_t)src, (void*)(uintptr_t)src_end,
            (unsigned long long)length);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-entry-gpr: seq=%u pc=%p rip=%p rflags=%llx "
            "rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx\n",
            seq, (void*)(uintptr_t)entry_ctx->pc,
            (void*)(uintptr_t)entry_ctx->regs.x64.rip,
            (unsigned long long)entry_ctx->regs.x64.rflags,
            (unsigned long long)entry_ctx->regs.x64.rax,
            (unsigned long long)entry_ctx->regs.x64.rbx,
            (unsigned long long)entry_ctx->regs.x64.rcx,
            (unsigned long long)entry_ctx->regs.x64.rdx,
            (unsigned long long)entry_ctx->regs.x64.rsi,
            (unsigned long long)entry_ctx->regs.x64.rdi,
            (unsigned long long)entry_ctx->regs.x64.rsp,
            (unsigned long long)entry_ctx->regs.x64.rbp);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-entry-ext: seq=%u r8=%llx r9=%llx r10=%llx r11=%llx "
            "r12=%llx r13=%llx r14=%llx r15=%llx\n",
            seq, (unsigned long long)entry_ctx->regs.x64.r8,
            (unsigned long long)entry_ctx->regs.x64.r9,
            (unsigned long long)entry_ctx->regs.x64.r10,
            (unsigned long long)entry_ctx->regs.x64.r11,
            (unsigned long long)entry_ctx->regs.x64.r12,
            (unsigned long long)entry_ctx->regs.x64.r13,
            (unsigned long long)entry_ctx->regs.x64.r14,
            (unsigned long long)entry_ctx->regs.x64.r15);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-memory: seq=%u dst_valid_pre=%08x dst_pre=%s "
            "dst_valid_native_post=%08x dst_native_post=%s src_valid_pre=%08x src_pre=%s "
            "src_valid_native_post=%08x src_native_post=%s\n",
            seq, frame->aa_dst_pre_valid, dst_pre, frame->aa_dst_post_valid, dst_post,
            frame->aa_src_pre_valid, src_pre, frame->aa_src_post_valid, src_post);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-insn: seq=%u guest_pc=%p guest_valid=%08x guest32=%s "
            "native_pc=%p native_valid=%02x native_w_m2_p2=%08x,%08x,%08x,%08x,%08x\n",
            seq, (void*)(uintptr_t)fault_guest, guest_valid, guest_text,
            (void*)(uintptr_t)frame->host_pc, native_valid,
            native_words[0], native_words[1], native_words[2], native_words[3], native_words[4]);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-vm: seq=%u fault_region=%p-%p prot=%x max=%x status=%d "
            "dst_region=%p-%p prot=%x max=%x status=%d src_region=%p-%p prot=%x max=%x status=%d\n",
            seq, (void*)(uintptr_t)fault_region.start, (void*)(uintptr_t)fault_region.end,
            fault_region.protection, fault_region.max_protection, fault_region.status,
            (void*)(uintptr_t)dst_region.start, (void*)(uintptr_t)dst_region.end,
            dst_region.protection, dst_region.max_protection, dst_region.status,
            (void*)(uintptr_t)src_region.start, (void*)(uintptr_t)src_region.end,
            src_region.protection, src_region.max_protection, src_region.status);
    fprintf(stderr,
            "macrunner-hb-aa-sigbus-ucontext-0: seq=%u valid=%u pc=%llx sp=%llx pstate=%llx "
            "x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx x6=%llx x7=%llx\n",
            seq, frame->host_context_valid ? 1u : 0u,
            (unsigned long long)frame->host_fault_pc,
            (unsigned long long)frame->host_sp,
            (unsigned long long)frame->host_pstate,
            (unsigned long long)frame->host_gpr[0], (unsigned long long)frame->host_gpr[1],
            (unsigned long long)frame->host_gpr[2], (unsigned long long)frame->host_gpr[3],
            (unsigned long long)frame->host_gpr[4], (unsigned long long)frame->host_gpr[5],
            (unsigned long long)frame->host_gpr[6], (unsigned long long)frame->host_gpr[7]);
    for (unsigned int base = 8; base < 31; base += 8) {
        unsigned int last = base + 7 < 31 ? base + 7 : 30;
        fprintf(stderr,
                "macrunner-hb-aa-sigbus-ucontext-n: seq=%u range=x%u-x%u "
                "v=%llx,%llx,%llx,%llx,%llx,%llx,%llx,%llx\n",
                seq, base, last,
                (unsigned long long)frame->host_gpr[base],
                (unsigned long long)frame->host_gpr[base + 1],
                (unsigned long long)frame->host_gpr[base + 2],
                (unsigned long long)frame->host_gpr[base + 3],
                (unsigned long long)frame->host_gpr[base + 4],
                (unsigned long long)frame->host_gpr[base + 5],
                (unsigned long long)frame->host_gpr[base + 6],
                (unsigned long long)(base + 7 < 31 ? frame->host_gpr[base + 7] : 0));
    }
    fflush(stderr);
}

static hb_result_t run_jit_block_with_signal_guard(hb_jit_runtime_t* rt,
                                                   hb_block_cache_entry_t* cached,
                                                   hb_exec_result_t* out,
                                                   uint64_t steps,
                                                   uint64_t blocks_executed) {
    typedef void (*jit_block_t)(hb_context_t*);
    hb_jit_signal_fault_frame_t frame;
    hb_context_t* ctx;
    hb_block_cache_entry_t* faulted;
    jit_block_t exec;

    if (!rt || !rt->ctx || !cached || !cached->native_code || !out)
        return HB_ERR_INVALID_ARG;

    ctx = rt->ctx;
    jit_aa_probe_mono_simd_copy_gate(rt, cached, steps, blocks_executed);
    jit_aa_mono_4ee14b_transparency_probe(rt, cached, steps, blocks_executed);
    if (jit_aa_force_mono_simd_copy_interp(rt, cached, out, steps, blocks_executed))
        return HB_OK;
    if (jit_signal_quarantine_enabled() &&
        (rt->jit_signal_disable ||
         jit_signal_quarantine_contains(rt, cached->guest_addr))) {
        ctx->pc = cached->guest_addr;
        sync_arch_pc_after_jit_block(ctx);
        return set_jit_interp_fallback_result(
            out, HB_ERR_UNSUPPORTED_FEATURE, steps, blocks_executed,
            rt->jit_signal_disable ?
                "JIT disabled after unmapped native signal; interpreter fallback" :
                "JIT native-signal block quarantined; interpreter fallback");
    }
    memset(&frame, 0, sizeof(frame));
    frame.prev = g_jit_signal_fault_frame;
    frame.rt = rt;
    frame.ctx = ctx;
    frame.entry = cached;
    frame.snapshot = *ctx;
    frame.steps = steps;
    frame.blocks_executed = blocks_executed;
    frame.aa_enabled = jit_aa_sigbus_probe_enabled();
    if (frame.aa_enabled) {
        frame.aa_dst_pre_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rcx, frame.aa_dst_pre);
        frame.aa_src_pre_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rdx, frame.aa_src_pre);
    }
    g_jit_signal_fault_frame = &frame;

    if (sigsetjmp(frame.env, 0) == 0) {
        exec = (jit_block_t)(void*)cached->native_code;
        frame.dispatched_guest = cached->guest_addr;
        frame.dispatched_native = (uint64_t)(uintptr_t)exec;
        frame.dispatched_native_size = cached->native_size;
        exec(ctx);
        g_jit_signal_fault_frame = frame.prev;
        return HB_OK;
    }

    g_jit_signal_fault_frame = frame.prev;
    if (frame.aa_enabled && frame.signal == SIGBUS) {
        frame.aa_dst_post_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rcx, frame.aa_dst_post);
        frame.aa_src_post_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rdx, frame.aa_src_post);
    }
    *ctx = frame.snapshot;
    faulted = block_cache_find_native_pc(rt->block_cache, frame.host_pc);
    if (jit_signal_quarantine_enabled_for(frame.signal)) {
        hb_block_cache_entry_t* quarantine_entry = faulted ? faulted : cached;
        uint64_t fault_guest = quarantine_entry ? quarantine_entry->guest_addr : 0;
        bool quarantined = fault_guest && jit_signal_quarantine_add(rt, fault_guest);
        const char* record = frame.signal == SIGILL ? "sigill-own" : "sigbus-invalidate";

        if (frame.signal == SIGBUS) jit_aa_report_sigbus(&frame, faulted);

        /* The snapshot is the only complete architectural checkpoint. Resume
         * from its entry; the interpreter advances to the quarantined block
         * without executing that native block again. */
        ctx->pc = cached->guest_addr;
        sync_arch_pc_after_jit_block(ctx);
        if (!quarantined) rt->jit_signal_disable = true;
        fprintf(stderr,
                "macrunner-hb-jit-%s: hostpc=%p fault=%p signal=%d "
                "fault_guest=%p resume_guest=%p mapped=%u quarantined=%u "
                "active_guard=%u disable_jit=%u count=%zu\n",
                record,
                (void*)(uintptr_t)frame.host_pc,
                (void*)(uintptr_t)frame.fault_addr,
                frame.signal,
                (void*)(uintptr_t)fault_guest,
                (void*)(uintptr_t)cached->guest_addr,
                faulted ? 1u : 0u, quarantined ? 1u : 0u,
                frame.active_guard_claim ? 1u : 0u,
                rt->jit_signal_disable ? 1u : 0u,
                rt->jit_signal_quarantine_count);
        fflush(stderr);
    }
    if (frame.signal == SIGILL && g_jit_sigill_ownership_reports++ < 32) {
        hb_block_cache_entry_t* source = faulted ? faulted : cached;
        uintptr_t native_start = source ? (uintptr_t)source->native_code : 0;
        uintptr_t native_end = source ? native_start + source->native_size : 0;
        uintptr_t arena_start = rt->jit_mem ? (uintptr_t)rt->jit_mem->executable : 0;
        uintptr_t arena_end = rt->jit_mem && rt->jit_mem->size <= UINTPTR_MAX - arena_start ?
                              arena_start + rt->jit_mem->size : 0;
        uintptr_t arena_used_end = rt->jit_mem && rt->jit_mem->used <= UINTPTR_MAX - arena_start ?
                                   arena_start + rt->jit_mem->used : 0;
        uintptr_t word_pc = (uintptr_t)frame.host_pc & ~(uintptr_t)3;
        hb_jit_aa_vm_region_t fault_region = jit_aa_query_vm_region(frame.host_pc);
        hb_jit_aa_vm_region_t dispatch_region = jit_aa_query_vm_region(frame.dispatched_native);
        hb_jit_aa_vm_region_t x21_region = jit_aa_query_vm_region(
            frame.host_context_valid ? frame.host_gpr[21] : 0);
        uint32_t native_word = frame.native_word;
        bool word_valid = frame.native_word_valid;
        char guest_bytes[64] = {0};
        char* p = guest_bytes;

        if (!word_valid && faulted && native_end >= native_start && word_pc >= native_start &&
            word_pc <= native_end && native_end - word_pc >= sizeof(native_word)) {
            memcpy(&native_word, (const void*)word_pc, sizeof(native_word));
            word_valid = true;
        }
        if (source && ctx && ctx->memory) {
            for (int i = 0; i < 16 && (size_t)(p - guest_bytes) < sizeof(guest_bytes) - 3; i++) {
                uint8_t b = 0;
                if (hb_memory_read_u8(ctx->memory,
                                      (hb_gva_t)(source->guest_addr + (uint64_t)i), &b) != HB_OK)
                    break;
                p += snprintf(p, sizeof(guest_bytes) - (size_t)(p - guest_bytes),
                              "%s%02x", i ? " " : "", b);
            }
        }
        fprintf(stderr,
                "macrunner-hb-jit-sigill-native: guest=%p owner_native=%p-%p "
                "hostpc=%p offset=%#llx mapped=%u active_guard=%u "
                "word_valid=%u word=%08x x20=%#llx x21=%#llx lr=%#llx gbytes=%s\n",
                (void*)(uintptr_t)(source ? source->guest_addr : 0),
                (void*)native_start, (void*)native_end,
                (void*)(uintptr_t)frame.host_pc,
                (unsigned long long)(faulted && (uintptr_t)frame.host_pc >= native_start ?
                                     (uintptr_t)frame.host_pc - native_start : 0),
                faulted ? 1u : 0u, frame.active_guard_claim ? 1u : 0u,
                word_valid ? 1u : 0u, native_word,
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[20] : 0),
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[21] : 0),
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[30] : 0),
                guest_bytes);
        fflush(stderr);

        fprintf(stderr,
                "macrunner-hb-jit-sigill-dispatch: dispatch_guest=%p "
                "dispatch_native=%p-%p current_guest=%p current_native=%p-%p "
                "hostpc=%p x8=%#llx x28=%#llx lr=%#llx "
                "fault_ctx_pc=%p fault_arch_pc=%p ic_guest=%p ic_native=%p "
                "arena=%p-%p used_end=%p in_arena=%u in_used=%u\n",
                (void*)(uintptr_t)frame.dispatched_guest,
                (void*)(uintptr_t)frame.dispatched_native,
                (void*)(uintptr_t)(frame.dispatched_native + frame.dispatched_native_size),
                (void*)(uintptr_t)(source ? source->guest_addr : 0),
                (void*)native_start, (void*)native_end,
                (void*)(uintptr_t)frame.host_pc,
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[8] : 0),
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[28] : 0),
                (unsigned long long)(frame.host_context_valid ? frame.host_gpr[30] : 0),
                (void*)(uintptr_t)frame.fault_guest_pc,
                (void*)(uintptr_t)frame.fault_arch_pc,
                (void*)(uintptr_t)frame.fault_indirect_ic_guest,
                (void*)(uintptr_t)frame.fault_indirect_ic_native,
                (void*)arena_start, (void*)arena_end, (void*)arena_used_end,
                arena_end >= arena_start && (uintptr_t)frame.host_pc >= arena_start &&
                    (uintptr_t)frame.host_pc < arena_end ? 1u : 0u,
                arena_used_end >= arena_start && (uintptr_t)frame.host_pc >= arena_start &&
                    (uintptr_t)frame.host_pc < arena_used_end ? 1u : 0u);
        fflush(stderr);

        fprintf(stderr,
                "macrunner-hb-jit-sigill-vm: hostpc=%p region=%p-%p prot=%#x max=%#x kr=%d "
                "dispatch=%p region=%p-%p prot=%#x max=%#x kr=%d "
                "x21=%p region=%p-%p prot=%#x max=%#x kr=%d same_host_x21=%u\n",
                (void*)(uintptr_t)frame.host_pc,
                (void*)(uintptr_t)fault_region.start, (void*)(uintptr_t)fault_region.end,
                fault_region.protection, fault_region.max_protection, fault_region.status,
                (void*)(uintptr_t)frame.dispatched_native,
                (void*)(uintptr_t)dispatch_region.start, (void*)(uintptr_t)dispatch_region.end,
                dispatch_region.protection, dispatch_region.max_protection, dispatch_region.status,
                (void*)(uintptr_t)(frame.host_context_valid ? frame.host_gpr[21] : 0),
                (void*)(uintptr_t)x21_region.start, (void*)(uintptr_t)x21_region.end,
                x21_region.protection, x21_region.max_protection, x21_region.status,
                frame.host_context_valid && frame.host_pc == frame.host_gpr[21] ? 1u : 0u);
        fflush(stderr);

        /* MOVDQA/MOVDQU stores lower through one codegen family.  Record every
         * exact DMB ISHST; STR X20,[X21]; STR X22,[X21,#8] instance in the
         * guarded source block.  The ordinal maps monotonically to the guest
         * SIMD stores and proves whether the encoder emitted a branch/zero word
         * at the RCX+0x60 trigger without changing generated execution. */
        if (source && source->native_code && source->native_size >= 12) {
            unsigned int stores = 0;
            for (size_t off = 0; off + 12 <= source->native_size; off += 4) {
                uint32_t words[7] = {0};
                uint32_t w0, w1, w2;
                size_t window_start;
                size_t window_size;

                memcpy(&w0, source->native_code + off, 4);
                memcpy(&w1, source->native_code + off + 4, 4);
                memcpy(&w2, source->native_code + off + 8, 4);
                if (w0 != 0xd5033abfu || w1 != 0xf90002b4u || w2 != 0xf90006b6u)
                    continue;
                window_start = off >= 8 ? off - 8 : 0;
                window_size = source->native_size - window_start;
                if (window_size > sizeof(words)) window_size = sizeof(words);
                memcpy(words, source->native_code + window_start, window_size);
                fprintf(stderr,
                        "macrunner-hb-jit-sigill-simd-store: ordinal=%u site=%p "
                        "offset=%#zx window_start=%#zx words=%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                        stores, source->native_code + off, off, window_start,
                        words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
                stores++;
            }
            fprintf(stderr,
                    "macrunner-hb-jit-sigill-simd-store-summary: guest=%p native=%p-%p "
                    "stores=%u expected_store_words=d5033abf,f90002b4,f90006b6\n",
                    (void*)(uintptr_t)source->guest_addr, source->native_code,
                    source->native_code + source->native_size, stores);
            fflush(stderr);
        }

        /* A SIGILL target outside the source entry is normally reached by a
         * direct/indirect native branch.  Decode the bounded source blob after
         * siglongjmp (never in the signal handler) and report only an edge whose
         * resolved target equals the fault PC.  This identifies the emitting
         * branch family without dumping the whole block or guessing from UDF #0. */
        if (source && source->native_code && source->native_size && frame.host_pc) {
            unsigned int matches = 0;
            for (size_t off = 0; off + sizeof(uint32_t) <= source->native_size; off += 4) {
                uint32_t insn;
                uintptr_t site = (uintptr_t)source->native_code + off;
                uintptr_t target = 0;
                unsigned int reg = 0;
                const char* kind = NULL;

                memcpy(&insn, source->native_code + off, sizeof(insn));
                if ((insn & 0xfc000000u) == 0x14000000u ||
                    (insn & 0xfc000000u) == 0x94000000u) {
                    int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
                    if (imm26 & 0x02000000) imm26 |= (int32_t)~0x03ffffffu;
                    target = (uintptr_t)((intptr_t)site + (intptr_t)imm26 * 4);
                    kind = (insn & 0x80000000u) ? "BL" : "B";
                } else if ((insn & 0xff000010u) == 0x54000000u) {
                    int32_t imm19 = (int32_t)((insn >> 5) & 0x7ffffu);
                    if (imm19 & 0x40000) imm19 |= (int32_t)~0x7ffffu;
                    target = (uintptr_t)((intptr_t)site + (intptr_t)imm19 * 4);
                    kind = "B.cond";
                } else if ((insn & 0xfffffc1fu) == 0xd61f0000u ||
                           (insn & 0xfffffc1fu) == 0xd63f0000u ||
                           (insn & 0xfffffc1fu) == 0xd65f0000u) {
                    reg = (insn >> 5) & 31u;
                    if (frame.host_context_valid && reg < 31) target = frame.host_gpr[reg];
                    if ((insn & 0xfffffc1fu) == 0xd61f0000u) kind = "BR";
                    else if ((insn & 0xfffffc1fu) == 0xd63f0000u) kind = "BLR";
                    else kind = "RET";
                }
                if (kind && target == (uintptr_t)frame.host_pc && matches++ < 8) {
                    fprintf(stderr,
                            "macrunner-hb-jit-sigill-branch-source: guest=%p site=%p "
                            "offset=%#zx word=%08x kind=%s reg=%u target=%p matched=1\n",
                            (void*)(uintptr_t)source->guest_addr, (void*)site, off, insn,
                            kind, reg, (void*)target);
                }
            }
            if (!matches) {
                uint32_t tail[4] = {0};
                size_t tail_size = source->native_size < sizeof(tail) ?
                                   source->native_size : sizeof(tail);
                memcpy((uint8_t*)tail + sizeof(tail) - tail_size,
                       source->native_code + source->native_size - tail_size, tail_size);
                fprintf(stderr,
                        "macrunner-hb-jit-sigill-branch-source: guest=%p owner_native=%p-%p "
                        "hostpc=%p matched=0 tail=%08x,%08x,%08x,%08x\n",
                        (void*)(uintptr_t)source->guest_addr,
                        source->native_code, source->native_code + source->native_size,
                        (void*)(uintptr_t)frame.host_pc,
                        tail[0], tail[1], tail[2], tail[3]);
            }
            fflush(stderr);
        }

        /* Search every live cache entry, not only the guarded entry.  This is
         * bounded to the used-slot index and runs after siglongjmp, so it is
         * signal-safe and does not turn SIGILL handling into a Mach-VM scan. */
        if (rt->block_cache && frame.host_pc) {
            unsigned int matches = 0;
            size_t scanned_entries = 0;
            for (size_t used = 0; used < rt->block_cache->used_count; used++) {
                size_t slot = rt->block_cache->used_slots[used];
                hb_block_cache_entry_t* entry;
                if (slot >= HB_BLOCK_CACHE_SIZE) continue;
                entry = &rt->block_cache->entries[slot];
                if (!entry->valid || !entry->native_code || !entry->native_size) continue;
                scanned_entries++;
                for (size_t off = 0; off + sizeof(uint32_t) <= entry->native_size; off += 4) {
                    uint32_t insn;
                    uintptr_t site = (uintptr_t)entry->native_code + off;
                    uintptr_t target = 0;
                    unsigned int reg = 0;
                    const char* kind = NULL;
                    memcpy(&insn, entry->native_code + off, sizeof(insn));
                    if ((insn & 0xfc000000u) == 0x14000000u ||
                        (insn & 0xfc000000u) == 0x94000000u) {
                        int32_t imm26 = (int32_t)(insn & 0x03ffffffu);
                        if (imm26 & 0x02000000) imm26 |= (int32_t)~0x03ffffffu;
                        target = (uintptr_t)((intptr_t)site + (intptr_t)imm26 * 4);
                        kind = (insn & 0x80000000u) ? "BL" : "B";
                    } else if ((insn & 0xff000010u) == 0x54000000u) {
                        int32_t imm19 = (int32_t)((insn >> 5) & 0x7ffffu);
                        if (imm19 & 0x40000) imm19 |= (int32_t)~0x7ffffu;
                        target = (uintptr_t)((intptr_t)site + (intptr_t)imm19 * 4);
                        kind = "B.cond";
                    } else if ((insn & 0xfffffc1fu) == 0xd61f0000u) {
                        reg = (insn >> 5) & 31u;
                        if (frame.host_context_valid && reg < 31) target = frame.host_gpr[reg];
                        kind = "BR";
                    }
                    if (kind && target == (uintptr_t)frame.host_pc && matches++ < 16) {
                        fprintf(stderr,
                                "macrunner-hb-jit-sigill-branch-all: guest=%p native=%p-%p "
                                "site=%p offset=%#zx word=%08x kind=%s reg=%u target=%p\n",
                                (void*)(uintptr_t)entry->guest_addr,
                                entry->native_code, entry->native_code + entry->native_size,
                                (void*)site, off, insn, kind, reg, (void*)target);
                    }
                }
            }
            fprintf(stderr,
                    "macrunner-hb-jit-sigill-branch-all-summary: hostpc=%p "
                    "entries=%zu matches=%u used_slots=%zu cache_count=%zu\n",
                    (void*)(uintptr_t)frame.host_pc, scanned_entries, matches,
                    rt->block_cache->used_count, rt->block_cache->count);
            fflush(stderr);
        }
    }
    if (g_jit_signal_fault_reports++ < 64) {
        /* MacRunner: dump the guest x86 bytes at the block entry (steps=0 means the
         * fault is at/near the first instruction) + the fault-address alignment, to
         * pin a misaligned LOCK atomic (SIGBUS=10 from JIT atomic lowered to LDXR/STXR
         * on an unaligned address). */
        char gb[64]; gb[0]=0;
        if (ctx && ctx->memory) {
            char *p = gb; uint64_t ga = cached->guest_addr;
            for (int i = 0; i < 16 && (size_t)(p-gb) < sizeof(gb)-3; i++) {
                uint8_t b = 0;
                if (hb_memory_read_u8(ctx->memory, (hb_gva_t)(ga + i), &b) != HB_OK) break;
                p += snprintf(p, sizeof(gb)-(p-gb), "%s%02x", i?" ":"", b);
            }
        }
        {
            /* Dump the native ARM64 words around the faulting host pc (JIT code is
             * host-mapped readable) to see if the 8-byte load was lowered to an
             * alignment-requiring instruction (LDAR/LDXR/atomic) vs a plain LDR. */
            const hb_block_cache_entry_t* native_entry = faulted ? faulted : cached;
            const uint32_t *hp = (const uint32_t *)(uintptr_t)(frame.host_pc & ~3ull);
            if (frame.host_pc >= (uint64_t)(uintptr_t)(native_entry->native_code + 8) &&
                frame.host_pc + 12 <=
                    (uint64_t)(uintptr_t)(native_entry->native_code + native_entry->native_size))
                fprintf(stderr, "macrunner-hb-jit-natinsn: hostpc=%p w[-2..+2]= %08x %08x [%08x] %08x %08x\n",
                        (void*)(uintptr_t)frame.host_pc,
                        hp[-2], hp[-1], hp[0], hp[1], hp[2]);
        }
        fprintf(stderr,
                "macrunner-hb-jit-regs: rax=%llx rcx=%llx rdx=%llx rbx=%llx rsp=%llx rbp=%llx "
                "rsi=%llx rdi=%llx r12=%llx fault=%llx rdx+8=%llx\n",
                (unsigned long long)ctx->regs.x64.rax, (unsigned long long)ctx->regs.x64.rcx,
                (unsigned long long)ctx->regs.x64.rdx, (unsigned long long)ctx->regs.x64.rbx,
                (unsigned long long)ctx->regs.x64.rsp, (unsigned long long)ctx->regs.x64.rbp,
                (unsigned long long)ctx->regs.x64.rsi, (unsigned long long)ctx->regs.x64.rdi,
                (unsigned long long)ctx->regs.x64.r12,
                (unsigned long long)frame.fault_addr,
                (unsigned long long)(ctx->regs.x64.rdx + 8));
        fprintf(stderr,
                "macrunner-hb-jit-signal-fallback: guest=%p native=%p-%p "
                "pc=%p fault=%p signal=%d steps=%llu blocks=%llu fault_align=%llu gbytes=%s\n",
                (void*)(uintptr_t)cached->guest_addr, cached->native_code,
                cached->native_code + cached->native_size,
                (void*)(uintptr_t)frame.host_pc,
                (void*)(uintptr_t)frame.fault_addr, frame.signal,
                (unsigned long long)frame.steps,
                (unsigned long long)frame.blocks_executed,
                (unsigned long long)(frame.fault_addr & 0xf), gb);
        fflush(stderr);
    }
    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                          frame.steps, frame.blocks_executed,
                                          "JIT native signal fault; interpreter fallback");
}

static void trace_jit_code_cache_full_once(hb_jit_runtime_t* rt,
                                           const char* reason,
                                           size_t needed) {
    if (!rt || !rt->jit_mem || !rt->block_cache) return;
    if (rt->code_cache_full_reports++) return;
    fprintf(stderr, "macrunner-hb-jit-code-cache-full: reason=%s used=%zu size=%zu "
            "needed=%zu entries=%zu capacity=%u\n",
            reason ? reason : "unknown", rt->jit_mem->used, rt->jit_mem->size,
            needed, rt->block_cache->count, (unsigned)HB_BLOCK_CACHE_SIZE);
    fflush(stderr);
}

static void try_promote_copy_scan_counted_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                               const hb_ir_block_t* block) {
    const hb_ir_block_t* body = NULL;
    const hb_ir_block_t* guard = NULL;
    hb_block_cache_entry_t* body_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        body_entry = block_cache_find(rt->block_cache, last->target);
        if (!body_entry || !body_entry->block || body_entry->fused) return;
        body = body_entry->block;
        guard = block;
    } else if (block->instr_count == 5 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* guard_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        if (!guard_entry || !guard_entry->block) return;
        body_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || body_entry->fused) return;
        body = block;
        guard = guard_entry->block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_copy_scan_counted_loop(cg, body, guard, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, body->guest_addr, dest, emitted_size,
                    (uint32_t)(body->instr_count + guard->instr_count), body, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=copy-scan-counted body=%p guard=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)body->guest_addr, (void*)(uintptr_t)guard->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static void try_promote_bounded_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* guard = NULL;
    const hb_ir_block_t* body = NULL;
    hb_block_cache_entry_t* guard_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* body_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        guard_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || !body_entry->block || !guard_entry || guard_entry->fused) return;
        guard = block;
        body = body_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        guard_entry = block_cache_find(rt->block_cache, last->target);
        if (!guard_entry || !guard_entry->block || guard_entry->fused) return;
        guard = guard_entry->block;
        body = block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_bounded_scan_loop(cg, guard, body, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, guard->guest_addr, dest, emitted_size,
                    (uint32_t)(guard->instr_count + body->instr_count), guard, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=bounded-byte-scan guard=%p body=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)guard->guest_addr, (void*)(uintptr_t)body->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool same_plain_runtime_reg_operand(const hb_ir_operand_t* a,
                                           const hb_ir_operand_t* b) {
    return a && b &&
           a->type == HB_OP_REG && b->type == HB_OP_REG &&
           a->reg == b->reg && a->size == b->size &&
           a->reg_offset == b->reg_offset;
}

static bool runtime_exact_reg_operand(const hb_ir_operand_t* op,
                                      hb_reg_t reg,
                                      hb_size_t size) {
    return op && op->type == HB_OP_REG && op->reg == reg &&
           op->size == size && op->reg_offset == 0;
}

static bool runtime_exact_mem_operand(const hb_ir_operand_t* op,
                                      hb_reg_t base,
                                      hb_reg_t index,
                                      uint8_t scale,
                                      int64_t disp,
                                      hb_size_t size) {
    return op && op->type == HB_OP_MEM && op->size == size &&
           op->mem.base == base && op->mem.index == index &&
           op->mem.scale == scale && op->mem.disp == disp &&
           op->mem.segment == 0 && !op->mem.addr32;
}

static bool zero_extend_mem8_to_reg32(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ZERO_EXTEND &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_32 &&
           instr->src1.type == HB_OP_MEM && instr->src1.size == HB_SIZE_8;
}

static bool add_imm1_same_reg64(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ADD &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, &instr->src1) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == 1;
}

static bool test_same_reg32(const hb_ir_instr_t* instr, const hb_ir_operand_t* reg) {
    return instr && reg && instr->op == HB_IR_TEST &&
           instr->src1.type == HB_OP_REG && instr->src1.size == HB_SIZE_32 &&
           same_plain_runtime_reg_operand(&instr->src1, &instr->src2) &&
           same_plain_runtime_reg_operand(&instr->src1, reg);
}

static bool byte_compare_loop_pair(const hb_ir_block_t* cmp_block,
                                   const hb_ir_block_t* backedge_block) {
    const hb_ir_instr_t *lhs, *rhs, *sub, *cmp_jcc;
    const hb_ir_instr_t *inc, *test, *back_jcc;
    if (!cmp_block || !backedge_block ||
        cmp_block->instr_count != 4 || backedge_block->instr_count != 3)
        return false;

    lhs = &cmp_block->instrs[0];
    rhs = &cmp_block->instrs[1];
    sub = &cmp_block->instrs[2];
    cmp_jcc = &cmp_block->instrs[3];
    inc = &backedge_block->instrs[0];
    test = &backedge_block->instrs[1];
    back_jcc = &backedge_block->instrs[2];

    if (!zero_extend_mem8_to_reg32(lhs) || !zero_extend_mem8_to_reg32(rhs))
        return false;
    if (sub->op != HB_IR_SUB ||
        !same_plain_runtime_reg_operand(&sub->dst, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src1, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src2, &rhs->dst))
        return false;
    if (cmp_jcc->op != HB_IR_Jcc ||
        (cmp_jcc->cc != HB_CC_E && cmp_jcc->cc != HB_CC_NE) ||
        cmp_jcc->guest_addr + cmp_jcc->guest_len != backedge_block->guest_addr)
        return false;
    if (!add_imm1_same_reg64(inc) ||
        !test_same_reg32(test, &rhs->dst) ||
        back_jcc->op != HB_IR_Jcc ||
        (back_jcc->cc != HB_CC_E && back_jcc->cc != HB_CC_NE) ||
        back_jcc->target != cmp_block->guest_addr)
        return false;
    return true;
}

static void try_promote_byte_compare_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* cmp_block = NULL;
    const hb_ir_block_t* backedge_block = NULL;
    hb_block_cache_entry_t* cmp_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 4 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* backedge_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        cmp_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!backedge_entry || !backedge_entry->block || !cmp_entry || cmp_entry->fused) return;
        cmp_block = block;
        backedge_block = backedge_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        cmp_entry = block_cache_find(rt->block_cache, last->target);
        if (!cmp_entry || !cmp_entry->block || cmp_entry->fused) return;
        cmp_block = cmp_entry->block;
        backedge_block = block;
    } else {
        return;
    }

    if (!byte_compare_loop_pair(cmp_block, backedge_block)) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, cmp_block, backedge_block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, cmp_block->guest_addr, dest, emitted_size,
                    (uint32_t)(cmp_block->instr_count + backedge_block->instr_count),
                    cmp_block, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=byte-compare-loop cmp=%p backedge=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)cmp_block->guest_addr,
                (void*)(uintptr_t)backedge_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool sub_imm_same_reg64(const hb_ir_instr_t* instr,
                               const hb_ir_operand_t* reg,
                               int64_t imm) {
    return instr && reg && instr->op == HB_IR_SUB &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, reg) &&
           same_plain_runtime_reg_operand(&instr->src1, reg) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == imm;
}

static bool load_test_nonzero_qword_block(const hb_ir_block_t* block,
                                          hb_ir_operand_t* index_reg,
                                          uint64_t* fallthrough,
                                          uint64_t* nonzero_target) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* test = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_64 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_64 ||
        load->src1.mem.index >= HB_REG_COUNT || load->src1.mem.scale != 8)
        return false;
    if (test->src1.type != HB_OP_REG || test->src1.size != HB_SIZE_64 ||
        !same_plain_runtime_reg_operand(&test->src1, &test->src2) ||
        !same_plain_runtime_reg_operand(&test->src1, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (index_reg) *index_reg = hb_ir_reg(load->src1.mem.index, HB_SIZE_64);
    if (fallthrough) *fallthrough = jcc->guest_addr + jcc->guest_len;
    if (nonzero_target) *nonzero_target = jcc->target;
    return true;
}

static bool dec_to_test_block(const hb_ir_block_t* block,
                              const hb_ir_operand_t* index_reg,
                              uint64_t* test_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* sub = &block->instrs[0];
    const hb_ir_instr_t* jmp = &block->instrs[1];
    if (!sub_imm_same_reg64(sub, index_reg, 1)) return false;
    if (jmp->op != HB_IR_JMP) return false;
    if (test_target) *test_target = jmp->target;
    return true;
}

static bool test_nonnegative_backedge_block(const hb_ir_block_t* block,
                                            const hb_ir_operand_t* index_reg,
                                            uint64_t load_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* test = &block->instrs[0];
    const hb_ir_instr_t* jcc = &block->instrs[1];
    return test && test->op == HB_IR_TEST &&
           same_plain_runtime_reg_operand(&test->src1, index_reg) &&
           same_plain_runtime_reg_operand(&test->src2, index_reg) &&
           jcc->op == HB_IR_Jcc && jcc->cc == HB_CC_NS && jcc->target == load_target;
}

static void try_promote_null_qword_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                             const hb_ir_block_t* block) {
    const hb_ir_block_t* load_block = NULL;
    const hb_ir_block_t* dec_block = NULL;
    const hb_ir_block_t* test_block = NULL;
    hb_block_cache_entry_t* load_entry = NULL;
    hb_ir_operand_t index_reg = hb_ir_none();
    uint64_t dec_addr = 0;
    uint64_t test_addr = 0;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block)
        return;

    if (load_test_nonzero_qword_block(block, &index_reg, &dec_addr, NULL)) {
        load_block = block;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr))
            return;
        hb_block_cache_entry_t* test_entry = block_cache_find(rt->block_cache, test_addr);
        if (!test_entry || !test_entry->block) return;
        dec_block = dec_entry->block;
        test_block = test_entry->block;
        load_entry = block_cache_find(rt->block_cache, load_block->guest_addr);
    } else if (block->instr_count == 2 && block->instrs[1].op == HB_IR_Jcc) {
        test_block = block;
        uint64_t load_addr = block->instrs[1].target;
        load_entry = block_cache_find(rt->block_cache, load_addr);
        if (!load_entry || !load_entry->block ||
            !load_test_nonzero_qword_block(load_entry->block, &index_reg, &dec_addr, NULL))
            return;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr) ||
            test_addr != test_block->guest_addr)
            return;
        load_block = load_entry->block;
        dec_block = dec_entry->block;
    } else {
        return;
    }

    if (!load_entry || load_entry->fused ||
        !test_nonnegative_backedge_block(test_block, &index_reg, load_block->guest_addr))
        return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_four_block_loop_helper(cg, load_block, dec_block,
                                                            test_block, NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, load_block->guest_addr, dest, emitted_size,
                    (uint32_t)(load_block->instr_count + dec_block->instr_count +
                               test_block->instr_count),
                    load_block, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=null-qword-scan load=%p dec=%p test=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)load_block->guest_addr,
                (void*)(uintptr_t)dec_block->guest_addr,
                (void*)(uintptr_t)test_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool load_cmp_jne_i32_entry_block(const hb_ir_block_t* block,
                                         uint64_t* equal_addr,
                                         uint64_t* less_addr) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* cmp = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_32 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_32)
        return false;
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_MEM || cmp->src1.size != HB_SIZE_32 ||
        !same_plain_runtime_reg_operand(&cmp->src2, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (equal_addr) *equal_addr = jcc->guest_addr + jcc->guest_len;
    if (less_addr) *less_addr = jcc->target;
    return true;
}

static bool cmp_setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                                hb_ir_operand_t* setcc_dst) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* cmp = &block->instrs[0];
    const hb_ir_instr_t* setcc = &block->instrs[1];
    const hb_ir_instr_t* ret = &block->instrs[2];
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_REG || cmp->src1.size != HB_SIZE_64 ||
        cmp->src2.type != HB_OP_REG || cmp->src2.size != HB_SIZE_64)
        return false;
    if (setcc->op != HB_IR_SETcc || setcc->cc != cc || setcc->dst.size != HB_SIZE_8 ||
        ret->op != HB_IR_RET)
        return false;
    if (setcc_dst) *setcc_dst = setcc->dst;
    return true;
}

static bool setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                            const hb_ir_operand_t* expected_dst) {
    if (!block || !expected_dst || block->instr_count != 2) return false;
    const hb_ir_instr_t* setcc = &block->instrs[0];
    const hb_ir_instr_t* ret = &block->instrs[1];
    return setcc->op == HB_IR_SETcc && setcc->cc == cc &&
           same_plain_runtime_reg_operand(&setcc->dst, expected_dst) &&
           ret->op == HB_IR_RET;
}

static const hb_ir_block_t* find_comparator_entry_pred(const hb_ir_block_t* block,
                                                       uint64_t* equal_addr,
                                                       uint64_t* less_addr) {
    if (load_cmp_jne_i32_entry_block(block, equal_addr, less_addr))
        return block;
    if (!block) return NULL;
    for (size_t i = 0; i < block->pred_count; i++) {
        const hb_ir_block_t* pred = block->pred[i];
        if (load_cmp_jne_i32_entry_block(pred, equal_addr, less_addr))
            return pred;
    }
    return NULL;
}

static const hb_ir_block_t* find_comparator_entry_near_cache(hb_block_cache_t* cache,
                                                             const hb_ir_block_t* equal_block,
                                                             uint64_t* equal_addr,
                                                             uint64_t* less_addr) {
    hb_ir_operand_t ignored = hb_ir_none();
    if (!cache || !equal_block || !cmp_setcc_ret_block(equal_block, HB_CC_B, &ignored))
        return NULL;
    for (uint64_t back = 1; back <= 16; back++) {
        hb_block_cache_entry_t* entry = block_cache_find(cache, equal_block->guest_addr - back);
        if (!entry || !entry->block) continue;
        uint64_t candidate_equal = 0;
        uint64_t candidate_less = 0;
        if (load_cmp_jne_i32_entry_block(entry->block, &candidate_equal, &candidate_less) &&
            candidate_equal == equal_block->guest_addr) {
            if (equal_addr) *equal_addr = candidate_equal;
            if (less_addr) *less_addr = candidate_less;
            return entry->block;
        }
    }
    return NULL;
}

static void try_promote_i32_less_tiebreaker_comparator(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                                       const hb_ir_block_t* block) {
    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block)
        return;

    uint64_t equal_addr = 0;
    uint64_t less_addr = 0;
    const hb_ir_block_t* entry_block = find_comparator_entry_pred(block, &equal_addr, &less_addr);
    if (!entry_block)
        entry_block = find_comparator_entry_near_cache(rt->block_cache, block, &equal_addr, &less_addr);
    if (!entry_block) return;

    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, entry_block->guest_addr);
    hb_block_cache_entry_t* equal = block_cache_find(rt->block_cache, equal_addr);
    hb_block_cache_entry_t* less = block_cache_find(rt->block_cache, less_addr);
    if (!entry || entry->fused || !equal || !equal->block)
        return;

    hb_ir_operand_t setcc_dst = hb_ir_none();
    if (!cmp_setcc_ret_block(equal->block, HB_CC_B, &setcc_dst))
        return;
    if (less && less->block && !setcc_ret_block(less->block, HB_CC_L, &setcc_dst))
        less = NULL;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_i32_less_tiebreaker_helper(
        cg, entry_block, equal->block, less && less->block ? less->block : NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, entry_block->guest_addr, dest, emitted_size,
                    (uint32_t)(entry_block->instr_count + equal->block->instr_count +
                               (less && less->block ? less->block->instr_count : 0)),
                    entry_block, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=i32-less-tiebreaker entry=%p equal=%p less=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)entry_block->guest_addr,
                (void*)(uintptr_t)equal->block->guest_addr,
                (void*)(uintptr_t)(less && less->block ? less->block->guest_addr : 0),
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool unity_sort_inner_block(const hb_ir_block_t* block, uint64_t* cont_addr) {
    if (!block || block->instr_count != 7) return false;
    const hb_ir_instr_t* i = block->instrs;

    if (i[0].op != HB_IR_LOAD ||
        !runtime_exact_reg_operand(&i[0].dst, HB_REG_RAX, HB_SIZE_64) ||
        !runtime_exact_mem_operand(&i[0].src1, HB_REG_RDI, HB_REG_COUNT, 1, 0, HB_SIZE_64))
        return false;
    if (i[1].op != HB_IR_MOV ||
        !runtime_exact_reg_operand(&i[1].dst, HB_REG_RCX, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[1].src1, HB_REG_R15, HB_SIZE_64))
        return false;
    if (i[2].op != HB_IR_STORE ||
        !runtime_exact_mem_operand(&i[2].src1, HB_REG_R14, HB_REG_COUNT, 1, 0, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[2].src2, HB_REG_RAX, HB_SIZE_64))
        return false;
    if (i[3].op != HB_IR_MOV ||
        !runtime_exact_reg_operand(&i[3].dst, HB_REG_R14, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[3].src1, HB_REG_RDI, HB_SIZE_64))
        return false;
    if (i[4].op != HB_IR_LOAD ||
        !runtime_exact_reg_operand(&i[4].dst, HB_REG_RDX, HB_SIZE_64) ||
        !runtime_exact_mem_operand(&i[4].src1, HB_REG_RDI, HB_REG_COUNT, 1, -8, HB_SIZE_64))
        return false;
    if (i[5].op != HB_IR_SUB ||
        !runtime_exact_reg_operand(&i[5].dst, HB_REG_RDI, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[5].src1, HB_REG_RDI, HB_SIZE_64) ||
        i[5].src2.type != HB_OP_IMM || i[5].src2.imm != 8)
        return false;
    if (i[6].op != HB_IR_CALL ||
        !runtime_exact_reg_operand(&i[6].src1, HB_REG_RBP, HB_SIZE_64))
        return false;

    if (cont_addr) *cont_addr = i[6].guest_addr + i[6].guest_len;
    return true;
}

static bool unity_sort_cont_block(const hb_ir_block_t* block,
                                  uint64_t sort_addr,
                                  uint64_t* fallthrough_addr) {
    if (!block || block->instr_count != 2) return false;
    const hb_ir_instr_t* i = block->instrs;

    if (i[0].op != HB_IR_TEST ||
        !runtime_exact_reg_operand(&i[0].src1, HB_REG_RAX, HB_SIZE_8) ||
        !runtime_exact_reg_operand(&i[0].src2, HB_REG_RAX, HB_SIZE_8))
        return false;
    if (i[1].op != HB_IR_Jcc || i[1].cc != HB_CC_NE || i[1].target != sort_addr)
        return false;
    if (fallthrough_addr) *fallthrough_addr = i[1].guest_addr + i[1].guest_len;
    return true;
}

static const hb_ir_block_t* find_unity_sort_inner_near_cont(hb_block_cache_t* cache,
                                                            const hb_ir_block_t* cont) {
    if (!cache || !cont) return NULL;
    for (uint64_t back = 1; back <= 64; back++) {
        hb_block_cache_entry_t* entry = block_cache_find(cache, cont->guest_addr - back);
        uint64_t candidate_cont = 0;
        if (entry && entry->block &&
            unity_sort_inner_block(entry->block, &candidate_cont) &&
            candidate_cont == cont->guest_addr)
            return entry->block;
    }
    return NULL;
}

static bool unity_sort_comparator_ready(hb_context_t* ctx,
                                        const hb_ir_block_t* cmp) {
    static const uint8_t unity_cmp_bytes[] = {
        0x8b, 0x02,             /* mov eax, [rdx] */
        0x39, 0x01,             /* cmp [rcx], eax */
        0x75, 0x07,             /* jne +7 */
        0x48, 0x3b, 0xca,       /* cmp rcx, rdx */
        0x0f, 0x92, 0xc0,       /* setb al */
        0xc3,                   /* ret */
        0x0f, 0x9c, 0xc0,       /* setl al */
        0xc3                    /* ret */
    };
    uint8_t bytes[sizeof(unity_cmp_bytes)];
    uint64_t equal_addr = 0;
    uint64_t less_addr = 0;
    if (!ctx || !ctx->memory || !cmp ||
        !load_cmp_jne_i32_entry_block(cmp, &equal_addr, &less_addr))
        return false;
    if (equal_addr != cmp->guest_addr + 0x06 || less_addr != cmp->guest_addr + 0x0d)
        return false;
    if (hb_memory_read(ctx->memory, cmp->guest_addr, bytes, sizeof(bytes)) != HB_OK)
        return false;
    return memcmp(bytes, unity_cmp_bytes, sizeof(bytes)) == 0;
}

static int trace_unity_sort_promote_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_UNITY_SORT_PROMOTE");
        if (env && *env && *env != '0') cached = 1;
        else cached = trace_jit_blocks_enabled();
    }
    return cached;
}

static void trace_unity_sort_promote(const char* reason,
                                     const hb_ir_block_t* block,
                                     const hb_ir_block_t* sort,
                                     const hb_ir_block_t* cont,
                                     const hb_ir_block_t* cmp,
                                     uint64_t rbp) {
    static unsigned count;
    if (!trace_unity_sort_promote_enabled() || count++ >= 2000)
        return;
    fprintf(stderr,
            "macrunner-hb-unity-sort-promote: reason=%s block=%p block_instrs=%zu "
            "sort=%p cont=%p cmp=%p rbp=%p\n",
            reason ? reason : "unknown",
            block ? (void*)(uintptr_t)block->guest_addr : NULL,
            block ? block->instr_count : 0,
            sort ? (void*)(uintptr_t)sort->guest_addr : NULL,
            cont ? (void*)(uintptr_t)cont->guest_addr : NULL,
            cmp ? (void*)(uintptr_t)cmp->guest_addr : NULL,
            (void*)(uintptr_t)rbp);
    fflush(stderr);
}

static void try_promote_unity_sort_inner_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                              const hb_ir_block_t* block) {
    const hb_ir_block_t* sort = NULL;
    const hb_ir_block_t* cont = NULL;
    hb_block_cache_entry_t* sort_entry = NULL;
    uint64_t cont_addr = 0;
    uint64_t ignored_fallthrough = 0;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block ||
        ctx->mode != HB_MODE_64BIT)
        return;

    if ((block->instr_count == 7 &&
         block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
        (block->instr_count == 2 &&
         block->instrs[block->instr_count - 1].op == HB_IR_Jcc)) {
        trace_unity_sort_promote("enter", block, NULL, NULL, NULL,
                                 ctx->regs.x64.rbp);
    }

    if (unity_sort_inner_block(block, &cont_addr)) {
        sort = block;
        hb_block_cache_entry_t* cont_entry = block_cache_find(rt->block_cache, cont_addr);
        if (!cont_entry || !cont_entry->block) {
            trace_unity_sort_promote("sort-cont-miss", block, sort, NULL, NULL,
                                     ctx->regs.x64.rbp);
            return;
        }
        cont = cont_entry->block;
    } else if (block->instr_count == 2 && block->instrs[1].op == HB_IR_Jcc) {
        sort = find_unity_sort_inner_near_cont(rt->block_cache, block);
        if (!sort || !unity_sort_inner_block(sort, &cont_addr) ||
            cont_addr != block->guest_addr) {
            trace_unity_sort_promote("guard-sort-miss", block, sort, block, NULL,
                                     ctx->regs.x64.rbp);
            return;
        }
        cont = block;
    } else {
        return;
    }

    if (!unity_sort_cont_block(cont, sort->guest_addr, &ignored_fallthrough)) {
        trace_unity_sort_promote("guard-shape-miss", block, sort, cont, NULL,
                                 ctx->regs.x64.rbp);
        return;
    }
    sort_entry = block_cache_find(rt->block_cache, sort->guest_addr);
    if (!sort_entry || sort_entry->fused) {
        trace_unity_sort_promote(sort_entry ? "sort-already-fused" : "sort-entry-miss",
                                 block, sort, cont, NULL, ctx->regs.x64.rbp);
        return;
    }

    hb_block_cache_entry_t* cmp_entry = block_cache_find(rt->block_cache, ctx->regs.x64.rbp);
    if (!cmp_entry || !cmp_entry->block) {
        trace_unity_sort_promote("cmp-entry-miss", block, sort, cont, NULL,
                                 ctx->regs.x64.rbp);
        return;
    }
    if (!unity_sort_comparator_ready(ctx, cmp_entry->block)) {
        trace_unity_sort_promote("cmp-shape-miss", block, sort, cont, cmp_entry->block,
                                 ctx->regs.x64.rbp);
        return;
    }
    trace_unity_sort_promote("emit", block, sort, cont, cmp_entry->block,
                             ctx->regs.x64.rbp);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_unity_sort_inner_loop_helper(
        cg, sort, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, sort->guest_addr, dest, emitted_size,
                    (uint32_t)(sort->instr_count + cont->instr_count),
                    sort, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr,
                "macrunner-hb-jit-fusion: kind=unity-sort-inner sort=%p cont=%p cmp=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)sort->guest_addr,
                (void*)(uintptr_t)cont->guest_addr,
                (void*)(uintptr_t)cmp_entry->block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool block_contains_lock_rmw_ir(const hb_ir_block_t* block) {
    if (!block) return false;
    for (size_t i = 0; i < block->instr_count; i++) {
        hb_ir_op_t op = block->instrs[i].op;
        if (op == HB_IR_CMPXCHG || op == HB_IR_CMPXCHG8B ||
            op == HB_IR_XCHG || op == HB_IR_XADD)
            return true;
    }
    return false;
}

static bool small_terminal_jcc_self_loop(const hb_ir_block_t* block) {
    if (!block || block->instr_count < 2 || block->instr_count > 16) return false;
    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (last->op != HB_IR_Jcc || last->target != block->guest_addr) return false;
    if (block_contains_lock_rmw_ir(block)) return false;
    for (size_t i = 0; i + 1 < block->instr_count; i++) {
        hb_ir_op_t op = block->instrs[i].op;
        if (op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
            op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ)
            return false;
    }
    return true;
}

static void try_promote_self_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                  const hb_ir_block_t* block) {
    bool has_lock_rmw = block_contains_lock_rmw_ir(block);
    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx ||
        has_lock_rmw || !small_terminal_jcc_self_loop(block))
        return;
    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, block->guest_addr);
    if (!entry || entry->fused) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, block, block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    hb_contract_telemetry_record_compile();
    block_cache_put(rt, rt->block_cache, block->guest_addr, dest, emitted_size,
                    (uint32_t)block->instr_count, block, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=self-loop block=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)block->guest_addr, dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static void try_promote_hot_block_families(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                           const hb_ir_block_t* block) {
    try_promote_copy_scan_counted_loop(rt, ctx, block);
    try_promote_bounded_scan_loop(rt, ctx, block);
    try_promote_byte_compare_loop(rt, ctx, block);
    try_promote_null_qword_scan_loop(rt, ctx, block);
    try_promote_i32_less_tiebreaker_comparator(rt, ctx, block);
    try_promote_unity_sort_inner_loop(rt, ctx, block);
    try_promote_self_loop(rt, ctx, block);
}

static bool should_retry_cached_promotion(const hb_block_cache_entry_t* entry) {
    if (!entry || entry->fused) return false;
    if (entry->hit_count < 4) return true;
    return (entry->hit_count & (entry->hit_count - 1)) == 0;
}

hb_result_t hb_jit_runtime_compile(hb_jit_runtime_t* rt, const hb_ir_func_t* func) {
    (void)rt; (void)func;
    /* Compilation is done on-demand per-block in hb_jit_runtime_run for MVP */
    return HB_OK;
}

static int macrunner_hb_jcc57fd_watch_enabled(void);

static hb_result_t hb_jit_runtime_run_legacy(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out) {
    if (!rt || !func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = rt->ctx;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;
    bool dispatch_stats_enabled_run = trace_dispatch_stats_enabled() != 0;
    ctx->last_result = HB_OK;
    if (dispatch_stats_enabled_run)
        dispatch_stats_register();

    while (1) {
        /* MacRunner 2026-06-23 (ABZU jcc pin): watch the load/test/jne block at
         * 0x14057fd10 and its jne targets 0x140580012 (taken, non-NULL path) and
         * 0x14057fd3d (not-taken, NULL path). Dumps rax + the next PC so we can see
         * whether the jne is taken despite a non-NULL global. Env-gated. */
        if (macrunner_hb_jcc57fd_watch_enabled()) {
            uint64_t pcw = ctx->pc;
            /* ABZU 0x142a00 module-init control-flow diff: log block entries across
             * 0x142a00, 0x50b0b0, 0x586630, 0x57fd10, and the golden return 0x145ec8. */
            /* MINIMAL low-overhead watch: only the post-vtable-#2 points + golden return.
             * 0x142a5e = block that issues vtable#2 (call [r9+0x20]); 0x142a6e = vtable#2 returned;
             * 0x142a72 = vtable#2 al=1 path; 0x142a85 = skip/next-module; 0x145ec8 = golden return. */
            /* 0x145bc0 post-0x145ec8 golden-path checks: find which branch diverges
             * (sends UE4 to the 0x14607b skip/abort path -> clean exit rc=1). */
            int hit = (pcw == 0x140145ec8ULL || pcw == 0x140145ecaULL || pcw == 0x140145edbULL ||
                       pcw == 0x140145eddULL || pcw == 0x140145ee4ULL || pcw == 0x140145eeaULL ||
                       pcw == 0x140145eefULL || pcw == 0x140145ef1ULL || pcw == 0x140145ef7ULL ||
                       pcw == 0x140145f0cULL || pcw == 0x140145f0eULL || pcw == 0x140145f11ULL ||
                       pcw == 0x140145f1aULL || pcw == 0x140145f4eULL || pcw == 0x14014607bULL ||
                       pcw == 0x1401476ffULL || pcw == 0x140142a00ULL);
            if (hit) {
                uint8_t flagb = 0xff;
                if (pcw == 0x140145eddULL) hb_memory_read_u8(ctx->memory, 0x142904c21ULL, &flagb);
                fprintf(stderr, "macrunner-hb-cfdiff: pc=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx sil=0x%02x flag[0x2904c21]=0x%02x r13=0x%llx r15=0x%llx rsp=0x%llx\n",
                        (unsigned long long)pcw, (unsigned long long)ctx->regs.x64.rax,
                        (unsigned long long)ctx->regs.x64.rbx, (unsigned long long)ctx->regs.x64.rcx,
                        (unsigned long long)ctx->regs.x64.rdx, (unsigned long long)ctx->regs.x64.rsi,
                        (unsigned)(ctx->regs.x64.rsi & 0xff), (unsigned)flagb,
                        (unsigned long long)ctx->regs.x64.r13, (unsigned long long)ctx->regs.x64.r15,
                        (unsigned long long)ctx->regs.x64.rsp);
            }
        }
        if (ctx->step_limit > 0 && steps >= ctx->step_limit) {
            out->result = HB_ERR_STEP_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            return set_runtime_fault_result(out, ctx, HB_ERR_BLOCK_LIMIT, steps,
                                            blocks_executed, "block limit reached");
        }

        hb_ir_block_t* block = find_block(func->cfg, ctx->pc);
        if (!block) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before current PC");
            }
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK; /* No block for PC — function exit or external call */
        }

        blocks_executed++;

        /* Check in-memory block cache */
        hb_block_cache_entry_t* cached = block_cache_find(rt->block_cache, ctx->pc);
        bool smc_evicted = false;
        cached = smc_reverify_entry(rt, cached, &smc_evicted);
        if (smc_evicted && smc_relift_should_exit(steps)) {
            /* Guest bytes changed under this translation: the caller's lifted IR is
             * stale too.  Exit so it re-lifts from current bytes. */
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (cached) {
            if (trace_jit_block_contains_guest(block, trace_jit_guest_addr())) {
                trace_unity_sort_promote("cache-hit-watch", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (block && ((block->instr_count == 7 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
                          (block->instr_count == 2 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_Jcc))) {
                trace_unity_sort_promote("cache-hit-gate", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (should_retry_cached_promotion(cached)) {
                const hb_ir_block_t* stable_block = cached->block ? cached->block : block;
                try_promote_hot_block_families(rt, ctx, stable_block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    /* MacRunner FIX#2a: non-latching (live block_cache_is_full gates). */
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", 0);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            trace_jit_cached_watch_block_once(rt, cached);
            hb_result_t run_result = run_jit_block_with_signal_guard(rt, cached, out,
                                                                      steps, blocks_executed);
            if (run_result != HB_OK || out->faulted) return run_result;
            trace_jit_hot_block_tick(rt, cached);
            steps += cached->steps;
            if (dispatch_stats_enabled_run)
                dispatch_stats_add(1, 1, cached->steps);
        } else {
            if (rt->code_cache_full || block_cache_is_full(rt->block_cache)) {
                /* MacRunner FIX#2a: do NOT latch code_cache_full here — the block-cache
                 * full state is already re-checked live via block_cache_is_full() in
                 * every JIT guard, so a transient hash-table fill no longer permanently
                 * disables JIT (which it did while the exec buffer was 82% free). The
                 * genuine hard limit (exec buffer full) still latches at jit_commit_blob. */
                trace_jit_code_cache_full_once(rt, "block-cache-full", 0);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            hb_cache_key_t persistent_key;
            bool have_persistent_key = false;
            bool loaded_from_persistent = false;
            uint8_t* dest = NULL;
            size_t emitted_size = 0;
            hb_result_t r;

            if (rt->persistent_cache &&
                persistent_cache_key_for_block(rt, block, &persistent_key) == HB_OK) {
                hb_cache_entry_t* disk_entry = NULL;
                have_persistent_key = true;
                r = hb_cache_lookup(rt->persistent_cache, &persistent_key, &disk_entry);
                if (r == HB_OK && disk_entry && disk_entry->valid) {
                    const uint8_t* load_code = NULL;
                    uint8_t* owned_load_code = NULL;
                    hb_ir_block_t* load_block = block_clone_for_cache(block);
                    emitted_size = disk_entry->native_size;
                    if (load_block &&
                        native_blob_prepare_cache_load(disk_entry->native_code,
                                                       disk_entry->native_size,
                                                       load_block, &load_code,
                                                       &owned_load_code) &&
                        (r = jit_commit_blob(rt, load_code, emitted_size, &dest)) == HB_OK) {
                        cached = block_cache_put(rt, rt->block_cache, ctx->pc, dest, emitted_size,
                                                 disk_entry->steps ? disk_entry->steps
                                                                    : jit_block_step_count(load_block),
                                                 load_block, false, true);
                        if (cached) {
                            load_block = NULL;
                            loaded_from_persistent = true;
                            hb_contract_telemetry_record_cache_hit(emitted_size);
                        }
                    }
                    free(owned_load_code);
                    if (load_block) hb_ir_block_destroy(load_block);
                } else {
                    hb_contract_telemetry_record_cache_miss();
                }
                hb_cache_entry_free(disk_entry);
            }

            if (!loaded_from_persistent) {
                hb_ir_block_t* compile_block = NULL;
                /* Compile block into codegen buffer */
                hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(4096);
                if (!code_buf) return HB_ERR_OUT_OF_MEMORY;
                compile_block = block_clone_for_cache(block);
                if (!compile_block) {
                    hb_codegen_buffer_destroy(code_buf);
                    return HB_ERR_OUT_OF_MEMORY;
                }

                hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
                if (!cg) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    return HB_ERR_OUT_OF_MEMORY;
                }

                r = hb_arm64_codegen_block_with_cfg(cg, compile_block, func->cfg, code_buf);
                hb_arm64_codegen_destroy(cg);
                if (r != HB_OK) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    out->result = r;
                    out->steps_executed = steps;
                    out->blocks_executed = blocks_executed;
                    out->faulted = true;
                    out->fault_reason = "JIT codegen failed";
                    return HB_OK;
                }

                emitted_size = code_buf->size;
                r = jit_commit_blob(rt, code_buf->code, emitted_size, &dest);
                if (r != HB_OK) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT code cache full; interpreter fallback");
                }
                hb_contract_telemetry_record_compile();

                if (rt->persistent_cache && have_persistent_key &&
                    code_buf->code && code_buf->size) {
                    const uint8_t* store_code = NULL;
                    uint8_t* owned_store_code = NULL;
                    hb_cache_entry_t metadata;
                    if (native_blob_prepare_cache_store(code_buf->code, code_buf->size,
                                                        compile_block, &store_code,
                                                        &owned_store_code)) {
                        memset(&metadata, 0, sizeof(metadata));
                        metadata.steps = jit_block_step_count(compile_block);
                        r = hb_cache_store(rt->persistent_cache, &persistent_key, store_code,
                                           code_buf->size, &metadata);
                        if (r == HB_OK) {
                            hb_contract_telemetry_record_cache_store(code_buf->size);
                        }
                        free(owned_store_code);
                    } else {
                        /* Attribute the skip: >1 helper call is the structural restriction in
                         * native_blob_single_arg_helper_stub; anything else is a single stub
                         * whose shape was not recognised. See native_blob_helper_call_count. */
                        if (native_blob_helper_call_count(code_buf->code, code_buf->size) > 1)
                            hb_contract_telemetry_record_cache_store_skip_multi();
                        else
                            hb_contract_telemetry_record_cache_store_skip_unmatched();
                        hb_contract_telemetry_record_cache_store_skip();
                    }
                } else if (rt->persistent_cache && have_persistent_key) {
                    hb_contract_telemetry_record_cache_store_skip();
                }
                hb_codegen_buffer_destroy(code_buf);

                /* Store in block cache */
                cached = block_cache_put(rt, rt->block_cache, ctx->pc, dest, emitted_size,
                                         jit_block_step_count(compile_block), compile_block,
                                         false, true);
                if (!cached) {
                    hb_ir_block_destroy(compile_block);
                    /* MacRunner FIX#2a: non-latching (live block_cache_is_full gates). */
                    trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                }
                trace_jit_block(ctx->pc, dest, emitted_size, block);
            } else if (trace_jit_blocks_enabled()) {
                fprintf(stderr, "macrunner-hb-translation-cache-hit: guest=%p native=%p-%p size=%zu\n",
                        (void*)(uintptr_t)ctx->pc, dest, dest + emitted_size, emitted_size);
                fflush(stderr);
            }

            if (!cached) {
                rt->code_cache_full = true;
                trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            if (!cached->fused) {
                const hb_ir_block_t* stable_block = cached->block ? cached->block : block;
                if (trace_jit_block_contains_guest(stable_block, trace_jit_guest_addr())) {
                    trace_unity_sort_promote("cache-put-watch", stable_block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                if (stable_block && ((stable_block->instr_count == 7 &&
                                       stable_block->instrs[stable_block->instr_count - 1].op == HB_IR_CALL) ||
                                      (stable_block->instr_count == 2 &&
                                       stable_block->instrs[stable_block->instr_count - 1].op == HB_IR_Jcc))) {
                    trace_unity_sort_promote("cache-put-gate", stable_block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                try_promote_hot_block_families(rt, ctx, stable_block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    rt->code_cache_full = true;
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", emitted_size);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            trace_jit_cached_watch_block_once(rt, cached);

            /* Execute */
            hb_result_t run_result = run_jit_block_with_signal_guard(rt, cached, out,
                                                                      steps, blocks_executed);
            if (run_result != HB_OK || out->faulted) return run_result;
            trace_jit_hot_block_tick(rt, cached);
            steps += cached->steps ? cached->steps : jit_block_step_count(block);
            if (dispatch_stats_enabled_run)
                dispatch_stats_add(1, 1, cached->steps ? cached->steps : jit_block_step_count(block));
        }
        sync_arch_pc_after_jit_block(ctx);
        trace_x86_low_pc_after_block(ctx, block, steps, blocks_executed);
        if (ctx->last_result != HB_OK) {
            if (trace_jit_helper_fault_enabled()) {
                static int t;
                if (t++ < 8)
                    fprintf(stderr, "macrunner-hb-block-fault-pc: pc=0x%llx last=%d guest_addr=0x%llx\n",
                            (unsigned long long)ctx->pc, (int)ctx->last_result,
                            block ? (unsigned long long)block->guest_addr : 0);
                trace_jit_helper_fault_block(ctx, block);
            }
            set_helper_fault_result(out, ctx, steps, blocks_executed);
            return HB_OK;
        }

        /* Determine if we should continue or stop */
        if (block->instr_count == 0) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
        const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
        const hb_ir_instr_t* terminal = transfer ? transfer : last;
        if (terminal->op == HB_IR_RET) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        /* For CALL/JMP/Jcc, PC was updated by JIT code; find next block */
        hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
        if (!next) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before branch target");
            }
            if (is_control_transfer_op(terminal->op)) {
                out->result = HB_OK;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK; /* External branch/call/return boundary */
            }
            out->result = HB_ERR_NOT_FOUND;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            out->faulted = true;
            out->fault_reason = "branch target block not found";
            return HB_OK;
        }
        if (terminal->op == HB_IR_JMP || terminal->op == HB_IR_Jcc ||
            terminal->op == HB_IR_CALL || terminal->op == HB_IR_LOOP ||
            terminal->op == HB_IR_JRCXZ) {
            /* Continue with the target block */
            continue;
        }

        /* Sequential block end — stop */
        ctx->pc = last->guest_addr + last->guest_len;
        if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
        else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
        next = find_block(func->cfg, ctx->pc);
        if (next) continue;
        out->result = HB_OK;
        out->steps_executed = steps;
        out->blocks_executed = blocks_executed;
        return HB_OK;
    }
}

hb_result_t hb_jit_runtime_run(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out) {
    int block_chain = runtime_block_chain_enabled();
    int single_lookup_gate = runtime_single_lookup_enabled();
    int indirect_ic_gate = runtime_indirect_ic_enabled();
    int dispatch_stats_gate = trace_dispatch_stats_enabled();
    static int dispatch_gate_trace_count;
    if (dispatch_gate_trace_count < 16 && trace_dispatch_gate_enabled()) {
        dispatch_gate_trace_count++;
        fprintf(stderr,
                "macrunner-hb-dispatch-gate: block_chain=%d single_lookup=%d "
                "indirect_ic=%d dispatch_stats=%d legacy=%d\n",
                block_chain, single_lookup_gate, indirect_ic_gate, dispatch_stats_gate,
                (!block_chain && !single_lookup_gate && !indirect_ic_gate));
        fflush(stderr);
    }
    if (!block_chain && !single_lookup_gate && !indirect_ic_gate)
        return hb_jit_runtime_run_legacy(rt, func, out);

    if (!rt || !func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = rt->ctx;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;
    bool chain_accounting = block_chain != 0;
    bool chain_patch_enabled = chain_accounting && ctx->step_limit == 0 && ctx->block_limit == 0;
    bool single_lookup = single_lookup_gate != 0;
    bool indirect_ic_enabled = indirect_ic_gate &&
                               ctx->step_limit == 0 && ctx->block_limit == 0;
    bool dispatch_fastpath = chain_accounting || single_lookup || indirect_ic_enabled;
    bool dispatch_stats_enabled_run = dispatch_stats_gate != 0;
    ctx->last_result = HB_OK;
    if (dispatch_stats_enabled_run)
        dispatch_stats_register();
    if (!indirect_ic_enabled) {
        ctx->indirect_ic_guest_addr = 0;
        ctx->indirect_ic_native_code = 0;
    }

    while (1) {
        /* MacRunner 2026-06-23 (ABZU jcc pin): watch the load/test/jne block at
         * 0x14057fd10 and its jne targets 0x140580012 (taken, non-NULL path) and
         * 0x14057fd3d (not-taken, NULL path). Dumps rax + the next PC so we can see
         * whether the jne is taken despite a non-NULL global. Env-gated. */
        if (macrunner_hb_jcc57fd_watch_enabled()) {
            uint64_t pcw = ctx->pc;
            /* ABZU 0x142a00 module-init control-flow diff: log block entries across
             * 0x142a00, 0x50b0b0, 0x586630, 0x57fd10, and the golden return 0x145ec8. */
            /* MINIMAL low-overhead watch: only the post-vtable-#2 points + golden return.
             * 0x142a5e = block that issues vtable#2 (call [r9+0x20]); 0x142a6e = vtable#2 returned;
             * 0x142a72 = vtable#2 al=1 path; 0x142a85 = skip/next-module; 0x145ec8 = golden return. */
            /* 0x145bc0 post-0x145ec8 golden-path checks: find which branch diverges
             * (sends UE4 to the 0x14607b skip/abort path -> clean exit rc=1). */
            int hit = (pcw == 0x140145ec8ULL || pcw == 0x140145ecaULL || pcw == 0x140145edbULL ||
                       pcw == 0x140145eddULL || pcw == 0x140145ee4ULL || pcw == 0x140145eeaULL ||
                       pcw == 0x140145eefULL || pcw == 0x140145ef1ULL || pcw == 0x140145ef7ULL ||
                       pcw == 0x140145f0cULL || pcw == 0x140145f0eULL || pcw == 0x140145f11ULL ||
                       pcw == 0x140145f1aULL || pcw == 0x140145f4eULL || pcw == 0x14014607bULL ||
                       pcw == 0x1401476ffULL || pcw == 0x140142a00ULL);
            if (hit) {
                uint8_t flagb = 0xff;
                if (pcw == 0x140145eddULL) hb_memory_read_u8(ctx->memory, 0x142904c21ULL, &flagb);
                fprintf(stderr, "macrunner-hb-cfdiff: pc=0x%llx rax=0x%llx rbx=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx sil=0x%02x flag[0x2904c21]=0x%02x r13=0x%llx r15=0x%llx rsp=0x%llx\n",
                        (unsigned long long)pcw, (unsigned long long)ctx->regs.x64.rax,
                        (unsigned long long)ctx->regs.x64.rbx, (unsigned long long)ctx->regs.x64.rcx,
                        (unsigned long long)ctx->regs.x64.rdx, (unsigned long long)ctx->regs.x64.rsi,
                        (unsigned)(ctx->regs.x64.rsi & 0xff), (unsigned)flagb,
                        (unsigned long long)ctx->regs.x64.r13, (unsigned long long)ctx->regs.x64.r15,
                        (unsigned long long)ctx->regs.x64.rsp);
            }
        }
        if (ctx->step_limit > 0 && steps >= ctx->step_limit) {
            out->result = HB_ERR_STEP_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            return set_runtime_fault_result(out, ctx, HB_ERR_BLOCK_LIMIT, steps,
                                            blocks_executed, "block limit reached");
        }

        hb_block_cache_entry_t* cached = NULL;
        hb_ir_block_t* block = NULL;
        bool smc_evicted = false;
        if (dispatch_fastpath) {
            cached = block_cache_find(rt->block_cache, ctx->pc);
            /* SMC reverify BEFORE borrowing cached->block: an eviction destroys
             * the owned block, so borrowing must happen after (or not at all). */
            cached = smc_reverify_entry(rt, cached, &smc_evicted);
            if (cached && cached->block)
                block = (hb_ir_block_t*)cached->block;
        }
        if (smc_evicted && smc_relift_should_exit(steps)) {
            /* Guest bytes changed under this translation: the caller's lifted IR is
             * stale too.  Exit so it re-lifts from current bytes (see the SMC re-lift
             * gate comment at smc_reverify_entry). */
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (!block)
            block = find_block(func->cfg, ctx->pc);
        if (!block) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before current PC");
            }
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK; /* No block for PC — function exit or external call */
        }

        if (!chain_accounting) blocks_executed++;

        /* Check in-memory block cache */
        if (!cached)
            cached = block_cache_find(rt->block_cache, ctx->pc);
        /* Non-fastpath resolves block from the CFG (not the entry), so reverify
         * here is borrow-safe; fastpath already reverified above. */
        if (!dispatch_fastpath) {
            cached = smc_reverify_entry(rt, cached, &smc_evicted);
            if (smc_evicted && smc_relift_should_exit(steps)) {
                out->result = HB_OK;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK;
            }
        }
        uint64_t run_block_delta = 1;
        if (cached) {
            if (trace_jit_block_contains_guest(block, trace_jit_guest_addr())) {
                trace_unity_sort_promote("cache-hit-watch", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (block && ((block->instr_count == 7 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
                          (block->instr_count == 2 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_Jcc))) {
                trace_unity_sort_promote("cache-hit-gate", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (should_retry_cached_promotion(cached)) {
                const hb_ir_block_t* stable_block = cached->block ? cached->block : block;
                try_promote_hot_block_families(rt, ctx, stable_block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    /* MacRunner FIX#2a: non-latching (live block_cache_is_full gates). */
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", 0);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            if (cached->block)
                block = (hb_ir_block_t*)cached->block;
            trace_jit_cached_watch_block_once(rt, cached);
            bool native_accounting = chain_accounting && entry_has_chain_slot(cached, NULL);
            uint64_t before_steps = native_accounting ? ctx->step_count : 0;
            uint64_t before_blocks = native_accounting ? ctx->block_count : 0;
            hb_result_t run_result = run_jit_block_with_signal_guard(rt, cached, out,
                                                                      steps, blocks_executed);
            if (run_result != HB_OK || out->faulted) return run_result;
            trace_jit_hot_block_tick(rt, cached);
            if (native_accounting) {
                uint64_t block_delta = ctx->block_count - before_blocks;
                uint64_t step_delta = ctx->step_count - before_steps;
                if (block_delta) {
                    run_block_delta = block_delta;
                    blocks_executed += block_delta;
                    steps += step_delta;
                    if (dispatch_stats_enabled_run) dispatch_stats_add(1, block_delta, step_delta);
                } else {
                    blocks_executed++;
                    steps += cached->steps;
                    if (dispatch_stats_enabled_run) dispatch_stats_add(1, 1, cached->steps);
                    native_accounting = false;
                }
            } else {
                steps += cached->steps;
                if (chain_accounting) blocks_executed++;
                if (dispatch_stats_enabled_run) dispatch_stats_add(1, 1, cached->steps);
            }
        } else {
            if (rt->code_cache_full || block_cache_is_full(rt->block_cache)) {
                /* MacRunner FIX#2a: do NOT latch code_cache_full here — the block-cache
                 * full state is already re-checked live via block_cache_is_full() in
                 * every JIT guard, so a transient hash-table fill no longer permanently
                 * disables JIT (which it did while the exec buffer was 82% free). The
                 * genuine hard limit (exec buffer full) still latches at jit_commit_blob. */
                trace_jit_code_cache_full_once(rt, "block-cache-full", 0);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            hb_cache_key_t persistent_key;
            bool have_persistent_key = false;
            bool loaded_from_persistent = false;
            uint8_t* dest = NULL;
            size_t emitted_size = 0;
            hb_result_t r;

            if (rt->persistent_cache &&
                persistent_cache_key_for_block(rt, block, &persistent_key) == HB_OK) {
                hb_cache_entry_t* disk_entry = NULL;
                have_persistent_key = true;
                r = hb_cache_lookup(rt->persistent_cache, &persistent_key, &disk_entry);
                if (r == HB_OK && disk_entry && disk_entry->valid) {
                    const uint8_t* load_code = NULL;
                    uint8_t* owned_load_code = NULL;
                    hb_ir_block_t* load_block = block_clone_for_cache(block);
                    emitted_size = disk_entry->native_size;
                    if (load_block &&
                        native_blob_prepare_cache_load(disk_entry->native_code,
                                                       disk_entry->native_size,
                                                       load_block, &load_code,
                                                       &owned_load_code) &&
                        (r = jit_commit_blob(rt, load_code, emitted_size, &dest)) == HB_OK) {
                        cached = block_cache_put(rt, rt->block_cache, ctx->pc, dest, emitted_size,
                                                 disk_entry->steps ? disk_entry->steps
                                                                    : jit_block_step_count(load_block),
                                                 load_block, false, true);
                        if (cached) {
                            load_block = NULL;
                            loaded_from_persistent = true;
                            hb_contract_telemetry_record_cache_hit(emitted_size);
                        }
                    }
                    free(owned_load_code);
                    if (load_block) hb_ir_block_destroy(load_block);
                } else {
                    hb_contract_telemetry_record_cache_miss();
                }
                hb_cache_entry_free(disk_entry);
            }

            if (!loaded_from_persistent) {
                hb_ir_block_t* compile_block = NULL;
                /* Compile block into codegen buffer */
                hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(4096);
                if (!code_buf) return HB_ERR_OUT_OF_MEMORY;
                compile_block = block_clone_for_cache(block);
                if (!compile_block) {
                    hb_codegen_buffer_destroy(code_buf);
                    return HB_ERR_OUT_OF_MEMORY;
                }

                hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
                if (!cg) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    return HB_ERR_OUT_OF_MEMORY;
                }

                r = hb_arm64_codegen_block_with_cfg(cg, compile_block, func->cfg, code_buf);
                hb_arm64_codegen_destroy(cg);
                if (r != HB_OK) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    out->result = r;
                    out->steps_executed = steps;
                    out->blocks_executed = blocks_executed;
                    out->faulted = true;
                    out->fault_reason = "JIT codegen failed";
                    return HB_OK;
                }

                emitted_size = code_buf->size;
                r = jit_commit_blob(rt, code_buf->code, emitted_size, &dest);
                if (r != HB_OK) {
                    hb_ir_block_destroy(compile_block);
                    hb_codegen_buffer_destroy(code_buf);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT code cache full; interpreter fallback");
                }
                hb_contract_telemetry_record_compile();

                if (rt->persistent_cache && have_persistent_key &&
                    code_buf->code && code_buf->size) {
                    const uint8_t* store_code = NULL;
                    uint8_t* owned_store_code = NULL;
                    hb_cache_entry_t metadata;
                    if (native_blob_prepare_cache_store(code_buf->code, code_buf->size,
                                                        compile_block, &store_code,
                                                        &owned_store_code)) {
                        memset(&metadata, 0, sizeof(metadata));
                        metadata.steps = jit_block_step_count(compile_block);
                        r = hb_cache_store(rt->persistent_cache, &persistent_key, store_code,
                                           code_buf->size, &metadata);
                        if (r == HB_OK) {
                            hb_contract_telemetry_record_cache_store(code_buf->size);
                        }
                        free(owned_store_code);
                    } else {
                        /* Attribute the skip: >1 helper call is the structural restriction in
                         * native_blob_single_arg_helper_stub; anything else is a single stub
                         * whose shape was not recognised. See native_blob_helper_call_count. */
                        if (native_blob_helper_call_count(code_buf->code, code_buf->size) > 1)
                            hb_contract_telemetry_record_cache_store_skip_multi();
                        else
                            hb_contract_telemetry_record_cache_store_skip_unmatched();
                        hb_contract_telemetry_record_cache_store_skip();
                    }
                } else if (rt->persistent_cache && have_persistent_key) {
                    hb_contract_telemetry_record_cache_store_skip();
                }
                hb_codegen_buffer_destroy(code_buf);

                /* Store in block cache */
                cached = block_cache_put(rt, rt->block_cache, ctx->pc, dest, emitted_size,
                                         jit_block_step_count(compile_block), compile_block,
                                         false, true);
                if (!cached) {
                    hb_ir_block_destroy(compile_block);
                    /* MacRunner FIX#2a: non-latching (live block_cache_is_full gates). */
                    trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                }
                trace_jit_block(ctx->pc, dest, emitted_size, block);
            } else if (trace_jit_blocks_enabled()) {
                fprintf(stderr, "macrunner-hb-translation-cache-hit: guest=%p native=%p-%p size=%zu\n",
                        (void*)(uintptr_t)ctx->pc, dest, dest + emitted_size, emitted_size);
                fflush(stderr);
            }

            if (!cached) {
                rt->code_cache_full = true;
                trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            if (!cached->fused) {
                const hb_ir_block_t* stable_block = cached->block ? cached->block : block;
                if (trace_jit_block_contains_guest(stable_block, trace_jit_guest_addr())) {
                    trace_unity_sort_promote("cache-put-watch", stable_block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                if (stable_block && ((stable_block->instr_count == 7 &&
                                       stable_block->instrs[stable_block->instr_count - 1].op == HB_IR_CALL) ||
                                      (stable_block->instr_count == 2 &&
                                       stable_block->instrs[stable_block->instr_count - 1].op == HB_IR_Jcc))) {
                    trace_unity_sort_promote("cache-put-gate", stable_block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                try_promote_hot_block_families(rt, ctx, stable_block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    rt->code_cache_full = true;
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", emitted_size);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            if (cached->block)
                block = (hb_ir_block_t*)cached->block;
            trace_jit_cached_watch_block_once(rt, cached);

            /* Execute */
            bool native_accounting = chain_accounting && entry_has_chain_slot(cached, NULL);
            uint64_t before_steps = native_accounting ? ctx->step_count : 0;
            uint64_t before_blocks = native_accounting ? ctx->block_count : 0;
            hb_result_t run_result = run_jit_block_with_signal_guard(rt, cached, out,
                                                                      steps, blocks_executed);
            if (run_result != HB_OK || out->faulted) return run_result;
            trace_jit_hot_block_tick(rt, cached);
            if (native_accounting) {
                uint64_t block_delta = ctx->block_count - before_blocks;
                uint64_t step_delta = ctx->step_count - before_steps;
                if (block_delta) {
                    run_block_delta = block_delta;
                    blocks_executed += block_delta;
                    steps += step_delta;
                    if (dispatch_stats_enabled_run) dispatch_stats_add(1, block_delta, step_delta);
                } else {
                    blocks_executed++;
                    steps += cached->steps ? cached->steps : jit_block_step_count(block);
                    if (dispatch_stats_enabled_run)
                        dispatch_stats_add(1, 1, cached->steps ? cached->steps : jit_block_step_count(block));
                }
            } else {
                steps += cached->steps ? cached->steps : jit_block_step_count(block);
                if (chain_accounting) blocks_executed++;
                if (dispatch_stats_enabled_run)
                    dispatch_stats_add(1, 1, cached->steps ? cached->steps : jit_block_step_count(block));
            }
        }
        sync_arch_pc_after_jit_block(ctx);
        trace_x86_low_pc_after_block(ctx, block, steps, blocks_executed);
        if (ctx->last_result != HB_OK) {
            if (trace_jit_helper_fault_enabled()) {
                static int t;
                if (t++ < 8)
                    fprintf(stderr, "macrunner-hb-block-fault-pc: pc=0x%llx last=%d guest_addr=0x%llx\n",
                            (unsigned long long)ctx->pc, (int)ctx->last_result,
                            block ? (unsigned long long)block->guest_addr : 0);
                trace_jit_helper_fault_block(ctx, block);
            }
            set_helper_fault_result(out, ctx, steps, blocks_executed);
            return HB_OK;
        }
        if (chain_accounting && run_block_delta > 1) {
            continue;
        }

        /* Determine if we should continue or stop */
        if (block->instr_count == 0) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
        const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
        const hb_ir_instr_t* terminal = transfer ? transfer : last;
        if (terminal->op == HB_IR_RET) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        /* For CALL/JMP/Jcc, PC was updated by JIT code; resolve the next block once. */
        hb_block_cache_entry_t* next_cached = NULL;
        hb_ir_block_t* next;
        if (dispatch_fastpath) {
            next_cached = block_cache_find(rt->block_cache, ctx->pc);
            next = (next_cached && next_cached->block)
                ? (hb_ir_block_t*)next_cached->block
                : find_block(func->cfg, ctx->pc);
        } else {
            next = find_block(func->cfg, ctx->pc);
        }
        if (!next) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before branch target");
            }
            if (is_control_transfer_op(terminal->op)) {
                out->result = HB_OK;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK; /* External branch/call/return boundary */
            }
            out->result = HB_ERR_NOT_FOUND;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            out->faulted = true;
            out->fault_reason = "branch target block not found";
            return HB_OK;
        }
        if (terminal->op == HB_IR_JMP || terminal->op == HB_IR_Jcc ||
            terminal->op == HB_IR_CALL || terminal->op == HB_IR_LOOP ||
            terminal->op == HB_IR_JRCXZ) {
            if (chain_patch_enabled && cached && next_cached)
                (void)patch_block_tail(rt, cached, next_cached);
            if (indirect_ic_enabled &&
                (terminal->op == HB_IR_JMP || terminal->op == HB_IR_CALL) &&
                terminal->src1.type != HB_OP_NONE)
                update_indirect_ic(ctx, next_cached, true);
            /* Continue with the target block */
            continue;
        }

        /* Sequential block end — stop */
        ctx->pc = last->guest_addr + last->guest_len;
        if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
        else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
        if (dispatch_fastpath) {
            next_cached = block_cache_find(rt->block_cache, ctx->pc);
            next = (next_cached && next_cached->block)
                ? (hb_ir_block_t*)next_cached->block
                : find_block(func->cfg, ctx->pc);
        } else {
            next = find_block(func->cfg, ctx->pc);
        }
        if (next) continue;
        out->result = HB_OK;
        out->steps_executed = steps;
        out->blocks_executed = blocks_executed;
        return HB_OK;
    }
}


static int macrunner_hb_jcc57fd_watch_enabled(void) {
    static int cache = -1;
    int v = __atomic_load_n(&cache, __ATOMIC_RELAXED);
    if (v < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_JCC57FD");
        v = e && e[0] && e[0] != '0';
        __atomic_store_n(&cache, v, __ATOMIC_RELAXED);
    }
    return v;
}
hb_result_t hb_runtime_run(hb_context_t* ctx, const hb_ir_func_t* func, hb_backend_t backend, hb_exec_result_t* out) {
    if (!ctx || !func || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));
    switch (backend) {
        case HB_BACKEND_INTERP: {
            hb_interpreter_t* i = hb_interpreter_create(ctx);
            if (!i) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_interpreter_run(i, func, out);
            hb_interpreter_destroy(i);
            return r;
        }
        case HB_BACKEND_JIT:
        case HB_BACKEND_AOT: {
            hb_jit_runtime_t* rt = hb_jit_runtime_create(ctx);
            if (!rt) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_jit_runtime_run(rt, func, out);
            hb_jit_runtime_destroy(rt);
            return r;
        }
    }
    return HB_ERR_INVALID_ARG;
}
