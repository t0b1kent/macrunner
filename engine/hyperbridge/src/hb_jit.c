#include "hb_codegen.h"
#include "hb_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#endif
#if defined(__APPLE__) && defined(__MACH__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#ifdef __APPLE__
#  ifndef MAP_JIT
#    define MAP_JIT 0x800
#  endif
#endif

#define HB_JIT_BUFFER_MAGIC 0x48424a4954425546ull /* HBJITBUF */

static bool force_jit_verify_failure(void) {
    const char* value = getenv("MACRUNNER_HB_TEST_FORCE_JIT_VERIFY_FAIL");
    return value && value[0] && value[0] != '0';
}

static bool jit_addr_has_prot(uintptr_t p, int required) {
    if (!p) return false;
#if defined(__APPLE__) && defined(__MACH__)
    mach_vm_address_t addr = (mach_vm_address_t)p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object_name = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &addr, &size,
                                      VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&info,
                                      &count, &object_name);
    if (object_name != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), object_name);
    }
    if (kr != KERN_SUCCESS) return false;
    if (p < (uintptr_t)addr || p >= (uintptr_t)(addr + size)) return false;
    return (info.protection & required) == required;
#else
    (void)required;
    return true;
#endif
}

static bool jit_range_has_prot(const void* ptr, size_t size, int required) {
    uintptr_t start = (uintptr_t)ptr;
    if (!start || size == 0) return false;
    uintptr_t end = start + size - 1;
    if (end < start) return false;
    return jit_addr_has_prot(start, required) && jit_addr_has_prot(end, required);
}

static bool hb_jit_buffer_is_valid(const hb_jit_buffer_t* buf) {
    if (!buf) return false;
    if (!jit_range_has_prot(buf, sizeof(*buf), PROT_READ)) return false;
    return buf->magic == HB_JIT_BUFFER_MAGIC;
}

static hb_result_t hb_jit_buffer_invalid(const char* fn, const hb_jit_buffer_t* buf) {
    fprintf(stderr, "macrunner-hb-jit-buffer-invalid: fn=%s buf=%p caller=%p\n",
            fn ? fn : "unknown", (const void*)buf, __builtin_return_address(0));
    fflush(stderr);
    return HB_ERR_INVALID_ARG;
}

hb_jit_buffer_t* hb_jit_buffer_create(size_t size) {
    hb_jit_buffer_t* buf = calloc(1, sizeof(hb_jit_buffer_t));
    if (!buf) return NULL;
    buf->size = size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
    flags |= MAP_JIT;
#endif
    buf->dirty_start = 0;
    buf->writable = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
    if (buf->writable == MAP_FAILED) {
#ifdef __APPLE__
        /* Retry without MAP_JIT if hardened runtime lacks JIT entitlement */
        buf->writable = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf->writable == MAP_FAILED) {
            free(buf);
            return NULL;
        }
#else
        free(buf);
        return NULL;
#endif
    }
#if defined(__APPLE__) && defined(__aarch64__)
    if (flags & MAP_JIT) {
        buf->thread_jit_write_protect = true;
        pthread_jit_write_protect_np(0);
    }
#endif
    buf->executable = buf->writable; /* on Apple Silicon with Hardened Runtime this needs special handling */
    buf->magic = HB_JIT_BUFFER_MAGIC;
    return buf;
}

void hb_jit_buffer_destroy(hb_jit_buffer_t* buf) {
    if (!hb_jit_buffer_is_valid(buf)) return;
    buf->magic = 0;
    if (buf->writable && buf->writable != MAP_FAILED) munmap(buf->writable, buf->size);
    free(buf);
}

/* MacRunner: reuse the mapping for a fresh codegen round (no munmap/mmap).
 * Only the bump pointer is rewound; the next codegen overwrites from offset 0.
 * Make the arena writable so the next emit can write (mirrors make_writable). */
hb_result_t hb_jit_buffer_reset(hb_jit_buffer_t* buf) {
    if (!hb_jit_buffer_is_valid(buf)) return hb_jit_buffer_invalid("reset", buf);
    buf->used = 0;
    buf->dirty_start = 0;
#if defined(__APPLE__) && defined(__aarch64__)
    if (buf->thread_jit_write_protect) {
        pthread_jit_write_protect_np(0);
        buf->is_executable = false;
        return HB_OK;
    }
#endif
    if (mprotect(buf->writable, buf->size, PROT_READ | PROT_WRITE) != 0) return HB_ERR_JIT_FAILED;
    buf->is_executable = false;
    return HB_OK;
}

hb_result_t hb_jit_buffer_commit(hb_jit_buffer_t* buf) {
    size_t dirty_start, dirty_end;
    if (!hb_jit_buffer_is_valid(buf)) return hb_jit_buffer_invalid("commit", buf);
    buf->is_executable = false;
    dirty_start = buf->dirty_start <= buf->used ? buf->dirty_start : 0;
    dirty_end = buf->used <= buf->size ? buf->used : buf->size;
    if (dirty_end > dirty_start)
        __builtin___clear_cache((char*)buf->writable + dirty_start,
                                (char*)buf->writable + dirty_end);
    buf->dirty_start = buf->used;
    if (force_jit_verify_failure()) return HB_ERR_JIT_FAILED;
#if defined(__APPLE__) && defined(__aarch64__)
    if (buf->thread_jit_write_protect) {
        pthread_jit_write_protect_np(1);
        buf->is_executable = true;
        return HB_OK;
    }
#endif
    if (mprotect(buf->writable, buf->size, PROT_READ | PROT_EXEC) != 0) return HB_ERR_JIT_FAILED;
    if (!jit_range_has_prot(buf->writable, buf->size, PROT_EXEC)) {
        (void)mprotect(buf->writable, buf->size, PROT_READ | PROT_WRITE);
        return HB_ERR_JIT_FAILED;
    }
    buf->is_executable = true;
    return HB_OK;
}

hb_result_t hb_jit_buffer_make_writable(hb_jit_buffer_t* buf) {
    if (!hb_jit_buffer_is_valid(buf)) return hb_jit_buffer_invalid("make-writable", buf);
#if defined(__APPLE__) && defined(__aarch64__)
    if (buf->thread_jit_write_protect) {
        pthread_jit_write_protect_np(0);
        buf->dirty_start = buf->used;
        buf->is_executable = false;
        return HB_OK;
    }
#endif
    if (mprotect(buf->writable, buf->size, PROT_READ | PROT_WRITE) != 0) return HB_ERR_JIT_FAILED;
    buf->dirty_start = buf->used;
    buf->is_executable = false;
    return HB_OK;
}

hb_result_t hb_jit_buffer_make_executable(hb_jit_buffer_t* buf) {
    return hb_jit_buffer_commit(buf);
}

void hb_jit_buffer_flush_icache(hb_jit_buffer_t* buf) {
    if (!hb_jit_buffer_is_valid(buf) || !buf->writable) return;
    __builtin___clear_cache((char*)buf->writable, (char*)buf->writable + buf->used);
}

/* --- Codegen buffer --- */
hb_codegen_buffer_t* hb_codegen_buffer_create(size_t cap) {
    hb_codegen_buffer_t* buf = calloc(1, sizeof(hb_codegen_buffer_t));
    if (!buf) return NULL;
    buf->code = calloc(1, cap);
    if (!buf->code) { free(buf); return NULL; }
    buf->capacity = cap;
    return buf;
}

void hb_codegen_buffer_destroy(hb_codegen_buffer_t* buf) {
    if (!buf) return;
    free(buf->code);
    free(buf);
}

hb_result_t hb_codegen_buffer_append(hb_codegen_buffer_t* buf, const uint8_t* bytes, size_t len) {
    if (!buf || !bytes) return HB_ERR_INVALID_ARG;
    if (buf->size + len > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        while (new_cap < buf->size + len) new_cap *= 2;
        uint8_t* new_code = realloc(buf->code, new_cap);
        if (!new_code) return HB_ERR_OUT_OF_MEMORY;
        buf->code = new_code;
        buf->capacity = new_cap;
    }
    memcpy(buf->code + buf->size, bytes, len);
    buf->size += len;
    return HB_OK;
}
