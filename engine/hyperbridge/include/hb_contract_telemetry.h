#ifndef HB_CONTRACT_TELEMETRY_H
#define HB_CONTRACT_TELEMETRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t open_ok;
    uint64_t open_fail;
    uint64_t hits;
    uint64_t misses;
    uint64_t stores;
    uint64_t store_skips;
    /* MacRunner 2026-07-29: why a compiled block could not be persisted. Measured on HK:
     * stores=117 against store_skips=40495, i.e. the persistent cache retains 0.3 % of what it
     * compiles, which is the dominant reason a cold start takes 476 s to the menu while Rosetta
     * — which translates ahead of time and keeps its result — reaches it in under 45 s.
     * native_blob_prepare_cache_store rejects any block whose code contains more than one
     * `blr x23`, so these counters size the fix before anyone rewrites the patcher. */
    uint64_t store_skip_multi_helper;   /* >1 helper call in the block */
    uint64_t store_skip_unmatched;      /* single helper, but arg1/helper mov not recognised */
    /* 2026-07-29: why a MULTI-helper block was still declined after the generalisation. The
     * first A/B cut multi-helper skips 118547 -> 43909; these four say what the remaining 44k
     * are, because each needs a different fix and guessing between them is how this project
     * loses days. */
    uint64_t mh_too_many;      /* more helper sites than the 16-site cap */
    uint64_t mh_wide_arg;      /* pointer moved into x2/x3/x4 somewhere in the block */
    uint64_t mh_unknown_helper;/* helper not in the cacheable set */
    uint64_t mh_arg_shape;     /* not exactly one recognised arg1 in a site's window */
    /* 2026-07-29: the relocation table itself, measured before anything is allowed to depend
     * on it. reloc_overflow > 0 means the 64-site cap is real and must be raised before the
     * store path switches from scanning to the table. */
    uint64_t reloc_blocks;     /* compiled blocks that produced a table */
    uint64_t reloc_sites;      /* total recorded sites across those blocks */
    uint64_t reloc_overflow;   /* blocks whose table overflowed */
    uint64_t bytes_loaded;
    uint64_t bytes_stored;
    uint64_t compile_count;
    uint64_t translation_count;
    uint64_t distinct_translation_count;
    uint64_t dispatches;
    uint64_t blocks;
    uint64_t steps;
} hb_contract_telemetry_counts_t;

int hb_contract_telemetry_enabled(void);
void hb_contract_telemetry_register_atexit(void);
int hb_contract_telemetry_emit_summary(FILE* stream);

void hb_contract_telemetry_record_open(bool ok);
void hb_contract_telemetry_record_cache_hit(uint64_t bytes_loaded);
void hb_contract_telemetry_record_cache_miss(void);
void hb_contract_telemetry_record_cache_store(uint64_t bytes_stored);
void hb_contract_telemetry_record_cache_store_skip(void);
void hb_contract_telemetry_record_cache_store_skip_multi(void);
void hb_contract_telemetry_record_cache_store_skip_unmatched(void);
void hb_contract_telemetry_record_mh_reason(int reason);
void hb_contract_telemetry_record_reloc(unsigned long sites, int overflow);
void hb_contract_telemetry_record_compile(void);
void hb_contract_telemetry_record_translation(bool distinct);
void hb_contract_telemetry_record_dispatch(uint64_t dispatches, uint64_t blocks, uint64_t steps);

void hb_contract_telemetry_snapshot(hb_contract_telemetry_counts_t* out);
int hb_contract_telemetry_format_summary(char* buf, size_t size,
                                         const hb_contract_telemetry_counts_t* counts);
void hb_contract_telemetry_reset_for_test(void);

#ifdef __cplusplus
}
#endif

#endif
