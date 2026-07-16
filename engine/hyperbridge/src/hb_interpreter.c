#include "hb_runtime.h"
#include "hb_memory.h"
#include "hb_flags.h"
#include "hb_x87.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

static const char* ir_op_name(hb_ir_op_t op);

static _Thread_local const hb_ir_instr_t* trace_current_instr;
static _Thread_local unsigned int trace_runtime_flags;

enum {
    TRACE_FLAG_MEM_WATCH     = 1u << 0,
    TRACE_FLAG_NATIVE_WRITES = 1u << 1,
    TRACE_FLAG_STACK_FAULTS  = 1u << 2,
    TRACE_FLAG_ATOMICS       = 1u << 3,
    TRACE_FLAG_FAULTS        = 1u << 4,
    TRACE_FLAG_PC            = 1u << 5,
    TRACE_FLAG_STRCPY        = 1u << 6,
    TRACE_FLAG_BRANCHES      = 1u << 7,
    TRACE_FLAG_SIMD          = 1u << 8,
    TRACE_FLAG_SIMD_DATA     = 1u << 9,
    TRACE_FLAG_BITOPS        = 1u << 10,
};

static inline void sync_arch_pc(hb_context_t* ctx) {
    if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
    else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
}

static bool trace_env_enabled(const char* name) {
    const char* val = getenv(name);
    return val && val[0] && val[0] != '0';
}

/* MacRunner (2026-06-17 — getenv-per-instruction storm fix, Lane A HK profiled run):
 * the per-instruction interpreter trace gates below used to call getenv() on EVERY
 * interpreted instruction even with tracing OFF (e.g. trace_bitops_selected read
 * BITOPS_START/END before checking its enable flag). getenv() takes the libc environ
 * lock (__findenv_locked); under the multi-thread interpreter fallback that kicks in
 * when the JIT code cache fills, that became an _os_unfair_lock_lock_slow/__ulock_wait2
 * contention storm burning ~66% of CPU (HK livelocked at D3D11-device-created, never
 * reaching swapchain). Fix: snapshot every trace env var ONCE into trace_cfg and read
 * the cache thereafter. Values reflect startup env (these are debug knobs set before
 * launch; runtime env changes are intentionally not re-read). */
typedef struct {
    unsigned int flags;                                  /* TRACE_FLAG_* bitset */
    bool mw_range_set;        uint64_t mw_start, mw_end;
    bool mw_pc_set;           uint64_t mw_pc_start, mw_pc_end;
    unsigned int mw_budget;
    bool bitops_range_set;    uint64_t bitops_start, bitops_end;
    unsigned int bitops_budget;
    bool branch_range_set;    uint64_t branch_start, branch_end;
    unsigned int branch_budget;
    bool pc_list_set;         char pc_list[256];
    unsigned int pc_limit;
    unsigned int strcpy_limit;
    unsigned int simd_budget;
    bool simd_data_range_set; uint64_t simd_data_start, simd_data_end;
    unsigned int simd_data_budget;
} hb_trace_cfg_t;

static hb_trace_cfg_t trace_cfg;   /* zero-init => tracing OFF / no range = the safe default */

static void hb_trace_parse_range(const char* sname, const char* ename,
                                 bool* set, uint64_t* lo, uint64_t* hi) {
    const char* s = getenv(sname);
    const char* e = getenv(ename);
    if (s && s[0]) {
        *set = true;
        *lo = strtoull(s, NULL, 0);
        *hi = (e && e[0]) ? strtoull(e, NULL, 0) : *lo;
        if (*hi < *lo) *hi = *lo;
    }
}

/* clamp=true keeps the original "0<p<100000 else default" rule (mem-watch/simd budgets);
 * clamp=false keeps the original "p ? p : default" rule (bitops/branch budgets). */
static unsigned int hb_trace_parse_budget(const char* name, unsigned int dflt, bool clamp) {
    const char* v = getenv(name);
    if (v && v[0]) {
        unsigned long p = strtoul(v, NULL, 0);
        if (clamp) { if (p > 0 && p < 100000) return (unsigned int)p; }
        else       { if (p) return (unsigned int)p; }
    }
    return dflt;
}

static void trace_cfg_load(hb_trace_cfg_t* c) {
    unsigned int flags = 0;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_MEM_WATCH")) flags |= TRACE_FLAG_MEM_WATCH;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_NATIVE_WRITES")) flags |= TRACE_FLAG_NATIVE_WRITES;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_STACK_FAULTS")) flags |= TRACE_FLAG_STACK_FAULTS;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_ATOMICS")) flags |= TRACE_FLAG_ATOMICS;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_FAULTS")) flags |= TRACE_FLAG_FAULTS;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_PC")) flags |= TRACE_FLAG_PC;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_STRCPY_PROBE")) flags |= TRACE_FLAG_STRCPY;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_BRANCHES")) flags |= TRACE_FLAG_BRANCHES;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_SIMD")) flags |= TRACE_FLAG_SIMD;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_SIMD_DATA")) flags |= TRACE_FLAG_SIMD_DATA;
    if (trace_env_enabled("MACRUNNER_HB_TRACE_BITOPS")) flags |= TRACE_FLAG_BITOPS;
    c->flags = flags;

    hb_trace_parse_range("MACRUNNER_HB_TRACE_MEM_WATCH_START", "MACRUNNER_HB_TRACE_MEM_WATCH_END",
                         &c->mw_range_set, &c->mw_start, &c->mw_end);
    hb_trace_parse_range("MACRUNNER_HB_TRACE_MEM_WATCH_PC_START", "MACRUNNER_HB_TRACE_MEM_WATCH_PC_END",
                         &c->mw_pc_set, &c->mw_pc_start, &c->mw_pc_end);
    c->mw_budget = hb_trace_parse_budget("MACRUNNER_HB_TRACE_MEM_WATCH_BUDGET", 160, true);

    hb_trace_parse_range("MACRUNNER_HB_TRACE_BITOPS_START", "MACRUNNER_HB_TRACE_BITOPS_END",
                         &c->bitops_range_set, &c->bitops_start, &c->bitops_end);
    c->bitops_budget = hb_trace_parse_budget("MACRUNNER_HB_TRACE_BITOPS_BUDGET", 400, false);

    hb_trace_parse_range("MACRUNNER_HB_TRACE_BRANCH_START", "MACRUNNER_HB_TRACE_BRANCH_END",
                         &c->branch_range_set, &c->branch_start, &c->branch_end);
    c->branch_budget = hb_trace_parse_budget("MACRUNNER_HB_TRACE_BRANCH_BUDGET", 300, false);

    {
        const char* v = getenv("MACRUNNER_HB_TRACE_PC");
        if (v && v[0]) {
            c->pc_list_set = true;
            strncpy(c->pc_list, v, sizeof(c->pc_list) - 1);
            c->pc_list[sizeof(c->pc_list) - 1] = 0;
        }
        const char* l = getenv("MACRUNNER_HB_TRACE_PC_LIMIT");
        c->pc_limit = (l && l[0]) ? (unsigned int)strtoul(l, NULL, 0) : 80;
    }
    {
        const char* l = getenv("MACRUNNER_HB_TRACE_STRCPY_PROBE_LIMIT");
        c->strcpy_limit = (l && l[0]) ? (unsigned int)strtoul(l, NULL, 0) : 200;
    }
    c->simd_budget = hb_trace_parse_budget("MACRUNNER_HB_TRACE_SIMD_BUDGET", 400, true);
    hb_trace_parse_range("MACRUNNER_HB_TRACE_SIMD_DATA_GUEST_START", "MACRUNNER_HB_TRACE_SIMD_DATA_GUEST_END",
                         &c->simd_data_range_set, &c->simd_data_start, &c->simd_data_end);
    c->simd_data_budget = hb_trace_parse_budget("MACRUNNER_HB_TRACE_SIMD_DATA_BUDGET", 256, true);
}

static int trace_cfg_ready;   /* 0=uninit, 1=loading, 2=ready */

static void trace_cfg_ensure(void) {
    int expected = 0;
    if (__atomic_load_n(&trace_cfg_ready, __ATOMIC_ACQUIRE) == 2) return;
    if (__atomic_compare_exchange_n(&trace_cfg_ready, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        trace_cfg_load(&trace_cfg);
        __atomic_store_n(&trace_cfg_ready, 2, __ATOMIC_RELEASE);
    }
    /* CAS loser: another thread is loading; trace_cfg stays zeroed (tracing OFF) for
     * this thread until it is published — harmless, the hot path only reads trace_cfg.flags. */
}

static void trace_refresh_runtime_flags(void) {
    trace_cfg_ensure();
    trace_runtime_flags = trace_cfg.flags;
}

static bool trace_mem_watch_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_MEM_WATCH) != 0;
}

static bool trace_mem_watch_range(uint64_t addr, size_t size, uint64_t* start, uint64_t* end) {
    uint64_t last;

    if (!trace_mem_watch_enabled() || !trace_cfg.mw_range_set) return false;
    *start = trace_cfg.mw_start;
    *end = trace_cfg.mw_end;
    last = size ? addr + size - 1 : addr;
    if (last < addr) return true;
    return addr <= *end && last >= *start;
}

static bool trace_mem_watch_take_budget(void) {
    static unsigned int count;
    unsigned int limit = trace_cfg.mw_budget;

    if (++count > limit) {
        if (count == limit + 1)
            fprintf(stderr, "macrunner-hb-mem-watch: budget exhausted, silencing\n");
        return false;
    }
    return true;
}

static bool trace_mem_watch_pc_allowed(uint64_t pc) {
    if (!trace_cfg.mw_pc_set) return true;
    return pc >= trace_cfg.mw_pc_start && pc <= trace_cfg.mw_pc_end;
}

static void trace_hex_bytes(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) fprintf(stderr, "%02x", bytes[i]);
}

static void trace_instr_bytes(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !ctx->memory || !instr) return;
    for (uint8_t i = 0; i < instr->guest_len && i < 15; i++) {
        uint8_t byte = 0;
        if (hb_memory_read_u8(ctx->memory, instr->guest_addr + i, &byte) == HB_OK)
            fprintf(stderr, "%02x", byte);
        else
            fprintf(stderr, "??");
    }
}

static void trace_mem_watch_bytes(hb_context_t* ctx, const char* phase, uint64_t addr,
                                  const void* data, size_t size, const void* before) {
    const hb_ir_instr_t* instr = trace_current_instr;
    uint64_t start = 0, end = 0;
    size_t count = size > 32 ? 32 : size;

    if (!ctx || !trace_mem_watch_range(addr, size, &start, &end)) return;
    if (!trace_mem_watch_pc_allowed(instr ? instr->guest_addr : 0)) return;
    if (!trace_mem_watch_take_budget()) return;

    fprintf(stderr,
            "macrunner-hb-mem-watch: phase=%s pc=0x%llx len=%u op=%s addr=0x%llx size=%zu "
            "watch=0x%llx-0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx rsi=0x%llx rdi=0x%llx "
            "bytes=",
            phase,
            instr ? (unsigned long long)instr->guest_addr : 0,
            instr ? instr->guest_len : 0,
            instr ? ir_op_name(instr->op) : "unknown",
            (unsigned long long)addr, size,
            (unsigned long long)start, (unsigned long long)end,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rdi);
    if (data && count) trace_hex_bytes((const uint8_t*)data, count);
    else fprintf(stderr, "-");
    if (before && count) {
        fprintf(stderr, " before=");
        trace_hex_bytes((const uint8_t*)before, count);
    }
    fprintf(stderr, " instr=");
    trace_instr_bytes(ctx, instr);
    fprintf(stderr, "\n");
}

static bool trace_native_write_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_NATIVE_WRITES) != 0;
}

static bool trace_native_write_addr(uint64_t addr, size_t size) {
    uint64_t last = addr;

    if (!trace_native_write_enabled()) return false;
    if (size) {
        last = addr + size - 1;
        if (last < addr) return true;
    }
    return addr < 0x7ffe0000000ULL && last >= 0x7ffd0000000ULL;
}

static void trace_guest_native_write(hb_context_t* ctx, const char* path,
                                     uint64_t addr, uint64_t val, hb_size_t sz) {
    uint64_t ret_addr = 0;
    uint8_t src[16] = {0};
    bool have_ret = false;
    bool have_src = false;

    if (!ctx || !trace_native_write_addr(addr, (size_t)sz)) return;
    if (ctx->mode == HB_MODE_64BIT && ctx->memory) {
        have_ret = hb_memory_read(ctx->memory, (hb_gva_t)ctx->regs.x64.rsp,
                                  &ret_addr, sizeof(ret_addr)) == HB_OK;
        have_src = ctx->regs.x64.rdx >= 0x10000 &&
                   hb_memory_read(ctx->memory, (hb_gva_t)ctx->regs.x64.rdx,
                                  src, sizeof(src)) == HB_OK;
    }
    fprintf(stderr,
            "macrunner-hb-guest-native-write: path=%s pc=0x%llx addr=0x%llx size=%u "
            "value=0x%llx rsp=0x%llx ret=%s0x%llx rax=0x%llx rdx=0x%llx "
            "r8=0x%llx r9=0x%llx rdi=0x%llx rsi=0x%llx rcx=0x%llx "
            "gs=0x%llx fs=0x%llx src16=%s%02x%02x%02x%02x%02x%02x%02x%02x"
            "%02x%02x%02x%02x%02x%02x%02x%02x\n",
            path,
            (unsigned long long)ctx->pc,
            (unsigned long long)addr,
            (unsigned)sz,
            (unsigned long long)val,
            (unsigned long long)ctx->regs.x64.rsp,
            have_ret ? "" : "invalid:",
            (unsigned long long)ret_addr,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)ctx->regs.x64.r8,
            (unsigned long long)ctx->regs.x64.r9,
            (unsigned long long)ctx->regs.x64.rdi,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->gs_base,
            (unsigned long long)ctx->fs_base,
            have_src ? "" : "invalid:",
            src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
            src[8], src[9], src[10], src[11], src[12], src[13], src[14], src[15]);
}

struct hb_interpreter {
    hb_context_t* ctx;
};

hb_interpreter_t* hb_interpreter_create(hb_context_t* ctx) {
    hb_interpreter_t* i = calloc(1, sizeof(hb_interpreter_t));
    if (!i) return NULL;
    i->ctx = ctx;
    return i;
}

void hb_interpreter_destroy(hb_interpreter_t* interp) {
    free(interp);
}

/* --- Register pointer helpers --- */
static uint64_t* reg_ptr_x64(hb_regs_x64_t* r, int idx) {
    switch (idx) {
        case 0: return &r->rax;
        case 1: return &r->rcx;
        case 2: return &r->rdx;
        case 3: return &r->rbx;
        case 4: return &r->rsp;
        case 5: return &r->rbp;
        case 6: return &r->rsi;
        case 7: return &r->rdi;
        case 8: return &r->r8;
        case 9: return &r->r9;
        case 10: return &r->r10;
        case 11: return &r->r11;
        case 12: return &r->r12;
        case 13: return &r->r13;
        case 14: return &r->r14;
        case 15: return &r->r15;
        case 16: return &r->rip;
        default: return NULL;
    }
}

static uint32_t* reg_ptr_x86(hb_regs_x86_t* r, int idx) {
    switch (idx) {
        case 0: return &r->eax;
        case 1: return &r->ecx;
        case 2: return &r->edx;
        case 3: return &r->ebx;
        case 4: return &r->esp;
        case 5: return &r->ebp;
        case 6: return &r->esi;
        case 7: return &r->edi;
        case 16: return &r->eip;
        default: return NULL;
    }
}

static uint64_t read_reg(hb_context_t* ctx, int idx) {
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t* p = reg_ptr_x86(&ctx->regs.x86, idx);
        return p ? *p : 0;
    }
    uint64_t* p = reg_ptr_x64(&ctx->regs.x64, idx);
    return p ? *p : 0;
}

static bool is_xmm_reg(int idx) {
    return idx >= HB_REG_XMM0 && idx <= HB_REG_XMM31;
}

static hb_result_t read_xmm_reg(hb_context_t* ctx, int idx, uint64_t out[2]) {
    if (!ctx || !out || !is_xmm_reg(idx)) return HB_ERR_INVALID_ARG;
    if (ctx->mode == HB_MODE_32BIT) {
        unsigned n = (unsigned)(idx - HB_REG_XMM0);
        if (n >= 8) return HB_ERR_INVALID_ARG;
        out[0] = ctx->regs.x86.xmm[n][0];
        out[1] = ctx->regs.x86.xmm[n][1];
        return HB_OK;
    }
    unsigned n = (unsigned)(idx - HB_REG_XMM0);
    if (n < 16) {
        out[0] = ctx->regs.x64.xmm[n][0];
        out[1] = ctx->regs.x64.xmm[n][1];
    } else {
        out[0] = ctx->xmm_ext[n - 16][0];
        out[1] = ctx->xmm_ext[n - 16][1];
    }
    return HB_OK;
}

static hb_result_t write_xmm_reg(hb_context_t* ctx, int idx, const uint64_t in[2]) {
    if (!ctx || !in || !is_xmm_reg(idx)) return HB_ERR_INVALID_ARG;
    if (ctx->mode == HB_MODE_32BIT) {
        unsigned n = (unsigned)(idx - HB_REG_XMM0);
        if (n >= 8) return HB_ERR_INVALID_ARG;
        ctx->regs.x86.xmm[n][0] = in[0];
        ctx->regs.x86.xmm[n][1] = in[1];
        return HB_OK;
    }
    unsigned n = (unsigned)(idx - HB_REG_XMM0);
    if (n < 16) {
        ctx->regs.x64.xmm[n][0] = in[0];
        ctx->regs.x64.xmm[n][1] = in[1];
    } else {
        ctx->xmm_ext[n - 16][0] = in[0];
        ctx->xmm_ext[n - 16][1] = in[1];
    }
    return HB_OK;
}

static hb_result_t read_vec_reg_bytes(hb_context_t* ctx, int idx, uint8_t* out, size_t bytes) {
    if (!ctx || !out || !is_xmm_reg(idx) || bytes > 64) return HB_ERR_INVALID_ARG;
    memset(out, 0, bytes);
    if (ctx->mode == HB_MODE_32BIT) {
        unsigned n = (unsigned)(idx - HB_REG_XMM0);
        if (n >= 8) return HB_ERR_INVALID_ARG;
        memcpy(out, ctx->regs.x86.xmm[n], bytes < 16 ? bytes : 16);
        if (bytes > 16) memcpy(out + 16, ctx->ymm_hi[n], bytes > 32 ? 16 : bytes - 16);
        if (bytes > 32) memcpy(out + 32, ctx->zmm_hi[n], bytes - 32);
        return HB_OK;
    }
    unsigned n = (unsigned)(idx - HB_REG_XMM0);
    if (n < 16) {
        memcpy(out, ctx->regs.x64.xmm[n], bytes < 16 ? bytes : 16);
        if (bytes > 16) memcpy(out + 16, ctx->ymm_hi[n], bytes > 32 ? 16 : bytes - 16);
        if (bytes > 32) memcpy(out + 32, ctx->zmm_hi[n], bytes - 32);
    } else {
        n -= 16;
        memcpy(out, ctx->xmm_ext[n], bytes < 16 ? bytes : 16);
        if (bytes > 16) memcpy(out + 16, ctx->ymm_hi_ext[n], bytes > 32 ? 16 : bytes - 16);
        if (bytes > 32) memcpy(out + 32, ctx->zmm_hi_ext[n], bytes - 32);
    }
    return HB_OK;
}

static hb_result_t write_vec_reg_bytes(hb_context_t* ctx, int idx, const uint8_t* in, size_t bytes) {
    if (!ctx || !in || !is_xmm_reg(idx) || bytes > 64) return HB_ERR_INVALID_ARG;
    uint8_t tmp[64] = {0};
    bool zero_ymm_upper = trace_current_instr && trace_current_instr->zero_ymm_upper && bytes <= 16;
    memcpy(tmp, in, bytes);
    if (ctx->mode == HB_MODE_32BIT) {
        unsigned n = (unsigned)(idx - HB_REG_XMM0);
        if (n >= 8) return HB_ERR_INVALID_ARG;
        if (bytes < 16) memcpy(ctx->regs.x86.xmm[n], tmp, bytes);
        else memcpy(ctx->regs.x86.xmm[n], tmp, 16);
        if (bytes > 16) memcpy(ctx->ymm_hi[n], tmp + 16, bytes > 32 ? 16 : bytes - 16);
        else if (zero_ymm_upper) {
            memset(ctx->ymm_hi[n], 0, sizeof(ctx->ymm_hi[n]));
            memset(ctx->zmm_hi[n], 0, sizeof(ctx->zmm_hi[n]));
        }
        if (bytes > 32) memcpy(ctx->zmm_hi[n], tmp + 32, bytes - 32);
        return HB_OK;
    }
    unsigned n = (unsigned)(idx - HB_REG_XMM0);
    if (n < 16) {
        if (bytes < 16) memcpy(ctx->regs.x64.xmm[n], tmp, bytes);
        else memcpy(ctx->regs.x64.xmm[n], tmp, 16);
        if (bytes > 16) memcpy(ctx->ymm_hi[n], tmp + 16, bytes > 32 ? 16 : bytes - 16);
        else if (zero_ymm_upper) {
            memset(ctx->ymm_hi[n], 0, sizeof(ctx->ymm_hi[n]));
            memset(ctx->zmm_hi[n], 0, sizeof(ctx->zmm_hi[n]));
        }
        if (bytes > 32) memcpy(ctx->zmm_hi[n], tmp + 32, bytes - 32);
    } else {
        n -= 16;
        if (bytes < 16) memcpy(ctx->xmm_ext[n], tmp, bytes);
        else memcpy(ctx->xmm_ext[n], tmp, 16);
        if (bytes > 16) memcpy(ctx->ymm_hi_ext[n], tmp + 16, bytes > 32 ? 16 : bytes - 16);
        else if (zero_ymm_upper) {
            memset(ctx->ymm_hi_ext[n], 0, sizeof(ctx->ymm_hi_ext[n]));
            memset(ctx->zmm_hi_ext[n], 0, sizeof(ctx->zmm_hi_ext[n]));
        }
        if (bytes > 32) memcpy(ctx->zmm_hi_ext[n], tmp + 32, bytes - 32);
    }
    return HB_OK;
}

#define HB_EVEX_TARGET_ARG_MASK   0x00ffffffu
#define HB_EVEX_TARGET_MASK_SHIFT 24
#define HB_EVEX_TARGET_ZERO_BIT   27
#define HB_EVEX_TARGET_PRESENT    (1u << 28)
#define HB_EVEX_ARG_BROADCAST     0x200u
#define HB_EVEX_ARG_ROUND_SHIFT   10
#define HB_EVEX_ARG_ROUND_MASK    0x1c00u

static uint32_t evex_target_arg(const hb_ir_instr_t* instr) {
    return (uint32_t)(instr->target & HB_EVEX_TARGET_ARG_MASK);
}

static bool evex_target_present(const hb_ir_instr_t* instr) {
    return (((uint32_t)instr->target) & HB_EVEX_TARGET_PRESENT) != 0;
}

static unsigned evex_target_mask(const hb_ir_instr_t* instr) {
    return (((uint32_t)instr->target) >> HB_EVEX_TARGET_MASK_SHIFT) & 7u;
}

static bool evex_target_zero(const hb_ir_instr_t* instr) {
    return (((uint32_t)instr->target) & (1u << HB_EVEX_TARGET_ZERO_BIT)) != 0;
}

static hb_result_t write_vec_reg_bytes_evex_masked(hb_context_t* ctx,
                                                   const hb_ir_instr_t* instr,
                                                   uint8_t* bytes_inout,
                                                   size_t bytes,
                                                   unsigned lane_bytes) {
    if (!evex_target_present(instr) || evex_target_mask(instr) == 0)
        return write_vec_reg_bytes(ctx, instr->dst.reg, bytes_inout, bytes);
    if (!ctx || !bytes_inout || instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg) ||
        lane_bytes == 0 || bytes > 64 || (bytes % lane_bytes) != 0)
        return HB_ERR_INTERNAL;

    uint8_t old[64] = {0};
    hb_result_t r = read_vec_reg_bytes(ctx, instr->dst.reg, old, bytes);
    if (r != HB_OK) return r;

    uint64_t k = ctx->k[evex_target_mask(instr) & 7u];
    bool zero = evex_target_zero(instr);
    unsigned lanes = (unsigned)(bytes / lane_bytes);
    for (unsigned lane = 0; lane < lanes; lane++) {
        size_t off = (size_t)lane * lane_bytes;
        if ((k >> lane) & 1u) continue;
        if (zero) memset(bytes_inout + off, 0, lane_bytes);
        else memcpy(bytes_inout + off, old + off, lane_bytes);
    }
    return write_vec_reg_bytes(ctx, instr->dst.reg, bytes_inout, bytes);
}

static hb_result_t write_vec_reg_bytes_evex_scalar_masked(hb_context_t* ctx,
                                                          const hb_ir_instr_t* instr,
                                                          uint8_t* bytes_inout,
                                                          unsigned lane_bytes) {
    if (!evex_target_present(instr) || evex_target_mask(instr) == 0)
        return write_vec_reg_bytes(ctx, instr->dst.reg, bytes_inout, 16);
    if (!ctx || !bytes_inout || instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg) ||
        !(lane_bytes == 4 || lane_bytes == 8))
        return HB_ERR_INTERNAL;

    uint64_t k = ctx->k[evex_target_mask(instr) & 7u];
    if (k & 1u) return write_vec_reg_bytes(ctx, instr->dst.reg, bytes_inout, 16);

    if (evex_target_zero(instr)) {
        memset(bytes_inout, 0, lane_bytes);
    } else {
        uint8_t old[16] = {0};
        hb_result_t r = read_vec_reg_bytes(ctx, instr->dst.reg, old, sizeof(old));
        if (r != HB_OK) return r;
        memcpy(bytes_inout, old, lane_bytes);
    }
    return write_vec_reg_bytes(ctx, instr->dst.reg, bytes_inout, 16);
}

static bool fp_cmp_predicate(bool unordered, int cmp_eq, int cmp_lt, int cmp_le, unsigned pred) {
    switch (pred & 31u) {
        case 0: case 16: return !unordered && cmp_eq;
        case 1: case 17: return !unordered && cmp_lt;
        case 2: case 18: return !unordered && cmp_le;
        case 3: case 19: return unordered;
        case 4: case 20: return unordered || !cmp_eq;
        case 5: case 21: return unordered || !cmp_lt;
        case 6: case 22: return unordered || !cmp_le;
        case 7: case 23: return !unordered;
        case 8: case 24: return unordered || cmp_eq;
        case 9: case 25: return unordered || !(cmp_eq || cmp_lt);
        case 10: case 26: return unordered || !(cmp_eq || cmp_lt || cmp_le);
        case 11: case 27: return false;
        case 12: case 28: return !unordered && !cmp_eq;
        case 13: case 29: return !unordered && (cmp_eq || !cmp_lt);
        case 14: case 30: return !unordered && !cmp_le;
        case 15: case 31: return true;
        default: return false;
    }
}

static size_t bytes_for_size(hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return 1;
        case HB_SIZE_16: return 2;
        case HB_SIZE_32: return 4;
        case HB_SIZE_64: return 8;
        case HB_SIZE_80: return 10;
        case HB_SIZE_128: return 16;
        case HB_SIZE_256: return 32;
        case HB_SIZE_512: return 64;
        default: return 0;
    }
}

static uint64_t resolve_addr(hb_context_t* ctx, const hb_ir_operand_t* op);
static hb_result_t mem_read(hb_context_t* ctx, uint64_t addr, uint64_t* out, hb_size_t sz);

static double hb_bits_to_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint64_t hb_double_to_bits(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float hb_bits_to_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t hb_float_to_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t hb_half_to_float_bits(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = ((uint32_t)h >> 10) & 0x1fu;
    uint32_t frac = (uint32_t)h & 0x03ffu;
    if (exp == 0) {
        if (frac == 0) return sign;
        int e = -14;
        while ((frac & 0x0400u) == 0) {
            frac <<= 1;
            e--;
        }
        frac &= 0x03ffu;
        return sign | (uint32_t)(e + 127) << 23 | (frac << 13);
    }
    if (exp == 0x1fu) return sign | 0x7f800000u | (frac << 13);
    return sign | ((exp + 112u) << 23) | (frac << 13);
}

static bool hb_round_shift_should_increment(uint64_t value, unsigned shift,
                                            unsigned mode, bool sign) {
    if (shift == 0) return false;
    uint64_t mask = (shift >= 64) ? UINT64_MAX : ((1ULL << shift) - 1ULL);
    uint64_t rem = value & mask;
    if (rem == 0) return false;
    if (mode == 1) return sign;      /* round toward -inf */
    if (mode == 2) return !sign;     /* round toward +inf */
    if (mode == 3) return false;     /* truncate */
    uint64_t half = 1ULL << (shift - 1);
    uint64_t lsb = (value >> shift) & 1ULL;
    return rem > half || (rem == half && lsb);
}

static uint16_t hb_float_bits_to_half(uint32_t bits, unsigned imm) {
    bool sign_bool = (bits & 0x80000000u) != 0;
    uint16_t sign = sign_bool ? 0x8000u : 0;
    unsigned mode = (imm & 0x04u) ? 0u : (imm & 0x03u); /* MXCSR is not modeled; use nearest-even. */
    uint32_t exp = (bits >> 23) & 0xffu;
    uint32_t frac = bits & 0x7fffffu;

    if (exp == 0xffu) {
        if (frac == 0) return (uint16_t)(sign | 0x7c00u);
        uint16_t payload = (uint16_t)(frac >> 13);
        if (payload == 0) payload = 1;
        return (uint16_t)(sign | 0x7c00u | payload | 0x0200u);
    }
    if (exp == 0) {
        if (frac == 0) return sign;
        if ((mode == 1 && sign_bool) || (mode == 2 && !sign_bool)) return (uint16_t)(sign | 1u);
        return sign;
    }

    int half_exp = (int)exp - 127 + 15;
    if (half_exp >= 31) {
        if (mode == 3 || (mode == 1 && !sign_bool) || (mode == 2 && sign_bool))
            return (uint16_t)(sign | 0x7bffu);
        return (uint16_t)(sign | 0x7c00u);
    }
    if (half_exp <= 0) {
        uint64_t sig = 0x800000u | frac;
        unsigned shift = (unsigned)(14 - half_exp);
        if (shift >= 64) {
            if ((mode == 1 && sign_bool) || (mode == 2 && !sign_bool)) return (uint16_t)(sign | 1u);
            return sign;
        }
        uint64_t mant = sig >> shift;
        if (shift < 64 && hb_round_shift_should_increment(sig, shift, mode, sign_bool)) mant++;
        if (mant >= 0x400u) return (uint16_t)(sign | 0x0400u);
        return (uint16_t)(sign | (uint16_t)mant);
    }

    uint16_t mant = (uint16_t)(frac >> 13);
    if (hb_round_shift_should_increment(frac, 13, mode, sign_bool)) {
        mant++;
        if (mant == 0x400u) {
            mant = 0;
            half_exp++;
            if (half_exp >= 31) return (uint16_t)(sign | 0x7c00u);
        }
    }
    return (uint16_t)(sign | ((uint16_t)half_exp << 10) | mant);
}

static int32_t hb_float_to_i32_sse(float value, bool truncate) {
    double rounded;
    if (!isfinite(value)) return INT32_MIN;
    rounded = truncate ? trunc((double)value) : nearbyint((double)value);
    if (rounded < -2147483648.0 || rounded >= 2147483648.0) return INT32_MIN;
    return (int32_t)rounded;
}

static int32_t hb_double_to_i32_sse(double value, bool truncate) {
    double rounded;
    if (!isfinite(value)) return INT32_MIN;
    rounded = truncate ? trunc(value) : nearbyint(value);
    if (rounded < -2147483648.0 || rounded >= 2147483648.0) return INT32_MIN;
    return (int32_t)rounded;
}

static int64_t hb_float_to_i64_sse(float value, bool truncate) {
    double rounded;
    if (!isfinite(value)) return INT64_MIN;
    rounded = truncate ? trunc((double)value) : nearbyint((double)value);
    if (rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0) return INT64_MIN;
    return (int64_t)rounded;
}

static int64_t hb_double_to_i64_sse(double value, bool truncate) {
    double rounded;
    if (!isfinite(value)) return INT64_MIN;
    rounded = truncate ? trunc(value) : nearbyint(value);
    if (rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0) return INT64_MIN;
    return (int64_t)rounded;
}

static hb_result_t read_scalar_double_bits(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t* out) {
    if (!ctx || !op || !out) return HB_ERR_INVALID_ARG;
    uint64_t bits = 0;
    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) {
        uint64_t xmm[2];
        hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
        if (r != HB_OK) return r;
        bits = xmm[0];
    } else if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        hb_result_t r = mem_read(ctx, addr, &bits, HB_SIZE_64);
        if (r != HB_OK) return r;
    } else {
        return HB_ERR_INTERNAL;
    }
    *out = bits;
    return HB_OK;
}

static hb_result_t read_scalar_double(hb_context_t* ctx, const hb_ir_operand_t* op, double* out) {
    uint64_t bits = 0;
    hb_result_t r = read_scalar_double_bits(ctx, op, &bits);
    if (r != HB_OK) return r;
    *out = hb_bits_to_double(bits);
    return HB_OK;
}

static hb_result_t write_scalar_double_bits(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t bits) {
    if (!ctx || !op || op->type != HB_OP_REG || !is_xmm_reg(op->reg)) return HB_ERR_INTERNAL;
    uint64_t xmm[2];
    hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
    if (r != HB_OK) return r;
    xmm[0] = bits;
    return write_xmm_reg(ctx, op->reg, xmm);
}

static hb_result_t write_scalar_double(hb_context_t* ctx, const hb_ir_operand_t* op, double value) {
    return write_scalar_double_bits(ctx, op, hb_double_to_bits(value));
}

static hb_result_t read_scalar_float_bits(hb_context_t* ctx, const hb_ir_operand_t* op, uint32_t* out) {
    if (!ctx || !op || !out) return HB_ERR_INVALID_ARG;
    uint32_t bits = 0;
    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) {
        uint64_t xmm[2];
        hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
        if (r != HB_OK) return r;
        bits = (uint32_t)xmm[0];
    } else if (op->type == HB_OP_MEM) {
        uint64_t raw = 0;
        uint64_t addr = resolve_addr(ctx, op);
        hb_result_t r = mem_read(ctx, addr, &raw, HB_SIZE_32);
        if (r != HB_OK) return r;
        bits = (uint32_t)raw;
    } else {
        return HB_ERR_INTERNAL;
    }
    *out = bits;
    return HB_OK;
}

static hb_result_t read_scalar_float(hb_context_t* ctx, const hb_ir_operand_t* op, float* out) {
    uint32_t bits = 0;
    hb_result_t r = read_scalar_float_bits(ctx, op, &bits);
    if (r != HB_OK) return r;
    *out = hb_bits_to_float(bits);
    return HB_OK;
}

static hb_result_t write_scalar_float_bits(hb_context_t* ctx, const hb_ir_operand_t* op, uint32_t bits) {
    if (!ctx || !op || op->type != HB_OP_REG || !is_xmm_reg(op->reg)) return HB_ERR_INTERNAL;
    uint64_t xmm[2];
    hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
    if (r != HB_OK) return r;
    xmm[0] = (xmm[0] & 0xffffffff00000000ULL) | bits;
    return write_xmm_reg(ctx, op->reg, xmm);
}

static hb_result_t write_scalar_float(hb_context_t* ctx, const hb_ir_operand_t* op, float value) {
    return write_scalar_float_bits(ctx, op, hb_float_to_bits(value));
}

static void write_scalar_compare_flags(hb_context_t* ctx, double lhs, double rhs) {
    hb_lazy_flags_clear(ctx);
    ctx->flags.of = false;
    ctx->flags.sf = false;
    ctx->flags.af = false;
    if (lhs != lhs || rhs != rhs) {
        ctx->flags.zf = true;
        ctx->flags.pf = true;
        ctx->flags.cf = true;
    } else {
        ctx->flags.zf = (lhs == rhs);
        ctx->flags.pf = false;
        ctx->flags.cf = (lhs < rhs);
    }
}

static float select_sse_minmax_float(float lhs, float rhs, bool is_max) {
    if (lhs != lhs || rhs != rhs) return rhs;
    return is_max ? (lhs > rhs ? lhs : rhs) : (lhs < rhs ? lhs : rhs);
}

static double select_sse_minmax_double(double lhs, double rhs, bool is_max) {
    if (lhs != lhs || rhs != rhs) return rhs;
    return is_max ? (lhs > rhs ? lhs : rhs) : (lhs < rhs ? lhs : rhs);
}

static bool hb_float_bits_is_nan(uint32_t bits) {
    return (bits & 0x7f800000u) == 0x7f800000u && (bits & 0x007fffffu) != 0;
}

static bool hb_float_bits_is_inf(uint32_t bits) {
    return (bits & 0x7fffffffu) == 0x7f800000u;
}

static bool hb_float_bits_is_zero(uint32_t bits) {
    return (bits & 0x7fffffffu) == 0;
}

static uint32_t hb_quiet_float_nan_bits(uint32_t bits) {
    return bits | 0x00400000u;
}

static bool hb_double_bits_is_nan(uint64_t bits) {
    return (bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
           (bits & 0x000fffffffffffffULL) != 0;
}

static bool hb_double_bits_is_inf(uint64_t bits) {
    return (bits & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL;
}

static bool hb_double_bits_is_zero(uint64_t bits) {
    return (bits & 0x7fffffffffffffffULL) == 0;
}

static uint64_t hb_quiet_double_nan_bits(uint64_t bits) {
    return bits | 0x0008000000000000ULL;
}

static bool hb_sse_arith_invalid_float(uint32_t lhs, uint32_t rhs, hb_ir_op_t op) {
    bool lhs_inf = hb_float_bits_is_inf(lhs), rhs_inf = hb_float_bits_is_inf(rhs);
    bool lhs_zero = hb_float_bits_is_zero(lhs), rhs_zero = hb_float_bits_is_zero(rhs);
    if (op == HB_IR_FADD) return lhs_inf && rhs_inf && ((lhs ^ rhs) & 0x80000000u);
    if (op == HB_IR_FSUB) return lhs_inf && rhs_inf && !((lhs ^ rhs) & 0x80000000u);
    if (op == HB_IR_FMUL) return (lhs_inf && rhs_zero) || (rhs_inf && lhs_zero);
    if (op == HB_IR_FDIV) return (lhs_inf && rhs_inf) || (lhs_zero && rhs_zero);
    return false;
}

static bool hb_sse_arith_invalid_double(uint64_t lhs, uint64_t rhs, hb_ir_op_t op) {
    bool lhs_inf = hb_double_bits_is_inf(lhs), rhs_inf = hb_double_bits_is_inf(rhs);
    bool lhs_zero = hb_double_bits_is_zero(lhs), rhs_zero = hb_double_bits_is_zero(rhs);
    if (op == HB_IR_FADD) return lhs_inf && rhs_inf && ((lhs ^ rhs) & 0x8000000000000000ULL);
    if (op == HB_IR_FSUB) return lhs_inf && rhs_inf && !((lhs ^ rhs) & 0x8000000000000000ULL);
    if (op == HB_IR_FMUL) return (lhs_inf && rhs_zero) || (rhs_inf && lhs_zero);
    if (op == HB_IR_FDIV) return (lhs_inf && rhs_inf) || (lhs_zero && rhs_zero);
    return false;
}

static uint32_t hb_sse_arith_float_bits(uint32_t lhs, uint32_t rhs, hb_ir_op_t op) {
    bool lhs_nan = hb_float_bits_is_nan(lhs), rhs_nan = hb_float_bits_is_nan(rhs);
    if (lhs_nan || rhs_nan) {
        uint32_t chosen = lhs_nan && (!rhs_nan || ((lhs & 0x007fffffu) >= (rhs & 0x007fffffu))) ? lhs : rhs;
        return hb_quiet_float_nan_bits(chosen);
    }
    if (hb_sse_arith_invalid_float(lhs, rhs, op)) return 0xffc00000u;
    float aval = hb_bits_to_float(lhs);
    float bval = hb_bits_to_float(rhs);
    float cval = op == HB_IR_FDIV ? (aval / bval) :
                 (op == HB_IR_FMUL ? (aval * bval) :
                  (op == HB_IR_FSUB ? (aval - bval) : (aval + bval)));
    return hb_float_to_bits(cval);
}

static uint32_t hb_sse_add_float_bits_er(uint32_t lhs, uint32_t rhs, unsigned er_mode) {
    if (er_mode <= 1) return hb_sse_arith_float_bits(lhs, rhs, HB_IR_FADD);
    if (hb_float_bits_is_nan(lhs) || hb_float_bits_is_nan(rhs) ||
        hb_sse_arith_invalid_float(lhs, rhs, HB_IR_FADD))
        return hb_sse_arith_float_bits(lhs, rhs, HB_IR_FADD);

    float aval = hb_bits_to_float(lhs);
    float bval = hb_bits_to_float(rhs);
    if (isinf(aval) || isinf(bval)) return hb_sse_arith_float_bits(lhs, rhs, HB_IR_FADD);

    long double exact = (long double)aval + (long double)bval;
    float candidate = (float)exact;
    long double rounded = (long double)candidate;

    if (er_mode == 2) {
        if (rounded > exact) candidate = nextafterf(candidate, -INFINITY);
    } else if (er_mode == 3) {
        if (rounded < exact) candidate = nextafterf(candidate, INFINITY);
    } else if (er_mode == 4) {
        if (exact > 0.0L && rounded > exact) candidate = nextafterf(candidate, -INFINITY);
        else if (exact < 0.0L && rounded < exact) candidate = nextafterf(candidate, INFINITY);
    }
    return hb_float_to_bits(candidate);
}

static uint64_t hb_sse_arith_double_bits(uint64_t lhs, uint64_t rhs, hb_ir_op_t op) {
    bool lhs_nan = hb_double_bits_is_nan(lhs), rhs_nan = hb_double_bits_is_nan(rhs);
    if (lhs_nan || rhs_nan) {
        uint64_t chosen = lhs_nan && (!rhs_nan || ((lhs & 0x000fffffffffffffULL) >= (rhs & 0x000fffffffffffffULL))) ? lhs : rhs;
        return hb_quiet_double_nan_bits(chosen);
    }
    if (hb_sse_arith_invalid_double(lhs, rhs, op)) return 0xfff8000000000000ULL;
    double aval = hb_bits_to_double(lhs);
    double bval = hb_bits_to_double(rhs);
    double cval = op == HB_IR_FDIV ? (aval / bval) :
                  (op == HB_IR_FMUL ? (aval * bval) :
                   (op == HB_IR_FSUB ? (aval - bval) : (aval + bval)));
    return hb_double_to_bits(cval);
}

static int16_t sat_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static int8_t sat_i8(int32_t v) {
    if (v > 127) return 127;
    if (v < -128) return -128;
    return (int8_t)v;
}

static uint16_t sat_u16_from_i32(int32_t v) {
    if (v < 0) return 0;
    if (v > 65535) return 65535;
    return (uint16_t)v;
}

static int64_t load_lane_signed(const uint8_t* p, unsigned lane) {
    if (lane == 1) return (int8_t)p[0];
    if (lane == 2) { int16_t v; memcpy(&v, p, sizeof(v)); return v; }
    if (lane == 4) { int32_t v; memcpy(&v, p, sizeof(v)); return v; }
    int64_t v; memcpy(&v, p, sizeof(v)); return v;
}

static uint64_t load_lane_unsigned(const uint8_t* p, unsigned lane) {
    if (lane == 1) return p[0];
    if (lane == 2) { uint16_t v; memcpy(&v, p, sizeof(v)); return v; }
    if (lane == 4) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
    uint64_t v; memcpy(&v, p, sizeof(v)); return v;
}

static void store_lane(uint8_t* p, unsigned lane, uint64_t v) {
    if (lane == 1) p[0] = (uint8_t)v;
    else if (lane == 2) { uint16_t w = (uint16_t)v; memcpy(p, &w, sizeof(w)); }
    else if (lane == 4) { uint32_t d = (uint32_t)v; memcpy(p, &d, sizeof(d)); }
    else { uint64_t q = v; memcpy(p, &q, sizeof(q)); }
}

/* SHA-NI helpers (Intel SDM Vol 2). These are pure functions over uint32_t
 * state, kept as file-static so the per-op dispatch can call them without
 * lambda overhead. Operands are little-endian dwords. */
static inline uint32_t sha_rol(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32 - n));
}
static inline uint32_t sha_ror(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static inline uint32_t sha_ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ ((~x) & z);
}
static inline uint32_t sha_maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
static inline uint32_t sha_parity(uint32_t x, uint32_t y, uint32_t z) {
    return x ^ y ^ z;
}
/* Big-sigma0 (SHA-256): ROR(x,2) ^ ROR(x,13) ^ ROR(x,22). */
static inline uint32_t sha_bigsig0(uint32_t x) {
    return sha_ror(x, 2) ^ sha_ror(x, 13) ^ sha_ror(x, 22);
}
/* Big-sigma1 (SHA-256): ROR(x,6) ^ ROR(x,11) ^ ROR(x,25). */
static inline uint32_t sha_bigsig1(uint32_t x) {
    return sha_ror(x, 6) ^ sha_ror(x, 11) ^ sha_ror(x, 25);
}
/* Small-sigma0 (SHA-256): ROR(x,7) ^ ROR(x,18) ^ SHR(x,3). */
static inline uint32_t sha_smallsig0(uint32_t x) {
    return sha_ror(x, 7) ^ sha_ror(x, 18) ^ (x >> 3);
}
/* Small-sigma1 (SHA-256): ROR(x,17) ^ ROR(x,19) ^ SHR(x,10). */
static inline uint32_t sha_smallsig1(uint32_t x) {
    return sha_ror(x, 17) ^ sha_ror(x, 19) ^ (x >> 10);
}

static const uint8_t aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static uint32_t aes_subword(uint32_t w) {
    uint32_t out = 0;
    for (unsigned i = 0; i < 4; i++) out |= (uint32_t)aes_sbox[(w >> (i * 8)) & 0xffu] << (i * 8);
    return out;
}

static uint32_t aes_rotword(uint32_t w) {
    return (w >> 8) | (w << 24);
}

static uint8_t aes_xtime(uint8_t x) {
    return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0u));
}

static uint8_t aes_gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1u) r ^= a;
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

static uint8_t gf2p8_inv(uint8_t value) {
    if (!value) return 0;
    uint8_t result = 1;
    uint8_t base = value;
    unsigned exp = 254;
    while (exp) {
        if (exp & 1u) result = aes_gmul(result, base);
        base = aes_gmul(base, base);
        exp >>= 1;
    }
    return result;
}

static uint8_t gf2p8_parity8(uint8_t value) {
    value ^= (uint8_t)(value >> 4);
    value ^= (uint8_t)(value >> 2);
    value ^= (uint8_t)(value >> 1);
    return value & 1u;
}

static uint8_t gf2p8_affine_byte(uint8_t value, const uint8_t matrix[8], uint8_t imm) {
    uint8_t out = 0;
    for (unsigned bit = 0; bit < 8; bit++) {
        uint8_t b = gf2p8_parity8((uint8_t)(value & matrix[bit]));
        b ^= (uint8_t)((imm >> bit) & 1u);
        out |= (uint8_t)(b << bit);
    }
    return out;
}

static uint8_t aes_inv_sbox_byte(uint8_t v) {
    for (unsigned i = 0; i < 256; i++) {
        if (aes_sbox[i] == v) return (uint8_t)i;
    }
    return 0;
}

static void aes_sub_bytes(uint8_t s[16], bool inverse) {
    for (unsigned i = 0; i < 16; i++) {
        s[i] = inverse ? aes_inv_sbox_byte(s[i]) : aes_sbox[s[i]];
    }
}

static void aes_shift_rows(uint8_t s[16], bool inverse) {
    uint8_t t[16];
    for (unsigned row = 0; row < 4; row++) {
        for (unsigned col = 0; col < 4; col++) {
            unsigned src_col = inverse ? ((col + 4 - row) & 3u) : ((col + row) & 3u);
            t[col * 4 + row] = s[src_col * 4 + row];
        }
    }
    memcpy(s, t, sizeof(t));
}

static void aes_mix_columns(uint8_t s[16], bool inverse) {
    for (unsigned col = 0; col < 4; col++) {
        uint8_t *a = s + col * 4;
        uint8_t r0, r1, r2, r3;
        if (inverse) {
            r0 = (uint8_t)(aes_gmul(a[0], 14) ^ aes_gmul(a[1], 11) ^ aes_gmul(a[2], 13) ^ aes_gmul(a[3], 9));
            r1 = (uint8_t)(aes_gmul(a[0], 9) ^ aes_gmul(a[1], 14) ^ aes_gmul(a[2], 11) ^ aes_gmul(a[3], 13));
            r2 = (uint8_t)(aes_gmul(a[0], 13) ^ aes_gmul(a[1], 9) ^ aes_gmul(a[2], 14) ^ aes_gmul(a[3], 11));
            r3 = (uint8_t)(aes_gmul(a[0], 11) ^ aes_gmul(a[1], 13) ^ aes_gmul(a[2], 9) ^ aes_gmul(a[3], 14));
        } else {
            r0 = (uint8_t)(aes_gmul(a[0], 2) ^ aes_gmul(a[1], 3) ^ a[2] ^ a[3]);
            r1 = (uint8_t)(a[0] ^ aes_gmul(a[1], 2) ^ aes_gmul(a[2], 3) ^ a[3]);
            r2 = (uint8_t)(a[0] ^ a[1] ^ aes_gmul(a[2], 2) ^ aes_gmul(a[3], 3));
            r3 = (uint8_t)(aes_gmul(a[0], 3) ^ a[1] ^ a[2] ^ aes_gmul(a[3], 2));
        }
        a[0] = r0;
        a[1] = r1;
        a[2] = r2;
        a[3] = r3;
    }
}

static void aes_xor_key(uint8_t s[16], const uint8_t key[16]) {
    for (unsigned i = 0; i < 16; i++) s[i] ^= key[i];
}

static void aes_round128(hb_ir_vec_op_t vop, const uint8_t state[16],
                         const uint8_t key[16], uint8_t out[16]) {
    memcpy(out, state, 16);
    if (vop == HB_VEC_AESIMC) {
        aes_mix_columns(out, true);
        return;
    }
    if (vop == HB_VEC_AESENC || vop == HB_VEC_AESENCLAST) {
        aes_sub_bytes(out, false);
        aes_shift_rows(out, false);
        if (vop == HB_VEC_AESENC) aes_mix_columns(out, false);
        aes_xor_key(out, key);
        return;
    }
    if (vop == HB_VEC_AESDEC) {
        aes_shift_rows(out, true);
        aes_sub_bytes(out, true);
        aes_mix_columns(out, true);
        aes_xor_key(out, key);
        return;
    }
    if (vop == HB_VEC_AESDECLAST) {
        aes_shift_rows(out, true);
        aes_sub_bytes(out, true);
        aes_xor_key(out, key);
    }
}

static void pclmul64(uint64_t a, uint64_t b, uint8_t out[16]) {
    __uint128_t acc = 0;
    for (unsigned bit = 0; bit < 64; bit++) {
        if ((b >> bit) & 1u) acc ^= ((__uint128_t)a) << bit;
    }
    uint64_t lo = (uint64_t)acc;
    uint64_t hi = (uint64_t)(acc >> 64);
    memcpy(out, &lo, sizeof(lo));
    memcpy(out + 8, &hi, sizeof(hi));
}

static int pcmp_explicit_len(uint32_t raw, unsigned max_units) {
    int32_t signed_len = (int32_t)raw;
    int64_t len = signed_len < 0 ? -(int64_t)signed_len : (int64_t)signed_len;
    if (len > (int64_t)max_units) len = max_units;
    return (int)len;
}

static uint64_t pcmp_unit_unsigned(const uint8_t* bytes, unsigned unit, unsigned idx) {
    if (unit == 1) return bytes[idx];
    uint16_t v;
    memcpy(&v, bytes + idx * 2, sizeof(v));
    return v;
}

static int64_t pcmp_unit_signed(const uint8_t* bytes, unsigned unit, unsigned idx) {
    if (unit == 1) return (int8_t)bytes[idx];
    int16_t v;
    memcpy(&v, bytes + idx * 2, sizeof(v));
    return v;
}

static int pcmp_implicit_len(const uint8_t* bytes, unsigned unit, unsigned max_units) {
    for (unsigned i = 0; i < max_units; i++) {
        if (pcmp_unit_unsigned(bytes, unit, i) == 0) return (int)i;
    }
    return (int)max_units;
}

static uint32_t pcmpxstr_result(hb_context_t* ctx, hb_ir_vec_op_t vop,
                                const uint8_t lhs[16], const uint8_t rhs[16],
                                unsigned imm, unsigned* max_units_out,
                                int* len1_out, int* len2_out) {
    unsigned unit = (imm & 1u) ? 2 : 1;
    bool is_signed = (imm & 2u) != 0;
    unsigned max_units = 16 / unit;
    bool explicit_len = vop == HB_VEC_PCMPESTRM || vop == HB_VEC_PCMPESTRI;
    int len1 = explicit_len ? pcmp_explicit_len((uint32_t)ctx->regs.x64.rax, max_units)
                            : pcmp_implicit_len(lhs, unit, max_units);
    int len2 = explicit_len ? pcmp_explicit_len((uint32_t)ctx->regs.x64.rdx, max_units)
                            : pcmp_implicit_len(rhs, unit, max_units);
    uint32_t res1 = 0;
    unsigned aggregation = (imm >> 2) & 3u;
    for (unsigned j = 0; j < max_units; j++) {
        bool bit = false;
        if (aggregation == 0) {
            if ((int)j < len2) {
                for (int i = 0; i < len1; i++) {
                    if (pcmp_unit_unsigned(lhs, unit, (unsigned)i) ==
                        pcmp_unit_unsigned(rhs, unit, j)) { bit = true; break; }
                }
            }
        } else if (aggregation == 1) {
            if ((int)j < len2) {
                for (int i = 0; i + 1 < len1; i += 2) {
                    if (is_signed) {
                        int64_t lo = pcmp_unit_signed(lhs, unit, (unsigned)i);
                        int64_t hi = pcmp_unit_signed(lhs, unit, (unsigned)(i + 1));
                        int64_t v = pcmp_unit_signed(rhs, unit, j);
                        if (lo <= v && v <= hi) { bit = true; break; }
                    } else {
                        uint64_t lo = pcmp_unit_unsigned(lhs, unit, (unsigned)i);
                        uint64_t hi = pcmp_unit_unsigned(lhs, unit, (unsigned)(i + 1));
                        uint64_t v = pcmp_unit_unsigned(rhs, unit, j);
                        if (lo <= v && v <= hi) { bit = true; break; }
                    }
                }
            }
        } else if (aggregation == 2) {
            if ((int)j < len1 && (int)j < len2 &&
                pcmp_unit_unsigned(lhs, unit, j) == pcmp_unit_unsigned(rhs, unit, j)) bit = true;
        } else {
            if (len1 == 0) {
                bit = (int)j <= len2;
            } else if ((int)j + len1 <= len2) {
                bit = true;
                for (int i = 0; i < len1; i++) {
                    if (pcmp_unit_unsigned(lhs, unit, (unsigned)i) !=
                        pcmp_unit_unsigned(rhs, unit, (unsigned)(j + i))) { bit = false; break; }
                }
            }
        }
        if (bit) res1 |= 1u << j;
    }
    uint32_t full = max_units == 16 ? 0xffffu : 0xffu;
    uint32_t valid2 = len2 >= (int)max_units ? full : ((1u << len2) - 1u);
    uint32_t polarity = (imm >> 4) & 3u;
    uint32_t res2 = res1;
    if (polarity == 1) res2 = (~res1) & full;
    else if (polarity == 2) res2 = res1 & valid2;
    else if (polarity == 3) res2 = (~res1) & valid2;
    *max_units_out = max_units;
    *len1_out = len1;
    *len2_out = len2;
    return res2 & full;
}

static hb_result_t read_xmm_operand(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t out[2]) {
    if (!ctx || !op || !out) return HB_ERR_INVALID_ARG;
    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) return read_xmm_reg(ctx, op->reg, out);
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        return hb_memory_read(ctx->memory, addr, out, sizeof(uint64_t) * 2);
    }
    return HB_ERR_INTERNAL;
}

static hb_result_t read_xmm_operand_bytes(hb_context_t* ctx, const hb_ir_operand_t* op,
                                          uint8_t* out, size_t bytes) {
    if (!ctx || !op || !out || bytes > 64) return HB_ERR_INVALID_ARG;
    memset(out, 0, bytes);
    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) {
        return read_vec_reg_bytes(ctx, op->reg, out, bytes);
    }
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        return hb_memory_read(ctx->memory, addr, out, bytes);
    }
    return HB_ERR_INTERNAL;
}

static hb_result_t read_packed_shift_count(hb_context_t* ctx, const hb_ir_operand_t* op,
                                           unsigned* out_count) {
    if (!ctx || !op || !out_count) return HB_ERR_INVALID_ARG;
    if (op->type == HB_OP_IMM) {
        *out_count = (unsigned)(op->imm & 0xff);
        return HB_OK;
    }
    uint8_t bytes[16];
    hb_result_t r = read_xmm_operand_bytes(ctx, op, bytes, sizeof(bytes));
    if (r != HB_OK) return r;
    uint64_t raw = 0;
    memcpy(&raw, bytes, sizeof(raw));
    *out_count = (unsigned)(raw & 0xff);
    return HB_OK;
}

static void write_reg(hb_context_t* ctx, int idx, uint64_t val) {
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t* p = reg_ptr_x86(&ctx->regs.x86, idx);
        if (p) *p = (uint32_t)val;
    } else {
        uint64_t* p = reg_ptr_x64(&ctx->regs.x64, idx);
        if (p) *p = val;
    }
}

static uint64_t mask_for_size(hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return 0xffULL;
        case HB_SIZE_16: return 0xffffULL;
        case HB_SIZE_32: return 0xffffffffULL;
        default: return 0xffffffffffffffffULL;
    }
}

static uint64_t trunc_to_size(uint64_t val, hb_size_t size) {
    return val & mask_for_size(size);
}

static uint64_t read_reg_sized(hb_context_t* ctx, int idx, hb_size_t size, uint8_t reg_offset) {
    return (read_reg(ctx, idx) >> ((unsigned)reg_offset * 8u)) & mask_for_size(size);
}

static uint64_t sign_extend_from_size(uint64_t val, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  return (uint64_t)(int64_t)(int8_t)val;
        case HB_SIZE_16: return (uint64_t)(int64_t)(int16_t)val;
        case HB_SIZE_32: return (uint64_t)(int64_t)(int32_t)val;
        default: return val;
    }
}

static unsigned bit_width_for_size(hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return 8;
        case HB_SIZE_16: return 16;
        case HB_SIZE_32: return 32;
        default: return 64;
    }
}

static unsigned popcount_u64(uint64_t value) {
    unsigned count = 0;
    while (value) {
        value &= value - 1;
        count++;
    }
    return count;
}

static bool parity_even_u8(uint8_t v) {
    return (popcount_u64(v) & 1u) == 0u;
}

static uint32_t hb_size_bytes(hb_size_t sz) {
    switch (sz) {
        case HB_SIZE_8:   return 1u;
        case HB_SIZE_16:  return 2u;
        case HB_SIZE_32:  return 4u;
        case HB_SIZE_64:  return 8u;
        case HB_SIZE_128: return 16u;
        case HB_SIZE_256: return 32u;
        case HB_SIZE_512: return 64u;
        default: return 0u;
    }
}

static void write_reg_sized(hb_context_t* ctx, int idx, uint64_t val, hb_size_t size) {
    if (ctx->mode == HB_MODE_32BIT) {
        if (size == HB_SIZE_8 || size == HB_SIZE_16) {
            uint64_t old = read_reg(ctx, idx);
            uint64_t mask = mask_for_size(size);
            write_reg(ctx, idx, (old & ~mask) | (val & mask));
        } else {
            write_reg(ctx, idx, val);
        }
        return;
    }

    if (size == HB_SIZE_32) {
        write_reg(ctx, idx, (uint32_t)val); /* x86-64 32-bit register writes zero-extend. */
    } else if (size == HB_SIZE_8 || size == HB_SIZE_16) {
        uint64_t old = read_reg(ctx, idx);
        uint64_t mask = mask_for_size(size);
        write_reg(ctx, idx, (old & ~mask) | (val & mask));
    } else {
        write_reg(ctx, idx, val);
    }
}

static void write_reg_sized_offset(hb_context_t* ctx, int idx, uint64_t val,
                                   hb_size_t size, uint8_t reg_offset) {
    if (reg_offset == 0) {
        write_reg_sized(ctx, idx, val, size);
        return;
    }
    uint64_t old = read_reg(ctx, idx);
    unsigned shift = (unsigned)reg_offset * 8u;
    uint64_t mask = mask_for_size(size) << shift;
    write_reg(ctx, idx, (old & ~mask) | ((val << shift) & mask));
}

/* Memory address resolution */
static uint64_t resolve_addr(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t base = 0;
    if (op->mem.base < HB_REG_COUNT) {
        if (op->mem.base == HB_REG_RIP) base = ctx->pc;
        else base = read_reg(ctx, op->mem.base);
    }
    uint64_t index = 0;
    if (op->mem.index < HB_REG_COUNT) {
        index = read_reg(ctx, op->mem.index);
    }
    if (op->mem.segment == 0x64) base += ctx->fs_base;
    else if (op->mem.segment == 0x65) base += ctx->gs_base;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t base32 = (uint32_t)base;
        uint32_t index32 = (uint32_t)index;
        return (uint32_t)(base32 + index32 * op->mem.scale + (uint32_t)op->mem.disp);
    }
    if (op->mem.addr32) {
        uint32_t base32 = (uint32_t)base;
        uint32_t index32 = (uint32_t)index;
        return (uint32_t)(base32 + index32 * op->mem.scale + (uint32_t)op->mem.disp);
    }
    return base + index * op->mem.scale + (uint64_t)op->mem.disp;
}

/* Find block by guest address */
static hb_ir_block_t* find_block(hb_ir_cfg_t* cfg, uint64_t addr) {
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

/* Memory read/write by size */
static hb_result_t mem_read(hb_context_t* ctx, uint64_t addr, uint64_t* out, hb_size_t sz) {
    switch (sz) {
        case HB_SIZE_8: {
            uint8_t v;
            hb_result_t r = hb_memory_read_u8(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            if (trace_mem_watch_enabled()) trace_mem_watch_bytes(ctx, "read", addr, &v, sizeof(v), NULL);
            return HB_OK;
        }
        case HB_SIZE_16: {
            uint16_t v;
            hb_result_t r = hb_memory_read_u16(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            if (trace_mem_watch_enabled()) trace_mem_watch_bytes(ctx, "read", addr, &v, sizeof(v), NULL);
            return HB_OK;
        }
        case HB_SIZE_32: {
            uint32_t v;
            hb_result_t r = hb_memory_read_u32(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            if (trace_mem_watch_enabled()) trace_mem_watch_bytes(ctx, "read", addr, &v, sizeof(v), NULL);
            return HB_OK;
        }
        case HB_SIZE_64: {
            uint64_t v;
            hb_result_t r = hb_memory_read_u64(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            if (trace_mem_watch_enabled()) trace_mem_watch_bytes(ctx, "read", addr, &v, sizeof(v), NULL);
            return HB_OK;
        }
        default:
            return HB_ERR_MEMORY_FAULT;
    }
}

static const hb_region_t* nearest_region(hb_memory_t* mem, uint64_t addr, int below) {
    const hb_region_t* best = NULL;
    if (!mem) return NULL;
    for (const hb_region_t* r = mem->regions; r; r = r->next) {
        uint64_t start = r->base;
        uint64_t end = r->base + r->size;
        if (below) {
            if (end <= addr && (!best || end > best->base + best->size)) best = r;
        } else {
            if (start > addr && (!best || start < best->base)) best = r;
        }
    }
    return best;
}

static void trace_stack_write_fault(hb_context_t* ctx, uint64_t addr, uint64_t val, hb_size_t sz, hb_result_t result) {
    static int budget = 0;
    if (!(trace_runtime_flags & TRACE_FLAG_STACK_FAULTS)) return;
    if (++budget > 20) return;

    hb_memory_t* mem = ctx ? ctx->memory : NULL;
    uint64_t rsp = ctx ? ctx->regs.x64.rsp : 0;
    const hb_region_t* hit = mem ? hb_memory_find_region(mem, addr) : NULL;
    const hb_region_t* below = nearest_region(mem, addr, 1);
    const hb_region_t* above = nearest_region(mem, addr, 0);

    fprintf(stderr,
            "macrunner-hb-stack-fault: result=%d addr=0x%llx size=%u value=0x%llx "
            "rip=0x%llx rsp=0x%llx stack=[0x%llx..0x%llx]\n",
            result, (unsigned long long)addr, (unsigned)sz, (unsigned long long)val,
            (unsigned long long)(ctx ? ctx->regs.x64.rip : 0),
            (unsigned long long)rsp,
            (unsigned long long)(mem ? mem->stack_bottom : 0),
            (unsigned long long)(mem ? mem->stack_top : 0));
    if (hit) {
        fprintf(stderr,
                "macrunner-hb-stack-fault: containing region base=0x%llx size=0x%zx perm=0x%x "
                "stack=%d heap=%d guard=%d allocated=%d\n",
                (unsigned long long)hit->base, hit->size, hit->perm,
                hit->is_stack, hit->is_heap, hit->is_guard, hit->allocated);
    } else {
        fprintf(stderr, "macrunner-hb-stack-fault: containing region none\n");
    }
    if (below) {
        fprintf(stderr,
                "macrunner-hb-stack-fault: below region base=0x%llx end=0x%llx size=0x%zx perm=0x%x "
                "stack=%d heap=%d guard=%d allocated=%d\n",
                (unsigned long long)below->base, (unsigned long long)(below->base + below->size),
                below->size, below->perm, below->is_stack, below->is_heap, below->is_guard, below->allocated);
    }
    if (above) {
        fprintf(stderr,
                "macrunner-hb-stack-fault: above region base=0x%llx end=0x%llx size=0x%zx perm=0x%x "
                "stack=%d heap=%d guard=%d allocated=%d\n",
                (unsigned long long)above->base, (unsigned long long)(above->base + above->size),
                above->size, above->perm, above->is_stack, above->is_heap, above->is_guard, above->allocated);
    }
}

static hb_result_t mem_write(hb_context_t* ctx, uint64_t addr, uint64_t val, hb_size_t sz) {
    hb_result_t r;
    uint8_t before[8] = {0};
    size_t bytes = (size_t)sz;
    if (bytes > sizeof(before)) bytes = sizeof(before);
    if (trace_mem_watch_enabled() && bytes)
        (void)hb_memory_read(ctx->memory, addr, before, bytes);
    trace_guest_native_write(ctx, "interp_mem_write", addr, val, sz);
    switch (sz) {
        case HB_SIZE_8:  r = hb_memory_write_u8(ctx->memory, addr, (uint8_t)val); break;
        case HB_SIZE_16: r = hb_memory_write_u16(ctx->memory, addr, (uint16_t)val); break;
        case HB_SIZE_32: r = hb_memory_write_u32(ctx->memory, addr, (uint32_t)val); break;
        case HB_SIZE_64: r = hb_memory_write_u64(ctx->memory, addr, val); break;
        default: r = HB_ERR_MEMORY_FAULT; break;
    }
    if (r != HB_OK) trace_stack_write_fault(ctx, addr, val, sz, r);
    else if (trace_mem_watch_enabled()) trace_mem_watch_bytes(ctx, "write", addr, &val, (size_t)sz, before);
    return r;
}

static double ext80_to_double(const uint8_t bytes[10]) {
    uint64_t sig;
    uint16_t se;
    bool neg;
    unsigned exp;

    memcpy(&sig, bytes, sizeof(sig));
    memcpy(&se, bytes + 8, sizeof(se));
    neg = (se & 0x8000u) != 0;
    exp = se & 0x7fffu;
    if (exp == 0 && sig == 0) {
        return neg ? -0.0 : 0.0;
    }
    if (exp == 0x7fffu) {
        return (sig == 0x8000000000000000ULL) ? (neg ? -INFINITY : INFINITY) : NAN;
    }
    {
        double frac = (double)sig / 9223372036854775808.0;
        int unbiased = (int)(exp ? exp : 1) - 16383;
        double out = ldexp(frac, unbiased);
        return neg ? -out : out;
    }
}

static hb_result_t x87_read_real_mem(hb_context_t* ctx, const hb_ir_operand_t* op, double* out) {
    uint64_t addr = resolve_addr(ctx, op);

    if (!out) return HB_ERR_INVALID_ARG;
    if (op->size == HB_SIZE_32) {
        float f;
        hb_result_t r = hb_memory_read(ctx->memory, addr, &f, sizeof(f));
        if (r != HB_OK) return r;
        *out = (double)f;
    } else if (op->size == HB_SIZE_64) {
        hb_result_t r = hb_memory_read(ctx->memory, addr, out, sizeof(*out));
        if (r != HB_OK) return r;
    } else if (op->size == HB_SIZE_80) {
        uint8_t bytes[10];
        hb_result_t r = hb_memory_read(ctx->memory, addr, bytes, sizeof(bytes));
        if (r != HB_OK) return r;
        *out = ext80_to_double(bytes);
    } else {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    return HB_OK;
}

static hb_result_t x87_fld_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    double value;
    hb_result_t r = x87_read_real_mem(ctx, op, &value);
    if (r != HB_OK) return r;
    return hb_x87_push_f64(hb_context_x87(ctx), value);
}

/* Convert an IEEE 754 double to an 80-bit extended precision encoding
 * (sign+exponent 16-bit, significand 64-bit with explicit integer bit).
 * Used by FSTP m80. The 80-bit format: bits 79 = sign, 78-64 = biased
 * exponent (15 bits), 63 = explicit integer bit (1 for normal), 62-0
 * = fraction (no hidden bit). For our double (53-bit mantissa), we have
 * to split it into explicit-int form. For subnormals/zero/inf/nan we
 * follow IEEE 754 conventions. */
static void double_to_ext80(double value, uint8_t out[10]) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bool neg = (bits >> 63) & 1;
    uint64_t frac = bits & 0x000FFFFFFFFFFFFFULL;
    unsigned exp = (unsigned)((bits >> 52) & 0x7FFu);

    uint16_t se = neg ? 0x8000u : 0u;
    uint64_t sig = 0;

    if (exp == 0 && frac == 0) {
        /* Zero */
        se |= 0x0000u;
        sig = 0;
    } else if (exp == 0x7FFu) {
        /* Inf / NaN */
        se |= 0x7FFFu;
        if (frac == 0) {
            sig = 0x8000000000000000ULL;  /* Inf: integer bit set, frac=0 */
        } else {
            sig = 0x8000000000000000ULL | frac;  /* NaN: keep payload */
        }
    } else {
        /* Normal finite: convert hidden-bit double to explicit-bit 80 */
        se |= (uint16_t)(exp - 0x3FFu + 0x3FFFu);
        sig = 0x8000000000000000ULL | frac;
    }
    memcpy(out, &sig, sizeof(sig));
    memcpy(out + 8, &se, sizeof(se));
}

static hb_result_t x87_st0_for_store(hb_context_t* ctx, double* value,
                                      bool* masked_underflow) {
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    hb_result_t r;

    if (!value || !masked_underflow) return HB_ERR_INVALID_ARG;
    *masked_underflow = false;
    r = hb_x87_st_f64(x87, 0, value);
    if (r == HB_OK) return HB_OK;
    if (r != HB_ERR_EXEC_FAULT) return r;
    r = hb_x87_stack_underflow(x87, value);
    if (r == HB_OK) *masked_underflow = true;
    return r;
}

static void x87_indefinite_ext80(uint8_t out[10]) {
    const uint64_t sig = UINT64_C(0xc000000000000000);
    const uint16_t se = UINT16_C(0xffff);
    memcpy(out, &sig, sizeof(sig));
    memcpy(out + 8, &se, sizeof(se));
}

static hb_result_t x87_fstp_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t addr = resolve_addr(ctx, op);
    double value;
    bool masked_underflow;
    hb_result_t r = x87_st0_for_store(ctx, &value, &masked_underflow);
    if (r != HB_OK) return r;

    if (op->size == HB_SIZE_32) {
        float f = (float)value;
        r = hb_memory_write(ctx->memory, addr, &f, sizeof(f));
    } else if (op->size == HB_SIZE_64) {
        r = hb_memory_write(ctx->memory, addr, &value, sizeof(value));
    } else if (op->size == HB_SIZE_80) {
        uint8_t bytes[10];
        if (masked_underflow) x87_indefinite_ext80(bytes);
        else double_to_ext80(value, bytes);
        r = hb_memory_write(ctx->memory, addr, bytes, sizeof(bytes));
    } else {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    if (r != HB_OK) return r;
    return hb_x87_fstp_pop(hb_context_x87(ctx));
}

static hb_result_t x87_fst_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t addr = resolve_addr(ctx, op);
    double value;
    bool masked_underflow;
    hb_result_t r = x87_st0_for_store(ctx, &value, &masked_underflow);
    if (r != HB_OK) return r;

    if (op->size == HB_SIZE_32) {
        float f = (float)value;
        return hb_memory_write(ctx->memory, addr, &f, sizeof(f));
    }
    if (op->size == HB_SIZE_64) {
        return hb_memory_write(ctx->memory, addr, &value, sizeof(value));
    }
    if (op->size == HB_SIZE_80) {
        uint8_t bytes[10];
        if (masked_underflow) x87_indefinite_ext80(bytes);
        else double_to_ext80(value, bytes);
        return hb_memory_write(ctx->memory, addr, bytes, sizeof(bytes));
    }
    return HB_ERR_UNSUPPORTED_FEATURE;
}

static hb_result_t x87_fild_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t addr = resolve_addr(ctx, op);
    double value;

    if (op->size == HB_SIZE_16) {
        int16_t v;
        hb_result_t r = hb_memory_read(ctx->memory, addr, &v, sizeof(v));
        if (r != HB_OK) return r;
        value = (double)v;
    } else if (op->size == HB_SIZE_32) {
        int32_t v;
        hb_result_t r = hb_memory_read(ctx->memory, addr, &v, sizeof(v));
        if (r != HB_OK) return r;
        value = (double)v;
    } else if (op->size == HB_SIZE_64) {
        int64_t v;
        hb_result_t r = hb_memory_read(ctx->memory, addr, &v, sizeof(v));
        if (r != HB_OK) return r;
        value = (double)v;
    } else {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    return hb_x87_push_f64(hb_context_x87(ctx), value);
}

static hb_result_t x87_fistp_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t addr = resolve_addr(ctx, op);

    if (op->size == HB_SIZE_16) {
        int16_t v;
        hb_result_t r = hb_x87_fistp_i16(hb_context_x87(ctx), &v);
        if (r != HB_OK) return r;
        return hb_memory_write(ctx->memory, addr, &v, sizeof(v));
    }
    if (op->size == HB_SIZE_32) {
        int32_t v;
        hb_result_t r = hb_x87_fistp_i32(hb_context_x87(ctx), &v);
        if (r != HB_OK) return r;
        return hb_memory_write(ctx->memory, addr, &v, sizeof(v));
    }
    if (op->size == HB_SIZE_64) {
        int64_t v;
        hb_result_t r = hb_x87_fistp_i64(hb_context_x87(ctx), &v);
        if (r != HB_OK) return r;
        return hb_memory_write(ctx->memory, addr, &v, sizeof(v));
    }
    return HB_ERR_UNSUPPORTED_FEATURE;
}

/* FIST = non-popping integer store. Same encoding as FISTP but no pop. */
static hb_result_t x87_fist_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t addr = resolve_addr(ctx, op);

    if (op->size == HB_SIZE_16) {
        int16_t v;
        hb_result_t r = hb_x87_fist_i16(hb_context_x87(ctx), &v);
        if (r != HB_OK) return r;
        return hb_memory_write(ctx->memory, addr, &v, sizeof(v));
    }
    if (op->size == HB_SIZE_32) {
        int32_t v;
        hb_result_t r = hb_x87_fist_i32(hb_context_x87(ctx), &v);
        if (r != HB_OK) return r;
        return hb_memory_write(ctx->memory, addr, &v, sizeof(v));
    }
    return HB_ERR_UNSUPPORTED_FEATURE;
}

static hb_result_t x87_fldcw_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint16_t cw;
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r = hb_memory_read(ctx->memory, addr, &cw, sizeof(cw));
    if (r != HB_OK) return r;
    return hb_x87_fldcw(hb_context_x87(ctx), cw);
}

static hb_result_t x87_fnstcw_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint16_t cw;
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r = hb_x87_fnstcw(hb_context_x87(ctx), &cw);
    if (r != HB_OK) return r;
    return hb_memory_write(ctx->memory, addr, &cw, sizeof(cw));
}

static hb_result_t x87_fnstsw_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint16_t sw = hb_context_x87(ctx)->status_word;
    uint64_t addr = resolve_addr(ctx, op);
    return hb_memory_write(ctx->memory, addr, &sw, sizeof(sw));
}

static void x87_env_wr16(uint8_t* env, unsigned off, uint16_t v) {
    env[off + 0] = (uint8_t)(v & 0xffu);
    env[off + 1] = (uint8_t)(v >> 8);
}

static uint16_t x87_env_rd16(const uint8_t* env, unsigned off) {
    return (uint16_t)(env[off] | ((uint16_t)env[off + 1] << 8));
}

#define HB_X86_DEFAULT_MXCSR 0x1f80u
#define HB_X86_MXCSR_MASK    0xffbfu

static void x87_env_wr32(uint8_t* env, unsigned off, uint32_t v) {
    env[off + 0] = (uint8_t)(v & 0xffu);
    env[off + 1] = (uint8_t)((v >> 8) & 0xffu);
    env[off + 2] = (uint8_t)((v >> 16) & 0xffu);
    env[off + 3] = (uint8_t)(v >> 24);
}

static void x87_set_tag_entry(hb_x87_state_t* x87, unsigned phys, uint16_t tag) {
    unsigned shift = phys * 2u;
    x87->tag_word = (uint16_t)((x87->tag_word & ~(0x3u << shift)) | ((tag & 0x3u) << shift));
}

static uint16_t x87_tag_from_double(double value) {
    switch (fpclassify(value)) {
        case FP_ZERO: return 0x1u;
        case FP_NAN:
        case FP_INFINITE:
        case FP_SUBNORMAL: return 0x2u;
        case FP_NORMAL:
        default: return 0x0u;
    }
}

static uint8_t x87_fxsave_abridged_ftw(const hb_x87_state_t* x87) {
    uint8_t ftw = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned phys = (x87->top + i) & 7u;
        if (((x87->tag_word >> (phys * 2u)) & 3u) != 3u)
            ftw |= (uint8_t)(1u << i);
    }
    return ftw;
}

static void x87_store_env32(const hb_x87_state_t* x87, uint8_t env[28]) {
    uint16_t sw = (uint16_t)((x87->status_word & ~(7u << 11)) | ((x87->top & 7u) << 11));

    memset(env, 0, 28);
    x87_env_wr16(env, 0, x87->control_word);
    x87_env_wr16(env, 4, sw);
    x87_env_wr16(env, 8, x87->tag_word);
}

static void x87_load_env32(hb_x87_state_t* x87, const uint8_t env[28]) {
    x87->control_word = x87_env_rd16(env, 0);
    x87->status_word = x87_env_rd16(env, 4);
    x87->tag_word = x87_env_rd16(env, 8);
    x87->top = (x87->status_word >> 11) & 7u;
}

static hb_result_t x87_fnstenv_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t env[28];
    uint64_t addr = resolve_addr(ctx, op);

    x87_store_env32(hb_context_x87(ctx), env);
    return hb_memory_write(ctx->memory, addr, env, sizeof(env));
}

static hb_result_t x87_fldenv_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t env[28];
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r = hb_memory_read(ctx->memory, addr, env, sizeof(env));

    if (r != HB_OK) return r;
    x87_load_env32(hb_context_x87(ctx), env);
    return HB_OK;
}

static hb_result_t x87_fnsave_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t image[108];
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r;

    memset(image, 0, sizeof(image));
    x87_store_env32(x87, image);
    for (unsigned i = 0; i < 8; i++) {
        unsigned phys = (x87->top + i) & 7u;
        if (((x87->tag_word >> (phys * 2u)) & 3u) != 3u)
            double_to_ext80(x87->st[phys], image + 28 + i * 10);
    }
    r = hb_memory_write(ctx->memory, addr, image, sizeof(image));
    if (r != HB_OK) return r;
    return hb_x87_fninit(x87);
}

static hb_result_t x87_frstor_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t image[108];
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r = hb_memory_read(ctx->memory, addr, image, sizeof(image));

    if (r != HB_OK) return r;
    x87_load_env32(x87, image);
    for (unsigned i = 0; i < 8; i++) {
        unsigned phys = (x87->top + i) & 7u;
        x87->st[phys] = ext80_to_double(image + 28 + i * 10);
    }
    return HB_OK;
}

static hb_result_t x87_fxsave_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t image[512];
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    uint16_t sw = (uint16_t)((x87->status_word & ~(7u << 11)) | ((x87->top & 7u) << 11));
    uint64_t addr = resolve_addr(ctx, op);

    memset(image, 0, sizeof(image));
    x87_env_wr16(image, 0x00, x87->control_word);
    x87_env_wr16(image, 0x02, sw);
    image[0x04] = x87_fxsave_abridged_ftw(x87);
    x87_env_wr32(image, 0x18, HB_X86_DEFAULT_MXCSR);
    x87_env_wr32(image, 0x1c, HB_X86_MXCSR_MASK);
    for (unsigned i = 0; i < 8; i++) {
        unsigned phys = (x87->top + i) & 7u;
        if (((x87->tag_word >> (phys * 2u)) & 3u) != 3u)
            double_to_ext80(x87->st[phys], image + 0x20 + i * 16);
    }
    /* XMM save area (offset 0xA0). Patch H fix: mode-aware. 32-bit guests keep
     * XMM0-7 in the x86 register view; 64-bit guests keep XMM0-15 in the x64 view
     * (regs is a union, so the two live at different offsets). Reading
     * ctx->regs.x86.xmm unconditionally saved the wrong bytes -- and only 8 of the
     * 16 registers -- for x64 FXSAVE/XSAVE. Use the same accessor the SSE ops use. */
    unsigned xmm_count = (ctx->mode == HB_MODE_32BIT) ? 8u : 16u;
    for (unsigned i = 0; i < xmm_count; i++) {
        uint64_t v[2] = {0, 0};
        (void)read_xmm_reg(ctx, HB_REG_XMM0 + (int)i, v);
        memcpy(image + 0xa0 + i * 16, v, 16);
    }
    return hb_memory_write(ctx->memory, addr, image, sizeof(image));
}

static hb_result_t x87_fxrstor_mem(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint8_t image[512];
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    uint64_t addr = resolve_addr(ctx, op);
    hb_result_t r = hb_memory_read(ctx->memory, addr, image, sizeof(image));

    if (r != HB_OK) return r;
    x87->control_word = x87_env_rd16(image, 0x00);
    x87->status_word = x87_env_rd16(image, 0x02);
    x87->top = (x87->status_word >> 11) & 7u;
    x87->tag_word = 0xffffu;
    for (unsigned i = 0; i < 8; i++) {
        unsigned phys = (x87->top + i) & 7u;
        if (image[0x04] & (uint8_t)(1u << i)) {
            x87->st[phys] = ext80_to_double(image + 0x20 + i * 16);
            x87_set_tag_entry(x87, phys, x87_tag_from_double(x87->st[phys]));
        } else {
            x87->st[phys] = 0.0;
            x87_set_tag_entry(x87, phys, 0x3u);
        }
    }
    /* XMM restore area -- mode-aware (see the save-side comment). Restoring into
     * ctx->regs.x86.xmm unconditionally wrote the wrong union member for x64
     * guests, so XMM registers were silently not restored after FXRSTOR/XRSTOR. */
    unsigned xmm_count = (ctx->mode == HB_MODE_32BIT) ? 8u : 16u;
    for (unsigned i = 0; i < xmm_count; i++) {
        uint64_t v[2] = {0, 0};
        memcpy(v, image + 0xa0 + i * 16, 16);
        (void)write_xmm_reg(ctx, HB_REG_XMM0 + (int)i, v);
    }
    return HB_OK;
}

static hb_result_t x87_arith_mem(hb_context_t* ctx, const hb_ir_operand_t* op, hb_ir_op_t arith_op) {
    double lhs;
    double rhs;
    double result;
    hb_result_t r = x87_read_real_mem(ctx, op, &rhs);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), 0, &lhs);
    if (r != HB_OK) return r;

    switch (arith_op) {
        case HB_IR_X87_FADD:  result = lhs + rhs; break;
        case HB_IR_X87_FMUL:  result = lhs * rhs; break;
        case HB_IR_X87_FSUB:  result = lhs - rhs; break;
        case HB_IR_X87_FSUBR: result = rhs - lhs; break;
        case HB_IR_X87_FDIV:  result = lhs / rhs; break;
        case HB_IR_X87_FDIVR: result = rhs / lhs; break;
        default: return HB_ERR_INTERNAL;
    }
    return hb_x87_set_st_f64(hb_context_x87(ctx), 0, result);
}

static hb_result_t x87_fcom_mem(hb_context_t* ctx, const hb_ir_operand_t* op, bool pop_after) {
    double rhs;
    hb_result_t r = x87_read_real_mem(ctx, op, &rhs);
    if (r != HB_OK) return r;
    r = hb_x87_fcom(hb_context_x87(ctx), rhs);
    if (r != HB_OK) return r;
    return pop_after ? hb_x87_pop(hb_context_x87(ctx)) : HB_OK;
}

static hb_result_t x87_st_index(const hb_ir_operand_t* op, unsigned* index) {
    if (!op || !index || op->type != HB_OP_IMM || op->imm < 0 || op->imm > 7) return HB_ERR_INTERNAL;
    *index = (unsigned)op->imm;
    return HB_OK;
}

/* DD D0+i / DD D8+i: copy ST(0) to ST(i), then optionally pop.
 * Register-form FST/FSTP uses the same IR operations as the memory forms,
 * with an immediate logical stack index as the destination. */
static hb_result_t x87_fst_st(hb_context_t* ctx, const hb_ir_operand_t* op,
                              bool pop_after) {
    hb_x87_state_t* x87 = hb_context_x87(ctx);
    unsigned index;
    double value;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(x87, 0, &value);
    if (r == HB_ERR_EXEC_FAULT) r = hb_x87_stack_underflow(x87, &value);
    if (r != HB_OK) return r;
    r = hb_x87_store_st_f64(x87, index, value);
    if (r != HB_OK) return r;
    return pop_after ? hb_x87_fstp_pop(x87) : HB_OK;
}

static hb_result_t x87_fld_st(hb_context_t* ctx, const hb_ir_operand_t* op) {
    unsigned index;
    double value;
    if (op && op->type == HB_OP_IMM && op->imm == -1) return hb_x87_push_f64(hb_context_x87(ctx), 1.0);
    if (op && op->type == HB_OP_IMM && op->imm == -2) {
        /* FLDZ: push 0.0 and set tag to "zero" (01). */
        hb_result_t r = hb_x87_push_f64(hb_context_x87(ctx), 0.0);
        if (r != HB_OK) return r;
        uint16_t shift = (uint16_t)(hb_context_x87(ctx)->top * 2u);
        hb_context_x87(ctx)->tag_word = (uint16_t)((hb_context_x87(ctx)->tag_word & ~(0x3u << shift)) |
                                                (0x1u << shift));
        return HB_OK;
    }
    /* FLD1/FLDZ/FLDL2T/FLDL2E/FLDPI/FLDLG2/FLDLN2 constants. The decoder
     * encodes the constant as a negative imm value: -1=FLD1, -2=FLDZ,
     * -3=FLDL2T (log2(10)), -4=FLDL2E (log2(e)), -5=FLDPI,
     * -6=FLDLG2 (log10(2)), -7=FLDLN2 (ln(2)). Push as a "valid" double. */
    if (op && op->type == HB_OP_IMM) {
        double cnst = 0.0;
        switch (op->imm) {
            case -3: cnst = 3.3219280948873623478703194294894; break;  /* log2(10) */
            case -4: cnst = 1.4426950408889634073599246810019; break;  /* log2(e) */
            case -5: cnst = 3.1415926535897932384626433832795; break;  /* pi */
            case -6: cnst = 0.3010299956639811952137388947245; break;  /* log10(2) */
            case -7: cnst = 0.6931471805599453094172321214582; break;  /* ln(2) */
            default: break;
        }
        if (op->imm >= -7 && op->imm <= -3) {
            return hb_x87_push_f64(hb_context_x87(ctx), cnst);
        }
    }
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &value);
    if (r != HB_OK) return r;
    return hb_x87_push_f64(hb_context_x87(ctx), value);
}

static hb_result_t x87_fxch(hb_context_t* ctx, const hb_ir_operand_t* op) {
    unsigned index;
    double st0;
    double sti;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), 0, &st0);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &sti);
    if (r != HB_OK) return r;
    r = hb_x87_set_st_f64(hb_context_x87(ctx), 0, sti);
    if (r != HB_OK) return r;
    return hb_x87_set_st_f64(hb_context_x87(ctx), index, st0);
}

static hb_result_t x87_arith_st0_sti(hb_context_t* ctx, const hb_ir_operand_t* op, hb_ir_op_t arith_op) {
    unsigned index;
    double lhs;
    double rhs;
    double result;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), 0, &lhs);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &rhs);
    if (r != HB_OK) return r;
    switch (arith_op) {
        case HB_IR_X87_FADD:  result = lhs + rhs; break;
        case HB_IR_X87_FMUL:  result = lhs * rhs; break;
        case HB_IR_X87_FSUB:  result = lhs - rhs; break;
        case HB_IR_X87_FSUBR: result = rhs - lhs; break;
        case HB_IR_X87_FDIV:  result = lhs / rhs; break;
        case HB_IR_X87_FDIVR: result = rhs / lhs; break;
        default: return HB_ERR_INTERNAL;
    }
    return hb_x87_set_st_f64(hb_context_x87(ctx), 0, result);
}

static hb_result_t x87_arith_pop_sti_st0(hb_context_t* ctx, const hb_ir_operand_t* op, hb_ir_op_t arith_op) {
    unsigned index;
    double st0;
    double sti;
    double result;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), 0, &st0);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &sti);
    if (r != HB_OK) return r;
    switch (arith_op) {
        case HB_IR_X87_FADDP:  result = sti + st0; break;
        case HB_IR_X87_FMULP:  result = sti * st0; break;
        case HB_IR_X87_FSUBP:  result = sti - st0; break;
        case HB_IR_X87_FSUBRP: result = st0 - sti; break;
        case HB_IR_X87_FDIVP:  result = sti / st0; break;
        case HB_IR_X87_FDIVRP: result = st0 / sti; break;
        default: return HB_ERR_INTERNAL;
    }
    r = hb_x87_set_st_f64(hb_context_x87(ctx), index, result);
    if (r != HB_OK) return r;
    return hb_x87_pop(hb_context_x87(ctx));
}

static hb_result_t x87_fcom_st(hb_context_t* ctx, const hb_ir_operand_t* op, unsigned pops) {
    unsigned index;
    double rhs;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &rhs);
    if (r != HB_OK) return r;
    r = hb_x87_fcom(hb_context_x87(ctx), rhs);
    if (r != HB_OK) return r;
    while (pops--) {
        r = hb_x87_pop(hb_context_x87(ctx));
        if (r != HB_OK) return r;
    }
    return HB_OK;
}

/* FCOMI/FCOMIP/FUCOMI/FUCOMIP — compare ST(0) to ST(i), set FPU C0/C2/C3
 * (same as FCOM/FUCOM), and ALSO mirror them to EFLAGS as ZF/PF/CF.
 *
 *   lhs < rhs  -> ZF=0 PF=0 CF=1
 *   lhs == rhs -> ZF=1 PF=0 CF=0
 *   lhs > rhs  -> ZF=0 PF=0 CF=0
 *   unordered (NaN)              -> ZF=1 PF=1 CF=1
 *
 * Per Intel SDM Vol 1 §8.1.8 (FCOMI/FCOMIP/FUCOMI/FUCOMIP) and Vol 2A
 * instruction entries. OF/SF/AF are cleared. IF is unchanged.
 *
 * `unordered` is true for FUCOMI/FUCOMIP — for the FPU SW, hb_x87_fcom
 * already produces C0=C2=C3=111 on NaN, so the unordered flag only matters
 * when the regular-FCOM "QNaN raises IE" semantic is wanted (gap matrix
 * #3 — FPU exception flags — not yet implemented). EFLAGS encoding is
 * the same for FCOMI and FUCOMI (NaN -> ZF=PF=CF=1). */
static hb_result_t x87_fcomi_st(hb_context_t* ctx, const hb_ir_operand_t* op,
                                unsigned pops, bool unordered) {
    (void)unordered; /* see comment above */
    unsigned index;
    double lhs, rhs;
    hb_result_t r = x87_st_index(op, &index);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), 0, &lhs);
    if (r != HB_OK) return r;
    r = hb_x87_st_f64(hb_context_x87(ctx), index, &rhs);
    if (r != HB_OK) return r;

    /* Update the FPU C0/C2/C3 condition flags first. */
    r = hb_x87_fcom(hb_context_x87(ctx), rhs);
    if (r != HB_OK) return r;

    /* Mirror to EFLAGS (lazy-flags cleared so values land in canonical slot). */
    hb_lazy_flags_clear(ctx);
    ctx->flags.of = false;
    ctx->flags.sf = false;
    ctx->flags.af = false;
    if (lhs != lhs || rhs != rhs) {
        ctx->flags.zf = true;
        ctx->flags.pf = true;
        ctx->flags.cf = true;
    } else if (lhs < rhs) {
        ctx->flags.zf = false;
        ctx->flags.pf = false;
        ctx->flags.cf = true;
    } else if (lhs == rhs) {
        ctx->flags.zf = true;
        ctx->flags.pf = false;
        ctx->flags.cf = false;
    } else {
        ctx->flags.zf = false;
        ctx->flags.pf = false;
        ctx->flags.cf = false;
    }

    while (pops--) {
        r = hb_x87_pop(hb_context_x87(ctx));
        if (r != HB_OK) return r;
    }
    return HB_OK;
}

static hb_result_t read_operand_value(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t* out) {
    if (!ctx || !op || !out) return HB_ERR_INVALID_ARG;
    if (op->type == HB_OP_REG) {
        *out = read_reg_sized(ctx, op->reg, op->size, op->reg_offset);
        return HB_OK;
    }
    if (op->type == HB_OP_IMM) {
        *out = (uint64_t)op->imm;
        return HB_OK;
    }
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        return mem_read(ctx, addr, out, op->size);
    }
    return HB_ERR_INTERNAL;
}

static hb_result_t write_operand_value(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t value) {
    if (!ctx || !op) return HB_ERR_INVALID_ARG;
    if (op->type == HB_OP_REG) {
        write_reg_sized_offset(ctx, op->reg, value, op->size, op->reg_offset);
        return HB_OK;
    }
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        return mem_write(ctx, addr, value, op->size);
    }
    return HB_ERR_INTERNAL;
}

static uint64_t bswap_sized_value(uint64_t value, hb_size_t size) {
    if (size == HB_SIZE_16) {
        uint16_t v = (uint16_t)value;
        return (uint16_t)((v >> 8) | (v << 8));
    }
    if (size == HB_SIZE_32) {
        uint32_t v = (uint32_t)value;
        return ((v & 0x000000ffU) << 24) |
               ((v & 0x0000ff00U) << 8)  |
               ((v & 0x00ff0000U) >> 8)  |
               ((v & 0xff000000U) >> 24);
    }
    if (size == HB_SIZE_64) {
        uint64_t v = value;
        return ((v & 0x00000000000000ffULL) << 56) |
               ((v & 0x000000000000ff00ULL) << 40) |
               ((v & 0x0000000000ff0000ULL) << 24) |
               ((v & 0x00000000ff000000ULL) << 8)  |
               ((v & 0x000000ff00000000ULL) >> 8)  |
               ((v & 0x0000ff0000000000ULL) >> 24) |
               ((v & 0x00ff000000000000ULL) >> 40) |
               ((v & 0xff00000000000000ULL) >> 56);
    }
    return value;
}

static hb_result_t read_seg_selector(hb_context_t* ctx, uint16_t seg, uint16_t* out) {
    if (!ctx || !out) return HB_ERR_INVALID_ARG;
    switch (seg) {
        case 0: *out = ctx->seg_es; return HB_OK;
        case 1: *out = ctx->seg_cs; return HB_OK;
        case 2: *out = ctx->seg_ss; return HB_OK;
        case 3: *out = ctx->seg_ds; return HB_OK;
        case 4: *out = ctx->seg_fs; return HB_OK;
        case 5: *out = ctx->seg_gs; return HB_OK;
        default: return HB_ERR_UNSUPPORTED_OPCODE;
    }
}

static hb_result_t write_seg_selector(hb_context_t* ctx, uint16_t seg, uint16_t value) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    switch (seg) {
        case 0: ctx->seg_es = value; return HB_OK;
        case 1: ctx->seg_cs = value; return HB_OK;
        case 2: ctx->seg_ss = value; return HB_OK;
        case 3: ctx->seg_ds = value; return HB_OK;
        case 4: ctx->seg_fs = value; return HB_OK;
        case 5: ctx->seg_gs = value; return HB_OK;
        default: return HB_ERR_UNSUPPORTED_OPCODE;
    }
}

static uint64_t status_flags_mask(void) {
    return (1ULL << 0) | (1ULL << 2) | (1ULL << 4) |
           (1ULL << 6) | (1ULL << 7) | (1ULL << 11);
}

static void sync_status_flags_from_image(hb_context_t* ctx, uint64_t image) {
    ctx->flags.cf = (image & (1ULL << 0)) != 0;
    ctx->flags.pf = (image & (1ULL << 2)) != 0;
    ctx->flags.af = (image & (1ULL << 4)) != 0;
    ctx->flags.zf = (image & (1ULL << 6)) != 0;
    ctx->flags.sf = (image & (1ULL << 7)) != 0;
    ctx->flags.of = (image & (1ULL << 11)) != 0;
}

static uint64_t flags_width_mask(hb_size_t size) {
    if (size == HB_SIZE_16) return 0xffffu;
    if (size == HB_SIZE_32) return 0xffffffffu;
    return UINT64_MAX;
}

static hb_result_t read_flags_image(hb_context_t* ctx, hb_size_t size, uint64_t* out) {
    uint64_t image;
    hb_result_t r;
    if (!ctx || !out) return HB_ERR_INVALID_ARG;
    r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ALL);
    if (r != HB_OK) return r;
    image = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.eflags : ctx->regs.x64.rflags;
    image &= ~status_flags_mask();
    image |= ctx->flags.cf ? (1ULL << 0) : 0;
    image |= ctx->flags.pf ? (1ULL << 2) : 0;
    image |= ctx->flags.af ? (1ULL << 4) : 0;
    image |= ctx->flags.zf ? (1ULL << 6) : 0;
    image |= ctx->flags.sf ? (1ULL << 7) : 0;
    image |= ctx->flags.of ? (1ULL << 11) : 0;
    image |= 0x2u;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eflags = (uint32_t)image;
    else ctx->regs.x64.rflags = image;
    *out = image & flags_width_mask(size);
    return HB_OK;
}

static hb_result_t write_flags_image(hb_context_t* ctx, hb_size_t size, uint64_t value) {
    uint64_t image;
    uint64_t mask;
    if (!ctx) return HB_ERR_INVALID_ARG;
    image = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.eflags : ctx->regs.x64.rflags;
    mask = flags_width_mask(size);
    image = (image & ~mask) | (value & mask);
    image |= 0x2u;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eflags = (uint32_t)image;
    else ctx->regs.x64.rflags = image;
    sync_status_flags_from_image(ctx, image);
    hb_lazy_flags_clear(ctx);
    return HB_OK;
}

static void trace_operand(const char* name, hb_context_t* ctx, const hb_ir_operand_t* op) {
    if (!op) return;
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        fprintf(stderr,
                " %s={type=MEM size=%d seg=0x%x base=%d index=%d scale=%u disp=0x%llx addr=0x%llx}",
                name, op->size, op->mem.segment, op->mem.base, op->mem.index,
                op->mem.scale, (unsigned long long)op->mem.disp, (unsigned long long)addr);
    } else if (op->type == HB_OP_REG) {
        fprintf(stderr, " %s={type=REG size=%d reg=%d value=0x%llx}",
                name, op->size, op->reg, (unsigned long long)read_reg(ctx, op->reg));
    } else if (op->type == HB_OP_IMM) {
        fprintf(stderr, " %s={type=IMM size=%d imm=0x%llx}",
                name, op->size, (unsigned long long)op->imm);
    } else {
        fprintf(stderr, " %s={type=%d size=%d}", name, op->type, op->size);
    }
}

static bool trace_atomics_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_ATOMICS) != 0;
}

static int trace_bitops_selected(const hb_ir_instr_t* instr) {
    if (!(trace_runtime_flags & TRACE_FLAG_BITOPS)) return 0;
    if (!trace_cfg.bitops_range_set) return 1;
    if (!instr) return 0;
    return instr->guest_addr >= trace_cfg.bitops_start &&
           instr->guest_addr <= trace_cfg.bitops_end;
}

static unsigned int trace_bitops_budget(void) {
    return trace_cfg.bitops_budget;
}

static const char* ir_op_name(hb_ir_op_t op) {
    switch (op) {
        case HB_IR_NOP: return "NOP";
        case HB_IR_MOV: return "MOV";
        case HB_IR_MOV_SEG: return "MOV_SEG";
        case HB_IR_LEA: return "LEA";
        case HB_IR_ADD: return "ADD";
        case HB_IR_ADC: return "ADC";
        case HB_IR_SUB: return "SUB";
        case HB_IR_SBB: return "SBB";
        case HB_IR_MUL: return "MUL";
        case HB_IR_IMUL: return "IMUL";
        case HB_IR_DIV: return "DIV";
        case HB_IR_IDIV: return "IDIV";
        case HB_IR_BT: return "BT";
        case HB_IR_BTS: return "BTS";
        case HB_IR_BTR: return "BTR";
        case HB_IR_BTC: return "BTC";
        case HB_IR_AND: return "AND";
        case HB_IR_OR: return "OR";
        case HB_IR_XOR: return "XOR";
        case HB_IR_NOT: return "NOT";
        case HB_IR_NEG: return "NEG";
        case HB_IR_SHL: return "SHL";
        case HB_IR_SHR: return "SHR";
        case HB_IR_SAR: return "SAR";
        case HB_IR_ROL: return "ROL";
        case HB_IR_ROR: return "ROR";
        case HB_IR_RCL: return "RCL";
        case HB_IR_RCR: return "RCR";
        case HB_IR_SHLD: return "SHLD";
        case HB_IR_SHRD: return "SHRD";
        case HB_IR_CMP: return "CMP";
        case HB_IR_TEST: return "TEST";
        case HB_IR_CMPXCHG: return "CMPXCHG";
        case HB_IR_CMPXCHG8B: return "CMPXCHG8B";
        case HB_IR_XCHG: return "XCHG";
        case HB_IR_XADD: return "XADD";
        case HB_IR_FENCE: return "FENCE";
        case HB_IR_LAHF: return "LAHF";
        case HB_IR_SAHF: return "SAHF";
        case HB_IR_CPUID: return "CPUID";
        case HB_IR_XGETBV: return "XGETBV";
        case HB_IR_RDRAND: return "RDRAND";
        case HB_IR_RDSEED: return "RDSEED";
        case HB_IR_SETcc: return "SETcc";
        case HB_IR_CMOVcc: return "CMOVcc";
        case HB_IR_LOAD: return "LOAD";
        case HB_IR_STORE: return "STORE";
        case HB_IR_PUSH: return "PUSH";
        case HB_IR_POP: return "POP";
        case HB_IR_PUSHF: return "PUSHF";
        case HB_IR_POPF: return "POPF";
        case HB_IR_CALL: return "CALL";
        case HB_IR_CALLF: return "CALLF";
        case HB_IR_JMPF: return "JMPF";
        case HB_IR_RETF: return "RETF";
        case HB_IR_IRET: return "IRET";
        case HB_IR_INT3: return "INT3";
        case HB_IR_INT: return "INT";
        case HB_IR_INT1: return "INT1";
        case HB_IR_INTO: return "INTO";
        case HB_IR_HLT: return "HLT";
        case HB_IR_IN: return "IN";
        case HB_IR_OUT: return "OUT";
        case HB_IR_XLAT: return "XLAT";
        case HB_IR_PUSH_SEG: return "PUSH_SEG";
        case HB_IR_POP_SEG: return "POP_SEG";
        case HB_IR_CLC: return "CLC";
        case HB_IR_STC: return "STC";
        case HB_IR_CMC: return "CMC";
        case HB_IR_CLD: return "CLD";
        case HB_IR_STD: return "STD";
        case HB_IR_CLI: return "CLI";
        case HB_IR_STI: return "STI";
        case HB_IR_ENTER: return "ENTER";
        case HB_IR_RET: return "RET";
        case HB_IR_JMP: return "JMP";
        case HB_IR_Jcc: return "Jcc";
        case HB_IR_LOOP: return "LOOP";
        case HB_IR_JRCXZ: return "JRCXZ";
        case HB_IR_SIGN_EXTEND: return "SIGN_EXTEND";
        case HB_IR_CWD: return "CWD";
        case HB_IR_MOVS: return "MOVS";
        case HB_IR_CMPS: return "CMPS";
        case HB_IR_LODS: return "LODS";
        case HB_IR_SCAS: return "SCAS";
        case HB_IR_STOS: return "STOS";
        case HB_IR_ZERO_EXTEND: return "ZERO_EXTEND";
        case HB_IR_TRUNC: return "TRUNC";
        case HB_IR_BSF: return "BSF";
        case HB_IR_TZCNT: return "TZCNT";
        case HB_IR_LZCNT: return "LZCNT";
        case HB_IR_BSR: return "BSR";
        case HB_IR_POPCNT: return "POPCNT";
        case HB_IR_BSWAP: return "BSWAP";
        case HB_IR_MOVBE: return "MOVBE";
        case HB_IR_MOVDIR64B: return "MOVDIR64B";
        case HB_IR_CRC32: return "CRC32";
        case HB_IR_ANDN: return "ANDN";
        case HB_IR_BEXTR: return "BEXTR";
        case HB_IR_BLSI: return "BLSI";
        case HB_IR_BLSMSK: return "BLSMSK";
        case HB_IR_BLSR: return "BLSR";
        case HB_IR_BZHI: return "BZHI";
        case HB_IR_MULX: return "MULX";
        case HB_IR_PDEP: return "PDEP";
        case HB_IR_PEXT: return "PEXT";
        case HB_IR_RORX: return "RORX";
        case HB_IR_SARX: return "SARX";
        case HB_IR_SHLX: return "SHLX";
        case HB_IR_SHRX: return "SHRX";
        case HB_IR_ADCX: return "ADCX";
        case HB_IR_ADOX: return "ADOX";
        case HB_IR_XMM_AND: return "XMM_AND";
        case HB_IR_XMM_SCALAR_MOV: return "XMM_SCALAR_MOV";
        case HB_IR_XMM_QWORD_LANE_MOV: return "XMM_QWORD_LANE_MOV";
        case HB_IR_XMM_ANDN: return "XMM_ANDN";
        case HB_IR_XMM_OR: return "XMM_OR";
        case HB_IR_XORPS: return "XORPS";
        case HB_IR_PCMPEQB: return "PCMPEQB";
        case HB_IR_PCMPEQW: return "PCMPEQW";
        case HB_IR_PCMPEQD: return "PCMPEQD";
        case HB_IR_PCMPGTB: return "PCMPGTB";
        case HB_IR_PCMPGTW: return "PCMPGTW";
        case HB_IR_PCMPGTD: return "PCMPGTD";
        case HB_IR_PMOVMSKB: return "PMOVMSKB";
        case HB_IR_MOVMSK: return "MOVMSK";
        case HB_IR_PUNPCK: return "PUNPCK";
        case HB_IR_PACKSSWB: return "PACKSSWB";
        case HB_IR_PACKUSWB: return "PACKUSWB";
        case HB_IR_PACKSSDW: return "PACKSSDW";
        case HB_IR_PMULLW: return "PMULLW";
        case HB_IR_PMULHW: return "PMULHW";
        case HB_IR_PMULHUW: return "PMULHUW";
        case HB_IR_PMADDWD: return "PMADDWD";
        case HB_IR_PADDSB: return "PADDSB";
        case HB_IR_PADDSW: return "PADDSW";
        case HB_IR_PADDUSB: return "PADDUSB";
        case HB_IR_PADDUSW: return "PADDUSW";
        case HB_IR_PAVGB: return "PAVGB";
        case HB_IR_PAVGW: return "PAVGW";
        case HB_IR_PSHUFB: return "PSHUFB";
        case HB_IR_PINSRW: return "PINSRW";
        case HB_IR_PEXTRW: return "PEXTRW";
        case HB_IR_PINSR: return "PINSR";
        case HB_IR_PEXTR: return "PEXTR";
        case HB_IR_INSERTPS: return "INSERTPS";
        case HB_IR_EXTRACTPS: return "EXTRACTPS";
        case HB_IR_PSHUF: return "PSHUF";
        case HB_IR_FSHUF: return "FSHUF";
        case HB_IR_PSRL: return "PSRL";
        case HB_IR_PSRA: return "PSRA";
        case HB_IR_PSLL: return "PSLL";
        case HB_IR_PSRLQ: return "PSRLQ";
        case HB_IR_PSLLQ: return "PSLLQ";
        case HB_IR_PSRLDQ: return "PSRLDQ";
        case HB_IR_PSLLDQ: return "PSLLDQ";
        case HB_IR_MOVD: return "MOVD";
        case HB_IR_CVTDQ2PD: return "CVTDQ2PD";
        case HB_IR_CVTDQ2PS: return "CVTDQ2PS";
        case HB_IR_CVTPS2DQ: return "CVTPS2DQ";
        case HB_IR_CVTTPS2DQ: return "CVTTPS2DQ";
        case HB_IR_CVTPS2PD: return "CVTPS2PD";
        case HB_IR_CVTPD2PS: return "CVTPD2PS";
        case HB_IR_CVTPD2DQ: return "CVTPD2DQ";
        case HB_IR_CVTTPD2DQ: return "CVTTPD2DQ";
        case HB_IR_CVTSS2SD: return "CVTSS2SD";
        case HB_IR_CVTSD2SS: return "CVTSD2SS";
        case HB_IR_CVTSI2SD: return "CVTSI2SD";
        case HB_IR_CVTSI2SS: return "CVTSI2SS";
        case HB_IR_FSQRT: return "FSQRT";
        case HB_IR_FRSQRT: return "FRSQRT";
        case HB_IR_FRCP: return "FRCP";
        case HB_IR_FROUND: return "FROUND";
        case HB_IR_FDP: return "FDP";
        case HB_IR_FADD: return "FADD";
        case HB_IR_FSUB: return "FSUB";
        case HB_IR_FMUL: return "FMUL";
        case HB_IR_FDIV: return "FDIV";
        case HB_IR_ADDSD: return "ADDSD";
        case HB_IR_SUBSD: return "SUBSD";
        case HB_IR_DIVSD: return "DIVSD";
        case HB_IR_MULSD: return "MULSD";
        case HB_IR_MULSS: return "MULSS";
        case HB_IR_DIVSS: return "DIVSS";
        case HB_IR_FMIN: return "FMIN";
        case HB_IR_FMAX: return "FMAX";
        case HB_IR_COMISS: return "COMISS";
        case HB_IR_COMISD: return "COMISD";
        case HB_IR_CVTSD2SI: return "CVTSD2SI";
        case HB_IR_CVTSS2SI: return "CVTSS2SI";
        case HB_IR_CVTTSD2SI: return "CVTTSD2SI";
        case HB_IR_CVTTSS2SI: return "CVTTSS2SI";
        case HB_IR_PADD: return "PADD";
        case HB_IR_PSUB: return "PSUB";
        case HB_IR_VEC_PACKED: return "VEC_PACKED";
        case HB_IR_EVEX_CMP_MASK: return "EVEX_CMP_MASK";
        case HB_IR_VZEROUPPER: return "VZEROUPPER";
        case HB_IR_X87_FLD: return "X87_FLD";
        case HB_IR_X87_FST: return "X87_FST";
        case HB_IR_X87_FSTP: return "X87_FSTP";
        case HB_IR_X87_FILD: return "X87_FILD";
        case HB_IR_X87_FISTP: return "X87_FISTP";
        case HB_IR_X87_FIST: return "X87_FIST";
        case HB_IR_X87_FLDCW: return "X87_FLDCW";
        case HB_IR_X87_FNSTCW: return "X87_FNSTCW";
        case HB_IR_X87_FNSTSW: return "X87_FNSTSW";
        case HB_IR_X87_FLDENV: return "X87_FLDENV";
        case HB_IR_X87_FNSTENV: return "X87_FNSTENV";
        case HB_IR_X87_FRSTOR: return "X87_FRSTOR";
        case HB_IR_X87_FNSAVE: return "X87_FNSAVE";
        case HB_IR_X87_FXSAVE: return "X87_FXSAVE";
        case HB_IR_X87_FXRSTOR: return "X87_FXRSTOR";
        case HB_IR_X87_FADD: return "X87_FADD";
        case HB_IR_X87_FMUL: return "X87_FMUL";
        case HB_IR_X87_FCOM: return "X87_FCOM";
        case HB_IR_X87_FCOMP: return "X87_FCOMP";
        case HB_IR_X87_FUCOM: return "X87_FUCOM";
        case HB_IR_X87_FUCOMP: return "X87_FUCOMP";
        case HB_IR_X87_FCOMI: return "X87_FCOMI";
        case HB_IR_X87_FUCOMI: return "X87_FUCOMI";
        case HB_IR_X87_FCOMIP: return "X87_FCOMIP";
        case HB_IR_X87_FUCOMIP: return "X87_FUCOMIP";
        case HB_IR_X87_FSUB: return "X87_FSUB";
        case HB_IR_X87_FSUBR: return "X87_FSUBR";
        case HB_IR_X87_FDIV: return "X87_FDIV";
        case HB_IR_X87_FDIVR: return "X87_FDIVR";
        case HB_IR_X87_FADDP: return "X87_FADDP";
        case HB_IR_X87_FMULP: return "X87_FMULP";
        case HB_IR_X87_FCOMPP: return "X87_FCOMPP";
        case HB_IR_X87_FSUBP: return "X87_FSUBP";
        case HB_IR_X87_FSUBRP: return "X87_FSUBRP";
        case HB_IR_X87_FDIVP: return "X87_FDIVP";
        case HB_IR_X87_FDIVRP: return "X87_FDIVRP";
        case HB_IR_X87_FXCH: return "X87_FXCH";
        case HB_IR_X87_FRNDINT: return "X87_FRNDINT";
        case HB_IR_X87_FINCSTP: return "X87_FINCSTP";
        case HB_IR_X87_FDECSTP: return "X87_FDECSTP";
        case HB_IR_X87_FNCLEX: return "X87_FNCLEX";
        case HB_IR_X87_FNINIT: return "X87_FNINIT";
        case HB_IR_X87_FXAM: return "X87_FXAM";
        case HB_IR_X87_FSQRT: return "X87_FSQRT";
        case HB_IR_X87_F2XM1: return "X87_F2XM1";
        case HB_IR_X87_FYL2X: return "X87_FYL2X";
        case HB_IR_X87_FPTAN: return "X87_FPTAN";
        case HB_IR_X87_FPATAN: return "X87_FPATAN";
        case HB_IR_X87_FXTRACT: return "X87_FXTRACT";
        case HB_IR_X87_FPREM1: return "X87_FPREM1";
        case HB_IR_X87_FPREM: return "X87_FPREM";
        case HB_IR_X87_FYL2XP1: return "X87_FYL2XP1";
        case HB_IR_X87_FSINCOS: return "X87_FSINCOS";
        case HB_IR_X87_FSCALE: return "X87_FSCALE";
        case HB_IR_X87_FSIN: return "X87_FSIN";
        case HB_IR_X87_FCOS: return "X87_FCOS";
        case HB_IR_X87_FNOP: return "X87_FNOP";
        case HB_IR_X87_FCHS: return "X87_FCHS";
        case HB_IR_X87_FABS: return "X87_FABS";
        case HB_IR_X87_FTST: return "X87_FTST";
        case HB_IR_PUSHA: return "PUSHA";
        case HB_IR_POPA: return "POPA";
        case HB_IR_AAA: return "AAA";
        case HB_IR_AAS: return "AAS";
        case HB_IR_AAM: return "AAM";
        case HB_IR_AAD: return "AAD";
        case HB_IR_DAA: return "DAA";
        case HB_IR_DAS: return "DAS";
        case HB_IR_BOUND: return "BOUND";
        case HB_IR_ARPL: return "ARPL";
        case HB_IR_LDS: return "LDS";
        case HB_IR_LES: return "LES";
        case HB_IR_LFS: return "LFS";
        case HB_IR_LGS: return "LGS";
        case HB_IR_HOST_CALL: return "HOST_CALL";
        case HB_IR_FAULT: return "FAULT";
        case HB_IR_UNSUPPORTED: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}

static void trace_exec_fault(hb_context_t* ctx, const hb_ir_instr_t* instr,
                             hb_result_t result, uint64_t block_guest,
                             size_t ir_idx, uint64_t step_idx) {
    static unsigned int fault_count;
    if (!(trace_runtime_flags & TRACE_FLAG_FAULTS)) return;
    if (++fault_count > 50) {
        if (fault_count == 51)
            fprintf(stderr, "macrunner-hb-fault: budget exhausted, silencing\n");
        return;
    }
    fprintf(stderr,
            "macrunner-hb-fault: pc=0x%llx ir_op=%s(%d) ir_idx=%zu step=%llu "
            "result=%s block=0x%llx guest=0x%llx len=%u "
            "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rsi=0x%llx rdi=0x%llx",
            (unsigned long long)ctx->pc, ir_op_name(instr->op), instr->op, ir_idx,
            (unsigned long long)step_idx, hb_result_string(result),
            (unsigned long long)block_guest, (unsigned long long)instr->guest_addr,
            instr->guest_len,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)ctx->regs.x64.rbx,
            (unsigned long long)ctx->regs.x64.rsp,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rdi);
    if (ctx->mode == HB_MODE_64BIT) {
        fprintf(stderr,
                " rbp=0x%llx r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx",
                (unsigned long long)ctx->regs.x64.rbp,
                (unsigned long long)ctx->regs.x64.r8,
                (unsigned long long)ctx->regs.x64.r9,
                (unsigned long long)ctx->regs.x64.r10,
                (unsigned long long)ctx->regs.x64.r11,
                (unsigned long long)ctx->regs.x64.r12,
                (unsigned long long)ctx->regs.x64.r13,
                (unsigned long long)ctx->regs.x64.r14,
                (unsigned long long)ctx->regs.x64.r15);
    }
    trace_operand("dst", ctx, &instr->dst);
    trace_operand("src1", ctx, &instr->src1);
    trace_operand("src2", ctx, &instr->src2);
    fprintf(stderr, " bytes=");
    for (uint8_t i = 0; i < instr->guest_len && i < 15; i++) {
        uint8_t byte = 0;
        if (ctx && ctx->memory && hb_memory_read_u8(ctx->memory, instr->guest_addr + i, &byte) == HB_OK)
            fprintf(stderr, "%02x", byte);
        else
            fprintf(stderr, "??");
    }
    fprintf(stderr, "\n");

    if (ctx->mode == HB_MODE_64BIT && ctx->memory) {
        uint8_t op0 = 0, op1 = 0;
        (void)hb_memory_read_u8(ctx->memory, instr->guest_addr, &op0);
        (void)hb_memory_read_u8(ctx->memory, instr->guest_addr + 1, &op1);
        if (op0 == 0xcd && op1 == 0x29) {
            fprintf(stderr,
                    "macrunner-fastfail: pc=0x%llx code=0x%llx rsp=0x%llx "
                    "rax=0x%llx rcx=0x%llx rdx=0x%llx rbp=0x%llx r10=0x%llx\n",
                    (unsigned long long)instr->guest_addr,
                    (unsigned long long)ctx->regs.x64.rcx,
                    (unsigned long long)ctx->regs.x64.rsp,
                    (unsigned long long)ctx->regs.x64.rax,
                    (unsigned long long)ctx->regs.x64.rcx,
                    (unsigned long long)ctx->regs.x64.rdx,
                    (unsigned long long)ctx->regs.x64.rbp,
                    (unsigned long long)ctx->regs.x64.r10);
            fprintf(stderr, "macrunner-fastfail-stack:");
            for (unsigned int i = 0; i < 48; i++) {
                uint64_t q = 0;
                hb_gva_t addr = ctx->regs.x64.rsp + (uint64_t)i * 8;
                if (hb_memory_read_u64(ctx->memory, addr, &q) == HB_OK)
                    fprintf(stderr, " [%u]=0x%llx", i, (unsigned long long)q);
                else
                    fprintf(stderr, " [%u]=<unmapped>", i);
            }
            fprintf(stderr, "\n");
            fprintf(stderr, "macrunner-fastfail-codeptrs:");
            for (unsigned int i = 0; i < 96; i++) {
                uint64_t q = 0;
                hb_gva_t addr = ctx->regs.x64.rsp + (uint64_t)i * 8;
                if (hb_memory_read_u64(ctx->memory, addr, &q) == HB_OK &&
                    q >= 0x140001000ULL && q < 0x140600000ULL)
                    fprintf(stderr, " [%u]=0x%llx", i, (unsigned long long)q);
            }
            fprintf(stderr, "\n");
        }
    }
}

static bool trace_pc_matches(const char* val, uint64_t guest_addr) {
    const char* p = val;
    while (p && *p) {
        char* end = NULL;
        uint64_t target = strtoull(p, &end, 0);
        if (end != p && target == guest_addr) return true;
        p = (end && end != p) ? end : p + 1;
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') p++;
    }
    return false;
}

static void trace_pc_probe(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    static unsigned int count;
    const char* val;
    unsigned int limit;
    if (!(trace_runtime_flags & TRACE_FLAG_PC)) return;
    val = trace_cfg.pc_list_set ? trace_cfg.pc_list : NULL;
    limit = trace_cfg.pc_limit;
    if (!val || !val[0] || !ctx || ctx->mode != HB_MODE_64BIT) return;

    if (!trace_pc_matches(val, instr->guest_addr)) return;
    if (++count > limit) return;

    fprintf(stderr,
            "macrunner-hb-pcprobe: pc=0x%llx count=%u "
            "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx "
            "rsi=0x%llx rdi=0x%llx r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
            "flags={cf=%u zf=%u sf=%u of=%u pf=%u af=%u} "
            "lazy={pending=%u kind=%u width=%u lhs=0x%llx rhs=0x%llx result=0x%llx count=0x%llx valid=0x%x materialized=0x%x unsupported=0x%x}\n",
            (unsigned long long)instr->guest_addr, count,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)ctx->regs.x64.rbx,
            (unsigned long long)ctx->regs.x64.rsp,
            (unsigned long long)ctx->regs.x64.rbp,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rdi,
            (unsigned long long)ctx->regs.x64.r8,
            (unsigned long long)ctx->regs.x64.r9,
            (unsigned long long)ctx->regs.x64.r10,
            (unsigned long long)ctx->regs.x64.r11,
            ctx->flags.cf, ctx->flags.zf, ctx->flags.sf,
            ctx->flags.of, ctx->flags.pf, ctx->flags.af,
            ctx->lazy_flags.pending, (unsigned)ctx->lazy_flags.kind,
            (unsigned)ctx->lazy_flags.width,
            (unsigned long long)ctx->lazy_flags.lhs,
            (unsigned long long)ctx->lazy_flags.rhs,
            (unsigned long long)ctx->lazy_flags.result,
            (unsigned long long)ctx->lazy_flags.count,
            ctx->lazy_flags.valid_mask,
            ctx->lazy_flags.materialized_mask,
            ctx->lazy_flags.unsupported_mask);

    const uint64_t ptrs[] = {
        ctx->regs.x64.rcx, ctx->regs.x64.rdx, ctx->regs.x64.r8,
        ctx->regs.x64.r9, ctx->regs.x64.rsi, ctx->regs.x64.rdi
    };
    const char* names[] = { "rcx", "rdx", "r8", "r9", "rsi", "rdi" };
    for (unsigned int p = 0; p < 6; p++) {
        fprintf(stderr, "macrunner-hb-pcprobe-mem: %s=0x%llx bytes=", names[p],
                (unsigned long long)ptrs[p]);
        for (unsigned int i = 0; i < 24; i++) {
            uint8_t b = 0;
            if (hb_memory_read_u8(ctx->memory, ptrs[p] + i, &b) == HB_OK)
                fprintf(stderr, "%02x", b);
            else {
                fprintf(stderr, "??");
                break;
            }
        }
        fprintf(stderr, " utf16=");
        for (unsigned int i = 0; i < 12; i++) {
            uint16_t ch = 0;
            if (hb_memory_read_u16(ctx->memory, ptrs[p] + (uint64_t)i * 2, &ch) != HB_OK) {
                fprintf(stderr, "?");
                break;
            }
            if (!ch) break;
            fprintf(stderr, "%c", (ch >= 32 && ch < 127) ? (char)ch : '.');
        }
        fprintf(stderr, "\n");
    }
}

static bool trace_strcpy_probe_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_STRCPY) != 0;
}

static size_t trace_bounded_strlen(hb_context_t* ctx, uint64_t addr, size_t limit, bool* found_nul) {
    if (found_nul) *found_nul = false;
    if (!ctx || !ctx->memory || addr < 0x10000) return 0;
    for (size_t i = 0; i < limit; i++) {
        uint8_t b = 0;
        if (hb_memory_read_u8(ctx->memory, addr + i, &b) != HB_OK) return i;
        if (b == 0) {
            if (found_nul) *found_nul = true;
            return i;
        }
    }
    return limit;
}

static void trace_ascii_bytes(hb_context_t* ctx, uint64_t addr, size_t limit) {
    fprintf(stderr, "bytes=");
    for (size_t i = 0; i < limit; i++) {
        uint8_t b = 0;
        if (!ctx || !ctx->memory || hb_memory_read_u8(ctx->memory, addr + i, &b) != HB_OK) {
            fprintf(stderr, "??");
            break;
        }
        fprintf(stderr, "%02x", b);
    }
    fprintf(stderr, " ascii=\"");
    for (size_t i = 0; i < limit; i++) {
        uint8_t b = 0;
        if (!ctx || !ctx->memory || hb_memory_read_u8(ctx->memory, addr + i, &b) != HB_OK) break;
        if (b == 0) break;
        fputc((b >= 32 && b < 127) ? (int)b : '.', stderr);
    }
    fprintf(stderr, "\"");
}

static void trace_strcpy_probe(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    static unsigned int count;
    unsigned int limit;
    const char* label = NULL;
    uint64_t src = 0;
    bool have_src = false;
    bool found_nul = false;
    size_t actual_len = 0;
    uint64_t object_src = 0;

    if (!trace_strcpy_probe_enabled() || !ctx || ctx->mode != HB_MODE_64BIT || !instr) return;
    limit = trace_cfg.strcpy_limit;

    switch (instr->guest_addr) {
        case 0x1403e72e0ULL: label = "before-strlen-call"; src = ctx->regs.x64.rcx; have_src = true; break;
        case 0x1403e72e5ULL:
            label = "after-strlen-return";
            if (hb_memory_read_u64(ctx->memory, ctx->regs.x64.rbx, &object_src) == HB_OK) {
                src = object_src;
                have_src = true;
            }
            break;
        case 0x1403e72ecULL: label = "before-alloc-size"; break;
        case 0x1403e72fcULL: label = "before-safe-copy-call"; src = ctx->regs.x64.r8; have_src = true; break;
        case 0x1403fb6b0ULL: label = "safe-copy-entry"; src = ctx->regs.x64.r8; have_src = true; break;
        case 0x1403fb70dULL: label = "safe-copy-erange-a"; break;
        case 0x1403fb718ULL: label = "safe-copy-erange-b"; break;
        default: return;
    }

    if (++count > limit) {
        if (count == limit + 1) fprintf(stderr, "macrunner-strcpy-probe: budget exhausted\n");
        return;
    }

    if (have_src) actual_len = trace_bounded_strlen(ctx, src, 256, &found_nul);

    fprintf(stderr,
            "macrunner-strcpy-probe: label=%s pc=0x%llx count=%u "
            "rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx rsp=0x%llx rbp=0x%llx "
            "r8=0x%llx r9=0x%llx rsi=0x%llx rdi=0x%llx src=0x%llx "
            "actual_len256=%zu nul=%u object_src=0x%llx ",
            label,
            (unsigned long long)instr->guest_addr,
            count,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rcx,
            (unsigned long long)ctx->regs.x64.rdx,
            (unsigned long long)ctx->regs.x64.rbx,
            (unsigned long long)ctx->regs.x64.rsp,
            (unsigned long long)ctx->regs.x64.rbp,
            (unsigned long long)ctx->regs.x64.r8,
            (unsigned long long)ctx->regs.x64.r9,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rdi,
            (unsigned long long)src,
            actual_len,
            found_nul ? 1u : 0u,
            (unsigned long long)object_src);
    if (have_src) trace_ascii_bytes(ctx, src, 64);
    fprintf(stderr, "\n");
}

static int trace_branches_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_BRANCHES) != 0;
}

static int trace_branch_selected(const hb_ir_instr_t* instr) {
    if (!trace_cfg.branch_range_set) return 1;
    if (!instr) return 0;
    return instr->guest_addr >= trace_cfg.branch_start &&
           instr->guest_addr <= trace_cfg.branch_end;
}

static unsigned int trace_branch_budget(void) {
    return trace_cfg.branch_budget;
}

static void trace_branch_event(hb_context_t* ctx, const hb_ir_instr_t* instr,
                               const char* phase, uint64_t rsp_before,
                               uint64_t stack_qword, uint64_t target) {
    static unsigned int branch_count;
    if (!trace_branches_enabled()) return;
    if (!trace_branch_selected(instr)) return;
    if (++branch_count > trace_branch_budget()) {
        if (branch_count == trace_branch_budget() + 1)
            fprintf(stderr, "macrunner-hb-branch: budget exhausted, silencing\n");
        return;
    }
    fprintf(stderr,
            "macrunner-hb-branch: phase=%s op=%s guest=0x%llx len=%u "
            "target=0x%llx rsp_before=0x%llx rsp_after=0x%llx stack_qword=0x%llx "
            "rax=0x%llx rbx=0x%llx rbp=0x%llx rsi=0x%llx rdi=0x%llx "
            "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
            phase ? phase : "?", ir_op_name(instr->op),
            (unsigned long long)instr->guest_addr, instr->guest_len,
            (unsigned long long)target,
            (unsigned long long)rsp_before,
            (unsigned long long)ctx->regs.x64.rsp,
            (unsigned long long)stack_qword,
            (unsigned long long)ctx->regs.x64.rax,
            (unsigned long long)ctx->regs.x64.rbx,
            (unsigned long long)ctx->regs.x64.rbp,
            (unsigned long long)ctx->regs.x64.rsi,
            (unsigned long long)ctx->regs.x64.rdi,
            (unsigned long long)ctx->regs.x64.r12,
            (unsigned long long)ctx->regs.x64.r13,
            (unsigned long long)ctx->regs.x64.r14,
            (unsigned long long)ctx->regs.x64.r15);
}

static bool operand_is_xmm_or_vecmem(const hb_ir_operand_t* op) {
    if (!op) return false;
    if (op->type == HB_OP_REG) return is_xmm_reg(op->reg);
    return op->type == HB_OP_MEM && (op->size == 16 || op->size == 32);
}

static bool trace_simd_op_selected(const hb_ir_instr_t* instr) {
    if (!instr) return false;
    switch (instr->op) {
        case HB_IR_XMM_AND:
        case HB_IR_XMM_ANDN:
        case HB_IR_XMM_OR:
        case HB_IR_XORPS:
        case HB_IR_PCMPEQB:
        case HB_IR_PCMPEQW:
        case HB_IR_PCMPEQD:
        case HB_IR_PCMPGTB:
        case HB_IR_PCMPGTW:
        case HB_IR_PCMPGTD:
        case HB_IR_PMOVMSKB:
        case HB_IR_MOVMSK:
        case HB_IR_PUNPCK:
        case HB_IR_PACKSSWB:
        case HB_IR_PACKUSWB:
        case HB_IR_PACKSSDW:
        case HB_IR_PMULLW:
        case HB_IR_PMULHW:
        case HB_IR_PMULHUW:
        case HB_IR_PMADDWD:
        case HB_IR_PADDSB:
        case HB_IR_PADDSW:
        case HB_IR_PADDUSB:
        case HB_IR_PADDUSW:
        case HB_IR_PAVGB:
        case HB_IR_PAVGW:
        case HB_IR_PSHUFB:
        case HB_IR_PINSRW:
        case HB_IR_PEXTRW:
        case HB_IR_PINSR:
        case HB_IR_PEXTR:
        case HB_IR_INSERTPS:
        case HB_IR_EXTRACTPS:
        case HB_IR_PSHUF:
        case HB_IR_FSHUF:
        case HB_IR_XMM_QWORD_LANE_MOV:
        case HB_IR_PSRL:
        case HB_IR_PSRA:
        case HB_IR_PSLL:
        case HB_IR_PSRLQ:
        case HB_IR_PSLLQ:
        case HB_IR_PSRLDQ:
        case HB_IR_PSLLDQ:
        case HB_IR_MOVD:
        case HB_IR_CVTDQ2PD:
        case HB_IR_CVTDQ2PS:
        case HB_IR_CVTPS2DQ:
        case HB_IR_CVTTPS2DQ:
        case HB_IR_CVTPS2PD:
        case HB_IR_CVTPD2PS:
        case HB_IR_CVTPD2DQ:
        case HB_IR_CVTTPD2DQ:
        case HB_IR_CVTSS2SD:
        case HB_IR_CVTSD2SS:
        case HB_IR_CVTSI2SD:
        case HB_IR_CVTSI2SS:
        case HB_IR_FSQRT:
        case HB_IR_FRSQRT:
        case HB_IR_FRCP:
        case HB_IR_FROUND:
        case HB_IR_FDP:
        case HB_IR_FADD:
        case HB_IR_FSUB:
        case HB_IR_FMUL:
        case HB_IR_FDIV:
        case HB_IR_ADDSD:
        case HB_IR_SUBSD:
        case HB_IR_DIVSD:
        case HB_IR_MULSD:
        case HB_IR_DIVSS:
        case HB_IR_MULSS:
        case HB_IR_FMIN:
        case HB_IR_FMAX:
        case HB_IR_COMISS:
        case HB_IR_COMISD:
        case HB_IR_CVTSD2SI:
        case HB_IR_CVTSS2SI:
        case HB_IR_CVTTSD2SI:
        case HB_IR_CVTTSS2SI:
        case HB_IR_PADD:
        case HB_IR_PSUB:
        case HB_IR_VEC_PACKED:
        case HB_IR_VZEROUPPER:
            return true;
        case HB_IR_LOAD:
            return operand_is_xmm_or_vecmem(&instr->dst) || operand_is_xmm_or_vecmem(&instr->src1);
        case HB_IR_STORE:
            return operand_is_xmm_or_vecmem(&instr->src1) || operand_is_xmm_or_vecmem(&instr->src2);
        case HB_IR_MOV:
            return operand_is_xmm_or_vecmem(&instr->dst) || operand_is_xmm_or_vecmem(&instr->src1);
        default:
            return false;
    }
}

static void trace_simd_exec(hb_context_t* ctx, const hb_ir_instr_t* instr,
                            uint64_t block_guest, size_t ir_idx, uint64_t step_idx) {
    static unsigned int simd_count;
    if (!(trace_runtime_flags & TRACE_FLAG_SIMD)) return;
    if (!trace_simd_op_selected(instr)) return;

    unsigned int limit = trace_cfg.simd_budget;
    if (++simd_count > limit) {
        if (simd_count == limit + 1)
            fprintf(stderr, "macrunner-hb-simd: budget exhausted, silencing\n");
        return;
    }

    fprintf(stderr,
            "macrunner-hb-simd: op=%s(%d) guest=0x%llx block=0x%llx ir_idx=%zu step=%llu "
            "dst=t%d/r%d/s%d src1=t%d/r%d/s%d src2=t%d/r%d/s%d bytes=",
            ir_op_name(instr->op), instr->op,
            (unsigned long long)instr->guest_addr,
            (unsigned long long)block_guest, ir_idx,
            (unsigned long long)step_idx,
            instr->dst.type, instr->dst.reg, instr->dst.size,
            instr->src1.type, instr->src1.reg, instr->src1.size,
            instr->src2.type, instr->src2.reg, instr->src2.size);
    for (uint8_t i = 0; i < instr->guest_len && i < 15; i++) {
        uint8_t byte = 0;
        if (ctx && ctx->memory && hb_memory_read_u8(ctx->memory, instr->guest_addr + i, &byte) == HB_OK)
            fprintf(stderr, "%02x", byte);
        else
            fprintf(stderr, "??");
    }
    fprintf(stderr, "\n");
}

static bool trace_simd_data_enabled(void) {
    return (trace_runtime_flags & TRACE_FLAG_SIMD_DATA) != 0;
}

static bool trace_simd_data_in_range(uint64_t guest) {
    if (!trace_cfg.simd_data_range_set) return true;
    return guest >= trace_cfg.simd_data_start && guest <= trace_cfg.simd_data_end;
}

static bool trace_simd_data_take_budget(void) {
    static unsigned int count;
    unsigned int limit = trace_cfg.simd_data_budget;
    if (++count > limit) {
        if (count == limit + 1)
            fprintf(stderr, "macrunner-hb-simd-data: budget exhausted, silencing\n");
        return false;
    }
    return true;
}

static void trace_simd_data_hex(const uint8_t* bytes, size_t count) {
    for (size_t i = 0; i < count; i++) fprintf(stderr, "%02x", bytes[i]);
}

static size_t trace_simd_operand_bytes(const hb_ir_operand_t* op, size_t fallback) {
    size_t bytes = bytes_for_size(op->size);
    if (!bytes) bytes = fallback;
    if (!bytes) bytes = 16;
    return bytes > 16 ? 16 : bytes;
}

static void trace_simd_data_operand(hb_context_t* ctx, const char* label,
                                    const hb_ir_operand_t* op, size_t fallback) {
    uint8_t bytes[16] = {0};
    size_t count = trace_simd_operand_bytes(op, fallback);

    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) {
        uint64_t xmm[2] = {0, 0};
        hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
        memcpy(bytes, xmm, sizeof(bytes));
        fprintf(stderr, " %s=xmm%d/%zu:", label, op->reg - HB_REG_XMM0, count);
        if (r == HB_OK) trace_simd_data_hex(bytes, count);
        else fprintf(stderr, "ERR%d", r);
        return;
    }

    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        hb_result_t r = hb_memory_read(ctx->memory, addr, bytes, count);
        fprintf(stderr, " %s=mem@0x%llx/%zu:", label, (unsigned long long)addr, count);
        if (r == HB_OK) trace_simd_data_hex(bytes, count);
        else fprintf(stderr, "ERR%d", r);
        return;
    }

    if (op->type == HB_OP_REG) {
        uint64_t value = read_reg_sized(ctx, op->reg, op->size, op->reg_offset);
        memcpy(bytes, &value, count > sizeof(value) ? sizeof(value) : count);
        fprintf(stderr, " %s=reg%d/%zu:", label, op->reg, count);
        trace_simd_data_hex(bytes, count);
        return;
    }

    if (op->type == HB_OP_IMM) {
        uint64_t value = (uint64_t)op->imm;
        memcpy(bytes, &value, count > sizeof(value) ? sizeof(value) : count);
        fprintf(stderr, " %s=imm/%zu:", label, count);
        trace_simd_data_hex(bytes, count);
    }
}

static void trace_simd_data_exec(hb_context_t* ctx, const hb_ir_instr_t* instr,
                                 const char* phase, uint64_t block_guest,
                                 size_t ir_idx, uint64_t step_idx) {
    if (!trace_simd_data_enabled()) return;
    if (!trace_simd_op_selected(instr)) return;
    if (!trace_simd_data_in_range(instr->guest_addr)) return;
    if (!trace_simd_data_take_budget()) return;

    size_t fallback = 16;
    if (instr->op == HB_IR_LOAD) fallback = trace_simd_operand_bytes(&instr->src1, 16);
    else if (instr->op == HB_IR_STORE) fallback = trace_simd_operand_bytes(&instr->src1, 16);
    else if (instr->op == HB_IR_MOV) fallback = trace_simd_operand_bytes(&instr->dst, 16);

    fprintf(stderr,
            "macrunner-hb-simd-data: phase=%s op=%s guest=0x%llx block=0x%llx ir_idx=%zu step=%llu",
            phase, ir_op_name(instr->op), (unsigned long long)instr->guest_addr,
            (unsigned long long)block_guest, ir_idx, (unsigned long long)step_idx);
    trace_simd_data_operand(ctx, "dst", &instr->dst, fallback);
    trace_simd_data_operand(ctx, "src1", &instr->src1, fallback);
    trace_simd_data_operand(ctx, "src2", &instr->src2, fallback);
    fprintf(stderr, "\n");
}

static hb_result_t exec_instr(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_result_t r;
    switch (instr->op) {
        case HB_IR_NOP:
            return HB_OK;

        case HB_IR_FENCE:
            switch ((hb_fence_kind_t)instr->src1.imm) {
                case HB_FENCE_ACQUIRE:
                    __atomic_thread_fence(__ATOMIC_ACQUIRE);
                    break;
                case HB_FENCE_RELEASE:
                    __atomic_thread_fence(__ATOMIC_RELEASE);
                    break;
                case HB_FENCE_FULL:
                default:
                    __atomic_thread_fence(__ATOMIC_SEQ_CST);
                    break;
            }
            return HB_OK;

        case HB_IR_MOV: {
            if (instr->dst.type == HB_OP_REG && instr->src1.type == HB_OP_REG &&
                is_xmm_reg(instr->dst.reg) && is_xmm_reg(instr->src1.reg)) {
                size_t bytes = bytes_for_size(instr->dst.size);
                if (bytes == 0) bytes = 16;
                unsigned lane = evex_target_arg(instr) & 0xffu;
                if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) lane = 4;
                uint8_t src[64], dst[64];
                r = read_vec_reg_bytes(ctx, instr->src1.reg, src, bytes);
                if (r != HB_OK) return r;
                if (bytes >= 16) return write_vec_reg_bytes_evex_masked(ctx, instr, src, bytes, lane);
                r = read_vec_reg_bytes(ctx, instr->dst.reg, dst, 16);
                if (r != HB_OK) return r;
                memcpy(dst, src, bytes);
                return write_vec_reg_bytes_evex_masked(ctx, instr, dst, 16, lane);
            }
            uint64_t val = 0;
            if (instr->src1.type == HB_OP_REG)
                val = read_reg_sized(ctx, instr->src1.reg, instr->src1.size, instr->src1.reg_offset);
            else if (instr->src1.type == HB_OP_IMM) val = (uint64_t)instr->src1.imm;
            else return HB_ERR_INTERNAL;
            if (instr->dst.type == HB_OP_REG) {
                hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
                if (!size) size = HB_SIZE_64;
                write_reg_sized_offset(ctx, instr->dst.reg, val, size, instr->dst.reg_offset);
            }
            else return HB_ERR_INTERNAL;
            return HB_OK;
        }

        case HB_IR_MOV_SEG: {
            if (instr->src1.type == HB_OP_IMM && instr->dst.type != HB_OP_IMM) {
                uint16_t selector = 0;
                r = read_seg_selector(ctx, (uint16_t)instr->src1.imm, &selector);
                if (r != HB_OK) return r;
                return write_operand_value(ctx, &instr->dst, selector);
            }
            if (instr->dst.type == HB_OP_IMM && instr->src1.type != HB_OP_IMM) {
                uint64_t selector = 0;
                r = read_operand_value(ctx, &instr->src1, &selector);
                if (r != HB_OK) return r;
                return write_seg_selector(ctx, (uint16_t)instr->dst.imm, (uint16_t)selector);
            }
            return HB_ERR_INTERNAL;
        }

        case HB_IR_LEA: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            if (instr->dst.type == HB_OP_REG) write_reg_sized(ctx, instr->dst.reg, addr, instr->dst.size);
            else return HB_ERR_INTERNAL;
            return HB_OK;
        }

        case HB_IR_ADD:
        case HB_IR_ADC:
        case HB_IR_SUB:
        case HB_IR_SBB:
        case HB_IR_AND:
        case HB_IR_OR:
        case HB_IR_XOR: {
            r = hb_flags_exec_binop_operand(ctx, instr->op, &instr->dst, &instr->src1, &instr->src2, NULL);
            if (r != HB_OK) return r;
            return HB_OK;
        }

        case HB_IR_IMUL: {
            uint64_t lhs = 0, rhs = 0;
            if (instr->dst.type == HB_OP_NONE) {
                r = read_operand_value(ctx, &instr->src1, &rhs);
                if (r != HB_OK) return r;
                hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
                if (size == HB_SIZE_8) {
                    int16_t result = (int16_t)((int8_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_8, 0) * (int8_t)rhs);
                    write_reg_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT8_MIN || result > INT8_MAX);
                } else if (size == HB_SIZE_16) {
                    int32_t result = (int32_t)((int16_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_16, 0) * (int16_t)rhs);
                    write_reg_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    write_reg_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT16_MIN || result > INT16_MAX);
                } else if (size == HB_SIZE_32) {
                    int64_t result = (int64_t)(int32_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_32, 0) * (int64_t)(int32_t)rhs;
                    write_reg_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                    write_reg_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                    ctx->flags.cf = ctx->flags.of = (result < INT32_MIN || result > INT32_MAX);
                } else if (size == HB_SIZE_64) {
                    __int128 result = (__int128)(int64_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_64, 0) * (__int128)(int64_t)rhs;
                    write_reg_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                    write_reg_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                    ctx->flags.cf = ctx->flags.of = (result < (__int128)INT64_MIN || result > (__int128)INT64_MAX);
                } else return HB_ERR_UNSUPPORTED_OPCODE;
                hb_lazy_flags_clear(ctx);
                return HB_OK;
            }
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            r = read_operand_value(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            int64_t slhs = (int64_t)sign_extend_from_size(trunc_to_size(lhs, size), size);
            int64_t srhs = (int64_t)sign_extend_from_size(trunc_to_size(rhs, size), size);
            __int128 full = (__int128)slhs * (__int128)srhs;
            uint64_t result = trunc_to_size((uint64_t)full, size);
            __int128 truncated_signed = (__int128)(int64_t)sign_extend_from_size(result, size);
            write_reg_sized(ctx, instr->dst.reg, result, size);
            ctx->flags.cf = ctx->flags.of = (full != truncated_signed);
            hb_lazy_flags_clear(ctx); /* x86 leaves several flags undefined for truncated IMUL. */
            return HB_OK;
        }

        case HB_IR_MUL: {
            uint64_t src = 0;
            r = read_operand_value(ctx, &instr->src1, &src);
            if (r != HB_OK) return r;
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            src = trunc_to_size(src, size);

            if (size == HB_SIZE_8) {
                uint16_t result = (uint16_t)((uint8_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_8, 0) * (uint8_t)src);
                write_reg_sized(ctx, HB_REG_RAX, result, HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 8) != 0);
            } else if (size == HB_SIZE_16) {
                uint32_t result = (uint32_t)(uint16_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_16, 0) * (uint32_t)(uint16_t)src;
                write_reg_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                write_reg_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 16) != 0);
            } else if (size == HB_SIZE_32) {
                uint64_t result = (uint64_t)(uint32_t)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_32, 0) * (uint64_t)(uint32_t)src;
                write_reg_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                write_reg_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                ctx->flags.cf = ctx->flags.of = ((result >> 32) != 0);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 result = (unsigned __int128)read_reg_sized(ctx, HB_REG_RAX, HB_SIZE_64, 0) * (unsigned __int128)src;
                write_reg_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                write_reg_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                ctx->flags.cf = ctx->flags.of = ((uint64_t)(result >> 64) != 0);
            } else return HB_ERR_UNSUPPORTED_OPCODE;

            hb_lazy_flags_clear(ctx);
            return HB_OK;
        }

        case HB_IR_DIV: {
            uint64_t divisor = 0;
            r = read_operand_value(ctx, &instr->src1, &divisor);
            if (r != HB_OK) return r;

            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            divisor = trunc_to_size(divisor, size);
            if (!divisor) return HB_ERR_EXEC_FAULT; /* x86 #DE: divide by zero. */

            if (size == HB_SIZE_8) {
                uint16_t dividend = (uint16_t)(read_reg(ctx, HB_REG_RAX) & 0xffff);
                uint16_t quotient = dividend / (uint8_t)divisor;
                uint16_t remainder = dividend % (uint8_t)divisor;
                if (quotient > 0xffU) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, (uint16_t)((remainder << 8) | quotient), HB_SIZE_16);
            } else if (size == HB_SIZE_16) {
                uint32_t dividend = ((uint32_t)(read_reg(ctx, HB_REG_RDX) & 0xffff) << 16) |
                                    (uint32_t)(read_reg(ctx, HB_REG_RAX) & 0xffff);
                uint32_t quotient = dividend / (uint32_t)divisor;
                uint32_t remainder = dividend % (uint32_t)divisor;
                if (quotient > 0xffffU) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, quotient, HB_SIZE_16);
                write_reg_sized(ctx, HB_REG_RDX, remainder, HB_SIZE_16);
            } else if (size == HB_SIZE_32) {
                uint64_t dividend = ((uint64_t)(uint32_t)read_reg(ctx, HB_REG_RDX) << 32) |
                                    (uint64_t)(uint32_t)read_reg(ctx, HB_REG_RAX);
                uint64_t quotient = dividend / (uint32_t)divisor;
                uint64_t remainder = dividend % (uint32_t)divisor;
                if (quotient > 0xffffffffULL) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, quotient, HB_SIZE_32);
                write_reg_sized(ctx, HB_REG_RDX, remainder, HB_SIZE_32);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 dividend = ((unsigned __int128)read_reg(ctx, HB_REG_RDX) << 64) |
                                             (unsigned __int128)read_reg(ctx, HB_REG_RAX);
                unsigned __int128 quotient = dividend / divisor;
                unsigned __int128 remainder = dividend % divisor;
                if (quotient > UINT64_MAX) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                write_reg_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
            } else {
                return HB_ERR_UNSUPPORTED_OPCODE;
            }

            hb_lazy_flags_clear(ctx); /* DIV leaves status flags undefined. */
            return HB_OK;
        }

        case HB_IR_IDIV: {
            uint64_t divisor_raw = 0;
            r = read_operand_value(ctx, &instr->src1, &divisor_raw);
            if (r != HB_OK) return r;
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t divisor_bits = trunc_to_size(divisor_raw, size);
            int64_t divisor = (int64_t)sign_extend_from_size(divisor_bits, size);
            if (!divisor) return HB_ERR_EXEC_FAULT;

            if (size == HB_SIZE_8) {
                int16_t dividend = (int16_t)(read_reg(ctx, HB_REG_RAX) & 0xffff);
                int64_t quotient = dividend / (int8_t)divisor;
                int64_t remainder = dividend % (int8_t)divisor;
                if (quotient < INT8_MIN || quotient > INT8_MAX) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX,
                                (uint16_t)(((uint8_t)remainder << 8) | (uint8_t)quotient),
                                HB_SIZE_16);
            } else if (size == HB_SIZE_16) {
                int32_t dividend = (int32_t)(((uint32_t)(read_reg(ctx, HB_REG_RDX) & 0xffff) << 16) |
                                             (uint32_t)(read_reg(ctx, HB_REG_RAX) & 0xffff));
                int64_t quotient = dividend / (int16_t)divisor;
                int64_t remainder = dividend % (int16_t)divisor;
                if (quotient < INT16_MIN || quotient > INT16_MAX) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, (uint16_t)quotient, HB_SIZE_16);
                write_reg_sized(ctx, HB_REG_RDX, (uint16_t)remainder, HB_SIZE_16);
            } else if (size == HB_SIZE_32) {
                int64_t dividend = (int64_t)(((uint64_t)(uint32_t)read_reg(ctx, HB_REG_RDX) << 32) |
                                             (uint64_t)(uint32_t)read_reg(ctx, HB_REG_RAX));
                int64_t quotient = dividend / (int32_t)divisor;
                int64_t remainder = dividend % (int32_t)divisor;
                if (quotient < INT32_MIN || quotient > INT32_MAX) return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, (uint32_t)quotient, HB_SIZE_32);
                write_reg_sized(ctx, HB_REG_RDX, (uint32_t)remainder, HB_SIZE_32);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 bits = ((unsigned __int128)read_reg(ctx, HB_REG_RDX) << 64) |
                                         (unsigned __int128)read_reg(ctx, HB_REG_RAX);
                __int128 dividend = (__int128)bits;
                __int128 quotient = dividend / (int64_t)divisor;
                __int128 remainder = dividend % (int64_t)divisor;
                if (quotient < (__int128)INT64_MIN || quotient > (__int128)INT64_MAX)
                    return HB_ERR_EXEC_FAULT;
                write_reg_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                write_reg_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
            } else return HB_ERR_UNSUPPORTED_OPCODE;

            hb_lazy_flags_clear(ctx);
            return HB_OK;
        }

        case HB_IR_BT:
        case HB_IR_BTS:
        case HB_IR_BTR:
        case HB_IR_BTC: {
            uint64_t base = 0, bit_raw = 0;
            hb_ir_operand_t target = instr->src1;
            r = read_operand_value(ctx, &instr->src1, &base);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &bit_raw);
            if (r != HB_OK) return r;

            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            unsigned width = (size == HB_SIZE_64) ? 64 : (size == HB_SIZE_16) ? 16 : 32;
            uint64_t bit = bit_raw & (width - 1);

            if (target.type == HB_OP_MEM && bit_raw >= width) {
                uint64_t addr = resolve_addr(ctx, &target);
                addr += (bit_raw / width) * (width / 8);
                target.mem.base = HB_REG_COUNT;
                target.mem.index = HB_REG_COUNT;
                target.mem.disp = (int64_t)addr;
                bit = bit_raw % width;
                r = read_operand_value(ctx, &target, &base);
                if (r != HB_OK) return r;
            }

            uint64_t mask = 1ULL << bit;
            uint64_t value = trunc_to_size(base, size);
            uint64_t new_value = value;
            ctx->flags.cf = (value & mask) != 0;
            hb_lazy_flags_clear(ctx);

            if (instr->op == HB_IR_BTS) {
                new_value = value | mask;
                r = write_operand_value(ctx, &target, new_value);
                if (r != HB_OK) return r;
            } else if (instr->op == HB_IR_BTR) {
                new_value = value & ~mask;
                r = write_operand_value(ctx, &target, new_value);
                if (r != HB_OK) return r;
            } else if (instr->op == HB_IR_BTC) {
                new_value = value ^ mask;
                r = write_operand_value(ctx, &target, new_value);
                if (r != HB_OK) return r;
            }
            if (trace_bitops_selected(instr)) {
                static unsigned int bitop_count;
                if (++bitop_count <= trace_bitops_budget()) {
                    uint64_t addr = 0;
                    if (target.type == HB_OP_MEM) addr = resolve_addr(ctx, &target);
                    fprintf(stderr,
                            "macrunner-hb-bitop: pc=0x%llx op=%s count=%u target_type=%d "
                            "addr=0x%llx size=%d bit_raw=0x%llx bit=%llu mask=0x%llx "
                            "old=0x%llx new=0x%llx cf=%u\n",
                            (unsigned long long)instr->guest_addr, ir_op_name(instr->op), bitop_count,
                            target.type, (unsigned long long)addr, target.size,
                            (unsigned long long)bit_raw, (unsigned long long)bit,
                            (unsigned long long)mask, (unsigned long long)value,
                            (unsigned long long)trunc_to_size(new_value, size), ctx->flags.cf);
                }
            }
            return HB_OK;
        }

        case HB_IR_CMP: {
            uint64_t a = 0;
            r = read_operand_value(ctx, &instr->src1, &a);
            if (r != HB_OK) return r;
            uint64_t b = 0;
            r = read_operand_value(ctx, &instr->src2, &b);
            if (r != HB_OK) return r;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, instr->src1.size, a, b, a - b, 0);
            return HB_OK;
        }

        case HB_IR_TEST: {
            uint64_t a = 0;
            r = read_operand_value(ctx, &instr->src1, &a);
            if (r != HB_OK) return r;
            uint64_t b = 0;
            r = read_operand_value(ctx, &instr->src2, &b);
            if (r != HB_OK) return r;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_TEST, instr->src1.size, a, b, a & b, 0);
            return HB_OK;
        }

        case HB_IR_CMPXCHG: {
            /* x86 LOCK-prefixed RMW implies a full memory barrier (TSO). ARM64 is weakly
             * ordered, so model it explicitly — otherwise a cross-thread publisher's store
             * is never observed by a spin-reader (livelock). See CLAUDE-GATE-DIAGNOSIS UPDATE 4. */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            uint64_t dst_val = 0;
            uint64_t src_val = 0;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = instr->src2.size ? instr->src2.size : HB_SIZE_32;

            r = read_operand_value(ctx, &instr->src1, &dst_val);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &src_val);
            if (r != HB_OK) return r;

            uint64_t acc = read_reg_sized(ctx, HB_REG_RAX, size, 0);
            dst_val = trunc_to_size(dst_val, size);
            src_val = trunc_to_size(src_val, size);

            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, size, acc, dst_val, acc - dst_val, 0);
            if (trace_atomics_enabled()) {
                fprintf(stderr,
                        "macrunner-hb-atomic: pc=0x%llx op=CMPXCHG size=%d acc=0x%llx dst=0x%llx src=0x%llx equal=%d",
                        (unsigned long long)ctx->pc, size, (unsigned long long)acc,
                        (unsigned long long)dst_val, (unsigned long long)src_val, acc == dst_val);
                trace_operand("dstop", ctx, &instr->src1);
                trace_operand("srcop", ctx, &instr->src2);
                fprintf(stderr, "\n");
            }
            if (acc == dst_val) {
                r = write_operand_value(ctx, &instr->src1, src_val);
                if (r != HB_OK) return r;
            } else {
                write_reg_sized(ctx, HB_REG_RAX, dst_val, size);
            }
            return HB_OK;
        }

        case HB_IR_CMPXCHG8B: {
            __atomic_thread_fence(__ATOMIC_SEQ_CST); /* x86 LOCK full barrier (TSO) — see UPDATE 4 */
            if (instr->dst.size == HB_SIZE_128) {
                uint64_t mem[2] = {0, 0};
                uint64_t acc[2] = {read_reg(ctx, HB_REG_RAX), read_reg(ctx, HB_REG_RDX)};
                uint64_t src[2] = {read_reg(ctx, HB_REG_RBX), read_reg(ctx, HB_REG_RCX)};
                uint64_t addr;
                bool equal;

                if (ctx->mode != HB_MODE_64BIT || instr->dst.type != HB_OP_MEM)
                    return HB_ERR_INTERNAL;
                addr = resolve_addr(ctx, &instr->dst);
                r = hb_memory_read(ctx->memory, addr, mem, sizeof(mem));
                if (r != HB_OK) return r;
                equal = mem[0] == acc[0] && mem[1] == acc[1];
                hb_lazy_flags_clear(ctx);
                ctx->flags.zf = equal;
                if (equal) {
                    r = hb_memory_write(ctx->memory, addr, src, sizeof(src));
                    if (r != HB_OK) return r;
                } else {
                    write_reg_sized(ctx, HB_REG_RAX, mem[0], HB_SIZE_64);
                    write_reg_sized(ctx, HB_REG_RDX, mem[1], HB_SIZE_64);
                }
                return HB_OK;
            }

            uint64_t mem = 0;
            uint64_t acc = ((uint64_t)(uint32_t)read_reg(ctx, HB_REG_RDX) << 32) |
                           (uint32_t)read_reg(ctx, HB_REG_RAX);
            uint64_t src = ((uint64_t)(uint32_t)read_reg(ctx, HB_REG_RCX) << 32) |
                           (uint32_t)read_reg(ctx, HB_REG_RBX);
            bool equal;

            r = read_operand_value(ctx, &instr->dst, &mem);
            if (r != HB_OK) return r;
            equal = (mem == acc);
            hb_lazy_flags_clear(ctx);
            ctx->flags.zf = equal;
            if (equal) {
                r = write_operand_value(ctx, &instr->dst, src);
                if (r != HB_OK) return r;
            } else {
                write_reg_sized(ctx, HB_REG_RAX, (uint32_t)mem, HB_SIZE_32);
                write_reg_sized(ctx, HB_REG_RDX, (uint32_t)(mem >> 32), HB_SIZE_32);
            }
            return HB_OK;
        }

        case HB_IR_XCHG: {
            /* XCHG with a memory operand is implicitly LOCK'd on x86 → full barrier (TSO). */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            uint64_t dst_val = 0;
            uint64_t src_val = 0;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = instr->src2.size ? instr->src2.size : HB_SIZE_32;

            r = read_operand_value(ctx, &instr->src1, &dst_val);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &src_val);
            if (r != HB_OK) return r;

            dst_val = trunc_to_size(dst_val, size);
            src_val = trunc_to_size(src_val, size);

            if (trace_atomics_enabled()) {
                fprintf(stderr,
                        "macrunner-hb-atomic: pc=0x%llx op=XCHG size=%d dst=0x%llx src=0x%llx",
                        (unsigned long long)ctx->pc, size, (unsigned long long)dst_val,
                        (unsigned long long)src_val);
                trace_operand("dstop", ctx, &instr->src1);
                trace_operand("srcop", ctx, &instr->src2);
                fprintf(stderr, "\n");
            }

            r = write_operand_value(ctx, &instr->src1, src_val);
            if (r != HB_OK) return r;
            r = write_operand_value(ctx, &instr->src2, dst_val);
            if (r != HB_OK) return r;
            return HB_OK;
        }

        case HB_IR_XADD: {
            /* x86 LOCK XADD implies a full memory barrier (TSO). */
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            uint64_t dst_val = 0;
            uint64_t src_val = 0;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = instr->src2.size ? instr->src2.size : HB_SIZE_32;

            r = read_operand_value(ctx, &instr->src1, &dst_val);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &src_val);
            if (r != HB_OK) return r;

            dst_val = trunc_to_size(dst_val, size);
            src_val = trunc_to_size(src_val, size);
            uint64_t result = trunc_to_size(dst_val + src_val, size);
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_ADD, size, dst_val, src_val, result, 0);

            if (trace_atomics_enabled()) {
                fprintf(stderr,
                        "macrunner-hb-atomic: pc=0x%llx op=XADD size=%d dst=0x%llx src=0x%llx result=0x%llx",
                        (unsigned long long)ctx->pc, size, (unsigned long long)dst_val,
                        (unsigned long long)src_val, (unsigned long long)result);
                trace_operand("dstop", ctx, &instr->src1);
                trace_operand("srcop", ctx, &instr->src2);
                fprintf(stderr, "\n");
            }

            r = write_operand_value(ctx, &instr->src1, result);
            if (r != HB_OK) return r;
            r = write_operand_value(ctx, &instr->src2, dst_val);
            if (r != HB_OK) return r;
            return HB_OK;
        }

        case HB_IR_LOAD: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg)) {
                uint8_t xmm[64] = {0};
                size_t bytes = bytes_for_size(instr->src1.size);
                if (bytes == 0) bytes = 16;
                unsigned lane = evex_target_arg(instr) & 0xffu;
                if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) lane = 4;
                if (bytes < 16 && !instr->zero_upper) {
                    r = read_vec_reg_bytes(ctx, instr->dst.reg, xmm, 16);
                    if (r != HB_OK) return r;
                }
                r = hb_memory_read(ctx->memory, addr, xmm, bytes);
                if (r != HB_OK) return r;
                trace_mem_watch_bytes(ctx, "read", addr, xmm, bytes, NULL);
                return write_vec_reg_bytes_evex_masked(ctx, instr, xmm, bytes < 16 ? 16 : bytes, lane);
            }
            uint64_t val = 0;
            r = mem_read(ctx, addr, &val, instr->dst.size);
            if (r != HB_OK) return r;
            if (instr->dst.type == HB_OP_REG)
                write_reg_sized_offset(ctx, instr->dst.reg, val, instr->dst.size, instr->dst.reg_offset);
            else return HB_ERR_INTERNAL;
            return HB_OK;
        }

        case HB_IR_STORE: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            if (instr->src2.type == HB_OP_REG && is_xmm_reg(instr->src2.reg)) {
                uint8_t xmm[64];
                size_t bytes = bytes_for_size(instr->src1.size);
                if (bytes == 0) bytes = bytes_for_size(instr->src2.size);
                if (bytes == 0) bytes = 16;
                r = read_vec_reg_bytes(ctx, instr->src2.reg, xmm, bytes);
                if (r != HB_OK) return r;
                uint64_t first_qword = 0;
                memcpy(&first_qword, xmm, sizeof(first_qword));
                trace_guest_native_write(ctx, "interp_xmm_store", addr, first_qword, (hb_size_t)bytes);
                uint8_t before[16] = {0};
                if (trace_mem_watch_enabled())
                    (void)hb_memory_read(ctx->memory, addr, before, bytes > sizeof(before) ? sizeof(before) : bytes);
                if (evex_target_present(instr) && evex_target_mask(instr) != 0) {
                    uint64_t k = ctx->k[evex_target_mask(instr) & 7u];
                    unsigned lane = evex_target_arg(instr) & 0xffu;
                    if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) lane = 4;
                    for (size_t off = 0; off < bytes; off += lane) {
                        if (!((k >> (off / lane)) & 1u)) continue;
                        r = hb_memory_write(ctx->memory, addr + off, xmm + off, lane);
                        if (r != HB_OK) break;
                    }
                } else {
                    r = hb_memory_write(ctx->memory, addr, xmm, bytes);
                }
                if (r != HB_OK) trace_stack_write_fault(ctx, addr, first_qword, (hb_size_t)bytes, r);
                else trace_mem_watch_bytes(ctx, "write", addr, xmm, bytes, before);
                return r;
            }
            uint64_t val = 0;
            if (instr->src2.type == HB_OP_REG)
                val = read_reg_sized(ctx, instr->src2.reg, instr->src2.size, instr->src2.reg_offset);
            else if (instr->src2.type == HB_OP_IMM) val = (uint64_t)instr->src2.imm;
            else return HB_ERR_INTERNAL;
            r = mem_write(ctx, addr, val, instr->src2.size);
            if (r != HB_OK) return r;
            return HB_OK;
        }

        case HB_IR_XMM_QWORD_LANE_MOV: {
            unsigned dst_lane = (unsigned)(instr->target & 0xff);
            unsigned src_lane = (unsigned)((instr->target >> 8) & 0xff);
            if (dst_lane > 1 || src_lane > 1) return HB_ERR_INTERNAL;
            if (instr->dst.type == HB_OP_MEM) {
                if (instr->src1.type != HB_OP_REG || !is_xmm_reg(instr->src1.reg)) return HB_ERR_INTERNAL;
                uint64_t src[2];
                r = read_xmm_reg(ctx, instr->src1.reg, src);
                if (r != HB_OK) return r;
                uint64_t addr = resolve_addr(ctx, &instr->dst);
                return hb_memory_write(ctx->memory, addr, &src[src_lane], sizeof(uint64_t));
            }
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t dst[2];
            r = read_xmm_reg(ctx, instr->dst.reg, dst);
            if (r != HB_OK) return r;
            if (instr->src1.type == HB_OP_REG && is_xmm_reg(instr->src1.reg)) {
                uint64_t src[2];
                r = read_xmm_reg(ctx, instr->src1.reg, src);
                if (r != HB_OK) return r;
                dst[dst_lane] = src[src_lane];
            } else if (instr->src1.type == HB_OP_MEM) {
                uint64_t lane = 0;
                uint64_t addr = resolve_addr(ctx, &instr->src1);
                r = hb_memory_read(ctx->memory, addr, &lane, sizeof(lane));
                if (r != HB_OK) return r;
                dst[dst_lane] = lane;
            } else {
                return HB_ERR_INTERNAL;
            }
            r = write_xmm_reg(ctx, instr->dst.reg, dst);
            if (r != HB_OK) return r;
            if (trace_current_instr && trace_current_instr->zero_ymm_upper) {
                unsigned n = (unsigned)(instr->dst.reg - HB_REG_XMM0);
                if (ctx->mode == HB_MODE_32BIT) return HB_OK;
                if (n < 16) memset(ctx->ymm_hi[n], 0, sizeof(ctx->ymm_hi[n]));
            }
            return HB_OK;
        }

        case HB_IR_X87_FLD:
            if (instr->src1.type == HB_OP_IMM) return x87_fld_st(ctx, &instr->src1);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fld_mem(ctx, &instr->src1);

        case HB_IR_X87_FST:
            if (instr->dst.type == HB_OP_IMM) return x87_fst_st(ctx, &instr->dst, false);
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fst_mem(ctx, &instr->dst);

        case HB_IR_X87_FSTP:
            if (instr->dst.type == HB_OP_IMM) return x87_fst_st(ctx, &instr->dst, true);
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fstp_mem(ctx, &instr->dst);

        case HB_IR_X87_FILD:
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fild_mem(ctx, &instr->src1);

        case HB_IR_X87_FISTP:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fistp_mem(ctx, &instr->dst);

        case HB_IR_X87_FIST:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fist_mem(ctx, &instr->dst);

        case HB_IR_X87_FLDCW:
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fldcw_mem(ctx, &instr->src1);

        case HB_IR_X87_FNSTCW:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fnstcw_mem(ctx, &instr->dst);

        case HB_IR_X87_FNSTSW:
            if (instr->dst.type == HB_OP_REG) {
                return write_operand_value(ctx, &instr->dst, hb_context_x87(ctx)->status_word);
            }
            if (instr->dst.type == HB_OP_MEM) {
                return x87_fnstsw_mem(ctx, &instr->dst);
            }
            return HB_ERR_INTERNAL;

        case HB_IR_X87_FLDENV:
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fldenv_mem(ctx, &instr->src1);

        case HB_IR_X87_FNSTENV:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fnstenv_mem(ctx, &instr->dst);

        case HB_IR_X87_FRSTOR:
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_frstor_mem(ctx, &instr->src1);

        case HB_IR_X87_FNSAVE:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fnsave_mem(ctx, &instr->dst);

        case HB_IR_X87_FXSAVE:
            if (instr->dst.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fxsave_mem(ctx, &instr->dst);

        case HB_IR_X87_FXRSTOR:
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fxrstor_mem(ctx, &instr->src1);

        case HB_IR_X87_FADD:
        case HB_IR_X87_FMUL:
        case HB_IR_X87_FSUB:
        case HB_IR_X87_FSUBR:
        case HB_IR_X87_FDIV:
        case HB_IR_X87_FDIVR:
            if (instr->src1.type == HB_OP_IMM) return x87_arith_st0_sti(ctx, &instr->src1, instr->op);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_arith_mem(ctx, &instr->src1, instr->op);

        case HB_IR_X87_FCOM:
            if (instr->src1.type == HB_OP_IMM) return x87_fcom_st(ctx, &instr->src1, 0);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fcom_mem(ctx, &instr->src1, false);

        case HB_IR_X87_FCOMP:
            if (instr->src1.type == HB_OP_IMM) return x87_fcom_st(ctx, &instr->src1, 1);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fcom_mem(ctx, &instr->src1, true);

        case HB_IR_X87_FUCOM:
            if (instr->src1.type == HB_OP_IMM) return x87_fcom_st(ctx, &instr->src1, 0);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fcom_mem(ctx, &instr->src1, false);

        case HB_IR_X87_FUCOMP:
            if (instr->src1.type == HB_OP_IMM) return x87_fcom_st(ctx, &instr->src1, 1);
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            return x87_fcom_mem(ctx, &instr->src1, true);

        case HB_IR_X87_FCOMI:
            return x87_fcomi_st(ctx, &instr->src1, 0, false);
        case HB_IR_X87_FUCOMI:
            return x87_fcomi_st(ctx, &instr->src1, 0, true);
        case HB_IR_X87_FCOMIP:
            return x87_fcomi_st(ctx, &instr->src1, 1, false);
        case HB_IR_X87_FUCOMIP:
            return x87_fcomi_st(ctx, &instr->src1, 1, true);

        case HB_IR_X87_FADDP:
        case HB_IR_X87_FMULP:
        case HB_IR_X87_FSUBP:
        case HB_IR_X87_FSUBRP:
        case HB_IR_X87_FDIVP:
        case HB_IR_X87_FDIVRP:
            return x87_arith_pop_sti_st0(ctx, &instr->src1, instr->op);

        case HB_IR_X87_FCOMPP:
            return x87_fcom_st(ctx, &instr->src1, 2);

        case HB_IR_X87_FXCH:
            return x87_fxch(ctx, &instr->src1);

        case HB_IR_X87_FRNDINT:
            return hb_x87_frndint(hb_context_x87(ctx));

        case HB_IR_X87_FINCSTP:
            return hb_x87_fincstp(hb_context_x87(ctx));

        case HB_IR_X87_FDECSTP:
            return hb_x87_fdecstp(hb_context_x87(ctx));

        case HB_IR_X87_FNCLEX:
            return hb_x87_fnclex(hb_context_x87(ctx));

        case HB_IR_X87_FNINIT:
            return hb_x87_fninit(hb_context_x87(ctx));

        case HB_IR_X87_FXAM:
            return hb_x87_fxam(hb_context_x87(ctx));

        case HB_IR_X87_FSQRT:   return hb_x87_fsqrt(hb_context_x87(ctx));
        case HB_IR_X87_F2XM1:   return hb_x87_f2xm1(hb_context_x87(ctx));
        case HB_IR_X87_FYL2X:   return hb_x87_fyl2x(hb_context_x87(ctx));
        case HB_IR_X87_FPTAN:   return hb_x87_fptan(hb_context_x87(ctx));
        case HB_IR_X87_FPATAN:  return hb_x87_fpatan(hb_context_x87(ctx));
        case HB_IR_X87_FXTRACT: return hb_x87_fxtract(hb_context_x87(ctx));
        case HB_IR_X87_FPREM1:  return hb_x87_fprem1(hb_context_x87(ctx));
        case HB_IR_X87_FPREM:   return hb_x87_fprem(hb_context_x87(ctx));
        case HB_IR_X87_FYL2XP1: return hb_x87_fyl2xp1(hb_context_x87(ctx));
        case HB_IR_X87_FSINCOS: return hb_x87_fsincos(hb_context_x87(ctx));
        case HB_IR_X87_FSCALE:  return hb_x87_fscale(hb_context_x87(ctx));
        case HB_IR_X87_FSIN:    return hb_x87_fsin(hb_context_x87(ctx));
        case HB_IR_X87_FCOS:    return hb_x87_fcos(hb_context_x87(ctx));
        case HB_IR_X87_FNOP:    return hb_x87_fnop(hb_context_x87(ctx));
        case HB_IR_X87_FCHS:    return hb_x87_fchs(hb_context_x87(ctx));
        case HB_IR_X87_FABS:    return hb_x87_fabs(hb_context_x87(ctx));
        case HB_IR_X87_FTST:    return hb_x87_ftst(hb_context_x87(ctx));

        case HB_IR_PUSHA: {
            /* PUSHA / PUSHAD — push EAX/ECX/EDX/EBX/EBP/ESI/EDI then the original ESP.
             * Order: EAX, ECX, EDX, EBX, original ESP, EBP, ESI, EDI. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t word_size = (instr->src1.size == HB_SIZE_16) ? 2 : 4;
            uint32_t esp_save = ctx->regs.x86.esp;
            uint32_t values[8];
            values[0] = ctx->regs.x86.eax;
            values[1] = ctx->regs.x86.ecx;
            values[2] = ctx->regs.x86.edx;
            values[3] = ctx->regs.x86.ebx;
            values[4] = esp_save;
            values[5] = ctx->regs.x86.ebp;
            values[6] = ctx->regs.x86.esi;
            values[7] = ctx->regs.x86.edi;
            /* PUSHA pushes registers in the order EAX, ECX, EDX, EBX, original
             * ESP, EBP, ESI, EDI. With ESP pre-decrement, the first push lands
             * at the HIGHEST address of the 8-slot block and the last push lands
             * at the LOWEST. So loop from i=0 to i=7 (NOT 7→0). */
            for (int i = 0; i < 8; i++) {
                if (word_size == 2) {
                    ctx->regs.x86.esp -= 2;
                    r = hb_memory_write_u16(ctx->memory, ctx->regs.x86.esp, (uint16_t)values[i]);
                } else {
                    ctx->regs.x86.esp -= 4;
                    r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, values[i]);
                }
                if (r != HB_OK) return r;
            }
            return HB_OK;
        }

        case HB_IR_POPA: {
            /* POPA / POPAD — reverse of PUSHA: pop EDI, ESI, EBP, (skip), EBX, EDX, ECX, EAX. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t word_size = (instr->dst.size == HB_SIZE_16) ? 2 : 4;
            uint32_t values[8];
            for (int i = 0; i < 8; i++) {
                if (word_size == 2) {
                    uint16_t v16 = 0;
                    r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp, &v16);
                    if (r != HB_OK) return r;
                    values[i] = v16;
                    ctx->regs.x86.esp += 2;
                } else {
                    r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &values[i]);
                    if (r != HB_OK) return r;
                    ctx->regs.x86.esp += 4;
                }
            }
            ctx->regs.x86.edi = values[0];
            ctx->regs.x86.esi = values[1];
            ctx->regs.x86.ebp = values[2];
            /* values[3] is the discarded ESP value. */
            ctx->regs.x86.ebx = values[4];
            ctx->regs.x86.edx = values[5];
            ctx->regs.x86.ecx = values[6];
            ctx->regs.x86.eax = values[7];
            return HB_OK;
        }

        case HB_IR_AAA: {
            /* AAA — ASCII Adjust After Addition. Modifies AL/AH and AF/CF. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            bool a = ((al & 0x0fu) > 9) || ctx->flags.af;
            if (a) {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | ((al + 6) & 0x0fu);
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffff00ffu) | (((ctx->regs.x86.eax >> 8) + 1) << 8);
            } else {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | (al & 0x0fu);
            }
            ctx->flags.af = a;
            ctx->flags.cf = a;
            return HB_OK;
        }

        case HB_IR_AAS: {
            /* AAS — ASCII Adjust After Subtraction (Intel SDM Vol.2A):
             *   IF (AL AND 0Fh) > 9 OR AF:  AX -= 6; AH -= 1; AF=CF=1
             *   ELSE                         AF=CF=0
             *   AL := AL AND 0Fh
             * NOTE: Unicorn 2.1.4 diverges (decrements AH by 2). We follow the
             * SDM = real-silicon contract; AAS is EXCLUDED from the Unicorn diff
             * (see HB-I386-DECODE-COMPLETE report, "Oracle Divergences"). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            bool a = ((al & 0x0fu) > 9) || ctx->flags.af;
            if (a) {
                uint16_t ax = (uint16_t)((ctx->regs.x86.eax & 0xffffu) - 6u); /* AX -= 6 (borrow into AH) */
                uint8_t ah = (uint8_t)((ax >> 8) - 1u);                       /* AH -= 1 */
                uint8_t new_al = (uint8_t)(ax & 0x0fu);                       /* AL := AL AND 0Fh */
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffff0000u) | ((uint32_t)ah << 8) | new_al;
            } else {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | (al & 0x0fu);
            }
            ctx->flags.af = a;
            ctx->flags.cf = a;
            return HB_OK;
        }

        case HB_IR_AAM: {
            /* AAM — ASCII Adjust After Multiply. AL = AL % imm8; AH = AL / imm8. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t base = (uint8_t)instr->src1.imm;
            if (base == 0) return HB_ERR_EXEC_FAULT;  /* #DE on divide by zero */
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            uint8_t ah = (uint8_t)((al / base) & 0xffu);
            uint8_t new_al = (uint8_t)(al % base);
            ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | new_al;
            ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffff00ffu) | ((uint32_t)ah << 8);
            /* SF/ZF/PF are set based on the new AL. */
            ctx->flags.sf = (new_al & 0x80u) != 0;
            ctx->flags.zf = new_al == 0;
            ctx->flags.pf = parity_even_u8(new_al);
            ctx->flags.cf = false;
            ctx->flags.of = false;
            ctx->flags.af = false;
            return HB_OK;
        }

        case HB_IR_AAD: {
            /* AAD — ASCII Adjust Before Division. AL = (AH * imm8 + AL) & 0xff; AH = 0. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t base = (uint8_t)instr->src1.imm;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            uint8_t ah = (uint8_t)(ctx->regs.x86.eax >> 8);
            uint8_t new_al = (uint8_t)((ah * base + al) & 0xffu);
            ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | new_al;
            ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffff00ffu);
            ctx->flags.sf = (new_al & 0x80u) != 0;
            ctx->flags.zf = new_al == 0;
            ctx->flags.pf = parity_even_u8(new_al);
            ctx->flags.cf = false;
            ctx->flags.of = false;
            ctx->flags.af = false;
            return HB_OK;
        }

        case HB_IR_DAA: {
            /* DAA — Decimal Adjust AL After Addition. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            bool cf_old = ctx->flags.cf;
            bool old_cf = cf_old;
            bool old_af = ctx->flags.af;
            if (((al & 0x0fu) > 9) || old_af) {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | ((al + 6) & 0xffu);
                ctx->flags.cf = old_cf || (al > 0xf9u);
                ctx->flags.af = true;
            } else {
                ctx->flags.af = false;
            }
            uint8_t al_after = (uint8_t)ctx->regs.x86.eax;
            if ((al_after > 0x99u) || old_cf) {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | ((al_after + 0x60u) & 0xffu);
                ctx->flags.cf = true;
            } else {
                ctx->flags.cf = false;
            }
            uint8_t new_al = (uint8_t)ctx->regs.x86.eax;
            ctx->flags.sf = (new_al & 0x80u) != 0;
            ctx->flags.zf = new_al == 0;
            ctx->flags.pf = parity_even_u8(new_al);
            ctx->flags.of = false;
            return HB_OK;
        }

        case HB_IR_DAS: {
            /* DAS — Decimal Adjust AL After Subtraction (Intel SDM Vol.2A).
             * Second-adjust condition: IF (old_AL > 99h) OR (old_CF) THEN AL -= 60h.
             * NOTE: Unicorn 2.1.4 drops the old_AL>99h clause (gates on CF only);
             * we follow the SDM = real-silicon contract; DAS is EXCLUDED from the
             * Unicorn diff (see HB-I386-DECODE-COMPLETE report, "Oracle Divergences"). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            bool old_cf = ctx->flags.cf;
            bool old_af = ctx->flags.af;
            if (((al & 0x0fu) > 9) || old_af) {
                uint8_t new_al = (uint8_t)(al - 6);
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | new_al;
                ctx->flags.cf = old_cf || (al < 6);
                ctx->flags.af = true;
            } else {
                ctx->flags.af = false;
            }
            /* Second adjust (SDM): gate on old_AL > 99h OR old_CF. */
            if (al > 0x99u || old_cf) {
                uint8_t al_after = (uint8_t)ctx->regs.x86.eax;
                uint8_t newer_al = (uint8_t)(al_after - 0x60u);
                ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xffffff00u) | newer_al;
                ctx->flags.cf = true;
            }
            uint8_t new_al = (uint8_t)ctx->regs.x86.eax;
            ctx->flags.sf = (new_al & 0x80u) != 0;
            ctx->flags.zf = new_al == 0;
            ctx->flags.pf = parity_even_u8(new_al);
            ctx->flags.of = false;
            return HB_OK;
        }

        case HB_IR_BOUND: {
            /* BOUND r16/32, m16/32&16/32 — array bounds check. Out-of-range → #BR.
             * dst = register, src1 = mBOUND (low, high pair). For the flat-memory
             * games HyperBridge targets this rarely trips; we surface the trap. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint64_t idx = 0;
            r = read_operand_value(ctx, &instr->dst, &idx);
            if (r != HB_OK) return r;
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            hb_size_t elem = (instr->dst.size == HB_SIZE_16) ? HB_SIZE_16 : HB_SIZE_32;
            uint64_t lo = 0, hi = 0;
            r = mem_read(ctx, addr, &lo, elem);
            if (r != HB_OK) return r;
            r = mem_read(ctx, addr + hb_size_bytes(elem), &hi, elem);
            if (r != HB_OK) return r;
            uint64_t ix = (elem == HB_SIZE_16) ? (idx & 0xffffu) : (idx & 0xffffffffu);
            uint64_t l = (elem == HB_SIZE_16) ? (uint16_t)lo : (uint32_t)lo;
            uint64_t h = (elem == HB_SIZE_16) ? (uint16_t)hi : (uint32_t)hi;
            /* Intel: out-of-range if index < low OR index > high (both signed). */
            int64_t s_ix = (elem == HB_SIZE_16) ? (int16_t)ix : (int32_t)ix;
            int64_t s_lo = (elem == HB_SIZE_16) ? (int16_t)l : (int32_t)l;
            int64_t s_hi = (elem == HB_SIZE_16) ? (int16_t)h : (int32_t)h;
            if (s_ix < s_lo || s_ix > s_hi) {
                return HB_ERR_EXEC_FAULT;  /* #BR equivalent. */
            }
            return HB_OK;
        }

        case HB_IR_ARPL: {
            /* ARPL r/m16, r16 — Adjust RPL Field of Selector.
             * If dst.RPL < src.RPL: ZF=1, dst.RPL = src.RPL. Else ZF=0. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t dst = 0, src = 0;
            if (instr->src1.type == HB_OP_REG) src = (uint16_t)read_reg_sized(ctx, instr->src1.reg, HB_SIZE_16, 0);
            else if (instr->src1.type == HB_OP_IMM) src = (uint16_t)instr->src1.imm;
            else if (instr->src1.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->src1);
                r = hb_memory_read_u16(ctx->memory, addr, &src);
                if (r != HB_OK) return r;
            } else return HB_ERR_INTERNAL;
            if (instr->dst.type == HB_OP_REG) dst = (uint16_t)read_reg_sized(ctx, instr->dst.reg, HB_SIZE_16, 0);
            else if (instr->dst.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->dst);
                r = hb_memory_read_u16(ctx->memory, addr, &dst);
                if (r != HB_OK) return r;
            } else return HB_ERR_INTERNAL;
            uint8_t dpl = dst & 0x3;
            uint8_t rpl = src & 0x3;
            if (rpl > dpl) {
                uint16_t nd = (dst & ~0x3u) | rpl;
                if (instr->dst.type == HB_OP_REG) write_reg_sized(ctx, instr->dst.reg, nd, HB_SIZE_16);
                else if (instr->dst.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->dst);
                    r = hb_memory_write_u16(ctx->memory, addr, nd);
                    if (r != HB_OK) return r;
                }
                ctx->flags.zf = true;
            } else {
                ctx->flags.zf = false;
            }
            return HB_OK;
        }

        case HB_IR_LDS:
        case HB_IR_LES:
        case HB_IR_LFS:
        case HB_IR_LGS: {
            /* Far pointer load: dst GPR = m32 offset, segment = m16 selector.
             * Memory operand is m48 (4 bytes offset + 2 bytes selector) OR m32 (FS/GS
             * ignore the high half in some encodings — but standard encoding is the
             * same 6-byte form). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            uint32_t off = 0;
            uint16_t sel = 0;
            r = hb_memory_read_u32(ctx->memory, addr, &off);
            if (r != HB_OK) return r;
            r = hb_memory_read_u16(ctx->memory, addr + 4, &sel);
            if (r != HB_OK) return r;
            if (instr->dst.type == HB_OP_REG) write_reg_sized(ctx, instr->dst.reg, off, HB_SIZE_32);
            else return HB_ERR_INTERNAL;
            /* Set the corresponding segment selector. For LDS/LES the visible seg is
             * updated; for LFS/LGS we also update the segment base (flat-model assumes
             * base 0; we just propagate the selector and let the rest of the translator
             * continue with selector-based addressing). */
            uint16_t which;
            switch (instr->op) {
                case HB_IR_LDS: which = 3; break;  /* DS = 3 */
                case HB_IR_LES: which = 0; break;  /* ES = 0 */
                case HB_IR_LFS: which = 4; break;  /* FS = 4 */
                case HB_IR_LGS: which = 5; break;  /* GS = 5 */
                default: return HB_ERR_INTERNAL;
            }
            r = write_seg_selector(ctx, which, sel);
            if (r != HB_OK) return r;
            if (instr->op == HB_IR_LFS) ctx->fs_base = 0;
            if (instr->op == HB_IR_LGS) ctx->gs_base = 0;
            return HB_OK;
        }

        case HB_IR_PUSHF: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t value = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            r = read_flags_image(ctx, size, &value);
            if (r != HB_OK) return r;
            if (ctx->mode == HB_MODE_32BIT) {
                if (size == HB_SIZE_16) {
                    ctx->regs.x86.esp -= 2;
                    r = hb_memory_write_u16(ctx->memory, ctx->regs.x86.esp, (uint16_t)value);
                } else {
                    ctx->regs.x86.esp -= 4;
                    r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t)value);
                }
            } else {
                ctx->regs.x64.rsp -= 8;
                r = hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp, value);
            }
            if (r != HB_OK) return r;
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, "pushf", rsp_before, value, 0);
            return HB_OK;
        }

        case HB_IR_POPF: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t value = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (ctx->mode == HB_MODE_32BIT) {
                if (size == HB_SIZE_16) {
                    uint16_t v16 = 0;
                    r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp, &v16);
                    if (r != HB_OK) return r;
                    value = v16;
                    ctx->regs.x86.esp += 2;
                } else {
                    uint32_t v32 = 0;
                    r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &v32);
                    if (r != HB_OK) return r;
                    value = v32;
                    ctx->regs.x86.esp += 4;
                }
            } else {
                r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &value);
                if (r != HB_OK) return r;
                ctx->regs.x64.rsp += 8;
            }
            r = write_flags_image(ctx, size, value);
            if (r != HB_OK) return r;
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, "popf", rsp_before, value, value);
            return HB_OK;
        }

        case HB_IR_CLC: ctx->flags.cf = 0; return HB_OK;
        case HB_IR_STC: ctx->flags.cf = 1; return HB_OK;
        case HB_IR_CMC: ctx->flags.cf = !ctx->flags.cf; return HB_OK;
        case HB_IR_CLD: {
            /* Clear DF (bit 10 of EFLAGS). */
            if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eflags &= ~(1u << 10);
            else ctx->regs.x64.rflags &= ~(1ULL << 10);
            return HB_OK;
        }
        case HB_IR_STD: {
            if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eflags |= (1u << 10);
            else ctx->regs.x64.rflags |= (1ULL << 10);
            return HB_OK;
        }
        case HB_IR_CLI: /* Interrupt flag — HyperBridge runs with IF=1; ignore. */ return HB_OK;
        case HB_IR_STI: return HB_OK;

        case HB_IR_PUSH_SEG: {
            /* i386-only: PUSH ES/CS/SS/DS (32-bit user mode).
             * src1.size = element size, src2.imm = segment selector. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t seg = (uint16_t)instr->src2.imm;
            uint16_t selector = ctx->regs.x86.seg[seg] & 0xFFFFu;
            uint8_t size = (uint8_t)(instr->src1.size ? instr->src1.size : 4);
            if (size == 2) {
                ctx->regs.x86.esp -= 2;
                r = hb_memory_write_u16(ctx->memory, ctx->regs.x86.esp, selector);
            } else {
                ctx->regs.x86.esp -= 4;
                r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t)selector);
            }
            return r;
        }

        case HB_IR_POP_SEG: {
            /* i386-only: POP ES/SS/DS (32-bit user mode). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t seg = (uint16_t)instr->src2.imm;
            uint8_t size = (uint8_t)(instr->dst.size ? instr->dst.size : 4);
            uint32_t value = 0;
            if (size == 2) {
                uint16_t v16 = 0;
                r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp, &v16);
                if (r != HB_OK) return r;
                value = v16;
                ctx->regs.x86.esp += 2;
            } else {
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &value);
                if (r != HB_OK) return r;
                ctx->regs.x86.esp += 4;
            }
            ctx->regs.x86.seg[seg] = value & 0xFFFFu;
            return HB_OK;
        }

        case HB_IR_RETF: {
            /* Far return. src1.imm = stack-adjust after pop (CA form has it, CB form is 0). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            int adjust = (int)instr->src1.imm;
            uint32_t eip = 0, eflags = 0;
            uint16_t cs = 0;
            r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &eip);
            if (r != HB_OK) return r;
            r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp + 4, &cs);
            if (r != HB_OK) return r;
            r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 6, &eflags);
            if (r != HB_OK) return r;
            ctx->regs.x86.esp += 12;
            ctx->regs.x86.eip = eip;
            ctx->regs.x86.seg[1] = cs;
            r = write_flags_image(ctx, HB_SIZE_32, eflags);
            if (r != HB_OK) return r;
            if (adjust) ctx->regs.x86.esp += (uint32_t)adjust;
            return HB_OK;
        }

        case HB_IR_IRET: {
            /* Interrupt return. src1.size = 16 (IRET) or 32 (IRETD). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t size = (uint8_t)(instr->src1.size ? instr->src1.size : 4);
            if (size == 2) {
                uint16_t ip = 0, cs = 0, fl = 0;
                r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp, &ip);
                if (r != HB_OK) return r;
                r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp + 2, &cs);
                if (r != HB_OK) return r;
                r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp + 4, &fl);
                if (r != HB_OK) return r;
                ctx->regs.x86.esp += 6;
                ctx->regs.x86.eip = ip;
                ctx->regs.x86.seg[1] = cs;
                r = write_flags_image(ctx, HB_SIZE_16, fl);
            } else {
                uint32_t eip = 0, eflags = 0;
                uint16_t cs = 0;
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &eip);
                if (r != HB_OK) return r;
                r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp + 4, &cs);
                if (r != HB_OK) return r;
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp + 6, &eflags);
                if (r != HB_OK) return r;
                ctx->regs.x86.esp += 12;
                ctx->regs.x86.eip = eip;
                ctx->regs.x86.seg[1] = cs;
                r = write_flags_image(ctx, HB_SIZE_32, eflags);
            }
            return r;
        }

        case HB_IR_INT3:
            /* Breakpoint. HyperBridge doesn't trap; treated as NOP. */
            return HB_OK;

        case HB_IR_INT1:
            return HB_OK;

        case HB_IR_INT: {
            /* INT n. HyperBridge doesn't dispatch to a real IDT; fault so the
             * user knows the guest used INT n. */
            (void)instr;
            return HB_ERR_UNSUPPORTED_OPCODE;
        }

        case HB_IR_INTO:
            /* Trap if OF=1. HyperBridge has no #OF trap, so we just NOP. */
            (void)instr;
            return HB_OK;

        case HB_IR_XLAT: {
            /* XLATB: AL = [EBX+AL] (or [BX+AL] with 0x67). */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint8_t al = (uint8_t)ctx->regs.x86.eax;
            uint32_t base = (uint32_t)ctx->regs.x86.ebx;
            uint32_t addr = base + al;
            uint8_t v = 0;
            r = hb_memory_read_u8(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            ctx->regs.x86.eax = (ctx->regs.x86.eax & 0xFFFFFF00u) | v;
            return HB_OK;
        }

        case HB_IR_ENTER: {
            /* ENTER frame_size, nesting — push EBP, allocate locals. */
            if (ctx->mode != HB_MODE_32BIT) return HB_ERR_UNSUPPORTED_OPCODE;
            uint16_t frame_size = (uint16_t)instr->src1.imm;
            uint8_t nesting = (uint8_t)instr->src2.imm;
            uint32_t ebp = ctx->regs.x86.ebp;
            /* Push EBP. */
            ctx->regs.x86.esp -= 4;
            r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, ebp);
            if (r != HB_OK) return r;
            uint32_t frame_ebp = ctx->regs.x86.esp;
            if (nesting > 0) {
                for (uint8_t i = 1; i < nesting; i++) {
                    ebp -= 4;
                    uint32_t tmp = 0;
                    r = hb_memory_read_u32(ctx->memory, ebp, &tmp);
                    if (r != HB_OK) return r;
                    ctx->regs.x86.esp -= 4;
                    r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, tmp);
                    if (r != HB_OK) return r;
                }
                /* push frame_ebp. */
                ctx->regs.x86.esp -= 4;
                r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, frame_ebp);
                if (r != HB_OK) return r;
            } else {
                ctx->regs.x86.esp -= 4;
                r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, frame_ebp);
                if (r != HB_OK) return r;
            }
            ctx->regs.x86.ebp = frame_ebp;
            ctx->regs.x86.esp -= frame_size;
            return HB_OK;
        }

        case HB_IR_PUSH: {
            uint64_t val = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            if (ctx->mode == HB_MODE_32BIT) {
                ctx->regs.x86.esp -= 4;
                r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t)val);
            } else {
                ctx->regs.x64.rsp -= 8;
                r = hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp, val);
            }
            if (r != HB_OK) return r;
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, "push", rsp_before, val, 0);
            return HB_OK;
        }

        case HB_IR_POP: {
            uint64_t val = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (ctx->mode == HB_MODE_32BIT) {
                if (instr->dst.size == HB_SIZE_16) {
                    uint16_t v16 = 0;
                    r = hb_memory_read_u16(ctx->memory, ctx->regs.x86.esp, &v16);
                    if (r != HB_OK) return r;
                    val = v16;
                    ctx->regs.x86.esp += 2;
                } else {
                    r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t*)&val);
                    if (r != HB_OK) return r;
                    ctx->regs.x86.esp += 4;
                }
            } else {
                r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &val);
                if (r != HB_OK) return r;
                ctx->regs.x64.rsp += 8;
            }
            if (instr->dst.type == HB_OP_REG) {
                write_reg_sized(ctx, instr->dst.reg, val, instr->dst.size);
            } else if (instr->dst.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->dst);
                r = mem_write(ctx, addr, val, instr->dst.size);
                if (r != HB_OK) return r;
            } else {
                return HB_ERR_INTERNAL;
            }
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, "pop", rsp_before, val, val);
            return HB_OK;
        }

        case HB_IR_CALL: {
            uint64_t ret_addr = instr->guest_addr + instr->guest_len;
            uint64_t target = instr->target;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (instr->src1.type != HB_OP_NONE) {
                r = read_operand_value(ctx, &instr->src1, &target);
                if (r != HB_OK) return r;
            }
            if (!target) {
                fprintf(stderr, "macrunner-hb-null-branch: op=CALL addr=0x%llx len=%u src_type=%d base=%d index=%d scale=%u disp=0x%llx size=%d\n",
                        (unsigned long long)instr->guest_addr, instr->guest_len, instr->src1.type,
                        instr->src1.type == HB_OP_MEM ? (int)instr->src1.mem.base : -1,
                        instr->src1.type == HB_OP_MEM ? (int)instr->src1.mem.index : -1,
                        instr->src1.type == HB_OP_MEM ? instr->src1.mem.scale : 0,
                        instr->src1.type == HB_OP_MEM ? (unsigned long long)instr->src1.mem.disp : 0,
                        instr->src1.size);
                return HB_ERR_EXEC_FAULT;
            }
            if (ctx->mode == HB_MODE_32BIT) {
                uint32_t new_esp = ctx->regs.x86.esp - 4;
                r = hb_memory_write_u32(ctx->memory, new_esp, (uint32_t)ret_addr);
                if (r == HB_OK) ctx->regs.x86.esp = new_esp;
            } else {
                uint64_t new_rsp = ctx->regs.x64.rsp - 8;
                r = hb_memory_write_u64(ctx->memory, new_rsp, ret_addr);
                if (r == HB_OK) ctx->regs.x64.rsp = new_rsp;
            }
            if (r != HB_OK) {
                static int traced;
                if (traced++ < 8)
                    fprintf(stderr, "macrunner-hb-interpcall-fail: rsp=0x%llx r=%d target=0x%llx\n",
                            (unsigned long long)rsp_before, (int)r, (unsigned long long)target);
                return r;
            }
            ctx->pc = target;
            sync_arch_pc(ctx);
            trace_branch_event(ctx, instr, "call", rsp_before, ret_addr, target);
            return HB_OK;
        }

        case HB_IR_RET: {
            uint64_t ret_addr = 0;
            uint64_t ret_imm = instr->src1.type == HB_OP_IMM ? (uint64_t)instr->src1.imm : 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (ctx->mode == HB_MODE_32BIT) {
                uint32_t ret32 = 0;
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &ret32);
                if (r != HB_OK) return r;
                ret_addr = ret32;
                ctx->regs.x86.esp += 4 + (uint32_t)ret_imm;
            } else {
                r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &ret_addr);
                if (r != HB_OK) return r;
                ctx->regs.x64.rsp += 8 + ret_imm;
            }
            ctx->pc = ret_addr;
            sync_arch_pc(ctx);
            trace_branch_event(ctx, instr, "ret", rsp_before, ret_addr, ret_addr);
            return HB_OK;
        }

        case HB_IR_JMP: {
            uint64_t target = instr->target;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (instr->src1.type != HB_OP_NONE) {
                r = read_operand_value(ctx, &instr->src1, &target);
                if (r != HB_OK) return r;
            }
            if (!target) {
                fprintf(stderr, "macrunner-hb-null-branch: op=JMP addr=0x%llx len=%u src_type=%d base=%d index=%d scale=%u disp=0x%llx size=%d\n",
                        (unsigned long long)instr->guest_addr, instr->guest_len, instr->src1.type,
                        instr->src1.type == HB_OP_MEM ? (int)instr->src1.mem.base : -1,
                        instr->src1.type == HB_OP_MEM ? (int)instr->src1.mem.index : -1,
                        instr->src1.type == HB_OP_MEM ? instr->src1.mem.scale : 0,
                        instr->src1.type == HB_OP_MEM ? (unsigned long long)instr->src1.mem.disp : 0,
                        instr->src1.size);
                return HB_ERR_EXEC_FAULT;
            }
            ctx->pc = target;
            sync_arch_pc(ctx);
            trace_branch_event(ctx, instr, "jmp", rsp_before, 0, target);
            return HB_OK;
        }

        case HB_IR_Jcc: {
            bool taken = false;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            r = hb_flags_eval_cond(ctx, instr->cc, &taken);
            if (r != HB_OK) return r;
            if (taken) {
                ctx->pc = instr->target;
            } else {
                ctx->pc = instr->guest_addr + instr->guest_len;
            }
            sync_arch_pc(ctx);
            trace_branch_event(ctx, instr, taken ? "jcc-taken" : "jcc-fallthrough",
                               rsp_before, instr->cc, ctx->pc);
            return HB_OK;
        }

        case HB_IR_LOOP:
        case HB_IR_JRCXZ: {
            uint64_t count = 0;
            uint64_t next = 0;
            bool taken = false;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            r = read_operand_value(ctx, &instr->dst, &count);
            if (r != HB_OK) return r;
            count = trunc_to_size(count, size);
            if (instr->op == HB_IR_JRCXZ) {
                taken = count == 0;
            } else {
                int kind = (int)instr->src1.imm; /* E0 LOOPNE, E1 LOOPE, E2 LOOP. */
                next = trunc_to_size(count - 1, size);
                r = write_operand_value(ctx, &instr->dst, next);
                if (r != HB_OK) return r;
                taken = next != 0 &&
                        (kind == 2 || (kind == 1 ? ctx->flags.zf : !ctx->flags.zf));
            }
            ctx->pc = taken ? instr->target : instr->guest_addr + instr->guest_len;
            sync_arch_pc(ctx);
            trace_branch_event(ctx, instr,
                               taken ? (instr->op == HB_IR_JRCXZ ? "jrcxz-taken" : "loop-taken")
                                     : (instr->op == HB_IR_JRCXZ ? "jrcxz-fallthrough" : "loop-fallthrough"),
                               rsp_before, count, ctx->pc);
            return HB_OK;
        }

        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR:
        case HB_IR_ROL:
        case HB_IR_ROR: {
            return hb_flags_exec_binop_operand(ctx, instr->op, &instr->dst,
                                               &instr->src1, &instr->src2, NULL);
        }

        case HB_IR_RCL:
        case HB_IR_RCR: {
            uint64_t value = 0, raw_count = 0;
            r = read_operand_value(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &raw_count);
            if (r != HB_OK) return r;
            r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_CF);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = HB_SIZE_64;
            unsigned width = bit_width_for_size(size);
            unsigned count = (unsigned)(raw_count & (width == 64 ? 0x3fU : 0x1fU));
            unsigned ring = width + 1;
            if (width <= 16) count %= ring;
            if (count == 0) return HB_OK;

            value = trunc_to_size(value, size);
            uint64_t value_mask = mask_for_size(size);
            unsigned __int128 ring_mask = (((unsigned __int128)1) << ring) - 1;
            unsigned __int128 combined = (((unsigned __int128)(ctx->flags.cf ? 1 : 0)) << width) | value;
            if (instr->op == HB_IR_RCL) {
                combined = ((combined << count) | (combined >> (ring - count))) & ring_mask;
            } else {
                combined = ((combined >> count) | (combined << (ring - count))) & ring_mask;
            }
            uint64_t result = (uint64_t)combined & value_mask;
            bool new_cf = ((combined >> width) & 1U) != 0;
            r = write_operand_value(ctx, &instr->dst, result);
            if (r != HB_OK) return r;
            hb_lazy_flags_clear(ctx);
            ctx->flags.cf = new_cf;
            if (count == 1) {
                bool msb = ((result >> (width - 1)) & 1U) != 0;
                if (instr->op == HB_IR_RCL) {
                    ctx->flags.of = msb != ctx->flags.cf;
                } else {
                    bool next = width > 1 ? (((result >> (width - 2)) & 1U) != 0) : false;
                    ctx->flags.of = msb != next;
                }
            }
            return HB_OK;
        }

        case HB_IR_SHLD:
        case HB_IR_SHRD: {
            return hb_flags_exec_double_shift_operand(ctx, instr->op, &instr->dst,
                                                      &instr->src1, &instr->src2, NULL);
        }

        case HB_IR_NOT: {
            uint64_t a = 0;
            r = read_operand_value(ctx, &instr->src1, &a);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = HB_SIZE_64;
            uint64_t result = ~a;
            result = trunc_to_size(result, size);
            return write_operand_value(ctx, &instr->dst, result);
        }

        case HB_IR_NEG: {
            uint64_t a = 0;
            r = read_operand_value(ctx, &instr->src1, &a);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = HB_SIZE_64;
            a = trunc_to_size(a, size);
            uint64_t result = trunc_to_size(0 - a, size);
            r = write_operand_value(ctx, &instr->dst, result);
            if (r != HB_OK) return r;
            hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_SUB, size, 0, a, result, 0);
            return HB_OK;
        }

        case HB_IR_LAHF: {
            r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_SF | HB_FLAG_BIT_ZF |
                                               HB_FLAG_BIT_AF | HB_FLAG_BIT_PF |
                                               HB_FLAG_BIT_CF);
            if (r != HB_OK) return r;
            uint8_t ah = 0x02;
            if (ctx->flags.sf) ah |= 0x80;
            if (ctx->flags.zf) ah |= 0x40;
            if (ctx->flags.af) ah |= 0x10;
            if (ctx->flags.pf) ah |= 0x04;
            if (ctx->flags.cf) ah |= 0x01;
            if (ctx->mode == HB_MODE_32BIT) {
                ctx->regs.x86.eax = (ctx->regs.x86.eax & ~0x0000ff00U) | ((uint32_t)ah << 8);
            } else {
                ctx->regs.x64.rax = (ctx->regs.x64.rax & ~0x000000000000ff00ULL) | ((uint64_t)ah << 8);
            }
            return HB_OK;
        }

        case HB_IR_SAHF: {
            uint8_t ah = ctx->mode == HB_MODE_32BIT ?
                (uint8_t)((ctx->regs.x86.eax >> 8) & 0xffU) :
                (uint8_t)((ctx->regs.x64.rax >> 8) & 0xffU);
            hb_lazy_flags_clear(ctx);
            ctx->flags.sf = (ah & 0x80) != 0;
            ctx->flags.zf = (ah & 0x40) != 0;
            ctx->flags.af = (ah & 0x10) != 0;
            ctx->flags.pf = (ah & 0x04) != 0;
            ctx->flags.cf = (ah & 0x01) != 0;
            return HB_OK;
        }

        case HB_IR_CPUID: {
            uint32_t leaf = (uint32_t)read_reg(ctx, HB_REG_RAX);
            uint32_t subleaf = (uint32_t)read_reg(ctx, HB_REG_RCX);
            uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            (void)subleaf;

            switch (leaf) {
                case 0:
                    eax = 7;
                    ebx = 0x756e6547U; /* "Genu" */
                    edx = 0x49656e69U; /* "ineI" */
                    ecx = 0x6c65746eU; /* "ntel" */
                    break;
                case 1:
                    eax = 0x000306a9U;
                    /* Advertise only families with interpreter semantics and tests.
                     * Keep SHA/GFNI/AVX-512 out of leaf 7 so guests take their
                     * fallback paths until broad semantics are modeled.
                     */
                    ecx = (1U << 0)  |  /* SSE3 */
                          (1U << 1)  |  /* PCLMULQDQ */
                          (1U << 9)  |  /* SSSE3 */
                          (1U << 12) |  /* FMA */
                          (1U << 19) |  /* SSE4.1 */
                          (1U << 20) |  /* SSE4.2 */
                          (1U << 23) |  /* POPCNT */
                          (1U << 25) |  /* AES */
                          (1U << 26) |  /* XSAVE */
                          (1U << 27) |  /* OSXSAVE */
                          (1U << 28) |  /* AVX */
                          (1U << 29);   /* F16C */
                    edx = (1U << 8)  |  /* CX8 */
                          (1U << 15) |  /* CMOV */
                          (1U << 23) |  /* MMX */
                          (1U << 24) |  /* FXSR */
                          (1U << 25) |  /* SSE */
                          (1U << 26);   /* SSE2 */
                    break;
                default:
                    break;
            }

            write_reg_sized(ctx, HB_REG_RAX, eax, HB_SIZE_32);
            write_reg_sized(ctx, HB_REG_RBX, ebx, HB_SIZE_32);
            write_reg_sized(ctx, HB_REG_RCX, ecx, HB_SIZE_32);
            write_reg_sized(ctx, HB_REG_RDX, edx, HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_XGETBV: {
            uint32_t ecx = (uint32_t)read_reg(ctx, HB_REG_RCX);
            uint64_t xcr0 = 0;

            if (ecx == 0)
                xcr0 = 0x7; /* x87 + SSE + YMM state for advertised AVX/FMA/F16C paths. */

            write_reg_sized(ctx, HB_REG_RAX, (uint32_t)xcr0, HB_SIZE_32);
            write_reg_sized(ctx, HB_REG_RDX, (uint32_t)(xcr0 >> 32), HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_RDRAND:
        case HB_IR_RDSEED: {
            /* Patch H: RDRAND/RDSEED r16/32/64. Destination is a register operand;
             * fill it with a fresh random value and report success (CF=1), clearing
             * OF/SF/ZF/AF/PF per the Intel SDM. arc4random_buf is a good-quality CSPRNG
             * on the host, adequate for both the "random" and "seed" flavours. */
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            uint64_t rnd = 0;
            arc4random_buf(&rnd, sizeof(rnd));
            write_reg_sized_offset(ctx, instr->dst.reg, rnd, size, instr->dst.reg_offset);
            hb_lazy_flags_clear(ctx);
            ctx->flags.cf = true;
            ctx->flags.of = false;
            ctx->flags.sf = false;
            ctx->flags.zf = false;
            ctx->flags.af = false;
            ctx->flags.pf = false;
            return HB_OK;
        }

        case HB_IR_SETcc: {
            bool value = false;
            r = hb_flags_eval_cond(ctx, instr->cc, &value);
            if (r != HB_OK) return r;
            if (instr->dst.type == HB_OP_REG) {
                write_reg_sized_offset(ctx, instr->dst.reg, value ? 1 : 0, HB_SIZE_8,
                                       instr->dst.reg_offset);
            } else if (instr->dst.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->dst);
                r = mem_write(ctx, addr, value ? 1 : 0, HB_SIZE_8);
                if (r != HB_OK) return r;
            } else {
                return HB_ERR_INTERNAL;
            }
            return HB_OK;
        }

        case HB_IR_CMOVcc: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            bool value = false;
            r = hb_flags_eval_cond(ctx, instr->cc, &value);
            if (r != HB_OK) return r;
            if (value) {
                uint64_t src = 0;
                if (instr->src1.type == HB_OP_REG) src = read_reg(ctx, instr->src1.reg);
                else if (instr->src1.type == HB_OP_IMM) src = (uint64_t)instr->src1.imm;
                else if (instr->src1.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->src1);
                    r = mem_read(ctx, addr, &src, instr->src1.size);
                    if (r != HB_OK) return r;
                } else {
                    return HB_ERR_INTERNAL;
                }
                write_reg(ctx, instr->dst.reg, src);
            }
            return HB_OK;
        }

        case HB_IR_ZERO_EXTEND: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            val = trunc_to_size(val, instr->src1.size);
            write_reg_sized(ctx, instr->dst.reg, val, instr->dst.size);
            return HB_OK;
        }

        case HB_IR_SIGN_EXTEND: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            val = sign_extend_from_size(trunc_to_size(val, instr->src1.size), instr->src1.size);
            write_reg_sized(ctx, instr->dst.reg, val, instr->dst.size);
            return HB_OK;
        }

        case HB_IR_CWD: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            if (size == HB_SIZE_16) {
                uint16_t ax = (uint16_t)read_reg(ctx, HB_REG_RAX);
                write_reg_sized(ctx, HB_REG_RDX, (ax & 0x8000) ? 0xffffU : 0, HB_SIZE_16);
            } else if (size == HB_SIZE_32) {
                uint32_t eax = (uint32_t)read_reg(ctx, HB_REG_RAX);
                write_reg_sized(ctx, HB_REG_RDX, (eax & 0x80000000U) ? 0xffffffffU : 0, HB_SIZE_32);
            } else if (size == HB_SIZE_64) {
                uint64_t rax = read_reg(ctx, HB_REG_RAX);
                write_reg_sized(ctx, HB_REG_RDX, (rax & 0x8000000000000000ULL) ? UINT64_MAX : 0, HB_SIZE_64);
            } else return HB_ERR_UNSUPPORTED_OPCODE;
            return HB_OK;
        }

        case HB_IR_MOVS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            bool mode32 = ctx->mode == HB_MODE_32BIT;
            uint64_t count = repeated ? (mode32 ? ctx->regs.x86.ecx : ctx->regs.x64.rcx) : 1;
            uint64_t rsi = mode32 ? ctx->regs.x86.esi : ctx->regs.x64.rsi;
            uint64_t rdi = mode32 ? ctx->regs.x86.edi : ctx->regs.x64.rdi;
            int64_t step = (int64_t)size;
            if ((mode32 ? ctx->regs.x86.eflags : ctx->regs.x64.rflags) & (1ULL << 10))
                step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                uint64_t value = 0;
                r = mem_read(ctx, rsi, &value, size);
                if (r != HB_OK) return r;
                r = mem_write(ctx, rdi, value, size);
                if (r != HB_OK) return r;
                rsi = (uint64_t)((int64_t)rsi + step);
                rdi = (uint64_t)((int64_t)rdi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            if (mode32) {
                ctx->regs.x86.esi = (uint32_t)rsi;
                ctx->regs.x86.edi = (uint32_t)rdi;
                if (repeated) ctx->regs.x86.ecx = (uint32_t)count;
            } else {
                ctx->regs.x64.rsi = rsi;
                ctx->regs.x64.rdi = rdi;
                if (repeated) ctx->regs.x64.rcx = count;
            }
            return HB_OK;
        }

        case HB_IR_CMPS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            bool mode32 = ctx->mode == HB_MODE_32BIT;
            uint64_t count = repeated ? (mode32 ? ctx->regs.x86.ecx : ctx->regs.x64.rcx) : 1;
            uint64_t rsi = mode32 ? ctx->regs.x86.esi : ctx->regs.x64.rsi;
            uint64_t rdi = mode32 ? ctx->regs.x86.edi : ctx->regs.x64.rdi;
            int64_t step = (int64_t)size;
            if ((mode32 ? ctx->regs.x86.eflags : ctx->regs.x64.rflags) & (1ULL << 10))
                step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                uint64_t left = 0, right = 0;
                r = mem_read(ctx, rsi, &left, size);
                if (r != HB_OK) return r;
                r = mem_read(ctx, rdi, &right, size);
                if (r != HB_OK) return r;
                left = trunc_to_size(left, size);
                right = trunc_to_size(right, size);
                hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, size, left, right, left - right, 0);
                r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF |
                                                   HB_FLAG_BIT_AF | HB_FLAG_BIT_PF |
                                                   HB_FLAG_BIT_CF | HB_FLAG_BIT_OF);
                if (r != HB_OK) return r;

                /* Per Intel SDM, the pointer update and count decrement happen
                 * BEFORE the termination check. The first terminating comparison
                 * (mismatch for REPE, match for REPNE) still updates pointers and
                 * decrements count, so the final state has pointers one past the
                 * element that triggered the stop. */
                rsi = (uint64_t)((int64_t)rsi + step);
                rdi = (uint64_t)((int64_t)rdi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (rep == 0xf2 && ctx->flags.zf) break;  /* REPNE/REPNZ stops on match. */
                if (rep == 0xf3 && !ctx->flags.zf) break; /* REPE/REPZ stops on mismatch. */
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            if (mode32) {
                ctx->regs.x86.esi = (uint32_t)rsi;
                ctx->regs.x86.edi = (uint32_t)rdi;
                if (repeated) ctx->regs.x86.ecx = (uint32_t)count;
            } else {
                ctx->regs.x64.rsi = rsi;
                ctx->regs.x64.rdi = rdi;
                if (repeated) ctx->regs.x64.rcx = count;
            }
            return HB_OK;
        }

        case HB_IR_LODS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            bool mode32 = ctx->mode == HB_MODE_32BIT;
            uint64_t count = repeated ? (mode32 ? ctx->regs.x86.ecx : ctx->regs.x64.rcx) : 1;
            uint64_t rsi = mode32 ? ctx->regs.x86.esi : ctx->regs.x64.rsi;
            int64_t step = (int64_t)size;
            if ((mode32 ? ctx->regs.x86.eflags : ctx->regs.x64.rflags) & (1ULL << 10))
                step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                uint64_t value = 0;
                r = mem_read(ctx, rsi, &value, size);
                if (r != HB_OK) return r;
                /* LODSB/LODSW write AL/AX only; LODSD zero-extends through EAX in
                 * x86-64, and LODSQ writes the full RAX. */
                if (mode32) {
                    write_reg_sized_offset(ctx, HB_REG_X86_EAX, value, size, 0);
                } else {
                    write_reg_sized_offset(ctx, HB_REG_RAX, value, size, 0);
                }
                rsi = (uint64_t)((int64_t)rsi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            if (mode32) {
                ctx->regs.x86.esi = (uint32_t)rsi;
                if (repeated) ctx->regs.x86.ecx = (uint32_t)count;
            } else {
                ctx->regs.x64.rsi = rsi;
                if (repeated) ctx->regs.x64.rcx = count;
            }
            return HB_OK;
        }

        case HB_IR_SCAS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            bool mode32 = ctx->mode == HB_MODE_32BIT;
            uint64_t count = repeated ? (mode32 ? ctx->regs.x86.ecx : ctx->regs.x64.rcx) : 1;
            uint64_t rdi = mode32 ? ctx->regs.x86.edi : ctx->regs.x64.rdi;
            uint64_t acc = read_reg_sized(ctx, HB_REG_RAX, size, 0);
            int64_t step = (int64_t)size;
            if ((mode32 ? ctx->regs.x86.eflags : ctx->regs.x64.rflags) & (1ULL << 10))
                step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                uint64_t mem = 0;
                r = mem_read(ctx, rdi, &mem, size);
                if (r != HB_OK) return r;
                mem = trunc_to_size(mem, size);
                hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_CMP, size, acc, mem, acc - mem, 0);
                r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF |
                                                   HB_FLAG_BIT_AF | HB_FLAG_BIT_PF |
                                                   HB_FLAG_BIT_CF | HB_FLAG_BIT_OF);
                if (r != HB_OK) return r;

                rdi = (uint64_t)((int64_t)rdi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (rep == 0xf2 && ctx->flags.zf) break;  /* REPNE/REPNZ stops on match. */
                if (rep == 0xf3 && !ctx->flags.zf) break; /* REPE/REPZ stops on mismatch. */
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            if (mode32) {
                ctx->regs.x86.edi = (uint32_t)rdi;
                if (repeated) ctx->regs.x86.ecx = (uint32_t)count;
            } else {
                ctx->regs.x64.rdi = rdi;
                if (repeated) ctx->regs.x64.rcx = count;
            }
            return HB_OK;
        }

        case HB_IR_STOS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            bool mode32 = ctx->mode == HB_MODE_32BIT;
            uint64_t count = repeated ? (mode32 ? ctx->regs.x86.ecx : ctx->regs.x64.rcx) : 1;
            uint64_t rdi = mode32 ? ctx->regs.x86.edi : ctx->regs.x64.rdi;
            uint64_t acc = read_reg_sized(ctx, HB_REG_RAX, size, 0);
            int64_t step = (int64_t)size;
            if ((mode32 ? ctx->regs.x86.eflags : ctx->regs.x64.rflags) & (1ULL << 10))
                step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                r = mem_write(ctx, rdi, acc, size);
                if (r != HB_OK) return r;
                rdi = (uint64_t)((int64_t)rdi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            if (mode32) {
                ctx->regs.x86.edi = (uint32_t)rdi;
                if (repeated) ctx->regs.x86.ecx = (uint32_t)count;
            } else {
                ctx->regs.x64.rdi = rdi;
                if (repeated) ctx->regs.x64.rcx = count;
            }
            return HB_OK;
        }

        case HB_IR_BSF: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            uint64_t result = 0;
            val = trunc_to_size(val, size);
            hb_lazy_flags_clear(ctx);
            ctx->flags.zf = (val == 0);
            if (val) {
                while (((val >> result) & 1ULL) == 0) result++;
                write_reg_sized(ctx, instr->dst.reg, result, size);
            }
            return HB_OK;
        }

        case HB_IR_TZCNT: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            val = trunc_to_size(val, size);
            uint64_t width = (size == HB_SIZE_32) ? 32 : (size == HB_SIZE_16) ? 16 : (size == HB_SIZE_8) ? 8 : 64;
            uint64_t result = width;
            if (val) {
                result = 0;
                while (((val >> result) & 1ULL) == 0) result++;
            }
            write_reg_sized(ctx, instr->dst.reg, result, size);
            hb_lazy_flags_clear(ctx);
            ctx->flags.zf = (result == 0);
            ctx->flags.cf = (val == 0);
            return HB_OK;
        }

        case HB_IR_LZCNT: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            val = trunc_to_size(val, size);
            uint64_t width = (size == HB_SIZE_32) ? 32 : (size == HB_SIZE_16) ? 16 : (size == HB_SIZE_8) ? 8 : 64;
            uint64_t result = width;
            if (val) {
                result = 0;
                for (int64_t bit = (int64_t)width - 1; bit >= 0 && ((val >> bit) & 1ULL) == 0; bit--) result++;
            }
            write_reg_sized(ctx, instr->dst.reg, result, size);
            hb_lazy_flags_clear(ctx);
            ctx->flags.zf = (result == 0);
            ctx->flags.cf = (val == 0);
            return HB_OK;
        }

        case HB_IR_BSR: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            uint64_t width = (size == HB_SIZE_32) ? 32 : (size == HB_SIZE_16) ? 16 : (size == HB_SIZE_8) ? 8 : 64;
            val = trunc_to_size(val, size);
            hb_lazy_flags_clear(ctx);
            ctx->flags.zf = (val == 0);
            if (val) {
                uint64_t result = width - 1;
                while (((val >> result) & 1ULL) == 0) result--;
                write_reg_sized(ctx, instr->dst.reg, result, size);
            }
            return HB_OK;
        }

        case HB_IR_POPCNT: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t val = 0;
            r = read_operand_value(ctx, &instr->src1, &val);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
            if (!size) size = HB_SIZE_64;
            val = trunc_to_size(val, size);
            uint64_t result = popcount_u64(val);
            write_reg_sized(ctx, instr->dst.reg, result, size);
            hb_lazy_flags_clear(ctx);
            ctx->flags.cf = false;
            ctx->flags.pf = false;
            ctx->flags.af = false;
            ctx->flags.zf = (val == 0);
            ctx->flags.sf = false;
            ctx->flags.of = false;
            return HB_OK;
        }

        case HB_IR_CRC32: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            uint64_t src = 0;
            r = read_operand_value(ctx, &instr->src1, &src);
            if (r != HB_OK) return r;
            hb_size_t src_size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            hb_size_t dst_size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            uint32_t crc = (uint32_t)read_reg_sized(ctx, instr->dst.reg, dst_size, 0);
            src = trunc_to_size(src, src_size);
            unsigned bytes = (src_size == HB_SIZE_8) ? 1u :
                             (src_size == HB_SIZE_16) ? 2u :
                             (src_size == HB_SIZE_64) ? 8u : 4u;
            for (unsigned byte = 0; byte < bytes; byte++) {
                crc ^= (uint8_t)(src >> (byte * 8u));
                for (unsigned bit = 0; bit < 8; bit++) {
                    crc = (crc >> 1) ^ ((crc & 1u) ? 0x82f63b78u : 0u);
                }
            }
            write_reg_sized(ctx, instr->dst.reg, (uint64_t)crc, dst_size);
            return HB_OK;
        }

        case HB_IR_ANDN:
        case HB_IR_BEXTR:
        case HB_IR_BLSI:
        case HB_IR_BLSMSK:
        case HB_IR_BLSR:
        case HB_IR_BZHI:
        case HB_IR_PDEP:
        case HB_IR_PEXT:
        case HB_IR_RORX:
        case HB_IR_SARX:
        case HB_IR_SHLX:
        case HB_IR_SHRX: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            unsigned width = (size == HB_SIZE_64) ? 64u : 32u;
            uint64_t src1 = 0, src2 = 0;
            r = read_operand_value(ctx, &instr->src1, &src1);
            if (r != HB_OK) return r;
            if (instr->src2.type != HB_OP_NONE) {
                r = read_operand_value(ctx, &instr->src2, &src2);
                if (r != HB_OK) return r;
            }
            src1 = trunc_to_size(src1, size);
            src2 = trunc_to_size(src2, size);
            uint64_t result = 0;
            bool set_logic_flags = false;
            bool cf = false, of = false;
            if (instr->op == HB_IR_ANDN) {
                result = (~src1) & src2;
                set_logic_flags = true;
            } else if (instr->op == HB_IR_BEXTR) {
                unsigned start = (unsigned)(src2 & 0xffu);
                unsigned len = (unsigned)((src2 >> 8) & 0xffu);
                if (start < width && len != 0) {
                    unsigned avail = width - start;
                    if (len > avail) len = avail;
                    result = (src1 >> start) & (len >= 64 ? UINT64_MAX : ((1ULL << len) - 1ULL));
                }
                set_logic_flags = true;
            } else if (instr->op == HB_IR_BLSI) {
                result = src1 & (0ULL - src1);
                cf = src1 != 0;
                set_logic_flags = true;
            } else if (instr->op == HB_IR_BLSMSK) {
                result = src1 ^ (src1 - 1ULL);
                cf = src1 == 0;
                set_logic_flags = true;
            } else if (instr->op == HB_IR_BLSR) {
                result = src1 & (src1 - 1ULL);
                cf = src1 == 0;
                set_logic_flags = true;
            } else if (instr->op == HB_IR_BZHI) {
                unsigned index = (unsigned)(src2 & 0xffu);
                if (index >= width) {
                    result = src1;
                    cf = true;
                } else {
                    result = index == 0 ? 0 : (src1 & ((1ULL << index) - 1ULL));
                }
                set_logic_flags = true;
            } else if (instr->op == HB_IR_PEXT || instr->op == HB_IR_PDEP) {
                uint64_t src = src1, mask = src2, bit = 1;
                result = 0;
                while (mask) {
                    uint64_t lowest = mask & (0ULL - mask);
                    if (instr->op == HB_IR_PEXT) {
                        if (src & lowest) result |= bit;
                    } else {
                        if (src & bit) result |= lowest;
                    }
                    mask &= mask - 1ULL;
                    if (mask) bit <<= 1;
                }
            } else if (instr->op == HB_IR_RORX) {
                unsigned count = (unsigned)(instr->src2.imm & 0xffu) % width;
                result = count == 0 ? src1 : ((src1 >> count) | (src1 << (width - count)));
            } else {
                unsigned count = (unsigned)(src2 & 0xffu);
                if (count >= width) {
                    if (instr->op == HB_IR_SARX) result = (src1 >> (width - 1)) ? mask_for_size(size) : 0;
                    else result = 0;
                } else if (instr->op == HB_IR_SARX) {
                    if (width == 64) result = (uint64_t)((int64_t)src1 >> count);
                    else result = (uint32_t)((int32_t)src1 >> count);
                } else if (instr->op == HB_IR_SHLX) {
                    result = src1 << count;
                } else {
                    result = src1 >> count;
                }
            }
            result = trunc_to_size(result, size);
            write_reg_sized(ctx, instr->dst.reg, result, size);
            if (set_logic_flags) {
                hb_lazy_flags_clear(ctx);
                ctx->flags.cf = cf;
                ctx->flags.of = of;
                ctx->flags.zf = (result == 0);
                ctx->flags.sf = ((result >> (width - 1)) & 1u) != 0;
                ctx->flags.pf = parity_even_u8((uint8_t)result);
                ctx->flags.af = false;
            }
            return HB_OK;
        }

        case HB_IR_MULX: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG) return HB_ERR_INTERNAL;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            uint64_t src = 0;
            r = read_operand_value(ctx, &instr->src2, &src);
            if (r != HB_OK) return r;
            src = trunc_to_size(src, size);
            uint64_t implicit = read_reg_sized(ctx, HB_REG_RDX, size, 0);
            if (size == HB_SIZE_64) {
                __uint128_t product = (__uint128_t)implicit * (__uint128_t)src;
                write_reg_sized(ctx, instr->dst.reg, (uint64_t)product, HB_SIZE_64);
                write_reg_sized(ctx, instr->src1.reg, (uint64_t)(product >> 64), HB_SIZE_64);
            } else {
                uint64_t product = (uint64_t)(uint32_t)implicit * (uint64_t)(uint32_t)src;
                write_reg_sized(ctx, instr->dst.reg, (uint32_t)product, HB_SIZE_32);
                write_reg_sized(ctx, instr->src1.reg, (uint32_t)(product >> 32), HB_SIZE_32);
            }
            return HB_OK;
        }

        case HB_IR_ADCX:
        case HB_IR_ADOX: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_ALL);
            if (r != HB_OK) return r;
            uint64_t dst = read_reg_sized(ctx, instr->dst.reg, instr->dst.size, 0);
            uint64_t src = 0;
            r = read_operand_value(ctx, &instr->src1, &src);
            if (r != HB_OK) return r;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            dst = trunc_to_size(dst, size);
            src = trunc_to_size(src, size);
            uint64_t carry_in = (instr->op == HB_IR_ADCX) ? (ctx->flags.cf ? 1u : 0u)
                                                          : (ctx->flags.of ? 1u : 0u);
            __uint128_t sum = (__uint128_t)dst + (__uint128_t)src + carry_in;
            uint64_t result = trunc_to_size((uint64_t)sum, size);
            unsigned width = (size == HB_SIZE_64) ? 64u : 32u;
            bool carry_out = (sum >> width) != 0;
            write_reg_sized(ctx, instr->dst.reg, result, size);
            hb_lazy_flags_clear(ctx);
            if (instr->op == HB_IR_ADCX) ctx->flags.cf = carry_out;
            else ctx->flags.of = carry_out;
            return HB_OK;
        }

        case HB_IR_BSWAP: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            hb_size_t size = instr->dst.size ? instr->dst.size : HB_SIZE_32;
            uint64_t val = read_reg(ctx, instr->dst.reg);
            if (size == HB_SIZE_64) {
                val = ((val & 0x00000000000000ffULL) << 56) |
                      ((val & 0x000000000000ff00ULL) << 40) |
                      ((val & 0x0000000000ff0000ULL) << 24) |
                      ((val & 0x00000000ff000000ULL) << 8)  |
                      ((val & 0x000000ff00000000ULL) >> 8)  |
                      ((val & 0x0000ff0000000000ULL) >> 24) |
                      ((val & 0x00ff000000000000ULL) >> 40) |
                      ((val & 0xff00000000000000ULL) >> 56);
                write_reg_sized(ctx, instr->dst.reg, val, HB_SIZE_64);
            } else if (size == HB_SIZE_32) {
                uint32_t v = (uint32_t)val;
                v = ((v & 0x000000ffU) << 24) |
                    ((v & 0x0000ff00U) << 8)  |
                    ((v & 0x00ff0000U) >> 8)  |
                    ((v & 0xff000000U) >> 24);
                write_reg_sized(ctx, instr->dst.reg, v, HB_SIZE_32);
            } else {
                return HB_ERR_UNSUPPORTED_OPCODE;
            }
            return HB_OK;
        }

        case HB_IR_MOVBE: {
            hb_size_t size = (hb_size_t)(instr->target ? instr->target : instr->dst.size);
            if (!(size == HB_SIZE_16 || size == HB_SIZE_32 || size == HB_SIZE_64)) {
                return HB_ERR_INTERNAL;
            }
            uint64_t value = 0;
            r = read_operand_value(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            value = bswap_sized_value(value, size);
            return write_operand_value(ctx, &instr->dst, value);
        }

        case HB_IR_MOVDIR64B: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint8_t bytes[64];
            uint64_t dst_addr = read_reg_sized(ctx, instr->dst.reg, HB_SIZE_64, instr->dst.reg_offset);
            uint64_t src_addr = resolve_addr(ctx, &instr->src1);
            r = hb_memory_read(ctx->memory, src_addr, bytes, sizeof(bytes));
            if (r != HB_OK) return r;
            return hb_memory_write(ctx->memory, dst_addr, bytes, sizeof(bytes));
        }

        case HB_IR_XMM_AND:
        case HB_IR_XMM_ANDN:
        case HB_IR_XMM_OR:
        case HB_IR_XORPS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint32_t arg = evex_target_arg(instr);
            bool broadcast = (arg & HB_EVEX_ARG_BROADCAST) != 0;
            unsigned lane = arg & 0xffu;
            if (!(lane == 4 || lane == 8)) lane = 4;
            uint8_t lhs[64], rhs[64], out[64];
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, broadcast ? lane : bytes);
            if (r != HB_OK) return r;
            if (broadcast) {
                for (unsigned i = 1; i < (unsigned)(bytes / lane); i++)
                    memcpy(rhs + i * lane, rhs, lane);
            }
            for (size_t n = 0; n < bytes; n++) {
                if (instr->op == HB_IR_XMM_AND) out[n] = lhs[n] & rhs[n];
                else if (instr->op == HB_IR_XMM_ANDN) out[n] = (uint8_t)(~lhs[n] & rhs[n]);
                else if (instr->op == HB_IR_XMM_OR) out[n] = lhs[n] | rhs[n];
                else out[n] = lhs[n] ^ rhs[n];
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, lane);
        }

        case HB_IR_XMM_SCALAR_MOV: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            unsigned lane = (unsigned)(instr->target & 0xffu);
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            uint8_t out[16] = {0};
            uint8_t scalar[8] = {0};
            if (instr->src1.type != HB_OP_NONE) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, out, sizeof(out));
                if (r != HB_OK) return r;
            }
            r = read_xmm_operand_bytes(ctx, &instr->src2, scalar, lane);
            if (r != HB_OK) return r;
            memcpy(out, scalar, lane);
            return write_vec_reg_bytes_evex_scalar_masked(ctx, instr, out, lane);
        }

        case HB_IR_PCMPEQB:
        case HB_IR_PCMPEQW:
        case HB_IR_PCMPEQD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            unsigned lane = instr->op == HB_IR_PCMPEQB ? 1 : (instr->op == HB_IR_PCMPEQW ? 2 : 4);
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes; i += lane) {
                if (!memcmp(lbytes + i, rbytes + i, lane))
                    memset(obytes + i, 0xff, lane);
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, lane);
        }

        case HB_IR_PCMPGTB:
        case HB_IR_PCMPGTW:
        case HB_IR_PCMPGTD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            unsigned lane = instr->op == HB_IR_PCMPGTB ? 1 : (instr->op == HB_IR_PCMPGTW ? 2 : 4);
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes; i += lane) {
                bool gt = false;
                if (lane == 1) gt = *(int8_t *)(lbytes + i) > *(int8_t *)(rbytes + i);
                else if (lane == 2) {
                    int16_t a, b;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    gt = a > b;
                } else {
                    int32_t a, b;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    gt = a > b;
                }
                if (gt) memset(obytes + i, 0xff, lane);
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, lane);
        }

        case HB_IR_PMOVMSKB: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG ||
                !is_xmm_reg(instr->src1.reg)) return HB_ERR_INTERNAL;
            uint64_t xmm[2];
            uint8_t bytes[16];
            uint32_t mask = 0;
            r = read_xmm_reg(ctx, instr->src1.reg, xmm);
            if (r != HB_OK) return r;
            memcpy(bytes, xmm, sizeof(bytes));
            for (unsigned i = 0; i < 16; i++)
                if (bytes[i] & 0x80) mask |= (uint32_t)1 << i;
            write_reg_sized(ctx, instr->dst.reg, mask, HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_MOVMSK: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG ||
                !is_xmm_reg(instr->src1.reg)) return HB_ERR_INTERNAL;
            uint64_t xmm[2];
            uint8_t bytes[16];
            uint32_t mask = 0;
            unsigned lane = (unsigned)(instr->target & 0xff);
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_reg(ctx, instr->src1.reg, xmm);
            if (r != HB_OK) return r;
            memcpy(bytes, xmm, sizeof(bytes));
            for (unsigned i = 0, bit = 0; i < 16; i += lane, bit++)
                if (bytes[i + lane - 1] & 0x80) mask |= (uint32_t)1 << bit;
            write_reg_sized(ctx, instr->dst.reg, mask, HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_PUNPCK: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;
            uint32_t target = evex_target_arg(instr);
            unsigned lane = (unsigned)(target & 0xff);
            bool high = (target & 0x100) != 0;
            if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            for (size_t base = 0; base < bytes; base += 16) {
                unsigned start = high ? 8 : 0;
                unsigned lanes = 8 / lane;
                unsigned out_pos = (unsigned)base;
                for (unsigned i = 0; i < lanes; i++) {
                    unsigned off = (unsigned)base + start + i * lane;
                    memcpy(obytes + out_pos, lbytes + off, lane);
                    out_pos += lane;
                    memcpy(obytes + out_pos, rbytes + off, lane);
                    out_pos += lane;
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, lane);
        }

        case HB_IR_PACKSSWB:
        case HB_IR_PACKUSWB:
        case HB_IR_PACKSSDW: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lhs[64], rhs[64], obytes[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
            if (r != HB_OK) return r;

            for (size_t base = 0; base < bytes; base += 16) {
                if (instr->op == HB_IR_PACKSSDW) {
                    for (unsigned i = 0; i < 4; i++) {
                        int32_t v;
                        int16_t o;
                        memcpy(&v, lhs + base + i * 4, sizeof(v));
                        o = v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
                        memcpy(obytes + base + i * 2, &o, sizeof(o));
                    }
                    for (unsigned i = 0; i < 4; i++) {
                        int32_t v;
                        int16_t o;
                        memcpy(&v, rhs + base + i * 4, sizeof(v));
                        o = v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v);
                        memcpy(obytes + base + 8 + i * 2, &o, sizeof(o));
                    }
                } else {
                    for (unsigned i = 0; i < 8; i++) {
                        int16_t v;
                        memcpy(&v, lhs + base + i * 2, sizeof(v));
                        if (instr->op == HB_IR_PACKUSWB)
                            obytes[base + i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
                        else
                            obytes[base + i] = (uint8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
                    }
                    for (unsigned i = 0; i < 8; i++) {
                        int16_t v;
                        memcpy(&v, rhs + base + i * 2, sizeof(v));
                        if (instr->op == HB_IR_PACKUSWB)
                            obytes[base + 8 + i] = v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
                        else
                            obytes[base + 8 + i] = (uint8_t)(v > 127 ? 127 : (v < -128 ? -128 : v));
                    }
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes,
                                                   instr->op == HB_IR_PACKSSDW ? 2 : 1);
        }

        case HB_IR_PMULLW:
        case HB_IR_PMULHW:
        case HB_IR_PMULHUW:
        case HB_IR_PMADDWD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lhs[64], rhs[64], out[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
            if (r != HB_OK) return r;

            if (instr->op == HB_IR_PMADDWD) {
                for (size_t off = 0; off < bytes; off += 4) {
                    int16_t a0, a1, b0, b1;
                    int32_t value;
                    memcpy(&a0, lhs + off, sizeof(a0));
                    memcpy(&a1, lhs + off + 2, sizeof(a1));
                    memcpy(&b0, rhs + off, sizeof(b0));
                    memcpy(&b1, rhs + off + 2, sizeof(b1));
                    value = (int32_t)a0 * (int32_t)b0 + (int32_t)a1 * (int32_t)b1;
                    memcpy(out + off, &value, sizeof(value));
                }
            } else {
                for (size_t off = 0; off < bytes; off += 2) {
                    uint16_t value;
                    if (instr->op == HB_IR_PMULHUW) {
                        uint16_t a, b;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        value = (uint16_t)(((uint32_t)a * (uint32_t)b) >> 16);
                    } else {
                        int16_t a, b;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        int32_t p = (int32_t)a * (int32_t)b;
                        value = (uint16_t)(instr->op == HB_IR_PMULLW ? p : (p >> 16));
                    }
                    memcpy(out + off, &value, sizeof(value));
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes,
                                                   instr->op == HB_IR_PMADDWD ? 4 : 2);
        }

        case HB_IR_PADDSB:
        case HB_IR_PADDSW:
        case HB_IR_PADDUSB:
        case HB_IR_PADDUSW:
        case HB_IR_PAVGB:
        case HB_IR_PAVGW: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;

            if (instr->op == HB_IR_PADDSB || instr->op == HB_IR_PADDUSB || instr->op == HB_IR_PAVGB) {
                for (unsigned i = 0; i < bytes; i++) {
                    if (instr->op == HB_IR_PAVGB) {
                        obytes[i] = (uint8_t)(((unsigned)lbytes[i] + (unsigned)rbytes[i] + 1u) >> 1);
                    } else if (instr->op == HB_IR_PADDUSB) {
                        unsigned v = (unsigned)lbytes[i] + (unsigned)rbytes[i];
                        obytes[i] = (uint8_t)(v > 255u ? 255u : v);
                    } else {
                        int v = (int)(int8_t)lbytes[i] + (int)(int8_t)rbytes[i];
                        if (v > 127) v = 127;
                        else if (v < -128) v = -128;
                        obytes[i] = (uint8_t)(int8_t)v;
                    }
                }
            } else {
                for (unsigned i = 0; i < bytes; i += 2) {
                    uint16_t a, b, value;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    if (instr->op == HB_IR_PAVGW) {
                        value = (uint16_t)(((uint32_t)a + (uint32_t)b + 1u) >> 1);
                    } else if (instr->op == HB_IR_PADDUSW) {
                        uint32_t v = (uint32_t)a + (uint32_t)b;
                        value = (uint16_t)(v > 65535u ? 65535u : v);
                    } else {
                        int32_t v = (int32_t)(int16_t)a + (int32_t)(int16_t)b;
                        if (v > 32767) v = 32767;
                        else if (v < -32768) v = -32768;
                        value = (uint16_t)(int16_t)v;
                    }
                    memcpy(obytes + i, &value, sizeof(value));
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes,
                                                   (instr->op == HB_IR_PADDSB || instr->op == HB_IR_PADDUSB ||
                                                    instr->op == HB_IR_PAVGB) ? 1 : 2);
        }

        case HB_IR_PSHUFB: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t src[64] = {0}, mask[64] = {0}, obytes[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, src, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, mask, bytes);
            if (r != HB_OK) return r;
            for (size_t i = 0; i < bytes; i++) {
                size_t lane_mask = bytes == 8 ? 0x07u : 0x0fu;
                size_t base = bytes == 8 ? 0 : (i & ~(size_t)0x0f);
                obytes[i] = (mask[i] & 0x80) ? 0 : src[base + (mask[i] & lane_mask)];
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, 1);
        }

        case HB_IR_PINSRW: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t xmm[2], out[2] = {0, 0};
            uint8_t bytes[16];
            uint64_t word = 0;
            unsigned lane = (unsigned)(instr->target & 7u);
            r = read_xmm_operand(ctx, &instr->src1, xmm);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &word);
            if (r != HB_OK) return r;
            memcpy(bytes, xmm, sizeof(bytes));
            bytes[lane * 2] = (uint8_t)(word & 0xffu);
            bytes[lane * 2 + 1] = (uint8_t)((word >> 8) & 0xffu);
            memcpy(out, bytes, sizeof(bytes));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_PEXTRW: {
            if (instr->dst.type != HB_OP_REG || is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t xmm[2];
            uint8_t bytes[16];
            unsigned lane = (unsigned)(instr->target & 7u);
            r = read_xmm_operand(ctx, &instr->src1, xmm);
            if (r != HB_OK) return r;
            memcpy(bytes, xmm, sizeof(bytes));
            uint16_t word = (uint16_t)bytes[lane * 2] | ((uint16_t)bytes[lane * 2 + 1] << 8);
            return write_operand_value(ctx, &instr->dst, word);
        }

        case HB_IR_INSERTPS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint8_t out[16], src[16] = {0};
            unsigned imm = (unsigned)(instr->target & 0xffu);
            unsigned src_lane = (imm >> 6) & 3u;
            unsigned dst_lane = (imm >> 4) & 3u;
            r = read_xmm_operand_bytes(ctx, &instr->src1, out, 16);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, src, instr->src2.type == HB_OP_MEM ? 4 : 16);
            if (r != HB_OK) return r;
            memcpy(out + dst_lane * 4, src + (instr->src2.type == HB_OP_MEM ? 0 : src_lane * 4), 4);
            for (unsigned lane = 0; lane < 4; lane++) {
                if (imm & (1u << lane)) memset(out + lane * 4, 0, 4);
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
        }

        case HB_IR_EXTRACTPS: {
            uint8_t src[16];
            uint32_t value;
            unsigned lane = (unsigned)(instr->target & 3u);
            r = read_xmm_operand_bytes(ctx, &instr->src1, src, 16);
            if (r != HB_OK) return r;
            memcpy(&value, src + lane * 4, sizeof(value));
            return write_operand_value(ctx, &instr->dst, value);
        }

        case HB_IR_PINSR: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            unsigned elem_size = (unsigned)(instr->target & 0xffu);
            unsigned lane = (unsigned)((instr->target >> 8) & 0xffu);
            if (!(elem_size == 1 || elem_size == 2 || elem_size == 4 || elem_size == 8)) return HB_ERR_INTERNAL;
            lane &= (16u / elem_size) - 1u;
            uint8_t out[16];
            uint64_t value = 0;
            r = read_xmm_operand_bytes(ctx, &instr->src1, out, 16);
            if (r != HB_OK) return r;
            r = read_operand_value(ctx, &instr->src2, &value);
            if (r != HB_OK) return r;
            memcpy(out + lane * elem_size, &value, elem_size);
            return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
        }

        case HB_IR_PEXTR: {
            unsigned elem_size = (unsigned)(instr->target & 0xffu);
            unsigned lane = (unsigned)((instr->target >> 8) & 0xffu);
            if (!(elem_size == 1 || elem_size == 2 || elem_size == 4 || elem_size == 8)) return HB_ERR_INTERNAL;
            lane &= (16u / elem_size) - 1u;
            uint8_t src[16];
            uint64_t value = 0;
            r = read_xmm_operand_bytes(ctx, &instr->src1, src, 16);
            if (r != HB_OK) return r;
            memcpy(&value, src + lane * elem_size, elem_size);
            return write_operand_value(ctx, &instr->dst, value);
        }

        case HB_IR_PSHUF: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            if (instr->src2.type != HB_OP_IMM) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t ibytes[64], obytes[64];
            unsigned imm = (unsigned)instr->src2.imm & 0xff;
            uint32_t target = evex_target_arg(instr);
            unsigned mask_lane = 4;
            r = read_xmm_operand_bytes(ctx, &instr->src1, ibytes, bytes);
            if (r != HB_OK) return r;
            memcpy(obytes, ibytes, bytes);
            for (size_t base = 0; base < bytes; base += 16) {
                if (target == 4) {
                    mask_lane = 4;
                    for (unsigned lane = 0; lane < 4; lane++) {
                        unsigned src = (imm >> (lane * 2)) & 3;
                        memcpy(obytes + base + lane * 4, ibytes + base + src * 4, 4);
                    }
                } else if (target == 2) {
                    mask_lane = 2;
                    for (unsigned lane = 0; lane < 4; lane++) {
                        unsigned src = (imm >> (lane * 2)) & 3;
                        memcpy(obytes + base + lane * 2, ibytes + base + src * 2, 2);
                    }
                } else if (target == 0x102) {
                    mask_lane = 2;
                    for (unsigned lane = 0; lane < 4; lane++) {
                        unsigned src = (imm >> (lane * 2)) & 3;
                        memcpy(obytes + base + 8 + lane * 2,
                               ibytes + base + 8 + src * 2, 2);
                    }
                } else {
                    return HB_ERR_INTERNAL;
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, mask_lane);
        }

        case HB_IR_FSHUF: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lhs[64], rhs[64], out_bytes[64] = {0};
            uint32_t target = evex_target_arg(instr);
            unsigned lane = (unsigned)(target & 0xff);
            unsigned imm = (unsigned)((target >> 8) & 0xff);
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
            if (r != HB_OK) return r;
            for (size_t base = 0; base < bytes; base += 16) {
                if (lane == 4) {
                    unsigned s0 = (imm >> 0) & 3;
                    unsigned s1 = (imm >> 2) & 3;
                    unsigned s2 = (imm >> 4) & 3;
                    unsigned s3 = (imm >> 6) & 3;
                    memcpy(out_bytes + base + 0, lhs + base + s0 * 4, 4);
                    memcpy(out_bytes + base + 4, lhs + base + s1 * 4, 4);
                    memcpy(out_bytes + base + 8, rhs + base + s2 * 4, 4);
                    memcpy(out_bytes + base + 12, rhs + base + s3 * 4, 4);
                } else {
                    unsigned s0 = imm & 1;
                    unsigned s1 = (imm >> 1) & 1;
                    memcpy(out_bytes + base + 0, lhs + base + s0 * 8, 8);
                    memcpy(out_bytes + base + 8, rhs + base + s1 * 8, 8);
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, out_bytes, bytes, lane);
        }

        case HB_IR_PSRL:
        case HB_IR_PSRA:
        case HB_IR_PSLL: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t src[32], dst[32] = {0};
            unsigned lane = (unsigned)(instr->target & 0xff);
            unsigned count = 0;
            r = read_packed_shift_count(ctx, &instr->src2, &count);
            if (r != HB_OK) return r;
            if (!(lane == 2 || lane == 4)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, src, bytes);
            if (r != HB_OK) return r;

            for (unsigned off = 0; off < bytes; off += lane) {
                if (lane == 2) {
                    uint16_t value;
                    memcpy(&value, src + off, sizeof(value));
                    uint16_t result = 0;
                    if (instr->op == HB_IR_PSRL) {
                        result = count >= 16 ? 0 : (uint16_t)(value >> count);
                    } else if (instr->op == HB_IR_PSLL) {
                        result = count >= 16 ? 0 : (uint16_t)(value << count);
                    } else {
                        int16_t signed_value;
                        memcpy(&signed_value, &value, sizeof(signed_value));
                        result = (uint16_t)(count >= 16 ? (signed_value < 0 ? -1 : 0)
                                                        : (int16_t)(signed_value >> count));
                    }
                    memcpy(dst + off, &result, sizeof(result));
                } else {
                    uint32_t value;
                    memcpy(&value, src + off, sizeof(value));
                    uint32_t result = 0;
                    if (instr->op == HB_IR_PSRL) {
                        result = count >= 32 ? 0 : (uint32_t)(value >> count);
                    } else if (instr->op == HB_IR_PSLL) {
                        result = count >= 32 ? 0 : (uint32_t)(value << count);
                    } else {
                        int32_t signed_value;
                        memcpy(&signed_value, &value, sizeof(signed_value));
                        result = (uint32_t)(count >= 32 ? (signed_value < 0 ? -1 : 0)
                                                        : (int32_t)(signed_value >> count));
                    }
                    memcpy(dst + off, &result, sizeof(result));
                }
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, dst, bytes);
        }

        case HB_IR_PSRLQ:
        case HB_IR_PSLLQ:
        case HB_IR_PSRLDQ:
        case HB_IR_PSLLDQ: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t src[32] = {0}, dst[32] = {0};
            unsigned count = 0;
            r = read_packed_shift_count(ctx, &instr->src2, &count);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src1, src, bytes);
            if (r != HB_OK) return r;

            if (instr->op == HB_IR_PSRLQ) {
                for (size_t off = 0; off < bytes; off += 8) {
                    uint64_t value = 0, result = 0;
                    memcpy(&value, src + off, sizeof(value));
                    result = (count > 63) ? 0 : (value >> count);
                    memcpy(dst + off, &result, sizeof(result));
                }
            } else if (instr->op == HB_IR_PSLLQ) {
                for (size_t off = 0; off < bytes; off += 8) {
                    uint64_t value = 0, result = 0;
                    memcpy(&value, src + off, sizeof(value));
                    result = (count > 63) ? 0 : (value << count);
                    memcpy(dst + off, &result, sizeof(result));
                }
            } else {
                for (size_t base = 0; base < bytes; base += 16) {
                    if (count >= 16) continue;
                    if (instr->op == HB_IR_PSRLDQ) {
                        memmove(dst + base, src + base + count, 16 - count);
                    } else {
                        memmove(dst + base + count, src + base, 16 - count);
                    }
                }
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, dst, bytes);
        }

        case HB_IR_PADD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            unsigned lane = (unsigned)(instr->target & 0xff);
            if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes; i += lane) {
                if (lane == 1) {
                    obytes[i] = (uint8_t)(lbytes[i] + rbytes[i]);
                } else if (lane == 2) {
                    uint16_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = (uint16_t)(a + b);
                    memcpy(obytes + i, &c, sizeof(c));
                } else if (lane == 4) {
                    uint32_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = a + b;
                    memcpy(obytes + i, &c, sizeof(c));
                } else {
                    uint64_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = a + b;
                    memcpy(obytes + i, &c, sizeof(c));
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, lane);
        }

        case HB_IR_PSUB: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lbytes[64], rbytes[64], obytes[64] = {0};
            unsigned lane = (unsigned)(instr->target & 0xff);
            if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lbytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rbytes, bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes; i += lane) {
                if (lane == 1) {
                    obytes[i] = (uint8_t)(lbytes[i] - rbytes[i]);
                } else if (lane == 2) {
                    uint16_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = (uint16_t)(a - b);
                    memcpy(obytes + i, &c, sizeof(c));
                } else if (lane == 4) {
                    uint32_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = a - b;
                    memcpy(obytes + i, &c, sizeof(c));
                } else {
                    uint64_t a, b, c;
                    memcpy(&a, lbytes + i, sizeof(a));
                    memcpy(&b, rbytes + i, sizeof(b));
                    c = a - b;
                    memcpy(obytes + i, &c, sizeof(c));
                }
            }
            return write_vec_reg_bytes_evex_masked(ctx, instr, obytes, bytes, lane);
        }

        case HB_IR_EVEX_CMP_MASK: {
            uint64_t meta = instr->target;
            unsigned kdst = (unsigned)(meta & 7u);
            unsigned kmask = (unsigned)((meta >> 3) & 7u);
            bool scalar = ((meta >> 6) & 1u) != 0;
            bool is_pd = ((meta >> 7) & 1u) != 0;
            bool broadcast = ((meta >> 8) & 1u) != 0;
            unsigned pred = (unsigned)((meta >> 16) & 31u);
            unsigned lane = is_pd ? 8u : 4u;
            size_t bytes = scalar ? lane : bytes_for_size(instr->src1.size);
            if (bytes == 0 || bytes > 64) bytes = scalar ? lane : 64;

            uint8_t lhs[64] = {0};
            uint8_t rhs[64] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, scalar ? 16 : bytes);
            if (r != HB_OK) return r;
            if (broadcast && instr->src2.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->src2);
                r = hb_memory_read(ctx->memory, addr, rhs, lane);
                if (r != HB_OK) return r;
                for (size_t off = lane; off < bytes; off += lane) memcpy(rhs + off, rhs, lane);
            } else {
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, scalar ? lane : bytes);
                if (r != HB_OK) return r;
            }

            unsigned lanes = scalar ? 1u : (unsigned)(bytes / lane);
            uint64_t result = 0;
            for (unsigned lane_idx = 0; lane_idx < lanes; lane_idx++) {
                size_t off = (size_t)lane_idx * lane;
                bool cmp;
                if (is_pd) {
                    double a, b;
                    memcpy(&a, lhs + off, sizeof(a));
                    memcpy(&b, rhs + off, sizeof(b));
                    bool unordered = isnan(a) || isnan(b);
                    cmp = fp_cmp_predicate(unordered, a == b, a < b, a <= b, pred);
                } else {
                    float a, b;
                    memcpy(&a, lhs + off, sizeof(a));
                    memcpy(&b, rhs + off, sizeof(b));
                    bool unordered = isnan(a) || isnan(b);
                    cmp = fp_cmp_predicate(unordered, a == b, a < b, a <= b, pred);
                }
                if (cmp) result |= 1ull << lane_idx;
            }
            if (kmask != 0) result &= ctx->k[kmask & 7u];
            ctx->k[kdst & 7u] = result & ((lanes >= 64) ? UINT64_MAX : ((1ull << lanes) - 1ull));
            return HB_OK;
        }

        case HB_IR_VEC_PACKED: {
            hb_ir_vec_op_t vop = (hb_ir_vec_op_t)(instr->target >> 32);
            unsigned arg = evex_target_arg(instr);
            unsigned imm = arg & 0xffu;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t dst_old[64] = {0}, lhs[64] = {0}, rhs[64] = {0}, out[64] = {0};
            if (vop >= HB_VEC_VFMADD132 && vop <= HB_VEC_VFNMSUB231) {
                if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
                unsigned lane = arg & 0xffu;
                bool scalar = (arg & 0x100u) != 0;
                if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
                bytes = scalar ? 16 : bytes;
                r = read_vec_reg_bytes(ctx, instr->dst.reg, dst_old, bytes);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, scalar ? lane : bytes);
                if (r != HB_OK) return r;
                if (scalar) memcpy(out, lhs, bytes);
                for (size_t off = 0; off < (scalar ? lane : bytes); off += lane) {
                    if (lane == 4) {
                        uint32_t dbits, s1bits, s2bits, rbits;
                        memcpy(&dbits, dst_old + off, sizeof(dbits));
                        memcpy(&s1bits, lhs + off, sizeof(s1bits));
                        memcpy(&s2bits, rhs + off, sizeof(s2bits));
                        float dval = hb_bits_to_float(dbits);
                        float s1 = hb_bits_to_float(s1bits);
                        float s2 = hb_bits_to_float(s2bits);
                        bool even_lane = ((off / lane) & 1u) == 0;
                        float result;
                        if (vop == HB_VEC_VFMADD132) result = fmaf(dval, s2, s1);
                        else if (vop == HB_VEC_VFMADD213) result = fmaf(s1, dval, s2);
                        else if (vop == HB_VEC_VFMADD231) result = fmaf(s1, s2, dval);
                        else if (vop == HB_VEC_VFMSUB132) result = fmaf(dval, s2, -s1);
                        else if (vop == HB_VEC_VFMSUB213) result = fmaf(s1, dval, -s2);
                        else if (vop == HB_VEC_VFMSUB231) result = fmaf(s1, s2, -dval);
                        else if (vop == HB_VEC_VFMADDSUB132) result = fmaf(dval, s2, even_lane ? -s1 : s1);
                        else if (vop == HB_VEC_VFMSUBADD132) result = fmaf(dval, s2, even_lane ? s1 : -s1);
                        else if (vop == HB_VEC_VFMADDSUB213) result = fmaf(s1, dval, even_lane ? -s2 : s2);
                        else if (vop == HB_VEC_VFMSUBADD213) result = fmaf(s1, dval, even_lane ? s2 : -s2);
                        else if (vop == HB_VEC_VFMADDSUB231) result = fmaf(s1, s2, even_lane ? -dval : dval);
                        else if (vop == HB_VEC_VFMSUBADD231) result = fmaf(s1, s2, even_lane ? dval : -dval);
                        else if (vop == HB_VEC_VFNMADD132) result = fmaf(-dval, s2, s1);
                        else if (vop == HB_VEC_VFNMADD213) result = fmaf(-s1, dval, s2);
                        else if (vop == HB_VEC_VFNMADD231) result = fmaf(-s1, s2, dval);
                        else if (vop == HB_VEC_VFNMSUB132) result = fmaf(-dval, s2, -s1);
                        else if (vop == HB_VEC_VFNMSUB213) result = fmaf(-s1, dval, -s2);
                        else result = fmaf(-s1, s2, -dval);
                        rbits = hb_float_to_bits(result);
                        memcpy(out + off, &rbits, sizeof(rbits));
                    } else {
                        uint64_t dbits, s1bits, s2bits, rbits;
                        memcpy(&dbits, dst_old + off, sizeof(dbits));
                        memcpy(&s1bits, lhs + off, sizeof(s1bits));
                        memcpy(&s2bits, rhs + off, sizeof(s2bits));
                        double dval = hb_bits_to_double(dbits);
                        double s1 = hb_bits_to_double(s1bits);
                        double s2 = hb_bits_to_double(s2bits);
                        bool even_lane = ((off / lane) & 1u) == 0;
                        double result;
                        if (vop == HB_VEC_VFMADD132) result = fma(dval, s2, s1);
                        else if (vop == HB_VEC_VFMADD213) result = fma(s1, dval, s2);
                        else if (vop == HB_VEC_VFMADD231) result = fma(s1, s2, dval);
                        else if (vop == HB_VEC_VFMSUB132) result = fma(dval, s2, -s1);
                        else if (vop == HB_VEC_VFMSUB213) result = fma(s1, dval, -s2);
                        else if (vop == HB_VEC_VFMSUB231) result = fma(s1, s2, -dval);
                        else if (vop == HB_VEC_VFMADDSUB132) result = fma(dval, s2, even_lane ? -s1 : s1);
                        else if (vop == HB_VEC_VFMSUBADD132) result = fma(dval, s2, even_lane ? s1 : -s1);
                        else if (vop == HB_VEC_VFMADDSUB213) result = fma(s1, dval, even_lane ? -s2 : s2);
                        else if (vop == HB_VEC_VFMSUBADD213) result = fma(s1, dval, even_lane ? s2 : -s2);
                        else if (vop == HB_VEC_VFMADDSUB231) result = fma(s1, s2, even_lane ? -dval : dval);
                        else if (vop == HB_VEC_VFMSUBADD231) result = fma(s1, s2, even_lane ? dval : -dval);
                        else if (vop == HB_VEC_VFNMADD132) result = fma(-dval, s2, s1);
                        else if (vop == HB_VEC_VFNMADD213) result = fma(-s1, dval, s2);
                        else if (vop == HB_VEC_VFNMADD231) result = fma(-s1, s2, dval);
                        else if (vop == HB_VEC_VFNMSUB132) result = fma(-dval, s2, -s1);
                        else if (vop == HB_VEC_VFNMSUB213) result = fma(-s1, dval, -s2);
                        else result = fma(-s1, s2, -dval);
                        rbits = hb_double_to_bits(result);
                        memcpy(out + off, &rbits, sizeof(rbits));
                    }
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 16);
            }
            if (vop == HB_VEC_VCVTPH2PS) {
                if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
                size_t src_bytes = bytes / 2;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, src_bytes);
                if (r != HB_OK) return r;
                for (size_t i = 0; i < src_bytes / 2; i++) {
                    uint16_t h;
                    uint32_t fbits;
                    memcpy(&h, lhs + i * 2, sizeof(h));
                    fbits = hb_half_to_float_bits(h);
                    memcpy(out + i * 4, &fbits, sizeof(fbits));
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 16);
            }
            if (vop == HB_VEC_VCVTPS2PH) {
                size_t src_bytes = bytes_for_size(instr->src1.size);
                if (src_bytes == 0) src_bytes = 16;
                size_t result_bytes = src_bytes / 2;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, src_bytes);
                if (r != HB_OK) return r;
                for (size_t i = 0; i < src_bytes / 4; i++) {
                    uint32_t fbits;
                    uint16_t h;
                    memcpy(&fbits, lhs + i * 4, sizeof(fbits));
                    h = hb_float_bits_to_half(fbits, imm);
                    memcpy(out + i * 2, &h, sizeof(h));
                }
                if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg))
                    return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
                if (instr->dst.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->dst);
                    return hb_memory_write(ctx->memory, addr, out, result_bytes);
                }
                return HB_ERR_INTERNAL;
            }
            if (vop == HB_VEC_PCMPESTRM || vop == HB_VEC_PCMPESTRI ||
                vop == HB_VEC_PCMPISTRM || vop == HB_VEC_PCMPISTRI) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 16);
                if (r != HB_OK) return r;
                unsigned max_units = 0;
                int len1 = 0, len2 = 0;
                uint32_t res = pcmpxstr_result(ctx, vop, lhs, rhs, imm, &max_units, &len1, &len2);
                ctx->lazy_flags.pending = false;
                ctx->flags.cf = res != 0;
                ctx->flags.zf = len2 < (int)max_units;
                ctx->flags.sf = len1 < (int)max_units;
                ctx->flags.of = (res & 1u) != 0;
                ctx->flags.af = false;
                ctx->flags.pf = false;
                if (vop == HB_VEC_PCMPESTRI || vop == HB_VEC_PCMPISTRI) {
                    uint32_t idx = 0;
                    if ((imm >> 6) & 1u) {
                        for (int i = (int)max_units - 1; i >= 0; i--) {
                            if ((res >> i) & 1u) { idx = (uint32_t)i; break; }
                        }
                    } else {
                        idx = (uint32_t)max_units;
                        for (unsigned i = 0; i < max_units; i++) {
                            if ((res >> i) & 1u) { idx = i; break; }
                        }
                    }
                    write_reg_sized(ctx, HB_REG_RCX, idx, HB_SIZE_32);
                    return HB_OK;
                }
                unsigned unit = (imm & 1u) ? 2 : 1;
                if ((imm >> 6) & 1u) {
                    for (unsigned i = 0; i < max_units; i++) {
                        if ((res >> i) & 1u) memset(out + i * unit, 0xff, unit);
                    }
                } else {
                    uint16_t compact = (uint16_t)res;
                    memcpy(out, &compact, sizeof(compact));
                }
                return write_vec_reg_bytes(ctx, HB_REG_XMM0, out, 16);
            }
            if (vop == HB_VEC_VEXTRACTF128 || vop == HB_VEC_VEXTRACTI128) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 32);
                if (r != HB_OK) return r;
                memcpy(out, lhs + ((imm & 1u) ? 16 : 0), 16);
                if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg)) {
                    return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
                }
                if (instr->dst.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->dst);
                    return hb_memory_write(ctx->memory, addr, out, 16);
                }
                return HB_ERR_INTERNAL;
            }
            if (vop == HB_VEC_VINSERTF128 || vop == HB_VEC_VINSERTI128) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 32);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 16);
                if (r != HB_OK) return r;
                memcpy(out, lhs, 32);
                memcpy(out + ((imm & 1u) ? 16 : 0), rhs, 16);
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 32);
            }
            if (vop == HB_VEC_VPERM2F128 || vop == HB_VEC_VPERM2I128) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 32);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 32);
                if (r != HB_OK) return r;
                const uint8_t* halves[4] = { lhs, lhs + 16, rhs, rhs + 16 };
                if (imm & 0x08u) memset(out, 0, 16);
                else memcpy(out, halves[imm & 3u], 16);
                if (imm & 0x80u) memset(out + 16, 0, 16);
                else memcpy(out + 16, halves[(imm >> 4) & 3u], 16);
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 32);
            }
            if (vop == HB_VEC_VPERMQ || vop == HB_VEC_VPERMPD) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 32);
                if (r != HB_OK) return r;
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned src_lane = (imm >> (lane * 2)) & 3u;
                    memcpy(out + lane * 8, lhs + src_lane * 8, 8);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 32);
            }
            if (vop == HB_VEC_VPSRLVD || vop == HB_VEC_VPSRLVQ ||
                vop == HB_VEC_VPSRAVD || vop == HB_VEC_VPSLLVD ||
                vop == HB_VEC_VPSLLVQ) {
                unsigned lane = (vop == HB_VEC_VPSRLVQ || vop == HB_VEC_VPSLLVQ) ? 8 : 4;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                if (r != HB_OK) return r;
                for (size_t off = 0; off < bytes; off += lane) {
                    uint64_t count = load_lane_unsigned(rhs + off, lane);
                    if (lane == 8) {
                        uint64_t a;
                        uint64_t v;
                        memcpy(&a, lhs + off, sizeof(a));
                        if (count >= 64) v = 0;
                        else if (vop == HB_VEC_VPSLLVQ) v = a << count;
                        else v = a >> count;
                        memcpy(out + off, &v, sizeof(v));
                    } else if (vop == HB_VEC_VPSRAVD) {
                        int32_t a;
                        int32_t v;
                        memcpy(&a, lhs + off, sizeof(a));
                        if (count >= 32) v = a < 0 ? -1 : 0;
                        else v = a >> count;
                        memcpy(out + off, &v, sizeof(v));
                    } else {
                        uint32_t a;
                        uint32_t v;
                        memcpy(&a, lhs + off, sizeof(a));
                        if (count >= 32) v = 0;
                        else if (vop == HB_VEC_VPSLLVD) v = a << count;
                        else v = a >> count;
                        memcpy(out + off, &v, sizeof(v));
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }
            if (vop == HB_VEC_VPERMD || vop == HB_VEC_VPERMPS) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 32);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 32);
                if (r != HB_OK) return r;
                for (unsigned lane = 0; lane < 8; lane++) {
                    uint32_t idx;
                    memcpy(&idx, lhs + lane * 4, sizeof(idx));
                    memcpy(out + lane * 4, rhs + ((idx & 7u) * 4), 4);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 32);
            }
            if (vop == HB_VEC_VGATHERDPS || vop == HB_VEC_VGATHERDPD ||
                vop == HB_VEC_VGATHERQPS || vop == HB_VEC_VGATHERQPD ||
                vop == HB_VEC_VPGATHERDD || vop == HB_VEC_VPGATHERDQ ||
                vop == HB_VEC_VPGATHERQD || vop == HB_VEC_VPGATHERQQ) {
                if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg) ||
                    instr->src1.type != HB_OP_REG || !is_xmm_reg(instr->src1.reg) ||
                    instr->src2.type != HB_OP_MEM || !instr->src2.mem.vsib ||
                    !is_xmm_reg(instr->src2.mem.index)) {
                    return HB_ERR_INTERNAL;
                }
                unsigned elem = instr->src2.mem.vsib_elem_size;
                unsigned index_size = instr->src2.mem.vsib_index_size;
                unsigned count = instr->src2.mem.vsib_count;
                if (!((elem == 4 || elem == 8) && (index_size == 4 || index_size == 8) &&
                      count > 0 && count <= 8)) return HB_ERR_INTERNAL;
                r = read_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
                if (r != HB_OK) return r;
                r = read_vec_reg_bytes(ctx, instr->src1.reg, lhs, bytes);
                if (r != HB_OK) return r;
                r = read_vec_reg_bytes(ctx, instr->src2.mem.index, rhs, index_size * count);
                if (r != HB_OK) return r;
                hb_ir_operand_t base_op = instr->src2;
                base_op.mem.index = HB_REG_COUNT;
                uint64_t base_addr = resolve_addr(ctx, &base_op);
                for (unsigned lane_i = 0; lane_i < count; lane_i++) {
                    size_t off = (size_t)lane_i * elem;
                    if (!(lhs[off + elem - 1] & 0x80)) continue;
                    int64_t idx;
                    if (index_size == 4) {
                        int32_t v;
                        memcpy(&v, rhs + lane_i * index_size, sizeof(v));
                        idx = v;
                    } else {
                        memcpy(&idx, rhs + lane_i * index_size, sizeof(idx));
                    }
                    uint64_t addr = base_addr + (uint64_t)(idx * (int64_t)instr->src2.mem.scale);
                    r = hb_memory_read(ctx->memory, addr, out + off, elem);
                    if (r != HB_OK) return r;
                    memset(lhs + off, 0, elem);
                }
                r = write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
                if (r != HB_OK) return r;
                return write_vec_reg_bytes(ctx, instr->src1.reg, lhs, bytes);
            }
            if (vop == HB_VEC_VMASKMOVDQU) {
                if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg) ||
                    instr->src1.type != HB_OP_REG || !is_xmm_reg(instr->src1.reg)) {
                    return HB_ERR_INTERNAL;
                }
                r = read_xmm_operand_bytes(ctx, &instr->dst, lhs, 16);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src1, rhs, 16);
                if (r != HB_OK) return r;
                uint64_t addr = ctx->regs.x64.rdi;
                for (unsigned off = 0; off < 16; off++) {
                    if (rhs[off] & 0x80u) {
                        r = hb_memory_write(ctx->memory, addr + off, lhs + off, 1);
                        if (r != HB_OK) return r;
                    }
                }
                return HB_OK;
            }
            if (vop == HB_VEC_VMASKMOVPS || vop == HB_VEC_VMASKMOVPD ||
                vop == HB_VEC_VPMASKMOVD || vop == HB_VEC_VPMASKMOVQ) {
                unsigned lane = imm == 8 ? 8 : 4;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg) &&
                    instr->src2.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->src2);
                    for (size_t off = 0; off < bytes; off += lane) {
                        if (lhs[off + lane - 1] & 0x80) {
                            r = hb_memory_read(ctx->memory, addr + off, out + off, lane);
                            if (r != HB_OK) return r;
                        }
                    }
                    return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
                }
                if (instr->dst.type == HB_OP_MEM && instr->src2.type == HB_OP_REG &&
                    is_xmm_reg(instr->src2.reg)) {
                    r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                    if (r != HB_OK) return r;
                    uint64_t addr = resolve_addr(ctx, &instr->dst);
                    for (size_t off = 0; off < bytes; off += lane) {
                        if (lhs[off + lane - 1] & 0x80) {
                            r = hb_memory_write(ctx->memory, addr + off, rhs + off, lane);
                            if (r != HB_OK) return r;
                        }
                    }
                    return HB_OK;
                }
                return HB_ERR_INTERNAL;
            }
            if (vop == HB_VEC_PCLMULQDQ) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 16);
                if (r != HB_OK) return r;
                uint64_t a, b;
                memcpy(&a, lhs + ((imm & 0x01u) ? 8 : 0), sizeof(a));
                memcpy(&b, rhs + ((imm & 0x10u) ? 8 : 0), sizeof(b));
                pclmul64(a, b, out);
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
            }
            if (vop == HB_VEC_AESKEYGENASSIST) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
                if (r != HB_OK) return r;
                uint32_t w1, w3, sw1, sw3, rw1, rw3;
                memcpy(&w1, lhs + 4, sizeof(w1));
                memcpy(&w3, lhs + 12, sizeof(w3));
                sw1 = aes_subword(w1);
                sw3 = aes_subword(w3);
                rw1 = aes_rotword(sw1) ^ (imm & 0xffu);
                rw3 = aes_rotword(sw3) ^ (imm & 0xffu);
                memcpy(out, &sw1, sizeof(sw1));
                memcpy(out + 4, &rw1, sizeof(rw1));
                memcpy(out + 8, &sw3, sizeof(sw3));
                memcpy(out + 12, &rw3, sizeof(rw3));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
            }
            if (vop == HB_VEC_AESIMC || vop == HB_VEC_AESENC || vop == HB_VEC_AESENCLAST ||
                vop == HB_VEC_AESDEC || vop == HB_VEC_AESDECLAST) {
                if (bytes != 16 && bytes != 32 && bytes != 64) return HB_ERR_INTERNAL;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                if (vop != HB_VEC_AESIMC) {
                    r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                    if (r != HB_OK) return r;
                }
                for (size_t off = 0; off < bytes; off += 16) {
                    aes_round128(vop, lhs + off, rhs + off, out + off);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 16);
            }
            if (vop == HB_VEC_SHA1NEXTE || vop == HB_VEC_SHA1MSG1 || vop == HB_VEC_SHA1MSG2 ||
                vop == HB_VEC_SHA1RNDS4 || vop == HB_VEC_SHA256RNDS2 ||
                vop == HB_VEC_SHA256MSG1 || vop == HB_VEC_SHA256MSG2) {
                /* SHA-NI extensions. Operate on 16 bytes (4×u32) per element. */
                if (bytes != 16) return HB_ERR_INTERNAL;
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, 16);
                if (r != HB_OK) return r;
                uint32_t s0[4], s1[4], d[4];
                /* DST dword layout per Intel spec (big-endian bit fields shown as
                 * little-endian dword index in our array):
                 *   s[0] = xmm[127:96], s[1] = xmm[95:64], s[2] = xmm[63:32], s[3] = xmm[31:0]
                 */
                for (unsigned k = 0; k < 4; k++) memcpy(&s0[k], lhs + (3 - k) * 4, 4);
                for (unsigned k = 0; k < 4; k++) memcpy(&s1[k], rhs + (3 - k) * 4, 4);
                switch (vop) {
                    case HB_VEC_SHA1NEXTE: {
                        /* TMP := (SRC1[127:96] ROL 30); DEST[127:96] := SRC2[127:96] + TMP;
                         * DEST[95:32] := SRC2[95:32]; DEST[31:0] := SRC2[31:0]. */
                        uint32_t a = s0[0];
                        uint32_t tmp = (a << 30) | (a >> 2);
                        d[0] = s1[0] + tmp;
                        d[1] = s1[1];
                        d[2] = s1[2];
                        d[3] = s1[3];
                        break;
                    }
                    case HB_VEC_SHA1MSG1: {
                        /* W0..W3 = SRC1 dwords; W4,W5 = SRC2[127:64].
                         * DEST[127:96] := W2 XOR W0;  DEST[95:64] := W3 XOR W1;
                         * DEST[63:32]  := W4 XOR W2;  DEST[31:0]   := W5 XOR W3. */
                        d[0] = s0[2] ^ s0[0];
                        d[1] = s0[3] ^ s0[1];
                        d[2] = s1[0] ^ s0[2];
                        d[3] = s1[1] ^ s0[3];
                        break;
                    }
                    case HB_VEC_SHA1MSG2: {
                        /* W13 := SRC2[95:64]; W14 := SRC2[63:32]; W15 := SRC2[31:0];
                         * W16 := (SRC1[127:96] XOR W13) ROL 1;
                         * W17 := (SRC1[95:64]  XOR W14) ROL 1;
                         * W18 := (SRC1[63:32]  XOR W15) ROL 1;
                         * W19 := (SRC1[31:0]   XOR W16) ROL 1;
                         * DEST[127:96] := W16; DEST[95:64] := W17;
                         * DEST[63:32]  := W18; DEST[31:0]  := W19. */
                        uint32_t w13 = s1[1], w14 = s1[2], w15 = s1[3];
                        uint32_t w16 = ((s0[0] ^ w13) << 1) | ((s0[0] ^ w13) >> 31);
                        uint32_t w17 = ((s0[1] ^ w14) << 1) | ((s0[1] ^ w14) >> 31);
                        uint32_t w18 = ((s0[2] ^ w15) << 1) | ((s0[2] ^ w15) >> 31);
                        uint32_t w19 = ((s0[3] ^ w16) << 1) | ((s0[3] ^ w16) >> 31);
                        d[0] = w16; d[1] = w17; d[2] = w18; d[3] = w19;
                        break;
                    }
                    case HB_VEC_SHA1RNDS4: {
                        /* A,B,C,D = SRC1[127:32]; W0E,W1,W2,W3 = SRC2 dwords.
                         * imm8[1:0] picks f()/K. Four rounds, then write A4,B4,C4,D4.
                         * Round 0: A_1 := f(B,C,D) + ROL(A,5) + W0E + K; E_1 := D.
                         * Round i (i=1..3): A_{i+1} := f(B_i,C_i,D_i) + ROL(A_i,5) + Wi + E_i + K;
                         *   B_{i+1} := A_i; C_{i+1} := ROL(B_i,30); D_{i+1} := C_i; E_{i+1} := D_i.
                         * Note: E_0 is implicit (the 5th state dword; carried in W0E
                         * for round 0). For rounds 1..3, E_i = D_{i-1}. */
                        static const uint32_t Ktab[4] = { 0x5A827999u, 0x6ED9EBA1u,
                                                          0x8F1BBCDCu, 0xCA62C1D6u };
                        unsigned k = imm & 0x3u;
                        uint32_t K = Ktab[k];
                        /* 5 slots: [0] = input, [1..4] = results of rounds 0..3. */
                        uint32_t Ast[5] = { s0[0], 0, 0, 0, 0 };
                        uint32_t Bst[5] = { s0[1], 0, 0, 0, 0 };
                        uint32_t Cst[5] = { s0[2], 0, 0, 0, 0 };
                        uint32_t Dst[5] = { s0[3], 0, 0, 0, 0 };
                        uint32_t Est[5] = { 0, 0, 0, 0, 0 };  /* Est[0] unused. */
                        uint32_t Ws[4]   = { s1[0], s1[1], s1[2], s1[3] };
                        for (int i = 0; i < 4; i++) {
                            uint32_t fi = (k == 0) ? sha_ch(Bst[i], Cst[i], Dst[i]) :
                                          (k == 1) ? sha_parity(Bst[i], Cst[i], Dst[i]) :
                                          (k == 2) ? sha_maj(Bst[i], Cst[i], Dst[i]) :
                                                     sha_parity(Bst[i], Cst[i], Dst[i]);
                            Ast[i + 1] = (i == 0) ? (fi + sha_rol(Ast[i], 5) + Ws[i] + K)
                                                  : (fi + sha_rol(Ast[i], 5) + Ws[i] + Est[i] + K);
                            Bst[i + 1] = Ast[i];
                            Cst[i + 1] = sha_rol(Bst[i], 30);
                            Dst[i + 1] = Cst[i];
                            Est[i + 1] = Dst[i];
                        }
                        /* Write A4, B4, C4, D4 (= Ast[4], Bst[4], Cst[4], Dst[4]). */
                        d[0] = Ast[4]; d[1] = Bst[4]; d[2] = Cst[4]; d[3] = Dst[4];
                        break;
                    }
                    case HB_VEC_SHA256MSG1: {
                        /* W4 := SRC2[31:0]; W3,W2,W1,W0 := SRC1[127:32];
                         * σ0(x) = ROR(x,7) XOR ROR(x,18) XOR (x >> 3);
                         * DEST[127:96] := W3 + σ0(W4);
                         * DEST[95:64]  := W2 + σ0(W3);
                         * DEST[63:32]  := W1 + σ0(W2);
                         * DEST[31:0]   := W0 + σ0(W1).
                         * In interp's Intel bit-numbered s0/s1:
                         *   s0[0]=SRC1[127:96]=W3, s0[1]=SRC1[95:64]=W2,
                         *   s0[2]=SRC1[63:32]=W1, s0[3]=SRC1[31:0]=W0.
                         *   s1[3]=SRC2[31:0]=W4.
                         * So d[0]=s0[3]+σ0(s1[3]), d[1]=s0[2]+σ0(s0[3]),
                         *    d[2]=s0[1]+σ0(s0[2]), d[3]=s0[0]+σ0(s0[1]). */
                        uint32_t w4 = s1[3];
                        d[0] = s0[3] + sha_smallsig0(w4);
                        d[1] = s0[2] + sha_smallsig0(s0[3]);
                        d[2] = s0[1] + sha_smallsig0(s0[2]);
                        d[3] = s0[0] + sha_smallsig0(s0[1]);
                        break;
                    }
                    case HB_VEC_SHA256MSG2: {
                        /* W14 := SRC2[95:64]; W15 := SRC2[127:96];
                         * W16 := SRC1[31:0]  + σ1(W14);
                         * W17 := SRC1[63:32] + σ1(W15);
                         * W18 := SRC1[95:64] + σ1(W16);
                         * W19 := SRC1[127:96] + σ1(W17);
                         * σ1(x) = ROR(x,17) XOR ROR(x,19) XOR (x >> 10);
                         * DEST[127:96] := W19; DEST[95:64] := W18;
                         * DEST[63:32]  := W17; DEST[31:0]  := W16. */
                        uint32_t w14 = s1[1], w15 = s1[0];
                        uint32_t w16 = s0[3] + sha_smallsig1(w14);
                        uint32_t w17 = s0[2] + sha_smallsig1(w15);
                        uint32_t w18 = s0[1] + sha_smallsig1(w16);
                        uint32_t w19 = s0[0] + sha_smallsig1(w17);
                        d[0] = w19; d[1] = w18; d[2] = w17; d[3] = w16;
                        break;
                    }
                    case HB_VEC_SHA256RNDS2: {
                        /* A0,B0 := SRC2[127:64]; C0,D0 := SRC1[127:64];
                         * E0,F0 := SRC2[63:32];  G0,H0 := SRC1[63:32];
                         * WK0 := XMM0[31:0]; WK1 := XMM0[63:32].
                         * For i in 0..1:
                         *   A_{i+1} := Ch(E_i,F_i,G_i) + Σ1(E_i) + WK_i + H_i + Maj(A_i,B_i,C_i) + Σ0(A_i);
                         *   B_{i+1} := A_i; C_{i+1} := B_i; D_{i+1} := C_i;
                         *   E_{i+1} := Ch(E_i,F_i,G_i) + Σ1(E_i) + WK_i + H_i + D_i;
                         *   F_{i+1} := E_i; G_{i+1} := F_i; H_{i+1} := G_i.
                         * DEST[127:96]:=A2; DEST[95:64]:=B2;
                         * DEST[63:32] :=E2; DEST[31:0]  :=F2. */
                        uint8_t xmm0[16];
                        r = read_vec_reg_bytes(ctx, HB_REG_XMM0, xmm0, 16);
                        if (r != HB_OK) return r;
                        uint32_t WK[2];
                        memcpy(&WK[0], xmm0 + 0, 4);  /* XMM0[31:0]  = LE dword 0 (offset 0) */
                        memcpy(&WK[1], xmm0 + 4, 4);  /* XMM0[63:32] = LE dword 1 (offset 4) */
                        uint32_t A = s1[0], B = s1[1], C = s0[0], D = s0[1];
                        uint32_t E = s1[2], F = s1[3], G = s0[2], H = s0[3];
                        for (int i = 0; i < 2; i++) {
                            uint32_t ch = sha_ch(E, F, G);
                            uint32_t s1e = sha_bigsig1(E);
                            uint32_t m = sha_maj(A, B, C);
                            uint32_t s0a = sha_bigsig0(A);
                            uint32_t An = ch + s1e + WK[i] + H + m + s0a;
                            uint32_t En = ch + s1e + WK[i] + H + D;
                            /* The new state for round i+1 uses OLD A..H, not the new An/En. */
                            uint32_t Aold = A, Bold = B, Cold = C;
                            uint32_t Eold = E, Fold = F, Gold = G;
                            A = An; B = Aold; C = Bold; D = Cold;
                            E = En; F = Eold; G = Fold; H = Gold;
                        }
                        d[0] = A; d[1] = B; d[2] = E; d[3] = F;
                        break;
                    }
                    default: return HB_ERR_UNSUPPORTED_OPCODE;
                }
                for (unsigned k = 0; k < 4; k++) memcpy(out + (3 - k) * 4, &d[k], 4);
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, 16);
            }
            if (vop == HB_VEC_GF2P8MULB) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                if (r != HB_OK) return r;
                for (size_t i = 0; i < bytes; i++) out[i] = aes_gmul(lhs[i], rhs[i]);
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 1);
            }
            if (vop == HB_VEC_GF2P8AFFINEQB || vop == HB_VEC_GF2P8AFFINEINVQB) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
                if (r != HB_OK) return r;
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                if (r != HB_OK) return r;
                for (size_t i = 0; i < bytes; i++) {
                    uint8_t value = lhs[i];
                    if (vop == HB_VEC_GF2P8AFFINEINVQB) value = gf2p8_inv(value);
                    out[i] = gf2p8_affine_byte(value, rhs + (i & ~(size_t)7), (uint8_t)imm);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }
            bool is_broadcast = vop == HB_VEC_VPBROADCASTB || vop == HB_VEC_VPBROADCASTW ||
                                vop == HB_VEC_VPBROADCASTD || vop == HB_VEC_VPBROADCASTQ ||
                                vop == HB_VEC_VBROADCASTSS || vop == HB_VEC_VBROADCASTSD ||
                                vop == HB_VEC_VBROADCASTF32X2 || vop == HB_VEC_VBROADCASTF64X2 ||
                                vop == HB_VEC_VBROADCASTF32X4 || vop == HB_VEC_VBROADCASTF64X4 ||
                                vop == HB_VEC_VBROADCASTF32X8 || vop == HB_VEC_VBROADCASTI32X2 ||
                                vop == HB_VEC_VBROADCASTI128;
            size_t src1_bytes = bytes;
            unsigned mask_lane = 0;
            if (is_broadcast) {
                src1_bytes = vop == HB_VEC_VPBROADCASTB ? 1 :
                             vop == HB_VEC_VPBROADCASTW ? 2 :
                             vop == HB_VEC_VPBROADCASTD ? 4 :
                             vop == HB_VEC_VPBROADCASTQ ? 8 :
                             vop == HB_VEC_VBROADCASTSS ? 4 :
                             vop == HB_VEC_VBROADCASTSD ? 8 :
                             vop == HB_VEC_VBROADCASTF32X2 ? 8 :
                             vop == HB_VEC_VBROADCASTF64X2 ? 16 :
                             vop == HB_VEC_VBROADCASTF32X4 ? 16 :
                             vop == HB_VEC_VBROADCASTF64X4 ? 32 :
                             vop == HB_VEC_VBROADCASTF32X8 ? 32 :
                             vop == HB_VEC_VBROADCASTI32X2 ? 8 : 16;
                mask_lane = vop == HB_VEC_VBROADCASTF32X2 ? 4 :
                            vop == HB_VEC_VBROADCASTF32X4 ? 4 :
                            vop == HB_VEC_VBROADCASTF32X8 ? 4 :
                            vop == HB_VEC_VBROADCASTI32X2 ? 4 :
                            vop == HB_VEC_VBROADCASTF64X2 ? 8 :
                            vop == HB_VEC_VBROADCASTF64X4 ? 8 :
                            (unsigned)src1_bytes;
            }
            if (instr->src1.type != HB_OP_NONE) {
                r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, src1_bytes);
                if (r != HB_OK) return r;
            }
            if (instr->src2.type != HB_OP_NONE) {
                r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
                if (r != HB_OK) return r;
            }

            if (is_broadcast) {
                unsigned lane = (unsigned)src1_bytes;
                if (vop == HB_VEC_VBROADCASTI128) {
                    for (size_t off = 0; off < bytes; off += 16) memcpy(out + off, lhs, 16);
                } else {
                    for (size_t off = 0; off < bytes; off += lane) memcpy(out + off, lhs, lane);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, mask_lane ? mask_lane : lane);
            }

            if (vop == HB_VEC_VMOVSLDUP || vop == HB_VEC_VMOVSHDUP ||
                vop == HB_VEC_VMOVDDUP) {
                for (size_t base = 0; base < bytes; base += 16) {
                    if (vop == HB_VEC_VMOVDDUP) {
                        memcpy(out + base, lhs + base, 8);
                        memcpy(out + base + 8, lhs + base, 8);
                    } else {
                        unsigned first = vop == HB_VEC_VMOVSHDUP ? 1 : 0;
                        memcpy(out + base, lhs + base + first * 4, 4);
                        memcpy(out + base + 4, lhs + base + first * 4, 4);
                        memcpy(out + base + 8, lhs + base + (first + 2) * 4, 4);
                        memcpy(out + base + 12, lhs + base + (first + 2) * 4, 4);
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_VPERMILPS || vop == HB_VEC_VPERMILPD) {
                unsigned lane = vop == HB_VEC_VPERMILPS ? 4 : 8;
                for (size_t block = 0; block < bytes; block += 16) {
                    unsigned lanes_per_block = 16 / lane;
                    for (unsigned j = 0; j < lanes_per_block; j++) {
                        unsigned sel;
                        if (instr->src2.type == HB_OP_NONE) {
                            sel = vop == HB_VEC_VPERMILPS
                                ? ((imm >> (2 * j)) & 3u)
                                : ((imm >> (j + (unsigned)(block / 16) * 2u)) & 1u);
                        } else if (vop == HB_VEC_VPERMILPS) {
                            uint32_t ctrl;
                            memcpy(&ctrl, rhs + block + j * lane, sizeof(ctrl));
                            sel = ctrl & 3u;
                        } else {
                            uint64_t ctrl;
                            memcpy(&ctrl, rhs + block + j * lane, sizeof(ctrl));
                            sel = (unsigned)((ctrl >> 1) & 1u);
                        }
                        memcpy(out + block + j * lane, lhs + block + sel * lane, lane);
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_VCMPPS || vop == HB_VEC_VCMPPD ||
                vop == HB_VEC_VCMPSS || vop == HB_VEC_VCMPSD) {
                bool is_pd = vop == HB_VEC_VCMPPD || vop == HB_VEC_VCMPSD;
                bool scalar = vop == HB_VEC_VCMPSS || vop == HB_VEC_VCMPSD;
                unsigned lane = is_pd ? 8 : 4;
                unsigned pred = imm & 31u;
                if (scalar) memcpy(out, lhs, 16);
                for (size_t off = 0; off < bytes; off += lane) {
                    if (scalar && off >= lane) break;
                    bool unordered, cmp = false;
                    if (is_pd) {
                        double a, b;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        unordered = isnan(a) || isnan(b);
                        switch (pred) {
                            case 0: case 16: cmp = !unordered && a == b; break;
                            case 1: case 17: cmp = !unordered && a < b; break;
                            case 2: case 18: cmp = !unordered && a <= b; break;
                            case 3: case 19: cmp = unordered; break;
                            case 4: case 20: cmp = unordered || a != b; break;
                            case 5: case 21: cmp = unordered || !(a < b); break;
                            case 6: case 22: cmp = unordered || !(a <= b); break;
                            case 7: case 23: cmp = !unordered; break;
                            case 8: case 24: cmp = unordered || a == b; break;
                            case 9: case 25: cmp = unordered || !(a >= b); break;
                            case 10: case 26: cmp = unordered || !(a > b); break;
                            case 11: case 27: cmp = false; break;
                            case 12: case 28: cmp = !unordered && a != b; break;
                            case 13: case 29: cmp = !unordered && a >= b; break;
                            case 14: case 30: cmp = !unordered && a > b; break;
                            case 15: case 31: cmp = true; break;
                        }
                    } else {
                        float a, b;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        unordered = isnan(a) || isnan(b);
                        switch (pred) {
                            case 0: case 16: cmp = !unordered && a == b; break;
                            case 1: case 17: cmp = !unordered && a < b; break;
                            case 2: case 18: cmp = !unordered && a <= b; break;
                            case 3: case 19: cmp = unordered; break;
                            case 4: case 20: cmp = unordered || a != b; break;
                            case 5: case 21: cmp = unordered || !(a < b); break;
                            case 6: case 22: cmp = unordered || !(a <= b); break;
                            case 7: case 23: cmp = !unordered; break;
                            case 8: case 24: cmp = unordered || a == b; break;
                            case 9: case 25: cmp = unordered || !(a >= b); break;
                            case 10: case 26: cmp = unordered || !(a > b); break;
                            case 11: case 27: cmp = false; break;
                            case 12: case 28: cmp = !unordered && a != b; break;
                            case 13: case 29: cmp = !unordered && a >= b; break;
                            case 14: case 30: cmp = !unordered && a > b; break;
                            case 15: case 31: cmp = true; break;
                        }
                    }
                    memset(out + off, cmp ? 0xff : 0x00, lane);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, scalar ? 16 : bytes);
            }

            if (vop == HB_VEC_VADDSUBPS || vop == HB_VEC_VADDSUBPD) {
                bool is_pd = vop == HB_VEC_VADDSUBPD;
                unsigned lane = is_pd ? 8 : 4;
                for (size_t off = 0; off < bytes; off += lane) {
                    bool add = ((off / lane) & 1u) != 0;
                    if (is_pd) {
                        uint64_t a, b, c;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        c = hb_sse_arith_double_bits(a, b, add ? HB_IR_FADD : HB_IR_FSUB);
                        memcpy(out + off, &c, sizeof(c));
                    } else {
                        uint32_t a, b, c;
                        memcpy(&a, lhs + off, sizeof(a));
                        memcpy(&b, rhs + off, sizeof(b));
                        c = hb_sse_arith_float_bits(a, b, add ? HB_IR_FADD : HB_IR_FSUB);
                        memcpy(out + off, &c, sizeof(c));
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_VHADDPS || vop == HB_VEC_VHADDPD ||
                vop == HB_VEC_VHSUBPS || vop == HB_VEC_VHSUBPD) {
                bool is_pd = vop == HB_VEC_VHADDPD || vop == HB_VEC_VHSUBPD;
                bool sub = vop == HB_VEC_VHSUBPS || vop == HB_VEC_VHSUBPD;
                hb_ir_op_t op = sub ? HB_IR_FSUB : HB_IR_FADD;
                for (size_t base = 0; base < bytes; base += 16) {
                    if (is_pd) {
                        uint64_t a, b, c;
                        memcpy(&a, lhs + base, sizeof(a));
                        memcpy(&b, lhs + base + 8, sizeof(b));
                        c = hb_sse_arith_double_bits(a, b, op);
                        memcpy(out + base, &c, sizeof(c));
                        memcpy(&a, rhs + base, sizeof(a));
                        memcpy(&b, rhs + base + 8, sizeof(b));
                        c = hb_sse_arith_double_bits(a, b, op);
                        memcpy(out + base + 8, &c, sizeof(c));
                    } else {
                        for (unsigned pair = 0; pair < 2; pair++) {
                            uint32_t a, b, c;
                            memcpy(&a, lhs + base + pair * 8, sizeof(a));
                            memcpy(&b, lhs + base + pair * 8 + 4, sizeof(b));
                            c = hb_sse_arith_float_bits(a, b, op);
                            memcpy(out + base + pair * 4, &c, sizeof(c));
                            memcpy(&a, rhs + base + pair * 8, sizeof(a));
                            memcpy(&b, rhs + base + pair * 8 + 4, sizeof(b));
                            c = hb_sse_arith_float_bits(a, b, op);
                            memcpy(out + base + 8 + pair * 4, &c, sizeof(c));
                        }
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_PTEST || vop == HB_VEC_VTESTPS || vop == HB_VEC_VTESTPD) {
                bool zf = true, cf = true;
                unsigned lane = vop == HB_VEC_VTESTPD ? 8 : 4;
                if (vop == HB_VEC_PTEST) lane = 1;
                for (size_t i = 0; i < bytes; i += lane) {
                    uint8_t a = lhs[i + lane - 1];
                    uint8_t b = rhs[i + lane - 1];
                    if (vop == HB_VEC_PTEST) {
                        if ((a & b) != 0) zf = false;
                        if (((uint8_t)~a & b) != 0) cf = false;
                    } else {
                        if ((a & b & 0x80u) != 0) zf = false;
                        if (((uint8_t)~a & b & 0x80u) != 0) cf = false;
                    }
                }
                hb_lazy_flags_clear(ctx);
                ctx->flags.zf = zf;
                ctx->flags.cf = cf;
                ctx->flags.of = ctx->flags.sf = ctx->flags.af = ctx->flags.pf = false;
                return HB_OK;
            }

            if (vop == HB_VEC_PHADDW || vop == HB_VEC_PHADDD || vop == HB_VEC_PHADDSW ||
                vop == HB_VEC_PHSUBW || vop == HB_VEC_PHSUBD || vop == HB_VEC_PHSUBSW) {
                unsigned lane = (vop == HB_VEC_PHADDW || vop == HB_VEC_PHADDSW ||
                                 vop == HB_VEC_PHSUBW || vop == HB_VEC_PHSUBSW) ? 2 : 4;
                bool sub = (vop == HB_VEC_PHSUBW || vop == HB_VEC_PHSUBD || vop == HB_VEC_PHSUBSW);
                bool sat = (vop == HB_VEC_PHADDSW || vop == HB_VEC_PHSUBSW);
                size_t lane_bytes = bytes < 16 ? 8 : 16;
                for (size_t base = 0; base < bytes; base += lane_bytes) {
                    unsigned pairs_per_src = (unsigned)(lane_bytes / (2 * lane));
                    for (unsigned s = 0; s < 2; s++) {
                        const uint8_t* src = s ? rhs + base : lhs + base;
                        for (unsigned p = 0; p < pairs_per_src; p++) {
                            int64_t a = load_lane_signed(src + p * 2 * lane, lane);
                            int64_t b = load_lane_signed(src + (p * 2 + 1) * lane, lane);
                            int64_t val = sub ? (a - b) : (a + b);
                            size_t off = base + (s * pairs_per_src + p) * lane;
                            if (sat) store_lane(out + off, lane, (uint16_t)sat_i16((int32_t)val));
                            else store_lane(out + off, lane, (uint64_t)val);
                        }
                    }
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, lane);
            }

            if (vop == HB_VEC_PMADDUBSW) {
                for (size_t i = 0; i < bytes; i += 2) {
                    int32_t v = (int32_t)lhs[i] * (int32_t)(int8_t)rhs[i] +
                                (int32_t)lhs[i + 1] * (int32_t)(int8_t)rhs[i + 1];
                    store_lane(out + i, 2, (uint16_t)sat_i16(v));
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 2);
            }

            if (vop == HB_VEC_PSIGNB || vop == HB_VEC_PSIGNW || vop == HB_VEC_PSIGND ||
                vop == HB_VEC_PABSB || vop == HB_VEC_PABSW || vop == HB_VEC_PABSD) {
                unsigned lane = (vop == HB_VEC_PSIGNB || vop == HB_VEC_PABSB) ? 1 :
                                (vop == HB_VEC_PSIGNW || vop == HB_VEC_PABSW) ? 2 : 4;
                for (size_t i = 0; i < bytes; i += lane) {
                    int64_t a = load_lane_signed(lhs + i, lane);
                    int64_t sign = (vop == HB_VEC_PSIGNB || vop == HB_VEC_PSIGNW || vop == HB_VEC_PSIGND)
                                       ? load_lane_signed(rhs + i, lane) : 1;
                    int64_t val = sign == 0 ? 0 : (sign < 0 ? -a : (a < 0 && vop >= HB_VEC_PABSB && vop <= HB_VEC_PABSD ? -a : a));
                    store_lane(out + i, lane, (uint64_t)val);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, lane);
            }

            if (vop == HB_VEC_PMULHRSW) {
                for (size_t i = 0; i < bytes; i += 2) {
                    int16_t a, b;
                    memcpy(&a, lhs + i, sizeof(a));
                    memcpy(&b, rhs + i, sizeof(b));
                    int32_t v = ((int32_t)a * (int32_t)b + 0x4000) >> 15;
                    store_lane(out + i, 2, (uint16_t)v);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 2);
            }

            if ((vop >= HB_VEC_PMOVSXBW && vop <= HB_VEC_PMOVSXDQ) ||
                (vop >= HB_VEC_PMOVZXBW && vop <= HB_VEC_PMOVZXDQ)) {
                bool sign = vop >= HB_VEC_PMOVSXBW && vop <= HB_VEC_PMOVSXDQ;
                unsigned src_lane = (vop == HB_VEC_PMOVSXBW || vop == HB_VEC_PMOVSXBD ||
                                     vop == HB_VEC_PMOVSXBQ || vop == HB_VEC_PMOVZXBW ||
                                     vop == HB_VEC_PMOVZXBD || vop == HB_VEC_PMOVZXBQ) ? 1 :
                                    (vop == HB_VEC_PMOVSXWD || vop == HB_VEC_PMOVSXWQ ||
                                     vop == HB_VEC_PMOVZXWD || vop == HB_VEC_PMOVZXWQ) ? 2 : 4;
                unsigned dst_lane = (vop == HB_VEC_PMOVSXBW || vop == HB_VEC_PMOVZXBW) ? 2 :
                                    (vop == HB_VEC_PMOVSXBD || vop == HB_VEC_PMOVSXWD ||
                                     vop == HB_VEC_PMOVZXBD || vop == HB_VEC_PMOVZXWD) ? 4 : 8;
                unsigned count = (unsigned)(bytes / dst_lane);
                for (unsigned i = 0; i < count; i++) {
                    uint64_t val = sign ? (uint64_t)load_lane_signed(lhs + i * src_lane, src_lane)
                                        : load_lane_unsigned(lhs + i * src_lane, src_lane);
                    store_lane(out + i * dst_lane, dst_lane, val);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, dst_lane);
            }

            if (vop == HB_VEC_PMULDQ) {
                for (size_t i = 0, o = 0; i + 4 <= bytes; i += 8, o += 8) {
                    int32_t a, b;
                    memcpy(&a, lhs + i, sizeof(a));
                    memcpy(&b, rhs + i, sizeof(b));
                    int64_t v = (int64_t)a * (int64_t)b;
                    memcpy(out + o, &v, sizeof(v));
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 8);
            }

            if (vop == HB_VEC_PCMPEQQ || vop == HB_VEC_PCMPGTQ) {
                for (size_t i = 0; i < bytes; i += 8) {
                    int64_t a, b;
                    memcpy(&a, lhs + i, sizeof(a));
                    memcpy(&b, rhs + i, sizeof(b));
                    if ((vop == HB_VEC_PCMPEQQ && a == b) || (vop == HB_VEC_PCMPGTQ && a > b))
                        memset(out + i, 0xff, 8);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 8);
            }

            if (vop == HB_VEC_PACKUSDW) {
                for (size_t base = 0; base < bytes; base += 16) {
                    for (unsigned i = 0; i < 4; i++) {
                        int32_t v;
                        memcpy(&v, lhs + base + i * 4, sizeof(v));
                        store_lane(out + base + i * 2, 2, sat_u16_from_i32(v));
                        memcpy(&v, rhs + base + i * 4, sizeof(v));
                        store_lane(out + base + 8 + i * 2, 2, sat_u16_from_i32(v));
                    }
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 2);
            }

            if (vop == HB_VEC_PSUBUSB || vop == HB_VEC_PSUBUSW ||
                vop == HB_VEC_PSUBSB || vop == HB_VEC_PSUBSW) {
                bool is_unsigned = vop == HB_VEC_PSUBUSB || vop == HB_VEC_PSUBUSW;
                unsigned lane = (vop == HB_VEC_PSUBUSB || vop == HB_VEC_PSUBSB) ? 1 : 2;
                for (size_t i = 0; i < bytes; i += lane) {
                    if (is_unsigned) {
                        uint64_t a = load_lane_unsigned(lhs + i, lane);
                        uint64_t b = load_lane_unsigned(rhs + i, lane);
                        store_lane(out + i, lane, a > b ? a - b : 0);
                    } else if (lane == 1) {
                        int32_t v = (int32_t)(int8_t)lhs[i] - (int32_t)(int8_t)rhs[i];
                        out[i] = (uint8_t)sat_i8(v);
                    } else {
                        int16_t a, b;
                        memcpy(&a, lhs + i, sizeof(a));
                        memcpy(&b, rhs + i, sizeof(b));
                        store_lane(out + i, lane, (uint16_t)sat_i16((int32_t)a - (int32_t)b));
                    }
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, lane);
            }

            if ((vop >= HB_VEC_PMINSB && vop <= HB_VEC_PMULLD) ||
                vop == HB_VEC_PMINUB || vop == HB_VEC_PMINSW ||
                vop == HB_VEC_PMAXUB || vop == HB_VEC_PMAXSW) {
                bool is_max = vop == HB_VEC_PMAXSB || vop == HB_VEC_PMAXSD ||
                              vop == HB_VEC_PMAXUW || vop == HB_VEC_PMAXUD ||
                              vop == HB_VEC_PMAXUB || vop == HB_VEC_PMAXSW;
                bool is_mul = vop == HB_VEC_PMULLD;
                bool is_signed = vop == HB_VEC_PMINSB || vop == HB_VEC_PMINSD ||
                                 vop == HB_VEC_PMAXSB || vop == HB_VEC_PMAXSD ||
                                 vop == HB_VEC_PMINSW || vop == HB_VEC_PMAXSW;
                unsigned lane = (vop == HB_VEC_PMINSB || vop == HB_VEC_PMAXSB ||
                                 vop == HB_VEC_PMINUB || vop == HB_VEC_PMAXUB) ? 1 :
                                (vop == HB_VEC_PMINUW || vop == HB_VEC_PMAXUW ||
                                 vop == HB_VEC_PMINSW || vop == HB_VEC_PMAXSW) ? 2 : 4;
                for (size_t i = 0; i < bytes; i += lane) {
                    uint64_t val;
                    if (is_mul) {
                        int32_t a, b, c;
                        memcpy(&a, lhs + i, sizeof(a));
                        memcpy(&b, rhs + i, sizeof(b));
                        c = a * b;
                        val = (uint32_t)c;
                    } else if (is_signed) {
                        int64_t a = load_lane_signed(lhs + i, lane);
                        int64_t b = load_lane_signed(rhs + i, lane);
                        val = (uint64_t)(is_max ? (a > b ? a : b) : (a < b ? a : b));
                    } else {
                        uint64_t a = load_lane_unsigned(lhs + i, lane);
                        uint64_t b = load_lane_unsigned(rhs + i, lane);
                        val = is_max ? (a > b ? a : b) : (a < b ? a : b);
                    }
                    store_lane(out + i, lane, val);
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, lane);
            }

            if (vop == HB_VEC_PMULUDQ) {
                for (size_t base = 0; base < bytes; base += 16) {
                    for (unsigned i = 0; i < 2; i++) {
                        uint32_t a, b;
                        size_t src_off = base + i * 8;
                        memcpy(&a, lhs + src_off, sizeof(a));
                        memcpy(&b, rhs + src_off, sizeof(b));
                        uint64_t v = (uint64_t)a * (uint64_t)b;
                        memcpy(out + base + i * 8, &v, sizeof(v));
                    }
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 8);
            }

            if (vop == HB_VEC_PSADBW) {
                for (size_t base = 0; base < bytes; base += 8) {
                    uint64_t sum = 0;
                    for (unsigned i = 0; i < 8; i++) {
                        int diff = (int)lhs[base + i] - (int)rhs[base + i];
                        sum += (uint64_t)(diff < 0 ? -diff : diff);
                    }
                    memcpy(out + base, &sum, sizeof(sum));
                }
                return write_vec_reg_bytes_evex_masked(ctx, instr, out, bytes, 8);
            }

            if (vop == HB_VEC_MPSADBW) {
                for (size_t base = 0; base < bytes; base += 16) {
                    unsigned lhs_base = ((imm >> 2) & 1u) * 4u;
                    unsigned rhs_base = (imm & 3u) * 4u;
                    for (unsigned j = 0; j < 8; j++) {
                        uint16_t sum = 0;
                        for (unsigned k = 0; k < 4; k++) {
                            int diff = (int)lhs[base + lhs_base + j + k] -
                                       (int)rhs[base + rhs_base + k];
                            sum = (uint16_t)(sum + (uint16_t)(diff < 0 ? -diff : diff));
                        }
                        memcpy(out + base + j * 2, &sum, sizeof(sum));
                    }
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_PHMINPOSUW) {
                uint16_t minv = 0xffff, mini = 0;
                for (uint16_t i = 0; i < 8; i++) {
                    uint16_t v;
                    memcpy(&v, lhs + i * 2, sizeof(v));
                    if (v < minv) { minv = v; mini = i; }
                }
                memset(out, 0, bytes);
                memcpy(out, &minv, sizeof(minv));
                memcpy(out + 2, &mini, sizeof(mini));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_PALIGNR) {
                size_t lane_bytes = bytes < 16 ? 8 : 16;
                for (size_t base = 0; base < bytes; base += lane_bytes) {
                    uint8_t cat[32] = {0};
                    memcpy(cat, rhs + base, lane_bytes);
                    memcpy(cat + lane_bytes, lhs + base, lane_bytes);
                    for (size_t i = 0; i < lane_bytes; i++)
                        out[base + i] = (imm + i < lane_bytes * 2) ? cat[imm + i] : 0;
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_PBLENDW || vop == HB_VEC_BLENDPS || vop == HB_VEC_BLENDPD ||
                vop == HB_VEC_VPBLENDD) {
                unsigned lane = (vop == HB_VEC_PBLENDW) ? 2 :
                                (vop == HB_VEC_BLENDPD ? 8 : 4);
                memcpy(out, lhs, bytes);
                for (unsigned i = 0; i < bytes / lane; i++)
                    if ((imm >> (i & 7)) & 1u) memcpy(out + i * lane, rhs + i * lane, lane);
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_PBLENDVB || vop == HB_VEC_BLENDVPS || vop == HB_VEC_BLENDVPD) {
                uint8_t mask[32] = {0};
                unsigned lane = vop == HB_VEC_PBLENDVB ? 1 : (vop == HB_VEC_BLENDVPS ? 4 : 8);
                r = read_vec_reg_bytes(ctx, HB_REG_XMM0, mask, bytes);
                if (r != HB_OK) return r;
                memcpy(out, lhs, bytes);
                for (unsigned i = 0; i < bytes; i += lane) {
                    if (mask[i + lane - 1] & 0x80) memcpy(out + i, rhs + i, lane);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            if (vop == HB_VEC_VPBLENDVB || vop == HB_VEC_VBLENDVPS || vop == HB_VEC_VBLENDVPD) {
                uint8_t mask[32] = {0};
                unsigned lane = vop == HB_VEC_VPBLENDVB ? 1 : (vop == HB_VEC_VBLENDVPS ? 4 : 8);
                int mask_reg = HB_REG_XMM0 + ((imm >> 4) & 0x0f);
                r = read_vec_reg_bytes(ctx, mask_reg, mask, bytes);
                if (r != HB_OK) return r;
                memcpy(out, lhs, bytes);
                for (unsigned i = 0; i < bytes; i += lane) {
                    if (mask[i + lane - 1] & 0x80) memcpy(out + i, rhs + i, lane);
                }
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
            }

            return HB_ERR_UNSUPPORTED_OPCODE;
        }

        case HB_IR_VZEROUPPER: {
            memset(ctx->ymm_hi, 0, sizeof(ctx->ymm_hi));
            return HB_OK;
        }

        case HB_IR_MOVD: {
            if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg) &&
                instr->src1.type == HB_OP_REG && is_xmm_reg(instr->src1.reg)) {
                uint64_t xmm[2];
                uint64_t out[2] = {0, 0};
                r = read_xmm_reg(ctx, instr->src1.reg, xmm);
                if (r != HB_OK) return r;
                out[0] = xmm[0];
                return write_vec_reg_bytes(ctx, instr->dst.reg, (const uint8_t*)out, 16);
            }
            if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg)) {
                uint64_t raw = 0;
                r = read_operand_value(ctx, &instr->src1, &raw);
                if (r != HB_OK) return r;
                uint64_t out[2] = { instr->src1.size == HB_SIZE_64 ? raw : (uint32_t)raw, 0 };
                return write_vec_reg_bytes(ctx, instr->dst.reg, (const uint8_t*)out, 16);
            }
            if (instr->src1.type == HB_OP_REG && is_xmm_reg(instr->src1.reg)) {
                uint64_t xmm[2];
                uint64_t low;
                r = read_xmm_reg(ctx, instr->src1.reg, xmm);
                if (r != HB_OK) return r;
                low = instr->dst.size == HB_SIZE_64 ? xmm[0] : (uint32_t)xmm[0];
                if (instr->dst.type == HB_OP_REG) {
                    write_reg_sized(ctx, instr->dst.reg, low, instr->dst.size == HB_SIZE_64 ? HB_SIZE_64 : HB_SIZE_32);
                    return HB_OK;
                }
                if (instr->dst.type == HB_OP_MEM) {
                    uint64_t addr = resolve_addr(ctx, &instr->dst);
                    return mem_write(ctx, addr, low, instr->dst.size == HB_SIZE_64 ? HB_SIZE_64 : HB_SIZE_32);
                }
            }
            return HB_ERR_INTERNAL;
        }

        case HB_IR_CVTDQ2PD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            if (bytes != 16 && bytes != 32) return HB_ERR_INTERNAL;
            uint8_t in[16] = {0}, out[32] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, bytes / 2);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes / 8; i++) {
                int32_t v;
                uint64_t bits;
                memcpy(&v, in + i * 4, sizeof(v));
                bits = hb_double_to_bits((double)v);
                memcpy(out + i * 8, &bits, sizeof(bits));
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
        }

        case HB_IR_CVTDQ2PS:
        case HB_IR_CVTPS2DQ:
        case HB_IR_CVTTPS2DQ: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            if (bytes != 16 && bytes != 32) return HB_ERR_INTERNAL;
            uint8_t ibytes[32] = {0}, obytes[32] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, ibytes, bytes);
            if (r != HB_OK) return r;
            if (instr->op == HB_IR_CVTDQ2PS) {
                for (unsigned i = 0; i < bytes / 4; i++) {
                    int32_t v;
                    float f;
                    uint32_t bits;
                    memcpy(&v, ibytes + i * 4, sizeof(v));
                    f = (float)v;
                    bits = hb_float_to_bits(f);
                    memcpy(obytes + i * 4, &bits, sizeof(bits));
                }
            } else {
                bool truncate = instr->op == HB_IR_CVTTPS2DQ;
                for (unsigned i = 0; i < bytes / 4; i++) {
                    uint32_t bits;
                    float f;
                    int32_t v;
                    memcpy(&bits, ibytes + i * 4, sizeof(bits));
                    f = hb_bits_to_float(bits);
                    v = hb_float_to_i32_sse(f, truncate);
                    memcpy(obytes + i * 4, &v, sizeof(v));
                }
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, obytes, bytes);
        }

        case HB_IR_CVTPS2PD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            if (bytes != 16 && bytes != 32) return HB_ERR_INTERNAL;
            uint8_t in[16] = {0}, out[32] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, bytes / 2);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < bytes / 8; i++) {
                uint32_t bits;
                uint64_t dbits;
                memcpy(&bits, in + i * 4, sizeof(bits));
                dbits = hb_double_to_bits((double)hb_bits_to_float(bits));
                memcpy(out + i * 8, &dbits, sizeof(dbits));
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, out, bytes);
        }

        case HB_IR_CVTPD2PS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t src_bytes = bytes_for_size(instr->src1.size);
            if (src_bytes == 0) src_bytes = 16;
            if (src_bytes != 16 && src_bytes != 32) return HB_ERR_INTERNAL;
            uint8_t in[32] = {0};
            uint8_t obytes[16] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, src_bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < src_bytes / 8; i++) {
                uint64_t bits;
                uint32_t fbits;
                memcpy(&bits, in + i * 8, sizeof(bits));
                fbits = hb_float_to_bits((float)hb_bits_to_double(bits));
                memcpy(obytes + i * 4, &fbits, sizeof(fbits));
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, obytes, 16);
        }

        case HB_IR_CVTPD2DQ:
        case HB_IR_CVTTPD2DQ: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            size_t src_bytes = bytes_for_size(instr->src1.size);
            if (src_bytes == 0) src_bytes = 16;
            if (src_bytes != 16 && src_bytes != 32) return HB_ERR_INTERNAL;
            uint8_t in[32] = {0};
            uint8_t obytes[16] = {0};
            bool truncate = instr->op == HB_IR_CVTTPD2DQ;
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, src_bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < src_bytes / 8; i++) {
                uint64_t bits;
                double d;
                int32_t v;
                memcpy(&bits, in + i * 8, sizeof(bits));
                d = hb_bits_to_double(bits);
                v = hb_double_to_i32_sse(d, truncate);
                memcpy(obytes + i * 4, &v, sizeof(v));
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, obytes, 16);
        }

        case HB_IR_CVTSS2SD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            float value = 0.0f;
            const hb_ir_operand_t *value_src = instr->src2.type == HB_OP_NONE ? &instr->src1 : &instr->src2;
            r = read_scalar_float(ctx, value_src, &value);
            if (r != HB_OK) return r;
            if (instr->src2.type != HB_OP_NONE) {
                uint8_t out[16] = {0};
                uint64_t bits = hb_double_to_bits((double)value);
                r = read_xmm_operand_bytes(ctx, &instr->src1, out, sizeof(out));
                if (r != HB_OK) return r;
                memcpy(out, &bits, sizeof(bits));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, sizeof(out));
            }
            return write_scalar_double(ctx, &instr->dst, (double)value);
        }

        case HB_IR_CVTSD2SS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            double value = 0.0;
            const hb_ir_operand_t *value_src = instr->src2.type == HB_OP_NONE ? &instr->src1 : &instr->src2;
            r = read_scalar_double(ctx, value_src, &value);
            if (r != HB_OK) return r;
            if (instr->src2.type != HB_OP_NONE) {
                uint8_t out[16] = {0};
                uint32_t bits = hb_float_to_bits((float)value);
                r = read_xmm_operand_bytes(ctx, &instr->src1, out, sizeof(out));
                if (r != HB_OK) return r;
                memcpy(out, &bits, sizeof(bits));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, sizeof(out));
            }
            return write_scalar_float(ctx, &instr->dst, (float)value);
        }

        case HB_IR_CVTSI2SD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t raw = 0;
            const hb_ir_operand_t *int_src = instr->src2.type == HB_OP_NONE ? &instr->src1 : &instr->src2;
            r = read_operand_value(ctx, int_src, &raw);
            if (r != HB_OK) return r;
            double value = (int_src->size == HB_SIZE_64)
                ? (double)(int64_t)raw
                : (double)(int32_t)(uint32_t)raw;
            if (instr->src2.type != HB_OP_NONE) {
                uint8_t out[16] = {0};
                uint64_t bits = hb_double_to_bits(value);
                r = read_xmm_operand_bytes(ctx, &instr->src1, out, sizeof(out));
                if (r != HB_OK) return r;
                memcpy(out, &bits, sizeof(bits));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, sizeof(out));
            }
            return write_scalar_double(ctx, &instr->dst, value);
        }

        case HB_IR_CVTSI2SS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t raw = 0;
            const hb_ir_operand_t *int_src = instr->src2.type == HB_OP_NONE ? &instr->src1 : &instr->src2;
            r = read_operand_value(ctx, int_src, &raw);
            if (r != HB_OK) return r;
            float value = (int_src->size == HB_SIZE_64)
                ? (float)(int64_t)raw
                : (float)(int32_t)(uint32_t)raw;
            if (instr->src2.type != HB_OP_NONE) {
                uint8_t out[16] = {0};
                uint32_t bits = hb_float_to_bits(value);
                r = read_xmm_operand_bytes(ctx, &instr->src1, out, sizeof(out));
                if (r != HB_OK) return r;
                memcpy(out, &bits, sizeof(bits));
                return write_vec_reg_bytes(ctx, instr->dst.reg, out, sizeof(out));
            }
            return write_scalar_float(ctx, &instr->dst, value);
        }

        case HB_IR_DIVSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs = 0, rhs = 0;
            r = read_scalar_double_bits(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double_bits(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double_bits(ctx, &instr->dst, hb_sse_arith_double_bits(lhs, rhs, HB_IR_FDIV));
        }

        case HB_IR_ADDSD:
        case HB_IR_SUBSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs = 0, rhs = 0;
            r = read_scalar_double_bits(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double_bits(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double_bits(ctx, &instr->dst,
                                            hb_sse_arith_double_bits(lhs, rhs,
                                                                     instr->op == HB_IR_ADDSD ? HB_IR_FADD : HB_IR_FSUB));
        }

        case HB_IR_MULSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs = 0, rhs = 0;
            r = read_scalar_double_bits(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double_bits(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double_bits(ctx, &instr->dst, hb_sse_arith_double_bits(lhs, rhs, HB_IR_FMUL));
        }

        case HB_IR_MULSS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint32_t lhs = 0, rhs = 0;
            r = read_scalar_float_bits(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_float_bits(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_float_bits(ctx, &instr->dst, hb_sse_arith_float_bits(lhs, rhs, HB_IR_FMUL));
        }

        case HB_IR_DIVSS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint32_t lhs = 0, rhs = 0;
            r = read_scalar_float_bits(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_float_bits(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_float_bits(ctx, &instr->dst, hb_sse_arith_float_bits(lhs, rhs, HB_IR_FDIV));
        }

        case HB_IR_FSQRT:
        case HB_IR_FRSQRT:
        case HB_IR_FRCP: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            unsigned arg = evex_target_arg(instr);
            bool scalar = (arg & 0x100) != 0;
            unsigned lane = arg & 0xffu;
            size_t bytes = scalar ? 16 : bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t src[64], out_bytes[64];
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            if (instr->op != HB_IR_FSQRT && lane != 4) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, out_bytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, src, scalar ? lane : bytes);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < (scalar ? 1U : (unsigned)(bytes / lane)); i++) {
                if (lane == 4) {
                    uint32_t bits, result_bits;
                    memcpy(&bits, src + i * 4, sizeof(bits));
                    float value = hb_bits_to_float(bits);
                    float result = sqrtf(value);
                    if (instr->op == HB_IR_FRSQRT) result = 1.0f / result;
                    else if (instr->op == HB_IR_FRCP) result = 1.0f / value;
                    result_bits = hb_float_to_bits(result);
                    memcpy(out_bytes + i * 4, &result_bits, sizeof(result_bits));
                } else {
                    uint64_t bits, result_bits;
                    memcpy(&bits, src + i * 8, sizeof(bits));
                    result_bits = hb_double_to_bits(sqrt(hb_bits_to_double(bits)));
                    memcpy(out_bytes + i * 8, &result_bits, sizeof(result_bits));
                }
            }
            if (scalar)
                return write_vec_reg_bytes_evex_scalar_masked(ctx, instr, out_bytes, lane);
            return write_vec_reg_bytes_evex_masked(ctx, instr, out_bytes, bytes, lane);
        }

        case HB_IR_FROUND: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            unsigned arg = evex_target_arg(instr);
            bool scalar = (arg & 0x100) != 0;
            unsigned lane = arg & 0xffu;
            unsigned imm = (unsigned)((instr->target >> 16) & 0xff);
            unsigned mode = (imm & 0x04u) ? 0u : (imm & 0x03u);
            size_t bytes = scalar ? 16 : bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            if (!(lane == 4 || lane == 8) || (bytes != 16 && bytes != 32)) return HB_ERR_INTERNAL;
            uint8_t out_bytes[32] = {0}, src[32] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, out_bytes, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, src, scalar ? lane : bytes);
            if (r != HB_OK) return r;
            for (unsigned off = 0; off < (scalar ? lane : (unsigned)bytes); off += lane) {
                if (lane == 4) {
                    uint32_t bits, rbits;
                    float value, rounded;
                    memcpy(&bits, src + off, sizeof(bits));
                    value = hb_bits_to_float(bits);
                    if (mode == 1) rounded = floorf(value);
                    else if (mode == 2) rounded = ceilf(value);
                    else if (mode == 3) rounded = truncf(value);
                    else rounded = nearbyintf(value);
                    rbits = hb_float_to_bits(rounded);
                    memcpy(out_bytes + off, &rbits, sizeof(rbits));
                } else {
                    uint64_t bits, rbits;
                    double value, rounded;
                    memcpy(&bits, src + off, sizeof(bits));
                    value = hb_bits_to_double(bits);
                    if (mode == 1) rounded = floor(value);
                    else if (mode == 2) rounded = ceil(value);
                    else if (mode == 3) rounded = trunc(value);
                    else rounded = nearbyint(value);
                    rbits = hb_double_to_bits(rounded);
                    memcpy(out_bytes + off, &rbits, sizeof(rbits));
                }
            }
            if (scalar)
                return write_vec_reg_bytes_evex_scalar_masked(ctx, instr, out_bytes, lane);
            return write_vec_reg_bytes_evex_masked(ctx, instr, out_bytes, bytes, lane);
        }

        case HB_IR_FDP: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            unsigned lane = (unsigned)(instr->target & 0xff);
            unsigned imm = (unsigned)((instr->target >> 16) & 0xff);
            size_t bytes = bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            if (!(lane == 4 || lane == 8) || (bytes != 16 && bytes != 32) || (lane == 8 && bytes != 16)) {
                return HB_ERR_INTERNAL;
            }
            uint8_t lhs[32] = {0}, rhs[32] = {0}, out_bytes[32] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, bytes);
            if (r != HB_OK) return r;
            for (unsigned base = 0; base < bytes; base += 16) {
                if (lane == 4) {
                    uint32_t prod[4] = {0, 0, 0, 0};
                    for (unsigned i = 0; i < 4; i++) {
                        if (imm & (1u << i)) {
                            uint32_t a, b;
                            memcpy(&a, lhs + base + i * 4, sizeof(a));
                            memcpy(&b, rhs + base + i * 4, sizeof(b));
                            prod[i] = hb_sse_arith_float_bits(a, b, HB_IR_FMUL);
                        }
                    }
                    uint32_t sum01 = hb_sse_arith_float_bits(prod[0], prod[1], HB_IR_FADD);
                    uint32_t sum23 = hb_sse_arith_float_bits(prod[2], prod[3], HB_IR_FADD);
                    uint32_t sum = hb_sse_arith_float_bits(sum01, sum23, HB_IR_FADD);
                    for (unsigned i = 0; i < 4; i++) {
                        if (imm & (1u << (4 + i))) memcpy(out_bytes + base + i * 4, &sum, sizeof(sum));
                    }
                } else {
                    uint64_t prod[2] = {0, 0};
                    for (unsigned i = 0; i < 2; i++) {
                        if (imm & (1u << i)) {
                            uint64_t a, b;
                            memcpy(&a, lhs + base + i * 8, sizeof(a));
                            memcpy(&b, rhs + base + i * 8, sizeof(b));
                            prod[i] = hb_sse_arith_double_bits(a, b, HB_IR_FMUL);
                        }
                    }
                    uint64_t sum = hb_sse_arith_double_bits(prod[0], prod[1], HB_IR_FADD);
                    for (unsigned i = 0; i < 2; i++) {
                        if (imm & (1u << (4 + i))) memcpy(out_bytes + base + i * 8, &sum, sizeof(sum));
                    }
                }
            }
            return write_vec_reg_bytes(ctx, instr->dst.reg, out_bytes, bytes);
        }

        case HB_IR_FADD:
        case HB_IR_FSUB:
        case HB_IR_FMUL:
        case HB_IR_FDIV: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            bool is_sub = instr->op == HB_IR_FSUB;
            bool is_mul = instr->op == HB_IR_FMUL;
            bool is_div = instr->op == HB_IR_FDIV;
            unsigned arg = evex_target_arg(instr);
            bool scalar = (arg & 0x100) != 0;
            bool broadcast = (arg & HB_EVEX_ARG_BROADCAST) != 0;
            unsigned er_mode = (arg & HB_EVEX_ARG_ROUND_MASK) >> HB_EVEX_ARG_ROUND_SHIFT;
            unsigned lane = arg & 0xffu;
            size_t bytes = scalar ? 16 : bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lhs[64], rhs[64], out_bytes[64];
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, (scalar || broadcast) ? lane : bytes);
            if (r != HB_OK) return r;
            if (broadcast) {
                for (unsigned i = 1; i < (unsigned)(bytes / lane); i++)
                    memcpy(rhs + i * lane, rhs, lane);
            }
            memcpy(out_bytes, lhs, bytes);
            for (unsigned i = 0; i < (scalar ? 1U : (unsigned)(bytes / lane)); i++) {
                if (lane == 4) {
                    uint32_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 4, sizeof(abits));
                    memcpy(&bbits, rhs + i * 4, sizeof(bbits));
                    if (!is_div && !is_mul && !is_sub && er_mode)
                        cbits = hb_sse_add_float_bits_er(abits, bbits, er_mode);
                    else
                        cbits = hb_sse_arith_float_bits(abits, bbits,
                                                         is_div ? HB_IR_FDIV :
                                                         (is_mul ? HB_IR_FMUL :
                                                          (is_sub ? HB_IR_FSUB : HB_IR_FADD)));
                    memcpy(out_bytes + i * 4, &cbits, sizeof(cbits));
                } else {
                    uint64_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 8, sizeof(abits));
                    memcpy(&bbits, rhs + i * 8, sizeof(bbits));
                    cbits = hb_sse_arith_double_bits(abits, bbits,
                                                      is_div ? HB_IR_FDIV :
                                                      (is_mul ? HB_IR_FMUL :
                                                       (is_sub ? HB_IR_FSUB : HB_IR_FADD)));
                    memcpy(out_bytes + i * 8, &cbits, sizeof(cbits));
                }
            }
            if (scalar)
                return write_vec_reg_bytes_evex_scalar_masked(ctx, instr, out_bytes, lane);
            return write_vec_reg_bytes_evex_masked(ctx, instr, out_bytes, bytes, lane);
        }

        case HB_IR_FMIN:
        case HB_IR_FMAX: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            bool is_max = instr->op == HB_IR_FMAX;
            unsigned arg = evex_target_arg(instr);
            bool scalar = (arg & 0x100) != 0;
            bool broadcast = (arg & HB_EVEX_ARG_BROADCAST) != 0;
            unsigned lane = (unsigned)(arg & 0xff);
            size_t bytes = scalar ? 16 : bytes_for_size(instr->dst.size);
            if (bytes == 0) bytes = 16;
            uint8_t lhs[64], rhs[64], out_bytes[64];
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, bytes);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, (scalar || broadcast) ? lane : bytes);
            if (r != HB_OK) return r;
            if (broadcast) {
                for (unsigned i = 1; i < (unsigned)(bytes / lane); i++)
                    memcpy(rhs + i * lane, rhs, lane);
            }
            memcpy(out_bytes, lhs, bytes);
            for (unsigned i = 0; i < (scalar ? 1U : (unsigned)(bytes / lane)); i++) {
                if (lane == 4) {
                    uint32_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 4, sizeof(abits));
                    memcpy(&bbits, rhs + i * 4, sizeof(bbits));
                    cbits = hb_float_to_bits(select_sse_minmax_float(hb_bits_to_float(abits),
                                                                      hb_bits_to_float(bbits),
                                                                      is_max));
                    memcpy(out_bytes + i * 4, &cbits, sizeof(cbits));
                } else {
                    uint64_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 8, sizeof(abits));
                    memcpy(&bbits, rhs + i * 8, sizeof(bbits));
                    cbits = hb_double_to_bits(select_sse_minmax_double(hb_bits_to_double(abits),
                                                                       hb_bits_to_double(bbits),
                                                                       is_max));
                    memcpy(out_bytes + i * 8, &cbits, sizeof(cbits));
                }
            }
            if (scalar)
                return write_vec_reg_bytes_evex_scalar_masked(ctx, instr, out_bytes, lane);
            return write_vec_reg_bytes_evex_masked(ctx, instr, out_bytes, bytes, lane);
        }

        case HB_IR_COMISS: {
            float lhs = 0.0f, rhs = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_float(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            write_scalar_compare_flags(ctx, (double)lhs, (double)rhs);
            return HB_OK;
        }

        case HB_IR_COMISD: {
            double lhs = 0.0, rhs = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            write_scalar_compare_flags(ctx, lhs, rhs);
            return HB_OK;
        }

        case HB_IR_CVTSD2SI:
        case HB_IR_CVTTSD2SI: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            double value = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            if (instr->dst.size == HB_SIZE_64) {
                int64_t v = hb_double_to_i64_sse(value, instr->op == HB_IR_CVTTSD2SI);
                write_reg_sized(ctx, instr->dst.reg, (uint64_t)v, HB_SIZE_64);
            } else {
                int32_t v = hb_double_to_i32_sse(value, instr->op == HB_IR_CVTTSD2SI);
                write_reg_sized(ctx, instr->dst.reg, (uint32_t)v, HB_SIZE_32);
            }
            return HB_OK;
        }

        case HB_IR_CVTSS2SI:
        case HB_IR_CVTTSS2SI: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            float value = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            if (instr->dst.size == HB_SIZE_64) {
                int64_t v = hb_float_to_i64_sse(value, instr->op == HB_IR_CVTTSS2SI);
                write_reg_sized(ctx, instr->dst.reg, (uint64_t)v, HB_SIZE_64);
            } else {
                int32_t v = hb_float_to_i32_sse(value, instr->op == HB_IR_CVTTSS2SI);
                write_reg_sized(ctx, instr->dst.reg, (uint32_t)v, HB_SIZE_32);
            }
            return HB_OK;
        }

        case HB_IR_UNSUPPORTED:
            return HB_ERR_UNSUPPORTED_OPCODE;

        case HB_IR_FAULT:
            return HB_ERR_EXEC_FAULT;

        default:
            return HB_ERR_UNSUPPORTED_OPCODE;
    }
}

hb_result_t hb_interpreter_exec_one_for_jit(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    return exec_instr(ctx, instr);
}

hb_result_t hb_interpreter_run(hb_interpreter_t* interp, const hb_ir_func_t* func, hb_exec_result_t* out) {
    if (!interp || !func || !out) return HB_ERR_INVALID_ARG;
    if (!func->cfg || !func->cfg->entry) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = interp->ctx;
    hb_ir_block_t* block = func->cfg->entry;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;

    trace_refresh_runtime_flags();
    const unsigned int instr_trace_flags = trace_runtime_flags &
        (TRACE_FLAG_PC | TRACE_FLAG_STRCPY | TRACE_FLAG_SIMD | TRACE_FLAG_SIMD_DATA);

    while (block) {
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            out->result = HB_ERR_BLOCK_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            out->faulted = true;
            out->fault_reason = "block limit reached";
            ctx->last_result = HB_ERR_BLOCK_LIMIT;
            return HB_ERR_BLOCK_LIMIT;
        }
        blocks_executed++;

        bool transferred = false;
        for (size_t i = 0; i < block->instr_count; i++) {
            if (ctx->step_limit > 0 && steps >= ctx->step_limit) {
                out->result = HB_ERR_STEP_LIMIT;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                return HB_OK;
            }
            steps++;

            const hb_ir_instr_t* instr = &block->instrs[i];
            trace_current_instr = instr;
            if (instr_trace_flags) {
                trace_pc_probe(ctx, instr);
                trace_strcpy_probe(ctx, instr);
                trace_simd_exec(ctx, instr, block->guest_addr, i, steps);
                trace_simd_data_exec(ctx, instr, "pre", block->guest_addr, i, steps);
            }
            hb_result_t r = exec_instr(ctx, instr);
            if (r == HB_OK && (trace_runtime_flags & TRACE_FLAG_SIMD_DATA))
                trace_simd_data_exec(ctx, instr, "post", block->guest_addr, i, steps);
            trace_current_instr = NULL;
            if (r == HB_ERR_UNSUPPORTED_OPCODE || r == HB_ERR_EXEC_FAULT) {
                trace_exec_fault(ctx, instr, r, block->guest_addr, i, steps);
                out->result = r;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                out->faulted = true;
                out->fault_reason = (instr->op == HB_IR_UNSUPPORTED) ? instr->comment : "exec fault";
                return HB_OK;
            }
            if (r != HB_OK) {
                trace_exec_fault(ctx, instr, r, block->guest_addr, i, steps);
                out->result = r;
                out->steps_executed = steps;
                out->blocks_executed = blocks_executed;
                out->faulted = true;
                out->fault_reason = "memory or internal fault";
                return HB_OK;
            }

            if (instr->op == HB_IR_JMP || instr->op == HB_IR_Jcc || instr->op == HB_IR_LOOP ||
                instr->op == HB_IR_JRCXZ || instr->op == HB_IR_CALL || instr->op == HB_IR_RET) {
                hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
                if (!next) {
                    if (func->truncated) {
                        out->result = HB_ERR_TRANSLATION_TRUNCATED;
                        out->steps_executed = steps;
                        out->blocks_executed = blocks_executed;
                        out->faulted = true;
                        out->fault_reason = "translated function truncated before branch target";
                        ctx->last_result = HB_ERR_TRANSLATION_TRUNCATED;
                        return HB_ERR_TRANSLATION_TRUNCATED;
                    }
                    if (instr->op == HB_IR_CALL || instr->op == HB_IR_RET ||
                        instr->op == HB_IR_JMP || instr->op == HB_IR_Jcc ||
                        instr->op == HB_IR_LOOP || instr->op == HB_IR_JRCXZ) {
                        out->result = HB_OK;
                        out->steps_executed = steps;
                        out->blocks_executed = blocks_executed;
                        return HB_OK;
                    }
                    out->result = HB_ERR_NOT_FOUND;
                    out->steps_executed = steps;
                    out->blocks_executed = blocks_executed;
                    out->faulted = true;
                    out->fault_reason = "branch target block not found";
                    return HB_OK;
                }
                block = next;
                transferred = true;
                break;
            }
        }

        if (transferred) {
            continue;
        } else if (block) {
            if (block->instr_count) {
                const hb_ir_instr_t* last = &block->instrs[block->instr_count - 1];
                ctx->pc = last->guest_addr + last->guest_len;
                sync_arch_pc(ctx);
                hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
                if (next) {
                    block = next;
                    continue;
                }
            }
            out->result = HB_OK;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
        }
    }

    out->result = HB_OK;
    out->steps_executed = steps;
    out->blocks_executed = blocks_executed;
    return HB_OK;
}
