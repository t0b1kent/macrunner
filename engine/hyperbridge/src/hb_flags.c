#include <stdio.h>
#include <stdlib.h>
#include "hb_flags.h"
#include "hb_memory.h"
#include <stdint.h>
#include <string.h>

static uint64_t trunc_val(uint64_t v, hb_size_t sz) {
    switch (sz) {
        case HB_SIZE_8: return v & 0xFFULL;
        case HB_SIZE_16: return v & 0xFFFFULL;
        case HB_SIZE_32: return v & 0xFFFFFFFFULL;
        default: return v;
    }
}

static uint64_t read_reg_value_sized(hb_context_t* ctx, uint64_t idx,
                                     hb_size_t size, uint8_t reg_offset) {
    return (hb_context_read_reg_value(ctx, idx) >> ((unsigned)reg_offset * 8u)) &
           trunc_val(~0ULL, size);
}

static void write_reg_value_sized_offset(hb_context_t* ctx, uint64_t idx, uint64_t val,
                                         hb_size_t size, uint8_t reg_offset) {
    if (reg_offset == 0) {
        hb_context_write_reg_value_sized(ctx, idx, val, size);
        return;
    }
    uint64_t old = hb_context_read_reg_value(ctx, idx);
    unsigned shift = (unsigned)reg_offset * 8u;
    uint64_t mask = trunc_val(~0ULL, size) << shift;
    hb_context_write_reg_value(ctx, idx, (old & ~mask) | ((val << shift) & mask));
}

static unsigned msb_bit(hb_size_t sz) {
    switch (sz) {
        case HB_SIZE_8: return 7;
        case HB_SIZE_16: return 15;
        case HB_SIZE_32: return 31;
        default: return 63;
    }
}

static bool parity_even8(uint64_t v) {
    uint8_t x = (uint8_t)(v & 0xFFU);
    x ^= (uint8_t)(x >> 4);
    x &= 0xFU;
    return ((0x6996U >> x) & 1U) == 0;
}

static uint32_t valid_mask_for(hb_lazy_flags_kind_t kind, uint64_t count) {
    switch (kind) {
        case HB_LAZY_FLAGS_ADD:
        case HB_LAZY_FLAGS_ADC:
        case HB_LAZY_FLAGS_SUB:
        case HB_LAZY_FLAGS_SBB:
        case HB_LAZY_FLAGS_CMP:
            return HB_FLAG_BIT_ALL;
        case HB_LAZY_FLAGS_AND:
        case HB_LAZY_FLAGS_OR:
        case HB_LAZY_FLAGS_XOR:
        case HB_LAZY_FLAGS_TEST:
            return HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                   HB_FLAG_BIT_OF | HB_FLAG_BIT_PF;
        case HB_LAZY_FLAGS_SHL:
        case HB_LAZY_FLAGS_SHR:
        case HB_LAZY_FLAGS_SAR: {
            uint32_t mask = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF | HB_FLAG_BIT_PF;
            if (count == 1) mask |= HB_FLAG_BIT_OF;
            return mask;
        }
        default:
            return 0;
    }
}

void hb_lazy_flags_clear(hb_context_t* ctx) {
    if (!ctx) return;
    memset(&ctx->lazy_flags, 0, sizeof(ctx->lazy_flags));
}

void hb_lazy_flags_note(hb_context_t* ctx, hb_lazy_flags_kind_t kind,
                        hb_size_t width, uint64_t lhs, uint64_t rhs,
                        uint64_t result, uint64_t count) {
    if (!ctx) return;
    if ((kind == HB_LAZY_FLAGS_SHL || kind == HB_LAZY_FLAGS_SHR || kind == HB_LAZY_FLAGS_SAR) &&
        count == 0) {
        return;
    }

    hb_lazy_flags_t* lf = &ctx->lazy_flags;
    memset(lf, 0, sizeof(*lf));
    lf->pending = true;
    lf->kind = kind;
    lf->width = (uint8_t)width;
    lf->lhs = lhs;
    lf->rhs = rhs;
    lf->result = result;
    lf->count = count;
    lf->valid_mask = valid_mask_for(kind, count);
    lf->unsupported_mask = HB_FLAG_BIT_ALL & ~lf->valid_mask;
}

static void set_flag(hb_flags_t* f, uint32_t bit, bool value) {
    if (bit == HB_FLAG_BIT_ZF) f->zf = value;
    else if (bit == HB_FLAG_BIT_SF) f->sf = value;
    else if (bit == HB_FLAG_BIT_CF) f->cf = value;
    else if (bit == HB_FLAG_BIT_OF) f->of = value;
    else if (bit == HB_FLAG_BIT_PF) f->pf = value;
    else if (bit == HB_FLAG_BIT_AF) f->af = value;
}

static bool compute_flag(const hb_lazy_flags_t* lf, uint32_t bit) {
    hb_size_t sz = (hb_size_t)lf->width;
    uint64_t lhs = trunc_val(lf->lhs, sz);
    uint64_t rhs = trunc_val(lf->rhs, sz);
    uint64_t res = trunc_val(lf->result, sz);
    unsigned msb = msb_bit(sz);
    uint64_t sign = 1ULL << msb;

    switch (bit) {
        case HB_FLAG_BIT_ZF:
            return res == 0;
        case HB_FLAG_BIT_SF:
            return ((res >> msb) & 1ULL) != 0;
        case HB_FLAG_BIT_PF:
            return parity_even8(res);
        case HB_FLAG_BIT_AF:
            return ((lhs ^ rhs ^ res) & 0x10ULL) != 0;
        case HB_FLAG_BIT_CF:
            switch (lf->kind) {
                case HB_LAZY_FLAGS_ADD:
                    return (unsigned __int128)lhs + (unsigned __int128)rhs >
                           (unsigned __int128)trunc_val(~0ULL, sz);
                case HB_LAZY_FLAGS_ADC:
                    return (unsigned __int128)lhs + (unsigned __int128)rhs +
                           (unsigned __int128)(lf->count ? 1 : 0) >
                           (unsigned __int128)trunc_val(~0ULL, sz);
                case HB_LAZY_FLAGS_SUB:
                case HB_LAZY_FLAGS_CMP:
                    return lhs < rhs;
                case HB_LAZY_FLAGS_SBB:
                    return (unsigned __int128)lhs <
                           (unsigned __int128)rhs + (unsigned __int128)(lf->count ? 1 : 0);
                case HB_LAZY_FLAGS_AND:
                case HB_LAZY_FLAGS_OR:
                case HB_LAZY_FLAGS_XOR:
                case HB_LAZY_FLAGS_TEST:
                    return false;
                case HB_LAZY_FLAGS_SHL:
                    return (lf->count <= msb + 1) ? (((lhs >> (msb + 1 - lf->count)) & 1ULL) != 0) : false;
                case HB_LAZY_FLAGS_SHR:
                case HB_LAZY_FLAGS_SAR:
                    return ((lhs >> (lf->count - 1)) & 1ULL) != 0;
                default:
                    return false;
            }
        case HB_FLAG_BIT_OF:
            switch (lf->kind) {
                case HB_LAZY_FLAGS_ADD:
                    return ((lhs ^ res) & (rhs ^ res) & sign) != 0;
                case HB_LAZY_FLAGS_ADC: {
                    uint64_t erhs = trunc_val(rhs + (lf->count ? 1 : 0), sz);
                    return ((lhs ^ res) & (erhs ^ res) & sign) != 0;
                }
                case HB_LAZY_FLAGS_SUB:
                case HB_LAZY_FLAGS_CMP:
                    return ((lhs ^ rhs) & (lhs ^ res) & sign) != 0;
                case HB_LAZY_FLAGS_SBB: {
                    uint64_t erhs = trunc_val(rhs + (lf->count ? 1 : 0), sz);
                    return ((lhs ^ erhs) & (lhs ^ res) & sign) != 0;
                }
                case HB_LAZY_FLAGS_AND:
                case HB_LAZY_FLAGS_OR:
                case HB_LAZY_FLAGS_XOR:
                case HB_LAZY_FLAGS_TEST:
                    return false;
                case HB_LAZY_FLAGS_SHL:
                    return ((((res >> msb) & 1ULL) != 0) !=
                            ((lf->count <= msb + 1) ? (((lhs >> (msb + 1 - lf->count)) & 1ULL) != 0) : false));
                case HB_LAZY_FLAGS_SHR:
                    return ((lhs >> msb) & 1ULL) != 0;
                case HB_LAZY_FLAGS_SAR:
                    return false;
                default:
                    return false;
            }
        default:
            return false;
    }
}

hb_result_t hb_lazy_flags_materialize(hb_context_t* ctx, uint32_t mask) {
    if (!ctx) return HB_ERR_INVALID_ARG;
    hb_lazy_flags_t* lf = &ctx->lazy_flags;
    if (!lf->pending) return HB_OK;

    if ((mask & lf->unsupported_mask) != 0) {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }

    static const uint32_t bits[] = {
        HB_FLAG_BIT_ZF, HB_FLAG_BIT_SF, HB_FLAG_BIT_CF,
        HB_FLAG_BIT_OF, HB_FLAG_BIT_PF, HB_FLAG_BIT_AF
    };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        uint32_t bit = bits[i];
        if ((mask & bit) != 0 && (lf->materialized_mask & bit) == 0) {
            set_flag(&ctx->flags, bit, compute_flag(lf, bit));
            lf->materialized_mask |= bit;
        }
    }
    return HB_OK;
}

hb_result_t hb_flags_eval_cond(hb_context_t* ctx, hb_cc_t cc, bool* out) {
    if (!ctx || !out) return HB_ERR_INVALID_ARG;
    uint32_t need = 0;
    switch (cc) {
        case HB_CC_E:
        case HB_CC_NE:
            need = HB_FLAG_BIT_ZF;
            break;
        case HB_CC_S:
        case HB_CC_NS:
            need = HB_FLAG_BIT_SF;
            break;
        case HB_CC_G:
        case HB_CC_GE:
        case HB_CC_L:
        case HB_CC_LE:
            need = HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_OF;
            break;
        case HB_CC_A:
        case HB_CC_AE:
        case HB_CC_B:
        case HB_CC_BE:
            need = HB_FLAG_BIT_CF | HB_FLAG_BIT_ZF;
            break;
        case HB_CC_O:
        case HB_CC_NO:
            need = HB_FLAG_BIT_OF;
            break;
        case HB_CC_P:
        case HB_CC_NP:
            need = HB_FLAG_BIT_PF;
            break;
        default:
            return HB_ERR_UNSUPPORTED_FEATURE;
    }

    hb_result_t r = hb_lazy_flags_materialize(ctx, need);
    if (r != HB_OK) return r;

    switch (cc) {
        case HB_CC_E: *out = ctx->flags.zf; return HB_OK;
        case HB_CC_NE: *out = !ctx->flags.zf; return HB_OK;
        case HB_CC_S: *out = ctx->flags.sf; return HB_OK;
        case HB_CC_NS: *out = !ctx->flags.sf; return HB_OK;
        case HB_CC_G: *out = !ctx->flags.zf && (ctx->flags.sf == ctx->flags.of); return HB_OK;
        case HB_CC_GE: *out = ctx->flags.sf == ctx->flags.of; return HB_OK;
        case HB_CC_L: *out = ctx->flags.sf != ctx->flags.of; return HB_OK;
        case HB_CC_LE: *out = ctx->flags.zf || (ctx->flags.sf != ctx->flags.of); return HB_OK;
        case HB_CC_A: *out = !ctx->flags.cf && !ctx->flags.zf; return HB_OK;
        case HB_CC_AE: *out = !ctx->flags.cf; return HB_OK;
        case HB_CC_B: *out = ctx->flags.cf; return HB_OK;
        case HB_CC_BE: *out = ctx->flags.cf || ctx->flags.zf; return HB_OK;
        case HB_CC_O: *out = ctx->flags.of; return HB_OK;
        case HB_CC_NO: *out = !ctx->flags.of; return HB_OK;
        case HB_CC_P: *out = ctx->flags.pf; return HB_OK;
        case HB_CC_NP: *out = !ctx->flags.pf; return HB_OK;
        default: return HB_ERR_UNSUPPORTED_FEATURE;
    }
}

uint64_t hb_context_read_reg_value(hb_context_t* ctx, uint64_t idx) {
    if (!ctx) return 0;
    if (ctx->mode == HB_MODE_32BIT) {
        switch (idx) {
            case 0: return ctx->regs.x86.eax;
            case 1: return ctx->regs.x86.ecx;
            case 2: return ctx->regs.x86.edx;
            case 3: return ctx->regs.x86.ebx;
            case 4: return ctx->regs.x86.esp;
            case 5: return ctx->regs.x86.ebp;
            case 6: return ctx->regs.x86.esi;
            case 7: return ctx->regs.x86.edi;
            case 16: return ctx->regs.x86.eip;
            default: return 0;
        }
    }
    switch (idx) {
        case 0: return ctx->regs.x64.rax;
        case 1: return ctx->regs.x64.rcx;
        case 2: return ctx->regs.x64.rdx;
        case 3: return ctx->regs.x64.rbx;
        case 4: return ctx->regs.x64.rsp;
        case 5: return ctx->regs.x64.rbp;
        case 6: return ctx->regs.x64.rsi;
        case 7: return ctx->regs.x64.rdi;
        case 8: return ctx->regs.x64.r8;
        case 9: return ctx->regs.x64.r9;
        case 10: return ctx->regs.x64.r10;
        case 11: return ctx->regs.x64.r11;
        case 12: return ctx->regs.x64.r12;
        case 13: return ctx->regs.x64.r13;
        case 14: return ctx->regs.x64.r14;
        case 15: return ctx->regs.x64.r15;
        case 16: return ctx->regs.x64.rip;
        default: return 0;
    }
}

void hb_context_write_reg_value(hb_context_t* ctx, uint64_t idx, uint64_t val) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT) {
        switch (idx) {
            case 0: ctx->regs.x86.eax = (uint32_t)val; return;
            case 1: ctx->regs.x86.ecx = (uint32_t)val; return;
            case 2: ctx->regs.x86.edx = (uint32_t)val; return;
            case 3: ctx->regs.x86.ebx = (uint32_t)val; return;
            case 4: ctx->regs.x86.esp = (uint32_t)val; return;
            case 5: ctx->regs.x86.ebp = (uint32_t)val; return;
            case 6: ctx->regs.x86.esi = (uint32_t)val; return;
            case 7: ctx->regs.x86.edi = (uint32_t)val; return;
            case 16: ctx->regs.x86.eip = (uint32_t)val; return;
            default: return;
        }
    }
    switch (idx) {
        case 0: ctx->regs.x64.rax = val; return;
        case 1: ctx->regs.x64.rcx = val; return;
        case 2: ctx->regs.x64.rdx = val; return;
        case 3: ctx->regs.x64.rbx = val; return;
        case 4: ctx->regs.x64.rsp = val; return;
        case 5: ctx->regs.x64.rbp = val; return;
        case 6: ctx->regs.x64.rsi = val; return;
        case 7: ctx->regs.x64.rdi = val; return;
        case 8: ctx->regs.x64.r8 = val; return;
        case 9: ctx->regs.x64.r9 = val; return;
        case 10: ctx->regs.x64.r10 = val; return;
        case 11: ctx->regs.x64.r11 = val; return;
        case 12: ctx->regs.x64.r12 = val; return;
        case 13: ctx->regs.x64.r13 = val; return;
        case 14: ctx->regs.x64.r14 = val; return;
        case 15: ctx->regs.x64.r15 = val; return;
        case 16: ctx->regs.x64.rip = val; return;
        default: return;
    }
}

void hb_context_write_reg_value_sized(hb_context_t* ctx, uint64_t idx, uint64_t val, hb_size_t size) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT) {
        if (size == HB_SIZE_8 || size == HB_SIZE_16) {
            uint64_t old = hb_context_read_reg_value(ctx, idx);
            uint64_t mask = trunc_val(~0ULL, size);
            hb_context_write_reg_value(ctx, idx, (old & ~mask) | (val & mask));
        } else {
            hb_context_write_reg_value(ctx, idx, (uint32_t)val);
        }
        return;
    }

    if (size == HB_SIZE_32) {
        hb_context_write_reg_value(ctx, idx, (uint32_t)val);
    } else if (size == HB_SIZE_8 || size == HB_SIZE_16) {
        uint64_t old = hb_context_read_reg_value(ctx, idx);
        uint64_t mask = trunc_val(~0ULL, size);
        hb_context_write_reg_value(ctx, idx, (old & ~mask) | (val & mask));
    } else {
        hb_context_write_reg_value(ctx, idx, val);
    }
}

static hb_lazy_flags_kind_t kind_for_op(hb_ir_op_t op) {
    switch (op) {
        case HB_IR_ADD: return HB_LAZY_FLAGS_ADD;
        case HB_IR_ADC: return HB_LAZY_FLAGS_ADC;
        case HB_IR_SUB: return HB_LAZY_FLAGS_SUB;
        case HB_IR_SBB: return HB_LAZY_FLAGS_SBB;
        case HB_IR_AND: return HB_LAZY_FLAGS_AND;
        case HB_IR_OR: return HB_LAZY_FLAGS_OR;
        case HB_IR_XOR: return HB_LAZY_FLAGS_XOR;
        case HB_IR_SHL: return HB_LAZY_FLAGS_SHL;
        case HB_IR_SHR: return HB_LAZY_FLAGS_SHR;
        case HB_IR_SAR: return HB_LAZY_FLAGS_SAR;
        case HB_IR_CMP: return HB_LAZY_FLAGS_CMP;
        case HB_IR_TEST: return HB_LAZY_FLAGS_TEST;
        default: return HB_LAZY_FLAGS_UNKNOWN;
    }
}

uint64_t hb_flags_exec_binop(hb_context_t* ctx, hb_ir_op_t op,
                             uint64_t dst_reg, uint8_t dst_reg_offset,
                             uint64_t src1_reg, uint8_t src1_reg_offset,
                             bool src2_is_reg, uint64_t src2_value,
                             uint8_t src2_reg_offset,
                             hb_size_t size) {
    uint64_t lhs = read_reg_value_sized(ctx, src1_reg, size, src1_reg_offset);
    uint64_t rhs = src2_is_reg ? read_reg_value_sized(ctx, src2_value, size, src2_reg_offset) : src2_value;
    uint64_t result = 0;
    uint64_t count = 0;
    uint64_t carry = 0;

    if (op == HB_IR_ADC || op == HB_IR_SBB) {
        (void)hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_CF);
        carry = ctx->flags.cf ? 1 : 0;
    }

    switch (op) {
        case HB_IR_ADD: result = lhs + rhs; break;
        case HB_IR_ADC: result = lhs + rhs + carry; count = carry; break;
        case HB_IR_SUB: result = lhs - rhs; break;
        case HB_IR_SBB: result = lhs - rhs - carry; count = carry; break;
        case HB_IR_AND: result = lhs & rhs; break;
        case HB_IR_OR: result = lhs | rhs; break;
        case HB_IR_XOR: result = lhs ^ rhs; break;
        case HB_IR_ROL:
        case HB_IR_ROR: {
            unsigned msb = msb_bit(size);
            unsigned width = msb + 1;
            uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << width) - 1ULL);
            uint64_t tlhs = trunc_val(lhs, size);
            uint64_t raw_count = rhs & ((size == HB_SIZE_64) ? 0x3FULL : 0x1FULL);
            count = raw_count % width;
            if (count == 0) {
                write_reg_value_sized_offset(ctx, dst_reg, tlhs, size, dst_reg_offset);
                return tlhs;
            }
            if (op == HB_IR_ROL) {
                result = ((tlhs << count) | (tlhs >> (width - count))) & mask;
            } else {
                result = ((tlhs >> count) | (tlhs << (width - count))) & mask;
            }
            if (ctx->lazy_flags.pending) {
                (void)hb_lazy_flags_materialize(ctx, ctx->lazy_flags.valid_mask);
                hb_lazy_flags_clear(ctx);
            }
            write_reg_value_sized_offset(ctx, dst_reg, result, size, dst_reg_offset);
            ctx->flags.cf = (op == HB_IR_ROL) ?
                ((result & 1ULL) != 0) :
                (((result >> msb) & 1ULL) != 0);
            if (count == 1) {
                bool top = ((result >> msb) & 1ULL) != 0;
                bool next = ((result >> (msb - 1)) & 1ULL) != 0;
                ctx->flags.of = (op == HB_IR_ROL) ?
                    (top != ctx->flags.cf) :
                    (top != next);
            }
            return result;
        }
        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR: {
            count = rhs & ((size == HB_SIZE_64) ? 0x3FULL : 0x1FULL);
            if (count == 0) {
                write_reg_value_sized_offset(ctx, dst_reg, lhs, size, dst_reg_offset);
                return lhs;
            }
            unsigned msb = msb_bit(size);
            uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << (msb + 1)) - 1ULL);
            uint64_t tlhs = trunc_val(lhs, size);
            if (op == HB_IR_SHL) {
                result = (tlhs << count) & mask;
            } else if (op == HB_IR_SHR) {
                result = tlhs >> count;
            } else {
                int64_t slhs = (int64_t)(tlhs << (63 - msb)) >> (63 - msb);
                result = (uint64_t)(slhs >> count) & mask;
            }
            break;
        }
        default:
            return 0;
    }

    write_reg_value_sized_offset(ctx, dst_reg, result, size, dst_reg_offset);
    hb_lazy_flags_note(ctx, kind_for_op(op), size, lhs, rhs, result, count);
    return result;
}

static hb_result_t mem_read_size(hb_context_t* ctx, uint64_t addr, hb_size_t size, uint64_t* out) {
    switch (size) {
        case HB_SIZE_8: {
            uint8_t v = 0; hb_result_t r = hb_memory_read_u8(ctx->memory, addr, &v);
            if (r != HB_OK) return r; *out = v; return HB_OK;
        }
        case HB_SIZE_16: {
            uint16_t v = 0; hb_result_t r = hb_memory_read_u16(ctx->memory, addr, &v);
            if (r != HB_OK) return r; *out = v; return HB_OK;
        }
        case HB_SIZE_32: {
            uint32_t v = 0; hb_result_t r = hb_memory_read_u32(ctx->memory, addr, &v);
            if (r != HB_OK) return r; *out = v; return HB_OK;
        }
        case HB_SIZE_64:
        default:
            return hb_memory_read_u64(ctx->memory, addr, out);
    }
}

static hb_result_t mem_write_size(hb_context_t* ctx, uint64_t addr, uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  return hb_memory_write_u8(ctx->memory, addr, (uint8_t)value);
        case HB_SIZE_16: return hb_memory_write_u16(ctx->memory, addr, (uint16_t)value);
        case HB_SIZE_32: return hb_memory_write_u32(ctx->memory, addr, (uint32_t)value);
        case HB_SIZE_64:
        default:         return hb_memory_write_u64(ctx->memory, addr, value);
    }
}

static uint64_t resolve_operand_addr(hb_context_t* ctx, const hb_ir_operand_t* op) {
    uint64_t base = 0;
    if (op->mem.base < HB_REG_COUNT) base = hb_context_read_reg_value(ctx, op->mem.base);
    uint64_t index = 0;
    if (op->mem.index < HB_REG_COUNT) index = hb_context_read_reg_value(ctx, op->mem.index);
    if (op->mem.segment == 0x64) base += ctx->fs_base;
    else if (op->mem.segment == 0x65) base += ctx->gs_base;
    uint64_t ea = base + index * op->mem.scale + (uint64_t)op->mem.disp;
    /* Truncate to 32 bits in i386 mode or with 0x67 address-size override. */
    if (ctx->mode == HB_MODE_32BIT || op->mem.addr32)
        ea = (uint32_t)ea;
    return ea;
}

/* MacRunner 2026-07-30 — size the lazy-flags re-read before changing anything.
 *
 * Clean profile of HK's critical thread puts hb_flags_read_operand_value at 7.7 % self, the second largest
 * single item, with hb_context_read_reg_value + hb_context_write_reg_value at 1.9 % each and
 * hb_jit_helper_exec_extend_operand_lazy at 1.7 % — about 13 % as a group. It is all reached from
 * hb_jit_helper_eval_cond_lazy, the C helper the emitter calls for EVERY Jcc, and Jcc is 71.1 % of all
 * dispatched terminals on this workload.
 *
 * The design defers flag computation and, when a Jcc finally needs a flag, re-reads the PENDING operation's
 * operands and recomputes. So the question that decides whether this is worth restructuring is what those
 * operands are: a register re-read is a few loads, but HB_OP_MEM goes through mem_read_size and the whole
 * software MMU (hb_memory_read 5.4 %, find_region_normalized 3.0 %). Counting by operand type says how much
 * of the MMU cost is actually the flag path in disguise. Gated, per-thread, reported every 4 M calls. */
static __thread uint64_t t_flagop_reg, t_flagop_imm, t_flagop_mem, t_flagop_next;

static int trace_flag_operands_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("MACRUNNER_HB_TRACE_FLAG_OPERANDS");
        cached = e && *e && *e != '0';
    }
    return cached;
}

static void flagop_note(int type) {
    if (!trace_flag_operands_enabled()) return;
    if (type == 0) t_flagop_reg++; else if (type == 1) t_flagop_imm++; else t_flagop_mem++;
    {
        uint64_t n = t_flagop_reg + t_flagop_imm + t_flagop_mem;
        if (n >= t_flagop_next) {
            t_flagop_next = n + 4000000ull;
            fprintf(stderr, "macrunner-hb-flagops: total=%llu reg=%llu imm=%llu mem=%llu mem_pct=%.2f\n",
                    (unsigned long long)n, (unsigned long long)t_flagop_reg,
                    (unsigned long long)t_flagop_imm, (unsigned long long)t_flagop_mem,
                    n ? 100.0 * (double)t_flagop_mem / (double)n : 0.0);
            fflush(stderr);
        }
    }
}

hb_result_t hb_flags_read_operand_value(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t* out) {
    if (!ctx || !op || !out) return HB_ERR_INVALID_ARG;
    flagop_note(op->type == HB_OP_REG ? 0 : (op->type == HB_OP_IMM ? 1 : 2));
    if (op->type == HB_OP_REG) {
        *out = read_reg_value_sized(ctx, op->reg, op->size, op->reg_offset);
        return HB_OK;
    }
    if (op->type == HB_OP_IMM) {
        *out = (uint64_t)op->imm;
        return HB_OK;
    }
    if (op->type == HB_OP_MEM) {
        return mem_read_size(ctx, resolve_operand_addr(ctx, op), op->size, out);
    }
    return HB_ERR_INVALID_ARG;
}

hb_result_t hb_flags_write_operand_value(hb_context_t* ctx, const hb_ir_operand_t* op, uint64_t value) {
    if (!ctx || !op) return HB_ERR_INVALID_ARG;
    if (op->type == HB_OP_REG) {
        write_reg_value_sized_offset(ctx, op->reg, value, op->size, op->reg_offset);
        return HB_OK;
    }
    if (op->type == HB_OP_MEM) {
        return mem_write_size(ctx, resolve_operand_addr(ctx, op), trunc_val(value, op->size), op->size);
    }
    return HB_ERR_INVALID_ARG;
}

hb_result_t hb_flags_exec_binop_operand(hb_context_t* ctx, hb_ir_op_t op,
                                        const hb_ir_operand_t* dst, const hb_ir_operand_t* src1,
                                        const hb_ir_operand_t* src2, uint64_t* out) {
    if (!ctx || !dst || !src1 || !src2) return HB_ERR_INVALID_ARG;
    uint64_t lhs = 0, rhs = 0;
    hb_result_t r = hb_flags_read_operand_value(ctx, src1, &lhs);
    if (r != HB_OK) return r;
    r = hb_flags_read_operand_value(ctx, src2, &rhs);
    if (r != HB_OK) return r;

    uint64_t carry = 0;
    if (op == HB_IR_ADC || op == HB_IR_SBB) {
        r = hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_CF);
        if (r != HB_OK) return r;
        carry = ctx->flags.cf ? 1 : 0;
    }

    uint64_t result = 0;
    uint64_t count = 0;
    switch (op) {
        case HB_IR_ADD: result = lhs + rhs; break;
        case HB_IR_ADC: result = lhs + rhs + carry; count = carry; break;
        case HB_IR_SUB: result = lhs - rhs; break;
        case HB_IR_SBB: result = lhs - rhs - carry; count = carry; break;
        case HB_IR_AND: result = lhs & rhs; break;
        case HB_IR_OR:  result = lhs | rhs; break;
        case HB_IR_XOR: result = lhs ^ rhs; break;
        case HB_IR_ROL:
        case HB_IR_ROR: {
            unsigned msb = msb_bit(dst->size);
            unsigned width = msb + 1;
            uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << width) - 1ULL);
            uint64_t tlhs = trunc_val(lhs, dst->size);
            uint64_t raw_count = rhs & ((dst->size == HB_SIZE_64) ? 0x3FULL : 0x1FULL);
            count = raw_count % width;
            if (count == 0) {
                result = tlhs;
                r = hb_flags_write_operand_value(ctx, dst, result);
                if (r != HB_OK) return r;
                if (out) *out = result;
                return HB_OK;
            }
            if (op == HB_IR_ROL) result = ((tlhs << count) | (tlhs >> (width - count))) & mask;
            else result = ((tlhs >> count) | (tlhs << (width - count))) & mask;
            r = hb_flags_write_operand_value(ctx, dst, result);
            if (r != HB_OK) return r;
            if (ctx->lazy_flags.pending) {
                (void)hb_lazy_flags_materialize(ctx, ctx->lazy_flags.valid_mask);
                hb_lazy_flags_clear(ctx);
            }
            ctx->flags.cf = (op == HB_IR_ROL) ?
                ((result & 1ULL) != 0) :
                (((result >> msb) & 1ULL) != 0);
            if (count == 1) {
                bool top = ((result >> msb) & 1ULL) != 0;
                bool next = ((result >> (msb - 1)) & 1ULL) != 0;
                ctx->flags.of = (op == HB_IR_ROL) ?
                    (top != ctx->flags.cf) :
                    (top != next);
            }
            if (out) *out = result;
            return HB_OK;
        }
        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR: {
            count = rhs & ((dst->size == HB_SIZE_64) ? 0x3FULL : 0x1FULL);
            if (count == 0) {
                result = trunc_val(lhs, dst->size);
                r = hb_flags_write_operand_value(ctx, dst, result);
                if (r != HB_OK) return r;
                if (out) *out = result;
                return HB_OK;
            }
            unsigned msb = msb_bit(dst->size);
            uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << (msb + 1)) - 1ULL);
            uint64_t tlhs = trunc_val(lhs, dst->size);
            if (op == HB_IR_SHL) result = (tlhs << count) & mask;
            else if (op == HB_IR_SHR) result = tlhs >> count;
            else {
                int64_t slhs = (int64_t)(tlhs << (63 - msb)) >> (63 - msb);
                result = (uint64_t)(slhs >> count) & mask;
            }
            break;
        }
        default: return HB_ERR_UNSUPPORTED_OPCODE;
    }
    result = trunc_val(result, dst->size);
    r = hb_flags_write_operand_value(ctx, dst, result);
    if (r != HB_OK) return r;
    hb_lazy_flags_note(ctx, kind_for_op(op), dst->size, lhs, rhs, result, count);
    if (out) *out = result;
    return HB_OK;
}

hb_result_t hb_flags_exec_double_shift_operand(hb_context_t* ctx, hb_ir_op_t op,
                                               const hb_ir_operand_t* dst,
                                               const hb_ir_operand_t* src,
                                               const hb_ir_operand_t* count_op,
                                               uint64_t* out) {
    if (!ctx || !dst || !src || !count_op) return HB_ERR_INVALID_ARG;
    if (op != HB_IR_SHLD && op != HB_IR_SHRD) return HB_ERR_UNSUPPORTED_OPCODE;

    uint64_t lhs = 0, rhs = 0, raw_count = 0;
    hb_result_t r = hb_flags_read_operand_value(ctx, dst, &lhs);
    if (r != HB_OK) return r;
    r = hb_flags_read_operand_value(ctx, src, &rhs);
    if (r != HB_OK) return r;
    r = hb_flags_read_operand_value(ctx, count_op, &raw_count);
    if (r != HB_OK) return r;

    hb_size_t size = dst->size ? dst->size : src->size;
    if (!size) size = HB_SIZE_64;
    unsigned msb = msb_bit(size);
    unsigned width = msb + 1;
    uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << width) - 1ULL);
    uint64_t count = raw_count & (size == HB_SIZE_64 ? 0x3fULL : 0x1fULL);
    uint64_t old = trunc_val(lhs, size);
    uint64_t srcv = trunc_val(rhs, size);

    if (count == 0) {
        r = hb_flags_write_operand_value(ctx, dst, old);
        if (r != HB_OK) return r;
        if (out) *out = old;
        return HB_OK;
    }
    if (count > width) count %= width;
    if (count == 0) count = width;

    uint64_t result;
    if (op == HB_IR_SHLD) {
        result = ((old << count) | (srcv >> (width - count))) & mask;
    } else {
        result = ((old >> count) | (srcv << (width - count))) & mask;
    }

    r = hb_flags_write_operand_value(ctx, dst, result);
    if (r != HB_OK) return r;

    if (ctx->lazy_flags.pending) {
        (void)hb_lazy_flags_materialize(ctx, ctx->lazy_flags.valid_mask);
        hb_lazy_flags_clear(ctx);
    }
    ctx->flags.cf = (op == HB_IR_SHLD) ?
        (((old >> (width - count)) & 1ULL) != 0) :
        (((old >> (count - 1)) & 1ULL) != 0);
    if (count == 1) {
        bool result_msb = ((result >> msb) & 1ULL) != 0;
        bool old_msb = ((old >> msb) & 1ULL) != 0;
        ctx->flags.of = (op == HB_IR_SHLD) ? (result_msb != ctx->flags.cf)
                                           : (result_msb != old_msb);
    }
    ctx->flags.sf = ((result >> msb) & 1ULL) != 0;
    ctx->flags.zf = (result == 0);
    ctx->flags.pf = parity_even8(result);
    if (out) *out = result;
    return HB_OK;
}

void hb_flags_exec_cmp_test(hb_context_t* ctx, hb_ir_op_t op,
                            bool src1_is_reg, uint64_t src1_value, uint8_t src1_reg_offset,
                            bool src2_is_reg, uint64_t src2_value, uint8_t src2_reg_offset,
                            hb_size_t size) {
    uint64_t lhs = src1_is_reg ? read_reg_value_sized(ctx, src1_value, size, src1_reg_offset) : src1_value;
    uint64_t rhs = src2_is_reg ? read_reg_value_sized(ctx, src2_value, size, src2_reg_offset) : src2_value;
    uint64_t result = (op == HB_IR_TEST) ? (lhs & rhs) : (lhs - rhs);
    hb_lazy_flags_note(ctx, kind_for_op(op), size, lhs, rhs, result, 0);
}
