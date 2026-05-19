#include "hb_thunk.h"
#include <stdlib.h>
#include <string.h>

hb_thunk_table_t* hb_thunk_table_create(void) {
    hb_thunk_table_t* t = calloc(1, sizeof(hb_thunk_table_t));
    if (!t) return NULL;
    t->capacity = 64;
    t->thunks = calloc(t->capacity, sizeof(hb_thunk_def_t));
    return t;
}

void hb_thunk_table_destroy(hb_thunk_table_t* table) {
    if (!table) return;
    free(table->thunks);
    free(table);
}

hb_result_t hb_thunk_register(hb_thunk_table_t* table, const hb_thunk_def_t* def) {
    if (!table || !def) return HB_ERR_INVALID_ARG;
    if (table->count >= table->capacity) return HB_ERR_OUT_OF_MEMORY;
    table->thunks[table->count++] = *def;
    return HB_OK;
}

hb_thunk_def_t* hb_thunk_find_by_name(hb_thunk_table_t* table, const char* dll, const char* func) {
    if (!table || !dll || !func) return NULL;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->thunks[i].dll_name, dll) == 0 && strcmp(table->thunks[i].func_name, func) == 0) {
            return &table->thunks[i];
        }
    }
    return NULL;
}

hb_thunk_def_t* hb_thunk_find_by_id(hb_thunk_table_t* table, uint32_t id) {
    if (!table) return NULL;
    for (size_t i = 0; i < table->count; i++) {
        if (table->thunks[i].id == id) return &table->thunks[i];
    }
    return NULL;
}

hb_result_t hb_thunk_invoke(hb_thunk_table_t* table, uint32_t id, hb_context_t* ctx) {
    hb_thunk_def_t* d = hb_thunk_find_by_id(table, id);
    if (!d) return HB_ERR_IMPORT_UNSUPPORTED;
    if (!d->fn) return HB_ERR_IMPORT_UNSUPPORTED;
    return d->fn(ctx);
}

static hb_result_t thunk_get_tick_count(hb_context_t* ctx) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    /* Placeholder: return 0 in RAX/EAX */
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = 0;
    else ctx->regs.x86.eax = 0;
    return HB_OK;
}

hb_result_t hb_thunk_init_builtins(hb_thunk_table_t* table) {
    if (!table) return HB_ERR_INVALID_ARG;
    hb_thunk_def_t def = {0};
    def.id = 1;
    def.dll_name = "kernel32.dll";
    def.func_name = "GetTickCount";
    def.fn = thunk_get_tick_count;
    def.description = "Returns 0 placeholder";
    return hb_thunk_register(table, &def);
}

#include "hb_memory.h"

#define HB_GENERATED_THUNK_MAX 128

static hb_generated_thunk_t generated_thunks[HB_GENERATED_THUNK_MAX];
static hb_thunk_stats_t generated_stats;

hb_result_t hb_thunk_trace_enable(hb_thunk_table_t* table, bool enable) {
    (void)table;
    (void)enable;
    return HB_OK;
}

bool hb_thunk_trace_is_enabled(hb_thunk_table_t* table) {
    (void)table;
    return false;
}

hb_result_t hb_thunk_get(uint64_t module_id, hb_thunk_signature_id_t signature_id, void* target_ptr, hb_generated_thunk_t* out) {
    if (!target_ptr || !out) {
        generated_stats.unsupported++;
        return HB_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < HB_GENERATED_THUNK_MAX; i++) {
        hb_generated_thunk_t* t = &generated_thunks[i];
        if (t->valid && t->module_id == module_id && t->signature_id == signature_id && t->target_ptr == target_ptr) {
            generated_stats.cache_hits++;
            *out = *t;
            return HB_OK;
        }
    }
    for (size_t i = 0; i < HB_GENERATED_THUNK_MAX; i++) {
        hb_generated_thunk_t* t = &generated_thunks[i];
        if (!t->valid) {
            memset(t, 0, sizeof(*t));
            t->module_id = module_id;
            t->signature_id = signature_id;
            t->target_ptr = target_ptr;
            t->native_entry = target_ptr;
            t->generation = generated_stats.generated + 1;
            t->valid = true;
            generated_stats.generated++;
            *out = *t;
            return HB_OK;
        }
    }
    generated_stats.unsupported++;
    return HB_ERR_OUT_OF_MEMORY;
}

hb_result_t hb_thunk_release(uint64_t module_id) {
    for (size_t i = 0; i < HB_GENERATED_THUNK_MAX; i++) {
        if (generated_thunks[i].valid && generated_thunks[i].module_id == module_id) {
            generated_thunks[i].valid = false;
            generated_stats.released++;
        }
    }
    return HB_OK;
}

hb_result_t hb_thunk_stats(hb_thunk_stats_t* out) {
    if (!out) return HB_ERR_INVALID_ARG;
    *out = generated_stats;
    out->wx_pages_used = false;
    return HB_OK;
}

static uint64_t hb_thunk_x64_arg(const hb_context_t* ctx, unsigned idx) {
    switch (idx) {
        case 0: return ctx->regs.x64.rcx;
        case 1: return ctx->regs.x64.rdx;
        case 2: return ctx->regs.x64.r8;
        case 3: return ctx->regs.x64.r9;
        default: return 0;
    }
}

static uint64_t hb_thunk_x64_stack_arg(hb_context_t* ctx, unsigned idx) {
    uint64_t val = 0;
    if (!ctx->memory) return 0;
    /* Windows x64: return address + 32-byte shadow space, then stack args. */
    if (hb_memory_read_u64(ctx->memory, (hb_gva_t)ctx->regs.x64.rsp + 8 + 32 + idx * 8, &val) != HB_OK) return 0;
    return val;
}

static uint64_t hb_thunk_x86_stack_arg(hb_context_t* ctx, unsigned idx) {
    uint32_t val = 0;
    if (!ctx->memory) return 0;
    if (hb_memory_read_u32(ctx->memory, (hb_gva_t)ctx->regs.x86.esp + 4 + idx * 4, &val) != HB_OK) return 0;
    return val;
}

hb_result_t hb_thunk_call_generated(hb_context_t* ctx, const hb_generated_thunk_t* thunk) {
    if (!ctx || !thunk || !thunk->valid || !thunk->target_ptr) return HB_ERR_INVALID_ARG;
    switch (thunk->signature_id) {
        case HB_THUNK_SIG_VOID_VOID: {
            typedef void (*fn_t)(void);
            ((fn_t)thunk->target_ptr)();
            return HB_OK;
        }
        case HB_THUNK_SIG_U32_VOID: {
            typedef uint32_t (*fn_t)(void);
            uint32_t rc = ((fn_t)thunk->target_ptr)();
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = rc;
            else ctx->regs.x86.eax = rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_U32_U32: {
            typedef uint32_t (*fn_t)(uint32_t);
            uint32_t a = ctx->mode == HB_MODE_64BIT ? (uint32_t)hb_thunk_x64_arg(ctx, 0) : (uint32_t)hb_thunk_x86_stack_arg(ctx, 0);
            uint32_t rc = ((fn_t)thunk->target_ptr)(a);
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = rc;
            else ctx->regs.x86.eax = rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_U64_U64_U64: {
            typedef uint64_t (*fn_t)(uint64_t, uint64_t);
            uint64_t a = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 0) : hb_thunk_x86_stack_arg(ctx, 0);
            uint64_t b = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 1) : hb_thunk_x86_stack_arg(ctx, 1);
            uint64_t rc = ((fn_t)thunk->target_ptr)(a, b);
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = rc;
            else ctx->regs.x86.eax = (uint32_t)rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_PTR_PTR: {
            typedef uintptr_t (*fn_t)(uintptr_t);
            uintptr_t a = ctx->mode == HB_MODE_64BIT ? (uintptr_t)hb_thunk_x64_arg(ctx, 0) : (uintptr_t)hb_thunk_x86_stack_arg(ctx, 0);
            uintptr_t rc = ((fn_t)thunk->target_ptr)(a);
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = (uint64_t)rc;
            else ctx->regs.x86.eax = (uint32_t)rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_U64_VOID: {
            typedef uint64_t (*fn_t)(void);
            uint64_t rc = ((fn_t)thunk->target_ptr)();
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = rc;
            else ctx->regs.x86.eax = (uint32_t)rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_VOID_U32: {
            typedef void (*fn_t)(uint32_t);
            uint32_t a = ctx->mode == HB_MODE_64BIT ? (uint32_t)hb_thunk_x64_arg(ctx, 0) : (uint32_t)hb_thunk_x86_stack_arg(ctx, 0);
            ((fn_t)thunk->target_ptr)(a);
            return HB_OK;
        }
        case HB_THUNK_SIG_BOOL_HANDLE_PTR_U32_PTR_PTR: {
            typedef uint32_t (*fn_t)(uint64_t, uint64_t, uint32_t, uint64_t, uint64_t);
            uint64_t a0 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 0) : hb_thunk_x86_stack_arg(ctx, 0);
            uint64_t a1 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 1) : hb_thunk_x86_stack_arg(ctx, 1);
            uint32_t a2 = ctx->mode == HB_MODE_64BIT ? (uint32_t)hb_thunk_x64_arg(ctx, 2) : (uint32_t)hb_thunk_x86_stack_arg(ctx, 2);
            uint64_t a3 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 3) : hb_thunk_x86_stack_arg(ctx, 3);
            uint64_t a4 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_stack_arg(ctx, 0) : hb_thunk_x86_stack_arg(ctx, 4);
            uint32_t rc = ((fn_t)thunk->target_ptr)(a0, a1, a2, a3, a4);
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = rc;
            else ctx->regs.x86.eax = rc;
            return HB_OK;
        }
        case HB_THUNK_SIG_I32_PTR_CSTR_U64_U64: {
            typedef int32_t (*fn_t)(uintptr_t, uintptr_t, uint64_t, uint64_t);
            uintptr_t a0 = ctx->mode == HB_MODE_64BIT ? (uintptr_t)hb_thunk_x64_arg(ctx, 0) : (uintptr_t)hb_thunk_x86_stack_arg(ctx, 0);
            uintptr_t a1 = ctx->mode == HB_MODE_64BIT ? (uintptr_t)hb_thunk_x64_arg(ctx, 1) : (uintptr_t)hb_thunk_x86_stack_arg(ctx, 1);
            uint64_t a2 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 2) : hb_thunk_x86_stack_arg(ctx, 2);
            uint64_t a3 = ctx->mode == HB_MODE_64BIT ? hb_thunk_x64_arg(ctx, 3) : hb_thunk_x86_stack_arg(ctx, 3);
            int32_t rc = ((fn_t)thunk->target_ptr)(a0, a1, a2, a3);
            if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rax = (uint32_t)rc;
            else ctx->regs.x86.eax = (uint32_t)rc;
            return HB_OK;
        }
        default:
            generated_stats.unsupported++;
            return HB_ERR_UNSUPPORTED_FEATURE;
    }
}
