#include "hb_lifter.h"
#include "hb_ir.h"
#include <string.h>

static hb_size_t size_from_dec(uint8_t sz) {
    switch (sz) {
        case 1: return HB_SIZE_8;
        case 2: return HB_SIZE_16;
        case 4: return HB_SIZE_32;
        case 8: return HB_SIZE_64;
        case 16: return HB_SIZE_128;
        default: return HB_SIZE_64;
    }
}

static hb_ir_operand_t operand_from_dec(const hb_decoded_t* dec, int slot) {
    const hb_decoded_t* d = dec; /* for field access */
    bool is_reg = (slot == 1) ? d->op1.is_reg : (slot == 2) ? d->op2.is_reg : d->op3.is_reg;
    bool is_imm = (slot == 1) ? d->op1.is_imm : (slot == 2) ? d->op2.is_imm : d->op3.is_imm;
    bool is_mem = (slot == 1) ? d->op1.is_mem : (slot == 2) ? d->op2.is_mem : d->op3.is_mem;
    uint8_t sz_raw = (slot == 1) ? d->op1.size : (slot == 2) ? d->op2.size : d->op3.size;
    uint8_t reg_offset = (slot == 1) ? d->op1.reg_offset : (slot == 2) ? d->op2.reg_offset : d->op3.reg_offset;
    hb_size_t sz = size_from_dec(sz_raw);
    if (is_reg) {
        int reg = (slot == 1) ? d->op1.reg : (slot == 2) ? d->op2.reg : d->op3.reg;
        hb_ir_operand_t op = hb_ir_reg((hb_reg_t)reg, sz);
        op.reg_offset = reg_offset;
        return op;
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
        uint8_t segment = (slot == 1) ? d->op1.mem.segment : (slot == 2) ? d->op2.mem.segment : d->op3.mem.segment;
        bool addr32 = (slot == 1) ? d->op1.mem.addr32 : (slot == 2) ? d->op2.mem.addr32 : d->op3.mem.addr32;
        bool rip_relative = (slot == 1) ? d->op1.mem.rip_relative : (slot == 2) ? d->op2.mem.rip_relative : d->op3.mem.rip_relative;
        uint64_t rip_target = (slot == 1) ? d->op1.mem.rip_target : (slot == 2) ? d->op2.mem.rip_target : d->op3.mem.rip_target;
        if (rip_relative) {
            base = -1;
            disp += (int64_t)rip_target;
        }
        hb_ir_operand_t op = hb_ir_mem_segment(
            (hb_reg_t)(base >= 0 ? base : HB_REG_COUNT),
            (hb_reg_t)(index >= 0 ? index : HB_REG_COUNT),
            scale, disp, sz, segment);
        op.mem.addr32 = addr32;
        return op;
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
        default: return HB_CC_E; /* fallback */
    }
}

static inline hb_ir_instr_t* emit(hb_ir_builder_t* b, hb_ir_instr_t* i, const hb_decoded_t* dec) {
    (void)b;
    if (i) { i->guest_addr = dec->addr; i->guest_len = dec->len; }
    return i;
}

static bool is_legacy_scalar_sse_mem_load(const hb_decoded_t* dec) {
    bool has_scalar_prefix = false;

    if (!dec) return false;
    for (uint8_t i = 0; i + 1 < dec->len && i < sizeof(dec->bytes); i++) {
        uint8_t byte = dec->bytes[i];
        if (byte == 0xf2 || byte == 0xf3) has_scalar_prefix = true;
        if (byte == 0x0f) return has_scalar_prefix && dec->bytes[i + 1] == 0x10;
    }
    return false;
}

hb_result_t hb_lift_x64(const hb_decoded_t* dec, hb_ir_builder_t* b) {
    if (!dec || !b) return HB_ERR_INVALID_ARG;

    switch (dec->opcode) {
        case HB_INS_MOV: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            if (dec->op1.is_mem && (dec->op2.is_reg || dec->op2.is_imm)) {
                /* MOV [mem], reg/imm */
                emit(b, hb_ir_emit_store(b, dst, src), dec);
            } else if (dec->op1.is_reg && dec->op2.is_mem) {
                /* MOV reg, [mem] */
                emit(b, hb_ir_emit_load(b, dst, src), dec);
            } else {
                /* MOV reg, reg/imm or other forms */
                emit(b, hb_ir_emit_mov(b, dst, src), dec);
            }
            return HB_OK;
        }
        case HB_INS_SSE_MOV: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            if (dec->op1.is_mem && dec->op2.is_reg) {
                emit(b, hb_ir_emit_store(b, dst, src), dec);
            } else if (dec->op1.is_reg && dec->op2.is_mem) {
                hb_ir_instr_t* i = emit(b, hb_ir_emit_load(b, dst, src), dec);
                if (i) i->zero_upper = is_legacy_scalar_sse_mem_load(dec);
            } else {
                emit(b, hb_ir_emit_mov(b, dst, src), dec);
            }
            return HB_OK;
        }
        case HB_INS_CVTSI2SD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTSI2SD);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTSI2SS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTSI2SS);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MULSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MULSD);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MULSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MULSS);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DIVSS: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_DIVSS);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MINPS:
        case HB_INS_MAXPS:
        case HB_INS_MINPD:
        case HB_INS_MAXPD:
        case HB_INS_MINSS:
        case HB_INS_MAXSS:
        case HB_INS_MINSD:
        case HB_INS_MAXSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, (dec->opcode == HB_INS_MAXPS ||
                                             dec->opcode == HB_INS_MAXPD ||
                                             dec->opcode == HB_INS_MAXSS ||
                                             dec->opcode == HB_INS_MAXSD) ? HB_IR_FMAX : HB_IR_FMIN);
            if (i) {
                unsigned lane = (dec->opcode == HB_INS_MINPD || dec->opcode == HB_INS_MAXPD ||
                                 dec->opcode == HB_INS_MINSD || dec->opcode == HB_INS_MAXSD) ? 8 : 4;
                bool scalar = (dec->opcode == HB_INS_MINSS || dec->opcode == HB_INS_MAXSS ||
                               dec->opcode == HB_INS_MINSD || dec->opcode == HB_INS_MAXSD);
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = lane | (scalar ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_COMISS: {
            hb_ir_operand_t lhs = operand_from_dec(dec, 1);
            hb_ir_operand_t rhs = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_COMISS);
            if (i) { i->src1 = lhs; i->src2 = rhs; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_COMISD: {
            hb_ir_operand_t lhs = operand_from_dec(dec, 1);
            hb_ir_operand_t rhs = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_COMISD);
            if (i) { i->src1 = lhs; i->src2 = rhs; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTTSD2SI: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTTSD2SI);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTTSS2SI: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTTSS2SI);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_XMM_AND:
        case HB_INS_XMM_ANDN:
        case HB_INS_XMM_OR:
        case HB_INS_XORPS:
        case HB_INS_PXOR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_op_t op = HB_IR_XORPS;
            if (dec->opcode == HB_INS_XMM_AND) op = HB_IR_XMM_AND;
            else if (dec->opcode == HB_INS_XMM_ANDN) op = HB_IR_XMM_ANDN;
            else if (dec->opcode == HB_INS_XMM_OR) op = HB_IR_XMM_OR;
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PCMPEQB:
        case HB_INS_PCMPEQW:
        case HB_INS_PCMPEQD: {
            hb_ir_op_t op = HB_IR_PCMPEQB;
            if (dec->opcode == HB_INS_PCMPEQW) op = HB_IR_PCMPEQW;
            else if (dec->opcode == HB_INS_PCMPEQD) op = HB_IR_PCMPEQD;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PCMPGTB:
        case HB_INS_PCMPGTW:
        case HB_INS_PCMPGTD: {
            hb_ir_op_t op = HB_IR_PCMPGTB;
            if (dec->opcode == HB_INS_PCMPGTW) op = HB_IR_PCMPGTW;
            else if (dec->opcode == HB_INS_PCMPGTD) op = HB_IR_PCMPGTD;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PMOVMSKB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PMOVMSKB);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVMSKPS:
        case HB_INS_MOVMSKPD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVMSK);
            if (i) {
                i->dst = dst;
                i->src1 = src;
                i->target = dec->opcode == HB_INS_MOVMSKPD ? 8 : 4;
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_UNPCKLPS:
        case HB_INS_UNPCKLPD:
        case HB_INS_UNPCKHPS:
        case HB_INS_UNPCKHPD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUNPCK);
            if (i) {
                bool packed_double = (dec->opcode == HB_INS_UNPCKLPD || dec->opcode == HB_INS_UNPCKHPD);
                bool high = (dec->opcode == HB_INS_UNPCKHPS || dec->opcode == HB_INS_UNPCKHPD);
                i->dst = operand_from_dec(dec, 1);
                i->src1 = i->dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = (packed_double ? 8 : 4) | (high ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PUNPCKLBW:
        case HB_INS_PUNPCKLWD:
        case HB_INS_PUNPCKLDQ:
        case HB_INS_PUNPCKLQDQ:
        case HB_INS_PUNPCKHBW:
        case HB_INS_PUNPCKHWD:
        case HB_INS_PUNPCKHDQ:
        case HB_INS_PUNPCKHQDQ: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PUNPCK);
            if (i) {
                unsigned lane = 1;
                if (dec->opcode == HB_INS_PUNPCKLWD || dec->opcode == HB_INS_PUNPCKHWD) lane = 2;
                else if (dec->opcode == HB_INS_PUNPCKLDQ || dec->opcode == HB_INS_PUNPCKHDQ) lane = 4;
                else if (dec->opcode == HB_INS_PUNPCKLQDQ || dec->opcode == HB_INS_PUNPCKHQDQ) lane = 8;
                bool high = (dec->opcode == HB_INS_PUNPCKHBW || dec->opcode == HB_INS_PUNPCKHWD ||
                             dec->opcode == HB_INS_PUNPCKHDQ || dec->opcode == HB_INS_PUNPCKHQDQ);
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = lane | (high ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PACKSSWB:
        case HB_INS_PACKUSWB:
        case HB_INS_PACKSSDW: {
            hb_ir_op_t op = HB_IR_PACKSSWB;
            if (dec->opcode == HB_INS_PACKUSWB) op = HB_IR_PACKUSWB;
            else if (dec->opcode == HB_INS_PACKSSDW) op = HB_IR_PACKSSDW;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PMULLW:
        case HB_INS_PMULHW:
        case HB_INS_PMULHUW:
        case HB_INS_PMADDWD: {
            hb_ir_op_t op = HB_IR_PMULLW;
            if (dec->opcode == HB_INS_PMULHW) op = HB_IR_PMULHW;
            else if (dec->opcode == HB_INS_PMULHUW) op = HB_IR_PMULHUW;
            else if (dec->opcode == HB_INS_PMADDWD) op = HB_IR_PMADDWD;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PADDSB:
        case HB_INS_PADDSW:
        case HB_INS_PADDUSB:
        case HB_INS_PADDUSW: {
            hb_ir_op_t op = HB_IR_PADDSB;
            if (dec->opcode == HB_INS_PADDSW) op = HB_IR_PADDSW;
            else if (dec->opcode == HB_INS_PADDUSB) op = HB_IR_PADDUSB;
            else if (dec->opcode == HB_INS_PADDUSW) op = HB_IR_PADDUSW;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PAVGB:
        case HB_INS_PAVGW: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_PAVGB ? HB_IR_PAVGB : HB_IR_PAVGW);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSHUFB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PSHUFB);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSHUFD:
        case HB_INS_PSHUFLW:
        case HB_INS_PSHUFHW: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PSHUF);
            if (i) {
                i->dst = operand_from_dec(dec, 1);
                i->src1 = operand_from_dec(dec, 2);
                i->src2 = operand_from_dec(dec, 3);
                i->target = dec->opcode == HB_INS_PSHUFD ? 4 : (dec->opcode == HB_INS_PSHUFLW ? 2 : 0x102);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSRLW:
        case HB_INS_PSRAW:
        case HB_INS_PSLLW:
        case HB_INS_PSRLD:
        case HB_INS_PSRAD:
        case HB_INS_PSLLD: {
            hb_ir_op_t op = HB_IR_PSRL;
            if (dec->opcode == HB_INS_PSRAW || dec->opcode == HB_INS_PSRAD) op = HB_IR_PSRA;
            else if (dec->opcode == HB_INS_PSLLW || dec->opcode == HB_INS_PSLLD) op = HB_IR_PSLL;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t imm = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->dst = dst;
                i->src1 = dst;
                i->src2 = imm;
                i->target = (dec->opcode == HB_INS_PSRLW ||
                             dec->opcode == HB_INS_PSRAW ||
                             dec->opcode == HB_INS_PSLLW) ? 2 : 4;
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSRLQ:
        case HB_INS_PSLLQ:
        case HB_INS_PSRLDQ:
        case HB_INS_PSLLDQ: {
            hb_ir_op_t op = HB_IR_PSRLDQ;
            if (dec->opcode == HB_INS_PSRLQ) op = HB_IR_PSRLQ;
            else if (dec->opcode == HB_INS_PSLLQ) op = HB_IR_PSLLQ;
            else if (dec->opcode == HB_INS_PSLLDQ) op = HB_IR_PSLLDQ;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t imm = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = imm; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PADDB:
        case HB_INS_PADDW:
        case HB_INS_PADDD:
        case HB_INS_PADDQ: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PADD);
            if (i) {
                unsigned lane = 1;
                if (dec->opcode == HB_INS_PADDW) lane = 2;
                else if (dec->opcode == HB_INS_PADDD) lane = 4;
                else if (dec->opcode == HB_INS_PADDQ) lane = 8;
                i->dst = operand_from_dec(dec, 1);
                i->src1 = i->dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = lane;
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_PSUBB:
        case HB_INS_PSUBW:
        case HB_INS_PSUBD:
        case HB_INS_PSUBQ: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_PSUB);
            if (i) {
                unsigned lane = 1;
                if (dec->opcode == HB_INS_PSUBW) lane = 2;
                else if (dec->opcode == HB_INS_PSUBD) lane = 4;
                else if (dec->opcode == HB_INS_PSUBQ) lane = 8;
                i->dst = operand_from_dec(dec, 1);
                i->src1 = i->dst;
                i->src2 = operand_from_dec(dec, 2);
                i->target = lane;
            }
            emit(b, i, dec);
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
        case HB_INS_ADC: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_ADC, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_SUB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_SUB, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_SBB: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_SBB, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_IMUL: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_operand_t imm = operand_from_dec(dec, 3);
            if (src.type == HB_OP_NONE)
                emit(b, hb_ir_emit_unop(b, HB_IR_IMUL, hb_ir_none(), dst), dec);
            else if (imm.type != HB_OP_NONE)
                emit(b, hb_ir_emit_binop(b, HB_IR_IMUL, dst, src, imm), dec);
            else
                emit(b, hb_ir_emit_binop(b, HB_IR_IMUL, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_MUL: {
            hb_ir_operand_t src = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_unop(b, HB_IR_MUL, hb_ir_none(), src), dec);
            return HB_OK;
        }
        case HB_INS_DIV: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_DIV);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_IDIV: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_IDIV);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_BT:
        case HB_INS_BTS:
        case HB_INS_BTR:
        case HB_INS_BTC: {
            hb_ir_op_t op = HB_IR_BT;
            if (dec->opcode == HB_INS_BTS) op = HB_IR_BTS;
            else if (dec->opcode == HB_INS_BTR) op = HB_IR_BTR;
            else if (dec->opcode == HB_INS_BTC) op = HB_IR_BTC;
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
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
        case HB_INS_CMPXCHG: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_CMPXCHG, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_CMPXCHG8B: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            emit(b, hb_ir_emit_unop(b, HB_IR_CMPXCHG8B, dst, dst), dec);
            return HB_OK;
        }
        case HB_INS_XCHG: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_XCHG, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_XADD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            emit(b, hb_ir_emit_binop(b, HB_IR_XADD, dst, dst, src), dec);
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
        case HB_INS_SAR:
        case HB_INS_ROL:
        case HB_INS_ROR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_op_t ir_op = (dec->opcode == HB_INS_SHL) ? HB_IR_SHL :
                               (dec->opcode == HB_INS_SHR) ? HB_IR_SHR :
                               (dec->opcode == HB_INS_SAR) ? HB_IR_SAR :
                               (dec->opcode == HB_INS_ROL) ? HB_IR_ROL : HB_IR_ROR;
            emit(b, hb_ir_emit_binop(b, ir_op, dst, dst, src), dec);
            return HB_OK;
        }
        case HB_INS_SHLD:
        case HB_INS_SHRD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_operand_t count = operand_from_dec(dec, 3);
            hb_ir_instr_t *i = hb_ir_emit(b, dec->opcode == HB_INS_SHLD ? HB_IR_SHLD : HB_IR_SHRD);
            if (i) { i->dst = dst; i->src1 = src; i->src2 = count; }
            emit(b, i, dec);
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
        case HB_INS_LAHF: {
            emit(b, hb_ir_emit(b, HB_IR_LAHF), dec);
            return HB_OK;
        }
        case HB_INS_SAHF: {
            emit(b, hb_ir_emit(b, HB_IR_SAHF), dec);
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
            hb_ir_instr_t *i = hb_ir_emit_call(b, dec->branch_target);
            if (i && dec->op1.present) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_RET: {
            hb_ir_instr_t *i = hb_ir_emit_ret(b);
            if (i && dec->ret_imm) i->src1 = hb_ir_imm(dec->ret_imm, HB_SIZE_16);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_JMP: {
            hb_ir_instr_t *i = hb_ir_emit_jmp(b, dec->branch_target);
            if (i && dec->op1.present) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
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
        case HB_INS_MOVZX: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_ZERO_EXTEND);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVSX:
        case HB_INS_MOVSXD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_SIGN_EXTEND);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CDQE: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_SIGN_EXTEND);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CWD: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CWD);
            if (i) i->src1 = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CMPS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CMPS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LODS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LODS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_SCAS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_SCAS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_TZCNT: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_TZCNT);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_LZCNT: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_LZCNT);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_BSR: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_BSR);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_BSWAP: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_BSWAP);
            if (i) i->dst = operand_from_dec(dec, 1);
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_STOS: {
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_STOS);
            if (i) {
                i->src1 = operand_from_dec(dec, 1);
                i->src2 = operand_from_dec(dec, 2);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_MOVD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_MOVD);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTDQ2PD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_CVTDQ2PD);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTDQ2PS:
        case HB_INS_CVTPS2DQ:
        case HB_INS_CVTTPS2DQ: {
            hb_ir_op_t op = HB_IR_CVTDQ2PS;
            if (dec->opcode == HB_INS_CVTPS2DQ) op = HB_IR_CVTPS2DQ;
            else if (dec->opcode == HB_INS_CVTTPS2DQ) op = HB_IR_CVTTPS2DQ;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CVTPS2PD:
        case HB_INS_CVTPD2PS:
        case HB_INS_CVTSS2SD:
        case HB_INS_CVTSD2SS: {
            hb_ir_op_t op = HB_IR_CVTPS2PD;
            if (dec->opcode == HB_INS_CVTPD2PS) op = HB_IR_CVTPD2PS;
            else if (dec->opcode == HB_INS_CVTSS2SD) op = HB_IR_CVTSS2SD;
            else if (dec->opcode == HB_INS_CVTSD2SS) op = HB_IR_CVTSD2SS;
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, op);
            if (i) { i->dst = dst; i->src1 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_ADDPS:
        case HB_INS_ADDPD:
        case HB_INS_ADDSS:
        case HB_INS_ADDSD:
        case HB_INS_SUBPS:
        case HB_INS_SUBPD:
        case HB_INS_SUBSS:
        case HB_INS_SUBSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            bool is_sub = (dec->opcode == HB_INS_SUBPS || dec->opcode == HB_INS_SUBPD ||
                           dec->opcode == HB_INS_SUBSS || dec->opcode == HB_INS_SUBSD);
            hb_ir_instr_t *i = hb_ir_emit(b, is_sub ? HB_IR_FSUB : HB_IR_FADD);
            if (i) {
                unsigned lane = (dec->opcode == HB_INS_ADDPD || dec->opcode == HB_INS_SUBPD ||
                                 dec->opcode == HB_INS_ADDSD || dec->opcode == HB_INS_SUBSD) ? 8 : 4;
                bool scalar = (dec->opcode == HB_INS_ADDSS || dec->opcode == HB_INS_SUBSS ||
                               dec->opcode == HB_INS_ADDSD || dec->opcode == HB_INS_SUBSD);
                i->dst = dst;
                i->src1 = dst;
                i->src2 = src;
                i->target = lane | (scalar ? 0x100 : 0);
            }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_DIVSD: {
            hb_ir_operand_t dst = operand_from_dec(dec, 1);
            hb_ir_operand_t src = operand_from_dec(dec, 2);
            hb_ir_instr_t *i = hb_ir_emit(b, HB_IR_DIVSD);
            if (i) { i->dst = dst; i->src1 = dst; i->src2 = src; }
            emit(b, i, dec);
            return HB_OK;
        }
        case HB_INS_CPUID: {
            emit(b, hb_ir_emit(b, HB_IR_CPUID), dec);
            return HB_OK;
        }
        case HB_INS_XGETBV: {
            emit(b, hb_ir_emit(b, HB_IR_XGETBV), dec);
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

hb_result_t hb_lift_func_x64(hb_decoder_t* dec, hb_ir_func_t** out) {
    if (!dec || !out) return HB_ERR_INVALID_ARG;

    hb_ir_func_t* func = hb_ir_func_create(dec->base_addr, dec->code_len);
    if (!func) return HB_ERR_OUT_OF_MEMORY;

    hb_ir_block_t* block = hb_ir_block_create(0, dec->base_addr);
    if (!block) {
        hb_ir_func_destroy(func);
        return HB_ERR_OUT_OF_MEMORY;
    }
    hb_ir_cfg_add_block(func->cfg, block);
    func->cfg->entry = block;

    hb_ir_builder_t* b = hb_ir_builder_create(func);
    if (!b) {
        hb_ir_func_destroy(func);
        return HB_ERR_OUT_OF_MEMORY;
    }
    hb_ir_builder_set_block(b, block);

    hb_decoded_t d;
    size_t count = 0;
    const size_t instr_limit = 10000;
    while (hb_decode_next(dec, &d) == HB_OK || d.opcode == HB_INS_UNSUPPORTED) {
        hb_result_t r = hb_lift_x64(&d, b);
        if (r != HB_OK && r != HB_ERR_UNSUPPORTED_FEATURE) {
            hb_ir_builder_destroy(b);
            hb_ir_func_destroy(func);
            return r;
        }
        count++;
        if (d.is_branch || d.is_ret || d.is_call) {
            /* For MVP: single basic block per function.
               Future: split into multiple blocks at branches. */
            break;
        }
        if (count >= instr_limit) {
            if (dec->pos < dec->code_len) {
                func->truncated = true;
                func->truncation_reason = "x64 lifter instruction limit";
            }
            break;
        }
    }

    hb_ir_builder_destroy(b);
    *out = func;
    return HB_OK;
}
