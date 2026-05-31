#include "hb_codegen.h"
#include "hb_runtime.h"
#include "hb_memory.h"
#include "hb_flags.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <limits.h>

/* --- ARM64 instruction encoding helpers --- */
static void emit_u32(hb_codegen_buffer_t* buf, uint32_t insn) {
    uint8_t b[4] = { insn & 0xFF, (insn >> 8) & 0xFF, (insn >> 16) & 0xFF, (insn >> 24) & 0xFF };
    hb_codegen_buffer_append(buf, b, 4);
}

static void emit_nop(hb_codegen_buffer_t* buf)   { emit_u32(buf, 0xd503201f); }
static void emit_ret(hb_codegen_buffer_t* buf)   { emit_u32(buf, 0xd65f03c0); }

static void emit_b(hb_codegen_buffer_t* buf, int32_t off) {
    uint32_t imm26 = ((off / 4) & 0x03FFFFFF);
    emit_u32(buf, 0x14000000 | imm26);
}

static void emit_mov_reg(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* ORR Xd, XZR, Xn */
    emit_u32(buf, 0xaa0003e0 | (rn << 16) | rd);
}

static void emit_add_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x8b000000 | (rm << 16) | (rn << 5) | rd);
}

static void emit_add_reg_lsl(hb_codegen_buffer_t* buf, int rd, int rn, int rm, uint32_t shift) {
    emit_u32(buf, 0x8b000000 | (rm << 16) | ((shift & 0x3f) << 10) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_sub_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xcb000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_and_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x8a000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_orr_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xaa000000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_eor_reg(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0xca000000 | (rm << 16) | (rn << 5) | rd);
}

static void emit_add_imm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imm12) {
    emit_u32(buf, 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static void emit_sub_imm(hb_codegen_buffer_t* buf, int rd, int rn, uint32_t imm12) {
    emit_u32(buf, 0xd1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_cmp_reg(hb_codegen_buffer_t* buf, int rn, int rm) {
    /* SUBS XZR, Xn, Xm */
    emit_u32(buf, 0xeb00001f | (rm << 16) | (rn << 5));
}

static void emit_cmp_imm(hb_codegen_buffer_t* buf, int rn, uint32_t imm12) {
    /* SUBS XZR, Xn, #imm12 */
    emit_u32(buf, 0xf100001f | ((imm12 & 0xFFF) << 10) | (rn << 5));
}

static void __attribute__((unused)) emit_tst_reg(hb_codegen_buffer_t* buf, int rn, int rm) {
    /* ANDS XZR, Xn, Xm */
    emit_u32(buf, 0xea00001f | (rm << 16) | (rn << 5));
}

static void emit_ldr_x(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDR Xt, [Xn, #off]  — off must be multiple of 8 */
    uint32_t imm12 = (off / 8) & 0xFFF;
    emit_u32(buf, 0xf9400000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_ldr_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDR Wt, [Xn, #off]  — off must be multiple of 4 */
    uint32_t imm12 = (off / 4) & 0xFFF;
    emit_u32(buf, 0xb9400000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_ldrb_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDRB Wt, [Xn, #off] */
    emit_u32(buf, 0x39400000 | ((off & 0xfff) << 10) | (rn << 5) | rt);
}

static void emit_ldrh_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* LDRH Wt, [Xn, #off] — off must be multiple of 2 */
    uint32_t imm12 = (off / 2) & 0xFFF;
    emit_u32(buf, 0x79400000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_str_x(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STR Xt, [Xn, #off] */
    uint32_t imm12 = (off / 8) & 0xFFF;
    emit_u32(buf, 0xf9000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_str_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STR Wt, [Xn, #off] — off must be multiple of 4 */
    uint32_t imm12 = (off / 4) & 0xFFF;
    emit_u32(buf, 0xb9000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_strb_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STRB Wt, [Xn, #off] */
    emit_u32(buf, 0x39000000 | ((off & 0xfff) << 10) | (rn << 5) | rt);
}

static void emit_strh_w(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STRH Wt, [Xn, #off] — off must be multiple of 2 */
    uint32_t imm12 = (off / 2) & 0xFFF;
    emit_u32(buf, 0x79000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_mov_imm64(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    /* MOVZ + up to 3 MOVK */
    emit_u32(buf, 0xd2800000 | ((val & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2a00000 | (((val >> 16) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2c00000 | (((val >> 32) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2e00000 | (((val >> 48) & 0xFFFF) << 5) | rd);
}

static void emit_mov_imm_compact(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    if (val <= 0xffffu) {
        emit_u32(buf, 0xd2800000 | ((uint32_t)val << 5) | rd);
        return;
    }
    emit_mov_imm64(buf, rd, val);
}

static void emit_blr(hb_codegen_buffer_t* buf, int rn) {
    emit_u32(buf, 0xd63f0000 | (rn << 5));
}

static void emit_bcond(hb_codegen_buffer_t* buf, int cond, int32_t off) {
    uint32_t imm19 = ((off / 4) & 0x7FFFF);
    emit_u32(buf, 0x54000000 | (imm19 << 5) | (cond & 0xF));
}

static void patch_u32(hb_codegen_buffer_t* buf, size_t pos, uint32_t insn) {
    if (!buf || !buf->code || pos + 4 > buf->size) return;
    buf->code[pos] = (uint8_t)(insn & 0xff);
    buf->code[pos + 1] = (uint8_t)((insn >> 8) & 0xff);
    buf->code[pos + 2] = (uint8_t)((insn >> 16) & 0xff);
    buf->code[pos + 3] = (uint8_t)((insn >> 24) & 0xff);
}

static size_t emit_bcond_deferred(hb_codegen_buffer_t* buf, int cond) {
    size_t pos = buf->size;
    emit_bcond(buf, cond, 0);
    return pos;
}

static void patch_bcond(hb_codegen_buffer_t* buf, size_t pos, int cond, size_t target) {
    int32_t off = (int32_t)target - (int32_t)pos;
    uint32_t imm19 = ((off / 4) & 0x7ffff);
    patch_u32(buf, pos, 0x54000000 | (imm19 << 5) | (cond & 0xf));
}

static size_t emit_b_deferred(hb_codegen_buffer_t* buf) {
    size_t pos = buf->size;
    emit_b(buf, 0);
    return pos;
}

static void patch_b(hb_codegen_buffer_t* buf, size_t pos, size_t target) {
    int32_t off = (int32_t)target - (int32_t)pos;
    uint32_t imm26 = ((off / 4) & 0x03ffffff);
    patch_u32(buf, pos, 0x14000000 | imm26);
}

static void __attribute__((unused)) emit_lslv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02000 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_lsrv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02400 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_asrv(hb_codegen_buffer_t* buf, int rd, int rn, int rm) {
    emit_u32(buf, 0x9ac02800 | (rm << 16) | (rn << 5) | rd);
}

static void __attribute__((unused)) emit_mvn(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* ORN Xd, XZR, Xn */
    emit_u32(buf, 0xaa2003e0 | (rn << 16) | rd);
}

static void __attribute__((unused)) emit_neg(hb_codegen_buffer_t* buf, int rd, int rn) {
    /* SUB Xd, XZR, Xn */
    emit_u32(buf, 0xcb0003e0 | (rn << 16) | rd);
}

static bool is_gpr_reg_operand(const hb_ir_operand_t* op) {
    return op && op->type == HB_OP_REG && op->reg < HB_REG_XMM0;
}

static bool is_plain_gpr_reg_operand(const hb_ir_operand_t* op) {
    return is_gpr_reg_operand(op) && op->reg_offset == 0;
}

static bool jit_direct_mem_enabled(void) {
    const char* val = getenv("MACRUNNER_HB_JIT_DIRECT_MEM");
    return val && val[0] && val[0] != '0';
}

/* Prologue: save x19-x23, lr; x19 = ctx */
static void emit_prologue(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xf81f0ff3); /* STR X19, [SP, #-16]! */
    emit_u32(buf, 0xf81f0ff4); /* STR X20, [SP, #-16]! */
    emit_u32(buf, 0xf81f0ff5); /* STR X21, [SP, #-16]! */
    emit_u32(buf, 0xf81f0ff6); /* STR X22, [SP, #-16]! */
    emit_u32(buf, 0xf81f0ff7); /* STR X23, [SP, #-16]! */
    emit_u32(buf, 0xf81f0ffe); /* STR LR, [SP, #-16]! */
    emit_mov_reg(buf, 19, 0); /* MOV X19, X0 (ctx) */
}

/* Epilogue: restore and ret */
static void emit_epilogue(hb_codegen_buffer_t* buf) {
    emit_u32(buf, 0xf84107fe); /* LDR LR, [SP], #16 */
    emit_u32(buf, 0xf84107f7); /* LDR X23, [SP], #16 */
    emit_u32(buf, 0xf84107f6); /* LDR X22, [SP], #16 */
    emit_u32(buf, 0xf84107f5); /* LDR X21, [SP], #16 */
    emit_u32(buf, 0xf84107f4); /* LDR X20, [SP], #16 */
    emit_u32(buf, 0xf84107f3); /* LDR X19, [SP], #16 */
    emit_ret(buf);
}

static void emit_return_if_helper_failed(hb_codegen_buffer_t* buf) {
    emit_ldr_w(buf, 22, 19, (uint32_t)offsetof(hb_context_t, last_result));
    emit_cmp_imm(buf, 22, 0);
    emit_bcond(buf, 0, 32); /* EQ -> skip inline epilogue */
    emit_epilogue(buf);
}

/* x64 register file offsets in hb_context_t */
static size_t x64_reg_off(int idx) {
    switch (idx) {
        case 0:  return offsetof(hb_context_t, regs.x64.rax);
        case 1:  return offsetof(hb_context_t, regs.x64.rcx);
        case 2:  return offsetof(hb_context_t, regs.x64.rdx);
        case 3:  return offsetof(hb_context_t, regs.x64.rbx);
        case 4:  return offsetof(hb_context_t, regs.x64.rsp);
        case 5:  return offsetof(hb_context_t, regs.x64.rbp);
        case 6:  return offsetof(hb_context_t, regs.x64.rsi);
        case 7:  return offsetof(hb_context_t, regs.x64.rdi);
        case 8:  return offsetof(hb_context_t, regs.x64.r8);
        case 9:  return offsetof(hb_context_t, regs.x64.r9);
        case 10: return offsetof(hb_context_t, regs.x64.r10);
        case 11: return offsetof(hb_context_t, regs.x64.r11);
        case 12: return offsetof(hb_context_t, regs.x64.r12);
        case 13: return offsetof(hb_context_t, regs.x64.r13);
        case 14: return offsetof(hb_context_t, regs.x64.r14);
        case 15: return offsetof(hb_context_t, regs.x64.r15);
        case 16: return offsetof(hb_context_t, regs.x64.rip);
        default: return 0;
    }
}

/* Map IR condition to ARM64 condition code */
static int __attribute__((unused)) arm64_cond(hb_cc_t cc) {
    switch (cc) {
        case HB_CC_E:  return 0;  /* EQ */
        case HB_CC_NE: return 1;  /* NE */
        case HB_CC_A:  return 8;  /* HI */
        case HB_CC_AE: return 2;  /* HS */
        case HB_CC_B:  return 3;  /* LO */
        case HB_CC_BE: return 9;  /* LS */
        case HB_CC_G:  return 12; /* GT */
        case HB_CC_GE: return 10; /* GE */
        case HB_CC_L:  return 11; /* LT */
        case HB_CC_LE: return 13; /* LE */
        case HB_CC_S:  return 4;  /* MI */
        case HB_CC_NS: return 5;  /* PL */
        case HB_CC_O:  return 6;  /* VS */
        case HB_CC_NO: return 7;  /* VC */
        default: return 0;
    }
}

/* Load IR operand into X20 (scratch) */
static void emit_load_operand(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->type == HB_OP_REG) {
        size_t off = x64_reg_off(op->reg);
        emit_ldr_x(buf, 20, 19, (uint32_t)off);
    } else if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 20, (uint64_t)op->imm);
    } else {
        emit_mov_imm64(buf, 20, 0);
    }
}

/* Store X20 into IR operand (must be register) */
static void emit_store_operand(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->type == HB_OP_REG) {
        size_t off = x64_reg_off(op->reg);
        emit_str_x(buf, 20, 19, (uint32_t)off);
    }
}

static bool is_direct_user_mem_operand(const hb_ir_operand_t* op) {
    if (!op || op->type != HB_OP_MEM) return false;
    if (op->mem.segment != 0 || op->mem.addr32) return false;
    if (op->size != HB_SIZE_8 && op->size != HB_SIZE_16 &&
        op->size != HB_SIZE_32 && op->size != HB_SIZE_64) return false;
    if (op->mem.scale != 1 && op->mem.scale != 2 &&
        op->mem.scale != 4 && op->mem.scale != 8) return false;
    if (op->mem.base != HB_REG_COUNT && op->mem.base >= HB_REG_XMM0) return false;
    if (op->mem.index != HB_REG_COUNT && op->mem.index >= HB_REG_XMM0) return false;
    return true;
}

static void emit_direct_mem_addr(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    if (op->mem.base < HB_REG_XMM0) {
        emit_ldr_x(buf, 21, 19, (uint32_t)x64_reg_off(op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        emit_ldr_x(buf, 22, 19, (uint32_t)x64_reg_off(op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, 22, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, 22, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, 22);
        }
    }
}

static bool mem_operand_uses_reg(const hb_ir_operand_t* op, hb_reg_t reg) {
    return op && op->type == HB_OP_MEM && (op->mem.base == reg || op->mem.index == reg);
}

static bool same_plain_gpr_operand(const hb_ir_operand_t* a, const hb_ir_operand_t* b) {
    return is_plain_gpr_reg_operand(a) && is_plain_gpr_reg_operand(b) &&
           a->reg == b->reg && a->size == b->size;
}

static bool emit_direct_mem_addr_with_override(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                               hb_reg_t override_reg, int override_arm_reg) {
    if (!is_direct_user_mem_operand(op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;

    if (op->mem.base < HB_REG_XMM0) {
        if (op->mem.base == override_reg) emit_mov_reg(buf, 21, override_arm_reg);
        else emit_ldr_x(buf, 21, 19, (uint32_t)x64_reg_off(op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        if (op->mem.index == override_reg) emit_mov_reg(buf, 23, override_arm_reg);
        else emit_ldr_x(buf, 23, 19, (uint32_t)x64_reg_off(op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, 23, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, 23, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, 23);
        }
    }
    return true;
}

static bool emit_direct_mem_addr_with_override_scratch(hb_codegen_buffer_t* buf,
                                                       const hb_ir_operand_t* op,
                                                       hb_reg_t override_reg,
                                                       int override_arm_reg,
                                                       int scratch_arm_reg) {
    if (!is_direct_user_mem_operand(op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;

    if (op->mem.base < HB_REG_XMM0) {
        if (op->mem.base == override_reg) emit_mov_reg(buf, 21, override_arm_reg);
        else emit_ldr_x(buf, 21, 19, (uint32_t)x64_reg_off(op->mem.base));
    } else {
        emit_mov_imm64(buf, 21, 0);
    }
    if (op->mem.index < HB_REG_XMM0) {
        uint32_t shift = (op->mem.scale == 1) ? 0 :
                         (op->mem.scale == 2) ? 1 :
                         (op->mem.scale == 4) ? 2 : 3;
        if (op->mem.index == override_reg) emit_mov_reg(buf, scratch_arm_reg, override_arm_reg);
        else emit_ldr_x(buf, scratch_arm_reg, 19, (uint32_t)x64_reg_off(op->mem.index));
        emit_add_reg_lsl(buf, 21, 21, scratch_arm_reg, shift);
    }
    if (op->mem.disp != 0) {
        if (op->mem.disp > 0 && op->mem.disp < 4096) {
            emit_add_imm(buf, 21, 21, (uint32_t)op->mem.disp);
        } else if (op->mem.disp < 0 && -op->mem.disp < 4096) {
            emit_sub_imm(buf, 21, 21, (uint32_t)(-op->mem.disp));
        } else {
            emit_mov_imm64(buf, scratch_arm_reg, (uint64_t)op->mem.disp);
            emit_add_reg(buf, 21, 21, scratch_arm_reg);
        }
    }
    return true;
}

static void emit_mask_x_reg_to_size(hb_codegen_buffer_t* buf, int reg, int scratch, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:
            emit_mov_imm_compact(buf, scratch, 0xffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_16:
            emit_mov_imm_compact(buf, scratch, 0xffffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_32:
            emit_mov_imm64(buf, scratch, 0xffffffffu);
            emit_and_reg(buf, reg, reg, scratch);
            break;
        case HB_SIZE_64:
        default:
            break;
    }
}

static bool emit_load_gpr_sized_to_reg(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                       int arm_reg) {
    size_t off;
    if (!is_gpr_reg_operand(op)) return false;
    off = x64_reg_off(op->reg) + op->reg_offset;
    switch (op->size) {
        case HB_SIZE_8:  emit_ldrb_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_16: emit_ldrh_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_32: emit_ldr_w(buf, arm_reg, 19, (uint32_t)off); return true;
        case HB_SIZE_64:
            if (op->reg_offset != 0) return false;
            emit_ldr_x(buf, arm_reg, 19, (uint32_t)off);
            return true;
        default:
            return false;
    }
}

static uint32_t lazy_valid_mask_for_kind(hb_lazy_flags_kind_t kind) {
    switch (kind) {
        case HB_LAZY_FLAGS_ADD:
        case HB_LAZY_FLAGS_SUB:
        case HB_LAZY_FLAGS_CMP:
            return HB_FLAG_BIT_ALL;
        case HB_LAZY_FLAGS_AND:
        case HB_LAZY_FLAGS_OR:
        case HB_LAZY_FLAGS_XOR:
        case HB_LAZY_FLAGS_TEST:
            return HB_FLAG_BIT_ZF | HB_FLAG_BIT_SF | HB_FLAG_BIT_CF |
                   HB_FLAG_BIT_OF | HB_FLAG_BIT_PF;
        default:
            return 0;
    }
}

static bool lazy_kind_for_scalar_op(hb_ir_op_t op, hb_lazy_flags_kind_t* out) {
    if (!out) return false;
    switch (op) {
        case HB_IR_ADD:  *out = HB_LAZY_FLAGS_ADD; return true;
        case HB_IR_SUB:  *out = HB_LAZY_FLAGS_SUB; return true;
        case HB_IR_AND:  *out = HB_LAZY_FLAGS_AND; return true;
        case HB_IR_OR:   *out = HB_LAZY_FLAGS_OR; return true;
        case HB_IR_XOR:  *out = HB_LAZY_FLAGS_XOR; return true;
        case HB_IR_CMP:  *out = HB_LAZY_FLAGS_CMP; return true;
        case HB_IR_TEST: *out = HB_LAZY_FLAGS_TEST; return true;
        default: return false;
    }
}

static void emit_note_lazy_from_x20_x21_x22(hb_codegen_buffer_t* buf,
                                            hb_lazy_flags_kind_t kind,
                                            hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    uint32_t valid = lazy_valid_mask_for_kind(kind);
    emit_mov_imm_compact(buf, 23, 1);
    emit_strb_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, pending));
    emit_mov_imm_compact(buf, 23, (uint64_t)kind);
    emit_str_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, kind));
    emit_mov_imm_compact(buf, 23, (uint64_t)width);
    emit_strb_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, width));
    emit_str_x(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, lhs));
    emit_str_x(buf, 21, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, rhs));
    emit_str_x(buf, 22, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, result));
    emit_mov_imm_compact(buf, 23, 0);
    emit_str_x(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, count));
    emit_mov_imm_compact(buf, 23, valid);
    emit_str_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, valid_mask));
    emit_mov_imm_compact(buf, 23, HB_FLAG_BIT_ALL & ~valid);
    emit_str_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, unsupported_mask));
    emit_mov_imm_compact(buf, 23, 0);
    emit_str_w(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, materialized_mask));
}

static void emit_note_lazy_cmp_from_x23_x22_x21(hb_codegen_buffer_t* buf, hb_size_t width) {
    uint32_t lazy_off = (uint32_t)offsetof(hb_context_t, lazy_flags);
    emit_mov_imm_compact(buf, 20, 1);
    emit_strb_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, pending));
    emit_mov_imm_compact(buf, 20, HB_LAZY_FLAGS_CMP);
    emit_str_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, kind));
    emit_mov_imm_compact(buf, 20, (uint64_t)width);
    emit_strb_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, width));
    emit_str_x(buf, 23, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, lhs));
    emit_str_x(buf, 22, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, rhs));
    emit_str_x(buf, 21, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, result));
    emit_mov_imm_compact(buf, 20, 0);
    emit_str_x(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, count));
    emit_mov_imm_compact(buf, 20, HB_FLAG_BIT_ALL);
    emit_str_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, valid_mask));
    emit_mov_imm_compact(buf, 20, 0);
    emit_str_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, unsupported_mask));
    emit_str_w(buf, 20, 19, lazy_off + (uint32_t)offsetof(hb_lazy_flags_t, materialized_mask));
}

static void emit_direct_mem_load_to_x20(hb_codegen_buffer_t* buf, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  emit_ldrb_w(buf, 20, 21, 0); break;
        case HB_SIZE_16: emit_ldrh_w(buf, 20, 21, 0); break;
        case HB_SIZE_32: emit_ldr_w(buf, 20, 21, 0); break;
        case HB_SIZE_64:
        default:         emit_ldr_x(buf, 20, 21, 0); break;
    }
}

static void emit_direct_mem_store_from_x20(hb_codegen_buffer_t* buf, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  emit_strb_w(buf, 20, 21, 0); break;
        case HB_SIZE_16: emit_strh_w(buf, 20, 21, 0); break;
        case HB_SIZE_32: emit_str_w(buf, 20, 21, 0); break;
        case HB_SIZE_64:
        default:         emit_str_x(buf, 20, 21, 0); break;
    }
}

static bool emit_load_gpr_sized_to_x20(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    return emit_load_gpr_sized_to_reg(buf, op, 20);
}

static void emit_store_x20_to_gpr_sized(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op) {
    size_t off = x64_reg_off(op->reg) + op->reg_offset;
    switch (op->size) {
        case HB_SIZE_8:
            emit_strb_w(buf, 20, 19, (uint32_t)off);
            break;
        case HB_SIZE_16:
            emit_strh_w(buf, 20, 19, (uint32_t)off);
            break;
        case HB_SIZE_32:
        case HB_SIZE_64:
        default:
            emit_str_x(buf, 20, 19, (uint32_t)x64_reg_off(op->reg));
            break;
    }
}

static bool emit_hot_scalar_scan_loop(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t* add;
    const hb_ir_instr_t* cmp;
    const hb_ir_instr_t* jcc;
    hb_reg_t scan_reg;
    int branch_cond;
    size_t loop_off;
    int32_t loop_delta;

    if (!jit_direct_mem_enabled() || !block || block->instr_count != 3) return false;

    add = &block->instrs[0];
    cmp = &block->instrs[1];
    jcc = &block->instrs[2];
    if (add->op != HB_IR_ADD || cmp->op != HB_IR_CMP || jcc->op != HB_IR_Jcc) return false;
    if (jcc->target != add->guest_addr) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!is_plain_gpr_reg_operand(&add->dst) || !is_plain_gpr_reg_operand(&add->src1)) return false;
    if (add->dst.reg != add->src1.reg || add->dst.size != HB_SIZE_64) return false;
    if (add->src2.type != HB_OP_IMM || add->src2.imm != 1) return false;
    if (cmp->src1.type != HB_OP_MEM || (cmp->src1.size != HB_SIZE_8 && cmp->src1.size != HB_SIZE_16))
        return false;
    if (!mem_operand_uses_reg(&cmp->src1, add->dst.reg)) return false;
    if (cmp->src2.type == HB_OP_REG && !is_gpr_reg_operand(&cmp->src2)) return false;
    if (cmp->src2.type != HB_OP_REG && cmp->src2.type != HB_OP_IMM) return false;

    scan_reg = add->dst.reg;
    emit_ldr_x(buf, 20, 19, (uint32_t)x64_reg_off(scan_reg)); /* X20 = scan register */

    if (cmp->src2.type == HB_OP_REG) {
        if (cmp->src2.reg == scan_reg) emit_mov_reg(buf, 22, 20);
        else emit_ldr_x(buf, 22, 19, (uint32_t)x64_reg_off(cmp->src2.reg));
    } else {
        emit_mov_imm64(buf, 22, (uint64_t)cmp->src2.imm);
    }
    emit_mask_x_reg_to_size(buf, 22, 23, cmp->src1.size);

    loop_off = buf->size;
    emit_add_imm(buf, 20, 20, 1);
    if (!emit_direct_mem_addr_with_override(buf, &cmp->src1, scan_reg, 20))
        return false;
    switch (cmp->src1.size) {
        case HB_SIZE_8:  emit_ldrb_w(buf, 23, 21, 0); break;
        case HB_SIZE_16: emit_ldrh_w(buf, 23, 21, 0); break;
        default: return false;
    }
    emit_sub_reg(buf, 21, 23, 22);
    emit_cmp_reg(buf, 23, 22);
    branch_cond = arm64_cond(jcc->cc);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, branch_cond, loop_delta);

    emit_str_x(buf, 20, 19, (uint32_t)x64_reg_off(scan_reg));
    emit_note_lazy_cmp_from_x23_x22_x21(buf, cmp->src1.size);
    emit_mov_imm64(buf, 21, jcc->guest_addr + jcc->guest_len);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static bool emit_scalar_operand_to_x20(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                       hb_size_t size) {
    if (!op) return false;
    if (op->type == HB_OP_REG) {
        hb_ir_operand_t sized = *op;
        sized.size = size;
        return emit_load_gpr_sized_to_reg(buf, &sized, 20);
    }
    if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 20, (uint64_t)op->imm);
        emit_mask_x_reg_to_size(buf, 20, 23, size);
        return true;
    }
    if (op->type == HB_OP_MEM && jit_direct_mem_enabled() && is_direct_user_mem_operand(op)) {
        emit_direct_mem_addr(buf, op);
        emit_direct_mem_load_to_x20(buf, size);
        return true;
    }
    return false;
}

static bool emit_scalar_operand_to_x21(hb_codegen_buffer_t* buf, const hb_ir_operand_t* op,
                                       hb_size_t size) {
    if (!op) return false;
    if (op->type == HB_OP_REG) {
        hb_ir_operand_t sized = *op;
        sized.size = size;
        return emit_load_gpr_sized_to_reg(buf, &sized, 21);
    }
    if (op->type == HB_OP_IMM) {
        emit_mov_imm64(buf, 21, (uint64_t)op->imm);
        emit_mask_x_reg_to_size(buf, 21, 23, size);
        return true;
    }
    return false;
}

static bool emit_cmp_zero_set_pc(hb_codegen_buffer_t* buf, hb_cc_t cc,
                                 uint64_t target, uint64_t fallthrough) {
    if (cc != HB_CC_E && cc != HB_CC_NE) return false;
    emit_cmp_imm(buf, 22, 0);
    emit_bcond(buf, arm64_cond(cc), 28);
    emit_mov_imm64(buf, 21, fallthrough);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
    emit_b(buf, 24);
    emit_mov_imm64(buf, 20, target);
    emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
    return true;
}

static void emit_set_pc_imm64(hb_codegen_buffer_t* buf, uint64_t pc) {
    emit_mov_imm64(buf, 21, pc);
    emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
}

static bool emit_scalar_flags_jcc_pair(hb_codegen_buffer_t* buf, const hb_ir_instr_t* op,
                                       const hb_ir_instr_t* jcc) {
    hb_lazy_flags_kind_t kind;
    hb_size_t size;
    bool writes_dst = false;
    if (!op || !jcc || jcc->op != HB_IR_Jcc) return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!lazy_kind_for_scalar_op(op->op, &kind)) return false;

    if (op->op == HB_IR_ADD || op->op == HB_IR_SUB ||
        op->op == HB_IR_AND || op->op == HB_IR_OR || op->op == HB_IR_XOR) {
        if (!is_plain_gpr_reg_operand(&op->dst) || !is_plain_gpr_reg_operand(&op->src1) ||
            (op->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&op->src2)) ||
            (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM))
            return false;
        size = op->dst.size;
        writes_dst = true;
    } else if (op->op == HB_IR_CMP || op->op == HB_IR_TEST) {
        if (op->src1.type == HB_OP_REG && op->src1.reg_offset != 0) return false;
        if (op->src2.type == HB_OP_REG && op->src2.reg_offset != 0) return false;
        if (op->src2.type != HB_OP_REG && op->src2.type != HB_OP_IMM) return false;
        size = op->src1.size;
    } else {
        return false;
    }

    if (!emit_scalar_operand_to_x20(buf, &op->src1, size)) return false;
    if (!emit_scalar_operand_to_x21(buf, &op->src2, size)) return false;
    switch (op->op) {
        case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
        case HB_IR_SUB:
        case HB_IR_CMP: emit_sub_reg(buf, 22, 20, 21); break;
        case HB_IR_AND:
        case HB_IR_TEST: emit_and_reg(buf, 22, 20, 21); break;
        case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
        case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
        default: return false;
    }
    emit_mask_x_reg_to_size(buf, 22, 23, size);
    emit_note_lazy_from_x20_x21_x22(buf, kind, size);
    if (writes_dst) {
        emit_mov_reg(buf, 20, 22);
        emit_store_x20_to_gpr_sized(buf, &op->dst);
    }
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static bool emit_copy_scan_body_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t *load, *store, *add, *test, *jcc;
    hb_ir_operand_t inc_reg_operand;
    if (!jit_direct_mem_enabled() || !block || block->instr_count != 5) return false;

    load = &block->instrs[0];
    store = &block->instrs[1];
    add = &block->instrs[2];
    test = &block->instrs[3];
    jcc = &block->instrs[4];
    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE ||
        add->op != HB_IR_ADD || test->op != HB_IR_TEST || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->cc != HB_CC_E && jcc->cc != HB_CC_NE) return false;
    if (!is_plain_gpr_reg_operand(&load->dst)) return false;
    if (load->dst.size != HB_SIZE_8 && load->dst.size != HB_SIZE_16) return false;
    if (!is_direct_user_mem_operand(&load->src1) || !is_direct_user_mem_operand(&store->src1))
        return false;
    if (!same_plain_gpr_operand(&load->dst, &store->src2)) return false;
    if (store->src1.size != load->dst.size) return false;
    if (!same_plain_gpr_operand(&load->dst, &test->src1) ||
        !same_plain_gpr_operand(&load->dst, &test->src2))
        return false;
    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (add->dst.reg == load->dst.reg) return false;

    emit_direct_mem_addr(buf, &load->src1);
    emit_direct_mem_load_to_x20(buf, load->dst.size);
    emit_store_x20_to_gpr_sized(buf, &load->dst);
    emit_direct_mem_addr(buf, &store->src1);
    emit_direct_mem_store_from_x20(buf, store->src1.size);

    inc_reg_operand = add->dst;
    if (!emit_load_gpr_sized_to_reg(buf, &inc_reg_operand, 22)) return false;
    emit_add_imm(buf, 22, 22, 1);
    emit_str_x(buf, 22, 19, (uint32_t)x64_reg_off(add->dst.reg));

    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, load->dst.size);
    return emit_cmp_zero_set_pc(buf, jcc->cc, jcc->target, jcc->guest_addr + jcc->guest_len);
}

static const hb_ir_block_t* find_cfg_block_for_codegen(const hb_ir_cfg_t* cfg, uint64_t addr) {
    if (!cfg) return NULL;
    for (size_t i = 0; i < cfg->block_count; i++) {
        if (cfg->blocks[i] && cfg->blocks[i]->guest_addr == addr) return cfg->blocks[i];
    }
    return NULL;
}

static bool direct_mem_addr_preserves_override(const hb_ir_operand_t* op, hb_reg_t reg) {
    if (!is_direct_user_mem_operand(op)) return false;
    if (op->mem.base == HB_REG_RIP || op->mem.index == HB_REG_RIP) return false;
    if (!mem_operand_uses_reg(op, reg)) return false;
    if (op->mem.disp <= -4096 || op->mem.disp >= 4096) return false;
    if (op->mem.base == reg) {
        return op->mem.index == HB_REG_COUNT || op->mem.index == reg;
    }
    return op->mem.index == reg;
}

static bool emit_copy_scan_counted_loop_block(hb_codegen_buffer_t* buf,
                                              const hb_ir_block_t* body,
                                              const hb_ir_block_t* guard) {
    const hb_ir_instr_t *load, *store, *add, *test, *body_jcc;
    const hb_ir_instr_t *guard_sub, *guard_jcc;
    hb_reg_t inc_reg, count_reg;
    size_t loop_off, test_exit_branch, count_done_branch, test_exit_off, done_off;
    int32_t loop_delta;

    if (!jit_direct_mem_enabled() || !body || !guard) return false;
    if (body->instr_count != 5 || guard->instr_count != 2) return false;

    load = &body->instrs[0];
    store = &body->instrs[1];
    add = &body->instrs[2];
    test = &body->instrs[3];
    body_jcc = &body->instrs[4];
    guard_sub = &guard->instrs[0];
    guard_jcc = &guard->instrs[1];

    if (load->op != HB_IR_LOAD || store->op != HB_IR_STORE ||
        add->op != HB_IR_ADD || test->op != HB_IR_TEST || body_jcc->op != HB_IR_Jcc ||
        guard_sub->op != HB_IR_SUB || guard_jcc->op != HB_IR_Jcc)
        return false;
    if (body_jcc->cc != HB_CC_E && body_jcc->cc != HB_CC_NE) return false;
    if (guard_jcc->cc != HB_CC_E && guard_jcc->cc != HB_CC_NE) return false;
    if (body_jcc->guest_addr + body_jcc->guest_len != guard->guest_addr) return false;
    if (guard_jcc->target != body->guest_addr) return false;

    if (!is_plain_gpr_reg_operand(&load->dst)) return false;
    if (load->dst.size != HB_SIZE_8 && load->dst.size != HB_SIZE_16) return false;
    if (!is_direct_user_mem_operand(&load->src1) || !is_direct_user_mem_operand(&store->src1))
        return false;
    if (store->src1.size != load->dst.size) return false;
    if (!same_plain_gpr_operand(&load->dst, &store->src2)) return false;
    if (!same_plain_gpr_operand(&load->dst, &test->src1) ||
        !same_plain_gpr_operand(&load->dst, &test->src2))
        return false;

    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (!is_plain_gpr_reg_operand(&guard_sub->dst) ||
        !same_plain_gpr_operand(&guard_sub->dst, &guard_sub->src1) ||
        guard_sub->dst.size != HB_SIZE_64 ||
        guard_sub->src2.type != HB_OP_IMM || guard_sub->src2.imm != 1)
        return false;

    inc_reg = add->dst.reg;
    count_reg = guard_sub->dst.reg;
    if (inc_reg == load->dst.reg || count_reg == load->dst.reg || count_reg == inc_reg)
        return false;
    if (mem_operand_uses_reg(&load->src1, load->dst.reg) ||
        mem_operand_uses_reg(&store->src1, load->dst.reg) ||
        mem_operand_uses_reg(&load->src1, count_reg) ||
        mem_operand_uses_reg(&store->src1, count_reg))
        return false;
    if (!direct_mem_addr_preserves_override(&load->src1, inc_reg) ||
        !direct_mem_addr_preserves_override(&store->src1, inc_reg))
        return false;

    emit_ldr_x(buf, 23, 19, (uint32_t)x64_reg_off(inc_reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)x64_reg_off(count_reg));

    loop_off = buf->size;
    if (!emit_direct_mem_addr_with_override(buf, &load->src1, inc_reg, 23)) return false;
    emit_direct_mem_load_to_x20(buf, load->dst.size);
    if (!emit_direct_mem_addr_with_override(buf, &store->src1, inc_reg, 23)) return false;
    emit_direct_mem_store_from_x20(buf, store->src1.size);
    emit_add_imm(buf, 23, 23, 1);

    emit_cmp_imm(buf, 20, 0);
    test_exit_branch = emit_bcond_deferred(buf, arm64_cond(body_jcc->cc));

    emit_mov_reg(buf, 21, 22);
    emit_sub_imm(buf, 22, 22, 1);
    emit_cmp_imm(buf, 22, 0);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(guard_jcc->cc), loop_delta);

    emit_store_x20_to_gpr_sized(buf, &load->dst);
    emit_str_x(buf, 23, 19, (uint32_t)x64_reg_off(inc_reg));
    emit_str_x(buf, 22, 19, (uint32_t)x64_reg_off(count_reg));
    emit_mov_reg(buf, 20, 21);
    emit_mov_imm_compact(buf, 21, 1);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_SUB, guard_sub->dst.size);
    emit_set_pc_imm64(buf, guard_jcc->guest_addr + guard_jcc->guest_len);
    count_done_branch = emit_b_deferred(buf);

    test_exit_off = buf->size;
    patch_bcond(buf, test_exit_branch, arm64_cond(body_jcc->cc), test_exit_off);
    emit_store_x20_to_gpr_sized(buf, &load->dst);
    emit_str_x(buf, 23, 19, (uint32_t)x64_reg_off(inc_reg));
    emit_str_x(buf, 22, 19, (uint32_t)x64_reg_off(count_reg));
    emit_mov_reg(buf, 21, 20);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_TEST, load->dst.size);
    emit_set_pc_imm64(buf, body_jcc->target);

    done_off = buf->size;
    patch_b(buf, count_done_branch, done_off);
    return true;
}

static bool emit_bounded_scan_loop_block(hb_codegen_buffer_t* buf,
                                         const hb_ir_block_t* guard,
                                         const hb_ir_block_t* body) {
    const hb_ir_instr_t *guard_cmp, *guard_jcc;
    const hb_ir_instr_t *add, *body_cmp, *body_jcc;
    hb_reg_t scan_reg, limit_reg;
    size_t loop_off, guard_exit_branch, body_done_branch, guard_exit_off, done_off;
    int32_t loop_delta;

    if (!jit_direct_mem_enabled() || !guard || !body) return false;
    if (guard->instr_count != 2 || body->instr_count != 3) return false;

    guard_cmp = &guard->instrs[0];
    guard_jcc = &guard->instrs[1];
    add = &body->instrs[0];
    body_cmp = &body->instrs[1];
    body_jcc = &body->instrs[2];

    if (guard_cmp->op != HB_IR_CMP || guard_jcc->op != HB_IR_Jcc ||
        add->op != HB_IR_ADD || body_cmp->op != HB_IR_CMP || body_jcc->op != HB_IR_Jcc)
        return false;
    if (guard_jcc->cc != HB_CC_E || body_jcc->cc != HB_CC_NE) return false;
    if (guard_jcc->guest_addr + guard_jcc->guest_len != body->guest_addr) return false;
    if (body_jcc->target != guard->guest_addr) return false;
    if (body_jcc->guest_addr + body_jcc->guest_len != guard_jcc->target) return false;

    if (!is_plain_gpr_reg_operand(&guard_cmp->src1) ||
        !is_plain_gpr_reg_operand(&guard_cmp->src2) ||
        guard_cmp->src1.size != HB_SIZE_64 || guard_cmp->src2.size != HB_SIZE_64)
        return false;
    scan_reg = guard_cmp->src1.reg;
    limit_reg = guard_cmp->src2.reg;
    if (scan_reg == limit_reg) return false;

    if (!is_plain_gpr_reg_operand(&add->dst) ||
        !same_plain_gpr_operand(&add->dst, &add->src1) ||
        add->dst.reg != scan_reg || add->dst.size != HB_SIZE_64 ||
        add->src2.type != HB_OP_IMM || add->src2.imm != 1)
        return false;
    if (!is_direct_user_mem_operand(&body_cmp->src1) ||
        body_cmp->src1.size != HB_SIZE_8 ||
        body_cmp->src2.type != HB_OP_IMM || body_cmp->src2.imm != 0)
        return false;
    if (!mem_operand_uses_reg(&body_cmp->src1, scan_reg)) return false;
    if (mem_operand_uses_reg(&body_cmp->src1, limit_reg)) return false;

    emit_ldr_x(buf, 23, 19, (uint32_t)x64_reg_off(scan_reg));
    emit_ldr_x(buf, 22, 19, (uint32_t)x64_reg_off(limit_reg));

    loop_off = buf->size;
    emit_cmp_reg(buf, 23, 22);
    guard_exit_branch = emit_bcond_deferred(buf, arm64_cond(guard_jcc->cc));

    emit_add_imm(buf, 23, 23, 1);
    if (!emit_direct_mem_addr_with_override_scratch(buf, &body_cmp->src1, scan_reg, 23, 20))
        return false;
    emit_direct_mem_load_to_x20(buf, body_cmp->src1.size);
    emit_cmp_imm(buf, 20, 0);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(body_jcc->cc), loop_delta);

    emit_str_x(buf, 23, 19, (uint32_t)x64_reg_off(scan_reg));
    emit_mov_imm_compact(buf, 21, 0);
    emit_mov_reg(buf, 22, 20);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, body_cmp->src1.size);
    emit_set_pc_imm64(buf, body_jcc->guest_addr + body_jcc->guest_len);
    body_done_branch = emit_b_deferred(buf);

    guard_exit_off = buf->size;
    patch_bcond(buf, guard_exit_branch, arm64_cond(guard_jcc->cc), guard_exit_off);
    emit_str_x(buf, 23, 19, (uint32_t)x64_reg_off(scan_reg));
    emit_mov_reg(buf, 20, 23);
    emit_mov_reg(buf, 21, 22);
    emit_sub_reg(buf, 22, 20, 21);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, guard_cmp->src1.size);
    emit_set_pc_imm64(buf, guard_jcc->target);

    done_off = buf->size;
    patch_b(buf, body_done_branch, done_off);
    return true;
}

static bool emit_store_count_loop_block(hb_codegen_buffer_t* buf, const hb_ir_block_t* block) {
    const hb_ir_instr_t *store, *inc_count, *inc_ptr, *cmp, *jcc;
    hb_reg_t count_reg, ptr_reg;
    hb_size_t count_size;
    size_t loop_off;
    int32_t loop_delta;

    if (!jit_direct_mem_enabled() || !block || block->instr_count != 5) return false;
    store = &block->instrs[0];
    inc_count = &block->instrs[1];
    inc_ptr = &block->instrs[2];
    cmp = &block->instrs[3];
    jcc = &block->instrs[4];

    if (store->op != HB_IR_STORE || inc_count->op != HB_IR_ADD ||
        inc_ptr->op != HB_IR_ADD || cmp->op != HB_IR_CMP || jcc->op != HB_IR_Jcc)
        return false;
    if (jcc->target != block->guest_addr) return false;
    if (jcc->cc != HB_CC_B && jcc->cc != HB_CC_NE) return false;
    if (!is_direct_user_mem_operand(&store->src1) || store->src1.size != HB_SIZE_8)
        return false;
    if (store->src2.type != HB_OP_REG && store->src2.type != HB_OP_IMM) return false;

    if (!is_plain_gpr_reg_operand(&inc_count->dst) ||
        !same_plain_gpr_operand(&inc_count->dst, &inc_count->src1) ||
        inc_count->src2.type != HB_OP_IMM || inc_count->src2.imm != 1)
        return false;
    if (inc_count->dst.size != HB_SIZE_32 && inc_count->dst.size != HB_SIZE_64) return false;
    if (!is_plain_gpr_reg_operand(&inc_ptr->dst) ||
        !same_plain_gpr_operand(&inc_ptr->dst, &inc_ptr->src1) ||
        inc_ptr->dst.size != HB_SIZE_64 ||
        inc_ptr->src2.type != HB_OP_IMM || inc_ptr->src2.imm != 1)
        return false;

    count_reg = inc_count->dst.reg;
    ptr_reg = inc_ptr->dst.reg;
    count_size = inc_count->dst.size;
    if (count_reg == ptr_reg) return false;
    if (mem_operand_uses_reg(&store->src1, count_reg)) return false;
    if (!mem_operand_uses_reg(&store->src1, ptr_reg)) return false;
    if (cmp->src1.type != HB_OP_REG || cmp->src1.reg != count_reg ||
        cmp->src1.size != count_size)
        return false;
    if (cmp->src2.type == HB_OP_REG) {
        if (!is_plain_gpr_reg_operand(&cmp->src2) || cmp->src2.reg == ptr_reg ||
            cmp->src2.reg == count_reg)
            return false;
    } else if (cmp->src2.type != HB_OP_IMM) {
        return false;
    }

    emit_ldr_x(buf, 22, 19, (uint32_t)x64_reg_off(count_reg));
    emit_mask_x_reg_to_size(buf, 22, 20, count_size);
    emit_ldr_x(buf, 23, 19, (uint32_t)x64_reg_off(ptr_reg));

    loop_off = buf->size;
    if (!emit_direct_mem_addr_with_override_scratch(buf, &store->src1, ptr_reg, 23, 20))
        return false;
    if (store->src2.type == HB_OP_REG) {
        if (store->src2.reg == count_reg) {
            emit_mov_reg(buf, 20, 22);
        } else if (store->src2.reg == ptr_reg) {
            emit_mov_reg(buf, 20, 23);
        } else {
            hb_ir_operand_t sized = store->src2;
            sized.size = store->src1.size;
            if (!emit_load_gpr_sized_to_reg(buf, &sized, 20)) return false;
        }
    } else {
        emit_mov_imm64(buf, 20, (uint64_t)store->src2.imm);
    }
    emit_direct_mem_store_from_x20(buf, store->src1.size);

    emit_add_imm(buf, 22, 22, 1);
    emit_mask_x_reg_to_size(buf, 22, 20, count_size);
    emit_add_imm(buf, 23, 23, 1);

    if (cmp->src2.type == HB_OP_REG) {
        hb_ir_operand_t sized = cmp->src2;
        sized.size = count_size;
        if (!emit_load_gpr_sized_to_reg(buf, &sized, 21)) return false;
    } else {
        emit_mov_imm64(buf, 21, (uint64_t)cmp->src2.imm);
        emit_mask_x_reg_to_size(buf, 21, 20, count_size);
    }
    emit_sub_reg(buf, 20, 22, 21);
    emit_cmp_reg(buf, 22, 21);
    loop_delta = (int32_t)loop_off - (int32_t)buf->size;
    emit_bcond(buf, arm64_cond(jcc->cc), loop_delta);

    emit_str_x(buf, 22, 19, (uint32_t)x64_reg_off(count_reg));
    emit_str_x(buf, 23, 19, (uint32_t)x64_reg_off(ptr_reg));
    emit_mov_reg(buf, 23, 20);
    emit_mov_reg(buf, 20, 22);
    emit_mov_reg(buf, 22, 23);
    emit_note_lazy_from_x20_x21_x22(buf, HB_LAZY_FLAGS_CMP, count_size);
    emit_set_pc_imm64(buf, jcc->guest_addr + jcc->guest_len);
    return true;
}

/* JIT helper declarations (implemented below) */
extern uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr);
extern void     hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                                 uint64_t dst_reg, uint64_t dst_size,
                                                 uint64_t dst_reg_offset);
extern void     hb_jit_helper_store_u64(hb_context_t* ctx, uint64_t addr, uint64_t val);
extern void     hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr, uint64_t val, uint64_t size);
extern void     hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_pop(hb_context_t* ctx);
extern void     hb_jit_helper_push(hb_context_t* ctx, uint64_t val);
extern void     hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta);
extern uint64_t hb_jit_helper_call(hb_context_t* ctx, uint64_t target, uint64_t ret_addr);
extern void     hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_exec_binop_lazy(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                              uint64_t src1_reg, uint64_t src2_is_reg,
                                              uint64_t src2_value, uint64_t size);
extern void     hb_jit_helper_exec_cmp_test_lazy(hb_context_t* ctx, uint64_t op,
                                                  uint64_t src1_is_reg, uint64_t src1_value,
                                                  uint64_t src2_is_reg, uint64_t src2_value,
                                                  uint64_t size);
extern void     hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc);
extern void     hb_jit_helper_exec_setcc_lazy(hb_context_t* ctx, uint64_t cc,
                                              uint64_t dst_reg);
extern void     hb_jit_helper_exec_cmovcc_lazy(hb_context_t* ctx, uint64_t cc,
                                               uint64_t dst_reg, uint64_t src_is_reg,
                                               uint64_t src_value, uint64_t size);
extern void     hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_lahf(hb_context_t* ctx);
extern void     hb_jit_helper_sahf(hb_context_t* ctx);
extern void     hb_jit_helper_cpuid(hb_context_t* ctx);
extern void     hb_jit_helper_xgetbv(hb_context_t* ctx);
extern void     hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern hb_result_t hb_interpreter_exec_one_for_jit(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr);

static void emit_call_helper(hb_codegen_buffer_t* buf, void* fn);

static bool operand_is_xmm_or_vecmem(const hb_ir_operand_t* op) {
    if (!op) return false;
    if (op->type == HB_OP_REG) return op->reg >= HB_REG_XMM0 && op->reg <= HB_REG_XMM15;
    return op->type == HB_OP_MEM && op->size == HB_SIZE_128;
}

static hb_result_t emit_interp_ir_helper(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    emit_mov_reg(buf, 0, 19);
    emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
    emit_call_helper(buf, (void*)hb_jit_helper_exec_interp_ir);
    emit_return_if_helper_failed(buf);
    return HB_OK;
}

/* Emit a call to a C helper via BLR */
static void emit_call_helper(hb_codegen_buffer_t* buf, void* fn) {
    emit_mov_imm64(buf, 23, (uint64_t)fn);
    emit_blr(buf, 23);
}

/* --- Per-instruction codegen --- */
static hb_result_t codegen_instr(hb_codegen_buffer_t* buf, const hb_ir_instr_t* instr) {
    switch (instr->op) {
        case HB_IR_NOP:
            emit_nop(buf);
            return HB_OK;

        case HB_IR_MOV:
            if (operand_is_xmm_or_vecmem(&instr->dst) || operand_is_xmm_or_vecmem(&instr->src1))
                return emit_interp_ir_helper(buf, instr);
            if (!is_gpr_reg_operand(&instr->dst) ||
                (instr->src1.type == HB_OP_REG && !is_gpr_reg_operand(&instr->src1)) ||
                instr->dst.size != HB_SIZE_64 ||
                (instr->src1.type == HB_OP_REG && instr->src1.size != HB_SIZE_64)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_mov_operand_lazy);
                return HB_OK;
            }
            emit_load_operand(buf, &instr->src1);
            emit_store_operand(buf, &instr->dst);
            return HB_OK;

        case HB_IR_ZERO_EXTEND:
        case HB_IR_SIGN_EXTEND:
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_extend_operand_lazy);
            return HB_OK;

        case HB_IR_LEA: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            uint64_t addr = 0;
            if (instr->src1.mem.base < HB_REG_COUNT) {
                if (instr->src1.mem.base == HB_REG_RIP) {
                    addr = instr->guest_addr + instr->guest_len;
                } else {
                    size_t off = x64_reg_off(instr->src1.mem.base);
                    emit_ldr_x(buf, 20, 19, (uint32_t)off);
                    addr = 0; /* loaded into X20 */
                }
            } else {
                emit_mov_imm64(buf, 20, 0);
            }
            if (instr->src1.mem.base == HB_REG_RIP) {
                emit_mov_imm64(buf, 20, addr);
            }
            if (instr->src1.mem.index < HB_REG_COUNT) {
                size_t off = x64_reg_off(instr->src1.mem.index);
                emit_ldr_x(buf, 21, 19, (uint32_t)off);
                /* ADD X20, X20, X21, LSL #scale */
                uint32_t shift = (instr->src1.mem.scale == 1) ? 0 :
                                 (instr->src1.mem.scale == 2) ? 1 :
                                 (instr->src1.mem.scale == 4) ? 2 :
                                 (instr->src1.mem.scale == 8) ? 3 : 0;
                emit_u32(buf, 0x8b000000 | (21 << 16) | (shift << 10) | (20 << 5) | 20);
            }
            if (instr->src1.mem.disp != 0) {
                if (instr->src1.mem.disp >= 0 && instr->src1.mem.disp < 4096) {
                    emit_add_imm(buf, 20, 20, (uint32_t)instr->src1.mem.disp);
                } else if (instr->src1.mem.disp < 0 && -instr->src1.mem.disp < 4096) {
                    emit_sub_imm(buf, 20, 20, (uint32_t)(-instr->src1.mem.disp));
                } else {
                    emit_mov_imm64(buf, 21, (uint64_t)instr->src1.mem.disp);
                    emit_add_reg(buf, 20, 20, 21);
                }
            }
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_ADD:
        case HB_IR_ADC:
        case HB_IR_SUB:
        case HB_IR_SBB:
        case HB_IR_AND:
        case HB_IR_OR:
        case HB_IR_XOR: {
            hb_lazy_flags_kind_t kind;
            if (!is_plain_gpr_reg_operand(&instr->dst) ||
                !is_plain_gpr_reg_operand(&instr->src1) ||
                (instr->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&instr->src2)) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_operand_lazy);
                return HB_OK;
            }
            if (instr->op == HB_IR_ADC || instr->op == HB_IR_SBB) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->op);
                emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
                emit_mov_imm64(buf, 3, (uint64_t)instr->src1.reg);
                emit_mov_imm64(buf, 4, instr->src2.type == HB_OP_REG ? 1ULL : 0ULL);
                emit_mov_imm64(buf, 5, instr->src2.type == HB_OP_REG ? (uint64_t)instr->src2.reg : (uint64_t)instr->src2.imm);
                emit_mov_imm64(buf, 6, (uint64_t)instr->dst.size);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_lazy);
                return HB_OK;
            }
            if (!lazy_kind_for_scalar_op(instr->op, &kind)) return HB_ERR_INTERNAL;
            if (!emit_load_gpr_sized_to_reg(buf, &instr->src1, 20)) return HB_ERR_INTERNAL;
            if (instr->src2.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src2, 21)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm64(buf, 21, (uint64_t)instr->src2.imm);
                emit_mask_x_reg_to_size(buf, 21, 23, instr->dst.size);
            }
            switch (instr->op) {
                case HB_IR_ADD: emit_add_reg(buf, 22, 20, 21); break;
                case HB_IR_SUB: emit_sub_reg(buf, 22, 20, 21); break;
                case HB_IR_AND: emit_and_reg(buf, 22, 20, 21); break;
                case HB_IR_OR:  emit_orr_reg(buf, 22, 20, 21); break;
                case HB_IR_XOR: emit_eor_reg(buf, 22, 20, 21); break;
                default: return HB_ERR_INTERNAL;
            }
            emit_mask_x_reg_to_size(buf, 22, 23, instr->dst.size);
            emit_note_lazy_from_x20_x21_x22(buf, kind, instr->dst.size);
            emit_mov_reg(buf, 20, 22);
            emit_store_x20_to_gpr_sized(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_MUL:
        case HB_IR_IMUL:
        case HB_IR_DIV:
        case HB_IR_IDIV: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_mul_div_operand);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR:
        case HB_IR_ROL:
        case HB_IR_ROR: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG)
                return emit_interp_ir_helper(buf, instr);
            if (!is_plain_gpr_reg_operand(&instr->dst) ||
                !is_plain_gpr_reg_operand(&instr->src1) ||
                (instr->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&instr->src2))) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_operand_lazy);
                return HB_OK;
            }
            uint64_t src2_type = 0;
            uint64_t src2_value = 0;
            if (instr->src2.type == HB_OP_REG) {
                src2_type = 1;
                src2_value = (uint64_t)instr->src2.reg;
            } else if (instr->src2.type == HB_OP_IMM) {
                src2_type = 0;
                src2_value = (uint64_t)instr->src2.imm;
            } else {
                return HB_ERR_INTERNAL;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->op);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_mov_imm64(buf, 3, (uint64_t)instr->src1.reg);
            emit_mov_imm64(buf, 4, src2_type);
            emit_mov_imm64(buf, 5, src2_value);
            emit_mov_imm64(buf, 6, (uint64_t)instr->dst.size);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_lazy);
            return HB_OK;
        }

        case HB_IR_SHLD:
        case HB_IR_SHRD: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_double_shift_operand);
            return HB_OK;
        }

        case HB_IR_NOT: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_not_operand_lazy);
            return HB_OK;
        }

        case HB_IR_NEG: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_neg_operand_lazy);
            return HB_OK;
        }

        case HB_IR_LAHF: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_lahf);
            return HB_OK;
        }

        case HB_IR_SAHF: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_sahf);
            return HB_OK;
        }

        case HB_IR_CPUID: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_cpuid);
            return HB_OK;
        }

        case HB_IR_XGETBV: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_xgetbv);
            return HB_OK;
        }

        case HB_IR_CMP:
        case HB_IR_TEST: {
            hb_lazy_flags_kind_t kind;
            if ((instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM) ||
                (instr->src1.type == HB_OP_REG && instr->src1.reg_offset != 0) ||
                (instr->src2.type == HB_OP_REG && instr->src2.reg_offset != 0)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_cmp_test_operand_lazy);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            if (!lazy_kind_for_scalar_op(instr->op, &kind)) return HB_ERR_INTERNAL;
            if (instr->src1.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src1, 20)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm64(buf, 20, (uint64_t)instr->src1.imm);
                emit_mask_x_reg_to_size(buf, 20, 23, instr->src1.size);
            }
            if (instr->src2.type == HB_OP_REG) {
                if (!emit_load_gpr_sized_to_reg(buf, &instr->src2, 21)) return HB_ERR_INTERNAL;
            } else {
                emit_mov_imm64(buf, 21, (uint64_t)instr->src2.imm);
                emit_mask_x_reg_to_size(buf, 21, 23, instr->src1.size);
            }
            if (instr->op == HB_IR_TEST) emit_and_reg(buf, 22, 20, 21);
            else emit_sub_reg(buf, 22, 20, 21);
            emit_mask_x_reg_to_size(buf, 22, 23, instr->src1.size);
            emit_note_lazy_from_x20_x21_x22(buf, kind, instr->src1.size);
            return HB_OK;
        }

        case HB_IR_LOAD: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            if (!is_gpr_reg_operand(&instr->dst))
                return emit_interp_ir_helper(buf, instr);
            if (jit_direct_mem_enabled() && is_direct_user_mem_operand(&instr->src1)) {
                emit_direct_mem_addr(buf, &instr->src1);
                emit_direct_mem_load_to_x20(buf, instr->src1.size);
                emit_store_x20_to_gpr_sized(buf, &instr->dst);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_load_operand_lazy);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_STORE: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            if (instr->src2.type == HB_OP_REG && !is_gpr_reg_operand(&instr->src2))
                return emit_interp_ir_helper(buf, instr);
            if (jit_direct_mem_enabled() && is_direct_user_mem_operand(&instr->src1) &&
                (instr->src2.type == HB_OP_REG || instr->src2.type == HB_OP_IMM)) {
                if (instr->src2.type == HB_OP_REG) {
                    if (!emit_load_gpr_sized_to_x20(buf, &instr->src2))
                        return HB_ERR_INTERNAL;
                } else {
                    emit_mov_imm64(buf, 20, (uint64_t)instr->src2.imm);
                }
                emit_direct_mem_addr(buf, &instr->src1);
                emit_direct_mem_store_from_x20(buf, instr->src1.size);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_store_operand_lazy);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_PUSH: {
            if (instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM)
                return emit_interp_ir_helper(buf, instr);
            emit_load_operand(buf, &instr->src1);
            emit_mov_reg(buf, 1, 20); /* X1 = value */
            emit_mov_reg(buf, 0, 19); /* X0 = ctx */
            emit_call_helper(buf, (void*)hb_jit_helper_push);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_POP: {
            if (instr->dst.type != HB_OP_REG)
                return emit_interp_ir_helper(buf, instr);
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_pop);
            emit_return_if_helper_failed(buf);
            emit_mov_reg(buf, 20, 0); /* result in X0 */
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_CALL: {
            if (instr->src1.type != HB_OP_NONE) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_call_operand);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            /* Push first so stack faults do not commit the branch target. */
            emit_mov_imm64(buf, 1, instr->guest_addr + instr->guest_len); /* ret addr */
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_push);
            emit_return_if_helper_failed(buf);
            emit_mov_imm64(buf, 20, instr->target);
            emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
            return HB_OK;
        }

        case HB_IR_RET: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_pop);
            emit_return_if_helper_failed(buf);
            emit_str_x(buf, 0, 19, (uint32_t)offsetof(hb_context_t, pc));
            if (instr->src1.type == HB_OP_IMM && instr->src1.imm) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->src1.imm);
                emit_call_helper(buf, (void*)hb_jit_helper_adjust_stack);
                emit_return_if_helper_failed(buf);
            }
            return HB_OK;
        }

        case HB_IR_JMP: {
            if (instr->src1.type != HB_OP_NONE) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_jmp_operand);
                emit_return_if_helper_failed(buf);
                return HB_OK;
            }
            emit_mov_imm64(buf, 20, instr->target);
            emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
            return HB_OK;
        }

        case HB_IR_Jcc: {
            /* Layout:
             *   helper eval + cmp x0, #0
             *   b.ne target     (4 bytes)
             *   mov_imm64 fallthrough (16 bytes)
             *   str_x fallthrough (4 bytes)
             *   b skip_target   (4 bytes)
             *   mov_imm64 target (16 bytes)
             *   str_x target    (4 bytes)
             * Total fallthrough+skip = 24 bytes from b.cond start.
             * Target path starts at offset 28 from b.cond.
             * b at offset 24 must skip 24 bytes to land after target path.
             */
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_call_helper(buf, (void*)hb_jit_helper_eval_cond_lazy);
            emit_return_if_helper_failed(buf);
            emit_cmp_imm(buf, 0, 0);
            emit_bcond(buf, 1, 28);      /* taken -> target mov_imm64 */
            emit_mov_imm64(buf, 21, instr->guest_addr + instr->guest_len);
            emit_str_x(buf, 21, 19, (uint32_t)offsetof(hb_context_t, pc));
            emit_b(buf, 24);             /* skip target path (20 bytes) + 4 padding */
            emit_mov_imm64(buf, 20, instr->target);
            emit_str_x(buf, 20, 19, (uint32_t)offsetof(hb_context_t, pc));
            return HB_OK;
        }

        case HB_IR_LOOP:
        case HB_IR_JRCXZ: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_loop_branch);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_SETcc: {
            if (instr->dst.type != HB_OP_REG || instr->dst.reg_offset != 0) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
                emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_setcc_operand_lazy);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_setcc_lazy);
            return HB_OK;
        }

        case HB_IR_CMOVcc: {
            if (instr->dst.type != HB_OP_REG) return HB_ERR_INTERNAL;
            if (instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
                emit_mov_imm64(buf, 2, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_cmovcc_operand_lazy);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->cc);
            emit_mov_imm64(buf, 2, (uint64_t)instr->dst.reg);
            emit_mov_imm64(buf, 3, instr->src1.type == HB_OP_REG ? 1ULL : 0ULL);
            emit_mov_imm64(buf, 4, instr->src1.type == HB_OP_REG ? (uint64_t)instr->src1.reg : (uint64_t)instr->src1.imm);
            emit_mov_imm64(buf, 5, (uint64_t)instr->dst.size);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_cmovcc_lazy);
            return HB_OK;
        }

        case HB_IR_BSF:
        case HB_IR_TZCNT:
        case HB_IR_LZCNT:
        case HB_IR_BSR: {
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_bit_scan);
            return HB_OK;
        }

        case HB_IR_MOV_SEG:
        case HB_IR_BT:
        case HB_IR_BTS:
        case HB_IR_BTR:
        case HB_IR_BTC:
        case HB_IR_CMPXCHG:
        case HB_IR_CMPXCHG8B:
        case HB_IR_XCHG:
        case HB_IR_XADD:
        case HB_IR_PUSHF:
        case HB_IR_POPF:
        case HB_IR_CWD:
        case HB_IR_MOVS:
        case HB_IR_CMPS:
        case HB_IR_LODS:
        case HB_IR_SCAS:
        case HB_IR_STOS:
        case HB_IR_TRUNC:
        case HB_IR_BSWAP:
        case HB_IR_XMM_AND:
        case HB_IR_XMM_QWORD_LANE_MOV:
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
        case HB_IR_PSHUF:
        case HB_IR_FSHUF:
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
        case HB_IR_CVTSS2SD:
        case HB_IR_CVTSD2SS:
        case HB_IR_CVTSI2SD:
        case HB_IR_CVTSI2SS:
        case HB_IR_FSQRT:
        case HB_IR_FRSQRT:
        case HB_IR_FRCP:
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
        case HB_IR_CVTTSD2SI:
        case HB_IR_CVTTSS2SI:
        case HB_IR_PADD:
        case HB_IR_PSUB:
        case HB_IR_X87_FLD:
        case HB_IR_X87_FST:
        case HB_IR_X87_FSTP:
        case HB_IR_X87_FILD:
        case HB_IR_X87_FISTP:
        case HB_IR_X87_FLDCW:
        case HB_IR_X87_FNSTCW:
        case HB_IR_X87_FNSTSW:
        case HB_IR_X87_FADD:
        case HB_IR_X87_FMUL:
        case HB_IR_X87_FCOM:
        case HB_IR_X87_FCOMP:
        case HB_IR_X87_FSUB:
        case HB_IR_X87_FSUBR:
        case HB_IR_X87_FDIV:
        case HB_IR_X87_FDIVR:
        case HB_IR_X87_FADDP:
        case HB_IR_X87_FMULP:
        case HB_IR_X87_FCOMPP:
        case HB_IR_X87_FSUBP:
        case HB_IR_X87_FSUBRP:
        case HB_IR_X87_FDIVP:
        case HB_IR_X87_FDIVRP:
        case HB_IR_X87_FXCH:
        case HB_IR_X87_FRNDINT:
        case HB_IR_X87_FNCLEX:
        case HB_IR_X87_FNINIT:
        case HB_IR_HOST_CALL:
            return emit_interp_ir_helper(buf, instr);

        case HB_IR_UNSUPPORTED:
        case HB_IR_FAULT:
            return HB_ERR_UNSUPPORTED_OPCODE;

        default:
            return HB_ERR_UNSUPPORTED_OPCODE;
    }
}

/* --- Helper implementations --- */
uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr) {
    uint64_t val = 0;
    if (!ctx) return 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    hb_result_t r = hb_memory_read_u64(ctx->memory, addr, &val);
    ctx->last_result = r;
    return val;
}

static uint64_t hb_jit_helper_load_sized_value(hb_context_t* ctx, uint64_t addr, hb_size_t size) {
    uint64_t val = 0;
    hb_result_t r = HB_OK;
    if (!ctx) return 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    switch (size) {
        case HB_SIZE_8: {
            uint8_t v = 0;
            r = hb_memory_read_u8(ctx->memory, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_16: {
            uint16_t v = 0;
            r = hb_memory_read_u16(ctx->memory, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_32: {
            uint32_t v = 0;
            r = hb_memory_read_u32(ctx->memory, addr, &v);
            val = v;
            break;
        }
        case HB_SIZE_64:
        default:
            r = hb_memory_read_u64(ctx->memory, addr, &val);
            break;
    }
    ctx->last_result = r;
    return val;
}

void hb_jit_helper_load_to_reg_sized(hb_context_t* ctx, uint64_t addr,
                                     uint64_t dst_reg, uint64_t dst_size,
                                     uint64_t dst_reg_offset) {
    hb_ir_operand_t dst;
    hb_size_t size = dst_size ? (hb_size_t)dst_size : HB_SIZE_64;
    uint64_t val = hb_jit_helper_load_sized_value(ctx, addr, size);
    if (!ctx || ctx->last_result != HB_OK) return;

    memset(&dst, 0, sizeof(dst));
    dst.type = HB_OP_REG;
    dst.reg = (uint8_t)dst_reg;
    dst.size = size;
    dst.reg_offset = (uint8_t)dst_reg_offset;
    ctx->last_result = hb_flags_write_operand_value(ctx, &dst, val);
}

void hb_jit_helper_exec_load_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &val);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &instr->dst, val);
    ctx->last_result = r;
}

void hb_jit_helper_exec_store_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src2, &val);
    if (r == HB_OK) r = hb_flags_write_operand_value(ctx, &instr->src1, val);
    ctx->last_result = r;
}

void hb_jit_helper_store_u64(hb_context_t* ctx, uint64_t addr, uint64_t val) {
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    ctx->last_result = hb_memory_write_u64(ctx->memory, addr, val);
}

void hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr, uint64_t val, uint64_t size) {
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    switch ((hb_size_t)size) {
        case HB_SIZE_8:
            ctx->last_result = hb_memory_write_u8(ctx->memory, addr, (uint8_t)val);
            break;
        case HB_SIZE_16:
            ctx->last_result = hb_memory_write_u16(ctx->memory, addr, (uint16_t)val);
            break;
        case HB_SIZE_32:
            ctx->last_result = hb_memory_write_u32(ctx->memory, addr, (uint32_t)val);
            break;
        case HB_SIZE_64:
        default:
            ctx->last_result = hb_memory_write_u64(ctx->memory, addr, val);
            break;
    }
}

uint64_t hb_jit_helper_pop(hb_context_t* ctx) {
    if (!ctx) return 0;
    uint64_t val = 0;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return 0;
    }
    hb_result_t r;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t val32 = 0;
        r = hb_memory_read_u32(ctx->memory, ctx->regs.x86.esp, &val32);
        val = val32;
    } else {
        r = hb_memory_read_u64(ctx->memory, ctx->regs.x64.rsp, &val);
    }
    ctx->last_result = r;
    if (r != HB_OK) return 0;
    if (ctx->mode == HB_MODE_32BIT)
        ctx->regs.x86.esp += 4;
    else
        ctx->regs.x64.rsp += 8;
    return val;
}

void hb_jit_helper_push(hb_context_t* ctx, uint64_t val) {
    if (!ctx) return;
    if (!ctx->memory) {
        ctx->last_result = HB_ERR_MEMORY_FAULT;
        return;
    }
    hb_result_t r;
    if (ctx->mode == HB_MODE_32BIT) {
        uint32_t new_esp = ctx->regs.x86.esp - 4;
        r = hb_memory_write_u32(ctx->memory, new_esp, (uint32_t)val);
        if (r == HB_OK) ctx->regs.x86.esp = new_esp;
    } else {
        uint64_t new_rsp = ctx->regs.x64.rsp - 8;
        r = hb_memory_write_u64(ctx->memory, new_rsp, val);
        if (r == HB_OK) ctx->regs.x64.rsp = new_rsp;
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_call_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t target = instr ? instr->target : 0;
    uint64_t ret_addr;
    hb_result_t r;
    if (!ctx || !instr) return;
    if (instr->src1.type != HB_OP_NONE) {
        r = hb_flags_read_operand_value(ctx, &instr->src1, &target);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
    }
    if (!target) {
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return;
    }
    ret_addr = instr->guest_addr + instr->guest_len;
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
        ctx->last_result = r;
        return;
    }
    ctx->pc = target;
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rip = target;
    else ctx->regs.x86.eip = (uint32_t)target;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_jmp_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t target = instr ? instr->target : 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    if (instr->src1.type != HB_OP_NONE) {
        r = hb_flags_read_operand_value(ctx, &instr->src1, &target);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
    }
    if (!target) {
        ctx->last_result = HB_ERR_EXEC_FAULT;
        return;
    }
    ctx->pc = target;
    if (ctx->mode == HB_MODE_64BIT) ctx->regs.x64.rip = target;
    else ctx->regs.x86.eip = (uint32_t)target;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta) {
    if (!ctx) return;
    if (ctx->mode == HB_MODE_32BIT)
        ctx->regs.x86.esp += (uint32_t)delta;
    else
        ctx->regs.x64.rsp += delta;
    ctx->last_result = HB_OK;
}

uint64_t hb_jit_helper_call(hb_context_t* ctx, uint64_t target, uint64_t ret_addr) {
    (void)ctx; (void)target; (void)ret_addr;
    return 0;
}

uint64_t hb_jit_helper_exec_binop_lazy(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                       uint64_t src1_reg, uint64_t src2_is_reg,
                                       uint64_t src2_value, uint64_t size) {
    return hb_flags_exec_binop(ctx, (hb_ir_op_t)op, dst_reg, 0, src1_reg, 0,
                               src2_is_reg != 0, src2_value, 0, (hb_size_t)size);
}

void hb_jit_helper_exec_cmp_test_lazy(hb_context_t* ctx, uint64_t op,
                                      uint64_t src1_is_reg, uint64_t src1_value,
                                      uint64_t src2_is_reg, uint64_t src2_value,
                                      uint64_t size) {
    hb_flags_exec_cmp_test(ctx, (hb_ir_op_t)op, src1_is_reg != 0, src1_value, 0,
                           src2_is_reg != 0, src2_value, 0, (hb_size_t)size);
}

void hb_jit_helper_exec_cmp_test_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t lhs = 0, rhs = 0;
    uint64_t result = 0;
    hb_result_t r;
    if (!ctx || !instr) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &lhs);
    if (r != HB_OK) { ctx->last_result = r; return; }
    r = hb_flags_read_operand_value(ctx, &instr->src2, &rhs);
    if (r != HB_OK) { ctx->last_result = r; return; }
    result = (instr->op == HB_IR_TEST) ? (lhs & rhs) : (lhs - rhs);
    hb_lazy_flags_note(ctx, instr->op == HB_IR_TEST ? HB_LAZY_FLAGS_TEST : HB_LAZY_FLAGS_CMP,
                       instr->src1.size, lhs, rhs, result, 0);
    ctx->last_result = HB_OK;
}

uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc) {
    bool value = false;
    hb_result_t r = hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value);
    if (ctx) ctx->last_result = r;
    if (r != HB_OK) return 0;
    return value ? 1 : 0;
}

static uint64_t hb_jit_trunc_to_size(uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8: return value & 0xffu;
        case HB_SIZE_16: return value & 0xffffu;
        case HB_SIZE_32: return value & 0xffffffffu;
        case HB_SIZE_64:
        default: return value;
    }
}

static int64_t hb_jit_sign_extend_from_size(uint64_t value, hb_size_t size) {
    switch (size) {
        case HB_SIZE_8:  return (int8_t)value;
        case HB_SIZE_16: return (int16_t)value;
        case HB_SIZE_32: return (int32_t)value;
        case HB_SIZE_64:
        default:         return (int64_t)value;
    }
}

void hb_jit_helper_exec_mul_div_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    hb_result_t r = HB_OK;
    uint64_t lhs = 0, rhs = 0;
    hb_size_t size;
    if (!ctx || !instr) return;

    switch (instr->op) {
        case HB_IR_IMUL:
            if (instr->dst.type == HB_OP_NONE) {
                r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
                if (r != HB_OK) break;
                size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
                if (size == HB_SIZE_8) {
                    int16_t result = (int16_t)((int8_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int8_t)rhs);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT8_MIN || result > INT8_MAX);
                } else if (size == HB_SIZE_16) {
                    int32_t result = (int32_t)((int16_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int16_t)rhs);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                    ctx->flags.cf = ctx->flags.of = (result < INT16_MIN || result > INT16_MAX);
                } else if (size == HB_SIZE_32) {
                    int64_t result = (int64_t)(int32_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (int64_t)(int32_t)rhs;
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                    ctx->flags.cf = ctx->flags.of = (result < INT32_MIN || result > INT32_MAX);
                } else if (size == HB_SIZE_64) {
                    __int128 result = (__int128)(int64_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (__int128)(int64_t)rhs;
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                    ctx->flags.cf = ctx->flags.of = (result < (__int128)INT64_MIN || result > (__int128)INT64_MAX);
                } else r = HB_ERR_UNSUPPORTED_OPCODE;
                hb_lazy_flags_clear(ctx);
                break;
            }
            r = hb_flags_read_operand_value(ctx, &instr->src1, &lhs);
            if (r != HB_OK) break;
            r = hb_flags_read_operand_value(ctx, &instr->src2, &rhs);
            if (r != HB_OK) break;
            size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, hb_jit_trunc_to_size(lhs * rhs, size), size);
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_MUL:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (size == HB_SIZE_8) {
                uint16_t result = (uint16_t)(uint8_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint16_t)(uint8_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, result, HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 8) != 0);
            } else if (size == HB_SIZE_16) {
                uint32_t result = (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint32_t)(uint16_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)result, HB_SIZE_16);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)(result >> 16), HB_SIZE_16);
                ctx->flags.cf = ctx->flags.of = ((result >> 16) != 0);
            } else if (size == HB_SIZE_32) {
                uint64_t result = (uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX) * (uint64_t)(uint32_t)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)result, HB_SIZE_32);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)(result >> 32), HB_SIZE_32);
                ctx->flags.cf = ctx->flags.of = ((result >> 32) != 0);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 result = (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX) * (unsigned __int128)rhs;
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)result, HB_SIZE_64);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)(result >> 64), HB_SIZE_64);
                ctx->flags.cf = ctx->flags.of = ((uint64_t)(result >> 64) != 0);
            } else r = HB_ERR_UNSUPPORTED_OPCODE;
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_DIV:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (!rhs) { r = HB_ERR_EXEC_FAULT; break; }
            if (size == HB_SIZE_8) {
                uint16_t dividend = (uint16_t)(hb_context_read_reg_value(ctx, HB_REG_RAX) & 0xffffu);
                uint16_t quotient = dividend / (uint8_t)rhs;
                uint16_t remainder = dividend % (uint8_t)rhs;
                if (quotient > UINT8_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient, HB_SIZE_16);
            } else if (size == HB_SIZE_16) {
                uint32_t dividend = ((uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 16) |
                                    (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                uint32_t quotient = dividend / (uint16_t)rhs;
                uint32_t remainder = dividend % (uint16_t)rhs;
                if (quotient > UINT16_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)quotient, HB_SIZE_16);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)remainder, HB_SIZE_16);
            } else if (size == HB_SIZE_32) {
                uint64_t dividend = ((uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
                                    (uint64_t)(uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                uint64_t quotient = dividend / (uint32_t)rhs;
                uint64_t remainder = dividend % (uint32_t)rhs;
                if (quotient > UINT32_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)quotient, HB_SIZE_32);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)remainder, HB_SIZE_32);
            } else if (size == HB_SIZE_64) {
                unsigned __int128 dividend = ((unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RDX) << 64) |
                                             (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX);
                unsigned __int128 quotient = dividend / rhs;
                unsigned __int128 remainder = dividend % rhs;
                if (quotient > UINT64_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
            } else r = HB_ERR_UNSUPPORTED_OPCODE;
            hb_lazy_flags_clear(ctx);
            break;

        case HB_IR_IDIV:
            r = hb_flags_read_operand_value(ctx, &instr->src1, &rhs);
            if (r != HB_OK) break;
            size = instr->src1.size ? instr->src1.size : HB_SIZE_32;
            rhs = hb_jit_trunc_to_size(rhs, size);
            if (!rhs) { r = HB_ERR_EXEC_FAULT; break; }
            {
                int64_t divisor = hb_jit_sign_extend_from_size(rhs, size);
                if (!divisor) { r = HB_ERR_EXEC_FAULT; break; }
                if (size == HB_SIZE_8) {
                    int16_t dividend = (int16_t)(hb_context_read_reg_value(ctx, HB_REG_RAX) & 0xffffu);
                    int64_t quotient = dividend / (int8_t)divisor;
                    int64_t remainder = dividend % (int8_t)divisor;
                    if (quotient < INT8_MIN || quotient > INT8_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, ((uint16_t)(uint8_t)remainder << 8) | (uint8_t)quotient, HB_SIZE_16);
                } else if (size == HB_SIZE_16) {
                    int32_t dividend = (int32_t)(((uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 16) |
                                                 (uint32_t)(uint16_t)hb_context_read_reg_value(ctx, HB_REG_RAX));
                    int64_t quotient = dividend / (int16_t)divisor;
                    int64_t remainder = dividend % (int16_t)divisor;
                    if (quotient < INT16_MIN || quotient > INT16_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint16_t)quotient, HB_SIZE_16);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint16_t)remainder, HB_SIZE_16);
                } else if (size == HB_SIZE_32) {
                    int64_t dividend = ((int64_t)(int32_t)hb_context_read_reg_value(ctx, HB_REG_RDX) << 32) |
                                       (uint32_t)hb_context_read_reg_value(ctx, HB_REG_RAX);
                    int64_t quotient = dividend / (int32_t)divisor;
                    int64_t remainder = dividend % (int32_t)divisor;
                    if (quotient < INT32_MIN || quotient > INT32_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint32_t)quotient, HB_SIZE_32);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint32_t)remainder, HB_SIZE_32);
                } else if (size == HB_SIZE_64) {
                    unsigned __int128 bits = ((unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RDX) << 64) |
                                             (unsigned __int128)hb_context_read_reg_value(ctx, HB_REG_RAX);
                    __int128 dividend = (__int128)bits;
                    __int128 quotient = dividend / (int64_t)divisor;
                    __int128 remainder = dividend % (int64_t)divisor;
                    if (quotient < (__int128)INT64_MIN || quotient > (__int128)INT64_MAX) { r = HB_ERR_EXEC_FAULT; break; }
                    hb_context_write_reg_value_sized(ctx, HB_REG_RAX, (uint64_t)quotient, HB_SIZE_64);
                    hb_context_write_reg_value_sized(ctx, HB_REG_RDX, (uint64_t)remainder, HB_SIZE_64);
                } else r = HB_ERR_UNSUPPORTED_OPCODE;
            }
            hb_lazy_flags_clear(ctx);
            break;

        default:
            r = HB_ERR_UNSUPPORTED_OPCODE;
            break;
    }
    ctx->last_result = r;
}

void hb_jit_helper_exec_loop_branch(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t count = 0;
    uint64_t next = 0;
    bool taken = false;
    hb_result_t r;
    hb_size_t size;
    if (!ctx || !instr) return;
    size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
    r = hb_flags_read_operand_value(ctx, &instr->dst, &count);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    count = hb_jit_trunc_to_size(count, size);
    if (instr->op == HB_IR_JRCXZ) {
        taken = count == 0;
    } else {
        int kind = (int)instr->src1.imm;
        next = hb_jit_trunc_to_size(count - 1, size);
        r = hb_flags_write_operand_value(ctx, &instr->dst, next);
        if (r != HB_OK) {
            ctx->last_result = r;
            return;
        }
        taken = next != 0 &&
                (kind == 2 || (kind == 1 ? ctx->flags.zf : !ctx->flags.zf));
    }
    ctx->pc = taken ? instr->target : instr->guest_addr + instr->guest_len;
    if (ctx->mode == HB_MODE_32BIT) ctx->regs.x86.eip = (uint32_t)ctx->pc;
    else ctx->regs.x64.rip = ctx->pc;
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_interp_ir(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) {
        if (ctx) ctx->last_result = HB_ERR_INVALID_ARG;
        return;
    }
    ctx->last_result = hb_interpreter_exec_one_for_jit(ctx, instr);
}

void hb_jit_helper_exec_setcc_lazy(hb_context_t* ctx, uint64_t cc, uint64_t dst_reg) {
    bool value = false;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK) return;
    hb_context_write_reg_value(ctx, dst_reg, value ? 1 : 0);
}

void hb_jit_helper_exec_cmovcc_lazy(hb_context_t* ctx, uint64_t cc, uint64_t dst_reg,
                                    uint64_t src_is_reg, uint64_t src_value, uint64_t size) {
    bool value = false;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK || !value) return;
    uint64_t src = src_is_reg ? hb_context_read_reg_value(ctx, src_value) : src_value;
    hb_context_write_reg_value_sized(ctx, dst_reg, src, (hb_size_t)size);
}

void hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    (void)hb_flags_write_operand_value(ctx, &instr->dst, src);
}

void hb_jit_helper_exec_binop_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) return;
    (void)hb_flags_exec_binop_operand(ctx, instr->op, &instr->dst, &instr->src1, &instr->src2, NULL);
}

void hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr) return;
    (void)hb_flags_exec_double_shift_operand(ctx, instr->op, &instr->dst, &instr->src1, &instr->src2, NULL);
}

void hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr) {
    bool value = false;
    if (!ctx || !instr) return;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK) return;
    (void)hb_flags_write_operand_value(ctx, &instr->dst, value ? 1 : 0);
}

void hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr) {
    bool value = false;
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value) != HB_OK || !value) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    (void)hb_flags_write_operand_value(ctx, &instr->dst, src);
}

void hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    (void)hb_flags_write_operand_value(ctx, &instr->dst, ~src);
}

void hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t src = 0;
    if (!ctx || !instr) return;
    hb_size_t size = instr->dst.size ? instr->dst.size : instr->src1.size;
    if (!size) size = HB_SIZE_64;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &src) != HB_OK) return;
    uint64_t mask = (size == HB_SIZE_8) ? 0xffULL :
                    (size == HB_SIZE_16) ? 0xffffULL :
                    (size == HB_SIZE_32) ? 0xffffffffULL : ~0ULL;
    uint64_t lhs = 0;
    uint64_t rhs = src & mask;
    uint64_t result = (0 - rhs) & mask;
    if (hb_flags_write_operand_value(ctx, &instr->dst, result) != HB_OK) return;
    hb_lazy_flags_note(ctx, HB_LAZY_FLAGS_SUB, size, lhs, rhs, result, 0);
}

void hb_jit_helper_exec_bit_scan(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    uint64_t val = 0;
    uint64_t result = 0;
    uint64_t width;
    hb_size_t size;
    hb_result_t r;

    if (!ctx || !instr || instr->dst.type != HB_OP_REG) return;
    r = hb_flags_read_operand_value(ctx, &instr->src1, &val);
    if (r != HB_OK) {
        ctx->last_result = r;
        return;
    }
    size = instr->dst.size ? instr->dst.size : HB_SIZE_64;
    val = hb_jit_trunc_to_size(val, size);
    width = (size == HB_SIZE_32) ? 32 : (size == HB_SIZE_16) ? 16 :
            (size == HB_SIZE_8) ? 8 : 64;

    hb_lazy_flags_clear(ctx);
    if (instr->op == HB_IR_BSF) {
        ctx->flags.zf = (val == 0);
        if (val) {
            while (((val >> result) & 1ULL) == 0) result++;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        }
    } else if (instr->op == HB_IR_TZCNT) {
        result = width;
        if (val) {
            result = 0;
            while (((val >> result) & 1ULL) == 0) result++;
        }
        hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        ctx->flags.zf = (result == 0);
        ctx->flags.cf = (val == 0);
    } else if (instr->op == HB_IR_LZCNT) {
        result = width;
        if (val) {
            result = 0;
            for (int64_t bit = (int64_t)width - 1; bit >= 0 && ((val >> bit) & 1ULL) == 0; bit--)
                result++;
        }
        hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        ctx->flags.zf = (result == 0);
        ctx->flags.cf = (val == 0);
    } else if (instr->op == HB_IR_BSR) {
        ctx->flags.zf = (val == 0);
        if (val) {
            result = width - 1;
            while (((val >> result) & 1ULL) == 0) result--;
            hb_context_write_reg_value_sized(ctx, instr->dst.reg, result, size);
        }
    }
    ctx->last_result = HB_OK;
}

void hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr) {
    if (!ctx || !instr || instr->dst.type != HB_OP_REG) return;

    uint64_t value = 0;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &value) != HB_OK) return;

    switch (instr->src1.size) {
        case HB_SIZE_8:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int8_t)value
                                                     : (value & 0xffULL);
            break;
        case HB_SIZE_16:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int16_t)value
                                                     : (value & 0xffffULL);
            break;
        case HB_SIZE_32:
            value = (instr->op == HB_IR_SIGN_EXTEND) ? (uint64_t)(int64_t)(int32_t)value
                                                     : (value & 0xffffffffULL);
            break;
        case HB_SIZE_64:
        default:
            break;
    }

    hb_context_write_reg_value_sized(ctx, (uint64_t)instr->dst.reg, value, instr->dst.size);
}

void hb_jit_helper_lahf(hb_context_t* ctx) {
    if (!ctx) return;
    if (hb_lazy_flags_materialize(ctx, HB_FLAG_BIT_SF | HB_FLAG_BIT_ZF |
                                      HB_FLAG_BIT_AF | HB_FLAG_BIT_PF |
                                      HB_FLAG_BIT_CF) != HB_OK) return;
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
}

void hb_jit_helper_sahf(hb_context_t* ctx) {
    if (!ctx) return;
    uint8_t ah = ctx->mode == HB_MODE_32BIT ?
        (uint8_t)((ctx->regs.x86.eax >> 8) & 0xffU) :
        (uint8_t)((ctx->regs.x64.rax >> 8) & 0xffU);
    hb_lazy_flags_clear(ctx);
    ctx->flags.sf = (ah & 0x80) != 0;
    ctx->flags.zf = (ah & 0x40) != 0;
    ctx->flags.af = (ah & 0x10) != 0;
    ctx->flags.pf = (ah & 0x04) != 0;
    ctx->flags.cf = (ah & 0x01) != 0;
}

static uint64_t jit_trunc_val(uint64_t v, uint64_t sz) {
    switch (sz) {
        case 1: return v & 0xFFULL;
        case 2: return v & 0xFFFFULL;
        case 4: return v & 0xFFFFFFFFULL;
        default: return v;
    }
}

static unsigned jit_msb_bit(uint64_t sz) {
    switch (sz) {
        case 1: return 7;
        case 2: return 15;
        case 4: return 31;
        default: return 63;
    }
}

static uint64_t jit_read_reg(hb_context_t* ctx, uint64_t idx) {
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
        default: return 0;
    }
}

static void jit_write_reg(hb_context_t* ctx, uint64_t idx, uint64_t val) {
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
        default: return;
    }
}

void hb_jit_helper_cpuid(hb_context_t* ctx) {
    if (!ctx) return;

    uint32_t leaf = (uint32_t)jit_read_reg(ctx, HB_REG_RAX);
    uint32_t subleaf = (uint32_t)jit_read_reg(ctx, HB_REG_RCX);
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
            /* Conservative feature surface: do not advertise AVX/OSXSAVE or
             * other optional opcode families until the bridge implements them.
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

    jit_write_reg(ctx, HB_REG_RAX, eax);
    jit_write_reg(ctx, HB_REG_RBX, ebx);
    jit_write_reg(ctx, HB_REG_RCX, ecx);
    jit_write_reg(ctx, HB_REG_RDX, edx);
}

void hb_jit_helper_xgetbv(hb_context_t* ctx) {
    if (!ctx) return;

    uint32_t ecx = (uint32_t)jit_read_reg(ctx, HB_REG_RCX);
    uint64_t xcr0 = ecx == 0 ? 0x3ULL : 0;

    jit_write_reg(ctx, HB_REG_RAX, (uint32_t)xcr0);
    jit_write_reg(ctx, HB_REG_RDX, (uint32_t)(xcr0 >> 32));
}

uint64_t hb_jit_helper_exec_shift(hb_context_t* ctx, uint64_t op, uint64_t dst_reg,
                                  uint64_t src1_reg, uint64_t src2_type,
                                  uint64_t src2_value, uint64_t size) {
    uint64_t a = jit_read_reg(ctx, src1_reg);
    uint64_t shift = src2_type == 1 ? jit_read_reg(ctx, src2_value) : src2_value;
    shift &= (size == 8) ? 0x3FULL : 0x1FULL;
    if (shift == 0) {
        jit_write_reg(ctx, dst_reg, a);
        return a;
    }

    unsigned msb = jit_msb_bit(size);
    uint64_t mask = (msb == 63) ? ~0ULL : ((1ULL << (msb + 1)) - 1ULL);
    uint64_t ta = jit_trunc_val(a, size);
    uint64_t result = 0;

    if (op == HB_IR_SHL) {
        result = (ta << shift) & mask;
        ctx->flags.cf = (shift <= msb + 1) ? (((ta >> (msb + 1 - shift)) & 1ULL) != 0) : false;
        ctx->flags.of = (shift == 1) ? ((((result >> msb) & 1ULL) != 0) != ctx->flags.cf) : false;
    } else if (op == HB_IR_SHR) {
        result = ta >> shift;
        ctx->flags.cf = ((ta >> (shift - 1)) & 1ULL) != 0;
        ctx->flags.of = false;
    } else {
        int64_t sa = (int64_t)(ta << (63 - msb)) >> (63 - msb);
        result = (uint64_t)(sa >> shift) & mask;
        ctx->flags.cf = ((ta >> (shift - 1)) & 1ULL) != 0;
        ctx->flags.of = false;
    }

    jit_write_reg(ctx, dst_reg, result);
    ctx->flags.zf = (result == 0);
    ctx->flags.sf = ((result >> msb) & 1ULL) != 0;
    return result;
}

/* --- Public API --- */
struct hb_arm64_codegen {
    hb_context_t* ctx;
};

hb_arm64_codegen_t* hb_arm64_codegen_create(hb_context_t* ctx) {
    hb_arm64_codegen_t* cg = calloc(1, sizeof(hb_arm64_codegen_t));
    if (!cg) return NULL;
    cg->ctx = ctx;
    return cg;
}

void hb_arm64_codegen_destroy(hb_arm64_codegen_t* cg) {
    free(cg);
}

hb_result_t hb_arm64_codegen_instr(hb_arm64_codegen_t* cg, const hb_ir_instr_t* instr, hb_codegen_buffer_t* out) {
    (void)cg;
    if (!instr || !out) return HB_ERR_INVALID_ARG;
    return codegen_instr(out, instr);
}

hb_result_t hb_arm64_codegen_copy_scan_counted_loop(hb_arm64_codegen_t* cg,
                                                    const hb_ir_block_t* body,
                                                    const hb_ir_block_t* guard,
                                                    hb_codegen_buffer_t* out) {
    (void)cg;
    if (!body || !guard || !out) return HB_ERR_INVALID_ARG;
    emit_prologue(out);
    if (!emit_copy_scan_counted_loop_block(out, body, guard)) {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_bounded_scan_loop(hb_arm64_codegen_t* cg,
                                               const hb_ir_block_t* guard,
                                               const hb_ir_block_t* body,
                                               hb_codegen_buffer_t* out) {
    (void)cg;
    if (!guard || !body || !out) return HB_ERR_INVALID_ARG;
    emit_prologue(out);
    if (!emit_bounded_scan_loop_block(out, guard, body)) {
        return HB_ERR_UNSUPPORTED_FEATURE;
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_block_with_cfg(hb_arm64_codegen_t* cg, const hb_ir_block_t* block,
                                            const hb_ir_cfg_t* cfg, hb_codegen_buffer_t* out) {
    (void)cg;
    if (!block || !out) return HB_ERR_INVALID_ARG;
    emit_prologue(out);
    if (cfg && block->instr_count == 5) {
        const hb_ir_instr_t* jcc = &block->instrs[4];
        const hb_ir_block_t* guard = (jcc->op == HB_IR_Jcc)
            ? find_cfg_block_for_codegen(cfg, jcc->guest_addr + jcc->guest_len)
            : NULL;
        if (emit_copy_scan_counted_loop_block(out, block, guard)) {
            emit_epilogue(out);
            return HB_OK;
        }
    }
    if (emit_store_count_loop_block(out, block)) {
        emit_epilogue(out);
        return HB_OK;
    }
    if (emit_copy_scan_body_block(out, block)) {
        emit_epilogue(out);
        return HB_OK;
    }
    if (emit_hot_scalar_scan_loop(out, block)) {
        emit_epilogue(out);
        return HB_OK;
    }
    for (size_t i = 0; i < block->instr_count; i++) {
        if (i + 1 < block->instr_count &&
            emit_scalar_flags_jcc_pair(out, &block->instrs[i], &block->instrs[i + 1])) {
            i++;
            continue;
        }
        hb_result_t r = codegen_instr(out, &block->instrs[i]);
        if (r != HB_OK) return r;
    }
    emit_epilogue(out);
    return HB_OK;
}

hb_result_t hb_arm64_codegen_block(hb_arm64_codegen_t* cg, const hb_ir_block_t* block, hb_codegen_buffer_t* out) {
    return hb_arm64_codegen_block_with_cfg(cg, block, NULL, out);
}

hb_result_t hb_arm64_codegen_func(hb_arm64_codegen_t* cg, const hb_ir_func_t* func, hb_codegen_buffer_t* out) {
    (void)cg;
    if (!func || !func->cfg || !out) return HB_ERR_INVALID_ARG;
    for (size_t i = 0; i < func->cfg->block_count; i++) {
        hb_result_t r = hb_arm64_codegen_block(cg, func->cfg->blocks[i], out);
        if (r != HB_OK) return r;
    }
    return HB_OK;
}
