#include "hb_runtime.h"
#include "hb_codegen.h"
#include "hb_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB_RUNTIME_PERSISTENT_CACHE_VERSION 15u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_MEM   0x01u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_STACK 0x02u
#define HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_SCAN 0x04u

#define HB_RUNTIME_CACHE_BLOCK_SENTINEL  0x48425254424c4b31ull /* HBRTBLK1 */
#define HB_RUNTIME_CACHE_HELPER_SENTINEL 0x48425254484c5000ull /* HBRTHLP + id */
#define HB_RUNTIME_CACHE_HELPER_MASK     0xfffffffffffff000ull
#define HB_RUNTIME_CACHE_INSTR_SENTINEL  0x48425254494e0000ull /* HBRTIN + index */
#define HB_RUNTIME_CACHE_INSTR_MASK      0xffffffffffff0000ull

extern void hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_xfg_dispatch_call(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void hb_jit_helper_exec_ir_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_load_cmp_jcc_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_cmp_setcc_ret_block(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_string_bsearch_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_freelist_fill_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_unity_u32_ptr_compare(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_bsearch_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_string_hash(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_string_equal(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_rowptr_entry(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_row_loop(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_row_entry(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_decode_col(hb_context_t* ctx, const hb_ir_block_t* block);
extern void hb_jit_helper_exec_mono_metadata_coded_index_search(hb_context_t* ctx, const hb_ir_block_t* block);

typedef struct hb_cached_helper_stub {
    size_t arg1_mov_off;
    size_t helper_mov_off;
    uint8_t helper_id;
    uint16_t instr_index;
    bool arg1_is_instr;
} hb_cached_helper_stub_t;

static uint64_t g_translation_cache_hits;
static uint64_t g_translation_cache_misses;
static uint64_t g_translation_cache_stores;
static uint64_t g_translation_cache_store_skips;
static uint64_t g_translation_cache_bytes_loaded;
static uint64_t g_translation_cache_bytes_stored;
static int g_translation_cache_atexit_registered;

static uint32_t jit_block_step_count(const hb_ir_block_t* block);
static void trace_jit_code_cache_full_once(hb_jit_runtime_t* rt,
                                           const char* reason,
                                           size_t needed);

static int translation_cache_trace_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_TRANSLATION_CACHE");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static void translation_cache_add_u64(uint64_t* dst, uint64_t val) {
    __atomic_fetch_add(dst, val, __ATOMIC_RELAXED);
}

static void translation_cache_trace_summary(void) {
    if (!translation_cache_trace_enabled()) return;
    fprintf(stderr,
            "macrunner-hb-translation-cache-summary: hits=%llu misses=%llu stores=%llu "
            "store_skips=%llu bytes_loaded=%llu bytes_stored=%llu\n",
            (unsigned long long)__atomic_load_n(&g_translation_cache_hits, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_translation_cache_misses, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_translation_cache_stores, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_translation_cache_store_skips, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_translation_cache_bytes_loaded, __ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&g_translation_cache_bytes_stored, __ATOMIC_RELAXED));
    fflush(stderr);
}

static void translation_cache_register_atexit(void) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&g_translation_cache_atexit_registered, &expected, 1,
                                    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        atexit(translation_cache_trace_summary);
    }
}

/* --- In-memory block cache helpers --- */
static size_t block_cache_hash(uint64_t addr) {
    return (size_t)((addr ^ (addr >> 32)) & (HB_BLOCK_CACHE_SIZE - 1));
}

static hb_block_cache_t* block_cache_create(void) {
    return calloc(1, sizeof(hb_block_cache_t));
}

static void block_cache_destroy(hb_block_cache_t* cache) {
    free(cache);
}

static hb_block_cache_entry_t* block_cache_find(hb_block_cache_t* cache, uint64_t addr) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) return NULL;
        if (cache->entries[probe].guest_addr == addr) return &cache->entries[probe];
    }
    return NULL;
}

static bool block_cache_is_full(const hb_block_cache_t* cache) {
    return cache && cache->count >= HB_BLOCK_CACHE_SIZE;
}

static hb_block_cache_entry_t* block_cache_put(hb_block_cache_t* cache, uint64_t addr, uint8_t* code,
                                               size_t size, uint32_t steps,
                                               const hb_ir_block_t* block, bool fused) {
    if (!cache) return NULL;
    size_t idx = block_cache_hash(addr);
    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        size_t probe = (idx + i) & (HB_BLOCK_CACHE_SIZE - 1);
        if (!cache->entries[probe].valid) {
            cache->entries[probe].guest_addr = addr;
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            cache->entries[probe].hit_count = 0;
            cache->entries[probe].block = block;
            cache->entries[probe].fused = fused;
            cache->entries[probe].valid = true;
            cache->count++;
            return &cache->entries[probe];
        }
        if (cache->entries[probe].guest_addr == addr) {
            /* Update existing entry */
            cache->entries[probe].native_code = code;
            cache->entries[probe].native_size = size;
            cache->entries[probe].steps = steps;
            cache->entries[probe].block = block;
            cache->entries[probe].fused = fused;
            return &cache->entries[probe];
        }
    }
    return NULL;
}

static int runtime_env_enabled(const char* name) {
    const char* val = getenv(name);
    return val && *val && *val != '0';
}

static int runtime_env_enabled_default_on(const char* name) {
    const char* val = getenv(name);
    return !val || !*val || *val != '0';
}

static uint8_t runtime_jit_flags(void) {
    uint8_t flags = 0;
    if (runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_MEM"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_MEM;
    if (runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_STACK"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_STACK;
    if (runtime_env_enabled("MACRUNNER_HB_JIT_DIRECT_MEM") ||
        runtime_env_enabled_default_on("MACRUNNER_HB_JIT_DIRECT_SCALAR_SCAN"))
        flags |= HB_RUNTIME_PERSISTENT_CACHE_FLAG_DIRECT_SCALAR_SCAN;
    return flags;
}

static bool native_blob_has_helper_call(const uint8_t* code, size_t size) {
    if (!code) return true;
    for (size_t i = 0; i + sizeof(uint32_t) <= size; i += sizeof(uint32_t)) {
        uint32_t insn;
        memcpy(&insn, code + i, sizeof(insn));
        if ((insn & 0xfffffc1fu) == 0xd63f0000u) return true; /* BLR Xn */
    }
    return false;
}

static void* helper_addr_for_cache_id(uint8_t id) {
    switch (id) {
        case 1: return (void*)hb_jit_helper_exec_ir_block;
        case 2: return (void*)hb_jit_helper_exec_load_cmp_jcc_block;
        case 3: return (void*)hb_jit_helper_exec_cmp_setcc_ret_block;
        case 4: return (void*)hb_jit_helper_exec_unity_string_bsearch_loop;
        case 5: return (void*)hb_jit_helper_exec_unity_freelist_fill_loop;
        case 6: return (void*)hb_jit_helper_exec_unity_u32_ptr_compare;
        case 7: return (void*)hb_jit_helper_exec_mono_metadata_bsearch_loop;
        case 8: return (void*)hb_jit_helper_exec_mono_string_hash;
        case 9: return (void*)hb_jit_helper_exec_mono_string_equal;
        case 10: return (void*)hb_jit_helper_exec_mono_metadata_rowptr_entry;
        case 11: return (void*)hb_jit_helper_exec_mono_metadata_decode_row_loop;
        case 12: return (void*)hb_jit_helper_exec_mono_metadata_decode_row_entry;
        case 13: return (void*)hb_jit_helper_exec_mono_metadata_decode_col;
        case 14: return (void*)hb_jit_helper_exec_mono_metadata_coded_index_search;
        case 15: return (void*)hb_jit_helper_exec_interp_ir;
        case 16: return (void*)hb_jit_helper_exec_load_operand_lazy;
        case 17: return (void*)hb_jit_helper_exec_store_operand_lazy;
        case 18: return (void*)hb_jit_helper_exec_call_operand;
        case 19: return (void*)hb_jit_helper_exec_xfg_dispatch_call;
        case 20: return (void*)hb_jit_helper_exec_jmp_operand;
        case 21: return (void*)hb_jit_helper_exec_cmp_test_operand_lazy;
        case 22: return (void*)hb_jit_helper_exec_binop_operand_lazy;
        case 23: return (void*)hb_jit_helper_exec_mul_div_operand;
        case 24: return (void*)hb_jit_helper_exec_double_shift_operand;
        case 25: return (void*)hb_jit_helper_exec_extend_operand_lazy;
        case 26: return (void*)hb_jit_helper_exec_mov_operand_lazy;
        case 27: return (void*)hb_jit_helper_exec_not_operand_lazy;
        case 28: return (void*)hb_jit_helper_exec_neg_operand_lazy;
        case 29: return (void*)hb_jit_helper_exec_bit_scan;
        case 30: return (void*)hb_jit_helper_exec_loop_branch;
        default: return NULL;
    }
}

static uint8_t helper_cache_id_for_addr(uint64_t addr) {
    for (uint8_t id = 1; id <= 30; id++) {
        if ((uintptr_t)helper_addr_for_cache_id(id) == (uintptr_t)addr) return id;
    }
    return 0;
}

static bool helper_cache_id_uses_instr_arg1(uint8_t id) {
    return id >= 15 && id <= 30;
}

static bool arm64_mov_imm64_at(const uint8_t* code, size_t size, size_t off,
                               int rd, uint64_t* value) {
    uint32_t insn[4];
    uint64_t v;
    if (!code || off + sizeof(insn) > size || rd < 0 || rd > 31) return false;
    memcpy(&insn[0], code + off, 4);
    memcpy(&insn[1], code + off + 4, 4);
    memcpy(&insn[2], code + off + 8, 4);
    memcpy(&insn[3], code + off + 12, 4);
    if ((insn[0] & 0xffe0001fu) != (0xd2800000u | (uint32_t)rd) ||
        (insn[1] & 0xffe0001fu) != (0xf2a00000u | (uint32_t)rd) ||
        (insn[2] & 0xffe0001fu) != (0xf2c00000u | (uint32_t)rd) ||
        (insn[3] & 0xffe0001fu) != (0xf2e00000u | (uint32_t)rd))
        return false;
    v = ((uint64_t)((insn[0] >> 5) & 0xffffu)) |
        ((uint64_t)((insn[1] >> 5) & 0xffffu) << 16) |
        ((uint64_t)((insn[2] >> 5) & 0xffffu) << 32) |
        ((uint64_t)((insn[3] >> 5) & 0xffffu) << 48);
    if (value) *value = v;
    return true;
}

static void arm64_patch_mov_imm64_at(uint8_t* code, size_t size, size_t off,
                                     int rd, uint64_t value) {
    uint32_t insn[4];
    if (!code || off + sizeof(insn) > size || rd < 0 || rd > 31) return;
    insn[0] = 0xd2800000u | (uint32_t)(((value >> 0) & 0xffffu) << 5) | (uint32_t)rd;
    insn[1] = 0xf2a00000u | (uint32_t)(((value >> 16) & 0xffffu) << 5) | (uint32_t)rd;
    insn[2] = 0xf2c00000u | (uint32_t)(((value >> 32) & 0xffffu) << 5) | (uint32_t)rd;
    insn[3] = 0xf2e00000u | (uint32_t)(((value >> 48) & 0xffffu) << 5) | (uint32_t)rd;
    memcpy(code + off, &insn[0], 4);
    memcpy(code + off + 4, &insn[1], 4);
    memcpy(code + off + 8, &insn[2], 4);
    memcpy(code + off + 12, &insn[3], 4);
}

static bool native_blob_single_arg_helper_stub(const uint8_t* code, size_t size,
                                               const hb_ir_block_t* block,
                                               bool canonical,
                                               hb_cached_helper_stub_t* out) {
    size_t blr_off = SIZE_MAX;
    size_t arg1_off = SIZE_MAX;
    uint8_t helper_id = 0;
    size_t arg1_count = 0;
    uint64_t helper_value = 0;
    uint16_t instr_index = 0;
    bool arg1_is_instr = false;

    if (!code || !size || !block) return false;
    for (size_t off = 0; off + 4 <= size; off += 4) {
        uint32_t insn;
        memcpy(&insn, code + off, sizeof(insn));
        if (insn == (0xd63f0000u | (23u << 5))) {
            if (blr_off != SIZE_MAX) return false;
            blr_off = off;
        }
    }
    if (blr_off == SIZE_MAX || blr_off < 16) return false;
    if (!arm64_mov_imm64_at(code, size, blr_off - 16, 23, &helper_value)) return false;
    if (canonical) {
        if ((helper_value & HB_RUNTIME_CACHE_HELPER_MASK) != HB_RUNTIME_CACHE_HELPER_SENTINEL)
            return false;
        helper_id = (uint8_t)(helper_value & 0xffu);
        if (!helper_addr_for_cache_id(helper_id)) return false;
    } else {
        helper_id = helper_cache_id_for_addr(helper_value);
        if (!helper_id) return false;
    }

    for (size_t off = 0; off + 16 <= size; off += 4) {
        uint64_t value = 0;
        if (arm64_mov_imm64_at(code, size, off, 2, &value) ||
            arm64_mov_imm64_at(code, size, off, 3, &value) ||
            arm64_mov_imm64_at(code, size, off, 4, &value))
            return false;
        if (arm64_mov_imm64_at(code, size, off, 1, &value)) {
            if (helper_cache_id_uses_instr_arg1(helper_id)) {
                if (canonical) {
                    if ((value & HB_RUNTIME_CACHE_INSTR_MASK) == HB_RUNTIME_CACHE_INSTR_SENTINEL) {
                        uint64_t idx = value & ~HB_RUNTIME_CACHE_INSTR_MASK;
                        if (idx >= block->instr_count || idx > UINT16_MAX) return false;
                        instr_index = (uint16_t)idx;
                        arg1_is_instr = true;
                        arg1_off = off;
                        arg1_count++;
                    }
                } else {
                    for (uint32_t idx = 0; idx < block->instr_count; idx++) {
                        if (value == (uint64_t)(uintptr_t)&block->instrs[idx]) {
                            instr_index = (uint16_t)idx;
                            arg1_is_instr = true;
                            arg1_off = off;
                            arg1_count++;
                            break;
                        }
                    }
                }
            } else {
                uint64_t expected = canonical ? HB_RUNTIME_CACHE_BLOCK_SENTINEL
                                              : (uint64_t)(uintptr_t)block;
                if (value == expected) {
                    arg1_off = off;
                    arg1_count++;
                }
            }
        }
    }
    if (arg1_count != 1 || arg1_off == SIZE_MAX) return false;
    if (out) {
        out->arg1_mov_off = arg1_off;
        out->helper_mov_off = blr_off - 16;
        out->helper_id = helper_id;
        out->instr_index = instr_index;
        out->arg1_is_instr = arg1_is_instr;
    }
    return true;
}

static bool native_blob_prepare_cache_store(const uint8_t* code, size_t size,
                                            const hb_ir_block_t* block,
                                            const uint8_t** out_code,
                                            uint8_t** owned_code) {
    hb_cached_helper_stub_t stub;
    uint8_t* patched;
    if (!code || !size || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;
    if (!native_blob_has_helper_call(code, size)) return true;
    if (!native_blob_single_arg_helper_stub(code, size, block, false, &stub))
        return false;
    patched = malloc(size);
    if (!patched) return false;
    memcpy(patched, code, size);
    arm64_patch_mov_imm64_at(patched, size, stub.arg1_mov_off, 1,
                             stub.arg1_is_instr
                                 ? (HB_RUNTIME_CACHE_INSTR_SENTINEL | stub.instr_index)
                                 : HB_RUNTIME_CACHE_BLOCK_SENTINEL);
    arm64_patch_mov_imm64_at(patched, size, stub.helper_mov_off, 23,
                             HB_RUNTIME_CACHE_HELPER_SENTINEL | stub.helper_id);
    *out_code = patched;
    *owned_code = patched;
    return true;
}

static bool native_blob_prepare_cache_load(const uint8_t* code, size_t size,
                                           const hb_ir_block_t* block,
                                           const uint8_t** out_code,
                                           uint8_t** owned_code) {
    hb_cached_helper_stub_t stub;
    uint8_t* patched;
    if (!code || !size || !out_code || !owned_code) return false;
    *out_code = code;
    *owned_code = NULL;
    if (!native_blob_has_helper_call(code, size)) return true;
    if (!native_blob_single_arg_helper_stub(code, size, block, true, &stub))
        return false;
    patched = malloc(size);
    if (!patched) return false;
    memcpy(patched, code, size);
    arm64_patch_mov_imm64_at(patched, size, stub.arg1_mov_off, 1,
                             stub.arg1_is_instr
                                 ? (uint64_t)(uintptr_t)&block->instrs[stub.instr_index]
                                 : (uint64_t)(uintptr_t)block);
    arm64_patch_mov_imm64_at(patched, size, stub.helper_mov_off, 23,
                             (uint64_t)(uintptr_t)helper_addr_for_cache_id(stub.helper_id));
    *out_code = patched;
    *owned_code = patched;
    return true;
}

static bool block_guest_span(const hb_ir_block_t* block, uint64_t* start, size_t* len) {
    uint64_t lo, hi;
    uint32_t steps;
    if (!block || !block->instr_count || !start || !len) return false;
    steps = jit_block_step_count(block);
    if (!steps || steps > block->instr_count) return false;
    lo = block->instrs[0].guest_addr;
    hi = lo;
    for (uint32_t i = 0; i < steps; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        uint64_t end = instr->guest_addr + instr->guest_len;
        if (instr->guest_addr < lo) lo = instr->guest_addr;
        if (end > hi) hi = end;
    }
    if (hi <= lo || hi - lo > 4096u) return false;
    *start = lo;
    *len = (size_t)(hi - lo);
    return true;
}

static hb_result_t persistent_cache_key_for_block(hb_jit_runtime_t* rt,
                                                  const hb_ir_block_t* block,
                                                  hb_cache_key_t* key) {
    uint64_t start = 0;
    size_t len = 0;
    uint8_t bytes[4096];
    hb_result_t r;
    if (!rt || !rt->ctx || !block || !key) return HB_ERR_INVALID_ARG;
    if (!block_guest_span(block, &start, &len)) return HB_ERR_UNSUPPORTED_FEATURE;
    r = hb_memory_read(rt->ctx->memory, start, bytes, len);
    if (r != HB_OK) return r;
    r = hb_cache_key_compute(bytes, len, rt->ctx->arch, HB_RUNTIME_PERSISTENT_CACHE_VERSION, key);
    if (r != HB_OK) return r;
    key->guest_addr = block->guest_addr;
    key->mode = (uint8_t)rt->ctx->mode;
    key->backend = (uint8_t)HB_BACKEND_JIT;
    key->flags = runtime_jit_flags();
    return HB_OK;
}

static hb_result_t jit_commit_blob(hb_jit_runtime_t* rt, const uint8_t* code, size_t size,
                                   uint8_t** out_dest) {
    hb_result_t r;
    uint8_t* dest;
    if (!rt || !rt->jit_mem || !code || !size || !out_dest) return HB_ERR_INVALID_ARG;
    if (rt->jit_mem->used + size > rt->jit_mem->size) {
        rt->code_cache_full = true;
        trace_jit_code_cache_full_once(rt, "jit-buffer-full", size);
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    r = hb_jit_buffer_make_writable(rt->jit_mem);
    if (r != HB_OK) return r;
    dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code, size);
    rt->jit_mem->used += size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    r = hb_jit_buffer_commit(rt->jit_mem);
    if (r != HB_OK) return r;
    *out_dest = dest;
    return HB_OK;
}

static int trace_jit_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_native_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_native_range_start(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_RANGE_START");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_native_range_end(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_NATIVE_RANGE_END");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_addr(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_ADDR");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_range_start(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_START");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static uint64_t trace_jit_guest_range_end(void) {
    static int parsed = 0;
    static uint64_t addr = 0;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_GUEST_RANGE_END");
        if (env && *env) addr = strtoull(env, NULL, 0);
        parsed = 1;
    }
    return addr;
}

static int trace_jit_blocks_budget_allows(int force) {
    static int count;
    static int exhausted;
    const char* env = getenv("MACRUNNER_HB_TRACE_JIT_BLOCK_BUDGET");
    int limit = env && *env ? atoi(env) : 2000;
    if (!trace_jit_blocks_enabled()) return 0;
    if (force || limit <= 0) return 1;
    if (count < limit) {
        count++;
        return 1;
    }
    if (!exhausted) {
        exhausted = 1;
        fprintf(stderr, "macrunner-hb-jit-block: trace budget exhausted at %d entries, silencing\n", limit);
        fflush(stderr);
    }
    return 0;
}

static void trace_jit_block(uint64_t guest_pc, const uint8_t* native, size_t native_size,
                            const hb_ir_block_t* block) {
    uint64_t watch = trace_jit_native_addr();
    uint64_t range_start = trace_jit_native_range_start();
    uint64_t range_end = trace_jit_native_range_end();
    uint64_t guest_watch = trace_jit_guest_addr();
    uint64_t guest_range_start = trace_jit_guest_range_start();
    uint64_t guest_range_end = trace_jit_guest_range_end();
    int matched = watch && (uintptr_t)native <= (uintptr_t)watch &&
                  (uintptr_t)watch < (uintptr_t)native + native_size;
    int range_matched = range_start && range_end && range_start < range_end &&
                        (uintptr_t)native < (uintptr_t)range_end &&
                        (uintptr_t)native + native_size > (uintptr_t)range_start;
    int guest_range_matched = guest_range_start && guest_range_end &&
                              guest_range_start < guest_range_end &&
                              guest_pc < guest_range_end;
    int guest_matched = guest_watch && guest_pc == guest_watch;
    const hb_ir_instr_t* first = (block && block->instr_count) ? &block->instrs[0] : NULL;
    const hb_ir_instr_t* last = (block && block->instr_count) ?
                                &block->instrs[block->instr_count - 1] : NULL;

    if (!guest_matched && guest_watch && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            uint64_t start = instr->guest_addr;
            uint64_t end = start + instr->guest_len;
            if (guest_watch == start || (instr->guest_len && guest_watch >= start && guest_watch < end)) {
                guest_matched = 1;
                break;
            }
        }
    }
    if (guest_range_matched && block) {
        guest_range_matched = 0;
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            uint64_t start = instr->guest_addr;
            uint64_t end = start + (instr->guest_len ? instr->guest_len : 1);
            if (start < guest_range_end && end > guest_range_start) {
                guest_range_matched = 1;
                break;
            }
        }
    }
    matched = matched || guest_matched || range_matched || guest_range_matched;
    if (!trace_jit_blocks_budget_allows(matched)) return;
    fprintf(stderr, "macrunner-hb-jit-block: guest=%p native=%p-%p size=%zu instrs=%zu "
            "first_op=%u first_guest=%p last_op=%u last_guest=%p last_target=%p%s\n",
            (void*)(uintptr_t)guest_pc, native, native + native_size, native_size,
            block ? block->instr_count : 0,
            first ? (unsigned)first->op : 0, first ? (void*)(uintptr_t)first->guest_addr : NULL,
            last ? (unsigned)last->op : 0, last ? (void*)(uintptr_t)last->guest_addr : NULL,
            last ? (void*)(uintptr_t)last->target : NULL, matched ? " match=1" : "");
    if (matched && block) {
        for (size_t i = 0; i < block->instr_count; i++) {
            const hb_ir_instr_t* instr = &block->instrs[i];
            fprintf(stderr, "macrunner-hb-jit-block-ir: guest=%p op=%u target=%p len=%u\n",
                    (void*)(uintptr_t)instr->guest_addr, (unsigned)instr->op,
                    (void*)(uintptr_t)instr->target, (unsigned)instr->guest_len);
            fprintf(stderr,
                    "macrunner-hb-jit-block-ir-operands: guest=%p "
                    "dst{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src1{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} "
                    "src2{t=%u sz=%u reg=%u off=%u imm=%lld mem=(base=%u index=%u scale=%u disp=%lld seg=%u addr32=%u)} cc=%u\n",
                    (void*)(uintptr_t)instr->guest_addr,
                    (unsigned)instr->dst.type, (unsigned)instr->dst.size,
                    (unsigned)instr->dst.reg, (unsigned)instr->dst.reg_offset,
                    (long long)instr->dst.imm, (unsigned)instr->dst.mem.base,
                    (unsigned)instr->dst.mem.index, (unsigned)instr->dst.mem.scale,
                    (long long)instr->dst.mem.disp, (unsigned)instr->dst.mem.segment,
                    (unsigned)instr->dst.mem.addr32,
                    (unsigned)instr->src1.type, (unsigned)instr->src1.size,
                    (unsigned)instr->src1.reg, (unsigned)instr->src1.reg_offset,
                    (long long)instr->src1.imm, (unsigned)instr->src1.mem.base,
                    (unsigned)instr->src1.mem.index, (unsigned)instr->src1.mem.scale,
                    (long long)instr->src1.mem.disp, (unsigned)instr->src1.mem.segment,
                    (unsigned)instr->src1.mem.addr32,
                    (unsigned)instr->src2.type, (unsigned)instr->src2.size,
                    (unsigned)instr->src2.reg, (unsigned)instr->src2.reg_offset,
                    (long long)instr->src2.imm, (unsigned)instr->src2.mem.base,
                    (unsigned)instr->src2.mem.index, (unsigned)instr->src2.mem.scale,
                    (long long)instr->src2.mem.disp, (unsigned)instr->src2.mem.segment,
                    (unsigned)instr->src2.mem.addr32,
                    (unsigned)instr->cc);
        }
    }
    fflush(stderr);
}

static bool trace_jit_block_contains_guest(const hb_ir_block_t* block, uint64_t guest) {
    if (!block || !guest) return false;
    if (block->guest_addr == guest) return true;
    for (size_t i = 0; i < block->instr_count; i++) {
        const hb_ir_instr_t* instr = &block->instrs[i];
        uint64_t start = instr->guest_addr;
        uint64_t end = start + instr->guest_len;
        if (guest == start || (instr->guest_len && guest >= start && guest < end))
            return true;
    }
    return false;
}

static void trace_jit_cached_watch_block_once(const hb_block_cache_entry_t* entry) {
    static uint64_t dumped_guest;
    uint64_t guest = trace_jit_guest_addr();
    if (!entry || !entry->valid || !entry->block || !trace_jit_blocks_enabled() || !guest)
        return;
    if (!trace_jit_block_contains_guest(entry->block, guest) ||
        dumped_guest == entry->block->guest_addr)
        return;
    dumped_guest = entry->block->guest_addr;
    trace_jit_block(entry->guest_addr, entry->native_code, entry->native_size, entry->block);
}

static int trace_jit_hot_blocks_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCKS");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static uint64_t trace_jit_hot_interval(void) {
    static int parsed;
    static uint64_t interval;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BLOCK_INTERVAL");
        interval = env && *env ? strtoull(env, NULL, 0) : 500000ULL;
        if (interval < 1000ULL) interval = 1000ULL;
        parsed = 1;
    }
    return interval;
}

static int trace_jit_hot_bytes_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES");
        cached = env && *env && *env != '0';
    }
    return cached;
}

static size_t trace_jit_hot_bytes_len(void) {
    static int parsed;
    static size_t len;
    if (!parsed) {
        const char* env = getenv("MACRUNNER_HB_TRACE_JIT_HOT_BYTES_LEN");
        len = env && *env ? (size_t)strtoull(env, NULL, 0) : 16;
        if (len < 1) len = 16;
        if (len > 128) len = 128;
        parsed = 1;
    }
    return len;
}

static void trace_jit_hot_guest_bytes(hb_context_t* ctx, uint64_t guest_addr) {
    uint8_t byte;
    size_t len;
    if (!trace_jit_hot_bytes_enabled() || !ctx || !ctx->memory) return;
    len = trace_jit_hot_bytes_len();
    fprintf(stderr, " bytes=");
    for (size_t i = 0; i < len; i++) {
        if (hb_memory_read_u8(ctx->memory, (hb_gva_t)(guest_addr + i), &byte) != HB_OK) {
            fprintf(stderr, "%s??", i ? " " : "");
            break;
        }
        fprintf(stderr, "%s%02x", i ? " " : "", byte);
    }
}

static void trace_jit_hot_block_tick(hb_jit_runtime_t* rt, hb_block_cache_entry_t* entry) {
    const size_t top_count = 12;
    hb_block_cache_entry_t* top[12] = {0};

    if (!rt || !entry || !trace_jit_hot_blocks_enabled()) return;
    entry->hit_count++;
    rt->hot_trace_blocks++;
    if (!rt->hot_trace_next)
        rt->hot_trace_next = trace_jit_hot_interval();
    if (rt->hot_trace_blocks < rt->hot_trace_next) return;
    rt->hot_trace_next += trace_jit_hot_interval();

    for (size_t i = 0; i < HB_BLOCK_CACHE_SIZE; i++) {
        hb_block_cache_entry_t* candidate = &rt->block_cache->entries[i];
        if (!candidate->valid || !candidate->hit_count) continue;
        for (size_t j = 0; j < top_count; j++) {
            if (!top[j] || candidate->hit_count > top[j]->hit_count) {
                for (size_t k = top_count - 1; k > j; k--) top[k] = top[k - 1];
                top[j] = candidate;
                break;
            }
        }
    }

    fprintf(stderr, "macrunner-hb-jit-hot-blocks: total=%llu interval=%llu used=%zu\n",
            (unsigned long long)rt->hot_trace_blocks,
            (unsigned long long)trace_jit_hot_interval(),
            rt->jit_mem ? rt->jit_mem->used : 0);
    for (size_t i = 0; i < top_count && top[i]; i++) {
        fprintf(stderr, "macrunner-hb-jit-hot-block: rank=%zu guest=%p hits=%llu native=%p "
                "size=%zu steps=%u",
                i + 1, (void*)(uintptr_t)top[i]->guest_addr,
                (unsigned long long)top[i]->hit_count,
                top[i]->native_code, top[i]->native_size, top[i]->steps);
        trace_jit_hot_guest_bytes(rt->ctx, top[i]->guest_addr);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

hb_jit_runtime_t* hb_jit_runtime_create(hb_context_t* ctx) {
    hb_jit_runtime_t* rt = calloc(1, sizeof(hb_jit_runtime_t));
    const char* size_env;
    size_t jit_size = 128u * 1024u * 1024u;
    if (!rt) return NULL;
    rt->ctx = ctx;
    size_env = getenv("MACRUNNER_HB_JIT_BUFFER_SIZE");
    if (size_env && *size_env) {
        unsigned long long parsed = strtoull(size_env, NULL, 0);
        if (parsed >= 65536ULL && parsed <= 512ULL * 1024ULL * 1024ULL)
            jit_size = (size_t)parsed;
    }
    rt->jit_mem = hb_jit_buffer_create(jit_size);
    if (!rt->jit_mem) { free(rt); return NULL; }
    rt->block_cache = block_cache_create();
    if (!rt->block_cache) {
        hb_jit_buffer_destroy(rt->jit_mem);
        free(rt);
        return NULL;
    }
    const char* cache_root = getenv("MACRUNNER_HB_TRANSLATION_CACHE_ROOT");
    if ((cache_root && *cache_root) || runtime_env_enabled("MACRUNNER_HB_TRANSLATION_CACHE")) {
        hb_cache_options_t options;
        memset(&options, 0, sizeof(options));
        rt->persistent_cache = hb_cache_open(cache_root && *cache_root ? cache_root : NULL, &options);
        translation_cache_register_atexit();
        if (translation_cache_trace_enabled()) {
            fprintf(stderr, "macrunner-hb-translation-cache-open: root=%s status=%s\n",
                    cache_root && *cache_root ? cache_root : "build/hyperbridge-cache",
                    rt->persistent_cache ? "ok" : "failed");
            fflush(stderr);
        }
    }
    return rt;
}

void hb_jit_runtime_destroy(hb_jit_runtime_t* rt) {
    if (!rt) return;
    if (rt->persistent_cache) translation_cache_trace_summary();
    hb_cache_close(rt->persistent_cache);
    hb_jit_buffer_destroy(rt->jit_mem);
    block_cache_destroy(rt->block_cache);
    free(rt);
}

/* Find block by guest address */
static hb_ir_block_t* find_block(const hb_ir_cfg_t* cfg, uint64_t addr) {
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

static bool is_control_transfer_op(hb_ir_op_t op) {
    return op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
           op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ;
}

static const hb_ir_instr_t* first_control_transfer_instr(const hb_ir_block_t* block) {
    if (!block) return NULL;
    for (size_t i = 0; i < block->instr_count; i++) {
        if (is_control_transfer_op(block->instrs[i].op)) return &block->instrs[i];
    }
    return NULL;
}

static uint32_t jit_block_step_count(const hb_ir_block_t* block) {
    const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
    if (!block) return 0;
    if (!transfer) return (uint32_t)block->instr_count;
    return (uint32_t)((size_t)(transfer - block->instrs) + 1);
}

static void sync_arch_pc_after_jit_block(hb_context_t* ctx) {
    if (!ctx) return;
    if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
    else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
}

static void set_helper_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                    uint64_t steps, uint64_t blocks_executed) {
    out->result = ctx->last_result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = "JIT helper fault";
}

static hb_result_t set_runtime_fault_result(hb_exec_result_t* out, hb_context_t* ctx,
                                            hb_result_t result, uint64_t steps,
                                            uint64_t blocks_executed,
                                            const char* reason) {
    if (ctx) ctx->last_result = result;
    out->result = result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = reason;
    return result;
}

static hb_result_t set_jit_interp_fallback_result(hb_exec_result_t* out,
                                                  hb_result_t result,
                                                  uint64_t steps,
                                                  uint64_t blocks_executed,
                                                  const char* reason) {
    out->result = result;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    out->faulted = true;
    out->fault_reason = reason;
    return HB_OK;
}

static void trace_jit_code_cache_full_once(hb_jit_runtime_t* rt,
                                           const char* reason,
                                           size_t needed) {
    if (!rt || !rt->jit_mem || !rt->block_cache) return;
    if (rt->code_cache_full_reports++) return;
    fprintf(stderr, "macrunner-hb-jit-code-cache-full: reason=%s used=%zu size=%zu "
            "needed=%zu entries=%zu capacity=%u\n",
            reason ? reason : "unknown", rt->jit_mem->used, rt->jit_mem->size,
            needed, rt->block_cache->count, (unsigned)HB_BLOCK_CACHE_SIZE);
    fflush(stderr);
}

static void try_promote_copy_scan_counted_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                               const hb_ir_block_t* block) {
    const hb_ir_block_t* body = NULL;
    const hb_ir_block_t* guard = NULL;
    hb_block_cache_entry_t* body_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        body_entry = block_cache_find(rt->block_cache, last->target);
        if (!body_entry || !body_entry->block || body_entry->fused) return;
        body = body_entry->block;
        guard = block;
    } else if (block->instr_count == 5 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* guard_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        if (!guard_entry || !guard_entry->block) return;
        body_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || body_entry->fused) return;
        body = block;
        guard = guard_entry->block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_copy_scan_counted_loop(cg, body, guard, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, body->guest_addr, dest, emitted_size,
                    (uint32_t)(body->instr_count + guard->instr_count), body, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=copy-scan-counted body=%p guard=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)body->guest_addr, (void*)(uintptr_t)guard->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static void try_promote_bounded_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* guard = NULL;
    const hb_ir_block_t* body = NULL;
    hb_block_cache_entry_t* guard_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 2 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* body_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        guard_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!body_entry || !body_entry->block || !guard_entry || guard_entry->fused) return;
        guard = block;
        body = body_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        guard_entry = block_cache_find(rt->block_cache, last->target);
        if (!guard_entry || !guard_entry->block || guard_entry->fused) return;
        guard = guard_entry->block;
        body = block;
    } else {
        return;
    }

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_bounded_scan_loop(cg, guard, body, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, guard->guest_addr, dest, emitted_size,
                    (uint32_t)(guard->instr_count + body->instr_count), guard, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=bounded-byte-scan guard=%p body=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)guard->guest_addr, (void*)(uintptr_t)body->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool same_plain_runtime_reg_operand(const hb_ir_operand_t* a,
                                           const hb_ir_operand_t* b) {
    return a && b &&
           a->type == HB_OP_REG && b->type == HB_OP_REG &&
           a->reg == b->reg && a->size == b->size &&
           a->reg_offset == b->reg_offset;
}

static bool runtime_exact_reg_operand(const hb_ir_operand_t* op,
                                      hb_reg_t reg,
                                      hb_size_t size) {
    return op && op->type == HB_OP_REG && op->reg == reg &&
           op->size == size && op->reg_offset == 0;
}

static bool runtime_exact_mem_operand(const hb_ir_operand_t* op,
                                      hb_reg_t base,
                                      hb_reg_t index,
                                      uint8_t scale,
                                      int64_t disp,
                                      hb_size_t size) {
    return op && op->type == HB_OP_MEM && op->size == size &&
           op->mem.base == base && op->mem.index == index &&
           op->mem.scale == scale && op->mem.disp == disp &&
           op->mem.segment == 0 && !op->mem.addr32;
}

static bool zero_extend_mem8_to_reg32(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ZERO_EXTEND &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_32 &&
           instr->src1.type == HB_OP_MEM && instr->src1.size == HB_SIZE_8;
}

static bool add_imm1_same_reg64(const hb_ir_instr_t* instr) {
    return instr && instr->op == HB_IR_ADD &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, &instr->src1) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == 1;
}

static bool test_same_reg32(const hb_ir_instr_t* instr, const hb_ir_operand_t* reg) {
    return instr && reg && instr->op == HB_IR_TEST &&
           instr->src1.type == HB_OP_REG && instr->src1.size == HB_SIZE_32 &&
           same_plain_runtime_reg_operand(&instr->src1, &instr->src2) &&
           same_plain_runtime_reg_operand(&instr->src1, reg);
}

static bool byte_compare_loop_pair(const hb_ir_block_t* cmp_block,
                                   const hb_ir_block_t* backedge_block) {
    const hb_ir_instr_t *lhs, *rhs, *sub, *cmp_jcc;
    const hb_ir_instr_t *inc, *test, *back_jcc;
    if (!cmp_block || !backedge_block ||
        cmp_block->instr_count != 4 || backedge_block->instr_count != 3)
        return false;

    lhs = &cmp_block->instrs[0];
    rhs = &cmp_block->instrs[1];
    sub = &cmp_block->instrs[2];
    cmp_jcc = &cmp_block->instrs[3];
    inc = &backedge_block->instrs[0];
    test = &backedge_block->instrs[1];
    back_jcc = &backedge_block->instrs[2];

    if (!zero_extend_mem8_to_reg32(lhs) || !zero_extend_mem8_to_reg32(rhs))
        return false;
    if (sub->op != HB_IR_SUB ||
        !same_plain_runtime_reg_operand(&sub->dst, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src1, &lhs->dst) ||
        !same_plain_runtime_reg_operand(&sub->src2, &rhs->dst))
        return false;
    if (cmp_jcc->op != HB_IR_Jcc ||
        (cmp_jcc->cc != HB_CC_E && cmp_jcc->cc != HB_CC_NE) ||
        cmp_jcc->guest_addr + cmp_jcc->guest_len != backedge_block->guest_addr)
        return false;
    if (!add_imm1_same_reg64(inc) ||
        !test_same_reg32(test, &rhs->dst) ||
        back_jcc->op != HB_IR_Jcc ||
        (back_jcc->cc != HB_CC_E && back_jcc->cc != HB_CC_NE) ||
        back_jcc->target != cmp_block->guest_addr)
        return false;
    return true;
}

static void try_promote_byte_compare_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                          const hb_ir_block_t* block) {
    const hb_ir_block_t* cmp_block = NULL;
    const hb_ir_block_t* backedge_block = NULL;
    hb_block_cache_entry_t* cmp_entry = NULL;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block || !block->instr_count)
        return;

    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (block->instr_count == 4 && last->op == HB_IR_Jcc) {
        hb_block_cache_entry_t* backedge_entry =
            block_cache_find(rt->block_cache, last->guest_addr + last->guest_len);
        cmp_entry = block_cache_find(rt->block_cache, block->guest_addr);
        if (!backedge_entry || !backedge_entry->block || !cmp_entry || cmp_entry->fused) return;
        cmp_block = block;
        backedge_block = backedge_entry->block;
    } else if (block->instr_count == 3 && last->op == HB_IR_Jcc) {
        cmp_entry = block_cache_find(rt->block_cache, last->target);
        if (!cmp_entry || !cmp_entry->block || cmp_entry->fused) return;
        cmp_block = cmp_entry->block;
        backedge_block = block;
    } else {
        return;
    }

    if (!byte_compare_loop_pair(cmp_block, backedge_block)) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, cmp_block, backedge_block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, cmp_block->guest_addr, dest, emitted_size,
                    (uint32_t)(cmp_block->instr_count + backedge_block->instr_count),
                    cmp_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=byte-compare-loop cmp=%p backedge=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)cmp_block->guest_addr,
                (void*)(uintptr_t)backedge_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool sub_imm_same_reg64(const hb_ir_instr_t* instr,
                               const hb_ir_operand_t* reg,
                               int64_t imm) {
    return instr && reg && instr->op == HB_IR_SUB &&
           instr->dst.type == HB_OP_REG && instr->dst.size == HB_SIZE_64 &&
           same_plain_runtime_reg_operand(&instr->dst, reg) &&
           same_plain_runtime_reg_operand(&instr->src1, reg) &&
           instr->src2.type == HB_OP_IMM && instr->src2.imm == imm;
}

static bool load_test_nonzero_qword_block(const hb_ir_block_t* block,
                                          hb_ir_operand_t* index_reg,
                                          uint64_t* fallthrough,
                                          uint64_t* nonzero_target) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* test = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_64 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_64 ||
        load->src1.mem.index >= HB_REG_COUNT || load->src1.mem.scale != 8)
        return false;
    if (test->src1.type != HB_OP_REG || test->src1.size != HB_SIZE_64 ||
        !same_plain_runtime_reg_operand(&test->src1, &test->src2) ||
        !same_plain_runtime_reg_operand(&test->src1, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (index_reg) *index_reg = hb_ir_reg(load->src1.mem.index, HB_SIZE_64);
    if (fallthrough) *fallthrough = jcc->guest_addr + jcc->guest_len;
    if (nonzero_target) *nonzero_target = jcc->target;
    return true;
}

static bool dec_to_test_block(const hb_ir_block_t* block,
                              const hb_ir_operand_t* index_reg,
                              uint64_t* test_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* sub = &block->instrs[0];
    const hb_ir_instr_t* jmp = &block->instrs[1];
    if (!sub_imm_same_reg64(sub, index_reg, 1)) return false;
    if (jmp->op != HB_IR_JMP) return false;
    if (test_target) *test_target = jmp->target;
    return true;
}

static bool test_nonnegative_backedge_block(const hb_ir_block_t* block,
                                            const hb_ir_operand_t* index_reg,
                                            uint64_t load_target) {
    if (!block || !index_reg || block->instr_count != 2) return false;
    const hb_ir_instr_t* test = &block->instrs[0];
    const hb_ir_instr_t* jcc = &block->instrs[1];
    return test && test->op == HB_IR_TEST &&
           same_plain_runtime_reg_operand(&test->src1, index_reg) &&
           same_plain_runtime_reg_operand(&test->src2, index_reg) &&
           jcc->op == HB_IR_Jcc && jcc->cc == HB_CC_NS && jcc->target == load_target;
}

static void try_promote_null_qword_scan_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                             const hb_ir_block_t* block) {
    const hb_ir_block_t* load_block = NULL;
    const hb_ir_block_t* dec_block = NULL;
    const hb_ir_block_t* test_block = NULL;
    hb_block_cache_entry_t* load_entry = NULL;
    hb_ir_operand_t index_reg = hb_ir_none();
    uint64_t dec_addr = 0;
    uint64_t test_addr = 0;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block)
        return;

    if (load_test_nonzero_qword_block(block, &index_reg, &dec_addr, NULL)) {
        load_block = block;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr))
            return;
        hb_block_cache_entry_t* test_entry = block_cache_find(rt->block_cache, test_addr);
        if (!test_entry || !test_entry->block) return;
        dec_block = dec_entry->block;
        test_block = test_entry->block;
        load_entry = block_cache_find(rt->block_cache, load_block->guest_addr);
    } else if (block->instr_count == 2 && block->instrs[1].op == HB_IR_Jcc) {
        test_block = block;
        uint64_t load_addr = block->instrs[1].target;
        load_entry = block_cache_find(rt->block_cache, load_addr);
        if (!load_entry || !load_entry->block ||
            !load_test_nonzero_qword_block(load_entry->block, &index_reg, &dec_addr, NULL))
            return;
        hb_block_cache_entry_t* dec_entry = block_cache_find(rt->block_cache, dec_addr);
        if (!dec_entry || !dec_entry->block ||
            !dec_to_test_block(dec_entry->block, &index_reg, &test_addr) ||
            test_addr != test_block->guest_addr)
            return;
        load_block = load_entry->block;
        dec_block = dec_entry->block;
    } else {
        return;
    }

    if (!load_entry || load_entry->fused ||
        !test_nonnegative_backedge_block(test_block, &index_reg, load_block->guest_addr))
        return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_four_block_loop_helper(cg, load_block, dec_block,
                                                            test_block, NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, load_block->guest_addr, dest, emitted_size,
                    (uint32_t)(load_block->instr_count + dec_block->instr_count +
                               test_block->instr_count),
                    load_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=null-qword-scan load=%p dec=%p test=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)load_block->guest_addr,
                (void*)(uintptr_t)dec_block->guest_addr,
                (void*)(uintptr_t)test_block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool load_cmp_jne_i32_entry_block(const hb_ir_block_t* block,
                                         uint64_t* equal_addr,
                                         uint64_t* less_addr) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* load = &block->instrs[0];
    const hb_ir_instr_t* cmp = &block->instrs[1];
    const hb_ir_instr_t* jcc = &block->instrs[2];
    if (load->op != HB_IR_LOAD ||
        load->dst.type != HB_OP_REG || load->dst.size != HB_SIZE_32 ||
        load->src1.type != HB_OP_MEM || load->src1.size != HB_SIZE_32)
        return false;
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_MEM || cmp->src1.size != HB_SIZE_32 ||
        !same_plain_runtime_reg_operand(&cmp->src2, &load->dst))
        return false;
    if (jcc->op != HB_IR_Jcc || jcc->cc != HB_CC_NE) return false;
    if (equal_addr) *equal_addr = jcc->guest_addr + jcc->guest_len;
    if (less_addr) *less_addr = jcc->target;
    return true;
}

static bool cmp_setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                                hb_ir_operand_t* setcc_dst) {
    if (!block || block->instr_count != 3) return false;
    const hb_ir_instr_t* cmp = &block->instrs[0];
    const hb_ir_instr_t* setcc = &block->instrs[1];
    const hb_ir_instr_t* ret = &block->instrs[2];
    if (cmp->op != HB_IR_CMP ||
        cmp->src1.type != HB_OP_REG || cmp->src1.size != HB_SIZE_64 ||
        cmp->src2.type != HB_OP_REG || cmp->src2.size != HB_SIZE_64)
        return false;
    if (setcc->op != HB_IR_SETcc || setcc->cc != cc || setcc->dst.size != HB_SIZE_8 ||
        ret->op != HB_IR_RET)
        return false;
    if (setcc_dst) *setcc_dst = setcc->dst;
    return true;
}

static bool setcc_ret_block(const hb_ir_block_t* block, hb_cc_t cc,
                            const hb_ir_operand_t* expected_dst) {
    if (!block || !expected_dst || block->instr_count != 2) return false;
    const hb_ir_instr_t* setcc = &block->instrs[0];
    const hb_ir_instr_t* ret = &block->instrs[1];
    return setcc->op == HB_IR_SETcc && setcc->cc == cc &&
           same_plain_runtime_reg_operand(&setcc->dst, expected_dst) &&
           ret->op == HB_IR_RET;
}

static const hb_ir_block_t* find_comparator_entry_pred(const hb_ir_block_t* block,
                                                       uint64_t* equal_addr,
                                                       uint64_t* less_addr) {
    if (load_cmp_jne_i32_entry_block(block, equal_addr, less_addr))
        return block;
    if (!block) return NULL;
    for (size_t i = 0; i < block->pred_count; i++) {
        const hb_ir_block_t* pred = block->pred[i];
        if (load_cmp_jne_i32_entry_block(pred, equal_addr, less_addr))
            return pred;
    }
    return NULL;
}

static const hb_ir_block_t* find_comparator_entry_near_cache(hb_block_cache_t* cache,
                                                             const hb_ir_block_t* equal_block,
                                                             uint64_t* equal_addr,
                                                             uint64_t* less_addr) {
    hb_ir_operand_t ignored = hb_ir_none();
    if (!cache || !equal_block || !cmp_setcc_ret_block(equal_block, HB_CC_B, &ignored))
        return NULL;
    for (uint64_t back = 1; back <= 16; back++) {
        hb_block_cache_entry_t* entry = block_cache_find(cache, equal_block->guest_addr - back);
        if (!entry || !entry->block) continue;
        uint64_t candidate_equal = 0;
        uint64_t candidate_less = 0;
        if (load_cmp_jne_i32_entry_block(entry->block, &candidate_equal, &candidate_less) &&
            candidate_equal == equal_block->guest_addr) {
            if (equal_addr) *equal_addr = candidate_equal;
            if (less_addr) *less_addr = candidate_less;
            return entry->block;
        }
    }
    return NULL;
}

static void try_promote_i32_less_tiebreaker_comparator(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                                       const hb_ir_block_t* block) {
    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block)
        return;

    uint64_t equal_addr = 0;
    uint64_t less_addr = 0;
    const hb_ir_block_t* entry_block = find_comparator_entry_pred(block, &equal_addr, &less_addr);
    if (!entry_block)
        entry_block = find_comparator_entry_near_cache(rt->block_cache, block, &equal_addr, &less_addr);
    if (!entry_block) return;

    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, entry_block->guest_addr);
    hb_block_cache_entry_t* equal = block_cache_find(rt->block_cache, equal_addr);
    hb_block_cache_entry_t* less = block_cache_find(rt->block_cache, less_addr);
    if (!entry || entry->fused || !equal || !equal->block)
        return;

    hb_ir_operand_t setcc_dst = hb_ir_none();
    if (!cmp_setcc_ret_block(equal->block, HB_CC_B, &setcc_dst))
        return;
    if (less && less->block && !setcc_ret_block(less->block, HB_CC_L, &setcc_dst))
        less = NULL;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_i32_less_tiebreaker_helper(
        cg, entry_block, equal->block, less && less->block ? less->block : NULL, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, entry_block->guest_addr, dest, emitted_size,
                    (uint32_t)(entry_block->instr_count + equal->block->instr_count +
                               (less && less->block ? less->block->instr_count : 0)),
                    entry_block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=i32-less-tiebreaker entry=%p equal=%p less=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)entry_block->guest_addr,
                (void*)(uintptr_t)equal->block->guest_addr,
                (void*)(uintptr_t)(less && less->block ? less->block->guest_addr : 0),
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool unity_sort_inner_block(const hb_ir_block_t* block, uint64_t* cont_addr) {
    if (!block || block->instr_count != 7) return false;
    const hb_ir_instr_t* i = block->instrs;

    if (i[0].op != HB_IR_LOAD ||
        !runtime_exact_reg_operand(&i[0].dst, HB_REG_RAX, HB_SIZE_64) ||
        !runtime_exact_mem_operand(&i[0].src1, HB_REG_RDI, HB_REG_COUNT, 1, 0, HB_SIZE_64))
        return false;
    if (i[1].op != HB_IR_MOV ||
        !runtime_exact_reg_operand(&i[1].dst, HB_REG_RCX, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[1].src1, HB_REG_R15, HB_SIZE_64))
        return false;
    if (i[2].op != HB_IR_STORE ||
        !runtime_exact_mem_operand(&i[2].src1, HB_REG_R14, HB_REG_COUNT, 1, 0, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[2].src2, HB_REG_RAX, HB_SIZE_64))
        return false;
    if (i[3].op != HB_IR_MOV ||
        !runtime_exact_reg_operand(&i[3].dst, HB_REG_R14, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[3].src1, HB_REG_RDI, HB_SIZE_64))
        return false;
    if (i[4].op != HB_IR_LOAD ||
        !runtime_exact_reg_operand(&i[4].dst, HB_REG_RDX, HB_SIZE_64) ||
        !runtime_exact_mem_operand(&i[4].src1, HB_REG_RDI, HB_REG_COUNT, 1, -8, HB_SIZE_64))
        return false;
    if (i[5].op != HB_IR_SUB ||
        !runtime_exact_reg_operand(&i[5].dst, HB_REG_RDI, HB_SIZE_64) ||
        !runtime_exact_reg_operand(&i[5].src1, HB_REG_RDI, HB_SIZE_64) ||
        i[5].src2.type != HB_OP_IMM || i[5].src2.imm != 8)
        return false;
    if (i[6].op != HB_IR_CALL ||
        !runtime_exact_reg_operand(&i[6].src1, HB_REG_RBP, HB_SIZE_64))
        return false;

    if (cont_addr) *cont_addr = i[6].guest_addr + i[6].guest_len;
    return true;
}

static bool unity_sort_cont_block(const hb_ir_block_t* block,
                                  uint64_t sort_addr,
                                  uint64_t* fallthrough_addr) {
    if (!block || block->instr_count != 2) return false;
    const hb_ir_instr_t* i = block->instrs;

    if (i[0].op != HB_IR_TEST ||
        !runtime_exact_reg_operand(&i[0].src1, HB_REG_RAX, HB_SIZE_8) ||
        !runtime_exact_reg_operand(&i[0].src2, HB_REG_RAX, HB_SIZE_8))
        return false;
    if (i[1].op != HB_IR_Jcc || i[1].cc != HB_CC_NE || i[1].target != sort_addr)
        return false;
    if (fallthrough_addr) *fallthrough_addr = i[1].guest_addr + i[1].guest_len;
    return true;
}

static const hb_ir_block_t* find_unity_sort_inner_near_cont(hb_block_cache_t* cache,
                                                            const hb_ir_block_t* cont) {
    if (!cache || !cont) return NULL;
    for (uint64_t back = 1; back <= 64; back++) {
        hb_block_cache_entry_t* entry = block_cache_find(cache, cont->guest_addr - back);
        uint64_t candidate_cont = 0;
        if (entry && entry->block &&
            unity_sort_inner_block(entry->block, &candidate_cont) &&
            candidate_cont == cont->guest_addr)
            return entry->block;
    }
    return NULL;
}

static bool unity_sort_comparator_ready(hb_context_t* ctx,
                                        const hb_ir_block_t* cmp) {
    static const uint8_t unity_cmp_bytes[] = {
        0x8b, 0x02,             /* mov eax, [rdx] */
        0x39, 0x01,             /* cmp [rcx], eax */
        0x75, 0x07,             /* jne +7 */
        0x48, 0x3b, 0xca,       /* cmp rcx, rdx */
        0x0f, 0x92, 0xc0,       /* setb al */
        0xc3,                   /* ret */
        0x0f, 0x9c, 0xc0,       /* setl al */
        0xc3                    /* ret */
    };
    uint8_t bytes[sizeof(unity_cmp_bytes)];
    uint64_t equal_addr = 0;
    uint64_t less_addr = 0;
    if (!ctx || !ctx->memory || !cmp ||
        !load_cmp_jne_i32_entry_block(cmp, &equal_addr, &less_addr))
        return false;
    if (equal_addr != cmp->guest_addr + 0x06 || less_addr != cmp->guest_addr + 0x0d)
        return false;
    if (hb_memory_read(ctx->memory, cmp->guest_addr, bytes, sizeof(bytes)) != HB_OK)
        return false;
    return memcmp(bytes, unity_cmp_bytes, sizeof(bytes)) == 0;
}

static int trace_unity_sort_promote_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* env = getenv("MACRUNNER_HB_TRACE_UNITY_SORT_PROMOTE");
        if (env && *env && *env != '0') cached = 1;
        else cached = trace_jit_blocks_enabled();
    }
    return cached;
}

static void trace_unity_sort_promote(const char* reason,
                                     const hb_ir_block_t* block,
                                     const hb_ir_block_t* sort,
                                     const hb_ir_block_t* cont,
                                     const hb_ir_block_t* cmp,
                                     uint64_t rbp) {
    static unsigned count;
    if (!trace_unity_sort_promote_enabled() || count++ >= 2000)
        return;
    fprintf(stderr,
            "macrunner-hb-unity-sort-promote: reason=%s block=%p block_instrs=%zu "
            "sort=%p cont=%p cmp=%p rbp=%p\n",
            reason ? reason : "unknown",
            block ? (void*)(uintptr_t)block->guest_addr : NULL,
            block ? block->instr_count : 0,
            sort ? (void*)(uintptr_t)sort->guest_addr : NULL,
            cont ? (void*)(uintptr_t)cont->guest_addr : NULL,
            cmp ? (void*)(uintptr_t)cmp->guest_addr : NULL,
            (void*)(uintptr_t)rbp);
    fflush(stderr);
}

static void try_promote_unity_sort_inner_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                              const hb_ir_block_t* block) {
    const hb_ir_block_t* sort = NULL;
    const hb_ir_block_t* cont = NULL;
    hb_block_cache_entry_t* sort_entry = NULL;
    uint64_t cont_addr = 0;
    uint64_t ignored_fallthrough = 0;

    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !block ||
        ctx->mode != HB_MODE_64BIT)
        return;

    if ((block->instr_count == 7 &&
         block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
        (block->instr_count == 2 &&
         block->instrs[block->instr_count - 1].op == HB_IR_Jcc)) {
        trace_unity_sort_promote("enter", block, NULL, NULL, NULL,
                                 ctx->regs.x64.rbp);
    }

    if (unity_sort_inner_block(block, &cont_addr)) {
        sort = block;
        hb_block_cache_entry_t* cont_entry = block_cache_find(rt->block_cache, cont_addr);
        if (!cont_entry || !cont_entry->block) {
            trace_unity_sort_promote("sort-cont-miss", block, sort, NULL, NULL,
                                     ctx->regs.x64.rbp);
            return;
        }
        cont = cont_entry->block;
    } else if (block->instr_count == 2 && block->instrs[1].op == HB_IR_Jcc) {
        sort = find_unity_sort_inner_near_cont(rt->block_cache, block);
        if (!sort || !unity_sort_inner_block(sort, &cont_addr) ||
            cont_addr != block->guest_addr) {
            trace_unity_sort_promote("guard-sort-miss", block, sort, block, NULL,
                                     ctx->regs.x64.rbp);
            return;
        }
        cont = block;
    } else {
        return;
    }

    if (!unity_sort_cont_block(cont, sort->guest_addr, &ignored_fallthrough)) {
        trace_unity_sort_promote("guard-shape-miss", block, sort, cont, NULL,
                                 ctx->regs.x64.rbp);
        return;
    }
    sort_entry = block_cache_find(rt->block_cache, sort->guest_addr);
    if (!sort_entry || sort_entry->fused) {
        trace_unity_sort_promote(sort_entry ? "sort-already-fused" : "sort-entry-miss",
                                 block, sort, cont, NULL, ctx->regs.x64.rbp);
        return;
    }

    hb_block_cache_entry_t* cmp_entry = block_cache_find(rt->block_cache, ctx->regs.x64.rbp);
    if (!cmp_entry || !cmp_entry->block) {
        trace_unity_sort_promote("cmp-entry-miss", block, sort, cont, NULL,
                                 ctx->regs.x64.rbp);
        return;
    }
    if (!unity_sort_comparator_ready(ctx, cmp_entry->block)) {
        trace_unity_sort_promote("cmp-shape-miss", block, sort, cont, cmp_entry->block,
                                 ctx->regs.x64.rbp);
        return;
    }
    trace_unity_sort_promote("emit", block, sort, cont, cmp_entry->block,
                             ctx->regs.x64.rbp);

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    hb_result_t r = hb_arm64_codegen_unity_sort_inner_loop_helper(
        cg, sort, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, sort->guest_addr, dest, emitted_size,
                    (uint32_t)(sort->instr_count + cont->instr_count),
                    sort, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr,
                "macrunner-hb-jit-fusion: kind=unity-sort-inner sort=%p cont=%p cmp=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)sort->guest_addr,
                (void*)(uintptr_t)cont->guest_addr,
                (void*)(uintptr_t)cmp_entry->block->guest_addr,
                dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static bool small_terminal_jcc_self_loop(const hb_ir_block_t* block) {
    if (!block || block->instr_count < 2 || block->instr_count > 16) return false;
    const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
    if (last->op != HB_IR_Jcc || last->target != block->guest_addr) return false;
    for (size_t i = 0; i + 1 < block->instr_count; i++) {
        hb_ir_op_t op = block->instrs[i].op;
        if (op == HB_IR_CALL || op == HB_IR_RET || op == HB_IR_JMP ||
            op == HB_IR_Jcc || op == HB_IR_LOOP || op == HB_IR_JRCXZ)
            return false;
    }
    return true;
}

static void try_promote_self_loop(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                  const hb_ir_block_t* block) {
    if (!rt || !rt->block_cache || !rt->jit_mem || rt->code_cache_full ||
        block_cache_is_full(rt->block_cache) || !ctx || !small_terminal_jcc_self_loop(block))
        return;
    hb_block_cache_entry_t* entry = block_cache_find(rt->block_cache, block->guest_addr);
    if (!entry || entry->fused) return;

    hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(1024);
    if (!code_buf) return;
    hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
    if (!cg) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    hb_result_t r = hb_arm64_codegen_two_block_loop_helper(cg, block, block, code_buf);
    hb_arm64_codegen_destroy(cg);
    if (r != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    if (hb_jit_buffer_make_writable(rt->jit_mem) != HB_OK) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }
    size_t needed = code_buf->size;
    if (rt->jit_mem->used + needed > rt->jit_mem->size) {
        hb_codegen_buffer_destroy(code_buf);
        return;
    }

    size_t emitted_size = code_buf->size;
    uint8_t* dest = rt->jit_mem->writable + rt->jit_mem->used;
    memcpy(dest, code_buf->code, emitted_size);
    rt->jit_mem->used += emitted_size;
    rt->jit_mem->used = (rt->jit_mem->used + 15) & ~15;
    hb_codegen_buffer_destroy(code_buf);

    if (hb_jit_buffer_commit(rt->jit_mem) != HB_OK) return;
    block_cache_put(rt->block_cache, block->guest_addr, dest, emitted_size,
                    (uint32_t)block->instr_count, block, true);
    if (trace_jit_blocks_enabled()) {
        fprintf(stderr, "macrunner-hb-jit-fusion: kind=self-loop block=%p "
                "native=%p-%p size=%zu\n",
                (void*)(uintptr_t)block->guest_addr, dest, dest + emitted_size, emitted_size);
        fflush(stderr);
    }
}

static void try_promote_hot_block_families(hb_jit_runtime_t* rt, hb_context_t* ctx,
                                           const hb_ir_block_t* block) {
    try_promote_copy_scan_counted_loop(rt, ctx, block);
    try_promote_bounded_scan_loop(rt, ctx, block);
    try_promote_byte_compare_loop(rt, ctx, block);
    try_promote_null_qword_scan_loop(rt, ctx, block);
    try_promote_i32_less_tiebreaker_comparator(rt, ctx, block);
    try_promote_unity_sort_inner_loop(rt, ctx, block);
    try_promote_self_loop(rt, ctx, block);
}

static bool should_retry_cached_promotion(const hb_block_cache_entry_t* entry) {
    if (!entry || entry->fused) return false;
    if (entry->hit_count < 4) return true;
    return (entry->hit_count & (entry->hit_count - 1)) == 0;
}

hb_result_t hb_jit_runtime_compile(hb_jit_runtime_t* rt, const hb_ir_func_t* func) {
    (void)rt; (void)func;
    /* Compilation is done on-demand per-block in hb_jit_runtime_run for MVP */
    return HB_OK;
}

hb_result_t hb_jit_runtime_run(hb_jit_runtime_t* rt, const hb_ir_func_t* func, hb_exec_result_t* out) {
    if (!rt || !func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = rt->ctx;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;
    ctx->last_result = HB_OK;

    while (1) {
        if (ctx->step_limit > 0 && steps >= ctx->step_limit) {
            out->result = HB_ERR_STEP_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            return set_runtime_fault_result(out, ctx, HB_ERR_BLOCK_LIMIT, steps,
                                            blocks_executed, "block limit reached");
        }

        hb_ir_block_t* block = find_block(func->cfg, ctx->pc);
        if (!block) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before current PC");
            }
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK; /* No block for PC — function exit or external call */
        }

        blocks_executed++;

        /* Check in-memory block cache */
        hb_block_cache_entry_t* cached = block_cache_find(rt->block_cache, ctx->pc);
        if (cached) {
            if (trace_jit_block_contains_guest(block, trace_jit_guest_addr())) {
                trace_unity_sort_promote("cache-hit-watch", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (block && ((block->instr_count == 7 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
                          (block->instr_count == 2 &&
                           block->instrs[block->instr_count - 1].op == HB_IR_Jcc))) {
                trace_unity_sort_promote("cache-hit-gate", block, NULL, NULL, NULL,
                                         ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
            }
            if (should_retry_cached_promotion(cached)) {
                try_promote_hot_block_families(rt, ctx, block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    rt->code_cache_full = true;
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", 0);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            trace_jit_cached_watch_block_once(cached);
            typedef void (*jit_block_t)(hb_context_t*);
            jit_block_t exec = (jit_block_t)(void*)cached->native_code;
            exec(ctx);
            trace_jit_hot_block_tick(rt, cached);
            steps += cached->steps;
        } else {
            if (rt->code_cache_full || block_cache_is_full(rt->block_cache)) {
                rt->code_cache_full = true;
                trace_jit_code_cache_full_once(rt, "block-cache-full", 0);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            hb_cache_key_t persistent_key;
            bool have_persistent_key = false;
            bool loaded_from_persistent = false;
            uint8_t* dest = NULL;
            size_t emitted_size = 0;
            hb_result_t r;

            if (rt->persistent_cache &&
                persistent_cache_key_for_block(rt, block, &persistent_key) == HB_OK) {
                hb_cache_entry_t* disk_entry = NULL;
                have_persistent_key = true;
                r = hb_cache_lookup(rt->persistent_cache, &persistent_key, &disk_entry);
                if (r == HB_OK && disk_entry && disk_entry->valid) {
                    const uint8_t* load_code = NULL;
                    uint8_t* owned_load_code = NULL;
                    emitted_size = disk_entry->native_size;
                    if (native_blob_prepare_cache_load(disk_entry->native_code,
                                                       disk_entry->native_size,
                                                       block, &load_code,
                                                       &owned_load_code) &&
                        (r = jit_commit_blob(rt, load_code, emitted_size, &dest)) == HB_OK) {
                        cached = block_cache_put(rt->block_cache, ctx->pc, dest, emitted_size,
                                                 disk_entry->steps ? disk_entry->steps
                                                                    : jit_block_step_count(block),
                                                 block, false);
                        if (cached) {
                            loaded_from_persistent = true;
                            translation_cache_add_u64(&g_translation_cache_hits, 1);
                            translation_cache_add_u64(&g_translation_cache_bytes_loaded, emitted_size);
                        }
                    }
                    free(owned_load_code);
                } else {
                    translation_cache_add_u64(&g_translation_cache_misses, 1);
                }
                hb_cache_entry_free(disk_entry);
            }

            if (!loaded_from_persistent) {
                /* Compile block into codegen buffer */
                hb_codegen_buffer_t* code_buf = hb_codegen_buffer_create(4096);
                if (!code_buf) return HB_ERR_OUT_OF_MEMORY;

                hb_arm64_codegen_t* cg = hb_arm64_codegen_create(ctx);
                if (!cg) { hb_codegen_buffer_destroy(code_buf); return HB_ERR_OUT_OF_MEMORY; }

                r = hb_arm64_codegen_block_with_cfg(cg, block, func->cfg, code_buf);
                hb_arm64_codegen_destroy(cg);
                if (r != HB_OK) {
                    hb_codegen_buffer_destroy(code_buf);
                    out->result = r;
                    out->steps_executed = steps;
                    out->blocks_executed = blocks_executed;
                    out->faulted = true;
                    out->fault_reason = "JIT codegen failed";
                    return HB_OK;
                }

                emitted_size = code_buf->size;
                r = jit_commit_blob(rt, code_buf->code, emitted_size, &dest);
                if (r != HB_OK) {
                    hb_codegen_buffer_destroy(code_buf);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT code cache full; interpreter fallback");
                }

                if (rt->persistent_cache && have_persistent_key &&
                    code_buf->code && code_buf->size) {
                    const uint8_t* store_code = NULL;
                    uint8_t* owned_store_code = NULL;
                    hb_cache_entry_t metadata;
                    if (native_blob_prepare_cache_store(code_buf->code, code_buf->size,
                                                        block, &store_code,
                                                        &owned_store_code)) {
                        memset(&metadata, 0, sizeof(metadata));
                        metadata.steps = jit_block_step_count(block);
                        r = hb_cache_store(rt->persistent_cache, &persistent_key, store_code,
                                           code_buf->size, &metadata);
                        if (r == HB_OK) {
                            translation_cache_add_u64(&g_translation_cache_stores, 1);
                            translation_cache_add_u64(&g_translation_cache_bytes_stored,
                                                      code_buf->size);
                        }
                        free(owned_store_code);
                    } else {
                        translation_cache_add_u64(&g_translation_cache_store_skips, 1);
                    }
                } else if (rt->persistent_cache && have_persistent_key) {
                    translation_cache_add_u64(&g_translation_cache_store_skips, 1);
                }
                hb_codegen_buffer_destroy(code_buf);

                /* Store in block cache */
                cached = block_cache_put(rt->block_cache, ctx->pc, dest, emitted_size,
                                         jit_block_step_count(block), block, false);
                if (!cached) {
                    rt->code_cache_full = true;
                    trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                }
                trace_jit_block(ctx->pc, dest, emitted_size, block);
            } else if (trace_jit_blocks_enabled()) {
                fprintf(stderr, "macrunner-hb-translation-cache-hit: guest=%p native=%p-%p size=%zu\n",
                        (void*)(uintptr_t)ctx->pc, dest, dest + emitted_size, emitted_size);
                fflush(stderr);
            }

            if (!cached) {
                rt->code_cache_full = true;
                trace_jit_code_cache_full_once(rt, "block-cache-put-failed", emitted_size);
                return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                      steps, blocks_executed,
                                                      "JIT code cache full; interpreter fallback");
            }

            if (!cached->fused) {
                if (trace_jit_block_contains_guest(block, trace_jit_guest_addr())) {
                    trace_unity_sort_promote("cache-put-watch", block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                if (block && ((block->instr_count == 7 &&
                               block->instrs[block->instr_count - 1].op == HB_IR_CALL) ||
                              (block->instr_count == 2 &&
                               block->instrs[block->instr_count - 1].op == HB_IR_Jcc))) {
                    trace_unity_sort_promote("cache-put-gate", block, NULL, NULL, NULL,
                                             ctx->mode == HB_MODE_64BIT ? ctx->regs.x64.rbp : 0);
                }
                try_promote_hot_block_families(rt, ctx, block);
                cached = block_cache_find(rt->block_cache, ctx->pc);
                if (!cached) {
                    rt->code_cache_full = true;
                    trace_jit_code_cache_full_once(rt, "block-cache-promote-lost-entry", emitted_size);
                    return set_jit_interp_fallback_result(out, HB_ERR_UNSUPPORTED_FEATURE,
                                                          steps, blocks_executed,
                                                          "JIT block cache lost promoted entry; interpreter fallback");
                }
            }
            trace_jit_cached_watch_block_once(cached);

            /* Execute */
            typedef void (*jit_block_t)(hb_context_t*);
            jit_block_t exec = (jit_block_t)(void*)dest;
            exec = (jit_block_t)(void*)cached->native_code;
            exec(ctx);
            trace_jit_hot_block_tick(rt, cached);
            steps += cached->steps ? cached->steps : jit_block_step_count(block);
        }
        sync_arch_pc_after_jit_block(ctx);
        if (ctx->last_result != HB_OK) {
            set_helper_fault_result(out, ctx, steps, blocks_executed);
            return HB_OK;
        }

        /* Determine if we should continue or stop */
        if (block->instr_count == 0) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        const hb_ir_instr_t* transfer = first_control_transfer_instr(block);
        const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
        const hb_ir_instr_t* terminal = transfer ? transfer : last;
        if (terminal->op == HB_IR_RET) {
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }

        /* For CALL/JMP/Jcc, PC was updated by JIT code; find next block */
        hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
        if (!next) {
            if (func->truncated) {
                return set_runtime_fault_result(out, ctx, HB_ERR_TRANSLATION_TRUNCATED,
                                                steps, blocks_executed,
                                                "translated function truncated before branch target");
            }
            if (is_control_transfer_op(terminal->op)) {
                out->result = HB_OK;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK; /* External branch/call/return boundary */
            }
            out->result = HB_ERR_NOT_FOUND;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            out->faulted = true;
            out->fault_reason = "branch target block not found";
            return HB_OK;
        }
        if (terminal->op == HB_IR_JMP || terminal->op == HB_IR_Jcc ||
            terminal->op == HB_IR_CALL || terminal->op == HB_IR_LOOP ||
            terminal->op == HB_IR_JRCXZ) {
            /* Continue with the target block */
            continue;
        }

        /* Sequential block end — stop */
        ctx->pc = last->guest_addr + last->guest_len;
        if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
        else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
        next = find_block(func->cfg, ctx->pc);
        if (next) continue;
        out->result = HB_OK;
        out->steps_executed = steps;
        out->blocks_executed = blocks_executed;
        return HB_OK;
    }
}

hb_result_t hb_runtime_run(hb_context_t* ctx, const hb_ir_func_t* func, hb_backend_t backend, hb_exec_result_t* out) {
    if (!ctx || !func || !out) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));
    switch (backend) {
        case HB_BACKEND_INTERP: {
            hb_interpreter_t* i = hb_interpreter_create(ctx);
            if (!i) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_interpreter_run(i, func, out);
            hb_interpreter_destroy(i);
            return r;
        }
        case HB_BACKEND_JIT:
        case HB_BACKEND_AOT: {
            hb_jit_runtime_t* rt = hb_jit_runtime_create(ctx);
            if (!rt) return HB_ERR_OUT_OF_MEMORY;
            hb_result_t r = hb_jit_runtime_run(rt, func, out);
            hb_jit_runtime_destroy(rt);
            return r;
        }
    }
    return HB_ERR_INVALID_ARG;
}
