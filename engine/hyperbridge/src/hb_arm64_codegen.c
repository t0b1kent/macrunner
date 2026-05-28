#include "hb_codegen.h"
#include "hb_runtime.h"
#include "hb_memory.h"
#include "hb_flags.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

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

static void emit_str_x(hb_codegen_buffer_t* buf, int rt, int rn, uint32_t off) {
    /* STR Xt, [Xn, #off] */
    uint32_t imm12 = (off / 8) & 0xFFF;
    emit_u32(buf, 0xf9000000 | (imm12 << 10) | (rn << 5) | rt);
}

static void emit_mov_imm64(hb_codegen_buffer_t* buf, int rd, uint64_t val) {
    /* MOVZ + up to 3 MOVK */
    emit_u32(buf, 0xd2800000 | ((val & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2a00000 | (((val >> 16) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2c00000 | (((val >> 32) & 0xFFFF) << 5) | rd);
    emit_u32(buf, 0xf2e00000 | (((val >> 48) & 0xFFFF) << 5) | rd);
}

static void emit_blr(hb_codegen_buffer_t* buf, int rn) {
    emit_u32(buf, 0xd63f0000 | (rn << 5));
}

static void emit_bcond(hb_codegen_buffer_t* buf, int cond, int32_t off) {
    uint32_t imm19 = ((off / 4) & 0x7FFFF);
    emit_u32(buf, 0x54000000 | (imm19 << 5) | (cond & 0xF));
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

/* JIT helper declarations (implemented below) */
extern uint64_t hb_jit_helper_load_u64(hb_context_t* ctx, uint64_t addr);
extern void     hb_jit_helper_store_u64(hb_context_t* ctx, uint64_t addr, uint64_t val);
extern void     hb_jit_helper_store_sized(hb_context_t* ctx, uint64_t addr, uint64_t val, uint64_t size);
extern uint64_t hb_jit_helper_pop(hb_context_t* ctx);
extern void     hb_jit_helper_push(hb_context_t* ctx, uint64_t val);
extern void     hb_jit_helper_adjust_stack(hb_context_t* ctx, uint64_t delta);
extern uint64_t hb_jit_helper_call(hb_context_t* ctx, uint64_t target, uint64_t ret_addr);
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
extern void     hb_jit_helper_exec_double_shift_operand(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_setcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_cmovcc_operand_lazy(hb_context_t* ctx, uint64_t cc, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_extend_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_mov_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_not_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_exec_neg_operand_lazy(hb_context_t* ctx, const hb_ir_instr_t* instr);
extern void     hb_jit_helper_lahf(hb_context_t* ctx);
extern void     hb_jit_helper_sahf(hb_context_t* ctx);
extern void     hb_jit_helper_cpuid(hb_context_t* ctx);
extern void     hb_jit_helper_xgetbv(hb_context_t* ctx);

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
            if (!is_plain_gpr_reg_operand(&instr->dst) ||
                !is_plain_gpr_reg_operand(&instr->src1) ||
                (instr->src2.type == HB_OP_REG && !is_plain_gpr_reg_operand(&instr->src2)) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_binop_operand_lazy);
                return HB_OK;
            }
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

        case HB_IR_SHL:
        case HB_IR_SHR:
        case HB_IR_SAR:
        case HB_IR_ROL:
        case HB_IR_ROR: {
            if (instr->dst.type != HB_OP_REG || instr->src1.type != HB_OP_REG) return HB_ERR_INTERNAL;
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
            if ((instr->src1.type != HB_OP_REG && instr->src1.type != HB_OP_IMM) ||
                (instr->src2.type != HB_OP_REG && instr->src2.type != HB_OP_IMM)) return HB_ERR_INTERNAL;
            if ((instr->src1.type == HB_OP_REG && instr->src1.reg_offset != 0) ||
                (instr->src2.type == HB_OP_REG && instr->src2.reg_offset != 0)) {
                emit_mov_reg(buf, 0, 19);
                emit_mov_imm64(buf, 1, (uint64_t)(uintptr_t)instr);
                emit_call_helper(buf, (void*)hb_jit_helper_exec_cmp_test_operand_lazy);
                return HB_OK;
            }
            emit_mov_reg(buf, 0, 19);
            emit_mov_imm64(buf, 1, (uint64_t)instr->op);
            emit_mov_imm64(buf, 2, instr->src1.type == HB_OP_REG ? 1ULL : 0ULL);
            emit_mov_imm64(buf, 3, instr->src1.type == HB_OP_REG ? (uint64_t)instr->src1.reg : (uint64_t)instr->src1.imm);
            emit_mov_imm64(buf, 4, instr->src2.type == HB_OP_REG ? 1ULL : 0ULL);
            emit_mov_imm64(buf, 5, instr->src2.type == HB_OP_REG ? (uint64_t)instr->src2.reg : (uint64_t)instr->src2.imm);
            emit_mov_imm64(buf, 6, (uint64_t)instr->src1.size);
            emit_call_helper(buf, (void*)hb_jit_helper_exec_cmp_test_lazy);
            return HB_OK;
        }

        case HB_IR_LOAD: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            /* Compute address into X0, call helper, result in X0 */
            uint64_t addr = 0;
            if (instr->src1.mem.base < HB_REG_COUNT) {
                if (instr->src1.mem.base == HB_REG_RIP) {
                    addr = instr->guest_addr + instr->guest_len;
                } else {
                    size_t off = x64_reg_off(instr->src1.mem.base);
                    emit_ldr_x(buf, 20, 19, (uint32_t)off);
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
            /* X0 = ctx, X1 = addr */
            emit_mov_reg(buf, 0, 19);
            emit_mov_reg(buf, 1, 20);
            emit_call_helper(buf, (void*)hb_jit_helper_load_u64);
            emit_return_if_helper_failed(buf);
            emit_mov_reg(buf, 20, 0); /* helper result in X0 */
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_STORE: {
            if (instr->src1.type != HB_OP_MEM) return HB_ERR_INTERNAL;
            /* Compute address into X1, value into X2 */
            uint64_t addr = 0;
            if (instr->src1.mem.base < HB_REG_COUNT) {
                if (instr->src1.mem.base == HB_REG_RIP) {
                    addr = instr->guest_addr + instr->guest_len;
                } else {
                    size_t off = x64_reg_off(instr->src1.mem.base);
                    emit_ldr_x(buf, 20, 19, (uint32_t)off);
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
            emit_mov_reg(buf, 1, 20); /* X1 = addr */
            emit_load_operand(buf, &instr->src2);
            emit_mov_reg(buf, 2, 20); /* X2 = value */
            emit_mov_imm64(buf, 3, (uint64_t)instr->src1.size); /* X3 = size */
            emit_mov_reg(buf, 0, 19); /* X0 = ctx */
            emit_call_helper(buf, (void*)hb_jit_helper_store_sized);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_PUSH: {
            emit_load_operand(buf, &instr->src1);
            emit_mov_reg(buf, 1, 20); /* X1 = value */
            emit_mov_reg(buf, 0, 19); /* X0 = ctx */
            emit_call_helper(buf, (void*)hb_jit_helper_push);
            emit_return_if_helper_failed(buf);
            return HB_OK;
        }

        case HB_IR_POP: {
            emit_mov_reg(buf, 0, 19);
            emit_call_helper(buf, (void*)hb_jit_helper_pop);
            emit_return_if_helper_failed(buf);
            emit_mov_reg(buf, 20, 0); /* result in X0 */
            emit_store_operand(buf, &instr->dst);
            return HB_OK;
        }

        case HB_IR_CALL: {
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
    if (!ctx || !instr) return;
    if (hb_flags_read_operand_value(ctx, &instr->src1, &lhs) != HB_OK) return;
    if (hb_flags_read_operand_value(ctx, &instr->src2, &rhs) != HB_OK) return;
    result = (instr->op == HB_IR_TEST) ? (lhs & rhs) : (lhs - rhs);
    hb_lazy_flags_note(ctx, instr->op == HB_IR_TEST ? HB_LAZY_FLAGS_TEST : HB_LAZY_FLAGS_CMP,
                       instr->src1.size, lhs, rhs, result, 0);
}

uint64_t hb_jit_helper_eval_cond_lazy(hb_context_t* ctx, uint64_t cc) {
    bool value = false;
    hb_result_t r = hb_flags_eval_cond(ctx, (hb_cc_t)cc, &value);
    if (ctx) ctx->last_result = r;
    if (r != HB_OK) return 0;
    return value ? 1 : 0;
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

hb_result_t hb_arm64_codegen_block(hb_arm64_codegen_t* cg, const hb_ir_block_t* block, hb_codegen_buffer_t* out) {
    (void)cg;
    if (!block || !out) return HB_ERR_INVALID_ARG;
    emit_prologue(out);
    for (size_t i = 0; i < block->instr_count; i++) {
        hb_result_t r = codegen_instr(out, &block->instrs[i]);
        if (r != HB_OK) return r;
    }
    emit_epilogue(out);
    return HB_OK;
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
