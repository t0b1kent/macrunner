#include "hb_runtime.h"
#include "hb_memory.h"
#include "hb_flags.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

static bool trace_native_write_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_TRACE_NATIVE_WRITES");
    return val && val[0] && val[0] != '0';
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
    return idx >= HB_REG_XMM0 && idx <= HB_REG_XMM15;
}

static hb_result_t read_xmm_reg(hb_context_t* ctx, int idx, uint64_t out[2]) {
    if (!ctx || !out || !is_xmm_reg(idx) || ctx->mode == HB_MODE_32BIT) return HB_ERR_INVALID_ARG;
    out[0] = ctx->regs.x64.xmm[idx - HB_REG_XMM0][0];
    out[1] = ctx->regs.x64.xmm[idx - HB_REG_XMM0][1];
    return HB_OK;
}

static hb_result_t write_xmm_reg(hb_context_t* ctx, int idx, const uint64_t in[2]) {
    if (!ctx || !in || !is_xmm_reg(idx) || ctx->mode == HB_MODE_32BIT) return HB_ERR_INVALID_ARG;
    ctx->regs.x64.xmm[idx - HB_REG_XMM0][0] = in[0];
    ctx->regs.x64.xmm[idx - HB_REG_XMM0][1] = in[1];
    return HB_OK;
}

static size_t bytes_for_size(hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return 1;
        case HB_SIZE_16: return 2;
        case HB_SIZE_32: return 4;
        case HB_SIZE_64: return 8;
        case HB_SIZE_128: return 16;
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

static hb_result_t read_scalar_double(hb_context_t* ctx, const hb_ir_operand_t* op, double* out) {
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
    *out = hb_bits_to_double(bits);
    return HB_OK;
}

static hb_result_t write_scalar_double(hb_context_t* ctx, const hb_ir_operand_t* op, double value) {
    if (!ctx || !op || op->type != HB_OP_REG || !is_xmm_reg(op->reg)) return HB_ERR_INTERNAL;
    uint64_t xmm[2];
    hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
    if (r != HB_OK) return r;
    xmm[0] = hb_double_to_bits(value);
    return write_xmm_reg(ctx, op->reg, xmm);
}

static hb_result_t read_scalar_float(hb_context_t* ctx, const hb_ir_operand_t* op, float* out) {
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
    *out = hb_bits_to_float(bits);
    return HB_OK;
}

static hb_result_t write_scalar_float(hb_context_t* ctx, const hb_ir_operand_t* op, float value) {
    if (!ctx || !op || op->type != HB_OP_REG || !is_xmm_reg(op->reg)) return HB_ERR_INTERNAL;
    uint64_t xmm[2];
    hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
    if (r != HB_OK) return r;
    xmm[0] = (xmm[0] & 0xffffffff00000000ULL) | hb_float_to_bits(value);
    return write_xmm_reg(ctx, op->reg, xmm);
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
    if (!ctx || !op || !out || bytes > 16) return HB_ERR_INVALID_ARG;
    memset(out, 0, 16);
    if (op->type == HB_OP_REG && is_xmm_reg(op->reg)) {
        uint64_t xmm[2];
        hb_result_t r = read_xmm_reg(ctx, op->reg, xmm);
        if (r != HB_OK) return r;
        memcpy(out, xmm, sizeof(xmm));
        return HB_OK;
    }
    if (op->type == HB_OP_MEM) {
        uint64_t addr = resolve_addr(ctx, op);
        return hb_memory_read(ctx->memory, addr, out, bytes);
    }
    return HB_ERR_INTERNAL;
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

static void write_reg_sized(hb_context_t* ctx, int idx, uint64_t val, hb_size_t size) {
    if (ctx->mode == HB_MODE_32BIT) {
        write_reg(ctx, idx, val);
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
            return HB_OK;
        }
        case HB_SIZE_16: {
            uint16_t v;
            hb_result_t r = hb_memory_read_u16(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            return HB_OK;
        }
        case HB_SIZE_32: {
            uint32_t v;
            hb_result_t r = hb_memory_read_u32(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
            return HB_OK;
        }
        case HB_SIZE_64: {
            uint64_t v;
            hb_result_t r = hb_memory_read_u64(ctx->memory, addr, &v);
            if (r != HB_OK) return r;
            *out = v;
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
    if (!getenv("MACRUNNER_HB_TRACE_STACK_FAULTS")) return;
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
    trace_guest_native_write(ctx, "interp_mem_write", addr, val, sz);
    switch (sz) {
        case HB_SIZE_8:  r = hb_memory_write_u8(ctx->memory, addr, (uint8_t)val); break;
        case HB_SIZE_16: r = hb_memory_write_u16(ctx->memory, addr, (uint16_t)val); break;
        case HB_SIZE_32: r = hb_memory_write_u32(ctx->memory, addr, (uint32_t)val); break;
        case HB_SIZE_64: r = hb_memory_write_u64(ctx->memory, addr, val); break;
        default: r = HB_ERR_MEMORY_FAULT; break;
    }
    if (r != HB_OK) trace_stack_write_fault(ctx, addr, val, sz, r);
    return r;
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
    const char* enabled = getenv("MACRUNNER_HB_TRACE_ATOMICS");
    return enabled && enabled[0] && enabled[0] != '0';
}

static const char* ir_op_name(hb_ir_op_t op) {
    switch (op) {
        case HB_IR_NOP: return "NOP";
        case HB_IR_MOV: return "MOV";
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
        case HB_IR_CMP: return "CMP";
        case HB_IR_TEST: return "TEST";
        case HB_IR_CMPXCHG: return "CMPXCHG";
        case HB_IR_XCHG: return "XCHG";
        case HB_IR_XADD: return "XADD";
        case HB_IR_LAHF: return "LAHF";
        case HB_IR_SAHF: return "SAHF";
        case HB_IR_CPUID: return "CPUID";
        case HB_IR_XGETBV: return "XGETBV";
        case HB_IR_SETcc: return "SETcc";
        case HB_IR_CMOVcc: return "CMOVcc";
        case HB_IR_LOAD: return "LOAD";
        case HB_IR_STORE: return "STORE";
        case HB_IR_PUSH: return "PUSH";
        case HB_IR_POP: return "POP";
        case HB_IR_CALL: return "CALL";
        case HB_IR_RET: return "RET";
        case HB_IR_JMP: return "JMP";
        case HB_IR_Jcc: return "Jcc";
        case HB_IR_SIGN_EXTEND: return "SIGN_EXTEND";
        case HB_IR_CWD: return "CWD";
        case HB_IR_SCAS: return "SCAS";
        case HB_IR_STOS: return "STOS";
        case HB_IR_ZERO_EXTEND: return "ZERO_EXTEND";
        case HB_IR_TRUNC: return "TRUNC";
        case HB_IR_TZCNT: return "TZCNT";
        case HB_IR_BSR: return "BSR";
        case HB_IR_BSWAP: return "BSWAP";
        case HB_IR_XMM_AND: return "XMM_AND";
        case HB_IR_XMM_ANDN: return "XMM_ANDN";
        case HB_IR_XMM_OR: return "XMM_OR";
        case HB_IR_XORPS: return "XORPS";
        case HB_IR_PCMPEQB: return "PCMPEQB";
        case HB_IR_PCMPEQW: return "PCMPEQW";
        case HB_IR_PCMPEQD: return "PCMPEQD";
        case HB_IR_PMOVMSKB: return "PMOVMSKB";
        case HB_IR_PUNPCK: return "PUNPCK";
        case HB_IR_PSHUF: return "PSHUF";
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
        case HB_IR_CVTSS2SD: return "CVTSS2SD";
        case HB_IR_CVTSD2SS: return "CVTSD2SS";
        case HB_IR_CVTSI2SD: return "CVTSI2SD";
        case HB_IR_CVTSI2SS: return "CVTSI2SS";
        case HB_IR_FADD: return "FADD";
        case HB_IR_FSUB: return "FSUB";
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
        case HB_IR_CVTTSD2SI: return "CVTTSD2SI";
        case HB_IR_CVTTSS2SI: return "CVTTSS2SI";
        case HB_IR_PADD: return "PADD";
        case HB_IR_HOST_CALL: return "HOST_CALL";
        case HB_IR_FAULT: return "FAULT";
        case HB_IR_UNSUPPORTED: return "UNSUPPORTED";
        default: return "UNKNOWN";
    }
}

static void trace_exec_fault(hb_context_t* ctx, const hb_ir_instr_t* instr,
                             hb_result_t result, uint64_t block_guest,
                             size_t ir_idx, uint64_t step_idx) {
    const char* enabled = getenv("MACRUNNER_HB_TRACE_FAULTS");
    static unsigned int fault_count;
    if (!enabled || !enabled[0] || enabled[0] == '0') return;
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
    const char* val = getenv("MACRUNNER_HB_TRACE_PC");
    const char* limit_env = getenv("MACRUNNER_HB_TRACE_PC_LIMIT");
    static unsigned int count;
    unsigned int limit = limit_env && limit_env[0] ? (unsigned int)strtoul(limit_env, NULL, 0) : 80;
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
    const char* enabled = getenv("MACRUNNER_HB_TRACE_STRCPY_PROBE");
    return enabled && enabled[0] && enabled[0] != '0';
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
    const char* limit_env = getenv("MACRUNNER_HB_TRACE_STRCPY_PROBE_LIMIT");
    unsigned int limit = limit_env && limit_env[0] ? (unsigned int)strtoul(limit_env, NULL, 0) : 200;
    const char* label = NULL;
    uint64_t src = 0;
    bool have_src = false;
    bool found_nul = false;
    size_t actual_len = 0;
    uint64_t object_src = 0;

    if (!trace_strcpy_probe_enabled() || !ctx || ctx->mode != HB_MODE_64BIT || !instr) return;

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
    const char* enabled = getenv("MACRUNNER_HB_TRACE_BRANCHES");
    return enabled && enabled[0] && enabled[0] != '0';
}

static void trace_branch_event(hb_context_t* ctx, const hb_ir_instr_t* instr,
                               const char* phase, uint64_t rsp_before,
                               uint64_t stack_qword, uint64_t target) {
    static unsigned int branch_count;
    if (!trace_branches_enabled()) return;
    if (++branch_count > 300) {
        if (branch_count == 301)
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

static hb_result_t exec_instr(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_result_t r;
    switch (instr->op) {
        case HB_IR_NOP:
            return HB_OK;

        case HB_IR_MOV: {
            if (instr->dst.type == HB_OP_REG && instr->src1.type == HB_OP_REG &&
                is_xmm_reg(instr->dst.reg) && is_xmm_reg(instr->src1.reg)) {
                uint64_t src[2];
                uint64_t dst[2];
                size_t bytes = bytes_for_size(instr->dst.size);
                if (bytes == 0) bytes = 16;
                r = read_xmm_reg(ctx, instr->src1.reg, src);
                if (r != HB_OK) return r;
                if (bytes >= 16) return write_xmm_reg(ctx, instr->dst.reg, src);
                r = read_xmm_reg(ctx, instr->dst.reg, dst);
                if (r != HB_OK) return r;
                memcpy(dst, src, bytes);
                return write_xmm_reg(ctx, instr->dst.reg, dst);
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
            uint64_t result = trunc_to_size(lhs * rhs, size);
            write_reg_sized(ctx, instr->dst.reg, result, size);
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
            ctx->flags.cf = (value & mask) != 0;
            hb_lazy_flags_clear(ctx);

            if (instr->op == HB_IR_BTS) {
                r = write_operand_value(ctx, &target, value | mask);
                if (r != HB_OK) return r;
            } else if (instr->op == HB_IR_BTR) {
                r = write_operand_value(ctx, &target, value & ~mask);
                if (r != HB_OK) return r;
            } else if (instr->op == HB_IR_BTC) {
                r = write_operand_value(ctx, &target, value ^ mask);
                if (r != HB_OK) return r;
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

        case HB_IR_XCHG: {
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
                uint64_t xmm[2] = {0, 0};
                size_t bytes = bytes_for_size(instr->src1.size);
                if (bytes == 0) bytes = 16;
                if (bytes < 16) {
                    r = read_xmm_reg(ctx, instr->dst.reg, xmm);
                    if (r != HB_OK) return r;
                }
                r = hb_memory_read(ctx->memory, addr, xmm, bytes);
                if (r != HB_OK) return r;
                return write_xmm_reg(ctx, instr->dst.reg, xmm);
            }
            uint64_t val = 0;
            r = mem_read(ctx, addr, &val, instr->dst.size);
            if (r != HB_OK) return r;
            if (instr->dst.type == HB_OP_REG) write_reg(ctx, instr->dst.reg, val);
            else return HB_ERR_INTERNAL;
            return HB_OK;
        }

        case HB_IR_STORE: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = resolve_addr(ctx, &instr->src1);
            if (instr->src2.type == HB_OP_REG && is_xmm_reg(instr->src2.reg)) {
                uint64_t xmm[2];
                size_t bytes = bytes_for_size(instr->src1.size);
                if (bytes == 0) bytes = bytes_for_size(instr->src2.size);
                if (bytes == 0) bytes = 16;
                r = read_xmm_reg(ctx, instr->src2.reg, xmm);
                if (r != HB_OK) return r;
                trace_guest_native_write(ctx, "interp_xmm_store", addr, xmm[0], (hb_size_t)bytes);
                r = hb_memory_write(ctx->memory, addr, xmm, bytes);
                if (r != HB_OK) trace_stack_write_fault(ctx, addr, xmm[0], (hb_size_t)bytes, r);
                return r;
            }
            uint64_t val = 0;
            if (instr->src2.type == HB_OP_REG) val = read_reg(ctx, instr->src2.reg);
            else if (instr->src2.type == HB_OP_IMM) val = (uint64_t)instr->src2.imm;
            else return HB_ERR_INTERNAL;
            r = mem_write(ctx, addr, val, instr->src2.size);
            if (r != HB_OK) return r;
            return HB_OK;
        }

        case HB_IR_PUSH: {
            uint64_t val = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (instr->src1.type == HB_OP_REG) val = read_reg(ctx, instr->src1.reg);
            else if (instr->src1.type == HB_OP_IMM) val = (uint64_t)instr->src1.imm;
            else return HB_ERR_INTERNAL;
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
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t*)&val);
                if (r != HB_OK) return r;
                ctx->regs.x86.esp += 4;
            } else {
                r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &val);
                if (r != HB_OK) return r;
                ctx->regs.x64.rsp += 8;
            }
            if (instr->dst.type == HB_OP_REG) write_reg(ctx, instr->dst.reg, val);
            else return HB_ERR_INTERNAL;
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
                ctx->regs.x86.esp -= 4;
                r = hb_memory_write_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t)ret_addr);
            } else {
                ctx->regs.x64.rsp -= 8;
                r = hb_memory_write_u64(ctx->memory, ctx->regs.x64.rsp, ret_addr);
            }
            if (r != HB_OK) return r;
            ctx->pc = target;
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, "call", rsp_before, ret_addr, target);
            return HB_OK;
        }

        case HB_IR_RET: {
            uint64_t ret_addr = 0;
            uint64_t rsp_before = ctx->mode == HB_MODE_32BIT ? ctx->regs.x86.esp : ctx->regs.x64.rsp;
            if (ctx->mode == HB_MODE_32BIT) {
                r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, (uint32_t*)&ret_addr);
                if (r != HB_OK) return r;
                ctx->regs.x86.esp += 4;
            } else {
                r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &ret_addr);
                if (r != HB_OK) return r;
                ctx->regs.x64.rsp += 8;
            }
            ctx->pc = ret_addr;
            if (ctx->mode == HB_MODE_64BIT)
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
            if (ctx->mode == HB_MODE_64BIT)
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
            if (ctx->mode == HB_MODE_64BIT)
                trace_branch_event(ctx, instr, taken ? "jcc-taken" : "jcc-fallthrough",
                                   rsp_before, instr->cc, ctx->pc);
            return HB_OK;
        }

        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR:
        case HB_IR_ROL:
        case HB_IR_ROR: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG) return HB_ERR_INTERNAL;
            if (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM) return HB_ERR_INTERNAL;
            hb_flags_exec_binop(ctx, instr->op,
                                instr->dst.reg, instr->dst.reg_offset,
                                instr->src1.reg, instr->src1.reg_offset,
                                instr->src2.type == HB_OP_REG,
                                instr->src2.type == HB_OP_REG ? (uint64_t)instr->src2.reg : (uint64_t)instr->src2.imm,
                                instr->src2.type == HB_OP_REG ? instr->src2.reg_offset : 0,
                                instr->dst.size);
            return HB_OK;
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
                    /* Be honest about the bridge's implemented opcode surface.
                     * Advertising AVX/OSXSAVE makes apps select VEX/YMM paths
                     * before HyperBridge can execute them. Keep the synthetic
                     * CPU at a conservative SSE2-era baseline until those
                     * feature families are implemented end-to-end.
                     */
                    ecx = 0;
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
                xcr0 = 0x3; /* x87 + SSE state only; AVX is intentionally not advertised. */

            write_reg_sized(ctx, HB_REG_RAX, (uint32_t)xcr0, HB_SIZE_32);
            write_reg_sized(ctx, HB_REG_RDX, (uint32_t)(xcr0 >> 32), HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_SETcc: {
            bool value = false;
            r = hb_flags_eval_cond(ctx, instr->cc, &value);
            if (r != HB_OK) return r;
            if (instr->dst.type == HB_OP_REG) {
                write_reg(ctx, instr->dst.reg, value ? 1 : 0);
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

        case HB_IR_SCAS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            uint64_t count = repeated ? ctx->regs.x64.rcx : 1;
            uint64_t rdi = ctx->regs.x64.rdi;
            uint64_t acc = read_reg_sized(ctx, HB_REG_RAX, size, 0);
            int64_t step = (int64_t)size;
            if (ctx->regs.x64.rflags & (1ULL << 10)) step = -step; /* Direction flag. */

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

            ctx->regs.x64.rdi = rdi;
            if (repeated) ctx->regs.x64.rcx = count;
            return HB_OK;
        }

        case HB_IR_STOS: {
            hb_size_t size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            uint64_t rep = (instr->src2.type == HB_OP_IMM) ? (uint64_t)instr->src2.imm : 0;
            bool repeated = (rep == 0xf2 || rep == 0xf3);
            uint64_t count = repeated ? ctx->regs.x64.rcx : 1;
            uint64_t rdi = ctx->regs.x64.rdi;
            uint64_t acc = read_reg_sized(ctx, HB_REG_RAX, size, 0);
            int64_t step = (int64_t)size;
            if (ctx->regs.x64.rflags & (1ULL << 10)) step = -step; /* Direction flag. */

            uint64_t iterations = 0;
            while (count > 0) {
                r = mem_write(ctx, rdi, acc, size);
                if (r != HB_OK) return r;
                rdi = (uint64_t)((int64_t)rdi + step);
                if (repeated) count--;
                if (!repeated) break;
                if (++iterations > 0x100000) return HB_ERR_STEP_LIMIT;
            }

            ctx->regs.x64.rdi = rdi;
            if (repeated) ctx->regs.x64.rcx = count;
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

        case HB_IR_XMM_AND:
        case HB_IR_XMM_ANDN:
        case HB_IR_XMM_OR:
        case HB_IR_XORPS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs[2], rhs[2], out[2];
            r = read_xmm_operand(ctx, &instr->src1, lhs);
            if (r != HB_OK) return r;
            r = read_xmm_operand(ctx, &instr->src2, rhs);
            if (r != HB_OK) return r;
            if (instr->op == HB_IR_XMM_AND) {
                out[0] = lhs[0] & rhs[0];
                out[1] = lhs[1] & rhs[1];
            } else if (instr->op == HB_IR_XMM_ANDN) {
                out[0] = ~lhs[0] & rhs[0];
                out[1] = ~lhs[1] & rhs[1];
            } else if (instr->op == HB_IR_XMM_OR) {
                out[0] = lhs[0] | rhs[0];
                out[1] = lhs[1] | rhs[1];
            } else {
                out[0] = lhs[0] ^ rhs[0];
                out[1] = lhs[1] ^ rhs[1];
            }
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_PCMPEQB:
        case HB_IR_PCMPEQW:
        case HB_IR_PCMPEQD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs[2], rhs[2], out[2] = {0, 0};
            uint8_t lbytes[16], rbytes[16], obytes[16];
            unsigned lane = instr->op == HB_IR_PCMPEQB ? 1 : (instr->op == HB_IR_PCMPEQW ? 2 : 4);
            r = read_xmm_operand(ctx, &instr->src1, lhs);
            if (r != HB_OK) return r;
            r = read_xmm_operand(ctx, &instr->src2, rhs);
            if (r != HB_OK) return r;
            memcpy(lbytes, lhs, sizeof(lbytes));
            memcpy(rbytes, rhs, sizeof(rbytes));
            memset(obytes, 0, sizeof(obytes));
            for (unsigned i = 0; i < 16; i += lane) {
                if (!memcmp(lbytes + i, rbytes + i, lane))
                    memset(obytes + i, 0xff, lane);
            }
            memcpy(out, obytes, sizeof(obytes));
            return write_xmm_reg(ctx, instr->dst.reg, out);
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

        case HB_IR_PUNPCK: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs[2], rhs[2];
            r = read_xmm_operand(ctx, &instr->src1, lhs);
            if (r != HB_OK) return r;
            r = read_xmm_operand(ctx, &instr->src2, rhs);
            if (r != HB_OK) return r;
            unsigned lane = (unsigned)(instr->target & 0xff);
            bool high = (instr->target & 0x100) != 0;
            if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            uint8_t lbytes[16], rbytes[16], obytes[16];
            memcpy(lbytes, lhs, sizeof(lbytes));
            memcpy(rbytes, rhs, sizeof(rbytes));
            memset(obytes, 0, sizeof(obytes));
            unsigned start = high ? 8 : 0;
            unsigned lanes = 8 / lane;
            unsigned out_pos = 0;
            for (unsigned i = 0; i < lanes; i++) {
                unsigned off = start + i * lane;
                memcpy(obytes + out_pos, lbytes + off, lane);
                out_pos += lane;
                memcpy(obytes + out_pos, rbytes + off, lane);
                out_pos += lane;
            }
            uint64_t out[2];
            memcpy(out, obytes, sizeof(obytes));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_PSHUF: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            if (instr->src2.type != HB_OP_IMM) return HB_ERR_INTERNAL;
            uint64_t in[2], out[2] = {0, 0};
            uint8_t ibytes[16], obytes[16];
            unsigned imm = (unsigned)instr->src2.imm & 0xff;
            r = read_xmm_operand(ctx, &instr->src1, in);
            if (r != HB_OK) return r;
            memcpy(ibytes, in, sizeof(ibytes));
            memcpy(obytes, ibytes, sizeof(obytes));
            if (instr->target == 4) {
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned src = (imm >> (lane * 2)) & 3;
                    memcpy(obytes + lane * 4, ibytes + src * 4, 4);
                }
            } else if (instr->target == 2) {
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned src = (imm >> (lane * 2)) & 3;
                    memcpy(obytes + lane * 2, ibytes + src * 2, 2);
                }
            } else if (instr->target == 0x102) {
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned src = (imm >> (lane * 2)) & 3;
                    memcpy(obytes + 8 + lane * 2, ibytes + 8 + src * 2, 2);
                }
            } else {
                return HB_ERR_INTERNAL;
            }
            memcpy(out, obytes, sizeof(obytes));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_PSRLQ:
        case HB_IR_PSLLQ:
        case HB_IR_PSRLDQ:
        case HB_IR_PSLLDQ: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            if (instr->src2.type != HB_OP_IMM) return HB_ERR_INTERNAL;
            uint64_t in[2], out[2] = {0, 0};
            unsigned count = (unsigned)(instr->src2.imm & 0xff);
            r = read_xmm_operand(ctx, &instr->src1, in);
            if (r != HB_OK) return r;

            if (instr->op == HB_IR_PSRLQ) {
                out[0] = (count > 63) ? 0 : (in[0] >> count);
                out[1] = (count > 63) ? 0 : (in[1] >> count);
            } else if (instr->op == HB_IR_PSLLQ) {
                out[0] = (count > 63) ? 0 : (in[0] << count);
                out[1] = (count > 63) ? 0 : (in[1] << count);
            } else {
                uint8_t src[16], dst[16] = {0};
                memcpy(src, in, sizeof(src));
                if (count < 16) {
                    if (instr->op == HB_IR_PSRLDQ) {
                        memmove(dst, src + count, 16 - count);
                    } else {
                        memmove(dst + count, src, 16 - count);
                    }
                }
                memcpy(out, dst, sizeof(out));
            }
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_PADD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t lhs[2], rhs[2], out[2] = {0, 0};
            uint8_t lbytes[16], rbytes[16], obytes[16];
            unsigned lane = (unsigned)(instr->target & 0xff);
            if (!(lane == 1 || lane == 2 || lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand(ctx, &instr->src1, lhs);
            if (r != HB_OK) return r;
            r = read_xmm_operand(ctx, &instr->src2, rhs);
            if (r != HB_OK) return r;
            memcpy(lbytes, lhs, sizeof(lbytes));
            memcpy(rbytes, rhs, sizeof(rbytes));
            for (unsigned i = 0; i < 16; i += lane) {
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
            memcpy(out, obytes, sizeof(out));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_MOVD: {
            if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg) &&
                instr->src1.type == HB_OP_REG && is_xmm_reg(instr->src1.reg)) {
                uint64_t xmm[2];
                uint64_t out[2] = {0, 0};
                r = read_xmm_reg(ctx, instr->src1.reg, xmm);
                if (r != HB_OK) return r;
                out[0] = xmm[0];
                return write_xmm_reg(ctx, instr->dst.reg, out);
            }
            if (instr->dst.type == HB_OP_REG && is_xmm_reg(instr->dst.reg)) {
                uint64_t raw = 0;
                r = read_operand_value(ctx, &instr->src1, &raw);
                if (r != HB_OK) return r;
                uint64_t out[2] = { instr->src1.size == HB_SIZE_64 ? raw : (uint32_t)raw, 0 };
                return write_xmm_reg(ctx, instr->dst.reg, out);
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
            uint64_t packed = 0;
            if (instr->src1.type == HB_OP_REG && is_xmm_reg(instr->src1.reg)) {
                uint64_t xmm[2];
                r = read_xmm_reg(ctx, instr->src1.reg, xmm);
                if (r != HB_OK) return r;
                packed = xmm[0];
            } else if (instr->src1.type == HB_OP_MEM) {
                uint64_t addr = resolve_addr(ctx, &instr->src1);
                r = mem_read(ctx, addr, &packed, HB_SIZE_64);
                if (r != HB_OK) return r;
            } else {
                return HB_ERR_INTERNAL;
            }
            int32_t lo = (int32_t)(uint32_t)(packed & 0xffffffffULL);
            int32_t hi = (int32_t)(uint32_t)(packed >> 32);
            uint64_t out[2] = { hb_double_to_bits((double)lo), hb_double_to_bits((double)hi) };
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_CVTDQ2PS:
        case HB_IR_CVTPS2DQ:
        case HB_IR_CVTTPS2DQ: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t in[2], out[2] = {0, 0};
            uint8_t ibytes[16], obytes[16] = {0};
            r = read_xmm_operand(ctx, &instr->src1, in);
            if (r != HB_OK) return r;
            memcpy(ibytes, in, sizeof(ibytes));

            if (instr->op == HB_IR_CVTDQ2PS) {
                for (unsigned i = 0; i < 4; i++) {
                    int32_t v;
                    float f;
                    uint32_t bits;
                    memcpy(&v, ibytes + i * 4, sizeof(v));
                    f = (float)v;
                    bits = hb_float_to_bits(f);
                    memcpy(obytes + i * 4, &bits, sizeof(bits));
                }
            } else {
                for (unsigned i = 0; i < 4; i++) {
                    uint32_t bits;
                    float f;
                    int32_t v;
                    memcpy(&bits, ibytes + i * 4, sizeof(bits));
                    f = hb_bits_to_float(bits);
                    /* CVTPS2DQ should honor MXCSR rounding. HyperBridge does not
                     * model MXCSR yet; truncation matches CVTTPS2DQ and is enough
                     * for current integer-valued Notepad++ geometry paths. */
                    v = (int32_t)f;
                    memcpy(obytes + i * 4, &v, sizeof(v));
                }
            }
            memcpy(out, obytes, sizeof(out));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_CVTPS2PD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint8_t in[16];
            uint64_t out[2];
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, 8);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < 2; i++) {
                uint32_t bits;
                memcpy(&bits, in + i * 4, sizeof(bits));
                out[i] = hb_double_to_bits((double)hb_bits_to_float(bits));
            }
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_CVTPD2PS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint8_t in[16];
            uint64_t out[2] = {0, 0};
            uint8_t obytes[16] = {0};
            r = read_xmm_operand_bytes(ctx, &instr->src1, in, 16);
            if (r != HB_OK) return r;
            for (unsigned i = 0; i < 2; i++) {
                uint64_t bits;
                uint32_t fbits;
                memcpy(&bits, in + i * 8, sizeof(bits));
                fbits = hb_float_to_bits((float)hb_bits_to_double(bits));
                memcpy(obytes + i * 4, &fbits, sizeof(fbits));
            }
            memcpy(out, obytes, sizeof(out));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_CVTSS2SD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            float value = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            return write_scalar_double(ctx, &instr->dst, (double)value);
        }

        case HB_IR_CVTSD2SS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            double value = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            return write_scalar_float(ctx, &instr->dst, (float)value);
        }

        case HB_IR_CVTSI2SD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t raw = 0;
            r = read_operand_value(ctx, &instr->src1, &raw);
            if (r != HB_OK) return r;
            double value = (instr->src1.size == HB_SIZE_64)
                ? (double)(int64_t)raw
                : (double)(int32_t)(uint32_t)raw;
            return write_scalar_double(ctx, &instr->dst, value);
        }

        case HB_IR_CVTSI2SS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            uint64_t raw = 0;
            r = read_operand_value(ctx, &instr->src1, &raw);
            if (r != HB_OK) return r;
            float value = (instr->src1.size == HB_SIZE_64)
                ? (float)(int64_t)raw
                : (float)(int32_t)(uint32_t)raw;
            return write_scalar_float(ctx, &instr->dst, value);
        }

        case HB_IR_DIVSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            double lhs = 0.0, rhs = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double(ctx, &instr->dst, lhs / rhs);
        }

        case HB_IR_ADDSD:
        case HB_IR_SUBSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            double lhs = 0.0, rhs = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double(ctx, &instr->dst,
                                       instr->op == HB_IR_ADDSD ? lhs + rhs : lhs - rhs);
        }

        case HB_IR_MULSD: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            double lhs = 0.0, rhs = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_double(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_double(ctx, &instr->dst, lhs * rhs);
        }

        case HB_IR_MULSS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            float lhs = 0.0f, rhs = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_float(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_float(ctx, &instr->dst, lhs * rhs);
        }

        case HB_IR_DIVSS: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            float lhs = 0.0f, rhs = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &lhs);
            if (r != HB_OK) return r;
            r = read_scalar_float(ctx, &instr->src2, &rhs);
            if (r != HB_OK) return r;
            return write_scalar_float(ctx, &instr->dst, lhs / rhs);
        }

        case HB_IR_FADD:
        case HB_IR_FSUB: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            bool is_sub = instr->op == HB_IR_FSUB;
            bool scalar = (instr->target & 0x100) != 0;
            unsigned lane = (unsigned)(instr->target & 0xff);
            uint8_t lhs[16], rhs[16], out_bytes[16];
            uint64_t out[2];
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, scalar ? lane : 16);
            if (r != HB_OK) return r;
            memcpy(out_bytes, lhs, sizeof(out_bytes));
            for (unsigned i = 0; i < (scalar ? 1U : 16U / lane); i++) {
                if (lane == 4) {
                    uint32_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 4, sizeof(abits));
                    memcpy(&bbits, rhs + i * 4, sizeof(bbits));
                    float aval = hb_bits_to_float(abits);
                    float bval = hb_bits_to_float(bbits);
                    cbits = hb_float_to_bits(is_sub ? (aval - bval) : (aval + bval));
                    memcpy(out_bytes + i * 4, &cbits, sizeof(cbits));
                } else {
                    uint64_t abits, bbits, cbits;
                    memcpy(&abits, lhs + i * 8, sizeof(abits));
                    memcpy(&bbits, rhs + i * 8, sizeof(bbits));
                    double aval = hb_bits_to_double(abits);
                    double bval = hb_bits_to_double(bbits);
                    cbits = hb_double_to_bits(is_sub ? (aval - bval) : (aval + bval));
                    memcpy(out_bytes + i * 8, &cbits, sizeof(cbits));
                }
            }
            memcpy(out, out_bytes, sizeof(out));
            return write_xmm_reg(ctx, instr->dst.reg, out);
        }

        case HB_IR_FMIN:
        case HB_IR_FMAX: {
            if (instr->dst.type != HB_OP_REG || !is_xmm_reg(instr->dst.reg)) return HB_ERR_INTERNAL;
            bool is_max = instr->op == HB_IR_FMAX;
            bool scalar = (instr->target & 0x100) != 0;
            unsigned lane = (unsigned)(instr->target & 0xff);
            uint8_t lhs[16], rhs[16], out_bytes[16];
            uint64_t out[2];
            if (!(lane == 4 || lane == 8)) return HB_ERR_INTERNAL;
            r = read_xmm_operand_bytes(ctx, &instr->src1, lhs, 16);
            if (r != HB_OK) return r;
            r = read_xmm_operand_bytes(ctx, &instr->src2, rhs, scalar ? lane : 16);
            if (r != HB_OK) return r;
            memcpy(out_bytes, lhs, sizeof(out_bytes));
            for (unsigned i = 0; i < (scalar ? 1U : 16U / lane); i++) {
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
            memcpy(out, out_bytes, sizeof(out));
            return write_xmm_reg(ctx, instr->dst.reg, out);
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

        case HB_IR_CVTTSD2SI: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            double value = 0.0;
            r = read_scalar_double(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            if (instr->dst.size == HB_SIZE_64)
                write_reg_sized(ctx, instr->dst.reg, (uint64_t)(int64_t)value, HB_SIZE_64);
            else
                write_reg_sized(ctx, instr->dst.reg, (uint32_t)(int32_t)value, HB_SIZE_32);
            return HB_OK;
        }

        case HB_IR_CVTTSS2SI: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            float value = 0.0f;
            r = read_scalar_float(ctx, &instr->src1, &value);
            if (r != HB_OK) return r;
            if (instr->dst.size == HB_SIZE_64)
                write_reg_sized(ctx, instr->dst.reg, (uint64_t)(int64_t)value, HB_SIZE_64);
            else
                write_reg_sized(ctx, instr->dst.reg, (uint32_t)(int32_t)value, HB_SIZE_32);
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

hb_result_t hb_interpreter_run(hb_interpreter_t* interp, const hb_ir_func_t* func, hb_exec_result_t* out) {
    if (!interp || !func || !out) return HB_ERR_INVALID_ARG;
    if (!func->cfg || !func->cfg->entry) return HB_ERR_INVALID_ARG;
    memset(out, 0, sizeof(hb_exec_result_t));

    hb_context_t* ctx = interp->ctx;
    hb_ir_block_t* block = func->cfg->entry;
    uint64_t steps = 0;
    uint64_t blocks_executed = 0;

    while (block) {
        if (ctx->block_limit > 0 && blocks_executed >= ctx->block_limit) {
            out->result = HB_ERR_BLOCK_LIMIT;
            out->steps_executed = steps;
            out->blocks_executed = blocks_executed;
            return HB_OK;
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
            trace_pc_probe(ctx, instr);
            trace_strcpy_probe(ctx, instr);
            hb_result_t r = exec_instr(ctx, instr);
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

            if (instr->op == HB_IR_JMP || instr->op == HB_IR_Jcc || instr->op == HB_IR_CALL || instr->op == HB_IR_RET) {
                hb_ir_block_t* next = find_block(func->cfg, ctx->pc);
                if (!next) {
                    if (instr->op == HB_IR_CALL || instr->op == HB_IR_RET) {
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
                if (ctx->arch == HB_ARCH_X64) ctx->regs.x64.rip = ctx->pc;
                else if (ctx->arch == HB_ARCH_X86) ctx->regs.x86.eip = (uint32_t)ctx->pc;
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
