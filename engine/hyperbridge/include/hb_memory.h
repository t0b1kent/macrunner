#ifndef HB_MEMORY_H
#define HB_MEMORY_H

#include "hb_result.h"
#include "hb_context.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define HB_GUEST32_SIZE 0x100000000ULL
#define HB_MEMORY_HOT_CACHE_SLOTS 16

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
    void* host_base; /* backing address for private guest mappings */
    size_t size;
    hb_perm_t perm;
    uint64_t gen;
    uint32_t vma_flags;
    bool is_stack;
    bool is_heap;
    bool is_guard;
    bool is_guest32;
    bool allocated; /* true if hb_memory_map allocated backing memory */
    struct hb_region* next;
    struct hb_region* tree_left;
    struct hb_region* tree_right;
    uint32_t tree_prio;
} hb_region_t;

/* Memory sandbox */
typedef struct hb_memory {
    hb_region_t* regions;
    hb_region_t* region_tree;
    size_t total_size;
    size_t max_size;
    uint64_t generation;
    void* guest32_base;
    size_t guest32_size;
    bool guest32_owned;
    hb_gva_t stack_top;
    hb_gva_t stack_bottom;
    hb_gva_t heap_base;
    size_t heap_size;

    hb_result_t (*special_read)(void* user, hb_gva_t addr, void* out, size_t size);
    hb_result_t (*special_write)(void* user, hb_gva_t addr, const void* in, size_t size);
    void* special_user;
    /* Host-side fault recovery: when a live-VM access faults on a page the host
     * has reserved-but-not-committed (e.g. a Windows guard page below a guest
     * stack), this lets the embedder run virtual_handle_fault and commit it so
     * the access can be retried.  Returns true if the page may now be valid. */
    bool (*special_grow)(void* user, hb_gva_t addr);

    /* MRU region cache epoch. Slots are thread-local in hb_memory.c; hot_gen is
     * bumped on structural map changes so per-thread slots cannot outlive a
     * split/remove/rebuild. hot[] is kept zeroed for diagnostics/back-compat. */
    hb_region_t* hot[HB_MEMORY_HOT_CACHE_SLOTS];
    uint64_t     hot_gen;
} hb_memory_t;

hb_memory_t* hb_memory_create(size_t max_size);
void hb_memory_init_environment(void);
void hb_memory_destroy(hb_memory_t* mem);

hb_result_t hb_memory_map(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);
hb_result_t hb_memory_map_private(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);
/* Replace the metadata for an existing host-owned live range with one
 * authoritative, fully covered region.  This never changes host VM protection. */
hb_result_t hb_memory_sync_live_range(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);
hb_result_t hb_memory_unmap(hb_memory_t* mem, hb_gva_t base);
hb_result_t hb_memory_protect(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm);

hb_result_t hb_memory_guest32_reserve(hb_memory_t* mem);
void* hb_memory_guest32_base(const hb_memory_t* mem);
void* hb_memory_guest32_to_host(hb_memory_t* mem, uint32_t guest_addr);
hb_result_t hb_memory_guest32_map(hb_memory_t* mem, uint32_t base, size_t size, hb_perm_t perm);
hb_result_t hb_memory_guest32_unmap(hb_memory_t* mem, uint32_t base, size_t size);
hb_result_t hb_memory_guest32_protect(hb_memory_t* mem, uint32_t base, size_t size, hb_perm_t perm);
uint64_t hb_memory_generation(const hb_memory_t* mem);
uint64_t hb_memory_region_generation(hb_memory_t* mem, hb_gva_t addr);

hb_result_t hb_memory_read(hb_memory_t* mem, hb_gva_t addr, void* out, size_t size);
hb_result_t hb_memory_write(hb_memory_t* mem, hb_gva_t addr, const void* in, size_t size);
void* hb_memory_host_ptr(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t perm);
void hb_memory_set_special_handlers(hb_memory_t* mem,
                                    hb_result_t (*read_fn)(void* user, hb_gva_t addr, void* out, size_t size),
                                    hb_result_t (*write_fn)(void* user, hb_gva_t addr, const void* in, size_t size),
                                    void* user);
void hb_memory_set_grow_handler(hb_memory_t* mem, bool (*grow_fn)(void* user, hb_gva_t addr));

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
