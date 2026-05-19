#ifndef HB_FAULT_H
#define HB_FAULT_H

#include "hb_result.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HB_FAULT_CLASS_OUTSIDE = 0,
    HB_FAULT_CLASS_EXECUTE_GUEST = 1,
    HB_FAULT_CLASS_NATIVE_BLOCK = 2,
    HB_FAULT_CLASS_DIRTY_WRITE = 3,
    HB_FAULT_CLASS_STALE_BLOCK = 4
} hb_fault_class_t;

typedef struct {
    uint64_t native_start;
    uint64_t native_end;
    uint64_t module_id;
    uint64_t guest_start;
    uint64_t guest_end;
    uint32_t page_generation;
    bool valid;
} hb_fault_block_t;

typedef struct {
    hb_fault_block_t* blocks;
    size_t count;
    size_t capacity;
    uint32_t global_generation;
    uint64_t lazy_translations;
    uint64_t dirty_invalidations;
    uint64_t stale_rejections;
} hb_fault_dispatcher_t;

typedef struct {
    hb_fault_class_t fault_class;
    uint64_t module_id;
    uint64_t guest_pc;
    uint32_t page_generation;
} hb_fault_result_t;

hb_result_t hb_fault_dispatcher_init(hb_fault_dispatcher_t* dispatcher, size_t capacity);
void hb_fault_dispatcher_destroy(hb_fault_dispatcher_t* dispatcher);
hb_result_t hb_fault_register_block(hb_fault_dispatcher_t* dispatcher, uint64_t native_start, uint64_t native_end, uint64_t module_id, uint64_t guest_start, uint64_t guest_end);
hb_result_t hb_fault_classify(hb_fault_dispatcher_t* dispatcher, uint64_t pc, bool is_execute, bool is_write, hb_fault_result_t* out);
hb_result_t hb_fault_mark_dirty(hb_fault_dispatcher_t* dispatcher, uint64_t module_id, uint64_t guest_page);
hb_result_t hb_fault_unload_module(hb_fault_dispatcher_t* dispatcher, uint64_t module_id);
hb_result_t hb_fault_lazy_translate(hb_fault_dispatcher_t* dispatcher, uint64_t module_id, uint64_t guest_pc, uint64_t native_start, uint64_t native_end);

#ifdef __cplusplus
}
#endif

#endif
