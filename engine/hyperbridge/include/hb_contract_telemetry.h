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
