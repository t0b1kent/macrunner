#include "hb_iat.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static uint64_t bridge_calls;

hb_result_t hb_iat_plan_init(hb_iat_rewrite_plan_t* plan, size_t capacity) {
    if (!plan || capacity == 0) return HB_ERR_INVALID_ARG;
    memset(plan, 0, sizeof(*plan));
    plan->entries = calloc(capacity, sizeof(hb_iat_rewrite_entry_t));
    if (!plan->entries) return HB_ERR_OUT_OF_MEMORY;
    plan->capacity = capacity;
    return HB_OK;
}

void hb_iat_plan_destroy(hb_iat_rewrite_plan_t* plan) {
    if (!plan) return;
    free(plan->entries);
    memset(plan, 0, sizeof(*plan));
}

hb_result_t hb_iat_plan_add(hb_iat_rewrite_plan_t* plan, const hb_iat_rewrite_entry_t* entry) {
    if (!plan || !entry || !entry->slot) return HB_ERR_INVALID_ARG;
    if (plan->count >= plan->capacity) return HB_ERR_OUT_OF_MEMORY;
    plan->entries[plan->count++] = *entry;
    return HB_OK;
}

hb_result_t hb_iat_plan_trace_jsonl(const hb_iat_rewrite_plan_t* plan, const char* path) {
    if (!plan || !path) return HB_ERR_INVALID_ARG;
    FILE* fp = fopen(path, "a");
    if (!fp) return HB_ERR_NOT_FOUND;
    for (size_t i = 0; i < plan->count; i++) {
        const hb_iat_rewrite_entry_t* e = &plan->entries[i];
        fprintf(fp, "{\"event\":\"plan\",\"module_id\":%llu,\"iat_rva\":%llu,\"dll\":\"%s\",\"import\":\"%s\",\"flags\":%u}\n",
                (unsigned long long)e->module_id, (unsigned long long)e->iat_rva, e->dll_name, e->import_name, e->flags);
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    return HB_OK;
}

hb_result_t hb_iat_plan_apply(hb_iat_rewrite_plan_t* plan, bool dry_run, hb_iat_stats_t* stats) {
    if (!plan) return HB_ERR_INVALID_ARG;
    for (size_t i = 0; i < plan->count; i++) {
        hb_iat_rewrite_entry_t* e = &plan->entries[i];
        if ((e->flags & HB_IAT_FLAG_ALLOWLISTED) == 0 || !e->bridge_target) {
            if (stats) stats->denied_count++;
            continue;
        }
        e->original_target = *e->slot;
        if (!dry_run) {
            *e->slot = e->bridge_target;
            __builtin___clear_cache((char*)e->slot, (char*)e->slot + sizeof(void*));
            e->flags |= HB_IAT_FLAG_APPLIED;
        }
        if (stats) stats->rewritten_count++;
    }
    plan->applied = !dry_run;
    return HB_OK;
}

hb_result_t hb_iat_plan_rollback(hb_iat_rewrite_plan_t* plan, hb_iat_stats_t* stats) {
    if (!plan) return HB_ERR_INVALID_ARG;
    for (size_t i = 0; i < plan->count; i++) {
        hb_iat_rewrite_entry_t* e = &plan->entries[i];
        if ((e->flags & HB_IAT_FLAG_APPLIED) != 0) {
            *e->slot = e->original_target;
            e->flags &= ~HB_IAT_FLAG_APPLIED;
            if (stats) stats->rollback_count++;
        }
    }
    plan->applied = false;
    return HB_OK;
}

uint64_t hb_call_import(uint64_t module_id, uint64_t import_id, void* guest_context) {
    (void)module_id;
    (void)import_id;
    (void)guest_context;
    bridge_calls++;
    return bridge_calls;
}
