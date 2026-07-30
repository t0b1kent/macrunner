#include "hb_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <setjmp.h>
#include <signal.h>

static __thread sigjmp_buf* g_safe_copy_jmp = NULL;
static struct sigaction g_prev_segv;
static struct sigaction g_prev_bus;
static int g_sig_handlers_installed = 0;

static void safe_copy_signal_handler(int sig, siginfo_t* info, void* context) {
    if (g_safe_copy_jmp) {
        siglongjmp(*g_safe_copy_jmp, 1);
    }
    if (sig == SIGSEGV && g_prev_segv.sa_sigaction) {
        g_prev_segv.sa_sigaction(sig, info, context);
    } else if (sig == SIGBUS && g_prev_bus.sa_sigaction) {
        g_prev_bus.sa_sigaction(sig, info, context);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void install_sig_handlers(void) {
    if (__atomic_test_and_set(&g_sig_handlers_installed, __ATOMIC_RELAXED)) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = safe_copy_signal_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
}
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* MacRunner 2026-06-23 (ABZU native-twin probe): targeted trace for the .data
 * global at guest VA 0x1429413d8 (RVA 0x29413d8). Logs, for each read/write that
 * touches the window, the region HB resolves (base/host_base/perm) and the host
 * pointer actually used — to confirm whether the RIP-relative STORE and LOAD of
 * the SAME guest RVA resolve to DIFFERENT host addresses. Env-gated. */
#define MACRUNNER_HB_DATADIVERGE_LO  0x142941000ULL
#define MACRUNNER_HB_DATADIVERGE_HI  0x142942000ULL
typedef struct hb_memory_environment {
    int trace_datadiverge;
    int disable_hot_cache;
    int trace_guest32_alias;
    int trace_native_writes;
    int trace_live_vm_access_fail;
    int live_vm_write_mach;
    int trace_store80;
    int trace_memcpy_len;
    int trace_jit_helper_fail;
    /* A/B arm selector for the hb_memory_protect multi-region path, so both arms
     * live in ONE binary: the list walk that shipped, or the treap range walk.
     * See the memmeter block below for why this is a switch and not two builds. */
    int memprotect_walk_list;
    unsigned long long trace_guest_write_start;
    unsigned long long trace_guest_write_stop;
} hb_memory_environment_t;

static hb_memory_environment_t macrunner_hb_memory_environment;
static int macrunner_hb_memory_environment_initialized;

static int hb_memory_env_enabled(const char* name) {
    const char* value = getenv(name);
    return value && value[0] && value[0] != '0';
}

void hb_memory_init_environment(void) {
    const char* live_fail;
    const char* live_mach;
    const char* guest_write;
    const char* trace_memcpy_len;
    char* endp = NULL;
    unsigned long long start = 0, stop = 0;

    if (__atomic_load_n(&macrunner_hb_memory_environment_initialized, __ATOMIC_ACQUIRE)) return;

    macrunner_hb_memory_environment.trace_datadiverge =
        hb_memory_env_enabled("MACRUNNER_HB_TRACE_DATADIVERGE");
    macrunner_hb_memory_environment.disable_hot_cache =
        hb_memory_env_enabled("MACRUNNER_HB_DISABLE_HOT_CACHE");
    macrunner_hb_memory_environment.trace_guest32_alias =
        hb_memory_env_enabled("MACRUNNER_HB_TRACE_GUEST32_ALIAS");
    macrunner_hb_memory_environment.trace_native_writes =
        hb_memory_env_enabled("MACRUNNER_HB_TRACE_NATIVE_WRITES");
    live_fail = getenv("MACRUNNER_HB_TRACE_LIVE_VM_ACCESS_FAIL");
    if (!live_fail || !live_fail[0]) live_fail = getenv("MACRUNNER_HB_TRACE_LIVE_VM_WRITE_FAIL");
    macrunner_hb_memory_environment.trace_live_vm_access_fail =
        live_fail && live_fail[0] && live_fail[0] != '0';
    live_mach = getenv("MACRUNNER_HB_LIVE_VM_WRITE_MACH");
    macrunner_hb_memory_environment.live_vm_write_mach =
        live_mach && live_mach[0] && atoi(live_mach) != 0;
    macrunner_hb_memory_environment.trace_store80 =
        getenv("MACRUNNER_HB_TRACE_STORE80") != NULL;
    trace_memcpy_len = getenv("MACRUNNER_HB_TRACE_MEMCPY_LEN");
    macrunner_hb_memory_environment.trace_memcpy_len =
        trace_memcpy_len && trace_memcpy_len[0];
    macrunner_hb_memory_environment.trace_jit_helper_fail =
        getenv("MACRUNNER_HB_TRACE_JIT_HELPER_FAIL") != NULL;
    /* Default is the treap range walk.  MACRUNNER_HB_MEMPROTECT_WALK=list selects
     * the pre-fix full-list scan, which is the control arm of the A/B. */
    {
        const char* walk = getenv("MACRUNNER_HB_MEMPROTECT_WALK");
        macrunner_hb_memory_environment.memprotect_walk_list =
            walk && walk[0] == 'l';
    }

    guest_write = getenv("MACRUNNER_HB_TRACE_GUEST_WRITE");
    if (guest_write && guest_write[0]) {
        start = strtoull(guest_write, &endp, 0);
        if (endp != guest_write)
            stop = (*endp == '-' || *endp == ':') ? strtoull(endp + 1, NULL, 0) : start + 1;
        else
            start = 0;
    }
    macrunner_hb_memory_environment.trace_guest_write_start = start;
    macrunner_hb_memory_environment.trace_guest_write_stop = stop;
    __atomic_store_n(&macrunner_hb_memory_environment_initialized, 1, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------------
 * memmeter -- direct measurement of the region-map cost.
 *
 * WHY IT EXISTS.  A `sample` of a live HK run put 913 of 2795 critical-path
 * samples inside hb_memory_protect as a childless leaf, and that was read as
 * "the O(N) list walk is long".  That is an INFERENCE from a sample ratio: it
 * assumes both which branch was hot and how long the list is.  Neither had been
 * measured.  These counters measure both, so the next run settles it instead of
 * arguing about it.
 *
 * DISCIPLINE, learned from this lane's own losses:
 *  - ALWAYS ON, not env-gated.  Only one HK title slot exists and three lanes
 *    compete for it, so every foreign lane's run must yield this data for free.
 *    The counters are relaxed atomic adds; the clock is read on 1 call in 64.
 *  - PERIODIC, not atexit.  A run killed by its timeout emits no atexit summary,
 *    and two of this lane's A/Bs were already lost that way.
 *  - fprintf, not snprintf into a fixed buffer.  The 1024-byte buffers in this
 *    tree return SILENTLY on overflow; a report line that can vanish is worse
 *    than none.  One fprintf is atomic under the FILE lock.
 *  - Counts are exact and uncapped.  A zero is a real zero.
 *
 * The decisive ratios the line reports:
 *    mp_visit / mp_walk   = nodes touched per multi-region protect  (walk length)
 *    regions              = live region count N at report time
 *    splits               = regions created by splitting, which NOTHING ever
 *                           merges back, so N only grows.
 */
static unsigned long long mm_mp_calls;      /* hb_memory_protect entries          */
static unsigned long long mm_mp_fast;       /* exact region, perm already correct */
static unsigned long long mm_mp_reenter;    /* contained-in-one-region split path */
static unsigned long long mm_mp_exact;      /* exact base+size region             */
static unsigned long long mm_mp_walk;       /* reached the multi-region walk      */
static unsigned long long mm_mp_visit;      /* nodes examined by that walk        */
static unsigned long long mm_mp_apply;      /* regions whose perm was written     */
static unsigned long long mm_mp_notfound;   /* walk matched nothing               */
static unsigned long long mm_mp_ns;         /* ns in protect, sampled 1/64        */
static unsigned long long mm_mp_ns_n;       /* how many calls that ns covers      */
static unsigned long long mm_slr_calls;     /* hb_memory_sync_live_range entries  */
static unsigned long long mm_slr_fast;      /* early return, map already correct  */
static unsigned long long mm_slr_scan;      /* nodes examined by its list scans   */
static unsigned long long mm_slr_repl;      /* took the replace+rebuild path      */
static unsigned long long mm_slr_ns;
static unsigned long long mm_slr_ns_n;
static unsigned long long mm_maps;          /* hb_memory_t instances created      */
static unsigned long long mm_ovl_calls;     /* any_overlap() calls (both map paths)*/
static unsigned long long mm_ovl_scan;      /* list nodes it examined              */
static unsigned long long mm_splits;        /* split_region_at allocations        */
static unsigned long long mm_rebuilds;      /* rebuild_region_tree calls          */
static unsigned long long mm_rebuild_nodes; /* nodes reinserted by those rebuilds */

#define MM_CLOCK_MASK      0x3fULL              /* time 1 call in 64 */
#define MM_REPORT_PERIOD_NS 5000000000ULL       /* one line per 5 s */
#define MM_REGION_WALK_CAP 4000000ULL   /* torn-list guard; the map is never this big */

/* The report cadence is TIME-based, not count-based.  A count trigger has to
 * guess the call rate: pick 262144 and a run that calls protect 100 times a
 * second emits its first line 45 minutes in, i.e. never, while a run calling it
 * a million times a second drowns the log.  Both failures are silent.  Keyed on
 * time, the line spacing is also directly usable as a time series. */
static unsigned long long mm_last_report_ns;

static bool mm_due(unsigned long long now) {
    unsigned long long last = __atomic_load_n(&mm_last_report_ns, __ATOMIC_RELAXED);
    if (now - last < MM_REPORT_PERIOD_NS) return false;
    /* One winner per period; losers skip rather than pile up duplicate lines. */
    return __atomic_compare_exchange_n(&mm_last_report_ns, &last, now, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

static unsigned long long mm_now_ns(void) {
#ifdef __APPLE__
    return (unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
#endif
}

static void mm_report(hb_memory_t* mem, const char* where) {
    unsigned long long n = 0;
    struct timeval tv;

    /* This walk is as safe as any_overlap(), which already runs unlocked on the
     * normal path -- but it runs once per 262144 calls, so cap it rather than
     * risk spinning on a list observed mid-mutation. */
    for (hb_region_t* r = mem ? mem->regions : NULL; r; r = r->next) {
        if (++n >= MM_REGION_WALK_CAP) break;
    }

    gettimeofday(&tv, NULL);
    /* `mem=` and `maps=` matter because the region map is PER GUEST THREAD --
     * macrunner_hb.c assigns ctx->memory = hb_memory_create(0) on each x64
     * context, and HK has been measured with 115 of them.  The global counters
     * aggregate across every map (so mp_visit/mp_walk, the walk length, is
     * map-agnostic and sound), but `regions` is one map's list length, and
     * without an identity a growth curve would silently interleave 115 of them. */
    fprintf(stderr,
            "macrunner-hb-memmeter: where=%s mem=%p maps=%llu epoch=%lld.%03d regions=%llu "
            "mp_calls=%llu mp_fast=%llu mp_reenter=%llu mp_exact=%llu mp_walk=%llu "
            "mp_visit=%llu mp_apply=%llu mp_notfound=%llu mp_ns=%llu mp_ns_n=%llu "
            "slr_calls=%llu slr_fast=%llu slr_scan=%llu slr_repl=%llu slr_ns=%llu slr_ns_n=%llu "
            "ovl_calls=%llu ovl_scan=%llu "
            "splits=%llu rebuilds=%llu rebuild_nodes=%llu walk=%s\n",
            where, (void*)mem, __atomic_load_n(&mm_maps, __ATOMIC_RELAXED),
            (long long)tv.tv_sec, (int)(tv.tv_usec / 1000), n,
            __atomic_load_n(&mm_mp_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_fast, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_reenter, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_exact, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_walk, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_visit, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_apply, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_notfound, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_ns, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_mp_ns_n, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_fast, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_scan, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_repl, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_ns, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_slr_ns_n, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_ovl_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_ovl_scan, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_splits, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_rebuilds, __ATOMIC_RELAXED),
            __atomic_load_n(&mm_rebuild_nodes, __ATOMIC_RELAXED),
            macrunner_hb_memory_environment.memprotect_walk_list ? "list" : "tree");
    fflush(stderr);
}

static int macrunner_hb_trace_datadiverge_enabled(void) {
    return macrunner_hb_memory_environment.trace_datadiverge;
}
static int macrunner_hb_datadiverge_hit(uint64_t addr, size_t size) {
    if (!macrunner_hb_trace_datadiverge_enabled()) return 0;
    if (addr + size < addr) return 0;
    return !(addr + size <= MACRUNNER_HB_DATADIVERGE_LO || addr >= MACRUNNER_HB_DATADIVERGE_HI);
}

static int macrunner_hb_disable_hot_cache_enabled(void) {
    return macrunner_hb_memory_environment.disable_hot_cache;
}

typedef struct hb_hot_cache_tls {
    hb_memory_t* mem;
    uint64_t epoch;
    hb_region_t* slot[HB_MEMORY_HOT_CACHE_SLOTS];
} hb_hot_cache_tls_t;

static __thread hb_hot_cache_tls_t g_hot_cache_tls;

static uint64_t hot_cache_epoch(hb_memory_t* mem) {
    return __atomic_load_n(&mem->hot_gen, __ATOMIC_ACQUIRE);
}

static void hot_cache_reset_tls(hb_memory_t* mem, uint64_t epoch) {
    g_hot_cache_tls.mem = mem;
    g_hot_cache_tls.epoch = epoch;
    memset(g_hot_cache_tls.slot, 0, sizeof(g_hot_cache_tls.slot));
}

/* MacRunner 2026-07-31 — is the region hot cache actually working?
 *
 * Clean critical-thread profile (instruments off) puts the guest-MMU group at 10.5 %:
 * find_region_normalized 3.0 % + hb_memory_read 5.4 % + hb_memory_write 2.1 %. The lookup already has an
 * O(log n) treap AND this per-thread MRU cache in front of it, so the cost is either cache misses falling
 * through to the treap or the cache being wiped. The wipe is the suspicious part: hot_cache_reset_tls clears
 * EVERY slot whenever the region epoch changes, and Mono maps and unmaps constantly, so a high reset rate
 * would mean the cache is permanently cold no matter how good its locality is.
 *
 * Three counters answer it from one run: lookups, hits, and resets. Gated and per-thread, reported every
 * 8 M lookups. hit_pct says whether the cache earns its keep; resets_per_1k_lookups says whether epoch churn
 * is destroying it. */
static __thread uint64_t t_hc_lookups, t_hc_hits, t_hc_resets, t_hc_next;

/* MacRunner 2026-07-31 — first measurement said 67.7 % hits / 0.04 resets per 1k lookups, so epoch churn is
 * NOT what costs us; the misses are. Two follow-up questions decide what to do about them, and both are
 * answerable in one run:
 *
 *   (a) How deep do HITS sit? The scan is 16 slots wide and linear. If hits concentrate in slot 0-1 the
 *       remaining 14 comparisons are dead weight on every miss.
 *   (b) Are MISSES curable by capacity? A miss that resolves to a real region (hot_cache_insert runs) can be
 *       caught by a bigger cache. A miss where the treap returns NULL — an unmapped guest address — can never
 *       be cached at any size, and needs a negative cache instead. t_hc_shadow is a 64-deep true-LRU shadow
 *       of the same reference stream, so the depth at which a missed region is found in it prices slots=32/64
 *       exactly, without rebuilding. */
#define HC_SHADOW 64
static __thread int t_hc_depth_hist[HB_MEMORY_HOT_CACHE_SLOTS];
static __thread uint64_t t_hc_miss_resolved, t_hc_shadow_absent;
static __thread hb_region_t* t_hc_shadow[HC_SHADOW];
static __thread uint64_t t_hc_shadow_hist[HC_SHADOW];
static __thread int t_hc_miss_pending;

static int trace_hot_cache_stats_enabled(void);
__thread uint64_t hb_trace_current_block_addr;
/* Which C caller produces the NULL answers. The guest-PC histogram came back diffuse (top block <=4 % of
 * 112.9 M), so the nulls are not one loop; the remaining question is whether they are a PROBE whose NULL is a
 * normal answer (hb_memory_host_ptr deciding the JIT cannot take a direct pointer) rather than a failed access.
 * One TLS tag set at each of the five resolve call sites answers it. */
enum { RSITE_OTHER = 0, RSITE_READ, RSITE_WRITE, RSITE_HOST_PTR, RSITE_FIND_REGION, RSITE_CHECK_PERM,
       RSITE_JIT_HOST_SPAN, RSITE_JIT_CODEGEN, RSITE_INTERP, RSITE_N };
static const char* const rsite_names[RSITE_N] = {
    "other", "read", "write", "host_ptr", "find_region", "check_perm",
    "jit_host_span", "jit_codegen", "interp"
};
__thread int hb_trace_rsite;
#define t_rsite hb_trace_rsite
static __thread uint64_t t_null_by_site[RSITE_N], t_lookup_by_site[RSITE_N];
static __thread uint64_t t_gap_hits, t_gap_fills, t_gap_flushes;  /* negative cache, defined below */
/* Express the saving in the unit that actually costs time: dependent, cache-cold pointer loads. A failed treap
 * walk over ~24000 regions is ~15-20 of them; the gap scan it replaces is a linear MRU array. Counting both
 * makes the trade a measured ratio instead of an argument from region count. */
static __thread uint64_t t_walk_nodes_fail, t_walk_fail, t_walk_nodes_ok, t_walk_ok, t_gap_scanned;

/* MacRunner 2026-07-31 — 97.4 % of the misses resolve to NULL: 167.7 M lookups per run for guest addresses
 * that NO region contains. A bigger cache cannot help those (measured: slots=64 would buy 0.3 points), so the
 * question is who asks. Bucket the unresolved addresses by 16 MB and keep one example each — if they cluster in
 * one or two buckets this is a single unregistered range (the guest stack is the obvious suspect: stack traffic
 * is the most frequent memory traffic any program has), which is a completely different and much cheaper fix
 * than tuning the cache. */
/* First cut of this sampler was first-come-first-served over 20 slots: twenty cold startup addresses took the
 * table and 99.96 % of the traffic fell into "other", so it measured nothing. Misra-Gries instead — a matching
 * key increments, a free slot is claimed, and an unmatched key decrements every counter. Any bucket holding
 * more than 1/32 of the stream is guaranteed to survive in the table, which is exactly the question. */
#define HC_NULLBUCKETS 32
static __thread uint64_t t_hc_null_key[HC_NULLBUCKETS], t_hc_null_cnt[HC_NULLBUCKETS];
static __thread uint64_t t_hc_null_ex[HC_NULLBUCKETS], t_hc_null_ex_last[HC_NULLBUCKETS];
static __thread uint64_t t_hc_null_other, t_hc_null_total;

/* Which guest block issues the unmapped lookups. Same Misra-Gries discipline as the address buckets: any
 * block responsible for more than 1/32 of the null stream is guaranteed to survive in the table. If they
 * concentrate in one or two blocks, the 166 M nulls are a loop, not diffuse probing — which is the difference
 * between a caching problem and a spin-wait worth far more than the cache. */
#define HC_PCSLOTS 32
static __thread uint64_t t_pc_key[HC_PCSLOTS], t_pc_cnt[HC_PCSLOTS], t_pc_addr[HC_PCSLOTS];
static __thread uint64_t t_pc_total, t_pc_evicted;

static void null_pc_note(hb_gva_t addr) {
    uint64_t pc = hb_trace_current_block_addr;
    int free_slot = -1;
    t_pc_total++;
    for (int i = 0; i < HC_PCSLOTS; i++) {
        if (t_pc_cnt[i] && t_pc_key[i] == pc) { t_pc_cnt[i]++; return; }
        if (!t_pc_cnt[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        t_pc_key[free_slot] = pc;
        t_pc_cnt[free_slot] = 1;
        t_pc_addr[free_slot] = (uint64_t)addr;
        return;
    }
    t_pc_evicted++;
    for (int i = 0; i < HC_PCSLOTS; i++) t_pc_cnt[i]--;
}

/* Set MACRUNNER_HB_NULL_SPAN_RECHECK=1 to restore the redundant re-lookup for an A/B. */
static int null_span_recheck_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_NULL_SPAN_RECHECK");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static int trace_null_pc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_NULL_PC");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static void hot_cache_note_unresolved(hb_gva_t addr) {
    uint64_t key = (uint64_t)addr >> 24;   /* 16 MB granularity */
    int free_slot = -1;
    if (!trace_hot_cache_stats_enabled()) return;
    if (trace_null_pc_enabled()) null_pc_note(addr);
    if (t_rsite < RSITE_N) t_null_by_site[t_rsite]++;
    /* the 16 MB address buckets already answered their question (two clusters, ~32 buckets); keep the code but
     * do not pay its 32-slot loop on every null unless asked */
    if (!getenv("MACRUNNER_HB_TRACE_NULL_BUCKETS")) return;
    t_hc_null_total++;
    for (int i = 0; i < HC_NULLBUCKETS; i++) {
        if (t_hc_null_cnt[i] && t_hc_null_key[i] == key) {
            t_hc_null_cnt[i]++;
            t_hc_null_ex_last[i] = (uint64_t)addr;
            return;
        }
        if (!t_hc_null_cnt[i] && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        t_hc_null_key[free_slot] = key;
        t_hc_null_cnt[free_slot] = 1;
        t_hc_null_ex[free_slot] = (uint64_t)addr;
        t_hc_null_ex_last[free_slot] = (uint64_t)addr;
        return;
    }
    t_hc_null_other++;
    for (int i = 0; i < HC_NULLBUCKETS; i++) t_hc_null_cnt[i]--;
}

static void hot_shadow_touch(hb_region_t* region, int* out_depth) {
    int depth = -1;
    for (int i = 0; i < HC_SHADOW; i++) {
        if (t_hc_shadow[i] != region) continue;
        depth = i;
        break;
    }
    if (out_depth) *out_depth = depth;
    if (depth == 0) return;
    int from = depth < 0 ? HC_SHADOW - 1 : depth;
    for (int i = from; i > 0; i--) t_hc_shadow[i] = t_hc_shadow[i - 1];
    t_hc_shadow[0] = region;
}

static int trace_hot_cache_stats_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_HOT_CACHE");
        cached = e && *e && *e != '0';
    }
    return cached;
}

/* Called from the resolve path with the region the treap returned, i.e. only after a real miss. */
static void hot_cache_note_resolved(hb_region_t* region) {
    int depth = -1;
    if (!trace_hot_cache_stats_enabled() || !t_hc_miss_pending) return;
    t_hc_miss_pending = 0;
    t_hc_miss_resolved++;
    hot_shadow_touch(region, &depth);
    if (depth < 0) t_hc_shadow_absent++;
    else t_hc_shadow_hist[depth]++;
}

static void hot_cache_note(int hit_depth, int reset, hb_region_t* hit_region) {
    if (!trace_hot_cache_stats_enabled()) return;
    t_hc_lookups++;
    if (t_rsite < RSITE_N) t_lookup_by_site[t_rsite]++;
    if (reset) t_hc_resets++;
    if (hit_depth >= 0) {
        t_hc_hits++;
        if (hit_depth < HB_MEMORY_HOT_CACHE_SLOTS) t_hc_depth_hist[hit_depth]++;
        hot_shadow_touch(hit_region, NULL);
        t_hc_miss_pending = 0;
    } else {
        t_hc_miss_pending = 1;
    }
    if (t_hc_lookups < t_hc_next) return;
    t_hc_next = t_hc_lookups + 8000000ull;

    uint64_t misses = t_hc_lookups - t_hc_hits;
    /* Cumulative hit rate a cache of N slots would have reached: real hits (all within 16) plus the missed
     * regions the 64-deep shadow found shallower than N. */
    uint64_t deeper = 0;
    for (int i = HB_MEMORY_HOT_CACHE_SLOTS; i < HC_SHADOW; i++) deeper += t_hc_shadow_hist[i];
    uint64_t d32 = 0;
    for (int i = HB_MEMORY_HOT_CACHE_SLOTS; i < 32; i++) d32 += t_hc_shadow_hist[i];
    double scan = 0.0;
    for (int i = 0; i < HB_MEMORY_HOT_CACHE_SLOTS; i++) scan += (double)t_hc_depth_hist[i] * (i + 1);
    fprintf(stderr, "macrunner-hb-hotcache: lookups=%llu hits=%llu hit_pct=%.2f resets=%llu "
                    "resets_per_1k_lookups=%.2f miss=%llu miss_resolved=%llu miss_null=%llu "
                    "miss_null_pct_of_miss=%.2f avg_hit_depth=%.2f hit_pct_if_32=%.2f hit_pct_if_64=%.2f "
                    "shadow_absent=%llu\n",
            (unsigned long long)t_hc_lookups, (unsigned long long)t_hc_hits,
            t_hc_lookups ? 100.0 * (double)t_hc_hits / (double)t_hc_lookups : 0.0,
            (unsigned long long)t_hc_resets,
            t_hc_lookups ? 1000.0 * (double)t_hc_resets / (double)t_hc_lookups : 0.0,
            (unsigned long long)misses, (unsigned long long)t_hc_miss_resolved,
            (unsigned long long)(misses - t_hc_miss_resolved),
            misses ? 100.0 * (double)(misses - t_hc_miss_resolved) / (double)misses : 0.0,
            t_hc_hits ? scan / (double)t_hc_hits : 0.0,
            t_hc_lookups ? 100.0 * (double)(t_hc_hits + d32) / (double)t_hc_lookups : 0.0,
            t_hc_lookups ? 100.0 * (double)(t_hc_hits + deeper) / (double)t_hc_lookups : 0.0,
            (unsigned long long)t_hc_shadow_absent);
    fprintf(stderr, "macrunner-hb-negcache: walk_fail=%llu nodes_per_failed_walk=%.1f "
                    "walk_ok=%llu nodes_per_ok_walk=%.1f gap_entries_scanned_per_hit=%.2f\n",
            (unsigned long long)t_walk_fail,
            t_walk_fail ? (double)t_walk_nodes_fail / (double)t_walk_fail : 0.0,
            (unsigned long long)t_walk_ok,
            t_walk_ok ? (double)t_walk_nodes_ok / (double)t_walk_ok : 0.0,
            t_gap_hits ? (double)t_gap_scanned / (double)t_gap_hits : 0.0);
    fprintf(stderr, "macrunner-hb-negcache: gap_hits=%llu gap_fills=%llu gap_flushes=%llu "
                    "gap_hits_per_1k_lookups=%.1f\n",
            (unsigned long long)t_gap_hits, (unsigned long long)t_gap_fills,
            (unsigned long long)t_gap_flushes,
            t_hc_lookups ? 1000.0 * (double)t_gap_hits / (double)t_hc_lookups : 0.0);
    fprintf(stderr, "macrunner-hb-nullsite:");
    for (int i = 0; i < RSITE_N; i++)
        fprintf(stderr, " %s=%llu/%llu", rsite_names[i], (unsigned long long)t_null_by_site[i],
                (unsigned long long)t_lookup_by_site[i]);
    fprintf(stderr, "\n");
    if (trace_null_pc_enabled()) {
        fprintf(stderr, "macrunner-hb-nullpc: total=%llu evicted=%llu", (unsigned long long)t_pc_total,
                (unsigned long long)t_pc_evicted);
        for (int i = 0; i < HC_PCSLOTS; i++) {
            if (t_pc_cnt[i] < t_pc_total / 100) continue;   /* >= 1 % of the null stream */
            fprintf(stderr, " [block=0x%llx n>=%llu pct>=%.1f ex_addr=0x%llx]",
                    (unsigned long long)t_pc_key[i], (unsigned long long)t_pc_cnt[i],
                    t_pc_total ? 100.0 * (double)t_pc_cnt[i] / (double)t_pc_total : 0.0,
                    (unsigned long long)t_pc_addr[i]);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "macrunner-hb-hotcache-null: total=%llu evicted=%llu",
            (unsigned long long)t_hc_null_total, (unsigned long long)t_hc_null_other);
    for (int i = 0; i < HC_NULLBUCKETS; i++) {
        if (t_hc_null_cnt[i] < t_hc_null_total / 200) continue;   /* >=0.5 % of the stream */
        fprintf(stderr, " [0x%llx000000 n>=%llu pct>=%.1f first=0x%llx last=0x%llx]",
                (unsigned long long)t_hc_null_key[i], (unsigned long long)t_hc_null_cnt[i],
                t_hc_null_total ? 100.0 * (double)t_hc_null_cnt[i] / (double)t_hc_null_total : 0.0,
                (unsigned long long)t_hc_null_ex[i], (unsigned long long)t_hc_null_ex_last[i]);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "macrunner-hb-hotcache-depth:");
    for (int i = 0; i < HB_MEMORY_HOT_CACHE_SLOTS; i++)
        fprintf(stderr, " d%d=%d", i, t_hc_depth_hist[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* MacRunner 2026-07-31 — the negative cache.
 *
 * Measured on a live HK boot: 97 % of hot-cache misses resolve to NULL — 166 M lookups per run for guest
 * addresses NO region contains, ~25 % of all lookups, each paying the full 16-slot scan AND a failed treap
 * walk. A bigger positive cache cannot touch them (slots=64 was worth 0.3 points), because the answer being
 * cached is the absence of a region.
 *
 * A failed treap walk already computes the exact answer for free: the last node we went LEFT at is the
 * successor (smallest base > addr) and the last we went RIGHT at is the predecessor, so [lo,hi) is a genuine
 * region-free gap — regions are non-overlapping and the treap is keyed by base. One entry therefore covers a
 * whole gap rather than one address, which is why 8 slots suffice for a stream spread over ~32 buckets.
 *
 * Soundness rests on two things: the epoch (bumped now on add as well as on remove/split/rebuild), and
 * skipping this entirely when guest32 is active, since that path's NULL means "found but not guest32", which
 * is a different statement. */
#define HB_GAP_SLOTS 8
typedef struct { hb_gva_t lo, hi; } hb_gap_ent_t;
static __thread struct {
    hb_memory_t* mem;
    uint64_t epoch;
    int count;
    hb_gap_ent_t e[HB_GAP_SLOTS];
} g_gap_tls;
/* Default OFF. The first A/B (both arms sampled at guest+164 s, same build) measured the negative cache
 * WORSE: find_region* self 4.05 % vs 1.96 %, dispatches at 300 s 532 M vs 560 M — despite it provably
 * skipping 99.4 % of a 17.2-node walk. The suspected cause is now addressed (duplicate acquire loads on a
 * shared line, plus adds wiping the positive cache), but the default stays where the evidence is until a
 * fresh A/B says otherwise. */
static int verify_neg_cache_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_VERIFY_NEG_CACHE");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static int neg_cache_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_NEG_CACHE");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static bool gap_lookup(hb_memory_t* mem, hb_gva_t addr, uint64_t epoch) {
    if (!neg_cache_enabled() || mem->guest32_base) return false;
    if (g_gap_tls.mem != mem || g_gap_tls.epoch != epoch) {
        g_gap_tls.mem = mem;
        g_gap_tls.epoch = epoch;
        g_gap_tls.count = 0;
        t_gap_flushes++;
        return false;
    }
    for (int i = 0; i < g_gap_tls.count; i++) {
        t_gap_scanned++;
        if (addr < g_gap_tls.e[i].lo || addr >= g_gap_tls.e[i].hi) continue;
        if (i) {
            hb_gap_ent_t tmp = g_gap_tls.e[i];
            g_gap_tls.e[i] = g_gap_tls.e[0];
            g_gap_tls.e[0] = tmp;
        }
        t_gap_hits++;
        return true;
    }
    return false;
}

static void gap_insert(hb_memory_t* mem, hb_gva_t lo, hb_gva_t hi, uint64_t epoch) {
    if (!neg_cache_enabled() || mem->guest32_base || hi <= lo) return;
    if (g_gap_tls.mem != mem || g_gap_tls.epoch != epoch) {
        g_gap_tls.mem = mem;
        g_gap_tls.epoch = epoch;
        g_gap_tls.count = 0;
    }
    for (int i = g_gap_tls.count < HB_GAP_SLOTS ? g_gap_tls.count : HB_GAP_SLOTS - 1; i > 0; i--)
        g_gap_tls.e[i] = g_gap_tls.e[i - 1];
    g_gap_tls.e[0].lo = lo;
    g_gap_tls.e[0].hi = hi;
    if (g_gap_tls.count < HB_GAP_SLOTS) g_gap_tls.count++;
    t_gap_fills++;
}

static hb_region_t* hot_cache_lookup(hb_memory_t* mem, hb_gva_t addr) {
    int did_reset = 0;
    if (macrunner_hb_disable_hot_cache_enabled()) return NULL;
    uint64_t epoch = hot_cache_epoch(mem);
    if (g_hot_cache_tls.mem != mem || g_hot_cache_tls.epoch != epoch) {
        hot_cache_reset_tls(mem, epoch);
        did_reset = 1;
    }

    for (int i = 0; i < HB_MEMORY_HOT_CACHE_SLOTS; i++) {
        hb_region_t* c = g_hot_cache_tls.slot[i];
        if (c && addr >= c->base && addr < c->base + c->size) {
            if (i != 0) {
                g_hot_cache_tls.slot[i] = g_hot_cache_tls.slot[0];
                g_hot_cache_tls.slot[0] = c;
            }
            hot_cache_note(i, did_reset, c);
            return c;
        }
    }
    hot_cache_note(-1, did_reset, NULL);
    return NULL;
}

static void hot_cache_insert(hb_memory_t* mem, hb_region_t* region) {
    if (!region || macrunner_hb_disable_hot_cache_enabled()) return;
    uint64_t epoch = hot_cache_epoch(mem);
    if (g_hot_cache_tls.mem != mem || g_hot_cache_tls.epoch != epoch)
        hot_cache_reset_tls(mem, epoch);

    for (int i = 0; i < HB_MEMORY_HOT_CACHE_SLOTS; i++) {
        if (g_hot_cache_tls.slot[i] == region) {
            if (i != 0) {
                g_hot_cache_tls.slot[i] = g_hot_cache_tls.slot[0];
                g_hot_cache_tls.slot[0] = region;
            }
            return;
        }
    }

    for (int i = HB_MEMORY_HOT_CACHE_SLOTS - 1; i > 0; i--)
        g_hot_cache_tls.slot[i] = g_hot_cache_tls.slot[i - 1];
    g_hot_cache_tls.slot[0] = region;
}


static size_t page_align(size_t sz) {
    return (sz + 4095) & ~4095;
}

static size_t hb_host_page_size(void) {
    static size_t cached = 0;
    if (!cached) {
        long v = sysconf(_SC_PAGESIZE);
        cached = v > 0 ? (size_t)v : 4096;
    }
    return cached;
}

static uintptr_t host_page_floor(uintptr_t addr) {
    size_t page = hb_host_page_size();
    return addr & ~(uintptr_t)(page - 1);
}

static uintptr_t host_page_ceil(uintptr_t addr) {
    size_t page = hb_host_page_size();
    return (addr + page - 1) & ~(uintptr_t)(page - 1);
}

static hb_gva_t page_floor_gva(hb_gva_t addr) {
    return addr & ~(hb_gva_t)4095;
}

static hb_gva_t page_ceil_gva(hb_gva_t addr) {
    return (addr + 4095) & ~(hb_gva_t)4095;
}

static bool range_overflows(hb_gva_t base, size_t size) {
    return size && base + (hb_gva_t)size <= base;
}

static bool trace_guest32_alias_enabled(void) {
    return macrunner_hb_memory_environment.trace_guest32_alias;
}

static void trace_guest32_alias(const char* reason, const hb_memory_t* mem,
                                uintptr_t original, uintptr_t base,
                                uintptr_t normalized, size_t size) {
    static int budget = 128;
    int left;

    if (!trace_guest32_alias_enabled()) return;
    if (original < 0x100000000ULL && (original & 0xffffffffULL) != 0x0050006fULL) return;
    left = __atomic_fetch_sub(&budget, 1, __ATOMIC_RELAXED);
    if (left <= 0) return;
    fprintf(stderr,
            "macrunner-hb-guest32-alias: reason=%s mem=%p guest32_base=%p "
            "original=0x%llx normalized=0x%llx size=%zu window=[0x%llx,0x%llx)\n",
            reason, (const void*)mem, mem ? mem->guest32_base : NULL,
            (unsigned long long)original, (unsigned long long)normalized, size,
            (unsigned long long)base, (unsigned long long)(base + HB_GUEST32_SIZE));
}

static bool normalize_guest32_mirror_addr(hb_memory_t* mem, hb_gva_t* addr, size_t size) {
    uintptr_t base, value, original;

    if (!mem || !addr) return true;
    value = (uintptr_t)*addr;
    if (!mem->guest32_base) {
        trace_guest32_alias("no_guest32_base", mem, value, 0, value, size);
        return true;
    }
    base = (uintptr_t)mem->guest32_base;
    original = value;
    if (value < base || value >= base + HB_GUEST32_SIZE) {
        trace_guest32_alias("out_of_window", mem, original, base, original, size);
        return true;
    }

    value -= base;
    if (size && value + size > HB_GUEST32_SIZE) {
        trace_guest32_alias("overflow", mem, original, base, value, size);
        return false;
    }
    trace_guest32_alias("normalized", mem, original, base, value, size);
    *addr = (hb_gva_t)value;
    return true;
}

static int prot_from_perm(hb_perm_t perm, bool guest32) {
    int prot = PROT_NONE;
    if (perm & HB_PERM_READ) prot |= PROT_READ;
    if (perm & HB_PERM_WRITE) prot |= PROT_WRITE;
    if (perm & HB_PERM_EXEC) {
        prot |= guest32 ? PROT_READ : PROT_EXEC;
    }
    return prot;
}

static void* region_host_ptr(const hb_region_t* r, hb_gva_t addr) {
    return (uint8_t*)r->host_base + (addr - r->base);
}

#ifdef __APPLE__
static bool guest32_copy_needs_mach(const hb_region_t* r, hb_gva_t addr, size_t size) {
    uintptr_t host;

    if (!r || !r->is_guest32 || !r->host_base || !size) return false;
    host = (uintptr_t)region_host_ptr(r, addr);

    /* x86 code can freely perform unaligned multi-byte accesses.  On macOS,
     * optimized host copies from the guest32 mirror can surface those as
     * native SIGBUS instead of a controlled HyperBridge memory result. */
    if (size > 1 && (host & ((size < sizeof(uint64_t) ? size : sizeof(uint64_t)) - 1)))
        return true;

    return host_page_floor(host) != host_page_floor(host + size - 1);
}

static hb_result_t mach_copy_from_host(void* out, const void* host, size_t size) {
    mach_vm_size_t copied = 0;
    kern_return_t kr;

    kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)host,
                                (mach_vm_size_t)size,
                                (mach_vm_address_t)(uintptr_t)out, &copied);
    return (kr == KERN_SUCCESS && copied == size) ? HB_OK : HB_ERR_MEMORY_FAULT;
}

static hb_result_t mach_copy_to_host(void* host, const void* in, size_t size) {
    kern_return_t kr;

    kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)(uintptr_t)host,
                       (vm_offset_t)(uintptr_t)in, (mach_msg_type_number_t)size);
    return kr == KERN_SUCCESS ? HB_OK : HB_ERR_MEMORY_FAULT;
}

static bool guest32_direct_copy_safe(const hb_region_t* r, hb_gva_t addr, size_t size) {
    return r && r->is_guest32 && r->host_base && !guest32_copy_needs_mach(r, addr, size);
}

static hb_result_t guest32_sync_host_protection(const hb_region_t* r, hb_gva_t addr, size_t size) {
    uintptr_t host;
    uintptr_t start;
    uintptr_t end;

    if (!r || !r->is_guest32 || !r->host_base || !size) return HB_ERR_INVALID_ARG;
    host = (uintptr_t)region_host_ptr(r, addr);
    start = host_page_floor(host);
    end = host_page_ceil(host + size);
    if (mprotect((void*)start, end - start, prot_from_perm(r->perm, true)) != 0)
        return HB_ERR_MEMORY_FAULT;
    return HB_OK;
}
#endif

static uint32_t region_prio(hb_gva_t base) {
    uint64_t x = (uint64_t)base;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static void tree_insert(hb_region_t** root, hb_region_t* r) {
    if (!*root) {
        *root = r;
        return;
    }

    if (r->base < (*root)->base) {
        tree_insert(&(*root)->tree_left, r);
        if ((*root)->tree_left && (*root)->tree_left->tree_prio < (*root)->tree_prio) {
            hb_region_t* n = (*root)->tree_left;
            (*root)->tree_left = n->tree_right;
            n->tree_right = *root;
            *root = n;
        }
    } else {
        tree_insert(&(*root)->tree_right, r);
        if ((*root)->tree_right && (*root)->tree_right->tree_prio < (*root)->tree_prio) {
            hb_region_t* n = (*root)->tree_right;
            (*root)->tree_right = n->tree_left;
            n->tree_left = *root;
            *root = n;
        }
    }
}

static void clear_hot_cache(hb_memory_t* mem) {
    if (!mem) return;
    memset(mem->hot, 0, sizeof(mem->hot));
    __atomic_add_fetch(&mem->hot_gen, 1, __ATOMIC_RELEASE);
    /* MacRunner 2026-07-31 — invalidate the NEGATIVE cache here too, conservatively.
     *
     * Splitting the epochs was right in principle (an add cannot invalidate a cached region) but it assumed
     * every add goes through insert_region_head. It does not: the VM-map sync path appends straight to
     * mem->regions and calls rebuild_region_tree, bumping only hot_gen. With the negative cache keyed solely on
     * add_gen, those regions materialised INSIDE cached gaps with nothing to invalidate them — 2118 verified
     * violations on a live boot (267 distinct addresses, all adjacent 64 KB commits), zero in the
     * single-threaded unit suite, which is why it took the real workload to expose it.
     *
     * Removes and splits do not strictly need to invalidate a gap, so this over-invalidates slightly; that is
     * the correct trade against a stale gap reporting mapped memory as unmapped. Plain adds still bump only
     * add_gen, so the positive cache keeps the hit rate the split bought it. */
    __atomic_add_fetch(&mem->add_gen, 1, __ATOMIC_RELEASE);
}

static void rebuild_region_tree(hb_memory_t* mem) {
    if (!mem) return;
    __atomic_add_fetch(&mm_rebuilds, 1, __ATOMIC_RELAXED);
    mem->region_tree = NULL;
    for (hb_region_t* r = mem->regions; r; r = r->next) {
        __atomic_add_fetch(&mm_rebuild_nodes, 1, __ATOMIC_RELAXED);
        r->tree_left = NULL;
        r->tree_right = NULL;
        r->tree_prio = region_prio(r->base);
        tree_insert(&mem->region_tree, r);
    }
    clear_hot_cache(mem);
}

static void bump_generation(hb_memory_t* mem, hb_region_t* r) {
    if (!mem) return;
    mem->generation++;
    if (r) r->gen = mem->generation;
}

static void insert_region_head(hb_memory_t* mem, hb_region_t* r) {
    r->next = mem->regions;
    mem->regions = r;
    r->tree_left = NULL;
    r->tree_right = NULL;
    r->tree_prio = region_prio(r->base);
    tree_insert(&mem->region_tree, r);
    /* Bump ONLY the negative-cache epoch: a gap entry must not outlive the gap, but the positive cache is
     * provably unaffected by an add, so wiping it here (as the first cut did) was pure loss — it cost every
     * thread its 16 slots and wrote a cache line 54 threads read on every lookup. */
    __atomic_add_fetch(&mem->add_gen, 1, __ATOMIC_RELEASE);
}

static bool range_overlaps(hb_gva_t a_base, size_t a_size, hb_gva_t b_base, size_t b_size) {
    hb_gva_t a_top = a_base + (hb_gva_t)a_size;
    hb_gva_t b_top = b_base + (hb_gva_t)b_size;
    return a_base < b_top && b_base < a_top;
}

/* The last unmeasured O(N) walk on a hot-ish path.  hb_memory_protect's walk is
 * now measured (40 % of protect calls, ~9 500 nodes each) and sync_live_range is
 * measured and cold (6 calls in 66 s).  This one guards both map paths, and with
 * `regions` reaching 16 000+ inside the first minute of an HK boot it is the
 * obvious next suspect -- so count it rather than argue about it. */
static bool any_overlap(hb_memory_t* mem, hb_gva_t base, size_t size) {
    unsigned long long scan = 0;
    bool hit = false;

    for (hb_region_t* r = mem ? mem->regions : NULL; r; r = r->next) {
        scan++;
        if (range_overlaps(base, size, r->base, r->size)) { hit = true; break; }
    }
    __atomic_add_fetch(&mm_ovl_calls, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&mm_ovl_scan, scan, __ATOMIC_RELAXED);
    return hit;
}

static hb_result_t split_region_at(hb_memory_t* mem, hb_gva_t addr) {
    hb_region_t* r;
    hb_region_t* n;
    hb_gva_t top;

    if (!mem) return HB_ERR_INVALID_ARG;
    r = hb_memory_find_region(mem, addr);
    if (!r) return HB_OK;
    top = r->base + r->size;
    if (addr == r->base || addr >= top) return HB_OK;
    n = calloc(1, sizeof(*n));
    if (!n) return HB_ERR_OUT_OF_MEMORY;
    /* Every split adds a region permanently: nothing in this file ever merges two
     * adjacent regions back together, so N is monotonically non-decreasing. */
    __atomic_add_fetch(&mm_splits, 1, __ATOMIC_RELAXED);

    *n = *r;
    n->base = addr;
    n->size = (size_t)(top - addr);
    if (r->host_base) n->host_base = region_host_ptr(r, addr);
    n->tree_left = NULL;
    n->tree_right = NULL;
    n->tree_prio = region_prio(n->base);
    n->next = r->next;

    r->size = (size_t)(addr - r->base);
    r->next = n;
    tree_insert(&mem->region_tree, n);
    clear_hot_cache(mem);
    return HB_OK;
}

static hb_result_t split_all_regions_at(hb_memory_t* mem, hb_gva_t addr) {
    hb_region_t* r;
    hb_gva_t top;

    if (!mem) return HB_ERR_INVALID_ARG;
    r = hb_memory_find_region(mem, addr);
    if (!r) return HB_OK;
    top = r->base + r->size;
    if (addr <= r->base || addr >= top) return HB_OK;
    return split_region_at(mem, addr);
}

static hb_result_t remove_region_node(hb_memory_t* mem, hb_region_t* target) {
    hb_region_t** p;

    if (!mem || !target) return HB_ERR_INVALID_ARG;
    p = &mem->regions;
    while (*p) {
        if (*p == target) {
            *p = target->next;
            mem->total_size -= target->size;
            clear_hot_cache(mem);
            free(target);
            rebuild_region_tree(mem);
            return HB_OK;
        }
        p = &(*p)->next;
    }
    return HB_ERR_NOT_FOUND;
}

static bool guest32_range_fully_mapped(hb_memory_t* mem, hb_gva_t start, hb_gva_t top) {
    hb_gva_t cur = start;

    while (cur < top) {
        hb_region_t* r = hb_memory_find_region(mem, cur);
        if (!r || !r->is_guest32 || r->base > cur) return false;
        cur = r->base + r->size;
    }
    return true;
}

static bool trace_bad_native_write_enabled(void) {
    return macrunner_hb_memory_environment.trace_native_writes;
}



#ifdef __APPLE__
static bool trace_live_vm_access_fail_enabled(void) {
    return macrunner_hb_memory_environment.trace_live_vm_access_fail;
}

static void trace_live_vm_access_fail(const char* op, hb_gva_t addr, size_t size,
                                      kern_return_t access_kr, const hb_region_t* cached) {
    mach_vm_address_t region = (mach_vm_address_t)addr;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t region_kr;

    if (!trace_live_vm_access_fail_enabled()) return;

    memset(&info, 0, sizeof(info));
    region_kr = mach_vm_region(mach_task_self(), &region, &region_size,
                               VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                               &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    fprintf(stderr,
            "macrunner-hb-live-vm-access-fail: op=%s addr=0x%llx size=%zu access_kr=%d "
            "mach_kr=%d mach_region=0x%llx mach_end=0x%llx mach_prot=0x%x "
            "cached_base=0x%llx cached_end=0x%llx cached_perm=0x%x\n",
            op, (unsigned long long)addr, size, access_kr, region_kr,
            (unsigned long long)region, (unsigned long long)(region + region_size),
            info.protection,
            cached ? (unsigned long long)cached->base : 0,
            cached ? (unsigned long long)(cached->base + cached->size) : 0,
            cached ? cached->perm : 0);
}

static hb_result_t write_live_vm_region(hb_memory_t* mem, hb_gva_t addr, const void* in, size_t size,
                                        const hb_region_t* region) {
    mach_port_t task = mach_task_self();
    kern_return_t kr = mach_vm_write(task, (mach_vm_address_t)addr,
                                     (vm_offset_t)(uintptr_t)in,
                                     (mach_msg_type_number_t)size);

    if (kr == KERN_SUCCESS) return HB_OK;
    trace_live_vm_access_fail("write", addr, size, kr, region);

    /* Windows guard-page / stack growth: the page is reserved in Wine's view
     * but not yet committed by macOS, so mach_vm_write faults.  Let the embedder
     * run virtual_handle_fault to commit the page (and grow the guest stack),
     * then retry once. */
    if (mem && mem->special_grow && mem->special_grow(mem->special_user, addr)) {
        kr = mach_vm_write(task, (mach_vm_address_t)addr, (vm_offset_t)(uintptr_t)in,
                           (mach_msg_type_number_t)size);
        if (kr == KERN_SUCCESS) return HB_OK;
        trace_live_vm_access_fail("write-after-grow", addr, size, kr, region);
    }

    if (region && (region->perm & HB_PERM_WRITE)) {
        mach_vm_address_t protect_base = (mach_vm_address_t)host_page_floor((uintptr_t)addr);
        mach_vm_address_t protect_top = (mach_vm_address_t)host_page_ceil((uintptr_t)addr + size);
        mach_vm_size_t protect_size = protect_top - protect_base;
        mach_vm_address_t query_addr = (mach_vm_address_t)addr;
        mach_vm_size_t query_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        vm_prot_t restore_prot = VM_PROT_READ;
        kern_return_t query_kr;

        memset(&info, 0, sizeof(info));
        query_kr = mach_vm_region(task, &query_addr, &query_size, VM_REGION_BASIC_INFO_64,
                                  (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(task, object);
        if (query_kr == KERN_SUCCESS && query_addr <= addr &&
            query_addr + query_size >= addr + size)
            restore_prot = info.protection;

        kr = mach_vm_protect(task, protect_base, protect_size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
        if (kr == KERN_SUCCESS) {
            kr = mach_vm_write(task, (mach_vm_address_t)addr, (vm_offset_t)(uintptr_t)in,
                               (mach_msg_type_number_t)size);
            if (restore_prot)
                (void)mach_vm_protect(task, protect_base, protect_size, FALSE, restore_prot);
            if (kr == KERN_SUCCESS) return HB_OK;
            trace_live_vm_access_fail("write-after-protect", addr, size, kr, region);
        } else {
            trace_live_vm_access_fail("write-protect", addr, size, kr, region);
        }
    }
    return HB_ERR_MEMORY_FAULT;
}
#endif

static bool live_vm_write_mach_enabled(void) {
    return macrunner_hb_memory_environment.live_vm_write_mach;
}

static void trace_bad_native_write(const char* path, hb_gva_t addr, const void* in, size_t size) {
    uint64_t sample = 0;
    size_t copy = size < sizeof(sample) ? size : sizeof(sample);

    if (!trace_bad_native_write_enabled()) return;
    if (addr < 0x7ffd0000000ULL || addr >= 0x7ffe0000000ULL) return;
    if (copy) memcpy(&sample, in, copy);
    fprintf(stderr,
            "macrunner-hb-native-write: path=%s addr=0x%llx size=%zu sample=0x%llx\n",
            path, (unsigned long long)addr, size, (unsigned long long)sample);
}

static bool trace_guest_write_match(hb_gva_t addr, size_t size) {
    unsigned long long start, stop;
    hb_gva_t top;

    start = __atomic_load_n(&macrunner_hb_memory_environment.trace_guest_write_start,
                            __ATOMIC_RELAXED);
    stop  = __atomic_load_n(&macrunner_hb_memory_environment.trace_guest_write_stop,
                            __ATOMIC_RELAXED);
    if (start == stop) return false;
    top = addr + size;
    if (top < addr) top = UINT64_MAX;
    return addr < (hb_gva_t)stop && top > (hb_gva_t)start;
}

static void trace_guest_write(const char* path, hb_gva_t addr, const void* in, size_t size) {
    uint64_t sample = 0;
    size_t copy = size < sizeof(sample) ? size : sizeof(sample);

    if (!trace_guest_write_match(addr, size)) return;
    if (copy) memcpy(&sample, in, copy);
    fprintf(stderr,
            "macrunner-hb-guest-write: path=%s addr=0x%llx size=%zu sample=0x%llx\n",
            path, (unsigned long long)addr, size, (unsigned long long)sample);
}

static hb_region_t* find_region_normalized(hb_memory_t* mem, hb_gva_t addr);
static hb_region_t* find_region_after_hot_miss(hb_memory_t* mem, hb_gva_t addr);
static bool check_perm_region(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p, hb_region_t** out);

/* MacRunner 2026-07-31 — every instance starts at a globally unique epoch.
 *
 * The per-thread caches key their validity on (mem pointer, hot_gen). calloc gives hot_gen = 0, so a destroyed
 * hb_memory_t whose address malloc later hands back produces a NEW instance that a stale TLS table matches
 * exactly — classic ABA. For the positive cache that means serving pointers to regions freed with the previous
 * instance; it is why hb_test_runner:19370 fails the moment anything else is cached on the same key. Seeding
 * from a global counter makes the pair unique for the process, so no reused address can alias.
 *
 * The stride keeps instances apart even after per-instance bumps: an instance would need 2^32 region mutations
 * to reach its successor's seed. */
static uint64_t g_memory_epoch_seq;

hb_memory_t* hb_memory_create(size_t max_size) {
    hb_memory_t* mem = calloc(1, sizeof(hb_memory_t));
    if (!mem) return NULL;
    hb_memory_init_environment();
    __atomic_add_fetch(&mm_maps, 1, __ATOMIC_RELAXED);
    mem->max_size = max_size;
    mem->hot_gen = __atomic_add_fetch(&g_memory_epoch_seq, 1, __ATOMIC_RELAXED) << 32;
    mem->add_gen = mem->hot_gen;   /* same ABA protection for the negative cache */
    return mem;
}

void hb_memory_destroy(hb_memory_t* mem) {
    if (!mem) return;
    hb_region_t* r = mem->regions;
    while (r) {
        hb_region_t* n = r->next;
        if (r->allocated) munmap(r->host_base, r->size);
        free(r);
        r = n;
    }
    if (mem->guest32_base && mem->guest32_owned) munmap(mem->guest32_base, mem->guest32_size);
    free(mem);
}

hb_result_t hb_memory_map(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    if (!mem) return HB_ERR_INVALID_ARG;
    if (!size || range_overflows(base, page_align(size))) return HB_ERR_INVALID_ARG;
    size_t alloc_size = page_align(size);
    hb_region_t* r = calloc(1, sizeof(hb_region_t));
    if (!r) return HB_ERR_OUT_OF_MEMORY;
    if (base == 0) {
        int prot = prot_from_perm(perm, false);
        void* p = mmap(NULL, alloc_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { free(r); return HB_ERR_OUT_OF_MEMORY; }
        r->base = (hb_gva_t)(uintptr_t)p;
        r->host_base = p;
        r->allocated = true;
    } else {
        r->base = base;
        r->host_base = NULL;
        r->allocated = false;
    }
    r->size = alloc_size;
    r->perm = perm;
    r->gen = mem->generation;
    insert_region_head(mem, r);
    mem->total_size += alloc_size;
    return HB_OK;
}

hb_result_t hb_memory_map_private(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    if (!mem || !base) return HB_ERR_INVALID_ARG;
    size_t alloc_size = page_align(size);
    if (!alloc_size || range_overflows(base, alloc_size)) return HB_ERR_INVALID_ARG;
    if (any_overlap(mem, base, alloc_size)) return HB_ERR_INVALID_ARG;
    hb_region_t* r = calloc(1, sizeof(hb_region_t));
    if (!r) return HB_ERR_OUT_OF_MEMORY;

    int prot = prot_from_perm(perm, false);

    void* p = mmap(NULL, alloc_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { free(r); return HB_ERR_OUT_OF_MEMORY; }

    r->base = base;
    r->host_base = p;
    r->allocated = true;
    r->size = alloc_size;
    r->perm = perm;
    r->gen = mem->generation;
    insert_region_head(mem, r);
    mem->total_size += alloc_size;
    return HB_OK;
}

static hb_result_t hb_memory_sync_live_range_inner(hb_memory_t* mem, hb_gva_t base, size_t size,
                                                   hb_perm_t perm) {
    hb_region_t* replacement;
    hb_region_t** link;
    hb_gva_t top;
    hb_result_t res;
    unsigned long long scan = 0; /* folded into mm_slr_scan once, see protect_range_t */

    if (!mem || !size || range_overflows(base, size)) return HB_ERR_INVALID_ARG;
    top = base + (hb_gva_t)size;

    /* Replay is frequent.  Once the authoritative live region has replaced
     * the old VM-map fragments, avoid another O(N) split/remove/tree rebuild. */
    for (hb_region_t* exact = mem->regions; exact; exact = exact->next) {
        bool other_overlap = false;

        scan++;
        if (exact->allocated || exact->is_guest32 || exact->base != base ||
            exact->size != size || exact->perm != perm)
            continue;
        for (hb_region_t* other = mem->regions; other; other = other->next) {
            scan++;
            if (other != exact && range_overlaps(base, size, other->base, other->size)) {
                other_overlap = true;
                break;
            }
        }
        if (!other_overlap) {
            __atomic_add_fetch(&mm_slr_scan, scan, __ATOMIC_RELAXED);
            __atomic_add_fetch(&mm_slr_fast, 1, __ATOMIC_RELAXED);
            return HB_OK;
        }
    }

    /* This API is deliberately limited to Wine/macOS-owned live mappings.
     * Guest32 and private HB allocations have real backing/protection semantics
     * and must continue through their dedicated map/protect paths. */
    for (hb_region_t* r = mem->regions; r; r = r->next) {
        scan++;
        if (range_overlaps(base, size, r->base, r->size) &&
            (r->allocated || r->is_guest32)) {
            __atomic_add_fetch(&mm_slr_scan, scan, __ATOMIC_RELAXED);
            return HB_ERR_INVALID_ARG;
        }
    }
    __atomic_add_fetch(&mm_slr_scan, scan, __ATOMIC_RELAXED);

    __atomic_add_fetch(&mm_slr_repl, 1, __ATOMIC_RELAXED);
    replacement = calloc(1, sizeof(*replacement));
    if (!replacement) return HB_ERR_OUT_OF_MEMORY;

    res = split_all_regions_at(mem, base);
    if (res != HB_OK) {
        free(replacement);
        rebuild_region_tree(mem);
        return res;
    }
    res = split_all_regions_at(mem, top);
    if (res != HB_OK) {
        free(replacement);
        rebuild_region_tree(mem);
        return res;
    }

    /* Drop every old live fragment inside the authoritative range.  This also
     * removes holes/fragmentation from VM-map snapshots: the replacement below
     * guarantees that every byte of the guest allocation resolves to one HB
     * region with the requested permission. */
    link = &mem->regions;
    while (*link) {
        hb_region_t* r = *link;
        if (!r->allocated && !r->is_guest32 &&
            r->base >= base && r->base < top) {
            *link = r->next;
            mem->total_size -= r->size;
            clear_hot_cache(mem);
            free(r);
            continue;
        }
        link = &r->next;
    }

    replacement->base = base;
    replacement->size = size;
    replacement->perm = perm;
    replacement->allocated = false;
    replacement->host_base = NULL;
    replacement->next = mem->regions;
    mem->regions = replacement;
    mem->total_size += size;
    bump_generation(mem, replacement);
    rebuild_region_tree(mem);
    return HB_OK;
}

hb_result_t hb_memory_sync_live_range(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    unsigned long long n = __atomic_add_fetch(&mm_slr_calls, 1, __ATOMIC_RELAXED);
    bool timed = (n & MM_CLOCK_MASK) == 0;
    unsigned long long t0 = timed ? mm_now_ns() : 0;
    hb_result_t r = hb_memory_sync_live_range_inner(mem, base, size, perm);

    if (timed) {
        unsigned long long t1 = mm_now_ns();
        __atomic_add_fetch(&mm_slr_ns, t1 - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&mm_slr_ns_n, 1, __ATOMIC_RELAXED);
        if (mm_due(t1)) mm_report(mem, "synclive");
    }
    return r;
}

hb_result_t hb_memory_guest32_reserve(hb_memory_t* mem) {
    const size_t reserve_size = (size_t)(HB_GUEST32_SIZE * 2ULL);
    uintptr_t raw_base;
    uintptr_t aligned;
    size_t prefix;
    size_t suffix;
    void* raw;

    if (!mem) return HB_ERR_INVALID_ARG;
    if (mem->guest32_base) return HB_OK;

    raw = mmap(NULL, reserve_size, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) return HB_ERR_OUT_OF_MEMORY;

    raw_base = (uintptr_t)raw;
    aligned = (raw_base + HB_GUEST32_SIZE - 1) & ~(uintptr_t)(HB_GUEST32_SIZE - 1);
    prefix = aligned - raw_base;
    suffix = reserve_size - (size_t)HB_GUEST32_SIZE - prefix;

    if (prefix) munmap(raw, prefix);
    if (suffix) munmap((void*)(aligned + HB_GUEST32_SIZE), suffix);

    mem->guest32_base = (void*)aligned;
    mem->guest32_size = (size_t)HB_GUEST32_SIZE;
    mem->guest32_owned = true;
    return HB_OK;
}

void* hb_memory_guest32_base(const hb_memory_t* mem) {
    return mem ? mem->guest32_base : NULL;
}

void* hb_memory_guest32_to_host(hb_memory_t* mem, uint32_t guest_addr) {
    hb_region_t* r;

    if (!mem || !mem->guest32_base) return NULL;
    r = hb_memory_find_region(mem, guest_addr);
    if (!r || !r->is_guest32 || !r->host_base) return NULL;
    return region_host_ptr(r, guest_addr);
}

hb_result_t hb_memory_guest32_map(hb_memory_t* mem, uint32_t base, size_t size, hb_perm_t perm) {
    size_t alloc_size;
    void* host;
    hb_region_t* r;
    uintptr_t host_start;
    uintptr_t host_top;

    if (!mem || !size) return HB_ERR_INVALID_ARG;
    if ((base & 4095u) || (size & 4095u)) return HB_ERR_INVALID_ARG;
    alloc_size = page_align(size);
    if ((uint64_t)base + alloc_size > HB_GUEST32_SIZE) return HB_ERR_INVALID_ARG;
    if (any_overlap(mem, base, alloc_size)) return HB_ERR_INVALID_ARG;
    if (hb_memory_guest32_reserve(mem) != HB_OK) return HB_ERR_OUT_OF_MEMORY;

    host = (uint8_t*)mem->guest32_base + base;
    host_start = host_page_floor((uintptr_t)host);
    host_top = host_page_ceil((uintptr_t)host + alloc_size);
    if (mprotect((void*)host_start, host_top - host_start, PROT_READ | PROT_WRITE) != 0) {
        return HB_ERR_MEMORY_FAULT;
    }

    r = calloc(1, sizeof(*r));
    if (!r) return HB_ERR_OUT_OF_MEMORY;

    r->base = base;
    r->host_base = host;
    r->size = alloc_size;
    r->perm = perm;
    r->is_guest32 = true;
    r->allocated = false;
    r->gen = mem->generation;
    if (perm & HB_PERM_EXEC) bump_generation(mem, r);
    insert_region_head(mem, r);
    mem->total_size += alloc_size;
    return HB_OK;
}

hb_result_t hb_memory_guest32_protect(hb_memory_t* mem, uint32_t base, size_t size, hb_perm_t perm) {
    hb_gva_t start;
    hb_gva_t top;
    hb_result_t res;
    bool touched_exec = false;

    if (!mem || !size) return HB_ERR_INVALID_ARG;

#ifdef __APPLE__
    /* MacRunner sentinel trace: log every call that touches the sentinel page [0x7BD8E000, 0x7BD8F000). */
    if ((hb_gva_t)base <= 0x7BD8E000u && (hb_gva_t)base + (hb_gva_t)size > 0x7BD8E000u) {
        fprintf(stderr, "macrunner-hb-sentinel-protect: caller base=%08x size=%zx perm=%d\n",
                base, size, (int)perm);
        fflush(stderr);
    }
#endif
    start = page_floor_gva(base);
    top = page_ceil_gva((hb_gva_t)base + (hb_gva_t)size);
    if (top > HB_GUEST32_SIZE || top <= start) return HB_ERR_INVALID_ARG;

    res = split_region_at(mem, start);
    if (res != HB_OK) return res;
    res = split_region_at(mem, top);
    if (res != HB_OK) return res;
    if (!guest32_range_fully_mapped(mem, start, top)) return HB_ERR_NOT_FOUND;

    for (hb_region_t* r = mem->regions; r; r = r->next) {
        if (!r->is_guest32 || r->base < start || r->base >= top) continue;
        if ((r->perm | perm) & HB_PERM_EXEC) touched_exec = true;
        r->perm = perm;
#ifdef __APPLE__
        if (r->host_base && guest32_sync_host_protection(r, r->base, r->size) != HB_OK)
            return HB_ERR_MEMORY_FAULT;
#endif
    }
    if (touched_exec) bump_generation(mem, NULL);
    return HB_OK;
}

hb_result_t hb_memory_guest32_unmap(hb_memory_t* mem, uint32_t base, size_t size) {
    hb_gva_t start;
    hb_gva_t top;
    hb_result_t res;
    bool touched_exec = false;
    hb_region_t* r;

    if (!mem || !size) return HB_ERR_INVALID_ARG;
    start = page_floor_gva(base);
    top = page_ceil_gva((hb_gva_t)base + (hb_gva_t)size);
    if (top > HB_GUEST32_SIZE || top <= start) return HB_ERR_INVALID_ARG;

    res = split_region_at(mem, start);
    if (res != HB_OK) return res;
    res = split_region_at(mem, top);
    if (res != HB_OK) return res;
    if (!guest32_range_fully_mapped(mem, start, top)) return HB_ERR_NOT_FOUND;

    r = mem->regions;
    while (r) {
        hb_region_t* next = r->next;
        if (r->is_guest32 && r->base >= start && r->base < top) {
            if (r->perm & HB_PERM_EXEC) touched_exec = true;
            remove_region_node(mem, r);
        }
        r = next;
    }
    if (touched_exec) bump_generation(mem, NULL);
    rebuild_region_tree(mem);
    return HB_OK;
}

uint64_t hb_memory_generation(const hb_memory_t* mem) {
    return mem ? mem->generation : 0;
}

uint64_t hb_memory_region_generation(hb_memory_t* mem, hb_gva_t addr) {
    hb_region_t* r = hb_memory_find_region(mem, addr);
    return r ? r->gen : 0;
}

hb_result_t hb_memory_unmap(hb_memory_t* mem, hb_gva_t base) {
    if (!mem) return HB_ERR_INVALID_ARG;
    hb_region_t** p = &mem->regions;
    while (*p) {
        if ((*p)->base == base) {
            hb_region_t* d = *p;
            *p = d->next;
            mem->total_size -= d->size;
            if (d->allocated) munmap(d->host_base, d->size);
            if (d->is_guest32 && (d->perm & HB_PERM_EXEC)) bump_generation(mem, NULL);
            clear_hot_cache(mem);
            free(d);
            rebuild_region_tree(mem);
            return HB_OK;
        }
        p = &(*p)->next;
    }
    return HB_ERR_NOT_FOUND;
}

/*
 * Applying a protection change to every region inside [start, top).
 *
 * This used to walk mem->regions end to end.  A `sample` of a live Hollow Knight
 * run in its slow regime (2026-07-29) put 913 of 2795 samples on the critical-path
 * thread inside hb_memory_protect, in a call-free loop -- 33 % of that thread, and
 * more than every guest memory read, write and region lookup in the whole process
 * put together (164 + 158 + 102), despite VirtualProtect being far rarer than
 * memory access.  That ratio only happens if the list is long, so the walk is the
 * cost, not the call rate.
 *
 * mem->region_tree is a treap keyed by base, so the same set can be reached in
 * O(log N + k).  The visitor only edits r->perm and host protection -- it never
 * changes the tree shape -- so an in-order traversal is safe here.  Two splits
 * have already run before this point, so no region straddles either boundary.
 *
 * One deliberate difference from the old loop: regions are now visited in base
 * order rather than list order.  Every region in range is still visited, and the
 * per-region work is unchanged; only *which* region reports the first mprotect
 * failure can differ, and an error is an error either way.
 */
typedef struct {
    hb_gva_t start;
    hb_gva_t top;
    hb_perm_t perm;
    bool found;
    bool touched_exec;
    hb_result_t res;
    /* Per-call, on the stack, folded into the globals ONCE at the end.  A shared
     * atomic per visited node would charge the list arm N contended RMWs per call
     * against the tree arm's ~log N, i.e. the instrument would manufacture part of
     * the very difference it is measuring. */
    unsigned long long visited;
    unsigned long long applied;
} protect_range_t;

/* The per-region work, shared verbatim by both traversals.  The A/B arms must
 * differ ONLY in how regions are reached -- if the bodies could drift, a timing
 * difference would not be attributable to the traversal. */
static void protect_apply_region(hb_region_t* node, protect_range_t* w) {
    hb_gva_t rtop;
    hb_perm_t old_perm;

    if (node->base < w->start || node->base >= w->top) return;
    rtop = node->base + node->size;
    if (rtop > w->top) return;

    w->applied++;
    w->found = true;
    old_perm = node->perm;
    if ((old_perm | w->perm) & HB_PERM_EXEC) w->touched_exec = true;

    /*
     * Non-allocated, non-guest32 regions describe Wine-owned live process
     * views.  Their host VM protection is owned by Wine/macOS.  Keep this
     * path metadata-only: applying HB_PERM_* with mprotect() would break
     * Wine's view manager and can make guest x64 code host-executable.
     */
    if (!node->allocated && !node->is_guest32) {
        node->perm = w->perm;
        return;
    }

    node->perm = w->perm;
    if (node->is_guest32) {
#ifdef __APPLE__
        if (node->host_base) {
            hb_result_t sync_r = guest32_sync_host_protection(node, node->base, node->size);
            if ((w->perm & HB_PERM_WRITE) && !(old_perm & HB_PERM_WRITE)) {
                fprintf(stderr,
                        "macrunner-hb-protect-rw-gated: class=guest32 base=0x%llx "
                        "size=%zu old_perm=0x%x new_perm=0x%x sync=%d\n",
                        (unsigned long long)node->base, node->size,
                        (unsigned)old_perm, (unsigned)w->perm, (int)sync_r);
                fflush(stderr);
            }
            if (sync_r != HB_OK) w->res = HB_ERR_MEMORY_FAULT;
        }
#endif
        return;
    }

    if (node->host_base) {
        int prot = prot_from_perm(w->perm, false);
        if (mprotect(node->host_base, node->size, prot) != 0) {
            w->res = HB_ERR_MEMORY_FAULT;
            return;
        }
        if ((w->perm & HB_PERM_WRITE) && !(old_perm & HB_PERM_WRITE)) {
            fprintf(stderr,
                    "macrunner-hb-protect-rw-gated: class=allocated base=0x%llx "
                    "size=%zu old_perm=0x%x new_perm=0x%x prot=0x%x\n",
                    (unsigned long long)node->base, node->size,
                    (unsigned)old_perm, (unsigned)w->perm, prot);
            fflush(stderr);
        }
    }
}

/* ARM "tree": reach the in-range regions through the treap, pruning both sides.
 * Keyed by base, so a node at or above `top` prunes its whole right subtree and
 * one below `start` prunes its left. */
static void protect_range_visit(hb_region_t* node, protect_range_t* w) {
    if (!node || w->res != HB_OK) return;
    w->visited++;

    if (node->base >= w->start) protect_range_visit(node->tree_left, w);
    if (node->base < w->top) protect_range_visit(node->tree_right, w);
    if (w->res != HB_OK) return;

    protect_apply_region(node, w);
}

/* ARM "list": the walk that shipped -- every region in the map, every call.
 * Kept as a live control arm so the A/B is one binary and one deploy. */
static void protect_range_walk_list(hb_memory_t* mem, protect_range_t* w) {
    for (hb_region_t* r = mem->regions; r; r = r->next) {
        w->visited++;
        if (w->res != HB_OK) return;
        protect_apply_region(r, w);
    }
}

static hb_result_t hb_memory_protect_inner(hb_memory_t* mem, hb_gva_t base, size_t size,
                                           hb_perm_t perm) {
    hb_gva_t start;
    hb_gva_t top;
    hb_region_t* exact;
    hb_result_t res;
    bool found = false;
    bool touched_exec = false;

    if (!mem || !size) return HB_ERR_INVALID_ARG;
    if (range_overflows(base, (hb_gva_t)size)) return HB_ERR_INVALID_ARG;

    start = page_floor_gva(base);
    top = page_ceil_gva(base + (hb_gva_t)size);
    if (top <= start) return HB_ERR_INVALID_ARG;

    exact = hb_memory_find_region(mem, start);
    if (exact && start >= exact->base && top <= exact->base + exact->size &&
        exact->perm == perm) {
        __atomic_add_fetch(&mm_mp_fast, 1, __ATOMIC_RELAXED);
        if (!exact->allocated && !exact->is_guest32) return HB_OK;
        if (exact->is_guest32) {
#ifdef __APPLE__
            if (exact->host_base &&
                guest32_sync_host_protection(exact, exact->base, exact->size) != HB_OK)
                return HB_ERR_MEMORY_FAULT;
#endif
            return HB_OK;
        }
        if (exact->host_base) {
            int prot = prot_from_perm(perm, false);
            if (mprotect(exact->host_base, exact->size, prot) != 0) return HB_ERR_MEMORY_FAULT;
        }
        return HB_OK;
    }

    /*
     * A protection change wholly contained by one region needs at most two
     * splits.  Re-enter after those splits so the exact-region path below
     * updates only the requested segment.  Falling through used to scan every
     * node in mem->regions even though the treap had already identified the
     * sole containing region.  Mono heap realloc traffic makes this the common
     * case, so that O(n) walk dominated the swapchain creator thread before its
     * first Present.
     *
     * Keep the full-list fallback for ranges that genuinely span multiple
     * regions or holes.
     */
    if (exact && start >= exact->base && top <= exact->base + exact->size &&
        (start != exact->base || top != exact->base + exact->size)) {
        __atomic_add_fetch(&mm_mp_reenter, 1, __ATOMIC_RELAXED);
        res = split_all_regions_at(mem, start);
        if (res != HB_OK) return res;
        res = split_all_regions_at(mem, top);
        if (res != HB_OK) return res;
        /* Re-enter the inner body, not the wrapper: this is one external call. */
        return hb_memory_protect_inner(mem, start, (size_t)(top - start), perm);
    }

    if (exact && exact->base == start && exact->size == (size_t)(top - start)) {
        hb_perm_t old_perm = exact->perm;

        __atomic_add_fetch(&mm_mp_exact, 1, __ATOMIC_RELAXED);

        if (!exact->allocated && !exact->is_guest32) {
            if ((old_perm | perm) & HB_PERM_EXEC) bump_generation(mem, exact);
            exact->perm = perm;
            return HB_OK;
        }

        exact->perm = perm;
        if (exact->is_guest32) {
#ifdef __APPLE__
            if (exact->host_base) {
                hb_result_t sync_r = guest32_sync_host_protection(exact, exact->base, exact->size);
                if ((perm & HB_PERM_WRITE) && !(old_perm & HB_PERM_WRITE)) {
                    fprintf(stderr,
                            "macrunner-hb-protect-rw-gated: class=guest32 base=0x%llx "
                            "size=%zu old_perm=0x%x new_perm=0x%x sync=%d\n",
                            (unsigned long long)exact->base, exact->size,
                            (unsigned)old_perm, (unsigned)perm, (int)sync_r);
                    fflush(stderr);
                }
                if (sync_r != HB_OK) return HB_ERR_MEMORY_FAULT;
            }
#endif
            if ((old_perm | perm) & HB_PERM_EXEC) bump_generation(mem, exact);
            return HB_OK;
        }

        if (exact->host_base) {
            int prot = prot_from_perm(perm, false);
            if (mprotect(exact->host_base, exact->size, prot) != 0) return HB_ERR_MEMORY_FAULT;
            if ((perm & HB_PERM_WRITE) && !(old_perm & HB_PERM_WRITE)) {
                fprintf(stderr,
                        "macrunner-hb-protect-rw-gated: class=allocated base=0x%llx "
                        "size=%zu old_perm=0x%x new_perm=0x%x prot=0x%x\n",
                        (unsigned long long)exact->base, exact->size,
                        (unsigned)old_perm, (unsigned)perm, prot);
                fflush(stderr);
            }
        }
        if ((old_perm | perm) & HB_PERM_EXEC) bump_generation(mem, exact);
        return HB_OK;
    }

    res = split_all_regions_at(mem, start);
    if (res != HB_OK) return res;
    res = split_all_regions_at(mem, top);
    if (res != HB_OK) return res;

    {
        protect_range_t walk;
        walk.start = start;
        walk.top = top;
        walk.perm = perm;
        walk.found = false;
        walk.touched_exec = false;
        walk.res = HB_OK;
        walk.visited = 0;
        walk.applied = 0;
        __atomic_add_fetch(&mm_mp_walk, 1, __ATOMIC_RELAXED);
        if (macrunner_hb_memory_environment.memprotect_walk_list)
            protect_range_walk_list(mem, &walk);
        else
            protect_range_visit(mem->region_tree, &walk);
        __atomic_add_fetch(&mm_mp_visit, walk.visited, __ATOMIC_RELAXED);
        __atomic_add_fetch(&mm_mp_apply, walk.applied, __ATOMIC_RELAXED);
        if (walk.res != HB_OK) return walk.res;
        found = walk.found;
        touched_exec = walk.touched_exec;
    }

    if (!found) {
        __atomic_add_fetch(&mm_mp_notfound, 1, __ATOMIC_RELAXED);
        return HB_ERR_NOT_FOUND;
    }
    if (touched_exec) bump_generation(mem, NULL);
    return HB_OK;
}

hb_result_t hb_memory_protect(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    unsigned long long n = __atomic_add_fetch(&mm_mp_calls, 1, __ATOMIC_RELAXED);
    bool timed = (n & MM_CLOCK_MASK) == 0;
    unsigned long long t0 = timed ? mm_now_ns() : 0;
    hb_result_t r = hb_memory_protect_inner(mem, base, size, perm);

    if (timed) {
        unsigned long long t1 = mm_now_ns();
        __atomic_add_fetch(&mm_mp_ns, t1 - t0, __ATOMIC_RELAXED);
        __atomic_add_fetch(&mm_mp_ns_n, 1, __ATOMIC_RELAXED);
        if (mm_due(t1)) mm_report(mem, "protect");
    }
    return r;
}

hb_result_t hb_memory_read(hb_memory_t* mem, hb_gva_t addr, void* out, size_t size) {
    hb_region_t* region = NULL;
    bool is_hit = false;
    int dv_hit = macrunner_hb_datadiverge_hit((uint64_t)addr, size);
    if (!mem || !out) return HB_ERR_INVALID_ARG;
    if (!normalize_guest32_mirror_addr(mem, &addr, size)) return HB_ERR_MEMORY_FAULT;
    t_rsite = RSITE_READ;
    region = hot_cache_lookup(mem, addr);
    is_hit = region != NULL;
    if (!region) {
        region = find_region_after_hot_miss(mem, addr);
    }
    if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-mem: gva=0x%llx size=%zu region=%p base=0x%llx host_base=%p rperm=%d\n", (unsigned long long)addr, size, (void*)region, region?(unsigned long long)region->base:0, region?region->host_base:NULL, region?(int)region->perm:-1);
/* MacRunner 2026-07-31 — do not re-derive a NULL we already have.
 *
 * Call-site attribution on a live boot: hb_memory_find_region answers NULL 94.7 % of the time and accounts for
 * 47.8 % of ALL null lookups, and the arithmetic closes exactly — its 63.0 M nulls equal read's 49.9 M plus
 * write's 12.3 M. The reason is right here: a read/write that misses calls can_read_span/can_write_span, which
 * calls check_perm_span, which looks up THE SAME address again. check_perm_span bails on the first null, so
 * when region is already NULL that second lookup is provably NULL as well (same addr — normalize was applied
 * before both — and same epoch), and can_read_span can only return false.
 *
 * So skip it: ~62 M full lookups per run, each a 16-slot scan plus a 17.2-node treap walk, that exist only to
 * recompute a value the caller is holding. Unlike caching the miss, this removes the work rather than making
 * it cheaper. Gated so it can be A/B'd. */
    if (!region || addr + size > region->base + region->size || !(region->perm & HB_PERM_READ)) {
        if ((region || null_span_recheck_enabled()) && hb_memory_can_read_span(mem, addr, size)) {
            uint8_t* dst = out;
            hb_gva_t cur = addr;
            size_t remaining = size;
            while (remaining) {
                hb_region_t* r = hb_memory_find_region(mem, cur);
                size_t chunk = (size_t)(r->base + r->size - cur);
                hb_result_t rr;
                if (chunk > remaining) chunk = remaining;
                rr = hb_memory_read(mem, cur, dst, chunk);
                if (rr != HB_OK) return rr;
                cur += chunk;
                dst += chunk;
                remaining -= chunk;
            }
            return HB_OK;
        }
        if (mem->special_read && mem->special_read(mem->special_user, addr, out, size) == HB_OK) return HB_OK;
        return HB_ERR_MEMORY_FAULT;
    }
#ifdef __APPLE__
    if (region && !region->allocated && !region->host_base) {
        memcpy(out, (const void*)(uintptr_t)addr, size);
        if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-mem: -> identity val=0x%llx\n", (unsigned long long)(size==8?*(const uint64_t*)out:0));
        return HB_OK;
    }
#endif
    if (region && region->host_base) {
#ifdef __APPLE__
        if (region->is_guest32)
        {
            const void* host = region_host_ptr(region, addr);
            /* Use memcpy only when live-memory mode is enabled AND the access is
             * known safe (aligned, single page).  In all other cases use the Mach
             * read path so a PROT_NONE host page returns HB_ERR_MEMORY_FAULT
             * instead of crashing inside memcpy (PC in libc = outside JIT slab =
             * signal guard misses it = recursive c0000005 fault loop). */
            if (is_hit && guest32_direct_copy_safe(region, addr, size))
            {
                sigjmp_buf jmp;
                install_sig_handlers();
                g_safe_copy_jmp = &jmp;
                if (sigsetjmp(jmp, 1) == 0)
                {
                    memcpy(out, host, size);
                    g_safe_copy_jmp = NULL;
                    return HB_OK;
                }
                else
                {
                    g_safe_copy_jmp = NULL;
                    return mach_copy_from_host(out, host, size);
                }
            }
            return mach_copy_from_host(out, host, size);
        }
        if (guest32_copy_needs_mach(region, addr, size))
            return mach_copy_from_host(out, region_host_ptr(region, addr), size);
#endif
        memcpy(out, region_host_ptr(region, addr), size);
        if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-mem: -> region val=0x%llx\n", (unsigned long long)(size==8?*(const uint64_t*)out:0));
        return HB_OK;
    }
    if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-mem: -> identity(fallback) val=0x%llx\n", (unsigned long long)(size==8?*(const uint64_t*)out:0));
    memcpy(out, (void*)(uintptr_t)addr, size);
    return HB_OK;
}

hb_result_t hb_memory_write(hb_memory_t* mem, hb_gva_t addr, const void* in, size_t size) {
    hb_region_t* region = NULL;
    bool is_hit = false;
    int grow_retried = 0;
    if (!mem || !in) return HB_ERR_INVALID_ARG;
    if (!normalize_guest32_mirror_addr(mem, &addr, size)) return HB_ERR_MEMORY_FAULT;
    trace_bad_native_write("hb_memory_write", addr, in, size);
    trace_guest_write("hb_memory_write", addr, in, size);
    /* HK lockstep: log the realloc-copy store that writes 0x80 into the 0x3xx offset array during
     * the r13=0x11 insert window (g_hk_tg), with the guest instruction rva — catches GPR AND SSE. */
    {
        extern int g_hk_tg; extern uint64_t g_hk_cur_ga;
        if (g_hk_tg && size >= 8 && macrunner_hb_memory_environment.trace_store80) {
            const uint8_t* b8 = (const uint8_t*)in;
            for (size_t o = 0; o + 8 <= size; o += 8) {
                uint64_t v; memcpy(&v, b8 + o, 8);
                if ((v & 0xffffffffull) == 0x80ull) {
                    static int n80 = 0;
                    if (n80 < 24) {
                        fprintf(stderr, "store80mem: addr=%llx off=%zu size=%zu val=%llx guest_rva=%llx\n",
                            (unsigned long long)(addr + o), o, size, (unsigned long long)v,
                            (unsigned long long)(g_hk_cur_ga - 0x87efc510000ull));
                        fflush(stderr); n80++;
                    }
                }
            }
        }
    }
    /* Memcpy length/extent audit (env MACRUNNER_HB_TRACE_MEMCPY_LEN set): the over-write goes
     * through hb_memory_write (interpreted SSE stores).  Track the LONGEST monotonic +0x10 run
     * of 16-byte writes in the high guest-heap slab range: max_run*0x10 = largest contiguous
     * memcpy.  ~16 MB => bounded per-slab (legit); ~3.5 GB => one unbounded copy (runaway). */
    {
        if (macrunner_hb_memory_environment.trace_memcpy_len &&
            size && size <= 64 && addr >= 0x300000000ull && addr < 0x400000000ull) {
            /* longest CONTIGUOUS forward byte-run (any stride <=0x40): a "run" continues while
             * each write starts within 0x40 of the previous. max_bytes = largest single memcpy. */
            static unsigned long long n, max_bytes, max_at;
            static hb_gva_t prev, run_start, alo = ~0ull, ahi;
            unsigned long long cur;
            n++;
            if (addr < alo) alo = addr;
            if (addr + size > ahi) ahi = addr + size;
            if (n == 1 || !(addr >= prev && addr - prev <= 0x40)) run_start = addr;
            cur = (unsigned long long)(addr + size - run_start);
            if (cur > max_bytes) { max_bytes = cur; max_at = addr; }
            prev = addr;
            if (n <= 4 || (n % 4000000ull) == 0) {
                fprintf(stderr, "macrunner-hb-memcpy-w: n=%llu addr=%llx sz=%zu span=%llx "
                        "cur_run=0x%llx max_bytes=0x%llx max_at=%llx\n",
                        n, (unsigned long long)addr, size, (unsigned long long)(ahi - alo),
                        cur, max_bytes, (unsigned long long)max_at);
                fflush(stderr);
            }
        }
    }
grow_retry:
    t_rsite = RSITE_WRITE;
    region = hot_cache_lookup(mem, addr);
    is_hit = region != NULL;
    if (!region) {
        region = find_region_after_hot_miss(mem, addr);
    }
    if (macrunner_hb_datadiverge_hit((uint64_t)addr, size)) {
        fprintf(stderr, "macrunner-hb-dv-write: gva=0x%llx size=%zu region=%p base=0x%llx rsize=0x%llx host_base=%p rperm=%d val=0x%llx\n",
                (unsigned long long)addr, size, (void*)region,
                region ? (unsigned long long)region->base : 0,
                region ? (unsigned long long)region->size : 0,
                region ? region->host_base : NULL,
                region ? (int)region->perm : -1,
                (unsigned long long)(size==8?*(const uint64_t*)in:0));
    }
#ifdef __APPLE__
    if ((addr & 0xfff) >= 0xfe0 && macrunner_hb_memory_environment.trace_jit_helper_fail) {
        static int t;
        if (t++ < 12) {
            int branch = (!region || addr + size > region->base + region->size || !(region->perm & HB_PERM_WRITE)) ? 0
                       : (!region->allocated && !region->host_base) ? (region->perm & HB_PERM_EXEC ? 2 : 1)
                       : region->host_base ? 3 : 4;
            fprintf(stderr, "macrunner-hb-wedge-write: addr=0x%llx size=%zu region=%p base=0x%llx rsize=0x%llx "
                    "perm=%d alloc=%d host=%p branch=%d\n",
                    (unsigned long long)addr, size, (void*)region,
                    region ? (unsigned long long)region->base : 0,
                    region ? (unsigned long long)region->size : 0,
                    region ? (int)region->perm : -1, region ? (int)region->allocated : -1,
                    region ? region->host_base : NULL, branch);
        }
    }
#endif
    if (!region || addr + size > region->base + region->size || !(region->perm & HB_PERM_WRITE)) {
#ifdef __APPLE__
        /* MacRunner sentinel trace: log write blocked on sentinel range or null-zone. */
        if ((addr >= 0x7BD8E000u && addr < 0x7BD8F000u) || addr < 0x1000u) {
            fprintf(stderr,
                    "macrunner-hb-write-blocked: addr=%08llx size=%zu "
                    "region=%s perm=%d no_w=%d\n",
                    (unsigned long long)addr, size,
                    region ? "found" : "null",
                    region ? (int)region->perm : -1,
                    region ? !(region->perm & HB_PERM_WRITE) : 1);
            fflush(stderr);
        }
#endif
        if ((region || null_span_recheck_enabled()) && hb_memory_can_write_span(mem, addr, size)) {
            const uint8_t* src = in;
            hb_gva_t cur = addr;
            size_t remaining = size;
            while (remaining) {
                hb_region_t* r = hb_memory_find_region(mem, cur);
                size_t chunk = (size_t)(r->base + r->size - cur);
                hb_result_t rr;
                if (chunk > remaining) chunk = remaining;
                rr = hb_memory_write(mem, cur, src, chunk);
                if (rr != HB_OK) return rr;
                cur += chunk;
                src += chunk;
                remaining -= chunk;
            }
            return HB_OK;
        }
        if (mem->special_write && mem->special_write(mem->special_user, addr, in, size) == HB_OK) return HB_OK;
        /* Guest stack growth below the recorded region bottom (x64 CALL/PUSH at
         * rsp already under region->base): the page may be Wine-reserved guard
         * space that virtual_handle_fault can commit.  Let the embedder grow,
         * then redo the region lookup and the whole write once. */
        if (!grow_retried && mem->special_grow && mem->special_grow(mem->special_user, addr)) {
            grow_retried = 1;
            goto grow_retry;
        }
        {
            static int traced;
            int heavy = (addr >= 0x3f0000000ULL && addr < 0x401000000ULL);
            if (heavy || traced++ < 8)
                fprintf(stderr, "macrunner-hb-write-deny: addr=0x%llx size=%zu region=%p base=0x%llx "
                        "rsize=0x%llx perm=%d alloc=%d host=%p\n",
                        (unsigned long long)addr, size, (void*)region,
                        region ? (unsigned long long)region->base : 0,
                        region ? (unsigned long long)region->size : 0,
                        region ? (int)region->perm : -1,
                        region ? (int)region->allocated : -1,
                        region ? region->host_base : NULL);
        }
        return HB_ERR_MEMORY_FAULT;
    }
#ifdef __APPLE__
    if (region && !region->allocated && !region->host_base) {
        if (!(region->perm & HB_PERM_EXEC) && !live_vm_write_mach_enabled()) {
            memcpy((void*)(uintptr_t)addr, in, size);
            return HB_OK;
        }
        hb_result_t result = write_live_vm_region(mem, addr, in, size, region);
        if (result == HB_OK && (region->perm & HB_PERM_EXEC)) bump_generation(mem, region);
        return result;
    }
#endif
    if (region && region->host_base) {
#ifdef __APPLE__
        if (region->is_guest32)
        {
            void* host = region_host_ptr(region, addr);
            if (region->perm & HB_PERM_EXEC)
            {
                // executable/translated -> NOT bare memcpy, call special_write (SMC invalidation)
                if (mem->special_write && mem->special_write(mem->special_user, addr, in, size) == HB_OK) return HB_OK;
                hb_result_t result = mach_copy_to_host(host, in, size);
                if (result != HB_OK) {
                    hb_result_t sync_r = HB_ERR_MEMORY_FAULT;
                    if (region->perm & HB_PERM_WRITE) {
                        sync_r = guest32_sync_host_protection(region, addr, size);
                        if (sync_r == HB_OK) result = mach_copy_to_host(host, in, size);
                    }
#ifdef __APPLE__
                    /* MacRunner sentinel trace: log mach_copy_to_host failure on sentinel page. */
                    if (addr >= 0x7BD8E000u && addr < 0x7BD8F000u) {
                        mach_vm_address_t dbg_addr = (mach_vm_address_t)(uintptr_t)host;
                        mach_vm_size_t dbg_sz = 0;
                        vm_region_basic_info_data_64_t dbg_ri;
                        mach_msg_type_number_t dbg_cnt = VM_REGION_BASIC_INFO_COUNT_64;
                        mach_port_t dbg_obj = MACH_PORT_NULL;
                        memset(&dbg_ri, 0, sizeof(dbg_ri));
                        mach_vm_region(mach_task_self(), &dbg_addr, &dbg_sz, VM_REGION_BASIC_INFO_64,
                                       (vm_region_info_t)&dbg_ri, &dbg_cnt, &dbg_obj);
                        if (dbg_obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), dbg_obj);
                        fprintf(stderr,
                                "macrunner-hb-sentinel-write-fail: addr=%08llx perm=%d "
                                "mach_prot=0x%x max_prot=0x%x sync_r=%d retry_r=%d\n",
                                (unsigned long long)addr, (int)region->perm,
                                (int)dbg_ri.protection, (int)dbg_ri.max_protection,
                                (int)sync_r, (int)result);
                        fflush(stderr);
                    }
#endif
                }
                if (result != HB_OK) return result;
            }
            else if (is_hit && guest32_direct_copy_safe(region, addr, size))
            {
                sigjmp_buf jmp;
                install_sig_handlers();
                g_safe_copy_jmp = &jmp;
                if (sigsetjmp(jmp, 1) == 0)
                {
                    memcpy(host, in, size);
                    g_safe_copy_jmp = NULL;
                }
                else
                {
                    g_safe_copy_jmp = NULL;
                    hb_result_t result = mach_copy_to_host(host, in, size);
                    if (result != HB_OK) {
                        hb_result_t sync_r = HB_ERR_MEMORY_FAULT;
                        if (region->perm & HB_PERM_WRITE) {
                            sync_r = guest32_sync_host_protection(region, addr, size);
                            if (sync_r == HB_OK) result = mach_copy_to_host(host, in, size);
                        }
#ifdef __APPLE__
                        /* MacRunner sentinel trace: log mach_copy_to_host failure on sentinel page. */
                        if (addr >= 0x7BD8E000u && addr < 0x7BD8F000u) {
                            mach_vm_address_t dbg_addr = (mach_vm_address_t)(uintptr_t)host;
                            mach_vm_size_t dbg_sz = 0;
                            vm_region_basic_info_data_64_t dbg_ri;
                            mach_msg_type_number_t dbg_cnt = VM_REGION_BASIC_INFO_COUNT_64;
                            mach_port_t dbg_obj = MACH_PORT_NULL;
                            memset(&dbg_ri, 0, sizeof(dbg_ri));
                            mach_vm_region(mach_task_self(), &dbg_addr, &dbg_sz, VM_REGION_BASIC_INFO_64,
                                           (vm_region_info_t)&dbg_ri, &dbg_cnt, &dbg_obj);
                            if (dbg_obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), dbg_obj);
                            fprintf(stderr,
                                    "macrunner-hb-sentinel-write-fail: addr=%08llx perm=%d "
                                    "mach_prot=0x%x max_prot=0x%x sync_r=%d retry_r=%d\n",
                                    (unsigned long long)addr, (int)region->perm,
                                    (int)dbg_ri.protection, (int)dbg_ri.max_protection,
                                    (int)sync_r, (int)result);
                            fflush(stderr);
                        }
#endif
                    }
                    if (result != HB_OK) return result;
                }
            }
            else
            {
                hb_result_t result = mach_copy_to_host(host, in, size);
                if (result != HB_OK) {
                    hb_result_t sync_r = HB_ERR_MEMORY_FAULT;
                    if (region->perm & HB_PERM_WRITE) {
                        sync_r = guest32_sync_host_protection(region, addr, size);
                        if (sync_r == HB_OK) result = mach_copy_to_host(host, in, size);
                    }
#ifdef __APPLE__
                    /* MacRunner sentinel trace: log mach_copy_to_host failure on sentinel page. */
                    if (addr >= 0x7BD8E000u && addr < 0x7BD8F000u) {
                        mach_vm_address_t dbg_addr = (mach_vm_address_t)(uintptr_t)host;
                        mach_vm_size_t dbg_sz = 0;
                        vm_region_basic_info_data_64_t dbg_ri;
                        mach_msg_type_number_t dbg_cnt = VM_REGION_BASIC_INFO_COUNT_64;
                        mach_port_t dbg_obj = MACH_PORT_NULL;
                        memset(&dbg_ri, 0, sizeof(dbg_ri));
                        mach_vm_region(mach_task_self(), &dbg_addr, &dbg_sz, VM_REGION_BASIC_INFO_64,
                                       (vm_region_info_t)&dbg_ri, &dbg_cnt, &dbg_obj);
                        if (dbg_obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), dbg_obj);
                        fprintf(stderr,
                                "macrunner-hb-sentinel-write-fail: addr=%08llx perm=%d "
                                "mach_prot=0x%x max_prot=0x%x sync_r=%d retry_r=%d\n",
                                (unsigned long long)addr, (int)region->perm,
                                (int)dbg_ri.protection, (int)dbg_ri.max_protection,
                                (int)sync_r, (int)result);
                        fflush(stderr);
                    }
#endif
                }
                if (result != HB_OK) return result;
            }
            if (region->perm & HB_PERM_EXEC) bump_generation(mem, region);
            return HB_OK;
        }
        if (guest32_copy_needs_mach(region, addr, size))
        {
            hb_result_t result = mach_copy_to_host(region_host_ptr(region, addr), in, size);
            if (result != HB_OK) return result;
            if (region->perm & HB_PERM_EXEC) bump_generation(mem, region);
            return HB_OK;
        }
#endif
        memcpy(region_host_ptr(region, addr), in, size);
        if (region->perm & HB_PERM_EXEC) bump_generation(mem, region);
        return HB_OK;
    }
    memcpy((void*)(uintptr_t)addr, in, size);
    return HB_OK;
}

void* hb_memory_host_ptr(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t perm) {
    hb_region_t* region;
    int dv_hit = macrunner_hb_datadiverge_hit((uint64_t)addr, size);

    if (!mem || !size) return NULL;
    if (!normalize_guest32_mirror_addr(mem, &addr, size)) return NULL;
    t_rsite = RSITE_HOST_PTR;
    region = find_region_normalized(mem, addr);
    if (dv_hit) {
        fprintf(stderr, "macrunner-hb-dv-read-host_ptr: gva=0x%llx size=%zu want_perm=%d region=%p base=0x%llx rsize=0x%llx host_base=%p rperm=%d\n",
                (unsigned long long)addr, size, (int)perm, (void*)region,
                region ? (unsigned long long)region->base : 0,
                region ? (unsigned long long)region->size : 0,
                region ? region->host_base : NULL,
                region ? (int)region->perm : -1);
    }
    if (!region || addr + size > region->base + region->size) { if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-host_ptr: NO-REGION -> NULL (live fallback)\n"); return NULL; }
    if ((region->perm & perm) != perm) { if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-host_ptr: PERM-MISMATCH -> NULL (live fallback)\n"); return NULL; }
#ifdef __APPLE__
    if (region->host_base && region->is_guest32 && (perm & HB_PERM_WRITE) &&
        guest32_copy_needs_mach(region, addr, size))
        return NULL;
#endif
    if (region->host_base) { void* p = region_host_ptr(region, addr); if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-host_ptr: -> region host=%p\n", p); return p; }
    if ((perm & HB_PERM_WRITE) && (region->perm & HB_PERM_EXEC)) { if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-host_ptr: EXEC-write-guard -> NULL\n"); return NULL; }
    if (dv_hit) fprintf(stderr, "macrunner-hb-dv-read-host_ptr: -> identity host=%p\n", (void*)(uintptr_t)addr);
    return (void*)(uintptr_t)addr;
}

void hb_memory_set_special_handlers(hb_memory_t* mem,
                                    hb_result_t (*read_fn)(void* user, hb_gva_t addr, void* out, size_t size),
                                    hb_result_t (*write_fn)(void* user, hb_gva_t addr, const void* in, size_t size),
                                    void* user) {
    if (!mem) return;
    mem->special_read = read_fn;
    mem->special_write = write_fn;
    mem->special_user = user;
}

void hb_memory_set_grow_handler(hb_memory_t* mem, bool (*grow_fn)(void* user, hb_gva_t addr)) {
    if (!mem) return;
    mem->special_grow = grow_fn;
}

#define RW_U(bits) \
hb_result_t hb_memory_read_u##bits(hb_memory_t* mem, hb_gva_t addr, uint##bits##_t* out) { \
    return hb_memory_read(mem, addr, out, sizeof(uint##bits##_t)); \
} \
hb_result_t hb_memory_write_u##bits(hb_memory_t* mem, hb_gva_t addr, uint##bits##_t val) { \
    return hb_memory_write(mem, addr, &val, sizeof(uint##bits##_t)); \
}

RW_U(8)
RW_U(16)
RW_U(32)
RW_U(64)

hb_result_t hb_memory_fetch(hb_memory_t* mem, hb_gva_t addr, uint8_t* out) {
    return hb_memory_read_u8(mem, addr, out);
}

hb_region_t* hb_memory_find_region(hb_memory_t* mem, hb_gva_t addr) {
    if (!mem) return NULL;
    if (!normalize_guest32_mirror_addr(mem, &addr, 1)) return NULL;
    /* Only claim the generic tag when no caller identified itself, otherwise the external tags below would be
     * overwritten by the very wrapper they call. Callers reset to 0 after use. */
    if (t_rsite < RSITE_JIT_HOST_SPAN) t_rsite = RSITE_FIND_REGION;
    return find_region_normalized(mem, addr);
}

/* MacRunner 2026-07-31 — hb_memory_read/hb_memory_write already ran hot_cache_lookup and only call this on a
 * miss, yet the unconditional lookup below then ran the SAME 16-slot linear scan a second time. That second
 * scan can only ever miss: same addr, and either the epoch is unchanged (same slots -> same verdict) or it
 * changed (hot_cache_reset_tls NULLs every slot -> empty scan). So it is pure waste, ~32 % of every lookup in
 * the run paying a 16-deep pointer chase twice. use_hot=false skips it on those known-miss paths; the gate
 * exists only so the redundancy can be re-armed for an A/B. */
static int hot_miss_skip_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_NO_HOT_MISS_SKIP");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static hb_region_t* find_region_impl(hb_memory_t* mem, hb_gva_t addr, bool use_hot) {
    if (!mem) return NULL;
    if (mem->guest32_base && addr < HB_GUEST32_SIZE) {
        hb_region_t* n = mem->region_tree;
        while (n) {
            if (addr < n->base) {
                n = n->tree_left;
            } else if (addr >= n->base + n->size) {
                n = n->tree_right;
            } else {
                return n->is_guest32 ? n : NULL;
            }
        }
    }

    if (use_hot) {
        hb_region_t* hot = hot_cache_lookup(mem, addr);
        if (hot) return hot;
    }

    /* ONE acquire load of the negative epoch for the whole resolve, not one per helper. */
    uint64_t add_epoch = __atomic_load_n(&mem->add_gen, __ATOMIC_ACQUIRE);
    if (gap_lookup(mem, addr, add_epoch)) {
        /* Falsify the bracket instead of arguing for it: with MACRUNNER_HB_VERIFY_NEG_CACHE the gap answer is
         * checked against the full walk it replaced, and any disagreement is printed with the entry that lied.
         * A gap hit claims "no region contains addr"; if the treap finds one, the bracket is wrong. */
        if (verify_neg_cache_enabled()) {
            for (hb_region_t* v = mem->region_tree; v; ) {
                if (addr < v->base) v = v->tree_left;
                else if (addr >= v->base + v->size) v = v->tree_right;
                else {
                    fprintf(stderr, "macrunner-hb-negcache-VIOLATION: addr=0x%llx claimed unmapped but region "
                                    "[0x%llx,0x%llx) contains it; gap[0]=[0x%llx,0x%llx) epoch=%llu\n",
                            (unsigned long long)addr, (unsigned long long)v->base,
                            (unsigned long long)(v->base + v->size),
                            (unsigned long long)g_gap_tls.e[0].lo, (unsigned long long)g_gap_tls.e[0].hi,
                            (unsigned long long)add_epoch);
                    fflush(stderr);
                    return v;
                }
            }
        }
        return NULL;
    }

    /* MacRunner (2026-06-17, HK first-frame perf): O(log n) treap walk instead of an O(n)
     * linear scan of mem->regions.  mem->region_tree is the SAME treap the guest32 path
     * walks above: keyed by base, maintained on every add (insert_region_head -> tree_insert)
     * and rebuilt on remove (rebuild_region_tree).  Regions are non-overlapping (every add is
     * guarded by any_overlap), so the containing region is UNIQUE and this walk returns
     * exactly the region the old linear scan would (identical result, just O(log n)).  Once the
     * module_from_pc mach-scan was fixed, find_region_normalized became the #1 main-thread
     * hotspot (~33% during Mono ReloadAssembly) — the guest's region list grows large under
     * Mono so the per-memory-access linear scan dominated. */
    hb_gva_t gap_lo = 0, gap_hi = ~(hb_gva_t)0;
    uint64_t nodes = 0;
    for (hb_region_t* n = mem->region_tree; n; ) {
        nodes++;
        if (addr < n->base) {
            if (n->base < gap_hi) gap_hi = n->base;      /* last left turn = successor */
            n = n->tree_left;
        } else if (addr >= n->base + n->size) {
            if (n->base + n->size > gap_lo) gap_lo = n->base + n->size;  /* last right turn = predecessor */
            n = n->tree_right;
        } else {
            if (trace_hot_cache_stats_enabled()) { t_walk_nodes_ok += nodes; t_walk_ok++; }
            hot_cache_insert(mem, n);
            hot_cache_note_resolved(n);
            return n;
        }
    }
    if (trace_hot_cache_stats_enabled()) { t_walk_nodes_fail += nodes; t_walk_fail++; }
    gap_insert(mem, gap_lo, gap_hi, add_epoch);
    hot_cache_note_unresolved(addr);
    return NULL;
}

static hb_region_t* find_region_normalized(hb_memory_t* mem, hb_gva_t addr) {
    return find_region_impl(mem, addr, true);
}

/* For callers that just missed in the hot cache themselves. */
static hb_region_t* find_region_after_hot_miss(hb_memory_t* mem, hb_gva_t addr) {
    return find_region_impl(mem, addr, hot_miss_skip_disabled());
}

static bool check_perm_region(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p, hb_region_t** out) {
    hb_region_t* r;
    if (!normalize_guest32_mirror_addr(mem, &addr, size)) return false;
    t_rsite = RSITE_CHECK_PERM;
    r = find_region_normalized(mem, addr);
    if (out) *out = r;
    if (!r) return false;
    if (addr + size > r->base + r->size) return false;
    return (r->perm & p) != 0;
}

static bool check_perm(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p) {
    return check_perm_region(mem, addr, size, p, NULL);
}

static bool check_perm_span(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p) {
    hb_gva_t cur = addr;
    size_t remaining = size;

    if (!mem) return false;
    if (!normalize_guest32_mirror_addr(mem, &cur, size)) return false;
    while (remaining) {
        hb_region_t* r = hb_memory_find_region(mem, cur);
        size_t chunk;

        if (!r || !(r->perm & p)) return false;
        chunk = (size_t)(r->base + r->size - cur);
        if (chunk > remaining) chunk = remaining;
        cur += chunk;
        remaining -= chunk;
    }
    return true;
}

bool hb_memory_can_read(hb_memory_t* mem, hb_gva_t addr, size_t size) {
    return check_perm(mem, addr, size, HB_PERM_READ);
}
bool hb_memory_can_write(hb_memory_t* mem, hb_gva_t addr, size_t size) {
    return check_perm(mem, addr, size, HB_PERM_WRITE);
}
bool hb_memory_can_exec(hb_memory_t* mem, hb_gva_t addr, size_t size) {
    return check_perm(mem, addr, size, HB_PERM_EXEC);
}

bool hb_memory_can_read_span(hb_memory_t* mem, hb_gva_t addr, size_t size) {
    return check_perm_span(mem, addr, size, HB_PERM_READ);
}

bool hb_memory_can_write_span(hb_memory_t* mem, hb_gva_t addr, size_t size) {
    return check_perm_span(mem, addr, size, HB_PERM_WRITE);
}

hb_result_t hb_memory_setup_stack(hb_memory_t* mem, hb_gva_t top, size_t size) {
    if (!mem) return HB_ERR_INVALID_ARG;
    (void)top;
    hb_result_t r = hb_memory_map(mem, 0, size, HB_PERM_READ | HB_PERM_WRITE);
    if (r != HB_OK) return r;
    hb_region_t* rg = mem->regions;
    if (!rg) return HB_ERR_INTERNAL;
    rg->is_stack = true;
    mem->stack_bottom = rg->base;
    mem->stack_top = rg->base + rg->size;
    return HB_OK;
}

hb_result_t hb_memory_setup_heap(hb_memory_t* mem, hb_gva_t base, size_t size) {
    if (!mem) return HB_ERR_INVALID_ARG;
    (void)base;
    hb_result_t r = hb_memory_map(mem, 0, size, HB_PERM_READ | HB_PERM_WRITE);
    if (r != HB_OK) return r;
    hb_region_t* rg = mem->regions;
    if (!rg) return HB_ERR_INTERNAL;
    rg->is_heap = true;
    mem->heap_base = rg->base;
    mem->heap_size = rg->size;
    return HB_OK;
}
