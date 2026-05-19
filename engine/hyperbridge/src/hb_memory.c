#include "hb_memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

static size_t page_align(size_t sz) {
    return (sz + 4095) & ~4095;
}

static bool trace_bad_native_write_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_TRACE_NATIVE_WRITES");
    return val && val[0] && val[0] != '0';
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

hb_memory_t* hb_memory_create(size_t max_size) {
    hb_memory_t* mem = calloc(1, sizeof(hb_memory_t));
    if (!mem) return NULL;
    mem->max_size = max_size;
    return mem;
}

void hb_memory_destroy(hb_memory_t* mem) {
    if (!mem) return;
    hb_region_t* r = mem->regions;
    while (r) { hb_region_t* n = r->next; if (r->allocated) munmap((void*)(uintptr_t)r->base, r->size); free(r); r = n; }
    free(mem);
}

hb_result_t hb_memory_map(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    if (!mem) return HB_ERR_INVALID_ARG;
    size_t alloc_size = page_align(size);
    hb_region_t* r = calloc(1, sizeof(hb_region_t));
    if (!r) return HB_ERR_OUT_OF_MEMORY;
    if (base == 0) {
        int prot = PROT_NONE;
        if (perm & HB_PERM_READ) prot |= PROT_READ;
        if (perm & HB_PERM_WRITE) prot |= PROT_WRITE;
        if (perm & HB_PERM_EXEC) prot |= PROT_EXEC;
        void* p = mmap(NULL, alloc_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { free(r); return HB_ERR_OUT_OF_MEMORY; }
        r->base = (hb_gva_t)(uintptr_t)p;
        r->allocated = true;
    } else {
        r->base = base;
        r->allocated = false;
    }
    r->size = alloc_size;
    r->perm = perm;
    r->next = mem->regions; mem->regions = r;
    mem->total_size += alloc_size;
    return HB_OK;
}

hb_result_t hb_memory_unmap(hb_memory_t* mem, hb_gva_t base) {
    if (!mem) return HB_ERR_INVALID_ARG;
    hb_region_t** p = &mem->regions;
    while (*p) {
        if ((*p)->base == base) {
            hb_region_t* d = *p;
            *p = d->next;
            mem->total_size -= d->size;
            if (d->allocated) munmap((void*)(uintptr_t)d->base, d->size);
            free(d);
            return HB_OK;
        }
        p = &(*p)->next;
    }
    return HB_ERR_NOT_FOUND;
}

hb_result_t hb_memory_protect(hb_memory_t* mem, hb_gva_t base, size_t size, hb_perm_t perm) {
    if (!mem) return HB_ERR_INVALID_ARG;
    hb_region_t* r = hb_memory_find_region(mem, base);
    if (!r) return HB_ERR_NOT_FOUND;

    /*
     * Non-allocated regions describe the live process address space.  Their
     * host VM protection is owned by Wine/macOS; HyperBridge permissions are
     * metadata used by the x64 interpreter.  Applying HB_PERM_EXEC back to the
     * host with mprotect() would make guest x64 code executable by the ARM64
     * CPU again, bypassing the signal bridge.
     */
    if (!r->allocated) {
        r->perm = perm;
        (void)size;
        return HB_OK;
    }

    int prot = PROT_NONE;
    if (perm & HB_PERM_READ) prot |= PROT_READ;
    if (perm & HB_PERM_WRITE) prot |= PROT_WRITE;
    if (perm & HB_PERM_EXEC) prot |= PROT_EXEC;
    if (mprotect((void*)(uintptr_t)r->base, r->size, prot) != 0) return HB_ERR_MEMORY_FAULT;
    r->perm = perm;
    (void)size;
    return HB_OK;
}

hb_result_t hb_memory_read(hb_memory_t* mem, hb_gva_t addr, void* out, size_t size) {
    hb_region_t* region;
    if (!mem || !out) return HB_ERR_INVALID_ARG;
    if (!hb_memory_can_read(mem, addr, size)) {
        if (hb_memory_can_read_span(mem, addr, size)) {
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
    region = hb_memory_find_region(mem, addr);
#ifdef __APPLE__
    if (region && !region->allocated) {
        mach_vm_size_t copied = 0;
        kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)addr,
                                                  (mach_vm_size_t)size,
                                                  (mach_vm_address_t)(uintptr_t)out, &copied);
        return (kr == KERN_SUCCESS && copied == size) ? HB_OK : HB_ERR_MEMORY_FAULT;
    }
#endif
    memcpy(out, (void*)(uintptr_t)addr, size);
    return HB_OK;
}

hb_result_t hb_memory_write(hb_memory_t* mem, hb_gva_t addr, const void* in, size_t size) {
    hb_region_t* region;
    if (!mem || !in) return HB_ERR_INVALID_ARG;
    trace_bad_native_write("hb_memory_write", addr, in, size);
    if (!hb_memory_can_write(mem, addr, size)) {
        if (hb_memory_can_write_span(mem, addr, size)) {
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
        return HB_ERR_MEMORY_FAULT;
    }
    region = hb_memory_find_region(mem, addr);
#ifdef __APPLE__
    if (region && !region->allocated) {
        kern_return_t kr = mach_vm_write(mach_task_self(), (mach_vm_address_t)addr,
                                         (vm_offset_t)(uintptr_t)in,
                                         (mach_msg_type_number_t)size);
        return kr == KERN_SUCCESS ? HB_OK : HB_ERR_MEMORY_FAULT;
    }
#endif
    memcpy((void*)(uintptr_t)addr, in, size);
    return HB_OK;
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
    for (hb_region_t* r = mem->regions; r; r = r->next) {
        if (addr >= r->base && addr < r->base + r->size) return r;
    }
    return NULL;
}

static bool check_perm(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p) {
    hb_region_t* r = hb_memory_find_region(mem, addr);
    if (!r) return false;
    if (addr + size > r->base + r->size) return false;
    return (r->perm & p) != 0;
}

static bool check_perm_span(hb_memory_t* mem, hb_gva_t addr, size_t size, hb_perm_t p) {
    hb_gva_t cur = addr;
    size_t remaining = size;

    if (!mem) return false;
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
