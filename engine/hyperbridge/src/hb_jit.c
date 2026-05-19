#include "hb_codegen.h"
#include "hb_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifdef __APPLE__
#  ifndef MAP_JIT
#    define MAP_JIT 0x800
#  endif
#endif

hb_jit_buffer_t* hb_jit_buffer_create(size_t size) {
    hb_jit_buffer_t* buf = calloc(1, sizeof(hb_jit_buffer_t));
    if (!buf) return NULL;
    buf->size = size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
    flags |= MAP_JIT;
#endif
    buf->writable = mmap(NULL, size, PROT_READ | PROT_WRITE, flags, -1, 0);
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
    buf->executable = buf->writable; /* on Apple Silicon with Hardened Runtime this needs special handling */
    return buf;
}

void hb_jit_buffer_destroy(hb_jit_buffer_t* buf) {
    if (!buf) return;
    if (buf->writable && buf->writable != MAP_FAILED) munmap(buf->writable, buf->size);
    free(buf);
}

hb_result_t hb_jit_buffer_commit(hb_jit_buffer_t* buf) {
    if (!buf) return HB_ERR_INVALID_ARG;
    if (mprotect(buf->writable, buf->size, PROT_READ | PROT_EXEC) != 0) return HB_ERR_JIT_FAILED;
    buf->is_executable = true;
    __builtin___clear_cache((char*)buf->writable, (char*)buf->writable + buf->size);
    return HB_OK;
}

hb_result_t hb_jit_buffer_make_writable(hb_jit_buffer_t* buf) {
    if (!buf) return HB_ERR_INVALID_ARG;
    if (mprotect(buf->writable, buf->size, PROT_READ | PROT_WRITE) != 0) return HB_ERR_JIT_FAILED;
    buf->is_executable = false;
    return HB_OK;
}

hb_result_t hb_jit_buffer_make_executable(hb_jit_buffer_t* buf) {
    return hb_jit_buffer_commit(buf);
}

void hb_jit_buffer_flush_icache(hb_jit_buffer_t* buf) {
    if (!buf || !buf->writable) return;
    __builtin___clear_cache((char*)buf->writable, (char*)buf->writable + buf->size);
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
