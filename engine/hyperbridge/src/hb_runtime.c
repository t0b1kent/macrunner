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
/* Key bit distinguishing entries written by the table-driven store path from the legacy one. */
#define HB_PERSIST_FLAG_RELOC            0x80u

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
    /* Whether `snapshot` above holds a real pre-image. A frame field rather than a local because
     * the recovery path reads it after siglongjmp, where a non-volatile local is indeterminate. */
    bool snapshot_valid;
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

/* MacRunner 2026-08-01 — how often is the fault-recovery branch actually TAKEN? Deliberately
 * ungated.
 *
 * Every dispatch pays for that branch in advance: a 790-byte frame memset, a 760-byte
 * hb_ctx_snapshot_save and a sigsetjmp, so that `hb_ctx_snapshot_restore` can roll the guest back
 * if the block faults. Whether that price is worth paying is one number — recoveries per
 * dispatch — and nothing in the tree reports it.
 *
 * The existing observation that it is nearly free ("20 480 faults and ripmap_check printed
 * nothing") is not evidence: ripmap_check sits behind MACRUNNER_HB_RIPMAP and prints once per
 * 1024 calls, so its silence is equally consistent with the gate being off. That is the trap this
 * lane hit three times already — a zero from an instrument nobody proved was switched on. Hence:
 * no env gate, no dependency on trace_dispatch_stats_enabled(), counted on the same line as the
 * dispatches it is a ratio of.
 *
 * Cost is an increment and a mask test per dispatch against a 760-byte copy already there, and one
 * fprintf per 2^20 dispatches. Per-thread counters, folded into a global only at flush time, so 64
 * threads do not contend on a cache line every dispatch. Printed periodically rather than at exit
 * because these runs die on the timeout's SIGKILL and never reach atexit. */
static uint64_t g_guard_dispatch_total;
static uint64_t g_guard_recover_total;

/* The live wire beside the zero — see guard_census_flush.
 *
 * A recovery count of zero is only evidence if the instrument could have moved, and the honest
 * way to show that is a counter on the SAME mechanism that does. These sit on the fault-claim
 * entry point, one fault apart from the recovery branch: claim_calls counts every time the signal
 * handler consults the guard, and claim_taken every time it hands control to siglongjmp. Faults
 * run at ~2000/s, five orders below the dispatch rate, so plain atomics are affordable here in a
 * way they would not be on the dispatch path. */
static uint64_t g_guard_claim_calls;
static uint64_t g_guard_claim_declined_frame;
static uint64_t g_guard_claim_declined_range;
static uint64_t g_guard_claim_taken;
static __thread uint64_t t_guard_dispatch;
static __thread uint64_t t_guard_recover;
static __thread uint64_t t_guard_flushed_dispatch;
static __thread uint64_t t_guard_flushed_recover;

/* Power of two: the hot-path test below is a mask, not a division. */
#define HB_GUARD_CENSUS_PERIOD (1ull << 20)

static void guard_census_flush(const char* why) {
    uint64_t d_add = t_guard_dispatch - t_guard_flushed_dispatch;
    uint64_t r_add = t_guard_recover - t_guard_flushed_recover;
    uint64_t d_tot, r_tot;

    t_guard_flushed_dispatch = t_guard_dispatch;
    t_guard_flushed_recover = t_guard_recover;
    d_tot = d_add ? __atomic_add_fetch(&g_guard_dispatch_total, d_add, __ATOMIC_RELAXED)
                  : __atomic_load_n(&g_guard_dispatch_total, __ATOMIC_RELAXED);
    r_tot = r_add ? __atomic_add_fetch(&g_guard_recover_total, r_add, __ATOMIC_RELAXED)
                  : __atomic_load_n(&g_guard_recover_total, __ATOMIC_RELAXED);

    /* recover_per_1e6 is the whole point: it is the share of dispatches whose snapshot was used
     * for anything, scaled so a cold branch is readable instead of rounding to 0.00 %. */
    fprintf(stderr,
            "macrunner-hb-guard-census: why=%s thread_dispatch=%llu thread_recover=%llu "
            "thread_recover_per_1e6=%.3f total_dispatch=%llu total_recover=%llu "
            "total_recover_per_1e6=%.3f claim_calls=%llu claim_taken=%llu "
            "claim_declined_frame=%llu claim_declined_range=%llu\n",
            why,
            (unsigned long long)t_guard_dispatch, (unsigned long long)t_guard_recover,
            t_guard_dispatch ? 1000000.0 * (double)t_guard_recover / (double)t_guard_dispatch : 0.0,
            (unsigned long long)d_tot, (unsigned long long)r_tot,
            d_tot ? 1000000.0 * (double)r_tot / (double)d_tot : 0.0,
            (unsigned long long)__atomic_load_n(&g_guard_claim_calls, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_guard_claim_taken, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_guard_claim_declined_frame, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_guard_claim_declined_range, __ATOMIC_RELAXED));
}

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

/* Terminal-op histogram slots — declared here because dispatch_stats_flush_thread below reports them;
 * the classifier that fills them, and the reasoning for classifying on the LAST instruction, are at
 * dispatch_stats_note_terminal. */
enum {
    HB_TERM_JMP_DIR = 0, HB_TERM_JMP_IND, HB_TERM_JCC, HB_TERM_CALL_DIR, HB_TERM_CALL_IND,
    HB_TERM_RET, HB_TERM_LOOP, HB_TERM_XFER_MID, HB_TERM_OTHER, HB_TERM_N
};
static const char* const hb_term_slot_names[HB_TERM_N] = {
    "jmp_dir", "jmp_ind", "jcc", "call_dir", "call_ind", "ret", "loop", "xfer_mid", "other"
};
static __thread uint64_t t_dispatch_term[HB_TERM_N];

/* Scan-length accounting for the O(N) CFG lookup every dispatch opens with — see find_block. */
static __thread uint64_t t_findblock_calls;
static __thread uint64_t t_findblock_iters;
static __thread uint64_t t_findblock_max_n;

/* MacRunner 2026-07-30 — WHY CHAINING DECLINES, counted per reason instead of guessed.
 *
 * The W^X fix stopped chaining from killing the guest, but avg_chain stayed at exactly 1.0000 over
 * 450 000 dispatches: the mechanism is safe and inert. patch_block_tail has eleven separate ways to
 * return false and the dispatcher can also decline to call it at all, so naming the culprit by reading is
 * a guess. One counter per exit tells it from a single run, which is the same discipline that turned the
 * dispatcher question into avg_chain rather than an eight-run A/B.
 *
 * `site_*` covers the call site (chain_patch_enabled is gated on ctx->step_limit and ctx->block_limit
 * being zero, so a title that sets either never patches at all), the rest are patch_block_tail's exits in
 * source order. Per-thread and non-atomic, like every other counter here. */
enum {
    CHAIN_SITE_CALLED = 0, CHAIN_SITE_PATCH_OFF, CHAIN_SITE_NO_ENTRY,
    CHAIN_DECL_GATE, CHAIN_DECL_INVALID, CHAIN_DECL_NOMETA, CHAIN_DECL_ALREADY,
    CHAIN_DECL_TERMINAL, CHAIN_DECL_BACKEDGE, CHAIN_DECL_SLOT_CUR, CHAIN_DECL_SLOT_NEXT,
    CHAIN_DECL_TRAMP, CHAIN_DECL_REACH, CHAIN_DECL_WRITEGATE, CHAIN_DECL_WPROT,
    CHAIN_DECL_XPROT, CHAIN_DECL_CAPPED, CHAIN_DECL_REFUSED, CHAIN_DECL_PATCHED, CHAIN_DECL_N
};
static const char* const hb_chain_decline_names[CHAIN_DECL_N] = {
    "site_called", "site_patch_off", "site_no_entry",
    "gate", "invalid", "nometa", "already",
    "terminal", "backedge", "slot_cur", "slot_next",
    "tramp", "reach", "write_off", "wprot",
    "xprot", "capped", "refused_near", "PATCHED"
};
static __thread uint64_t t_chain_decline[CHAIN_DECL_N];

/* Defined with the other block helpers further down; needed by the classifier above it. */
static const hb_ir_instr_t* first_control_transfer_instr(const hb_ir_block_t* block);

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

/* MacRunner 2026-07-30 — register ONCE PER THREAD, because this is called from dispatch_stats_add on
 * every single dispatch and everything below it is once-only work behind a CAS. The unconditional
 * runtime_now_ns() ahead of that CAS therefore bought a clock_gettime per dispatch: measured at 21 of
 * 536 samples, 3.9 % of the critical thread, in the SPEEDDIG1 profile
 * (dispatch_stats_add -> clock_gettime). An instrument that charges 4 % for being switched on makes
 * every timing arm it appears in pessimistic, which is the opposite of its job. */
static __thread int t_dispatch_stats_registered;

static void dispatch_stats_register(void) {
    uint64_t now;
    int expected = 0;
    if (!trace_dispatch_stats_enabled()) return;
    if (t_dispatch_stats_registered) return;
    t_dispatch_stats_registered = 1;
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
    /* avg_chain is thread_blocks/thread_dispatches: exactly 1.00 when every block transition returns
     * to the dispatcher, above 1 as chaining takes hold. Printed here so it needs no arithmetic at
     * read time, and per-thread so a parked thread cannot dilute the critical one. The terminal
     * histogram beside it is the ceiling — see dispatch_stats_note_terminal. */
    {
        uint64_t d = t_dispatch_stats_dispatches;
        uint64_t chainable = t_dispatch_term[HB_TERM_JMP_DIR];
        uint64_t classified = 0;
        int i;
        for (i = 0; i < HB_TERM_N; i++) classified += t_dispatch_term[i];
        fprintf(stderr, "macrunner-hb-chainlen: thread_dispatches=%llu thread_blocks=%llu "
                        "avg_chain=%.4f chainable_pct=%.2f",
                (unsigned long long)d, (unsigned long long)t_dispatch_stats_blocks,
                d ? (double)t_dispatch_stats_blocks / (double)d : 0.0,
                classified ? 100.0 * (double)chainable / (double)classified : 0.0);
        for (i = 0; i < HB_TERM_N; i++)
            fprintf(stderr, " %s=%llu", hb_term_slot_names[i],
                    (unsigned long long)t_dispatch_term[i]);
        fprintf(stderr, "\n");
        /* Why chaining declined, per reason — see the enum at hb_chain_decline_names. Printed only when
         * something has been counted, so a non-chaining run does not carry a line of zeros. */
        {
            uint64_t any = 0;
            for (i = 0; i < CHAIN_DECL_N; i++) any += t_chain_decline[i];
            if (any) {
                fprintf(stderr, "macrunner-hb-chaindecline:");
                for (i = 0; i < CHAIN_DECL_N; i++)
                    if (t_chain_decline[i])
                        fprintf(stderr, " %s=%llu", hb_chain_decline_names[i],
                                (unsigned long long)t_chain_decline[i]);
                fprintf(stderr, "\n");
            }
        }
        /* The O(N) CFG scan every dispatch opens with on the default path — see find_block. */
        fprintf(stderr, "macrunner-hb-findblock: thread_calls=%llu thread_iters=%llu avg_scan=%.2f "
                        "max_block_count=%llu scans_per_dispatch=%.2f\n",
                (unsigned long long)t_findblock_calls, (unsigned long long)t_findblock_iters,
                t_findblock_calls ? (double)t_findblock_iters / (double)t_findblock_calls : 0.0,
                (unsigned long long)t_findblock_max_n,
                d ? (double)t_findblock_calls / (double)d : 0.0);
    }
    fflush(stderr);
}

/* MacRunner 2026-07-30 — TERMINAL-OP HISTOGRAM of dispatched blocks: the chaining CEILING as a
 * number rather than an argument.
 *
 * blocks/dispatches (already reported above as thread_blocks/thread_dispatches) says whether chaining
 * is happening. It does not say how much chaining could ever happen, and that is the figure which
 * decides whether the dispatcher round trip is worth attacking this way at all: block_terminal_is_chainable
 * admits ONLY a direct HB_IR_JMP, so every dispatched block whose terminal is Jcc, RET, an indirect
 * jump or a call is ineligible no matter how well the trampoline works. In compiled x86 the executed
 * terminals are dominated by Jcc backedges and CALL/RET, so the ceiling may be far below the 297-339
 * of ~535 samples the profile attributes to dispatch overhead.
 *
 * Classified on the LAST instruction, deliberately: that is the exact expression
 * block_terminal_is_chainable uses, so jmp_dir is the eligible population and not an approximation of
 * it. xfer_mid counts blocks whose first control transfer is NOT the last instruction, where the
 * last-instruction model does not hold at all — reported separately so it cannot quietly inflate any
 * other bucket. Per-thread and non-atomic, following the t_dispatch_stats_* convention right above:
 * summing over 64 threads would mix in the parked ones, and rule two here is that the critical thread
 * is the measurement. */
static int trace_null_pc_enabled_rt(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_NULL_PC");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static void dispatch_stats_note_terminal(const hb_ir_block_t* block) {
    const hb_ir_instr_t* last;
    const hb_ir_instr_t* transfer;
    int slot;

    if (!trace_dispatch_stats_enabled()) return;
    if (!block || block->instr_count == 0) {
        t_dispatch_term[HB_TERM_OTHER]++;
        return;
    }
    last = &block->instrs[block->instr_count - 1];
    transfer = first_control_transfer_instr(block);
    if (transfer && transfer != last) {
        t_dispatch_term[HB_TERM_XFER_MID]++;
        return;
    }
    switch (last->op) {
    case HB_IR_JMP:
        slot = (last->src1.type == HB_OP_NONE) ? HB_TERM_JMP_DIR : HB_TERM_JMP_IND;
        break;
    case HB_IR_CALL:
        slot = (last->src1.type == HB_OP_NONE) ? HB_TERM_CALL_DIR : HB_TERM_CALL_IND;
        break;
    case HB_IR_Jcc:    slot = HB_TERM_JCC; break;
    case HB_IR_RET:    slot = HB_TERM_RET; break;
    case HB_IR_LOOP:
    case HB_IR_JRCXZ:  slot = HB_TERM_LOOP; break;
    default:           slot = HB_TERM_OTHER; break;
    }
    t_dispatch_term[slot]++;
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
    /* This path fills `instrs` by memcpy rather than through hb_ir_emit, so it must carry the
     * memo itself. The copy is instruction-for-instruction identical, so the source's answer is
     * valid for it; if the source was never asked, UNCOMPUTED propagates and the clone resolves it
     * on first use. Cached blocks are the ones the dispatcher actually sees, so getting this wrong
     * would be invisible in translation and wrong at run time. */
    copy->first_transfer_idx = block->first_transfer_idx;
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

/* Defined next to chain_trampoline_build, which explains the whole arrangement; declared here
 * because eviction is the other half of it and comes first in this file. */
static uint64_t* chain_trampoline_slot(uint8_t* tramp);
static uint8_t* chain_trampoline_bailout(uint8_t* tramp);

static void block_cache_prepare_replace_entry(hb_jit_runtime_t* rt, hb_block_cache_t* cache,
                                              hb_block_cache_entry_t* entry) {
    int made_writable = 0;

    if (!entry || !entry->valid) return;
    /* MacRunner 2026-07-30 — the eviction literal store USED TO HAPPEN OUTSIDE THE WRITABLE BRACKET, and
     * that is the same defect class already fixed in chain_trampoline_for: a plain store into the JIT arena
     * while it is mapped read+execute.
     *
     * It fires exactly when a trampoline exists for the evicted block, which is precisely the variable the
     * bisection isolated. MACRUNNER_HB_CHAIN_PATCH=0 boots because it never creates a trampoline, so
     * `self->in_trampoline` is always NULL and this store never runs; every arm that creates one wedges —
     * including CHAIN_WRITE=0, which installs no tail patches at all and therefore rules out both the patch
     * and the chained execution as causes. Note the W^X cycle below runs in the booting arm too (it is gated
     * only on runtime_block_chain_enabled()), so the cycle itself was never the difference — the unbracketed
     * store was.
     *
     * Both writes now share ONE bracket. The ordering the original comment cared about is preserved inside
     * it: the literal is retired BEFORE the unchain walk, so the window in which a predecessor could still
     * enter dead code stays closed. */
    if (runtime_block_chain_enabled() && rt && rt->jit_mem && cache &&
        hb_jit_buffer_make_writable(rt->jit_mem) == HB_OK) {
        hb_block_chain_meta_t* self = block_cache_chain_meta(cache, entry, false);
        made_writable = 1;
        /* One store retires every inbound chain at once. A literal is data, so no i-cache maintenance is
         * needed for it — but it is still arena memory and still needs the arena writable. */
        if (self && self->in_trampoline) {
            uint64_t* slot = chain_trampoline_slot(self->in_trampoline);
            uint8_t* bail = chain_trampoline_bailout(self->in_trampoline);
            if (slot && bail)
                __atomic_store_n(slot, (uint64_t)(uintptr_t)bail, __ATOMIC_RELEASE);
        }
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
    /* The redesign this was waiting for has landed: the single-slot raw tail patch became a per-target
     * trampoline whose eviction is one 8-byte store (f17c5202), and the branch is now guarded on
     * ctx->pc, so a mispredicted successor returns to the dispatcher instead of executing the wrong
     * guest block. Default stays 0 until a measured run moves it — avg_chain (dispatch_stats) is the
     * number that decides, and it has been exactly 1.0000 while this is off. */
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_BLOCK_CHAIN", 0);
}

static int runtime_single_lookup_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SINGLE_LOOKUP", 0);
}

/* MacRunner 2026-07-30 — BISECT HANDLE for the chaining exit=5.
 *
 * Arming MACRUNNER_HB_BLOCK_CHAIN changes three things at once, which is why 5/5 runs dying told us
 * nothing about WHICH: at emit time it adds a 4-NOP chain slot and 7 instructions of block/step counter
 * accounting to every block (hb_arm64_codegen.c, same env var), and at run time it lets
 * patch_block_tail rewrite block tails to branch through a trampoline.
 *
 * Setting MACRUNNER_HB_CHAIN_PATCH=0 keeps the whole emit side and disables only the run-time patching.
 * If a run then BOOTS, the emitted shape is innocent and the defect is in the patch/trampoline/eviction
 * machinery; if it still dies at exit=5, the defect is in the emitted block itself — the slot or the
 * counter accounting — which would be the more surprising answer and worth knowing before touching the
 * trampoline again. One 45-second run decides it, because these deaths are fast. */
static int runtime_chain_patch_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_PATCH", 1);
}

/* Second half of that bisect, now that CHAIN_PATCH=0 has cleared the emit side (it booted to
 * Begin MonoManager at +61.3 s and 19.15 M dispatches, where every patching arm dies at exit=5 within a
 * second of the first dispatch).
 *
 * The patch path still does two separable things: it lazily COMMITS a 128-byte trampoline into the JIT
 * arena for the target, and it WRITES two instructions over the predecessor's chain slot — i.e. it
 * modifies code that other threads may be executing, on a W^X MAP_JIT mapping, with i-cache maintenance.
 * MACRUNNER_HB_CHAIN_WRITE=0 keeps the trampoline commit and skips only that write, so a boot tells us
 * the arena allocation is fine and the live-code modification is fatal, while another exit=5 points at
 * committing into the arena while the guest runs. */
static int runtime_chain_write_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_WRITE", 1);
}

/* MacRunner 2026-07-30 — third bisect handle, for the wedge that appears once chaining actually engages.
 *
 * With the slot predicate fixed, chaining works (avg_chain 5.19, 12033 tails patched) and the boot wedges
 * deterministically at ~+53 s, 2/2, with identical counters — a repeatable state, not a race. The leading
 * explanation is a chained CYCLE: a guest loop whose blocks are chained in both directions runs natively and
 * correctly, but re-enters the dispatcher only on a mispredict, so a hot loop with a rare exit edge spins in
 * the arena while ctx->block_count climbs. That is exactly the observed shape — blocks high, dispatches low,
 * then silence.
 *
 * MACRUNNER_HB_CHAIN_FORWARD_ONLY=1 declines to patch when the successor's guest address is not strictly
 * greater than the predecessor's, which is the shape of a loop backedge. If the wedge disappears while
 * avg_chain stays above 1, the cycle explanation is confirmed and forward-only chaining is a usable subset;
 * if the wedge survives, the cause is instead the 5x larger steps/blocks deltas feeding back through
 * out->steps_executed into macrunner_hb_run_x64's loop, and the cycle idea is refuted. Default 0, so it
 * changes nothing until a measurement says otherwise. */
static int runtime_chain_forward_only_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_FORWARD_ONLY", 0);
}

/* MacRunner 2026-07-30 — bisect on HOW MUCH chaining is survivable, after both kind-based splits failed.
 *
 * Restricting WHICH edges get chained has now been tried twice and neither changed the wedge: the widened
 * terminal set and the original direct-JMP-only set both die, and forward-only chaining (which removed
 * 258 640 backedges and dropped avg_chain from 5.19 to 1.37) wedges identically. So the next axis is
 * quantity, not kind. MACRUNNER_HB_CHAIN_MAX_PATCHES=N stops after N successful patches; 0 means unlimited.
 *
 * If a small N boots and unlimited wedges, the wedge is cumulative — a resource or state effect — and the
 * cap is itself a shippable subset of chaining. If even a handful of patches wedges, then
 * MACRUNNER_HB_TRACE_CHAIN_EDGE=1 has already logged those few cur->next guest pairs and the culprit edge
 * is named outright. Either way the answer is one run, which is why this is the axis to bisect. */
static uint64_t runtime_chain_max_patches(void) {
    static int parsed;
    static uint64_t limit;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_CHAIN_MAX_PATCHES");
        limit = (env && *env) ? strtoull(env, NULL, 0) : 0;
        parsed = 1;
    }
    return limit;
}

static int trace_chain_edge_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_TRACE_CHAIN_EDGE", 0);
}

/* MacRunner 2026-07-30 — log edges NEAR one guest address instead of just the first 64.
 *
 * The fault is deterministic at guest pc 0x87ef2469a30 (mono-2.0-bdwgc.dll+0x69a30, entered with garbage
 * rcx/rdx identical across three runs), but with ~11 000 tails patched a first-64 cap never reaches that
 * region, so "is the faulting block reached by a chain" stayed unanswerable. MACRUNNER_HB_CHAIN_EDGE_NEAR
 * takes that guest address and logs every edge whose predecessor or successor lies within +-64 KB of it.
 * Observation only — it decides whether the chain even touches the block that dies, which five mechanism
 * guesses could not. */
static uint64_t runtime_chain_edge_near(void) {
    static int parsed;
    static uint64_t addr;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_CHAIN_EDGE_NEAR");
        addr = (env && *env) ? strtoull(env, NULL, 0) : 0;
        parsed = 1;
    }
    return addr;
}

/* Refuse to chain exactly the edges the near-filter selects. If the fault then vanishes, the defect belongs
 * to this block pair; if it merely relocates to another chained edge, the defect is general to chaining. */
static int runtime_chain_refuse_near(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_REFUSE_NEAR", 0);
}

static int chain_edge_is_near(uint64_t a, uint64_t b) {
    uint64_t c = runtime_chain_edge_near();
    if (!c) return 0;
    return (a > c ? a - c : c - a) <= 0x10000ull || (b > c ? b - c : c - b) <= 0x10000ull;
}

static uint64_t g_chain_patches_installed;

/* MacRunner 2026-07-30 — observe the guest state ACROSS a chained transition, which is the one thing none
 * of the cap/kind arms could see.
 *
 * Established by bisection: no patch boots to Begin MonoManager, while 1, 8 and 11 458 patches all wedge,
 * and refusing self edges or backedges changes nothing. So the first chained transition is already fatal
 * and the question is no longer WHICH edge but WHAT it does. block_delta > 1 is exactly the signal that a
 * chain executed inside one native dispatch, so printing ctx->pc beside the predecessor's guest_addr there
 * separates the two remaining possibilities: a PC that is not a sane guest address means the trampoline is
 * corrupting control flow (and since the encodings are clang-verified byte for byte, that would point at the
 * native_code+12 entry contract or the frame, not the instruction bytes), while a sane PC means nothing is
 * corrupted and the loss is the per-dispatch host work a chained transition skips. */
static int chain_edge_is_near(uint64_t a, uint64_t b);

static void trace_chain_transition(const hb_context_t* ctx, const hb_block_cache_entry_t* cur,
                                   uint64_t block_delta, uint64_t step_delta,
                                   uint64_t before_rcx, uint64_t before_rdx,
                                   uint64_t before_rbp, uint64_t before_rsp) {
    static uint64_t shown;
    int near;
    if (!trace_chain_edge_enabled() || !ctx || !cur) return;

    /* MacRunner 2026-07-31 — the failure dump says the corrupted register is RBP, not rcx/rdx.
     * A chaining run dies at +50.9 s with `rbp=0x8` at mono-2.0-bdwgc.dll rva 0x6adc2, and that dump appears
     * 0x in the baseline and lean-frame controls, so it belongs to chaining. This alert is deliberately NOT
     * subject to the 32-line cap below: the cap is why the first 32 transitions all looked clean while the
     * one that matters was never printed. */
    if (before_rbp >= 0x10000 && ctx->regs.x64.rbp < 0x10000) {
        fprintf(stderr, "macrunner-hb-chain-RBP-VIOLATION: from=0x%llx pc_after=0x%llx blocks=%llu steps=%llu "
                        "rbp %llx->%llx rsp %llx->%llx rcx %llx->%llx\n",
                (unsigned long long)cur->guest_addr, (unsigned long long)ctx->pc,
                (unsigned long long)block_delta, (unsigned long long)step_delta,
                (unsigned long long)before_rbp, (unsigned long long)ctx->regs.x64.rbp,
                (unsigned long long)before_rsp, (unsigned long long)ctx->regs.x64.rsp,
                (unsigned long long)before_rcx, (unsigned long long)ctx->regs.x64.rcx);
        fflush(stderr);
    }

    near = chain_edge_is_near(cur->guest_addr, ctx->pc);
    if (!near && __atomic_add_fetch(&shown, 1, __ATOMIC_RELAXED) > 32) return;
    /* rcx/rdx are the two registers that come back identically garbage in every failing run
     * (rcx=0xf0e0993f rdx=0x320eec31), so printing them either side of the chained run says whether the
     * chain corrupts them or inherits them already wrong. */
    fprintf(stderr, "macrunner-hb-chaintransit:%s from=0x%llx pc_after=0x%llx blocks=%llu steps=%llu "
                    "rcx %llx->%llx rdx %llx->%llx rbp %llx->%llx rsp %llx->%llx\n",
            near ? " NEAR" : "",
            (unsigned long long)cur->guest_addr, (unsigned long long)ctx->pc,
            (unsigned long long)block_delta, (unsigned long long)step_delta,
            (unsigned long long)before_rcx, (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)before_rdx, (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)before_rbp, (unsigned long long)ctx->regs.x64.rbp,
            (unsigned long long)before_rsp, (unsigned long long)ctx->regs.x64.rsp);
    fflush(stderr);
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

/* Declarations for the 24 helpers registered below; they live in hb_arm64_codegen.c. */
extern void     hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta);
extern void     hb_jit_helper_cpuid(hb_context_t* ctx);
extern uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc);
extern void     hb_jit_helper_exec_atomic_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_exec_binop_lazy(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                              uint64_t src1_reg, uint64_t src2_is_reg,
                                              uint64_t src2_value, uint64_t size);
extern void     hb_jit_helper_exec_cmovcc_lazy(hb_context_t* ctx, uint64_t cc,
                                               uint64_t dst_reg, uint64_t src_is_reg,
                                               uint64_t src_value, uint64_t size);
extern void     hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_four_block_loop(hb_context_t* ctx,
                                                   const hb_ir_block_t* first,
                                                   const hb_ir_block_t* second,
                                                   const hb_ir_block_t* third,
                                                   const hb_ir_block_t* fourth);
extern void     hb_jit_helper_exec_i32_less_tiebreaker(hb_context_t* ctx,
                                                       const hb_ir_block_t* entry,
                                                       const hb_ir_block_t* equal,
                                                       const hb_ir_block_t* less);
extern void hb_jit_helper_exec_popf_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_pushf_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_setcc_lazy(hb_context_t* ctx, uint64_t cc,
                                              uint64_t dst_reg);
extern void     hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_two_block_loop(hb_context_t* ctx,
                                                  const hb_ir_block_t* first,
                                                  const hb_ir_block_t* second);
extern void     hb_jit_helper_exec_unity_sort_inner_loop(hb_context_t* ctx,
                                                         const hb_ir_block_t* sort);
extern void     hb_jit_helper_lahf(hb_context_t* ctx);
extern void     hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                                 uint64_t dst_reg, uint64_t dst_size,
                                                 uint64_t dst_reg_offset);
extern uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr);
extern uint64_t hb_jit_helper_pop(hb_context_t* ctx);
extern void     hb_jit_helper_push(hb_context_t* ctx, uint64_t val);
extern void     hb_jit_helper_sahf(hb_context_t* ctx);
extern void     hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr,
                                          uint64_t val, uint64_t size);
extern void     hb_jit_helper_store_u128(hb_context_t* ctx, uint64_t addr,
                                         uint64_t lo, uint64_t hi);
extern void     hb_jit_helper_xgetbv(hb_context_t* ctx);

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
        /* MacRunner 2026-07-29: 24 helpers were emitted by codegen but absent here, so any
         * block calling one could not be persisted. Measured as the single largest cause of
         * declined stores — mh_unkhelper=3931, ahead of the x2/x3/x4 veto (2746) and the site
         * cap (573). Registering them is a far smaller change than the patcher work that
         * preceded it, and it was invisible until the rejection counters existed. */
        case 32: return (void*)hb_jit_helper_adjust_stack;
        case 33: return (void*)hb_jit_helper_cpuid;
        case 34: return (void*)hb_jit_helper_eval_cond_lazy;
        case 35: return (void*)hb_jit_helper_exec_atomic_ir;
        case 36: return (void*)hb_jit_helper_exec_binop_lazy;
        case 37: return (void*)hb_jit_helper_exec_cmovcc_lazy;
        case 38: return (void*)hb_jit_helper_exec_cmovcc_operand_lazy;
        case 39: return (void*)hb_jit_helper_exec_four_block_loop;
        case 40: return (void*)hb_jit_helper_exec_i32_less_tiebreaker;
        case 41: return (void*)hb_jit_helper_exec_popf_ir;
        case 42: return (void*)hb_jit_helper_exec_pushf_ir;
        case 43: return (void*)hb_jit_helper_exec_setcc_lazy;
        case 44: return (void*)hb_jit_helper_exec_setcc_operand_lazy;
        case 45: return (void*)hb_jit_helper_exec_two_block_loop;
        case 46: return (void*)hb_jit_helper_exec_unity_sort_inner_loop;
        case 47: return (void*)hb_jit_helper_lahf;
        case 48: return (void*)hb_jit_helper_load_to_reg_sized;
        case 49: return (void*)hb_jit_helper_load_u64;
        case 50: return (void*)hb_jit_helper_pop;
        case 51: return (void*)hb_jit_helper_push;
        case 52: return (void*)hb_jit_helper_sahf;
        case 53: return (void*)hb_jit_helper_store_sized;
        case 54: return (void*)hb_jit_helper_store_u128;
        case 55: return (void*)hb_jit_helper_xgetbv;
        default: return NULL;
    }
}

static uint8_t helper_cache_id_for_addr(uint64_t addr) {
    for (uint8_t id = 1; id <= 55; id++) {
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

/* MacRunner 2026-07-29 — MULTI-HELPER PERSISTENCE.
 *
 * native_blob_single_arg_helper_stub() returns false the moment it sees a second `blr x23`, so a
 * block containing two helper calls can never be written to the persistent cache. On Hollow
 * Knight that shows up as stores=117 against store_skips=40495: the cache retains 0.3 % of what
 * it compiles and has therefore reached a steady state where it can never learn the rest. A cold
 * start needs 476 s to the menu — 232 s of it in the Mono load phase — while Rosetta, which
 * translates once and keeps the result, gets there in under 45 s.
 *
 * This finds EVERY helper site instead of bailing at the second, scoping each site's `mov x1`
 * search to the window between the previous `blr` and this one. Everything the single-stub
 * matcher rejected for safety is preserved: a `mov` into x2/x3/x4 anywhere in the block still
 * rejects the whole block, each site must resolve to a known helper id, and each must have
 * exactly one arg1 in its window.
 *
 * DEFAULT OFF. This patches emitted machine code: a mistake here executes wrong instructions
 * rather than failing loudly, so it ships behind MACRUNNER_HB_CACHE_MULTI_HELPER=1 and gets
 * proven by an A/B before it becomes the default. */
#define HB_MULTI_HELPER_MAX 16

static bool native_blob_multi_helper_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("MACRUNNER_HB_CACHE_MULTI_HELPER");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

/* Why a multi-helper block still could not be persisted. Measured because the first A/B cut
 * multi-helper skips from 118547 to 43909 — a threefold improvement, but 44k blocks are still
 * being dropped and there are four different reasons that could be doing it. Attributing them
 * is the difference between a targeted second pass and another guess. */
typedef enum {
    HB_MH_OK = 0,
    HB_MH_TOO_MANY,      /* more helper sites than HB_MULTI_HELPER_MAX */
    HB_MH_WIDE_ARG,      /* pointer moved into x2/x3/x4 somewhere in the block */
    HB_MH_UNKNOWN_HELPER,/* helper address/id not in the cacheable set */
    HB_MH_ARG_SHAPE      /* not exactly one recognised arg1 in this site's window */
} hb_mh_reason_t;

/* Collect one stub per helper call site. Returns false if any site fails to resolve, so a
 * partially-understood block is never persisted. */
static bool native_blob_helper_stubs(const uint8_t* code, size_t size,
                                     const hb_ir_block_t* block, bool canonical,
                                     hb_cached_helper_stub_t* out, size_t* out_count,
                                     hb_mh_reason_t* out_reason) {
    size_t blrs[HB_MULTI_HELPER_MAX];
    size_t n = 0;

    if (out_reason) *out_reason = HB_MH_OK;
    if (!code || !size || !block || !out || !out_count) return false;

    for (size_t off = 0; off + 4 <= size; off += 4) {
        uint32_t insn;
        memcpy(&insn, code + off, sizeof(insn));
        if (insn == (0xd63f0000u | (23u << 5))) {
            if (n >= HB_MULTI_HELPER_MAX) {
                if (out_reason) *out_reason = HB_MH_TOO_MANY;
                return false;
            }
            blrs[n++] = off;
        }
    }
    if (!n) return false;

    /* Global safety check, unchanged from the single-stub matcher: a pointer moved into any of
     * x2/x3/x4 means an argument shape this code does not understand well enough to rewrite. */
    for (size_t off = 0; off + 16 <= size; off += 4) {
        uint64_t value = 0;
        if (arm64_mov_imm64_at(code, size, off, 2, &value) ||
            arm64_mov_imm64_at(code, size, off, 3, &value) ||
            arm64_mov_imm64_at(code, size, off, 4, &value)) {
            if (out_reason) *out_reason = HB_MH_WIDE_ARG;
            return false;
        }
    }

    for (size_t i = 0; i < n; i++) {
        size_t blr_off = blrs[i];
        size_t win_lo = (i == 0) ? 0 : blrs[i - 1] + 4;
        uint64_t helper_value = 0;
        uint8_t helper_id;
        size_t arg1_off = SIZE_MAX;
        size_t arg1_count = 0;
        uint16_t instr_index = 0;
        bool arg1_is_instr = false;

        if (blr_off < 16 || blr_off - 16 < win_lo) return false;
        if (!arm64_mov_imm64_at(code, size, blr_off - 16, 23, &helper_value)) return false;
        if (canonical) {
            if ((helper_value & HB_RUNTIME_CACHE_HELPER_MASK) != HB_RUNTIME_CACHE_HELPER_SENTINEL)
                return false;
            helper_id = (uint8_t)(helper_value & 0xffu);
            if (!helper_addr_for_cache_id(helper_id)) {
                if (out_reason) *out_reason = HB_MH_UNKNOWN_HELPER;
                return false;
            }
        } else {
            helper_id = helper_cache_id_for_addr(helper_value);
            if (!helper_id) {
                if (out_reason) *out_reason = HB_MH_UNKNOWN_HELPER;
                return false;
            }
        }

        for (size_t off = win_lo; off + 16 <= size && off < blr_off; off += 4) {
            uint64_t value = 0;
            if (!arm64_mov_imm64_at(code, size, off, 1, &value)) continue;
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
        if (arg1_count != 1 || arg1_off == SIZE_MAX) {
            if (out_reason) *out_reason = HB_MH_ARG_SHAPE;
            return false;
        }

        out[i].arg1_mov_off = arg1_off;
        out[i].helper_mov_off = blr_off - 16;
        out[i].helper_id = helper_id;
        out[i].instr_index = instr_index;
        out[i].arg1_is_instr = arg1_is_instr;
    }
    *out_count = n;
    return true;
}

/* ---- persistence driven by the relocation table ---------------------------
 * MacRunner 2026-07-29.
 *
 * Everything above this line tries to RECOGNISE the emitted code: find the `blr x23`, walk
 * backwards 16 bytes for the helper mov, scan a window for an arg1 mov, insist there is exactly
 * one, and veto the block if anything moved an imm64 into x2/x3/x4. Measured on Hollow Knight,
 * that matcher rejects 62 % of everything the JIT compiles, and the attribution says the
 * rejections are its own artefacts rather than properties of the code:
 *
 *   mh_argshape  24585 (46 %)  — the lazy-flag and setcc paths put `instr->cc`, an integer, in
 *                                x1 and the pointer in x2, so "exactly one recognised arg1"
 *                                comes out zero. These are the densest paths in Mono output.
 *   mh_widearg   11759 (22 %)  — the veto matches the mov PATTERN and never looks at the value.
 *                                Ten of the eighteen x2/x3/x4 sites move a small constant
 *                                (`instr->dst.reg`, `dst.size`, a 0/1 flag) that needs no
 *                                relocation at all.
 *   mh_toomany    3202 (6 %)   — a 16-site cap in the matcher, against a 256-entry table.
 *
 * Codegen already records every one of these sites as it emits them (hb_codegen.h), so none of
 * this recognition is necessary. Reading the table instead:
 *
 *   store — classify each recorded site BY VALUE, register-agnostic: this block, one of its
 *           instructions, a registered helper, a value provably too small to be a host pointer
 *           (leave it), or an unresolvable host pointer (decline the block, and only this last
 *           case describes the guest code rather than this implementation).
 *   load  — scan for the 4-instruction form whose immediate is one of the sentinels and patch it
 *           back, taking the destination register from the encoded instruction. No windows, no
 *           pairing, no site cap, nothing to recognise.
 *
 * SOUNDNESS. This is only complete if the table sees every value that can differ between the run
 * that stores a block and the run that loads it. It does: a whole-file grep of the 103
 * emit_mov_imm64() call sites for `(uint64_t)(uintptr_t)` gives 46 hits, at x1 (38), x2 (5),
 * x3 (2), x4 (1), plus x23 for helper addresses — and codegen_note_reloc() records exactly those
 * five registers. The other 33 sites (x5, x6, x20, x21, x22) carry only guest-derived values
 * (`src1.imm`, `target`, `guest_addr`, `mem.disp`, `dst.size`), which the cache key already
 * covers. If that grep ever stops holding, the reloc_desync counter below is what will say so.
 *
 * DEFAULT OFF behind MACRUNNER_HB_CACHE_RELOC=1, like the multi-helper path before it: this
 * rewrites emitted machine code, where a mistake executes wrong instructions instead of failing
 * loudly. The round-trip self-check is kept and now exercises the new load path. */

/* macOS arm64 maps __PAGEZERO over the low 4 GB, so no host pointer can live below it. A value
 * under this floor is provably not a pointer into anything that moves between runs. */
#define HB_RELOC_HOST_PTR_FLOOR 0x100000000ull
/* ...and user virtual addresses are 47-bit, so nothing at or above 2^48 is one either. That
 * second half is not pedantry: `emit_mov_imm64(buf, 1, (uint64_t)instr->src1.imm)` and the
 * mem.disp sites pass SIGNED guest values through a uint64_t cast, so a displacement of -8 —
 * about as common as x86 gets — arrives here as 0xfffffffffffffff8. Testing only the low floor
 * would class every negative immediate as an unresolvable host pointer and decline the block.
 * Sign-extended constants are stable across runs and need no relocation at all. */
#define HB_RELOC_HOST_PTR_CEIL  0x0001000000000000ull

/* x23 is the helper-call target register AND codegen's scratch for a large `mem.disp`. Named so
 * the one remaining mention of it is visibly a counter and not a classifier. */
#define HB_RELOC_SCRATCH_REG_X23 23

/* Sound in the direction that matters: it may call a genuine pointer "maybe", never a provable
 * non-pointer. Everything it returns true for is declined rather than mis-restored. */
static bool reloc_value_may_be_host_pointer(uint64_t v) {
    return v >= HB_RELOC_HOST_PTR_FLOOR && v < HB_RELOC_HOST_PTR_CEIL;
}

/* Mirrors the switch in hb_contract_telemetry_record_reloc_decline(). */
typedef enum {
    HB_RELOC_OK = 0,
    HB_RELOC_DECLINE_OVERFLOW = 1,      /* table overflowed — not trustworthy for this block */
    HB_RELOC_DECLINE_UNKNOWN_HELPER = 2,/* x23 value is not a registered helper */
    HB_RELOC_DECLINE_HOSTPTR = 3,       /* host pointer that is not this block or its instrs */
    HB_RELOC_DECLINE_COLLISION = 4,     /* a literal already looks like a sentinel */
    HB_RELOC_DECLINE_DESYNC = 5,        /* table offset does not decode as the recorded mov */
    HB_RELOC_DECLINE_ROUNDTRIP = 6      /* store->load did not reproduce the original bytes */
} hb_reloc_decline_t;

static bool native_blob_reloc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("MACRUNNER_HB_CACHE_RELOC");
        cached = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return cached != 0;
}

/* Decode a 4-instruction MOVZ/MOVK immediate without being told which register to expect —
 * the load path has only the bytes. The destination comes out of the MOVZ and is then verified
 * against the remaining three by the existing decoder, so a coincidental byte pattern that is
 * not a well-formed sequence is still rejected. */
static bool arm64_mov_imm64_any_at(const uint8_t* code, size_t size, size_t off,
                                   int* rd_out, uint64_t* value) {
    uint32_t first;
    int rd;
    if (!code || off + 16 > size) return false;
    memcpy(&first, code + off, sizeof(first));
    rd = (int)(first & 0x1fu);
    if (!arm64_mov_imm64_at(code, size, off, rd, value)) return false;
    if (rd_out) *rd_out = rd;
    return true;
}

static bool reloc_value_is_sentinel(uint64_t v) {
    return v == HB_RUNTIME_CACHE_BLOCK_SENTINEL ||
           (v & HB_RUNTIME_CACHE_INSTR_MASK) == HB_RUNTIME_CACHE_INSTR_SENTINEL ||
           (v & HB_RUNTIME_CACHE_HELPER_MASK) == HB_RUNTIME_CACHE_HELPER_SENTINEL;
}

typedef enum {
    HB_RELOC_VAL_LITERAL,   /* not a sentinel — leave it alone */
    HB_RELOC_VAL_RESOLVED,  /* a sentinel, restored against this block */
    HB_RELOC_VAL_CORRUPT    /* a sentinel that does not resolve — refuse the whole blob */
} hb_reloc_val_t;

static hb_reloc_val_t reloc_sentinel_restore(uint64_t v, const hb_ir_block_t* block,
                                             uint64_t* out) {
    if (v == HB_RUNTIME_CACHE_BLOCK_SENTINEL) {
        *out = (uint64_t)(uintptr_t)block;
        return HB_RELOC_VAL_RESOLVED;
    }
    if ((v & HB_RUNTIME_CACHE_INSTR_MASK) == HB_RUNTIME_CACHE_INSTR_SENTINEL) {
        uint64_t idx = v & ~HB_RUNTIME_CACHE_INSTR_MASK;
        if (!block->instrs || idx >= block->instr_count) return HB_RELOC_VAL_CORRUPT;
        *out = (uint64_t)(uintptr_t)&block->instrs[idx];
        return HB_RELOC_VAL_RESOLVED;
    }
    if ((v & HB_RUNTIME_CACHE_HELPER_MASK) == HB_RUNTIME_CACHE_HELPER_SENTINEL) {
        void* addr = helper_addr_for_cache_id((uint8_t)(v & 0xffu));
        if (!addr) return HB_RELOC_VAL_CORRUPT;
        *out = (uint64_t)(uintptr_t)addr;
        return HB_RELOC_VAL_RESOLVED;
    }
    return HB_RELOC_VAL_LITERAL;
}

/* Is this value one of the block's own instruction pointers? Pointer arithmetic rather than a
 * scan over instr_count, so the cost does not grow with block length. */
static bool reloc_instr_index(const hb_ir_block_t* block, uint64_t value, uint64_t* idx_out) {
    uintptr_t base, v, delta;
    size_t stride;
    if (!block || !block->instrs || !block->instr_count) return false;
    base = (uintptr_t)block->instrs;
    v = (uintptr_t)value;
    if (v < base) return false;
    stride = sizeof(block->instrs[0]);
    delta = v - base;
    if (delta % stride) return false;
    delta /= stride;
    if (delta >= block->instr_count || delta > UINT16_MAX) return false;
    *idx_out = (uint64_t)delta;
    return true;
}

/* CENSUS of the one decline that is left. After the relocation table landed, Hollow Knight measured
 * stores=239364 store_skips=8841 with rl_hostptr=8841 and every other reason exactly 0 — so these
 * blocks are the entire missing 3.5 %, and what they point AT decides whether that is recoverable.
 *
 * The claim above is that they are other IR blocks handed to the fused hot-family helpers. If so
 * they are nameable: a foreign block is reachable through this block's CFG, and it also carries a
 * guest_addr that is identical in every run. If instead they are heap or arena addresses, they are
 * not nameable and the 3.5 % is a ceiling rather than a gap.
 *
 * Measurement only — the caller declines either way. Buckets match
 * hb_contract_telemetry_record_hostptr_census(). */
/* CENSUS v1 WAS VOID — kept written down because the failure is more instructive than the result.
 *
 * v1 bucketed the declined value against block->succ and block->pred and reported, very
 * consistently, hp_succ=0 hp_pred=0 hp_succinstr=0 hp_other=8485 with the four summing exactly to
 * rl_hostptr. The sum checking out is what made it look sound. It was not: hb_ir_cfg_add_edge() has
 * ZERO callers in the engine, so succ_count and pred_count are 0 for every block that has ever
 * existed here, and those three buckets were unreachable by construction. "Not found via the CFG"
 * was a tautology, not a measurement -- the same class of mistake as reading a counter behind a
 * gate that was never switched on.
 *
 * v2 therefore measures something that cannot be empty: the DESTINATION REGISTER, which the
 * relocation table records for every site and which the decline path already has in hand. reg tells
 * us the calling convention position -- 1/2/3/4 are helper arguments, so a block pointer handed to
 * a fused hot-family helper lands in one of those, while an address that is not an argument at all
 * lands elsewhere. Plus a one-shot dump of the first few actual values next to `block` and
 * `block->instrs`, because at this point looking at the numbers beats bucketing against a guess. */
static int reloc_hostptr_census_bucket(const hb_ir_block_t* block, uint64_t value, uint8_t reg) {
    static int dumped;

    if (hb_contract_telemetry_enabled() &&
        __atomic_fetch_add(&dumped, 1, __ATOMIC_RELAXED) < 6) {
        fprintf(stderr,
                "macrunner-hb-hostptr-sample: value=0x%llx reg=%u block=%p instrs=%p"
                " instr_count=%zu succ_count=%zu\n",
                (unsigned long long)value, (unsigned)reg, (const void*)block,
                block ? (const void*)block->instrs : NULL,
                block ? block->instr_count : (size_t)0,
                block ? block->succ_count : (size_t)0);
        fflush(stderr);
    }

    switch (reg) {
        case 1: return 0;
        case 2: return 1;
        case 3: return 2;
        default: return 3;  /* 4, 23, or anything else — split further only if this points there */
    }
}

/* A literal that already looks like a sentinel would be indistinguishable from one we wrote, so
 * the load-time scan would rewrite it. Declining such a block makes that scan unambiguous by
 * construction rather than by probability. Sentinels sit at 0x4842_5254_xxxx_xxxx, far above any
 * plausible small constant and far from any macOS heap pointer, so this should never fire — the
 * counter says whether "should" is true. */
static bool reloc_blob_has_stray_sentinel(const uint8_t* code, size_t size) {
    for (size_t off = 0; off + 16 <= size; off += 4) {
        int rd = 0;
        uint64_t v = 0;
        if (arm64_mov_imm64_any_at(code, size, off, &rd, &v) && reloc_value_is_sentinel(v))
            return true;
    }
    return false;
}

static bool native_blob_reloc_load(const uint8_t* code, size_t size,
                                   const hb_ir_block_t* block,
                                   const uint8_t** out_code,
                                   uint8_t** owned_code) {
    uint8_t* patched = NULL;
    if (!code || !size || !block || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;

    for (size_t off = 0; off + 16 <= size; off += 4) {
        int rd = 0;
        uint64_t v = 0, restored = 0;
        if (!arm64_mov_imm64_any_at(code, size, off, &rd, &v)) continue;
        switch (reloc_sentinel_restore(v, block, &restored)) {
            case HB_RELOC_VAL_LITERAL:
                continue;
            case HB_RELOC_VAL_CORRUPT:
                /* A sentinel we cannot resolve means the entry does not belong to this block.
                 * Fail the load so the dispatcher recompiles; never hand back code with a
                 * sentinel still in it, which would branch to 0x4842525448... */
                free(patched);
                return false;
            case HB_RELOC_VAL_RESOLVED:
                break;
        }
        if (!patched) {
            patched = malloc(size);
            if (!patched) return false;
            memcpy(patched, code, size);
        }
        arm64_patch_mov_imm64_at(patched, size, off, rd, restored);
    }
    if (patched) {
        *out_code = patched;
        *owned_code = patched;
    }
    return true;
}

static bool native_blob_reloc_store(const hb_codegen_buffer_t* buf,
                                    const hb_ir_block_t* block,
                                    const uint8_t** out_code,
                                    uint8_t** owned_code,
                                    hb_reloc_decline_t* why) {
    const uint8_t* code;
    size_t size;
    uint8_t* patched = NULL;
    size_t patched_sites = 0, literal_sites = 0, highhalf_sites = 0, x23_value_sites = 0;

    if (why) *why = HB_RELOC_OK;
    if (!buf || !buf->code || !buf->size || !block || !out_code || !owned_code) return false;
    code = buf->code;
    size = buf->size;
    *out_code = code;
    *owned_code = NULL;

    /* An overflowed table is missing sites, and a missing site is exactly the silent failure
     * this design exists to avoid. hb_codegen.h sets the cap at 256 against a measured 4.9 sites
     * per block, so this is a guard, not a path. */
    if (buf->reloc_overflow) {
        if (why) *why = HB_RELOC_DECLINE_OVERFLOW;
        return false;
    }
    if (reloc_blob_has_stray_sentinel(code, size)) {
        if (why) *why = HB_RELOC_DECLINE_COLLISION;
        return false;
    }
    if (!buf->reloc_count) {
        /* Nothing to relocate: the blob is position-independent as emitted. */
        hb_contract_telemetry_record_reloc_store(0, 0);
        return true;
    }

    patched = malloc(size);
    if (!patched) return false;
    memcpy(patched, code, size);

    for (size_t i = 0; i < buf->reloc_count; i++) {
        const hb_codegen_reloc_t* rl = &buf->relocs[i];
        uint64_t seen = 0, idx = 0, sentinel;

        /* The table must describe the code it came from. If a later pass rewrote these bytes,
         * or an offset drifted, this catches it here instead of at some unrelated cache hit. */
        if (!arm64_mov_imm64_at(code, size, rl->off, (int)rl->reg, &seen) || seen != rl->value) {
            if (why) *why = HB_RELOC_DECLINE_DESYNC;
            goto decline;
        }

        /* By KIND, not by register. `reg == 23` used to stand in for "helper address" and was
         * wrong: emit_mask_x_reg_to_size() uses x23 as scratch and emits `mov x23, 0xffffffff`
         * for the zero-extension every 32-bit x86 operand needs — a plain 32-bit ADD produces
         * exactly that one relocation. Looking 0xffffffff up in the helper table failed and
         * declined the whole block: 53 655 on Hollow Knight, 90 % of every decline. A mask is a
         * constant, so it now falls through to the value classification below, lands under the
         * 4 GB floor, and is left alone — which is all it ever needed. */
        if (rl->kind == HB_RELOC_KIND_HELPER) {
            uint8_t id = helper_cache_id_for_addr(rl->value);
            if (!id) {
                if (why) *why = HB_RELOC_DECLINE_UNKNOWN_HELPER;
                goto decline;
            }
            sentinel = HB_RUNTIME_CACHE_HELPER_SENTINEL | id;
        } else if (rl->value == (uint64_t)(uintptr_t)block) {
            sentinel = HB_RUNTIME_CACHE_BLOCK_SENTINEL;
        } else if (reloc_instr_index(block, rl->value, &idx)) {
            sentinel = HB_RUNTIME_CACHE_INSTR_SENTINEL | idx;
        } else if (!reloc_value_may_be_host_pointer(rl->value)) {
            /* Provably not a host pointer — a condition code, an opcode, a register number, a
             * size, or a sign-extended negative guest constant. Identical in every run, so it
             * needs no relocation. This is the case the old matcher threw whole blocks away
             * for. Counted split by half so the next run says which rule earned the retention. */
            literal_sites++;
            if (rl->value >= HB_RELOC_HOST_PTR_CEIL) highhalf_sites++;
            /* The site the register-based test used to declare an unknown helper. Counting only —
             * the classification above is on kind, and scripts/hb-check-reloc-invariant.sh fails
             * the build if `rl->reg == 23` ever decides anything again. */
            if (rl->reg == HB_RELOC_SCRATCH_REG_X23) x23_value_sites++;
            continue;
        } else {
            /* A host pointer this block cannot name: `first`/`second`/`sort`/`entry` point into
             * OTHER IR blocks, which will not exist at load time. Genuinely un-persistable, and
             * the only decline here that is about the code rather than about this matcher. */
            hb_contract_telemetry_record_hostptr_census(
                reloc_hostptr_census_bucket(block, rl->value, rl->reg));
            if (why) *why = HB_RELOC_DECLINE_HOSTPTR;
            goto decline;
        }

        arm64_patch_mov_imm64_at(patched, size, rl->off, (int)rl->reg, sentinel);
        patched_sites++;
    }

    /* SELF-CHECK, kept from the multi-helper path and now covering the new load path.
     *
     * Patching emitted machine code is the one class of change here that fails silently: a wrong
     * offset does not crash the patcher, it executes wrong instructions later and far from the
     * cause. So prove the transformation is invertible before trusting it — run the actual load
     * path over the patched form and require the result to be byte-identical to what codegen
     * produced. One extra pass and a memcmp per stored block, paid at compile time, never on the
     * hot path. */
    {
        const uint8_t* back = NULL;
        uint8_t* owned_back = NULL;
        bool sound;

        if (!native_blob_reloc_load(patched, size, block, &back, &owned_back)) {
            if (why) *why = HB_RELOC_DECLINE_ROUNDTRIP;
            goto decline;
        }
        sound = (memcmp(back, code, size) == 0);
        free(owned_back);
        if (!sound) {
            if (why) *why = HB_RELOC_DECLINE_ROUNDTRIP;
            goto decline;
        }
    }

    hb_contract_telemetry_record_reloc_store((unsigned long)patched_sites,
                                             (unsigned long)literal_sites);
    hb_contract_telemetry_record_reloc_highhalf((unsigned long)highhalf_sites);
    hb_contract_telemetry_record_reloc_x23_value((unsigned long)x23_value_sites);
    *out_code = patched;
    *owned_code = patched;
    return true;

decline:
    free(patched);
    *out_code = code;
    *owned_code = NULL;
    return false;
}

/* MacRunner 2026-07-30 — timed wrapper. Follows the _inner convention already used in
 * hb_memory_protect_inner: the body has many exits and wrapping is safer than threading a stop
 * through each one. This is the half of a compile that contains the round-trip self-check, which
 * re-runs the whole load path and memcmps it for every stored block. */
static bool native_blob_prepare_cache_store_inner(const hb_codegen_buffer_t* buf,
                                            const uint8_t* code, size_t size,
                                            const hb_ir_block_t* block,
                                            const uint8_t** out_code,
                                            uint8_t** owned_code);

static uint64_t hb_time_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool native_blob_prepare_cache_store(const hb_codegen_buffer_t* buf,
                                            const uint8_t* code, size_t size,
                                            const hb_ir_block_t* block,
                                            const uint8_t** out_code,
                                            uint8_t** owned_code) {
    uint64_t t0 = hb_time_now_ns();
    bool r = native_blob_prepare_cache_store_inner(buf, code, size, block, out_code, owned_code);
    uint64_t t1 = hb_time_now_ns();
    if (t1 > t0) hb_contract_telemetry_add_time(1, t1 - t0);
    return r;
}

static bool native_blob_prepare_cache_store_inner(const hb_codegen_buffer_t* buf,
                                            const uint8_t* code, size_t size,
                                            const hb_ir_block_t* block,
                                            const uint8_t** out_code,
                                            uint8_t** owned_code) {
    hb_cached_helper_stub_t stub;
    uint8_t* patched;
    if (!code || !size || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;

    if (native_blob_reloc_enabled() && buf) {
        hb_reloc_decline_t why = HB_RELOC_OK;
        if (native_blob_reloc_store(buf, block, out_code, owned_code, &why)) return true;
        hb_contract_telemetry_record_reloc_decline((int)why);
        return false;
    }

    if (native_blob_multi_helper_enabled() && native_blob_has_helper_call(code, size)) {
        hb_cached_helper_stub_t stubs[HB_MULTI_HELPER_MAX];
        size_t count = 0;
        hb_mh_reason_t mh_reason = HB_MH_OK;
        if (native_blob_helper_stubs(code, size, block, false, stubs, &count, &mh_reason) && count > 1) {
            patched = malloc(size);
            if (!patched) return false;
            memcpy(patched, code, size);
            for (size_t i = 0; i < count; i++) {
                arm64_patch_mov_imm64_at(patched, size, stubs[i].arg1_mov_off, 1,
                                         stubs[i].arg1_is_instr
                                             ? (HB_RUNTIME_CACHE_INSTR_SENTINEL | stubs[i].instr_index)
                                             : HB_RUNTIME_CACHE_BLOCK_SENTINEL);
                arm64_patch_mov_imm64_at(patched, size, stubs[i].helper_mov_off, 23,
                                         HB_RUNTIME_CACHE_HELPER_SENTINEL | stubs[i].helper_id);
            }

            /* SELF-CHECK, and the reason this is safe to ship at all.
             *
             * Patching emitted machine code is the one class of change here that fails silently:
             * a wrong offset does not crash the patcher, it executes wrong instructions later and
             * far from the cause. So prove the transformation is invertible before trusting it —
             * re-derive the stubs from the patched form exactly as the load path will
             * (canonical=true) and patch them back. If the result is not byte-identical to what
             * codegen produced, this block's round trip is not sound and we decline to persist
             * something we cannot faithfully restore.
             *
             * One extra scan and memcmp per stored block, paid at compile time, never on the hot
             * path. Cheap insurance against the only failure mode here that has no symptom. */
            {
                hb_cached_helper_stub_t back[HB_MULTI_HELPER_MAX];
                size_t back_count = 0;
                uint8_t* restored = malloc(size);
                bool sound = false;

                if (restored) {
                    memcpy(restored, patched, size);
                    if (native_blob_helper_stubs(patched, size, block, true, back, &back_count, NULL) &&
                        back_count == count) {
                        for (size_t i = 0; i < back_count; i++) {
                            arm64_patch_mov_imm64_at(restored, size, back[i].arg1_mov_off, 1,
                                                     back[i].arg1_is_instr
                                                         ? (uint64_t)(uintptr_t)&block->instrs[back[i].instr_index]
                                                         : (uint64_t)(uintptr_t)block);
                            arm64_patch_mov_imm64_at(restored, size, back[i].helper_mov_off, 23,
                                                     (uint64_t)(uintptr_t)helper_addr_for_cache_id(back[i].helper_id));
                        }
                        sound = (memcmp(restored, code, size) == 0);
                    }
                    free(restored);
                }
                if (!sound) {
                    free(patched);
                    goto multi_helper_declined;
                }
            }

            *out_code = patched;
            *owned_code = patched;
            return true;
        }
    }
multi_helper_declined:;
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

    /* The table-driven load needs nothing but the sentinels the store path wrote, so it also
     * reads blobs the older matchers produced — they use the same encoding. The persistent key
     * still carries a mode bit (HB_PERSIST_FLAG_RELOC) so the two arms of an A/B can never share
     * entries in the other direction: a reloc-stored blob can hold sentinels in x2/x3/x4, which
     * the legacy load below does not know to restore. */
    if (native_blob_reloc_enabled())
        return native_blob_reloc_load(code, size, block, out_code, owned_code);

    /* Mirror of the multi-helper store path. A block written with several sentinel-patched sites
     * can only be loaded by code that patches all of them back, so the two must agree exactly:
     * restoring only the first would leave the rest jumping to a sentinel value. */
    if (native_blob_multi_helper_enabled() && native_blob_has_helper_call(code, size)) {
        hb_cached_helper_stub_t stubs[HB_MULTI_HELPER_MAX];
        size_t count = 0;
        if (native_blob_helper_stubs(code, size, block, true, stubs, &count, NULL) && count > 1) {
            patched = malloc(size);
            if (!patched) return false;
            memcpy(patched, code, size);
            for (size_t i = 0; i < count; i++) {
                arm64_patch_mov_imm64_at(patched, size, stubs[i].arg1_mov_off, 1,
                                         stubs[i].arg1_is_instr
                                             ? (uint64_t)(uintptr_t)&block->instrs[stubs[i].instr_index]
                                             : (uint64_t)(uintptr_t)block);
                arm64_patch_mov_imm64_at(patched, size, stubs[i].helper_mov_off, 23,
                                         (uint64_t)(uintptr_t)helper_addr_for_cache_id(stubs[i].helper_id));
            }
            *out_code = patched;
            *owned_code = patched;
            return true;
        }
    }
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

/* MacRunner 2026-08-01 — FEX-style guest-position reconstruction, built alongside the old path.
 *
 * FEX carries no context snapshot: a fault is resolved by folding the faulting host PC through a
 * per-block (host delta -> guest RIP delta) table, so rollback granularity is one guest
 * instruction and work completed before it is never discarded. Our equivalent needs no emitted
 * table — the guest side is block->instrs[i].guest_addr, and host_off[i] now records where that
 * instruction's code begins.
 *
 * Nothing is removed yet. This runs in parallel with the snapshot path and reports disagreements,
 * because five hypotheses about the chaining defect were refuted by measurement today and
 * replacing a mechanism on the strength of a sixth would be the same mistake. */
static void ripmap_attach(hb_block_cache_entry_t* entry, const hb_codegen_buffer_t* buf);

static int ripmap_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_RIPMAP", 0);
}

/* Rank 1 of the prior-art synthesis: stop paying the per-dispatch pre-image.
 *
 * The census settled the premise -- the snapshot is restored 0 times in 741 M dispatches, and
 * claim_taken is 0 against ~7.2 M claim_calls -- so what the copy buys is a rollback that does
 * not happen. QEMU (encode_search / cpu_unwind_data_from_tb) and FEX (JITCodeTail ->
 * RestoreRIPFromHostPC) both resolve a fault by recovering the faulting guest instruction's
 * POSITION from a side table read only on the fault path, and carry no per-block pre-image at all.
 *
 * With this on, the guard resolves ctx->pc through the map instead of restoring registers. What
 * that gives up is stated plainly: a block that faulted part-way leaves its partial effects in
 * place. That is the same trade the chain-scoped rollback gate already makes, and it is why this
 * is a gate with a default of OFF rather than a deletion.
 *
 * The FRAME stays. It is not only the pre-image: the signal handler identifies its own faults
 * through g_jit_signal_fault_frame, 3.6 M times per run. Removing the copy is not removing it. */
static int nosnapshot_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_NO_SNAPSHOT", 0);
}

/* The map is optional evidence under MACRUNNER_HB_RIPMAP, but it is the ONLY way back under
 * MACRUNNER_HB_NO_SNAPSHOT -- so it has to be built whenever either gate is on. */
static int ripmap_needed(void) { return ripmap_enabled() || nosnapshot_enabled(); }

static uint64_t t_ripmap_hit, t_ripmap_miss, t_ripmap_agree, t_ripmap_disagree;
/* Recoveries under MACRUNNER_HB_NO_SNAPSHOT: position recovered from the map, versus faults where
 * the map could not answer. The premise is that both stay at 0 (the branch is cold); an unresolved
 * count that is not 0 is the number that decides whether this scheme can replace the snapshot. */
static uint64_t t_nosnap_resolved, t_nosnap_unresolved;

/* Copy the host-offset map the codegen recorded into the cached entry, which outlives the buffer.
 * Only while the gate is on: normal runs allocate nothing. A block whose instruction count
 * overflowed the codegen table keeps no map and simply falls back to the snapshot path. */
static void ripmap_attach(hb_block_cache_entry_t* entry, const hb_codegen_buffer_t* buf) {
    size_t bytes;
    if (!entry || !buf || !ripmap_needed()) return;
    if (buf->host_off_overflow || !buf->host_off_count) return;
    bytes = (size_t)buf->host_off_count * sizeof(uint32_t);
    entry->host_off = (uint32_t*)malloc(bytes);
    if (!entry->host_off) return;
    entry->host_instr = (uint16_t*)malloc((size_t)buf->host_off_count * sizeof(uint16_t));
    if (!entry->host_instr) { free(entry->host_off); entry->host_off = NULL; return; }
    memcpy(entry->host_off, buf->host_off, bytes);
    memcpy(entry->host_instr, buf->host_instr, (size_t)buf->host_off_count * sizeof(uint16_t));
    /* Set last: the resolver takes a non-zero count as "both arrays are populated". */
    entry->host_off_count = buf->host_off_count;
}

/* host_pc -> exact guest address of the instruction being executed, or 0 if unresolvable. */
static uint64_t ripmap_guest_for_host_pc(const hb_block_cache_entry_t* entry, uint64_t host_pc) {
    size_t lo = 0, hi, mid, best;
    uint16_t instr;
    uint64_t off;
    if (!entry || !entry->host_off || !entry->host_instr || !entry->host_off_count ||
        !entry->block)
        return 0;
    if (!entry->native_code || host_pc < (uint64_t)(uintptr_t)entry->native_code) return 0;
    off = host_pc - (uint64_t)(uintptr_t)entry->native_code;
    if (off >= entry->native_size) return 0;
    /* Last entry whose offset is <= off: the instruction the faulting PC belongs to. */
    hi = entry->host_off_count - 1; best = 0;
    while (lo <= hi) {
        mid = lo + (hi - lo) / 2;
        if (entry->host_off[mid] <= off) { best = mid; lo = mid + 1; }
        else { if (!mid) break; hi = mid - 1; }
    }
    /* best indexes the MAP, not the instruction list -- the two diverge in any block where a
     * fusion fired, so the instruction index has to be read out of the map rather than assumed
     * equal to it. Getting this wrong returns a plausible-looking address from the same block,
     * which is exactly the kind of wrong answer a counter cannot flag as wrong. */
    instr = entry->host_instr[best];
    if (instr >= entry->block->instr_count) return 0;
    return entry->block->instrs[instr].guest_addr;
}

static void ripmap_check(const hb_block_cache_entry_t* faulted, uint64_t host_pc,
                         uint64_t snapshot_pc) {
    uint64_t guest;
    if (!ripmap_enabled()) return;
    guest = ripmap_guest_for_host_pc(faulted, host_pc);
    if (!guest) { t_ripmap_miss++; }
    else {
        t_ripmap_hit++;
        if (guest == snapshot_pc) t_ripmap_agree++;
        else {
            t_ripmap_disagree++;
            /* A disagreement is the interesting case and must not hide behind a coarse period:
             * the snapshot restores the position the DISPATCH began at, the map gives the
             * instruction that actually faulted, and where a chain retired several blocks those
             * are supposed to differ. That difference is the whole argument for the scheme. */
            if (t_ripmap_disagree <= 16 || (t_ripmap_disagree & 0xffu) == 0)
                fprintf(stderr,
                        "macrunner-hb-ripmap: disagree=%llu agree=%llu miss=%llu "
                        "map_guest=0x%llx snapshot_pc=0x%llx delta=%lld\n",
                        (unsigned long long)t_ripmap_disagree, (unsigned long long)t_ripmap_agree,
                        (unsigned long long)t_ripmap_miss, (unsigned long long)guest,
                        (unsigned long long)snapshot_pc, (long long)(guest - snapshot_pc));
        }
    }
    if (((t_ripmap_hit + t_ripmap_miss) & 0x3ffu) == 0)
        fprintf(stderr, "macrunner-hb-ripmap-progress: hit=%llu miss=%llu agree=%llu disagree=%llu\n",
                (unsigned long long)t_ripmap_hit, (unsigned long long)t_ripmap_miss,
                (unsigned long long)t_ripmap_agree, (unsigned long long)t_ripmap_disagree);
}

static void block_cache_evict_entry(hb_jit_runtime_t* rt, hb_block_cache_t* cache,
                                    hb_block_cache_entry_t* entry) {
    size_t idx;
    if (!cache || !entry || !entry->valid) return;
    idx = block_cache_entry_index(cache, entry);
    if (runtime_block_chain_enabled())
        block_cache_prepare_replace_entry(rt, cache, entry);
    block_cache_release_owned_block(entry, NULL);
    free(entry->host_off);
    free(entry->host_instr);
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
    /* Blobs written by the table-driven store can carry sentinels in registers the legacy load
     * path never restores, so the two must not read each other's entries. Keying on the mode
     * makes a mixed cache directory a miss rather than a wrong translation. */
    if (native_blob_reloc_enabled()) key->flags |= HB_PERSIST_FLAG_RELOC;
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

/* Per-thread JIT runtime construction cost.
 *
 * HK builds one of these per guest thread -- 115 have been counted -- and each
 * one mmaps a 128 MB JIT buffer and opens the persistent cache.  A burst of 15
 * was observed inside the 58.5 s PhysX-ready-to-XInput stretch, which is the
 * largest unexplained block in the deterministic boot prefix, and nothing has
 * ever timed this path.  One line per construction (≈115 lines/run) says whether
 * it is seconds or microseconds, and costs nothing to leave on. */
static unsigned long long mm_rt_created;
static unsigned long long mm_rt_total_ns;

static unsigned long long mm_rt_now_ns(void) {
#ifdef __APPLE__
    return (unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx) {
    unsigned long long rt_t0 = mm_rt_now_ns();
    unsigned long long rt_buf_ns = 0, rt_cache_ns = 0, rt_mark;
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
    rt_mark = mm_rt_now_ns();
    rt->jit_mem = hb_jit_buffer_create(jit_size);
    rt_buf_ns = mm_rt_now_ns() - rt_mark;
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
        rt_mark = mm_rt_now_ns();
        rt->persistent_cache = hb_cache_open(cache_root && *cache_root ? cache_root : NULL, &options);
        rt_cache_ns = mm_rt_now_ns() - rt_mark;
        hb_contract_telemetry_record_open(rt->persistent_cache != NULL);
        translation_cache_register_atexit();
        if (translation_cache_trace_enabled()) {
            fprintf(stderr, "macrunner-hb-translation-cache-open: root=%s status=%s\n",
                    cache_root && *cache_root ? cache_root : "build/hyperbridge-cache",
                    rt->persistent_cache ? "ok" : "failed");
            fflush(stderr);
        }
    }
    {
        unsigned long long total = mm_rt_now_ns() - rt_t0;
        unsigned long long n = __atomic_add_fetch(&mm_rt_created, 1, __ATOMIC_RELAXED);
        unsigned long long sum = __atomic_add_fetch(&mm_rt_total_ns, total, __ATOMIC_RELAXED);
        fprintf(stderr,
                "macrunner-hb-rtmeter: n=%llu total_ms=%.2f jitbuf_ms=%.2f cacheopen_ms=%.2f "
                "cum_total_ms=%.2f jit_size=%zu\n",
                n, (double)total / 1e6, (double)rt_buf_ns / 1e6,
                (double)rt_cache_ns / 1e6, (double)sum / 1e6, jit_size);
        fflush(stderr);
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
/* MacRunner 2026-07-30 — this is a LINEAR SCAN over every block in the lifted function, comparing
 * guest addresses, and on the default configuration the dispatch loop begins with it unconditionally
 * (hb_jit_runtime_run_legacy, "hb_ir_block_t* block = find_block(func->cfg, ctx->pc)") BEFORE the O(1)
 * hash lookup block_cache_find that follows a few lines later. So today every guest block transition
 * pays a scan whose length is the function's block count.
 *
 * Whether that matters is a number, not an argument, so count it instead of reasoning about it: the
 * scan length is accumulated once per CALL rather than per iteration, which keeps the added cost O(1)
 * and leaves the loop itself untouched. Reported per thread beside avg_chain. If avg_scan comes back
 * in the hundreds, MACRUNNER_HB_SINGLE_LOOKUP=1 — which reorders the fast path to consult the hash
 * FIRST and reach find_block only on a miss — is worth far more than block chaining, and unlike
 * chaining it patches no code. This is the same shape as the hb_memory_protect O(N) walk that turned
 * out to be 27 % of a run. (Counters declared up with t_dispatch_term, which reports them.) */
static hb_ir_block_t* find_block(const hb_ir_cfg_t* cfg, uint64_t addr) {
    size_t n = cfg->block_count;
    if (n > t_findblock_max_n) t_findblock_max_n = n;
    for (size_t i = 0; i < n; i++) {
        if (cfg->blocks[i]->guest_addr == addr) {
            t_findblock_calls++;
            t_findblock_iters += (uint64_t)i + 1;
            return cfg->blocks[i];
        }
    }
    t_findblock_calls++;
    t_findblock_iters += (uint64_t)n;
    return NULL;
}

static bool is_control_transfer_op(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

/* MacRunner 2026-08-01 — memoised, because this linear scan was the top self-time item on the
 * critical thread.
 *
 * Profiled with the dispatch-stats instrument OFF (it inflates the caller by ~10 points), the two
 * hot PCs inside hb_jit_runtime_run together carried 24.2 % / 18.6 % of the critical thread's
 * samples across two samples of one run. Disassembly identifies one of them as this loop: a
 * 184-byte-stride walk testing each op against the control-transfer bitmask — i.e. hb_ir_instr_t
 * is 184 bytes, so a 3-instruction block touches ~552 bytes of cold-ish memory purely to answer
 * "where is the terminal", on EVERY dispatch.
 *
 * The answer cannot change: it is a pure function of `instrs`, which is fixed once translation
 * finishes. So compute it once and keep it on the block. hb_ir_emit invalidates the memo, so a
 * block still being built can never serve a stale answer.
 *
 * The write is a benign race by construction: two threads racing on the same block compute the
 * SAME value from the same immutable input, so a torn read is impossible (aligned int32) and a
 * lost update only costs a recompute. No lock, no atomic ordering requirement. */
/* A/B by ENVIRONMENT ONLY — one binary, no deploy between arms, which is the only way to compare
 * two arms of a boot whose marker time already spans 210-628 s without also varying the build.
 * Default 1 (memoised); MACRUNNER_HB_TERMINAL_MEMO=0 restores the per-dispatch scan. */
static int terminal_memo_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_TERMINAL_MEMO", 1);
}

static const hb_ir_instr_t* first_control_transfer_instr(const hb_ir_block_t* block) {
    int32_t idx;
    if (!block) return NULL;

    if (!terminal_memo_enabled()) {
        for (size_t i = 0; i < block->instr_count; i++)
            if (is_control_transfer_op(block->instrs[i].op)) return &block->instrs[i];
        return NULL;
    }

    idx = block->first_transfer_idx;
    if (idx == HB_IR_TRANSFER_NONE) return NULL;
    if (idx >= 0)
        return (size_t)idx < block->instr_count ? &block->instrs[idx] : NULL;

    for (size_t i = 0; i < block->instr_count; i++) {
        if (is_control_transfer_op(block->instrs[i].op)) {
            ((hb_ir_block_t*)block)->first_transfer_idx = (int32_t)i;
            return &block->instrs[i];
        }
    }
    ((hb_ir_block_t*)block)->first_transfer_idx = HB_IR_TRANSFER_NONE;
    return NULL;
}

/* MacRunner 2026-07-30 — a chainable terminal must be a DIRECT jump, and the src1 test is what
 * makes it one.
 *
 * This accepted any HB_IR_JMP, which is wrong in a way that would not have shown up as a crash.
 * hb_arm64_codegen.c:4625 splits the op on exactly this field: src1.type == HB_OP_NONE emits
 * emit_set_pc_imm64(instr->target) — one fixed successor, known at translation time — while
 * src1.type != HB_OP_NONE emits emit_native_indirect_jmp, which computes the target into x20 at run
 * time. The same test appears at the indirect-IC call site in the dispatcher below.
 *
 * Chaining an indirect jump means patch_block_tail nails its tail to whichever block happened to
 * follow it the first time it ran; every later execution with a different computed target then runs
 * the wrong guest code, silently, with no fault to notice. Indirect jumps are how vtable, switch and
 * import dispatch work, so this would have fired constantly the moment MACRUNNER_HB_BLOCK_CHAIN was
 * armed — a hazard entirely separate from the eviction one the chain-entry trampoline removes. */
/* MacRunner 2026-07-30 — WIDENED, because the direct-JMP-only rule capped chaining at 7.4 % of
 * dispatches and the PC guard removes the reason for the rule.
 *
 * Measured over 205.8 M dispatched blocks on HK's critical thread (run SPEEDDIG1): jcc 71.1 %,
 * ret 9.1 %, call_dir 8.0 %, jmp_dir 7.4 %, call_ind 2.8 %, jmp_ind 1.6 %. Only jmp_dir was eligible,
 * so the whole mechanism could address at most 7.4 % of the round trips that make up ~54 % of that
 * thread. The single-successor restriction existed because the tail patch branched unconditionally:
 * anything with more than one possible successor would have run the wrong guest block.
 *
 * chain_trampoline_build now checks ctx->pc against the target's guest_addr before branching, so a
 * wrong guess costs four instructions and a normal dispatch instead of silent corruption. That admits
 * Jcc (either edge), and indirect jumps and calls with it — the case that was outright unsafe before.
 *
 * RET stays out on purpose: its successor is a return address that differs per call site, so it would
 * mispredict nearly always and pay the guard for nothing. That leaves 90.9 % of terminals eligible
 * against 7.4 %. MACRUNNER_HB_CHAIN_WIDE=0 restores the direct-JMP-only set for an A/B of the widening
 * on its own. */
static int runtime_chain_wide_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_WIDE", 1);
}

static bool block_terminal_is_chainable(const hb_ir_block_t* block) {
    const hb_ir_instr_t* terminal;
    if (!block || block->instr_count == 0) return false;
    terminal = &block->instrs[block->instr_count - 1];
    if (!runtime_chain_wide_enabled())
        return terminal->op == HB_IR_JMP && terminal->src1.type == HB_OP_NONE;
    return terminal->op == HB_IR_JMP || terminal->op == HB_IR_Jcc ||
           terminal->op == HB_IR_CALL;
}

/* MacRunner 2026-07-30 — this must recognise a slot that has ALREADY been patched, and until it did, it
 * made block chaining unmeasurable and self-limiting at the same time.
 *
 * It sniffs the emitted bytes rather than carrying a flag, which is the right call: the persistent
 * translation cache restores blobs, so anything derived from the bytes survives a cache load for free
 * while a struct field would have to be serialised. What was wrong is that it demanded all four words
 * still be NOP — and patch_block_tail overwrites the first two with `MOV X0, X19` / `B <trampoline>`.
 * So every successfully chained block stopped satisfying it, with two consequences measured on HK
 * (`macrunner-hb-chaindecline`, 6365 tails patched):
 *
 *   - `native_accounting = chain_accounting && entry_has_chain_slot(cached, NULL)` went false for exactly
 *     the chained blocks, so the dispatcher counted blocks_executed++ instead of reading the ctx->block_count
 *     delta the emitted accounting maintains. avg_chain was therefore pinned at 1.0000 by construction, no
 *     matter how well chaining worked — the metric was blind to its own subject.
 *   - a patched block was rejected as a chain TARGET (slot_next=47054), so chaining throttled itself.
 *
 * Accepting both shapes fixes both. Re-patching a patched slot is harmless and in fact useful (it re-aims
 * the chain at a new successor); the `meta->target_code` early return upstream is what keeps it from
 * happening needlessly. */
static bool entry_has_chain_slot(const hb_block_cache_entry_t* entry, size_t* offset) {
    static const uint32_t arm64_nop = 0xd503201fu;
    static const uint32_t arm64_mov_x0_x19 = 0xaa1303e0u; /* MOV X0, X19 — what the patch writes */
    static const uint32_t epilogue[4] = {
        0xa9427bf7u, /* LDP X23, LR,  [SP, #32] */
        0xa9415bf5u, /* LDP X21, X22, [SP, #16] */
        0xa8c353f3u, /* LDP X19, X20, [SP], #48 */
        0xd65f03c0u  /* RET */
    };
    uint32_t w[4];
    uint32_t insn;
    bool pristine, patched;

    if (!entry || !entry->native_code || entry->native_size < 32) return false;
    size_t pos = entry->native_size - 32;
    for (size_t i = 0; i < 4; i++)
        memcpy(&w[i], entry->native_code + pos + i * 4, sizeof(w[i]));

    pristine = (w[0] == arm64_nop && w[1] == arm64_nop && w[2] == arm64_nop && w[3] == arm64_nop);
    /* B is 0b000101<imm26>; the trampoline branch is unconditional and always in range by construction. */
    patched = (w[0] == arm64_mov_x0_x19 && (w[1] & 0xfc000000u) == 0x14000000u &&
               w[2] == arm64_nop && w[3] == arm64_nop);
    if (!pristine && !patched) return false;

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

/* MacRunner 2026-07-30 — CHAIN-ENTRY TRAMPOLINE.
 *
 * Why this exists: patch_block_tail below writes a direct `B` to next->native_code+12, i.e. a raw
 * pointer into the JIT arena. When the target is evicted its arena memory is reused, so every branch
 * still pointing at it becomes a jump into unrelated code. Eviction does try to undo those patches
 * (block_cache_unchain_references), but that path needs the arena writable and silently gives up
 * when it is not — and undoing a patch means rewriting INSTRUCTIONS, which on ARM64 obliges us to
 * get i-cache maintenance right against threads that may be executing them. That combination is why
 * MACRUNNER_HB_BLOCK_CHAIN has been default-off and marked unsafe, and why it has been armed in
 * 0 of 218 runs — leaving every guest block transition to pay a full dispatcher round trip, which
 * the profile shows as 297-339 of ~535 samples on the critical thread inside hb_jit_runtime_run.
 *
 * The trampoline removes the hazard instead of tracking it. Predecessors branch to a per-target stub
 *
 *      LDR X16, <literal>
 *      BR  X16
 *      <literal>            ; live entry, or this target's bail-out
 *
 * and eviction becomes ONE 8-byte store to the literal. Every inbound chain is redirected at once,
 * however many there are, so single-slot metadata stops being a limitation. A literal is data, not
 * code, so no i-cache maintenance is involved in the switch at all — which is the part that made
 * patching the branch itself hard to do safely.
 *
 * The bail-out is generated per target rather than shared because it has to name the guest address
 * to resume at:
 *
 *      LDR X1, <literal>            ; guest_addr
 *      STR X1, [X0, #pc]            ; X0 holds ctx — the chain slot's MOV X0, X19 put it there
 *      LDP X23, LR,  [SP, #32]      ; the same epilogue every block ends with, so the frame the
 *      LDP X21, X22, [SP, #16]      ; ORIGINATING block pushed is unwound exactly as on a normal
 *      LDP X19, X20, [SP], #48      ; return, and the dispatcher resumes at ctx->pc
 *      RET
 *
 * Space is committed BEFORE the instructions are built, and the instructions are then written
 * knowing the final address. That is not fussiness: the literal must be 8-byte aligned for the
 * eviction store to be atomic, and the offset that achieves that is only knowable once the arena
 * has handed out an address. */

#define HB_CHAIN_TRAMPOLINE_BYTES 128

static uint32_t arm64_ldr_x_literal(int rt_reg, ptrdiff_t byte_delta) {
    /* LDR Xt, <label> — imm19 counts instructions, not bytes. */
    uint32_t imm19 = (uint32_t)((byte_delta / 4) & 0x7ffff);
    return 0x58000000u | (imm19 << 5) | (uint32_t)rt_reg;
}

static uint32_t arm64_br_reg(int rn) { return 0xd61f0000u | ((uint32_t)rn << 5); }

static uint32_t arm64_str_x_off(int rt_reg, int rn, unsigned byte_off) {
    return 0xf9000000u | (((uint32_t)(byte_off / 8) & 0xfffu) << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rt_reg;
}

static uint32_t arm64_ldr_x_off(int rt_reg, int rn, unsigned byte_off) {
    return 0xf9400000u | (((uint32_t)(byte_off / 8) & 0xfffu) << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rt_reg;
}

/* CMP Xn, Xm — SUBS XZR, Xn, Xm. */
static uint32_t arm64_cmp_x(int rn, int rm) {
    return 0xeb000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | 31u;
}

/* B.<cond> to an absolute address. cond 1 == NE. */
static uint32_t arm64_bcond_to(const uint8_t* from, const uint8_t* to, uint32_t cond) {
    intptr_t off = (intptr_t)(to - from);
    return 0x54000000u | ((uint32_t)((off / 4) & 0x7ffff) << 5) | (cond & 0xfu);
}

/* MacRunner 2026-07-30 — PC-GUARDED CHAIN: one layout helper, because three functions have to agree
 * about where the literals live and a silent disagreement between them would corrupt control flow.
 *
 * Offsets are derived from `dest`'s own address so both literals land 8-aligned — the eviction store
 * has to be a single atomic 8-byte write, and only the arena knows the final address. */
struct hb_chain_tramp_layout {
    size_t expect_lit;   /* .quad this target's guest_addr */
    size_t live_lit;     /* .quad live entry, or this trampoline's eviction bail-out */
    size_t mispredict;   /* bare epilogue: leave ctx->pc alone, return to the dispatcher */
    size_t evict_bail;   /* set ctx->pc to guest_addr, then epilogue */
    size_t total;
};

static bool chain_tramp_layout(const uint8_t* dest, struct hb_chain_tramp_layout* out) {
    size_t off;
    if (!dest || ((uintptr_t)dest & 3u) != 0) return false;

    /* guard (6 instrs) then the mispredict epilogue (4 instrs) */
    out->mispredict = 6 * 4;
    off = out->mispredict + 4 * 4;
    while ((((uintptr_t)dest + off) & 7u) != 0) off += 4;
    out->expect_lit = off;
    out->live_lit = off + 8;
    out->evict_bail = off + 16;
    out->total = out->evict_bail + 6 * 4;
    return out->total <= HB_CHAIN_TRAMPOLINE_BYTES;
}

/* Lays out, at `dest`: trampoline (LDR/BR + literal) followed by this target's bail-out.
 * Returns the trampoline address, or NULL if the layout cannot be aligned within the block. */
/* Builds into `out` the trampoline that will LIVE at `at`. The two are separated so the whole thing can be
 * assembled in a local buffer and handed to jit_commit_blob in one shot: every displacement below is
 * computed from `at`, every store goes to `out`. That keeps `jit_commit_blob` the only writer that ever
 * toggles the arena's W^X state — see chain_trampoline_for. */
static uint8_t* chain_trampoline_build_at(uint8_t* out, const uint8_t* at, uint8_t* live_entry,
                                          uint64_t guest_addr) {
    static const uint32_t epilogue[4] = {
        0xa9427bf7u, /* LDP X23, LR,  [SP, #32] */
        0xa9415bf5u, /* LDP X21, X22, [SP, #16] */
        0xa8c353f3u, /* LDP X19, X20, [SP], #48 */
        0xd65f03c0u  /* RET */
    };
    struct hb_chain_tramp_layout L;
    uint8_t* mis;
    uint8_t* bail;
    size_t i;

    if (!out || !at || !live_entry) return NULL;
    if (!chain_tramp_layout(at, &L)) return NULL;

    mis = out + L.mispredict;
    bail = out + L.evict_bail;

    /* The guard. X0 holds ctx — the predecessor's chain slot put it there with MOV X0, X19 — and the
     * predecessor's terminal has already stored its computed next PC into ctx->pc. So comparing that
     * against this target's own guest_addr asks exactly the right question: "is this the successor I
     * was chained for?" A match branches into the target's live code; a mismatch falls into a bare
     * epilogue and lets the dispatcher resolve ctx->pc as it always would.
     *
     * That check is what makes chaining safe for terminals with more than one successor. A Jcc that
     * takes its other edge, an indirect jump through a different vtable slot, an indirect call to a
     * different callee: each simply mispredicts and pays four extra instructions instead of executing
     * the wrong guest block. X16/X17 are IP0/IP1, scratch at any call boundary. */
    arm64_store_u32(out + 0, arm64_ldr_x_off(16, 0, 544 /* offsetof(hb_context_t, pc) */));
    arm64_store_u32(out + 4, arm64_ldr_x_literal(17, (ptrdiff_t)(L.expect_lit - 4)));
    arm64_store_u32(out + 8, arm64_cmp_x(16, 17));
    arm64_store_u32(out + 12, arm64_bcond_to(at + 12, at + L.mispredict, 1 /* NE */));
    arm64_store_u32(out + 16, arm64_ldr_x_literal(16, (ptrdiff_t)(L.live_lit - 16)));
    arm64_store_u32(out + 20, arm64_br_reg(16));

    /* Mispredict: unwind the frame the ORIGINATING block pushed, exactly as a normal return would, and
     * leave ctx->pc as the terminal set it. */
    for (i = 0; i < 4; i++)
        arm64_store_u32(mis + i * 4, epilogue[i]);
    memset(out + L.mispredict + 16, 0, L.expect_lit - (L.mispredict + 16)); /* alignment padding */

    /* Eviction bail-out: reached only because eviction flipped live_lit to point here, in which case
     * the guard has already confirmed ctx->pc == guest_addr; the store keeps that true for a resumed
     * dispatch and costs nothing. */
    arm64_store_u32(bail + 0,
                    arm64_ldr_x_literal(1, (ptrdiff_t)L.expect_lit - (ptrdiff_t)L.evict_bail));
    arm64_store_u32(bail + 4, arm64_str_x_off(1, 0, 544));
    for (i = 0; i < 4; i++)
        arm64_store_u32(bail + 8 + i * 4, epilogue[i]);

    __atomic_store_n((uint64_t*)(void*)(out + L.expect_lit), guest_addr, __ATOMIC_RELAXED);
    __atomic_store_n((uint64_t*)(void*)(out + L.live_lit), (uint64_t)(uintptr_t)live_entry,
                     __ATOMIC_RELAXED);
    return out;
}

/* Where the literal that selects live-entry vs bail-out lives, for a trampoline at `tramp`. */
static uint64_t* chain_trampoline_slot(uint8_t* tramp) {
    struct hb_chain_tramp_layout L;
    if (!chain_tramp_layout(tramp, &L)) return NULL;
    return (uint64_t*)(void*)(tramp + L.live_lit);
}

/* Address of this trampoline's bail-out, i.e. what the literal is set to on eviction. */
static uint8_t* chain_trampoline_bailout(uint8_t* tramp) {
    struct hb_chain_tramp_layout L;
    if (!chain_tramp_layout(tramp, &L)) return NULL;
    return tramp + L.evict_bail;
}

/* Get, or lazily create, the trampoline through which others reach `entry`. */
static uint8_t* chain_trampoline_for(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry) {
    hb_block_chain_meta_t* meta;
    uint8_t zeros[HB_CHAIN_TRAMPOLINE_BYTES];
    uint8_t* dest = NULL;

    if (!rt || !rt->block_cache || !entry || !entry->native_code) return NULL;
    meta = block_cache_chain_meta(rt->block_cache, entry, true);
    if (!meta) return NULL;
    if (meta->in_trampoline) return meta->in_trampoline;

    /* MacRunner 2026-07-30 — the make_writable/commit bracket around the BUILD is what was missing, and
     * it is why block chaining has never booted.
     *
     * jit_commit_blob makes the arena writable only for its own memcpy of `zeros` and then calls
     * hb_jit_buffer_commit, which re-protects it. chain_trampoline_build then wrote 20 instruction words
     * straight into that just-re-protected page — a plain store to non-writable JIT memory, which takes
     * the process down with no guest-visible exception. That matches the signature exactly: exit=5 within
     * a second of the first dispatch, no SIGSEGV/SIGBUS/quarantine/interp-fallback line anywhere.
     * patch_block_tail below has always bracketed its own two stores this way; this path did not.
     *
     * Bisected rather than guessed (HK, 45-second arms): emit side alone (MACRUNNER_HB_CHAIN_PATCH=0)
     * boots to Begin MonoManager with 19.15 M dispatches, while committing the trampoline with the tail
     * write still disabled (MACRUNNER_HB_CHAIN_WRITE=0) dies at exit=5 — and in that arm no trampoline is
     * ever branched to, so only building one can be at fault. */
    /* MacRunner 2026-07-30 — assembled in a LOCAL buffer and committed in ONE jit_commit_blob call, so no
     * writer outside jit_commit_blob ever toggles the arena's W^X state.
     *
     * The earlier version reserved the space, then wrote the instructions straight into the arena behind a
     * second make_writable/commit bracket. That bracket was itself suspect: measured across five arms, the
     * only configuration that boots is the one performing ZERO extra cycles (MACRUNNER_HB_CHAIN_PATCH=0),
     * while one extra cycle — whether from this build or from the tail patch — wedges the guest, even though
     * the chained transition it installs provably executes correctly
     * (`chaintransit: from=0x87ef3e43250 pc_after=0x87ef3e4325c blocks=4`). And such a bracket flushes
     * nothing: make_writable sets dirty_start = used, so the paired commit sees an empty dirty range and
     * skips __builtin___clear_cache, leaving correctness to the explicit block_cache_clear_icache below.
     *
     * jit_commit_blob places the blob at writable + used, so the address is predictable before the call;
     * the layout is computed for THAT address and the result verified against what we actually got, because
     * emitting a trampoline whose literals are relative to the wrong address would be silent corruption. */
    {
        uint8_t built[HB_CHAIN_TRAMPOLINE_BYTES];
        const uint8_t* predicted = rt->jit_mem->writable + rt->jit_mem->used;

        memset(zeros, 0, sizeof(zeros));
        memset(built, 0, sizeof(built));
        if (!chain_trampoline_build_at(built, predicted, entry->native_code + 12, entry->guest_addr))
            return NULL;
        if (jit_commit_blob(rt, built, sizeof(built), &dest) != HB_OK || !dest) return NULL;
        if (dest != predicted) return NULL; /* layout was computed for another address — refuse to chain */
    }
    block_cache_clear_icache(dest, HB_CHAIN_TRAMPOLINE_BYTES);
    meta->in_trampoline = dest;
    return dest;
}

/* Scope the fault rollback to the block its snapshot describes — see the call site for why. */
static int chain_scoped_rollback_enabled(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_CHAIN_SCOPED_ROLLBACK", 0);
}

static uint64_t t_chain_rollback_skipped;

static bool patch_block_tail(hb_jit_runtime_t* rt, hb_block_cache_entry_t* cur,
                             hb_block_cache_entry_t* next) {
    hb_block_chain_meta_t* meta;
    uint8_t* target;
    uint8_t* patch;
    size_t patch_offset;
    bool ok;

    if (!runtime_block_chain_enabled() || !runtime_chain_patch_enabled() ||
        !rt || !rt->jit_mem || !cur || !next) {
        t_chain_decline[CHAIN_DECL_GATE]++;
        return false;
    }
    if (!cur->valid || !next->valid || !cur->native_code || !next->native_code) {
        t_chain_decline[CHAIN_DECL_INVALID]++;
        return false;
    }
    meta = block_cache_chain_meta(rt->block_cache, cur, true);
    if (!meta) { t_chain_decline[CHAIN_DECL_NOMETA]++; return false; }
    if (meta->target_code) {
        t_chain_decline[CHAIN_DECL_ALREADY]++;
        /* "Already chained to this same successor?" — answered by the guest address stored
         * alongside, not by the code pointer. meta->target_code holds the TRAMPOLINE address
         * (assigned from `target` below), never next->native_code + 16, so the old comparison
         * was unconditionally false: a tail that was already patched always reported failure and
         * could never be re-aimed. The trampoline also enters its target at +12
         * (chain_trampoline_for) while update_indirect_ic uses +16, so no single constant would
         * have made the pointer form right either. */
        return meta->guest_addr == next->guest_addr;
    }
    if (!block_terminal_is_chainable(cur->block)) {
        t_chain_decline[CHAIN_DECL_TERMINAL]++;
        return false;
    }
    if (runtime_chain_refuse_near() && chain_edge_is_near(cur->guest_addr, next->guest_addr)) {
        t_chain_decline[CHAIN_DECL_REFUSED]++;
        return false;
    }
    if (runtime_chain_forward_only_enabled() && next->guest_addr <= cur->guest_addr) {
        t_chain_decline[CHAIN_DECL_BACKEDGE]++;
        return false;
    }
    if (!entry_has_chain_slot(cur, &patch_offset)) {
        t_chain_decline[CHAIN_DECL_SLOT_CUR]++;
        return false;
    }
    if (!entry_has_chain_slot(next, NULL)) {
        t_chain_decline[CHAIN_DECL_SLOT_NEXT]++;
        return false;
    }

    /* Through the trampoline, not at the code: see chain_trampoline_build above. Falling back to
     * the raw address would reintroduce exactly the dangling-branch hazard this exists to remove,
     * so a trampoline we cannot create means we do not chain. */
    target = chain_trampoline_for(rt, next);
    if (!target) { t_chain_decline[CHAIN_DECL_TRAMP]++; return false; }
    patch = cur->native_code + patch_offset;
    if (!arm64_branch_reaches(patch + sizeof(uint32_t), target)) {
        t_chain_decline[CHAIN_DECL_REACH]++;
        return false;
    }

    /* Bisect stop: the trampoline above is committed, the block tail is left alone. */
    if (!runtime_chain_write_enabled()) {
        t_chain_decline[CHAIN_DECL_WRITEGATE]++;
        return false;
    }

    {
        uint64_t cap = runtime_chain_max_patches();
        uint64_t n = __atomic_add_fetch(&g_chain_patches_installed, 1, __ATOMIC_RELAXED);
        if (cap && n > cap) {
            __atomic_sub_fetch(&g_chain_patches_installed, 1, __ATOMIC_RELAXED);
            t_chain_decline[CHAIN_DECL_CAPPED]++;
            return false;
        }
        if (trace_chain_edge_enabled() &&
            (n <= 64 || chain_edge_is_near(cur->guest_addr, next->guest_addr))) {
            fprintf(stderr, "macrunner-hb-chainedge: n=%llu cur=0x%llx next=0x%llx tramp=%p\n",
                    (unsigned long long)n, (unsigned long long)cur->guest_addr,
                    (unsigned long long)next->guest_addr, (void*)target);
            fflush(stderr);
        }
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        t_chain_decline[CHAIN_DECL_WPROT]++;
        return false;
    }
    arm64_store_u32(patch, arm64_mov_reg_u32(0, 19)); /* MOV X0, X19 (ctx) */
    arm64_store_u32(patch + sizeof(uint32_t),
                    arm64_b_to(patch + sizeof(uint32_t), target));
    block_cache_clear_icache(patch, 2 * sizeof(uint32_t));
    ok = hb_jit_buffer_make_executable(rt->jit_mem) == HB_OK;
    if (!ok) { t_chain_decline[CHAIN_DECL_XPROT]++; return false; }
    t_chain_decline[CHAIN_DECL_PATCHED]++;

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

    /* Counted before any decline, so "the handler never consults the guard" and "it consults and
     * refuses" stay distinguishable — they argue for the same conclusion but by different routes. */
    __atomic_add_fetch(&g_guard_claim_calls, 1, __ATOMIC_RELAXED);

    if (!frame || !frame->rt || !frame->rt->jit_mem || !frame->entry ||
        !frame->entry->native_code || !frame->entry->native_size) {
        __atomic_add_fetch(&g_guard_claim_declined_frame, 1, __ATOMIC_RELAXED);
        return 0;
    }

    native_start = (uintptr_t)frame->entry->native_code;
    native_end = native_start + frame->entry->native_size;
    slab_start = (uintptr_t)frame->rt->jit_mem->executable;
    slab_end = slab_start + frame->rt->jit_mem->used;
    if (native_end < native_start || slab_end < slab_start) {
        __atomic_add_fetch(&g_guard_claim_declined_range, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (!((uintptr_t)pc >= native_start && (uintptr_t)pc < native_end) &&
        !((uintptr_t)pc >= slab_start && (uintptr_t)pc < slab_end)) {
        __atomic_add_fetch(&g_guard_claim_declined_range, 1, __ATOMIC_RELAXED);
        return 0;
    }

    __atomic_add_fetch(&g_guard_claim_taken, 1, __ATOMIC_RELAXED);
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
    /* The second route into the recovery branch. Counted on the same pair of counters so
     * claim_taken means "siglongjmp was attempted", by whichever door. */
    __atomic_add_fetch(&g_guard_claim_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_guard_claim_taken, 1, __ATOMIC_RELAXED);
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

/* MacRunner 2026-07-30 — the per-block context snapshot, minus the part the JIT cannot touch.
 *
 * Stable baseline from three loop iterations: run_jit_block_with_signal_guard 96/99/102 samples on
 * the critical thread with _platform_memmove 65/69/80 of them, i.e. the copy below is the largest
 * identified cost left on that thread now that promotion is off.
 *
 * sizeof(hb_context_t) is 2616 bytes, and bytes [1472, 2496) — xmm_ext, ymm_hi_ext, zmm_hi_ext —
 * are one contiguous 1024-byte block the header calls "Interpreter-only ... for AVX-512 ZMM16..31".
 * Verified rather than trusted: a grep of the whole engine finds those three fields referenced ONLY
 * in hb_interpreter.c and hb_context.c, with zero mentions in hb_arm64_codegen.c or in this file.
 * So emitted code cannot change them, and not snapshotting them is not an approximation — it is the
 * correct rule, restore-what-changed.
 *
 * BOTH sides must skip the same range. The memset above no longer zeroes `snapshot`, so the
 * un-copied bytes hold whatever was on the stack; a full-struct restore would push that garbage into
 * the live context. Hence a matching pair rather than a lone optimisation on the save path.
 *
 * DEFAULT FLIPPED ON 2026-07-30, after the measurement this gate was waiting for. Run SNAPSHOT760 with the
 * widened window (760 bytes instead of the full 2616): on the critical thread `_platform_memmove` self time
 * fell from 81/536 = 15.1 % to 16/498 = 3.2 %, and run_jit_block_with_signal_guard's own self time from
 * 10.6 % to 7.8 % — a within-run before/after on the exact item the change targets. Time to
 * `Restored language` came in at 369.0 s against 448.4 s and 490.3 s for the two previous best runs and a
 * 593 +- 78 s project baseline, with the Mono phase at 109.7 s against 135.5/150.5 s, and ZERO
 * `HyperBridge run failed` lines.
 *
 * The marker time is n=1, so the load-bearing evidence is the profile share, not the clock.
 * MACRUNNER_HB_SNAPSHOT_SKIP_INTERP=0 restores the full-struct copy in one env var if anything downstream
 * disagrees. */
static int snapshot_skip_interp_only(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SNAPSHOT_SKIP_INTERP", 1);
}

/* MacRunner 2026-07-30 — the window starts at ymm_hi, not xmm_ext, which nearly triples what it skips.
 *
 * Two things had to be established first. (1) This gate is DEFAULT OFF, so every run measured this week has
 * been copying the whole 2616-byte struct (`*dst = *src`), not the 1592 the comment above describes — the
 * profile's 15.1 % `_platform_memmove` is the cost of the FULL copy. (2) The interpreter-only AVX region is
 * not just xmm_ext..zmm_hi_ext but starts three fields earlier: ymm_hi [640,896), zmm_hi [896,1408) and the
 * AVX-512 opmask k [1408,1472) are contiguous with xmm_ext [1472,1728), ymm_hi_ext [1728,1984) and
 * zmm_hi_ext [1984,2496) — one unbroken 1856-byte run of interpreter EVEX/AVX state.
 *
 * Safe because the emitter cannot reach it, checked exhaustively rather than by sampling: the COMPLETE set of
 * `offsetof(hb_context_t, …)` in hb_arm64_codegen.c is block_count, flags, guest32_base,
 * indirect_ic_guest_addr, indirect_ic_native_code, last_result, lazy_flags, pc, step_count — plus the regs
 * union — and every one of those lies outside [640,2496): regs [32,432), flags [432,438),
 * lazy_flags [440,504), step_count [512,520), block_count [528,536), pc [544,552), last_result [584,588),
 * guest32_base [2496,2504), codegen_flags [2504,2508), indirect_ic_* [2512,2528). ymm_hi/zmm_hi/k are
 * referenced 0 times in the emitter and only from hb_interpreter.c and hb_context.c.
 *
 * Result: the snapshot copies [0,640) + [2496,2616) = 760 bytes instead of 2616, a 71 % cut on a path taken
 * on every single dispatch. Still one gate, still both sides skipping the same range. */
#define HB_CTX_INTERP_ONLY_BEGIN offsetof(hb_context_t, ymm_hi)
#define HB_CTX_INTERP_ONLY_END   offsetof(hb_context_t, guest32_base)

/* Measurement arm for the per-dispatch snapshot — see the call site. Default OFF, and it must stay
 * OFF: it prices the copy, it does not replace it. */
static int snapshot_measure_skip_save(void) {
    static int cached = -1;
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_SNAPSHOT_MEASURE_SKIP", 0);
}

static inline void hb_ctx_snapshot_save(hb_context_t* dst, const hb_context_t* src) {
    if (!snapshot_skip_interp_only()) { *dst = *src; return; }
    memcpy(dst, src, HB_CTX_INTERP_ONLY_BEGIN);
    memcpy((char*)dst + HB_CTX_INTERP_ONLY_END, (const char*)src + HB_CTX_INTERP_ONLY_END,
           sizeof(hb_context_t) - HB_CTX_INTERP_ONLY_END);
}

static inline void hb_ctx_snapshot_restore(hb_context_t* dst, const hb_context_t* src) {
    if (!snapshot_skip_interp_only()) { *dst = *src; return; }
    memcpy(dst, src, HB_CTX_INTERP_ONLY_BEGIN);
    memcpy((char*)dst + HB_CTX_INTERP_ONLY_END, (const char*)src + HB_CTX_INTERP_ONLY_END,
           sizeof(hb_context_t) - HB_CTX_INTERP_ONLY_END);
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
    /* MacRunner 2026-07-30 — this runs on EVERY guest block dispatch, and a profile of the thread
     * that actually drives startup found it: of 273 samples inside hb_jit_runtime_run, 39 were
     * _platform_memset and 47 _platform_memmove, both under this function — 31 % of the critical
     * path spent on bookkeeping rather than on the guest.
     *
     * The memset zeroes the whole frame, whose largest member by far is `snapshot`, a full
     * hb_context_t at 2616 bytes — and that member is then completely overwritten by the assignment
     * a few lines below. Zeroing it is pure waste, so skip it: that is about two thirds of the
     * zeroing on a path taken millions of times per startup.
     *
     * Written as two ranges around the member rather than as a list of field initialisers so a field
     * added later still gets zeroed by construction: it necessarily falls in one range or the other.
     * The whole correctness argument is that `snapshot` is unconditionally assigned below, which is
     * checkable at a glance.
     *
     * The 2616-byte COPY is deliberately NOT touched here. It is the pre-image the fault path
     * restores with `*ctx = frame.snapshot`, so eliding it needs a policy for blocks that have never
     * faulted — a behaviour change that wants its own measurement, not a drive-by. */
    memset(&frame, 0, offsetof(hb_jit_signal_fault_frame_t, snapshot));
    memset((char*)&frame + offsetof(hb_jit_signal_fault_frame_t, snapshot) + sizeof(frame.snapshot),
           0,
           sizeof(frame) - offsetof(hb_jit_signal_fault_frame_t, snapshot) - sizeof(frame.snapshot));
    frame.prev = g_jit_signal_fault_frame;
    frame.rt = rt;
    frame.ctx = ctx;
    frame.entry = cached;
    /* MacRunner 2026-08-01 — MEASUREMENT ARM. Not a candidate default, and not safe as one.
     *
     * The census settled that this snapshot is restored 0 times in 741 M dispatches, and the
     * profile says the guard is the largest attackable item on the critical thread: 14.4 % / 13.3 %
     * self across two samples, plus 5.6 % / 5.4 % of memmove+memset, against 16.9 % / 15.3 % for
     * hb_jit_runtime_run itself. The 760-byte copy is `static inline`, so most of it lands in the
     * guard's OWN self time rather than in _platform_memmove — which is why the earlier 3.2 %
     * memmove reading understated it.
     *
     * Before building the FEX-style resume that would make eliding this copy CORRECT (host-offset
     * map -> exact faulting guest RIP -> no rollback at all), measure what eliding it is worth.
     * This gate answers that and nothing else: it skips the save, leaving the recovery path with a
     * frame whose `snapshot` is zeroed rather than a true pre-image.
     *
     * That is only tolerable because it is observable: recovery entries are counted UNGATED by the
     * census, so a run that takes this arm reports `total_recover=` and any non-zero value
     * invalidates its own timing. Never default this on; the real fix is the resume map. */
    frame.aa_enabled = jit_aa_sigbus_probe_enabled();
    /* The aa probe reads frame.snapshot for its pre/post windows, so it forces the copy even when
     * the no-snapshot gate is on: a probe that reads uninitialised memory would report noise and
     * look like a finding. Probe off is the normal configuration, so this costs nothing in a
     * measurement run -- but it must be stated, because the two gates otherwise interact silently. */
    frame.snapshot_valid = !nosnapshot_enabled() || frame.aa_enabled;
    if (frame.snapshot_valid && !snapshot_measure_skip_save())
        hb_ctx_snapshot_save(&frame.snapshot, ctx);
    frame.steps = steps;
    frame.blocks_executed = blocks_executed;
    if (frame.aa_enabled) {
        frame.aa_dst_pre_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rcx, frame.aa_dst_pre);
        frame.aa_src_pre_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rdx, frame.aa_src_pre);
    }
    g_jit_signal_fault_frame = &frame;

    /* Counted here, before sigsetjmp, so it is one increment per dispatch on both the returning
     * path and the faulting one. Thread-local, so the guard's own re-entry through siglongjmp
     * cannot double-count it. */
    if ((++t_guard_dispatch & (HB_GUARD_CENSUS_PERIOD - 1)) == 0)
        guard_census_flush("period");

    if (sigsetjmp(frame.env, 0) == 0) {
        exec = (jit_block_t)(void*)cached->native_code;
        frame.dispatched_guest = cached->guest_addr;
        frame.dispatched_native = (uint64_t)(uintptr_t)exec;
        frame.dispatched_native_size = cached->native_size;
        /* One call per native dispatch, which is what makes this the right place for it: every
         * dispatch reaches here exactly once, on the legacy path and the fast path alike. */
        dispatch_stats_note_terminal(cached->block);
        if (trace_null_pc_enabled_rt()) hb_trace_current_block_addr = cached->guest_addr;
        exec(ctx);
        g_jit_signal_fault_frame = frame.prev;
        return HB_OK;
    }

    /* Reached only via siglongjmp, i.e. the snapshot is about to be used. The first few print
     * individually because "does it ever happen at all" is the question, and a purely periodic
     * report cannot distinguish never from rarely; after that the period keeps a hot branch from
     * drowning the log in its own measurement. */
    ++t_guard_recover;
    if (t_guard_recover <= 8 || (t_guard_recover & 0xfffu) == 0)
        guard_census_flush("recover");

    g_jit_signal_fault_frame = frame.prev;
    if (frame.aa_enabled && frame.signal == SIGBUS) {
        frame.aa_dst_post_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rcx, frame.aa_dst_post);
        frame.aa_src_post_valid = jit_aa_capture_window(
            frame.snapshot.memory, frame.snapshot.regs.x64.rdx, frame.aa_src_post);
    }
    /* MacRunner 2026-07-31 — a snapshot may only roll back the block it describes.
     *
     * hb_ctx_snapshot_save() runs ONCE per dispatch (above). Unchained, a dispatch is one block
     * and the contract holds. Chained, one dispatch retires 2-6 blocks (traces show blocks=2,3,4,6)
     * while the snapshot still describes the state before the FIRST — so restoring it after a fault
     * in a later block discards guest work that legitimately completed. With 3.6 M faults per run
     * (99.3% BUS_ADRALN) this is a hot path, not an edge case, and it matches every symptom: damage
     * accumulating across consecutive edges, values identical run to run because they are STALE
     * rather than random, and the REFUSE_NEAR differential reporting the defect as general to
     * chaining — any chain longer than one block has it.
     *
     * frame.entry is the block the snapshot was taken for; block_cache_find_native_pc() recovers
     * the block that actually faulted. When they differ, the snapshot is simply not a valid
     * pre-image, and applying it is worse than leaving the context alone: ctx->pc is maintained by
     * every terminator, so the dispatcher can carry on from where the guest really is.
     *
     * Gated, default OFF, because "do not roll back" leaves the faulting block's partial effects in
     * place — the lesser of two wrongs, but a behaviour change that has to be measured rather than
     * assumed. */
    faulted = block_cache_find_native_pc(rt->block_cache, frame.host_pc);
    /* Compare the FEX-style reconstruction against what the snapshot would restore, before the
     * snapshot is applied and the evidence is gone. */
    if (!frame.snapshot_valid) {
        /* No pre-image exists, so there is nothing to roll back TO. Recover the guest POSITION
         * from the map and let the dispatcher carry on from where the guest actually is -- which
         * is what QEMU and FEX do, and what ctx->pc alone cannot give us, since it names the block
         * entry rather than the faulting instruction.
         *
         * Note frame.snapshot is NOT read here, not even for a comparison: it was never written,
         * and reading it would be reading uninitialised stack. */
        uint64_t guest = ripmap_guest_for_host_pc(faulted, frame.host_pc);
        if (guest) {
            ctx->pc = guest;
            sync_arch_pc_after_jit_block(ctx);
            t_nosnap_resolved++;
        } else {
            t_nosnap_unresolved++;
        }
        /* Printed from the first occurrence: "does the cold branch ever fire, and can the map
         * answer when it does" is the whole question this arm exists to settle, and a periodic
         * report cannot tell never from rarely. */
        if ((t_nosnap_resolved + t_nosnap_unresolved) <= 16 ||
            ((t_nosnap_resolved + t_nosnap_unresolved) & 0xffu) == 0)
            fprintf(stderr,
                    "macrunner-hb-nosnap: resolved=%llu unresolved=%llu guest=0x%llx "
                    "host_pc=0x%llx entry_guest=0x%llx\n",
                    (unsigned long long)t_nosnap_resolved,
                    (unsigned long long)t_nosnap_unresolved, (unsigned long long)guest,
                    (unsigned long long)frame.host_pc,
                    (unsigned long long)(frame.entry ? frame.entry->guest_addr : 0));
    } else {
        /* Compare the map's answer against what the snapshot would restore, before the snapshot is
         * applied and the evidence is gone. */
        ripmap_check(faulted, frame.host_pc, frame.snapshot.pc);
        if (!chain_scoped_rollback_enabled() || !faulted || faulted == frame.entry)
            hb_ctx_snapshot_restore(ctx, &frame.snapshot);
        else {
        /* Print it. Four times this session a counter I added decided the outcome instead of the
         * thing being measured — a 2^23 census period that turned 36% into a reported 57%, an
         * unrepresentative early sample, and twice a predicate whose counters never fired because
         * a short-circuit upstream meant it was never called. A counter nobody reads is worse than
         * no counter: it looks like evidence. */
        if (++t_chain_rollback_skipped <= 8 || (t_chain_rollback_skipped & 0x3ffu) == 0)
            fprintf(stderr,
                    "macrunner-hb-chain-rollback-skipped: n=%llu faulted_guest=0x%llx entry_guest=0x%llx\n",
                    (unsigned long long)t_chain_rollback_skipped,
                    (unsigned long long)(faulted ? faulted->guest_addr : 0),
                    (unsigned long long)(frame.entry ? frame.entry->guest_addr : 0));
        }
    }
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
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
    /* A fused hot-family block: written straight into jit_mem, never offered to the persistent
     * cache. Counted here so it stops looking like a cache decline. */
    hb_contract_telemetry_record_promote_compile();
    block_cache_put(rt, rt->block_cache, block->guest_addr, dest, emitted_size,
                    (uint32_t)block->instr_count, block, true, false);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=self-loop block=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)block->guest_addr, dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

/* MacRunner 2026-07-30 — one kill switch for all seven families, so the question they raise can be
 * answered by measurement instead of by reading.
 *
 * What a profile of the thread that drives startup shows: try_promote_hot_block_families is 40
 * samples at the top of stack, and under the helper it installs —
 * hb_jit_helper_exec_two_block_loop — sits hb_jit_helper_exec_ir_block_once, then
 * exec_instr_unlocked, then mem_read → hb_memory_read → find_region_normalized. That is the
 * INTERPRETER, with a region lookup per guest memory access.
 *
 * Reading the helper confirms the shape: it tries three hand-written fast paths
 * (test/jne epilogue, cmp/rol/test, vector store) and, when none of them matches, runs a
 * budgeted while loop interpreting the IR block by block. So a promoted hot loop is not translated
 * to ARM64 at all — it becomes a call into an interpreter — while an unpromoted block goes through
 * run_jit_block_with_signal_guard and executes native code.
 *
 * Which of those is faster is not obvious and must not be guessed: the fused path saves dispatch and
 * guard overhead per iteration, the plain path executes real instructions. Hence a gate rather than a
 * deletion. Default 1, so this commit changes nothing until the A/B says which way to set it. */
static int promote_families_enabled(void) {
    static int cached = -1;
    /* Default flipped to 0 on 2026-07-30. Measured, three runs:
     *   promotion on   771.0 s and 780.0 s to the language marker
     *   promotion off  250.3 s
     * plus the profile of the critical thread, 113 -> 35 samples in hb_jit_runtime_run, with every
     * component falling together (two_block_loop 34->0, signal guard 32->5, memmove 25->4,
     * find_region 8->0). The two "on" runs landing within 9 s of each other is what makes this a
     * bimodal distribution rather than noise, and it retro-explains the 272/321/463/528/750/804 s
     * spread that made every A/B this month unreadable: fast runs were the ones where promotion
     * never caught a hot loop.
     *
     * =1 restores the old behaviour. The three hand-written fast paths inside
     * hb_jit_helper_exec_two_block_loop are presumably a win where they match; what loses is the
     * interpreter fallback they sit in front of. Re-enabling per-family, once each family can be
     * measured on its own, is the follow-up. */
    return runtime_env_flag_cached(&cached, "MACRUNNER_HB_PROMOTE_FAMILIES", 0);
}

static void try_promote_hot_block_families(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                           const hb_ir_block_t* block) {
    if (!promote_families_enabled()) return;
    try_promote_copy_scan_counted_loop(rt, ctx, block);
    try_promote_bounded_scan_loop(rt, ctx, block);
    try_promote_byte_compare_loop(rt, ctx, block);
    try_promote_null_qword_scan_loop(rt, ctx, block);
    try_promote_i32_less_tiebreaker_comparator(rt, ctx, block);
    try_promote_unity_sort_inner_loop(rt, ctx, block);
    try_promote_self_loop(rt, ctx, block);
}

static bool should_retry_cached_promotion(const hb_block_cache_entry_t* entry) {
    /* This gate is a retry for promotion, so with promotion off there is nothing to retry into.
     * Without this line it fires on EVERY dispatch: the backoff below reads entry->hit_count,
     * but hit_count is only ever incremented at trace_jit_hot_block_tick():2771, which sits
     * BELOW that function's `!trace_jit_hot_blocks_enabled()` early return at :2770. Tracing is
     * off in every normal run, so hit_count stays 0 for the life of the entry, `hit_count < 4`
     * is permanently true, and each dispatch pays an extra block_cache_find() into a 36 MB
     * (524288 x ~72 B) open-addressed table — a likely DRAM access — plus a call to
     * try_promote_hot_block_families() that returns immediately because promote_families_enabled()
     * is 0. Pure cost, no effect.
     *
     * Found while accounting per-block dispatch cost: ~350-500 host instructions and ~1.5 KB of
     * memset/memcpy traffic per guest block, against a translated block averaging 34 ARM64
     * instructions. Halving the hash probing is small against that, but it is free and it is on
     * the hottest path in the engine. */
    if (!promote_families_enabled()) return false;
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
                /* Measure the relocation table before anything depends on it. */
                hb_contract_telemetry_record_reloc((unsigned long)code_buf->reloc_count,
                                                   code_buf->reloc_overflow ? 1 : 0);

                if (rt->persistent_cache && have_persistent_key &&
                    code_buf->code && code_buf->size) {
                    const uint8_t* store_code = NULL;
                    uint8_t* owned_store_code = NULL;
                    hb_cache_entry_t metadata;
                    if (native_blob_prepare_cache_store(code_buf, code_buf->code, code_buf->size,
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
                         * whose shape was not recognised. See native_blob_helper_call_count.
                         *
                         * Skipped entirely in table-driven mode: the mh_* reasons describe the
                         * old matcher, which did not run, and re-deriving them here would both
                         * cost a scan and put numbers in the log that mean nothing. The rl_*
                         * counters carry the attribution instead. */
                        if (native_blob_reloc_enabled()) {
                            /* already attributed by native_blob_prepare_cache_store */
                        } else if (native_blob_helper_call_count(code_buf->code, code_buf->size) > 1) {
                            hb_contract_telemetry_record_cache_store_skip_multi();
                            {   /* re-derive the reason for attribution only */
                                hb_cached_helper_stub_t why[HB_MULTI_HELPER_MAX];
                                size_t whyn = 0; hb_mh_reason_t r = HB_MH_OK;
                                (void)native_blob_helper_stubs(code_buf->code, code_buf->size,
                                                               compile_block, false, why, &whyn, &r);
                                hb_contract_telemetry_record_mh_reason((int)r);
                            }
                        }
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
                ripmap_attach(cached, code_buf);
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

/* MacRunner 2026-07-30 — CHAIN LENGTH: this wrapper used to count it here, and the number it produced
 * did not mean what it said. It divided blocks executed by calls to THIS function, describing that as
 * "blocks per dispatcher entry, exactly 1.0 when every transition goes through the dispatcher". But
 * the dispatcher is the while (1) loop inside hb_jit_runtime_run_inner: one call to this function
 * retires as many blocks as the guest runs before it leaves the lifted function, so the ratio was
 * blocks-per-lifted-function-entry and was already far above 1 with chaining off. Read as documented
 * it would have reported chaining as working on a build where it does nothing.
 *
 * The denominator has to be native dispatches, and that counter already exists, per-thread and
 * wall-clock stamped: dispatch_stats (MACRUNNER_HB_TRACE_DISPATCH_STATS=1) increments dispatches once
 * per dispatch and blocks by the real retired count, on the legacy path as well as the fast path, so
 * thread_blocks/thread_dispatches is the honest avg_chain. It is printed as avg_chain= there, beside
 * the terminal histogram that gives the ceiling. Duplicating it here bought a wrong second opinion. */
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
            uint64_t before_rcx = ctx->regs.x64.rcx;
            uint64_t before_rdx = ctx->regs.x64.rdx;
            uint64_t before_rbp = ctx->regs.x64.rbp;
            uint64_t before_rsp = ctx->regs.x64.rsp;
            /* Entry-state probe for ONE guest block, so the same line can be compared with chaining on and
             * off. The chained arm shows rcx/rdx arriving as f0e0993f/320eec31 — the exact values the fault
             * reports — and this says what they are when the block is reached the ordinary way. */
            if (trace_chain_edge_enabled() && cached->guest_addr == runtime_chain_edge_near()) {
                static uint64_t seen;
                if (__atomic_add_fetch(&seen, 1, __ATOMIC_RELAXED) <= 8)
                    fprintf(stderr, "macrunner-hb-blockentry: guest=0x%llx rcx=0x%llx rdx=0x%llx "
                                    "rax=0x%llx rsp=0x%llx n=%llu\n",
                            (unsigned long long)cached->guest_addr,
                            (unsigned long long)ctx->regs.x64.rcx, (unsigned long long)ctx->regs.x64.rdx,
                            (unsigned long long)ctx->regs.x64.rax, (unsigned long long)ctx->regs.x64.rsp,
                            (unsigned long long)seen), fflush(stderr);
            }
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
                    if (block_delta > 1)
                        trace_chain_transition(ctx, cached, block_delta, step_delta,
                                               before_rcx, before_rdx, before_rbp, before_rsp);
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
                /* Measure the relocation table before anything depends on it. */
                hb_contract_telemetry_record_reloc((unsigned long)code_buf->reloc_count,
                                                   code_buf->reloc_overflow ? 1 : 0);

                if (rt->persistent_cache && have_persistent_key &&
                    code_buf->code && code_buf->size) {
                    const uint8_t* store_code = NULL;
                    uint8_t* owned_store_code = NULL;
                    hb_cache_entry_t metadata;
                    if (native_blob_prepare_cache_store(code_buf, code_buf->code, code_buf->size,
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
                         * whose shape was not recognised. See native_blob_helper_call_count.
                         *
                         * Skipped entirely in table-driven mode: the mh_* reasons describe the
                         * old matcher, which did not run, and re-deriving them here would both
                         * cost a scan and put numbers in the log that mean nothing. The rl_*
                         * counters carry the attribution instead. */
                        if (native_blob_reloc_enabled()) {
                            /* already attributed by native_blob_prepare_cache_store */
                        } else if (native_blob_helper_call_count(code_buf->code, code_buf->size) > 1) {
                            hb_contract_telemetry_record_cache_store_skip_multi();
                            {   /* re-derive the reason for attribution only */
                                hb_cached_helper_stub_t why[HB_MULTI_HELPER_MAX];
                                size_t whyn = 0; hb_mh_reason_t r = HB_MH_OK;
                                (void)native_blob_helper_stubs(code_buf->code, code_buf->size,
                                                               compile_block, false, why, &whyn, &r);
                                hb_contract_telemetry_record_mh_reason((int)r);
                            }
                        }
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
            uint64_t before_rcx = ctx->regs.x64.rcx;
            uint64_t before_rdx = ctx->regs.x64.rdx;
            uint64_t before_rbp = ctx->regs.x64.rbp;
            uint64_t before_rsp = ctx->regs.x64.rsp;
            /* Entry-state probe for ONE guest block, so the same line can be compared with chaining on and
             * off. The chained arm shows rcx/rdx arriving as f0e0993f/320eec31 — the exact values the fault
             * reports — and this says what they are when the block is reached the ordinary way. */
            if (trace_chain_edge_enabled() && cached->guest_addr == runtime_chain_edge_near()) {
                static uint64_t seen;
                if (__atomic_add_fetch(&seen, 1, __ATOMIC_RELAXED) <= 8)
                    fprintf(stderr, "macrunner-hb-blockentry: guest=0x%llx rcx=0x%llx rdx=0x%llx "
                                    "rax=0x%llx rsp=0x%llx n=%llu\n",
                            (unsigned long long)cached->guest_addr,
                            (unsigned long long)ctx->regs.x64.rcx, (unsigned long long)ctx->regs.x64.rdx,
                            (unsigned long long)ctx->regs.x64.rax, (unsigned long long)ctx->regs.x64.rsp,
                            (unsigned long long)seen), fflush(stderr);
            }
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
                    if (block_delta > 1)
                        trace_chain_transition(ctx, cached, block_delta, step_delta,
                                               before_rcx, before_rdx, before_rbp, before_rsp);
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
            if (chain_accounting) {
                if (!chain_patch_enabled) t_chain_decline[CHAIN_SITE_PATCH_OFF]++;
                else if (!cached || !next_cached) t_chain_decline[CHAIN_SITE_NO_ENTRY]++;
                else t_chain_decline[CHAIN_SITE_CALLED]++;
            }
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
