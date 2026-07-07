#include "hb_wow64cpu.h"
#include "hb_decoder.h"
#include "hb_flags.h"
#include "hb_lifter.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HB_WOW64CPU_DEFAULT_MAX_CODE_BYTES 4096u
#define HB_WOW64CPU_IR_CACHE_SIZE 8192u
#define HB_WOW64CPU_TEB32_PEB 0x30u
#define HB_WOW64CPU_PEB32_PROCESS_PARAMETERS 0x10u
#define HB_WOW64CPU_RTL_USER_PROCESS_PARAMETERS32_ENVIRONMENT 0x48u
#define HB_WOW64CPU_STATUS_VARIABLE_NOT_FOUND 0xc0000100u
#define HB_WOW64CPU_STATUS_BUFFER_TOO_SMALL 0xc0000023u

typedef struct {
    uint32_t pc;
    hb_ir_func_t* func;
} hb_wow64_ir_cache_entry_t;

typedef struct {
    hb_wow64_ir_cache_entry_t entries[HB_WOW64CPU_IR_CACHE_SIZE];
} hb_wow64_ir_cache_t;

static size_t wow64_ir_cache_hash(uint32_t pc) {
    return (size_t)((pc >> 4) ^ (pc >> 17)) & (HB_WOW64CPU_IR_CACHE_SIZE - 1);
}

static hb_ir_func_t* wow64_ir_cache_find(hb_wow64_ir_cache_t* cache, uint32_t pc) {
    size_t idx, i;

    if (!cache) return NULL;
    idx = wow64_ir_cache_hash(pc);
    for (i = 0; i < HB_WOW64CPU_IR_CACHE_SIZE; i++) {
        hb_wow64_ir_cache_entry_t* entry = &cache->entries[(idx + i) & (HB_WOW64CPU_IR_CACHE_SIZE - 1)];
        if (!entry->func) return NULL;
        if (entry->pc == pc) return entry->func;
    }
    return NULL;
}

static bool wow64_ir_cache_put(hb_wow64_ir_cache_t* cache, uint32_t pc, hb_ir_func_t* func) {
    size_t idx, i;

    if (!cache || !func) return false;
    idx = wow64_ir_cache_hash(pc);
    for (i = 0; i < HB_WOW64CPU_IR_CACHE_SIZE; i++) {
        hb_wow64_ir_cache_entry_t* entry = &cache->entries[(idx + i) & (HB_WOW64CPU_IR_CACHE_SIZE - 1)];
        if (!entry->func) {
            entry->pc = pc;
            entry->func = func;
            return true;
        }
        if (entry->pc == pc) return true;
    }
    return false;
}

static void wow64_ir_cache_destroy(hb_wow64_ir_cache_t* cache) {
    size_t i;

    if (!cache) return;
    for (i = 0; i < HB_WOW64CPU_IR_CACHE_SIZE; i++)
        if (cache->entries[i].func) hb_ir_func_destroy(cache->entries[i].func);
    free(cache);
}

static void wow64_process_ir_cache_reset(hb_wow64_process_t* process) {
    if (!process || !process->ir_cache) return;
    wow64_ir_cache_destroy((hb_wow64_ir_cache_t*)process->ir_cache);
    process->ir_cache = NULL;
}

static bool func_ends_in_control_transfer(const hb_ir_func_t* func) {
    if (!func || !func->cfg || !func->cfg->entry) return false;
    hb_ir_block_t* block = func->cfg->entry;
    if (!block->instr_count) return false;

    switch (block->instrs[block->instr_count - 1].op) {
        case HB_IR_CALL:
        case HB_IR_RET:
        case HB_IR_JMP:
        case HB_IR_Jcc:
            return true;
        default:
            return false;
    }
}

static bool trace_wow64_pc_enabled(uint32_t pc) {
    const char* spec = getenv("MACRUNNER_HB_TRACE_PC");
    char* end = NULL;
    unsigned long start, stop;

    if (getenv("MACRUNNER_XTAJIT_TRACE_ALL_SIMULATE") && pc == 0x7bde072c)
        return true;

    if (!spec || !*spec) return false;
    start = strtoul(spec, &end, 0);
    if (end == spec) return false;
    if (*end == '-' || *end == ':') {
        stop = strtoul(end + 1, NULL, 0);
        return pc >= (uint32_t)start && pc <= (uint32_t)stop;
    }
    return pc == (uint32_t)start;
}

static void trace_wow64_bytes(uint32_t pc, const uint8_t* code, size_t len) {
    size_t n = len < 32 ? len : 32;

    fprintf(stderr, "macrunner-hb-wow64-sim: phase=decode pc=%08x len=%zu bytes=", pc, len);
    for (size_t i = 0; i < n; i++) fprintf(stderr, "%02x", code[i]);
    if (len > n) fprintf(stderr, "...");
    fputc('\n', stderr);
}

static bool trace_wcslen_pattern_enabled(uint32_t pc) {
    const char* spec = getenv("MACRUNNER_HB_TRACE_WCSLEN_PATTERN");
    char* end = NULL;
    unsigned long value;

    if (!spec || !*spec) return false;
    value = strtoul(spec, &end, 0);
    if (end == spec) return false;
    return pc == (uint32_t)value;
}

static void trace_wcslen_pattern(uint32_t pc, const uint8_t* code, size_t len, bool cached, bool matched) {
    static unsigned int count;
    size_t n;

    if (++count > 16) return;
    fprintf(stderr,
            "macrunner-hb-wow64-wcslen-pattern: pc=%08x cached=%u matched=%u len=%zu bytes=",
            pc, cached ? 1u : 0u, matched ? 1u : 0u, len);
    n = len < 48 ? len : 48;
    for (size_t i = 0; i < n; i++) fprintf(stderr, "%02x", code ? code[i] : 0);
    if (len > n) fprintf(stderr, "...");
    fputc('\n', stderr);
}

static bool is_wow64_bop_opcode(const uint8_t* code, size_t len) {
    return len >= 2 && code[0] == 0x0f && (code[1] == 0xff || code[1] == 0xfe);
}

static bool is_wine_x86_wcslen_pattern(const uint8_t* code, size_t len) {
    static const uint8_t prologue[] = {
        0x55,                               /* push ebp */
        0x89, 0xe5,                         /* mov ebp, esp */
        0xb8, 0xfe, 0xff, 0xff, 0xff,       /* mov eax, -2 */
        0x8b, 0x4d, 0x08                    /* mov ecx, [ebp+8] */
    };
    static const uint8_t loop[] = {
        0x66, 0x83, 0x7c, 0x01, 0x02, 0x00, /* cmp word [ecx+eax+2], 0 */
        0x8d, 0x40, 0x02,                   /* lea eax, [eax+2] */
        0x75, 0xf5,                         /* jne loop */
        0xd1, 0xf8,                         /* sar eax, 1 */
        0x5d,                               /* pop ebp */
        0xc3                                /* ret */
    };
    size_t pos = 0;

    if (len >= 2 && code[0] == 0x66 && code[1] == 0x90) pos = 2; /* xchg ax, ax */
    else if (len >= 2 && code[0] == 0x8b && code[1] == 0xff) pos = 2; /* mov edi, edi */

    if (len < pos + sizeof(prologue) + sizeof(loop)) return false;
    if (memcmp(code + pos, prologue, sizeof(prologue)) != 0) return false;
    pos += sizeof(prologue);

    while (pos < len) {
        if (len - pos >= sizeof(loop) && memcmp(code + pos, loop, sizeof(loop)) == 0)
            return true;

        if (code[pos] == 0x90) {
            pos++;
            continue;
        }
        if (len - pos >= 3 && code[pos] == 0x0f && code[pos + 1] == 0x1f) {
            pos += 3;
            continue;
        }
        if (len - pos >= 7 && code[pos] == 0x66 && code[pos + 1] == 0x66 &&
            code[pos + 2] == 0x66 && code[pos + 3] == 0x66 && code[pos + 4] == 0x66) {
            pos += 7;
            continue;
        }
        return false;
    }
    return false;
}

static bool is_wine_x86_rtl_query_env_pattern(const uint8_t* code, size_t len) {
    static const uint8_t prefix[] = {
        0x55,             /* push ebp */
        0x89, 0xe5,       /* mov ebp, esp */
        0x53,             /* push ebx */
        0x57,             /* push edi */
        0x56              /* push esi */
    };
    static const uint8_t loads[] = {
        0x8b, 0x5d, 0x10, /* mov ebx, [ebp+0x10] */
        0x8b, 0x75, 0x0c, /* mov esi, [ebp+0x0c] */
        0x8b, 0x7d, 0x08  /* mov edi, [ebp+0x08] */
    };
    size_t pos = 0;

    if (len >= 2 && code[0] == 0x66 && code[1] == 0x90) pos = 2;
    else if (len >= 2 && code[0] == 0x8b && code[1] == 0xff) pos = 2;

    if (len < pos + sizeof(prefix) + 1 + sizeof(loads)) return false;
    if (memcmp(code + pos, prefix, sizeof(prefix)) != 0) return false;
    pos += sizeof(prefix);
    if (len - pos >= 3 && code[pos] == 0x83 && code[pos + 1] == 0xec && code[pos + 2] == 0x10)
        pos += 3;
    else if (code[pos] == 0x50)
        pos += 1;
    else
        return false;
    return len >= pos + sizeof(loads) && memcmp(code + pos, loads, sizeof(loads)) == 0;
}

static bool is_wine_x86_strcmp_pattern(const uint8_t* code, size_t len) {
    static const uint8_t prologue[] = {
        0x55,             /* push ebp */
        0x89, 0xe5,       /* mov ebp, esp */
        0x8b, 0x45, 0x0c, /* mov eax, [ebp+0x0c] */
        0x8b, 0x55, 0x08, /* mov edx, [ebp+0x08] */
        0x0f, 0xb6, 0x0a  /* movzx ecx, byte [edx] */
    };
    size_t pos = 0;

    if (len >= 2 && code[0] == 0x66 && code[1] == 0x90) pos = 2;
    else if (len >= 2 && code[0] == 0x8b && code[1] == 0xff) pos = 2;

    return len >= pos + sizeof(prologue) && memcmp(code + pos, prologue, sizeof(prologue)) == 0;
}

static bool try_run_wine_x86_strcmp(hb_context_t* ctx, const uint8_t* code,
                                    size_t len, hb_exec_result_t* out) {
    uint32_t esp, ret_addr, lhs, rhs;
    int32_t result = 0;
    hb_memory_t* mem;

    if (!ctx || !ctx->memory || !out || !is_wine_x86_strcmp_pattern(code, len))
        return false;

    memset(out, 0, sizeof(*out));
    mem = ctx->memory;
    esp = ctx->regs.x86.esp;
    if (hb_memory_read_u32(mem, esp, &ret_addr) != HB_OK ||
        hb_memory_read_u32(mem, esp + 4u, &lhs) != HB_OK ||
        hb_memory_read_u32(mem, esp + 8u, &rhs) != HB_OK) {
        return false;
    }

    for (;;) {
        uint8_t a = 0, b = 0;

        if (hb_memory_read_u8(mem, lhs, &a) != HB_OK ||
            hb_memory_read_u8(mem, rhs, &b) != HB_OK) {
            return false;
        }
        if (a != b) {
            result = a > b ? 1 : -1;
            break;
        }
        if (!a) break;
        lhs++;
        rhs++;
    }

    ctx->regs.x86.eax = (uint32_t)result;
    ctx->regs.x86.esp = esp + 4u;
    ctx->pc = ret_addr;
    ctx->regs.x86.eip = ret_addr;
    out->result = HB_OK;
    out->steps_executed = 16;
    out->blocks_executed = 1;
    return true;
}

static uint16_t wow64_fold_wchar(uint16_t ch) {
    if (ch >= 'a' && ch <= 'z') return (uint16_t)(ch - ('a' - 'A'));
    return ch;
}

static bool wow64_guest_wcslen(hb_memory_t* mem, uint32_t ptr, uint32_t* out_chars) {
    uint32_t chars = 0;

    if (!mem || !out_chars) return false;
    for (;;) {
        uint16_t ch = 0;
        if (chars >= 0x100000u) return false;
        if (hb_memory_read_u16(mem, ptr + chars * 2u, &ch) != HB_OK) return false;
        if (!ch) {
            *out_chars = chars;
            return true;
        }
        chars++;
    }
}

static bool wow64_find_env_value(hb_memory_t* mem, uint32_t env, uint32_t name,
                                 uint32_t name_chars, uint32_t* value,
                                 uint32_t* value_chars) {
    uint32_t var = env;

    if (!mem || !env || !name || !name_chars || !value || !value_chars) return false;

    for (;;) {
        uint32_t len = 0;
        uint32_t first_eq = UINT32_MAX;
        bool same = true;

        if (!wow64_guest_wcslen(mem, var, &len)) return false;
        if (!len) return false;

        for (uint32_t i = 1; i < len; i++) {
            uint16_t ch = 0;
            if (hb_memory_read_u16(mem, var + i * 2u, &ch) != HB_OK) return false;
            if (ch == '=') {
                first_eq = i;
                break;
            }
        }
        if (len > name_chars && first_eq == name_chars) {
            for (uint32_t i = 0; i < name_chars; i++) {
                uint16_t lhs = 0, rhs = 0;
                if (hb_memory_read_u16(mem, var + i * 2u, &lhs) != HB_OK ||
                    hb_memory_read_u16(mem, name + i * 2u, &rhs) != HB_OK) {
                    return false;
                }
                if (wow64_fold_wchar(lhs) != wow64_fold_wchar(rhs)) {
                    same = false;
                    break;
                }
            }
            if (same) {
                *value = var + (name_chars + 1u) * 2u;
                return wow64_guest_wcslen(mem, *value, value_chars);
            }
        }
        var += (len + 1u) * 2u;
    }
}

static bool wow64_copy_guest_wstr(hb_memory_t* mem, uint32_t dst, uint32_t src, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        uint8_t byte = 0;
        if (hb_memory_read_u8(mem, src + i, &byte) != HB_OK ||
            hb_memory_write_u8(mem, dst + i, byte) != HB_OK) {
            return false;
        }
    }
    return true;
}

static bool try_run_wine_x86_rtl_query_env(hb_context_t* ctx, const uint8_t* code,
                                           size_t len, hb_exec_result_t* out) {
    uint32_t esp, ret_addr, env, name, value;
    uint16_t name_len = 0, value_max = 0;
    uint32_t name_buf = 0, value_buf = 0, name_chars, found_value = 0, found_chars = 0;
    uint32_t status = HB_WOW64CPU_STATUS_VARIABLE_NOT_FOUND;
    hb_memory_t* mem;

    if (!ctx || !ctx->memory || !out || !is_wine_x86_rtl_query_env_pattern(code, len))
        return false;

    memset(out, 0, sizeof(*out));
    mem = ctx->memory;
    esp = ctx->regs.x86.esp;
    if (hb_memory_read_u32(mem, esp, &ret_addr) != HB_OK ||
        hb_memory_read_u32(mem, esp + 4u, &env) != HB_OK ||
        hb_memory_read_u32(mem, esp + 8u, &name) != HB_OK ||
        hb_memory_read_u32(mem, esp + 12u, &value) != HB_OK ||
        hb_memory_read_u16(mem, name, &name_len) != HB_OK ||
        hb_memory_read_u32(mem, name + 4u, &name_buf) != HB_OK ||
        hb_memory_write_u16(mem, value, 0) != HB_OK) {
        return false;
    }

    name_chars = (uint32_t)name_len / 2u;
    if (name_chars) {
        if (!env) {
            uint32_t peb = 0, params = 0;
            if (hb_memory_read_u32(mem, (uint32_t)ctx->fs_base + HB_WOW64CPU_TEB32_PEB, &peb) != HB_OK ||
                hb_memory_read_u32(mem, peb + HB_WOW64CPU_PEB32_PROCESS_PARAMETERS, &params) != HB_OK ||
                hb_memory_read_u32(mem, params + HB_WOW64CPU_RTL_USER_PROCESS_PARAMETERS32_ENVIRONMENT, &env) != HB_OK) {
                return false;
            }
        }
        if (wow64_find_env_value(mem, env, name_buf, name_chars, &found_value, &found_chars)) {
            uint32_t value_len = found_chars * 2u;

            if (hb_memory_read_u16(mem, value + 2u, &value_max) != HB_OK ||
                hb_memory_read_u32(mem, value + 4u, &value_buf) != HB_OK ||
                hb_memory_write_u16(mem, value, (uint16_t)value_len) != HB_OK) {
                return false;
            }
            if (value_len <= value_max) {
                uint32_t copy_bytes = value_len + 2u;
                if (copy_bytes > value_max) copy_bytes = value_max;
                if (copy_bytes && !wow64_copy_guest_wstr(mem, value_buf, found_value, copy_bytes))
                    return false;
                status = 0;
            } else {
                status = HB_WOW64CPU_STATUS_BUFFER_TOO_SMALL;
            }
        }
    }

    ctx->regs.x86.eax = status;
    ctx->regs.x86.esp = esp + 16u;
    ctx->pc = ret_addr;
    ctx->regs.x86.eip = ret_addr;
    out->result = HB_OK;
    out->steps_executed = 32;
    out->blocks_executed = 1;
    return true;
}

static bool try_run_wine_x86_wcslen(hb_context_t* ctx, const uint8_t* code,
                                    size_t len, hb_exec_result_t* out) {
    uint32_t esp, ret_addr, str_addr, byte_len;
    uint32_t chars = 0;
    hb_result_t r;

    if (!ctx || !ctx->memory || !out || !is_wine_x86_wcslen_pattern(code, len))
        return false;

    memset(out, 0, sizeof(*out));
    esp = ctx->regs.x86.esp;
    r = hb_memory_read_u32(ctx->memory, esp, &ret_addr);
    if (r == HB_OK) r = hb_memory_read_u32(ctx->memory, esp + 4u, &str_addr);
    if (r != HB_OK) {
        ctx->last_result = r;
        out->result = r;
        out->faulted = true;
        out->fault_reason = "wcslen intrinsic stack read fault";
        return true;
    }

    for (;;) {
        uint16_t ch = 0;
        uint32_t addr = str_addr + chars * 2u;

        r = hb_memory_read_u16(ctx->memory, addr, &ch);
        if (r != HB_OK) {
            ctx->last_result = r;
            out->result = r;
            out->faulted = true;
            out->fault_reason = "wcslen intrinsic string read fault";
            return true;
        }
        if (!ch) break;
        chars++;
    }

    byte_len = chars * 2u;
    ctx->regs.x86.eax = chars;
    ctx->regs.x86.ecx = str_addr;
    ctx->regs.x86.esp = esp + 4u;
    ctx->pc = ret_addr;
    ctx->regs.x86.eip = ret_addr;
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_SAR, HB_SIZE_32, byte_len, 1, chars, 1);

    out->result = HB_OK;
    out->steps_executed = 8;
    out->blocks_executed = 1;
    return true;
}

static uint64_t wow64_simulate_block_limit(void) {
    const char* value = getenv("MACRUNNER_HB_WOW64_BLOCK_LIMIT");
    char* end = NULL;
    unsigned long long parsed;

    if (!value || !*value) return 0;
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == value || (end && *end != '\0')) return 0;
    return (uint64_t)parsed;
}

static bool valid_process(const hb_wow64_process_t* process) {
    return process &&
           process->size >= sizeof(*process) &&
           process->version == HB_WOW64CPU_ABI_VERSION &&
           process->memory &&
           process->guest32_base;
}

static bool valid_thread(const hb_wow64_thread_t* thread) {
    return thread &&
           thread->size >= sizeof(*thread) &&
           thread->version == HB_WOW64CPU_ABI_VERSION &&
           thread->machine == HB_WOW64_MACHINE_I386 &&
           thread->ctx;
}

hb_result_t hb_wow64cpu_process_init(hb_wow64_process_t* process) {
    hb_memory_t* external_memory;

    if (!process || process->size < sizeof(*process)) return HB_ERR_INVALID_ARG;
    external_memory = process->memory;
    memset((uint8_t*)process + offsetof(hb_wow64_process_t, version), 0,
           sizeof(*process) - offsetof(hb_wow64_process_t, version));

    process->version = HB_WOW64CPU_ABI_VERSION;
    process->memory = external_memory ? external_memory : hb_memory_create(0);
    if (!process->memory) return HB_ERR_OUT_OF_MEMORY;
    process->owns_memory = external_memory ? 0u : 1u;
    if (hb_memory_guest32_reserve(process->memory) != HB_OK) {
        if (process->owns_memory) hb_memory_destroy(process->memory);
        process->memory = NULL;
        return HB_ERR_OUT_OF_MEMORY;
    }

    process->guest32_base = hb_memory_guest32_base(process->memory);
    process->bop_code[0] = 0x0f;
    process->bop_code[1] = 0xff;
    process->bop_size = sizeof(process->bop_code);
    return HB_OK;
}

void hb_wow64cpu_process_destroy(hb_wow64_process_t* process) {
    if (!process) return;
    wow64_process_ir_cache_reset(process);
    if (process->memory && process->owns_memory) hb_memory_destroy(process->memory);
    process->memory = NULL;
    process->guest32_base = NULL;
    process->owns_memory = 0;
}

hb_result_t hb_wow64cpu_thread_init(hb_wow64_process_t* process, hb_wow64_thread_t* thread) {
    hb_context_t* ctx;

    if (!thread || thread->size < sizeof(*thread) || !valid_process(process)) return HB_ERR_INVALID_ARG;
    memset((uint8_t*)thread + offsetof(hb_wow64_thread_t, version), 0,
           sizeof(*thread) - offsetof(hb_wow64_thread_t, version));

    ctx = hb_context_create(HB_ARCH_X86, HB_BACKEND_INTERP);
    if (!ctx) return HB_ERR_OUT_OF_MEMORY;
    ctx->memory = process->memory;
    ctx->guest32_base = (uint64_t)(uintptr_t)hb_memory_guest32_base(process->memory);

    thread->version = HB_WOW64CPU_ABI_VERSION;
    thread->machine = HB_WOW64_MACHINE_I386;
    thread->process = process;
    thread->ctx = ctx;
    return HB_OK;
}

void hb_wow64cpu_thread_destroy(hb_wow64_thread_t* thread) {
    if (!thread) return;
    if (thread->jit_rt) {
        hb_jit_runtime_destroy(thread->jit_rt);
        thread->jit_rt = NULL;
    }
    if (thread->ctx) {
        thread->ctx->memory = NULL;
        hb_context_destroy(thread->ctx);
    }
    thread->ctx = NULL;
    thread->process = NULL;
}

hb_result_t hb_wow64cpu_get_bop_code(const hb_wow64_process_t* process,
                                     const uint8_t** code,
                                     uint32_t* size) {
    if (!valid_process(process) || !code || !size) return HB_ERR_INVALID_ARG;
    *code = process->bop_code;
    *size = process->bop_size;
    return HB_OK;
}

hb_result_t hb_wow64cpu_import_i386_context(hb_wow64_thread_t* thread,
                                            const hb_wow64_i386_context_t* in) {
    hb_context_t* ctx;

    if (!valid_thread(thread) || !in ||
        in->size < sizeof(*in) ||
        in->version != HB_WOW64CPU_ABI_VERSION) return HB_ERR_INVALID_ARG;

    ctx = thread->ctx;
    ctx->regs.x86.eax = in->eax;
    ctx->regs.x86.ebx = in->ebx;
    ctx->regs.x86.ecx = in->ecx;
    ctx->regs.x86.edx = in->edx;
    ctx->regs.x86.esi = in->esi;
    ctx->regs.x86.edi = in->edi;
    ctx->regs.x86.esp = in->esp;
    ctx->regs.x86.ebp = in->ebp;
    ctx->regs.x86.eip = in->eip;
    ctx->regs.x86.eflags = in->eflags;
    ctx->regs.x86.x87 = in->x87;
    ctx->pc = in->eip;
    ctx->fs_base = in->fs_base;
    ctx->gs_base = in->gs_base;
    ctx->seg_cs = in->seg_cs;
    ctx->seg_ds = in->seg_ds;
    ctx->seg_es = in->seg_es;
    ctx->seg_fs = in->seg_fs;
    ctx->seg_gs = in->seg_gs;
    ctx->seg_ss = in->seg_ss;
    return HB_OK;
}

hb_result_t hb_wow64cpu_export_i386_context(const hb_wow64_thread_t* thread,
                                            hb_wow64_i386_context_t* out) {
    const hb_context_t* ctx;

    if (!valid_thread(thread) || !out || out->size < sizeof(*out)) return HB_ERR_INVALID_ARG;
    ctx = thread->ctx;
    memset((uint8_t*)out + offsetof(hb_wow64_i386_context_t, version), 0,
           sizeof(*out) - offsetof(hb_wow64_i386_context_t, version));
    out->version = HB_WOW64CPU_ABI_VERSION;
    out->eax = ctx->regs.x86.eax;
    out->ebx = ctx->regs.x86.ebx;
    out->ecx = ctx->regs.x86.ecx;
    out->edx = ctx->regs.x86.edx;
    out->esi = ctx->regs.x86.esi;
    out->edi = ctx->regs.x86.edi;
    out->esp = ctx->regs.x86.esp;
    out->ebp = ctx->regs.x86.ebp;
    out->eip = ctx->regs.x86.eip;
    out->eflags = ctx->regs.x86.eflags;
    out->fs_base = (uint32_t)ctx->fs_base;
    out->gs_base = (uint32_t)ctx->gs_base;
    out->seg_cs = ctx->seg_cs;
    out->seg_ds = ctx->seg_ds;
    out->seg_es = ctx->seg_es;
    out->seg_fs = ctx->seg_fs;
    out->seg_gs = ctx->seg_gs;
    out->seg_ss = ctx->seg_ss;
    out->x87 = ctx->regs.x86.x87;
    return HB_OK;
}

hb_result_t hb_wow64cpu_simulate(hb_wow64_thread_t* thread,
                                 hb_backend_t backend,
                                 size_t max_code_bytes,
                                 hb_exec_result_t* out) {
    uint8_t code[HB_WOW64CPU_DEFAULT_MAX_CODE_BYTES];
    hb_context_t* ctx;
    uint64_t total_steps = 0;
    uint64_t total_blocks = 0;
    uint64_t dispatched = 0;
    uint64_t block_limit;
    bool ran_block = false;
    hb_wow64_ir_cache_t* ir_cache;
    hb_result_t result = HB_OK;

    if (!valid_thread(thread) || !out) return HB_ERR_INVALID_ARG;
    ctx = thread->ctx;
    if (!valid_process(thread->process) || ctx->arch != HB_ARCH_X86 ||
        ctx->mode != HB_MODE_32BIT || ctx->memory != thread->process->memory) {
        return HB_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (backend == HB_BACKEND_AOT) backend = HB_BACKEND_INTERP;
    if (max_code_bytes == 0 || max_code_bytes > sizeof(code)) max_code_bytes = sizeof(code);
    block_limit = wow64_simulate_block_limit();

    ir_cache = thread->process->ir_cache;
    if (!ir_cache) {
        ir_cache = calloc(1, sizeof(*ir_cache));
        thread->process->ir_cache = ir_cache;
    }

    while (!block_limit || dispatched < block_limit) {
        hb_decoder_t* dec = NULL;
        hb_ir_func_t* func = NULL;
        hb_exec_result_t block_out;
        hb_result_t r;
        uint32_t pc = ctx->regs.x86.eip;
        size_t len = 0;
        bool chain;
        bool trace_pc = trace_wow64_pc_enabled(pc);
        bool transient_func = false;

        ctx->pc = pc;
        if (trace_pc)
        {
            fprintf(stderr, "macrunner-hb-wow64-sim: phase=block-start dispatched=%llu pc=%08x esp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x eflags=%08x\n",
                    (unsigned long long)dispatched, pc, ctx->regs.x86.esp, ctx->regs.x86.eax,
                    ctx->regs.x86.ebx, ctx->regs.x86.ecx, ctx->regs.x86.edx,
                    ctx->regs.x86.esi, ctx->regs.x86.edi, ctx->regs.x86.ebp,
                    ctx->regs.x86.eflags);
            fflush(stderr);
        }
        func = wow64_ir_cache_find(ir_cache, pc);
        if (func && trace_wcslen_pattern_enabled(pc))
            trace_wcslen_pattern(pc, NULL, 0, true, false);
        if (!func) {
            hb_region_t* region = hb_memory_find_region(ctx->memory, pc);
            if (region && (region->perm & HB_PERM_EXEC) && pc >= region->base && pc < region->base + region->size) {
                size_t chunk = (size_t)(region->base + region->size - pc);
                if (chunk > max_code_bytes) chunk = max_code_bytes;
                r = hb_memory_read(ctx->memory, pc, code, chunk);
                if (r == HB_OK) len = chunk;
            }
            /* If the region read stopped at a page boundary mid-instruction,
             * top up byte-by-byte across the boundary (x86 insns up to 15 B). */
            if (len < 15) {
                while (len < max_code_bytes) {
                    uint32_t guest = pc + (uint32_t)len;
                    if (!hb_memory_can_exec(ctx->memory, guest, 1)) break;
                    r = hb_memory_fetch(ctx->memory, guest, &code[len]);
                    if (r != HB_OK) break;
                    len++;
                }
            }
            if (!len) {
                if (ran_block) break;
                ctx->last_result = HB_ERR_MEMORY_FAULT;
                out->result = HB_ERR_MEMORY_FAULT;
                out->faulted = true;
                out->fault_reason = "unable to fetch executable i386 code";
                result = HB_ERR_MEMORY_FAULT;
                goto done;
            }
            if (is_wow64_bop_opcode(code, len)) break;
            if (trace_wcslen_pattern_enabled(pc))
                trace_wcslen_pattern(pc, code, len, false, is_wine_x86_wcslen_pattern(code, len));
            if (try_run_wine_x86_rtl_query_env(ctx, code, len, &block_out)) {
                ran_block = true;
                dispatched++;
                total_steps += block_out.steps_executed;
                total_blocks += block_out.blocks_executed;
                continue;
            }
            if (try_run_wine_x86_strcmp(ctx, code, len, &block_out)) {
                ran_block = true;
                dispatched++;
                total_steps += block_out.steps_executed;
                total_blocks += block_out.blocks_executed;
                continue;
            }
            if (try_run_wine_x86_wcslen(ctx, code, len, &block_out)) {
                if (block_out.faulted) {
                    *out = block_out;
                    result = block_out.result;
                    goto done;
                }
                ran_block = true;
                dispatched++;
                total_steps += block_out.steps_executed;
                total_blocks += block_out.blocks_executed;
                continue;
            }
            if (trace_pc)
            {
                trace_wow64_bytes(pc, code, len);
                fflush(stderr);
            }

            dec = hb_decoder_create(HB_ARCH_X86, code, len, pc);
            if (!dec) {
                result = HB_ERR_OUT_OF_MEMORY;
                goto done;
            }
            r = hb_lift_func_x86(dec, &func);
            hb_decoder_destroy(dec);
            if (r != HB_OK) {
                result = r;
                goto done;
            }
            if (!wow64_ir_cache_put(ir_cache, pc, func)) transient_func = true;
        }

        chain = func_ends_in_control_transfer(func);
        memset(&block_out, 0, sizeof(block_out));
        if (backend == HB_BACKEND_JIT || backend == HB_BACKEND_AOT) {
            if (!thread->jit_rt) {
                thread->jit_rt = hb_jit_runtime_create(ctx);
                if (!thread->jit_rt) { result = HB_ERR_OUT_OF_MEMORY; goto done; }
            }
            r = hb_jit_runtime_run(thread->jit_rt, func, &block_out);
        } else {
            r = hb_runtime_run(ctx, func, backend, &block_out);
        }
        if (trace_pc)
        {
            fprintf(stderr, "macrunner-hb-wow64-sim: phase=block-done dispatched=%llu pc=%08x r=%d out=%d faulted=%u steps=%llu blocks=%llu next=%08x esp=%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x eflags=%08x\n",
                    (unsigned long long)dispatched, pc, r, block_out.result, block_out.faulted,
                    (unsigned long long)block_out.steps_executed,
                    (unsigned long long)block_out.blocks_executed,
                    ctx->regs.x86.eip, ctx->regs.x86.esp,
                    ctx->regs.x86.eax, ctx->regs.x86.ebx, ctx->regs.x86.ecx,
                    ctx->regs.x86.edx, ctx->regs.x86.esi, ctx->regs.x86.edi,
                    ctx->regs.x86.ebp, ctx->regs.x86.eflags);
            fflush(stderr);
        }
        if (transient_func) hb_ir_func_destroy(func);

        ran_block = true;
        dispatched++;
        total_steps += block_out.steps_executed;
        total_blocks += block_out.blocks_executed;

        if (r != HB_OK || block_out.result != HB_OK || block_out.faulted) {
            *out = block_out;
            out->steps_executed = total_steps;
            out->blocks_executed = total_blocks;
            result = r;
            goto done;
        }
        if (!chain) break;
    }

    out->result = HB_OK;
    out->steps_executed = total_steps;
    out->blocks_executed = total_blocks;
done:
    return result;
}

hb_result_t hb_wow64cpu_notify_memory_alloc(hb_wow64_process_t* process,
                                            uint32_t base,
                                            size_t size,
                                            hb_perm_t perm) {
    if (!valid_process(process)) return HB_ERR_INVALID_ARG;
    wow64_process_ir_cache_reset(process);
    return hb_memory_guest32_map(process->memory, base, size, perm);
}

hb_result_t hb_wow64cpu_notify_memory_protect(hb_wow64_process_t* process,
                                              uint32_t base,
                                              size_t size,
                                              hb_perm_t perm) {
    if (!valid_process(process)) return HB_ERR_INVALID_ARG;
    wow64_process_ir_cache_reset(process);
    return hb_memory_guest32_protect(process->memory, base, size, perm);
}

hb_result_t hb_wow64cpu_notify_memory_free(hb_wow64_process_t* process,
                                           uint32_t base,
                                           size_t size) {
    if (!valid_process(process)) return HB_ERR_INVALID_ARG;
    wow64_process_ir_cache_reset(process);
    return hb_memory_guest32_unmap(process->memory, base, size);
}
