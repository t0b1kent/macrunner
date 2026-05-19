#include "hb_lifter.h"
#include "hb_ir.h"
#include <string.h>

static hb_size_t size_from_dec(uint8_t sz) {
    switch (sz) {
        case 1: return HB_SIZE_8;
        case 2: return HB_SIZE_16;
        case 4: return HB_SIZE_32;
        case 8: return HB_SIZE_64;
        default: return HB_SIZE_32;
    }
}

static hb_ir_operand_t operand_from_dec(const hb_decoded_t* dec, int slot) {
    const hb_decoded_t* d = dec;
    bool is_reg = (slot == 1) ? d->op1.is_reg : (slot == 2) ? d->op2.is_reg : d->op3.is_reg;
    bool is_imm = (slot == 1) ? d->op1.is_imm : (slot == 2) ? d->op2.is_imm : d->op3.is_imm;
    bool is_mem = (slot == 1) ? d->op1.is_mem : (slot == 2) ? d->op2.is_mem : d->op3.is_mem;
    uint8_t sz_raw = (slot == 1) ? d->op1.size : (slot == 2) ? d->op2.size : d->op3.size;
    hb_size_t sz = size_from_dec(sz_raw);
    if (is_reg) {
        int reg = (slot == 1) ? d->op1.reg : (slot == 2) ? d->op2.reg : d->op3.reg;
        return hb_ir_reg((hb_reg_t)reg, sz);
    }
    if (is_imm) {
        int64_t imm = (slot == 1) ? d->op1.imm : (slot == 2) ? d->op2.imm : d->op3.imm;
        return hb_ir_imm(imm, sz);
    }
    if (is_mem) {
        int base = (slot == 1) ? d->op1.mem.base : (slot == 2) ? d->op2.mem.base : d->op3.mem.base;
        int index = (slot == 1) ? d->op1.mem.index : (slot == 2) ? d->op2.mem.index : d->op3.mem.index;
        uint8_t scale = (slot == 1) ? d->op1.mem.scale : (slot == 2) ? d->op2.mem.scale : d->op3.mem.scale;
        int64_t disp = (slot == 1) ? d->op1.mem.disp : (slot == 2) ? d->op2.mem.disp : d->op3.mem.disp;
        return hb_ir_mem(
            (hb_reg_t)(base >= 0 ? base : HB_REG_COUNT),
            (hb_reg_t)(index >= 0 ? index : HB_REG_COUNT),
            scale, disp, sz);
    }
    return hb_ir_none();
}

static hb_cc_t cc_from_dec(int cond) {
    switch (cond) {
        case HB_COND_E:  return HB_CC_E;
        case HB_COND_NE: return HB_CC_NE;
        case HB_COND_S:  return HB_CC_S;
        case HB_COND_NS: return HB_CC_NS;
        case HB_COND_G:  return HB_CC_G;
        case HB_COND_GE: return HB_CC_GE;
        case HB_COND_L:  return HB_CC_L;
        case HB_COND_LE: return HB_CC_LE;
        case HB_COND_A:  return HB_CC_A;
        case HB_COND_AE: return HB_CC_AE;
        case HB_COND_B:  return HB_CC_B;
        case HB_COND_BE: return HB_CC_BE;
        case HB_COND_O:  return HB_CC_O;
        case HB_COND_NO: return HB_CC_NO;
        case HB_COND_P:  return HB_CC_P;
        case HB_COND_NP: return HB_CC_NP;
        default: return HB_CC_E;
    }
}

static inline hb_ir_instr_t* emit(hb_ir_builder_t* b, hb_ir_instr_t* i, const hb_decoded_t* dec) {
    (void)b;
    if (i) { i->guest_addr = dec->addr; i->guest_len = dec->len; }
    return i;
}

hb_result_t hb_lift_x86(const hb_decoded_t* dec, hb_ir_builder_t* b) {
    if (!dec || !b) return HB_ERR_INVALID_ARG;

    switch (dec->opcode) {
        case HB_INS_MOV: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            if (dec->op1.is_mem && dec->op2.is_reg) {
                emit(b, hb_ir_emit_store(b, dst, src), dec);
            } else if (dec->op1.is_reg && dec->op2.is_mem) {
                emit(b, hb_ir_emit_load(b, dst, src), dec);
            } else {
                emit(b, hb_ir_emit_mov(b, dst, src), dec);
            }
            return HB_OK;
        }
        case HB_INS_LEA: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_lea(b, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_ADD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_ADD, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_SUB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_SUB, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_CMP: {
            hb_ir_operand_t a = operand_from_dec(dec, 1);
            hb_ir_operand_t b_op = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_cmp(b, a, b_op), dec);
            return HB_OK;
        }
        case HB_INS_TEST: {
            hb_ir_operand_t a = operand_from_dec(dec, 1);
            hb_ir_operand_t b_op = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_test(b, a, b_op), dec);
            return HB_OK;
        }
        case HB_INS_AND: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_AND, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_OR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_OR, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_XOR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_XOR, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_INC: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t one = hb_ir_imm(1, dst.size);
            emit(b, hb_ir_emit_binop(b, HB_IR_ADD, dst, dst, one), dec);
            return HB_OK;
        }
        case HB_INS_DEC: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t one = hb_ir_imm(1, dst.size);
            emit(b, hb_ir_emit_binop(b, HB_IR_SUB, dst, dst, one), dec);
            return HB_OK;
        }
        case HB_INS_SHL:
        case HB_INS_SHR:
        case HB_INS_SAR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_op_t ir_op = (dec->opcode == HB_INS_SHL) ? HB_IR_SHL :
                               (dec->opcode == HB_INS_SHR) ? HB_IR_SHR : HB_IR_SAR;
            emit(b, hb_ir_emit_binop(b, ir_op, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_NOT: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_unop(b, HB_IR_NOT, dst, dst), dec);
            return HB_OK;
        }
        case HB_INS_NEG: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_unop(b, HB_IR_NEG, dst, dst), dec);
            return HB_OK;
        }
        case HB_INS_PUSH: {
            hb_ir_operand_t src = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_push(b, src), dec);
            return HB_OK;
        }
        case HB_INS_POP: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_pop(b, dst), dec);
            return HB_OK;
        }
        case HB_INS_CALL: {
            emit(b, hb_ir_emit_call(b, dec->branch_target), dec);
            return HB_OK;
        }
        case HB_INS_RET: {
            emit(b, hb_ir_emit_ret(b), dec);
            return HB_OK;
        }
        case HB_INS_JMP: {
            emit(b, hb_ir_emit_jmp(b, dec->branch_target), dec);
            return HB_OK;
        }
        case HB_INS_Jcc: {
            hb_cc_t cc = cc_from_dec(dec->cond);
            emit(b, hb_ir_emit_jcc(b, cc, dec->branch_target), dec);
            return HB_OK;
        }
        case HB_INS_SETcc: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_setcc(b, cc_from_dec(dec->cond), dst), dec);
            return HB_OK;
        }
        case HB_INS_CMOVcc: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_cmovcc(b, cc_from_dec(dec->cond), dst, src), dec);
            return HB_OK;
        }
        case HB_INS_NOP: {
            emit(b, hb_ir_emit(b, HB_IR_NOP), dec);
            return HB_OK;
        }
        default:
            emit(b, hb_ir_emit_unsupported(b, hb_opcode_name(dec->opcode), dec->addr,
                                   (uint8_t*)dec->bytes, dec->len), dec);
            return HB_ERR_UNSUPPORTED_FEATURE;
    }
}

hb_result_t hb_lift_func_x86(hb_decoder_t* dec, hb_ir_func_t** out) {
    if (!dec || !out) return HB_ERR_INVALID_ARG;

    hb_ir_func_t* func = hb_ir_func_create(dec->base_addr, dec->code_len);
    if (!func) return HB_ERR_OUT_OF_MEMORY;

    hb_ir_block_t* block = hb_ir_block_create(0, dec->base_addr);
    if (!block) {
        hb_ir_func_destroy(func);
        return HB_ERR_OUT_OF_MEMORY;
    }
    hb_ir_cfg_add_block(func->cfg, block);

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    if (!b) {
        hb_ir_func_destroy(func);
        return HB_ERR_OUT_OF_MEMORY;
    }
    hb_ir_builder_set_block(b, block);

    hb_decoded_t d;
    size_t count = 0;
    while (hb_decode_next(dec, &d) == HB_OK || d.opcode == HB_INS_UNSUPPORTED) {
        hb_result_t r = hb_lift_x86(&d, b);
        if (r != HB_OK && r != HB_ERR_UNSUPPORTED_FEATURE) {
            hb_ir_builder_destroy(b);
            hb_ir_func_destroy(func);
            return r;
        }
        count++;
        if (count > 10000) break;
        if (d.is_branch || d.is_ret || d.is_call) break;
    }

    hb_ir_builder_destroy(b);
    *out = func;
    return HB_OK;
}
