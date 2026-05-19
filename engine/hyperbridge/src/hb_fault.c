#include "hb_fault.h"
#include <stdlib.h>
#include <string.h>

static uint64_t page_base(uint64_t addr) { return addr & ~0xfffULL; }

hb_result_t hb_fault_dispatcher_init(hb_fault_dispatcher_t* dispatcher, size_t capacity) {
    if (!dispatcher || capacity == 0) return HB_ERR_INVALID_ARG;
    memset(dispatcher, 0, sizeof(*dispatcher));
    dispatcher->blocks = calloc(capacity, sizeof(hb_fault_block_t));
    if (!dispatcher->blocks) return HB_ERR_OUT_OF_MEMORY;
    dispatcher->capacity = capacity;
    dispatcher->global_generation = 1;
    return HB_OK;
}

void hb_fault_dispatcher_destroy(hb_fault_dispatcher_t* dispatcher) {
    if (!dispatcher) return;
    free(dispatcher->blocks);
    memset(dispatcher, 0, sizeof(*dispatcher));
}

hb_result_t hb_fault_register_block(hb_fault_dispatcher_t* d, uint64_t native_start, uint64_t native_end, uint64_t module_id, uint64_t guest_start, uint64_t guest_end) {
    if (!d || native_start >= native_end || guest_start >= guest_end) return HB_ERR_INVALID_ARG;
    if (d->count >= d->capacity) return HB_ERR_OUT_OF_MEMORY;
    d->blocks[d->count++] = (hb_fault_block_t){native_start, native_end, module_id, guest_start, guest_end, d->global_generation, true};
    return HB_OK;
}

hb_result_t hb_fault_classify(hb_fault_dispatcher_t* d, uint64_t pc, bool is_execute, bool is_write, hb_fault_result_t* out) {
    if (!d || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->fault_class = HB_FAULT_CLASS_OUTSIDE;
    for (size_t i = 0; i < d->count; i++) {
        hb_fault_block_t* b = &d->blocks[i];
        if (!b->valid) continue;
        if (pc >= b->native_start && pc < b->native_end) {
            out->fault_class = b->page_generation == d->global_generation ? HB_FAULT_CLASS_NATIVE_BLOCK : HB_FAULT_CLASS_STALE_BLOCK;
            out->module_id = b->module_id;
            out->guest_pc = b->guest_start + (pc - b->native_start);
            out->page_generation = b->page_generation;
            if (out->fault_class == HB_FAULT_CLASS_STALE_BLOCK) d->stale_rejections++;
            return HB_OK;
        }
        if (pc >= b->guest_start && pc < b->guest_end) {
            out->fault_class = is_write ? HB_FAULT_CLASS_DIRTY_WRITE : (is_execute ? HB_FAULT_CLASS_EXECUTE_GUEST : HB_FAULT_CLASS_OUTSIDE);
            out->module_id = b->module_id;
            out->guest_pc = pc;
            out->page_generation = d->global_generation;
            return HB_OK;
        }
    }
    return HB_OK;
}

hb_result_t hb_fault_mark_dirty(hb_fault_dispatcher_t* d, uint64_t module_id, uint64_t guest_page) {
    if (!d) return HB_ERR_INVALID_ARG;
    d->global_generation++;
    d->dirty_invalidations++;
    guest_page = page_base(guest_page);
    for (size_t i = 0; i < d->count; i++) {
        hb_fault_block_t* b = &d->blocks[i];
        if (b->module_id == module_id && page_base(b->guest_start) == guest_page) b->page_generation = d->global_generation - 1;
    }
    return HB_OK;
}

hb_result_t hb_fault_unload_module(hb_fault_dispatcher_t* d, uint64_t module_id) {
    if (!d) return HB_ERR_INVALID_ARG;
    for (size_t i = 0; i < d->count; i++) if (d->blocks[i].module_id == module_id) d->blocks[i].valid = false;
    return HB_OK;
}

hb_result_t hb_fault_lazy_translate(hb_fault_dispatcher_t* d, uint64_t module_id, uint64_t guest_pc, uint64_t native_start, uint64_t native_end) {
    if (!d) return HB_ERR_INVALID_ARG;
    d->lazy_translations++;
    return hb_fault_register_block(d, native_start, native_end, module_id, guest_pc, guest_pc + (native_end - native_start));
}
