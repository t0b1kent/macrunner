#include "hb_contract_telemetry.h"

#include <stdlib.h>
#include <string.h>

static uint64_t g_open_ok;
static uint64_t g_open_fail;
static uint64_t g_hits;
static uint64_t g_misses;
static uint64_t g_stores;
static uint64_t g_store_skips;
static uint64_t g_store_skip_multi;
static uint64_t g_store_skip_unmatched;
static uint64_t g_mh_too_many;
static uint64_t g_mh_wide_arg;
static uint64_t g_mh_unknown_helper;
static uint64_t g_mh_arg_shape;
static uint64_t g_reloc_blocks;
static uint64_t g_reloc_sites;
static uint64_t g_reloc_overflow;
static uint64_t g_rl_stores;
static uint64_t g_rl_literal_sites;
static uint64_t g_rl_highhalf_sites;
static uint64_t g_rl_patched_sites;
static uint64_t g_rl_x23_value;
static uint64_t g_promo_compiles;
static uint64_t g_rl_hp_succ;
static uint64_t g_rl_hp_pred;
static uint64_t g_rl_hp_succ_instr;
static uint64_t g_rl_hp_other;
static uint64_t g_rl_hostptr;
static uint64_t g_rl_unknown_helper;
static uint64_t g_rl_overflow;
static uint64_t g_rl_desync;
static uint64_t g_rl_collision;
static uint64_t g_rl_roundtrip;
static uint64_t g_bytes_loaded;
static uint64_t g_bytes_stored;
static uint64_t g_compile_count;
static uint64_t g_translation_count;
static uint64_t g_distinct_translation_count;
static uint64_t g_dispatches;
static uint64_t g_blocks;
static uint64_t g_steps;
static int g_trace_enabled_cached = -1;
static int g_atexit_registered;
static int g_summary_emitted;

static void telemetry_add(uint64_t* dst, uint64_t val) {
    if (!hb_contract_telemetry_enabled()) return;
    __atomic_fetch_add(dst, val, __ATOMIC_RELAXED);
}

static uint64_t telemetry_load(const uint64_t* src) {
    return __atomic_load_n(src, __ATOMIC_RELAXED);
}

/* Defined below, next to the summary emitter it shares a format with. */
static void hb_contract_telemetry_maybe_emit_progress(void);

int hb_contract_telemetry_enabled(void) {
    int cached = __atomic_load_n(&g_trace_enabled_cached, __ATOMIC_RELAXED);
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_TRANSLATION_CACHE");
        cached = env && *env && *env != '0';
        __atomic_store_n(&g_trace_enabled_cached, cached, __ATOMIC_RELAXED);
    }
    return cached;
}

void hb_contract_telemetry_record_open(bool ok) {
    telemetry_add(ok ? &g_open_ok : &g_open_fail, 1);
}

void hb_contract_telemetry_record_cache_hit(uint64_t bytes_loaded) {
    telemetry_add(&g_hits, 1);
    telemetry_add(&g_bytes_loaded, bytes_loaded);
}

void hb_contract_telemetry_record_cache_miss(void) {
    telemetry_add(&g_misses, 1);
}

void hb_contract_telemetry_record_cache_store(uint64_t bytes_stored) {
    telemetry_add(&g_stores, 1);
    telemetry_add(&g_bytes_stored, bytes_stored);
}

void hb_contract_telemetry_record_cache_store_skip(void) {
    telemetry_add(&g_store_skips, 1);
}

void hb_contract_telemetry_record_cache_store_skip_multi(void) {
    telemetry_add(&g_store_skip_multi, 1);
}

void hb_contract_telemetry_record_cache_store_skip_unmatched(void) {
    telemetry_add(&g_store_skip_unmatched, 1);
}

void hb_contract_telemetry_record_mh_reason(int reason) {
    switch (reason) {
        case 1: telemetry_add(&g_mh_too_many, 1); break;
        case 2: telemetry_add(&g_mh_wide_arg, 1); break;
        case 3: telemetry_add(&g_mh_unknown_helper, 1); break;
        case 4: telemetry_add(&g_mh_arg_shape, 1); break;
        default: break;
    }
}

void hb_contract_telemetry_record_reloc(unsigned long sites, int overflow) {
    telemetry_add(&g_reloc_blocks, 1);
    telemetry_add(&g_reloc_sites, (uint64_t)sites);
    if (overflow) telemetry_add(&g_reloc_overflow, 1);
}

void hb_contract_telemetry_record_reloc_store(unsigned long patched, unsigned long literal) {
    telemetry_add(&g_rl_stores, 1);
    telemetry_add(&g_rl_patched_sites, (uint64_t)patched);
    telemetry_add(&g_rl_literal_sites, (uint64_t)literal);
}

void hb_contract_telemetry_record_reloc_highhalf(unsigned long sites) {
    telemetry_add(&g_rl_highhalf_sites, (uint64_t)sites);
}

void hb_contract_telemetry_record_reloc_x23_value(unsigned long sites) {
    telemetry_add(&g_rl_x23_value, (uint64_t)sites);
}

void hb_contract_telemetry_record_promote_compile(void) {
    telemetry_add(&g_promo_compiles, 1);
}

/* Reasons mirror hb_reloc_decline_t in hb_runtime.c. Kept as an int across the boundary so the
 * telemetry header does not have to know the runtime's private enum. */
void hb_contract_telemetry_record_reloc_decline(int reason) {
    switch (reason) {
        case 1: telemetry_add(&g_rl_overflow, 1); break;
        case 2: telemetry_add(&g_rl_unknown_helper, 1); break;
        case 3: telemetry_add(&g_rl_hostptr, 1); break;
        case 4: telemetry_add(&g_rl_collision, 1); break;
        case 5: telemetry_add(&g_rl_desync, 1); break;
        case 6: telemetry_add(&g_rl_roundtrip, 1); break;
        default: break;
    }
}

void hb_contract_telemetry_record_hostptr_census(int bucket) {
    switch (bucket) {
        case 0: telemetry_add(&g_rl_hp_succ, 1); break;
        case 1: telemetry_add(&g_rl_hp_pred, 1); break;
        case 2: telemetry_add(&g_rl_hp_succ_instr, 1); break;
        default: telemetry_add(&g_rl_hp_other, 1); break;
    }
}

void hb_contract_telemetry_record_compile(void) {
    telemetry_add(&g_compile_count, 1);
    hb_contract_telemetry_maybe_emit_progress();
}

void hb_contract_telemetry_record_translation(bool distinct) {
    telemetry_add(&g_translation_count, 1);
    if (distinct) telemetry_add(&g_distinct_translation_count, 1);
}

void hb_contract_telemetry_record_dispatch(uint64_t dispatches, uint64_t blocks, uint64_t steps) {
    telemetry_add(&g_dispatches, dispatches);
    telemetry_add(&g_blocks, blocks);
    telemetry_add(&g_steps, steps);
}

void hb_contract_telemetry_snapshot(hb_contract_telemetry_counts_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->open_ok = telemetry_load(&g_open_ok);
    out->open_fail = telemetry_load(&g_open_fail);
    out->hits = telemetry_load(&g_hits);
    out->misses = telemetry_load(&g_misses);
    out->stores = telemetry_load(&g_stores);
    out->store_skip_multi_helper = telemetry_load(&g_store_skip_multi);
    out->store_skip_unmatched = telemetry_load(&g_store_skip_unmatched);
    out->mh_too_many = telemetry_load(&g_mh_too_many);
    out->mh_wide_arg = telemetry_load(&g_mh_wide_arg);
    out->mh_unknown_helper = telemetry_load(&g_mh_unknown_helper);
    out->mh_arg_shape = telemetry_load(&g_mh_arg_shape);
    out->reloc_blocks = telemetry_load(&g_reloc_blocks);
    out->reloc_sites = telemetry_load(&g_reloc_sites);
    out->reloc_overflow = telemetry_load(&g_reloc_overflow);
    out->rl_stores = telemetry_load(&g_rl_stores);
    out->rl_literal_sites = telemetry_load(&g_rl_literal_sites);
    out->rl_highhalf_sites = telemetry_load(&g_rl_highhalf_sites);
    out->rl_patched_sites = telemetry_load(&g_rl_patched_sites);
    out->rl_x23_value = telemetry_load(&g_rl_x23_value);
    out->promo_compiles = telemetry_load(&g_promo_compiles);
    out->rl_hostptr = telemetry_load(&g_rl_hostptr);
    out->rl_hp_succ = telemetry_load(&g_rl_hp_succ);
    out->rl_hp_pred = telemetry_load(&g_rl_hp_pred);
    out->rl_hp_succ_instr = telemetry_load(&g_rl_hp_succ_instr);
    out->rl_hp_other = telemetry_load(&g_rl_hp_other);
    out->rl_unknown_helper = telemetry_load(&g_rl_unknown_helper);
    out->rl_overflow = telemetry_load(&g_rl_overflow);
    out->rl_desync = telemetry_load(&g_rl_desync);
    out->rl_collision = telemetry_load(&g_rl_collision);
    out->rl_roundtrip = telemetry_load(&g_rl_roundtrip);
    out->store_skips = telemetry_load(&g_store_skips);
    out->bytes_loaded = telemetry_load(&g_bytes_loaded);
    out->bytes_stored = telemetry_load(&g_bytes_stored);
    out->compile_count = telemetry_load(&g_compile_count);
    out->translation_count = telemetry_load(&g_translation_count);
    out->distinct_translation_count = telemetry_load(&g_distinct_translation_count);
    out->dispatches = telemetry_load(&g_dispatches);
    out->blocks = telemetry_load(&g_blocks);
    out->steps = telemetry_load(&g_steps);
}

int hb_contract_telemetry_format_summary(char* buf, size_t size,
                                         const hb_contract_telemetry_counts_t* counts) {
    if (!buf || !size || !counts) return -1;
    return snprintf(buf, size,
                    "macrunner-hb-translation-cache-summary: "
                    "open_ok=%llu open_fail=%llu hits=%llu misses=%llu stores=%llu "
                    "store_skips=%llu store_skip_multi=%llu store_skip_unmatched=%llu "
                    "mh_toomany=%llu mh_widearg=%llu mh_unkhelper=%llu mh_argshape=%llu "
                    "reloc_blocks=%llu reloc_sites=%llu reloc_overflow=%llu "
                    "rl_stores=%llu rl_patched=%llu rl_literal=%llu rl_highhalf=%llu "
                    "rl_x23val=%llu promo_compiles=%llu "
                    "rl_hostptr=%llu hp_succ=%llu hp_pred=%llu hp_succinstr=%llu hp_other=%llu "
                    "rl_unkhelper=%llu rl_overflow=%llu "
                    "rl_desync=%llu rl_collision=%llu rl_roundtrip=%llu "
                    "bytes_loaded=%llu bytes_stored=%llu "
                    "compile_count=%llu translation_count=%llu "
                    "distinct_translation_count=%llu dispatches=%llu blocks=%llu steps=%llu\n",
                    (unsigned long long)counts->open_ok,
                    (unsigned long long)counts->open_fail,
                    (unsigned long long)counts->hits,
                    (unsigned long long)counts->misses,
                    (unsigned long long)counts->stores,
                    (unsigned long long)counts->store_skips,
                    (unsigned long long)counts->store_skip_multi_helper,
                    (unsigned long long)counts->store_skip_unmatched,
                    (unsigned long long)counts->mh_too_many,
                    (unsigned long long)counts->mh_wide_arg,
                    (unsigned long long)counts->mh_unknown_helper,
                    (unsigned long long)counts->mh_arg_shape,
                    (unsigned long long)counts->reloc_blocks,
                    (unsigned long long)counts->reloc_sites,
                    (unsigned long long)counts->reloc_overflow,
                    (unsigned long long)counts->rl_stores,
                    (unsigned long long)counts->rl_patched_sites,
                    (unsigned long long)counts->rl_literal_sites,
                    (unsigned long long)counts->rl_highhalf_sites,
                    (unsigned long long)counts->rl_x23_value,
                    (unsigned long long)counts->promo_compiles,
                    (unsigned long long)counts->rl_hostptr,
                    (unsigned long long)counts->rl_hp_succ,
                    (unsigned long long)counts->rl_hp_pred,
                    (unsigned long long)counts->rl_hp_succ_instr,
                    (unsigned long long)counts->rl_hp_other,
                    (unsigned long long)counts->rl_unknown_helper,
                    (unsigned long long)counts->rl_overflow,
                    (unsigned long long)counts->rl_desync,
                    (unsigned long long)counts->rl_collision,
                    (unsigned long long)counts->rl_roundtrip,
                    (unsigned long long)counts->bytes_loaded,
                    (unsigned long long)counts->bytes_stored,
                    (unsigned long long)counts->compile_count,
                    (unsigned long long)counts->translation_count,
                    (unsigned long long)counts->distinct_translation_count,
                    (unsigned long long)counts->dispatches,
                    (unsigned long long)counts->blocks,
                    (unsigned long long)counts->steps);
}

int hb_contract_telemetry_emit_summary(FILE* stream) {
    hb_contract_telemetry_counts_t counts;
    /* 4096, not 1024: the line now carries 32 %llu fields, each up to 20 digits plus a label.
     * The old 1024 was sized for 20 fields and the nine rl_* counters would have overrun it.
     *
     * That mattered more than arithmetic usually does here, because the overflow branch below
     * used to `return 0` — the whole measurement vanished with no error, which is precisely the
     * failure this file exists to prevent, and it had already cost two A/Bs. It now says so on
     * stderr instead of disappearing. A truncation is a bug in this file, not a run condition,
     * so it should be impossible to read the log and not notice. */
    char line[4096];
    int expected = 0;
    int n;

    if (!stream || !hb_contract_telemetry_enabled()) return 0;
    if (!__atomic_compare_exchange_n(&g_summary_emitted, &expected, 1, false,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return 0;
    hb_contract_telemetry_snapshot(&counts);
    n = hb_contract_telemetry_format_summary(line, sizeof(line), &counts);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        fprintf(stream,
                "macrunner-hb-translation-cache-summary-TRUNCATED: need=%d have=%zu"
                " (raise the buffer in hb_contract_telemetry.c)\n",
                n, sizeof(line));
        fflush(stream);
        return 0;
    }
    fputs(line, stream);
    fflush(stream);
    return 1;
}

/* MacRunner 2026-07-29: emit a PROGRESS line every N compiles.
 *
 * The summary above is one-shot and runs from atexit, so a run killed by its harness timeout
 * emits nothing at all. Every cache A/B today produced "сводки нет" instead of numbers, because
 * Hollow Knight's Mono load phase alone is 232 s and no run survives to a clean exit inside a
 * measurement-sized budget. Tying progress to compile count rather than to a timer keeps it
 * free of threads and signal-safety questions, and makes the cadence proportional to the work
 * being measured. */
#define HB_TELEMETRY_PROGRESS_EVERY 5000

static void hb_contract_telemetry_maybe_emit_progress(void) {
    static uint64_t next_at = HB_TELEMETRY_PROGRESS_EVERY;
    hb_contract_telemetry_counts_t counts;
    char line[4096];  /* see the note in emit_summary: too small means silent nothing */
    uint64_t compiles;
    int n;

    if (!hb_contract_telemetry_enabled()) return;
    compiles = telemetry_load(&g_compile_count);
    if (compiles < __atomic_load_n(&next_at, __ATOMIC_RELAXED)) return;
    __atomic_store_n(&next_at, compiles + HB_TELEMETRY_PROGRESS_EVERY, __ATOMIC_RELAXED);

    hb_contract_telemetry_snapshot(&counts);
    n = hb_contract_telemetry_format_summary(line, sizeof(line), &counts);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        fprintf(stderr, "macrunner-hb-translation-cache-progress-TRUNCATED: need=%d have=%zu\n",
                n, sizeof(line));
        fflush(stderr);
        return;
    }
    /* Same fields, different marker, so a partial reading can never be mistaken for the final
     * one — today's worst hours came from reading numbers that meant something else. */
    fputs("macrunner-hb-translation-cache-progress: ", stderr);
    fputs(line + sizeof("macrunner-hb-translation-cache-summary: ") - 1, stderr);
    fflush(stderr);
}

static void hb_contract_telemetry_atexit_summary(void) {
    (void)hb_contract_telemetry_emit_summary(stderr);
}

void hb_contract_telemetry_register_atexit(void) {
    int expected = 0;
    if (!hb_contract_telemetry_enabled()) return;
    if (__atomic_compare_exchange_n(&g_atexit_registered, &expected, 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        atexit(hb_contract_telemetry_atexit_summary);
    }
}

void hb_contract_telemetry_reset_for_test(void) {
    __atomic_store_n(&g_open_ok, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_open_fail, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hits, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_misses, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_stores, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_store_skips, 0, __ATOMIC_RELAXED);
    /* These were missing, so a test that reset and re-ran saw the previous case's rejection
     * counts added to its own. Every counter this file owns belongs here. */
    __atomic_store_n(&g_store_skip_multi, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_store_skip_unmatched, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mh_too_many, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mh_wide_arg, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mh_unknown_helper, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_mh_arg_shape, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_reloc_blocks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_reloc_sites, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_reloc_overflow, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_stores, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_literal_sites, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_highhalf_sites, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_patched_sites, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_x23_value, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_promo_compiles, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_hostptr, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_unknown_helper, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_overflow, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_desync, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_collision, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_rl_roundtrip, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bytes_loaded, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_bytes_stored, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_compile_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_translation_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_distinct_translation_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_dispatches, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_blocks, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_steps, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_trace_enabled_cached, -1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_summary_emitted, 0, __ATOMIC_RELAXED);
}
