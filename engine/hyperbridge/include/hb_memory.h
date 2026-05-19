#ifndef HB_MEMORY_H
#define HB_MEMORY_H

#include "hb_result.h"
#include "hb_context.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory permissions */
typedef enum {
    HB_PERM_NONE = 0,
    HB_PERM_READ = 1 << 0,
    HB_PERM_WRITE = 1 << 1,
    HB_PERM_EXEC = 1 << 2
} hb_perm_t;

/* Memory region */
typedef struct hb_region {
    hb_gva_t base;
    size_t size;
    hb_perm_t perm;
    bool is_stack;
    bool is_heap;
    bool is_guard;
    bool allocated; /* true if hb_memory_map allocated backing memory */
    struct hb_region* next;
} hb_region_t;

/* Memory sandbox */
typedef struct hb_memory {
    hb_region_t* regions;
    size_t total_size;
    size_t max_size;
    hb_gva_t stack_top;
    hb_gva_t stack_bottom;
    hb_gva_t heap_base;
    size_t heap_size;

    hb_result_t (*special_read)(void* user, hb_gva_t addr, void* out, size_t size);
    hb_result_t (*special_write)(void* user, hb_gva_t addr, const void* in, size_t size);
    void* special_user;
} hb_memory_t;

hb_memory_t* hb_memory_create(size_t max_size);
void hb_memory_destroy(hb_memory_t* mem);

hb_result_t hb_memory_map(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);
hb_result_t hb_memory_unmap(hb_memory_t* mem, hb_gva_t base);
hb_result_t hb_memory_protect(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);

hb_result_t hb_memory_read(hb_memory_t* mem, hb_gva_t addr, void* out, size_t size);
hb_result_t hb_memory_write(hb_memory_t* mem, hb_gva_t addr, const void* in, size_t size);
void hb_memory_set_special_handlers(hb_memory_t* mem,
                                    hb_result_t (*read_fn)(void* user, hb_gva_t addr, void* out, size_t size),
                                    hb_result_t (*write_fn)(void* user, hb_gva_t addr, const void* in, size_t size),
                                    void* user);

hb_result_t hb_memory_read_u8(hb_memory_t* mem, hb_gva_t addr, uint8_t* out);
hb_result_t hb_memory_read_u16(hb_memory_t* mem, hb_gva_t addr, uint16_t* out);
hb_result_t hb_memory_read_u32(hb_memory_t* mem, hb_gva_t addr, uint32_t* out);
hb_result_t hb_memory_read_u64(hb_memory_t* mem, hb_gva_t addr, uint64_t* out);

hb_result_t hb_memory_write_u8(hb_memory_t* mem, hb_gva_t addr, uint8_t val);
hb_result_t hb_memory_write_u16(hb_memory_t* mem, hb_gva_t addr, uint16_t val);
hb_result_t hb_memory_write_u32(hb_memory_t* mem, hb_gva_t addr, uint32_t val);
hb_result_t hb_memory_write_u64(hb_memory_t* mem, hb_gva_t addr, uint64_t val);

hb_result_t hb_memory_fetch(hb_memory_t* mem, hb_gva_t addr, uint8_t* out);

hb_region_t* hb_memory_find_region(hb_memory_t* mem, hb_gva_t addr);
bool hb_memory_can_read(hb_memory_t* mem, hb_gva_t addr, size_t size);
bool hb_memory_can_write(hb_memory_t* mem, hb_gva_t addr, size_t size);
bool hb_memory_can_exec(hb_memory_t* mem, hb_gva_t addr, size_t size);
bool hb_memory_can_read_span(hb_memory_t* mem, hb_gva_t addr, size_t size);
bool hb_memory_can_write_span(hb_memory_t* mem, hb_gva_t addr, size_t size);

hb_result_t hb_memory_setup_stack(hb_memory_t* mem, hb_gva_t top, size_t size);
hb_result_t hb_memory_setup_heap(hb_memory_t* mem, hb_gva_t base, size_t size);

#ifdef __cplusplus
}
#endif

#endif
